#include "PluginHost.h"

namespace td
{
/** Wraps a loaded plug-in as an insert. Always runs the plug-in as a stereo effect. */
class Vst3Insert : public InsertProcessor
{
public:
    Vst3Insert (std::unique_ptr<juce::AudioPluginInstance> p, juce::PluginDescription d)
        : plugin (std::move (p)), description (std::move (d)) {}

    ~Vst3Insert() override { if (prepared) plugin->releaseResources(); }

    void prepare (double sr, int maxBlock, int) override
    {
        if (prepared) plugin->releaseResources();
        plugin->setPlayConfigDetails (2, 2, sr, maxBlock);
        plugin->prepareToPlay (sr, maxBlock);
        work.setSize (2, maxBlock);
        midi.ensureSize (256);
        prepared = true;
    }

    void process (juce::AudioBuffer<float>& b) override
    {
        const int n = b.getNumSamples(), nc = b.getNumChannels();
        if (n > work.getNumSamples() || nc < 1) return;
        work.copyFrom (0, 0, b, 0, 0, n);
        work.copyFrom (1, 0, b, nc > 1 ? 1 : 0, 0, n);
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), 2, n);
        midi.clear();
        plugin->processBlock (view, midi);
        b.copyFrom (0, 0, work, 0, 0, n);
        if (nc > 1) b.copyFrom (1, 0, work, 1, 0, n);
    }

    juce::String getName() const override { return description.name; }
    juce::String getDescription() const override { if (auto x = description.createXml()) return x->toString(); return {}; }
    juce::String getStateBase64() const override { juce::MemoryBlock mb; plugin->getStateInformation (mb); return mb.toBase64Encoding(); }

    int numParams() const override { return plugin->getParameters().size(); }
    juce::String paramName (int i) const override
    {
        auto& ps = plugin->getParameters();
        return juce::isPositiveAndBelow (i, ps.size()) ? ps[i]->getName (64) : juce::String();
    }
    bool paramAutomatable (int i) const override
    {
        auto& ps = plugin->getParameters();
        return juce::isPositiveAndBelow (i, ps.size()) && ps[i]->isAutomatable() && ! ps[i]->isMetaParameter();
    }
    float getParamNorm (int i) const override
    {
        auto& ps = plugin->getParameters();
        return juce::isPositiveAndBelow (i, ps.size()) ? ps[i]->getValue() : 0.0f;
    }
    void setParamNorm (int i, float v) override
    {
        auto& ps = plugin->getParameters();
        if (juce::isPositiveAndBelow (i, ps.size())) ps[i]->setValue (juce::jlimit (0.0f, 1.0f, v));
    }

    juce::AudioPluginInstance& getPlugin() { return *plugin; }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    bool prepared = false;
};

class PluginHost::EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow (PluginHost& h, Vst3Insert* i, juce::AudioProcessorEditor* editor)
        : juce::DocumentWindow (i->getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          host (h), insert (i)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        if (host.adoptWindow) host.adoptWindow (*this);
        setVisible (true);
    }
    void closeButtonPressed() override
    {
        auto* self = this;
        juce::MessageManager::callAsync ([h = &host, self]() { h->closeEditorsFor (self->insert); });
    }
    PluginHost& host;
    Vst3Insert* insert;
};

PluginHost::PluginHost() : juce::Thread ("Plug-in scanner")
{
    formats.addFormat (new juce::VST3PluginFormat());
    listFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Fermata").getChildFile ("plugins.xml");
    if (auto xml = juce::XmlDocument::parse (listFile))
        known.recreateFromXml (*xml);
}

PluginHost::~PluginHost()
{
    stopThread (5000);
    closeAllEditors();
}

void PluginHost::startScan (std::function<void()> whenFinished)
{
    if (scanning.load()) return;
    finished = std::move (whenFinished);
    scanning = true;
    startThread();
}

void PluginHost::run()
{
    for (auto* format : formats.getFormats())
    {
        juce::PluginDirectoryScanner scanner (known, *format, format->getDefaultLocationsToSearch(), true, juce::File(), false);
        juce::String name;
        while (! threadShouldExit() && scanner.scanNextFile (true, name))
        {
            const juce::ScopedLock sl (statusLock);
            status = "Scanning: " + name;
        }
    }
    listFile.getParentDirectory().createDirectory();
    if (auto xml = known.createXml()) xml->writeTo (listFile);
    {
        const juce::ScopedLock sl (statusLock);
        status = "Found " + juce::String (known.getNumTypes()) + " plug-ins";
    }
    scanning = false;
    juce::MessageManager::callAsync ([this]() { if (finished) finished(); });
}

std::unique_ptr<InsertProcessor> PluginHost::create (const juce::PluginDescription& d, double sr, int block, juce::String& error)
{
    auto instance = formats.createPluginInstance (d, sr, block, error);
    if (instance == nullptr) return nullptr;
    return std::make_unique<Vst3Insert> (std::move (instance), d);
}

InsertFactory PluginHost::makeFactory (std::function<double()> sampleRate, std::function<int()> blockSize)
{
    return [this, sampleRate, blockSize] (const juce::String& desc, const juce::String& state) -> std::unique_ptr<InsertProcessor>
    {
        auto xml = juce::XmlDocument::parse (desc);
        if (xml == nullptr) return nullptr;
        juce::PluginDescription d;
        if (! d.loadFromXml (*xml)) return nullptr;
        juce::String err;
        auto p = create (d, sampleRate(), blockSize(), err);
        if (p == nullptr) return nullptr;
        juce::MemoryBlock mb;
        if (state.isNotEmpty() && mb.fromBase64Encoding (state))
            static_cast<Vst3Insert*> (p.get())->getPlugin().setStateInformation (mb.getData(), (int) mb.getSize());
        return p;
    };
}

void PluginHost::showEditor (InsertProcessor* p)
{
    auto* v = dynamic_cast<Vst3Insert*> (p);
    if (v == nullptr) return;
    for (auto& e : editors)
        if (e->insert == v) { e->toFront (true); return; }
    if (! v->getPlugin().hasEditor()) return;
    if (auto* ed = v->getPlugin().createEditorIfNeeded())
        editors.push_back (std::make_unique<EditorWindow> (*this, v, ed));
}

void PluginHost::closeEditorsFor (InsertProcessor* p)
{
    for (size_t i = editors.size(); i-- > 0;)
        if (editors[i]->insert == p) editors.erase (editors.begin() + (long) i);
}

void PluginHost::closeAllEditors() { editors.clear(); }

void PluginHost::forEachEditor (const std::function<void (juce::Component&)>& fn) { for (auto& e : editors) fn (*e); }

juce::PopupMenu PluginHost::buildPluginMenu (int baseId, std::vector<juce::PluginDescription>& outDescs) const
{
    juce::PopupMenu menu;
    auto types = known.getTypes();
    std::map<juce::String, std::vector<juce::PluginDescription>> byMaker;
    for (auto& t : types) byMaker[t.manufacturerName.isEmpty() ? juce::String ("Other") : t.manufacturerName].push_back (t);
    int id = baseId;
    for (auto& [maker, list] : byMaker)
    {
        juce::PopupMenu sub;
        for (auto& d : list) { outDescs.push_back (d); sub.addItem (id++, d.name); }
        menu.addSubMenu (maker, sub);
    }
    return menu;
}
} // namespace td

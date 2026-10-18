#include "PluginHost.h"
#include "WinOwner.h"
#include "../core/HostTransport.h"

namespace td
{
/** Wraps a loaded plug-in as an insert. Always runs the plug-in as a stereo effect. It is also the plug-in's play head: it tells the plug-in
    whether Fermata is playing / recording and where on the timeline it is (see HostTransport), which plug-ins that listen while the host
    plays (Auto-Align 2 ...) need. */
class Vst3Insert : public InsertProcessor, private juce::AudioPlayHead, private juce::AudioProcessorListener
{
public:
    Vst3Insert (std::unique_ptr<juce::AudioPluginInstance> p, juce::PluginDescription d)
        : plugin (std::move (p)), description (std::move (d)) { plugin->setPlayHead (this); plugin->addListener (this); }

    ~Vst3Insert() override { plugin->removeListener (this); if (prepared) plugin->releaseResources(); plugin->setPlayHead (nullptr); }

    void prepare (double sr, int maxBlock, int) override
    {
        if (prepared) plugin->releaseResources();
        // the main input and output as stereo, the proper VST3 way (other buses, a side-chain say, are left as the plug-in has them)
        auto layout = plugin->getBusesLayout();
        bool change = false;
        if (! layout.inputBuses.isEmpty() && layout.inputBuses.getReference (0) != juce::AudioChannelSet::stereo()) { layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo(); change = true; }
        if (! layout.outputBuses.isEmpty() && layout.outputBuses.getReference (0) != juce::AudioChannelSet::stereo()) { layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo(); change = true; }
        if (change) plugin->setBusesLayout (layout);                         // (a plug-in that refuses keeps its own)
        plugin->setRateAndBufferSizeDetails (sr, maxBlock);
        plugin->prepareToPlay (sr, maxBlock);
        // the buffer handed to the plug-in has as many channels as it uses (at least 2); the ones past the first two (a side-chain) are silent
        chans = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        monoOut = plugin->getMainBusNumOutputChannels() == 1;
        work.setSize (chans, maxBlock);
        midi.ensureSize (256);
        prepared = true;
    }

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo info;
        const auto* t = currentHostTransport();
        const double sr = t != nullptr && t->sampleRate > 0.0 ? t->sampleRate : 48000.0;
        const juce::int64 pos = t != nullptr ? juce::jmax<juce::int64> (0, t->samplePos) : 0;
        const double secs = (double) pos / sr;
        info.setIsPlaying (t != nullptr && t->playing);
        info.setIsRecording (t != nullptr && t->recording);
        info.setIsLooping (false);
        info.setTimeInSamples (pos);
        info.setTimeInSeconds (secs);
        info.setBpm (120.0);                                                   // (classical recordings have no tempo map: a plain 120, 4/4)
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        info.setPpqPosition (secs * 2.0);
        info.setPpqPositionOfLastBarStart (std::floor (secs * 2.0 / 4.0) * 4.0);
        info.setFrameRate (juce::AudioPlayHead::fps25);
        return info;
    }

    int getLatencySamples() const override { return plugin != nullptr ? plugin->getLatencySamples() : 0; }

    void process (juce::AudioBuffer<float>& b) override
    {
        const int total = b.getNumSamples(), nc = b.getNumChannels(), cap = work.getNumSamples();
        if (nc < 1 || cap < 1) return;
        // a block longer than the plug-in was prepared for goes through in pieces (it used to skip the plug-in altogether, so the sound went
        // past it untouched: a reverb loaded with a project while the interface ran a bigger buffer than 512 did nothing)
        for (int off = 0; off < total; off += cap)
        {
            const int n = juce::jmin (cap, total - off);
            work.copyFrom (0, 0, b, 0, off, n);
            work.copyFrom (1, 0, b, nc > 1 ? 1 : 0, off, n);
            for (int c = 2; c < chans; ++c) work.clear (c, 0, n);
            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), chans, n);
            midi.clear();
            plugin->processBlock (view, midi);
            if (monoOut) work.copyFrom (1, 0, work, 0, 0, n);                  // a mono-only plug-in: its output on both sides
            b.copyFrom (0, off, work, 0, 0, n);
            if (nc > 1) b.copyFrom (1, off, work, 1, 0, n);
        }
    }

    juce::String getName() const override { return description.name; }
    juce::String getDescription() const override { if (auto x = description.createXml()) return x->toString(); return {}; }
    juce::String getStateBase64() const override
    {
        stateChanged.store (false);                                           // (before asking: a change made while it answers marks it again)
        juce::MemoryBlock mb; plugin->getStateInformation (mb);
        cachedState = mb.toBase64Encoding(); cachedAt = juce::Time::getMillisecondCounter();
        return cachedState;
    }
    /** The automatic save: the last state, unless the plug-in reported a change since, its window is open (some plug-ins change things there
        without reporting them: a reverb's impulse loaded from a file, say), or it is a minute old. */
    juce::String getStateBase64Quick() const override
    {
        const bool fresh = cachedAt != 0 && ! stateChanged.load() && plugin->getActiveEditor() == nullptr
                           && juce::Time::getMillisecondCounter() - cachedAt < 60000;
        return fresh ? cachedState : getStateBase64();
    }

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
        if (juce::isPositiveAndBelow (i, ps.size())) { ps[i]->setValue (juce::jlimit (0.0f, 1.0f, v)); stateChanged.store (true); }     // (automation: setValue tells no listener)
    }

    juce::AudioPluginInstance& getPlugin() { return *plugin; }
    /** The state was set from outside (a project load, a copy): what is cached is no longer it. */
    void stateWasSet() noexcept { stateChanged.store (true); }

private:
    // AudioProcessorListener (any thread, the audio thread included: only a flag is set)
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { stateChanged.store (true); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override { stateChanged.store (true); }
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    mutable juce::String cachedState;                                         // message thread
    mutable juce::uint32 cachedAt = 0;
    mutable std::atomic<bool> stateChanged { true };
    int chans = 2;
    bool monoOut = false;
    bool prepared = false;
};

/** A plug-in's own window. Space still plays / stops the take or edit window that was last in use, even while the plug-in's window has the keyboard
    (the plug-in's editor is a window of its own inside this one and keeps the keys to itself, so the space bar is watched instead). */
class PluginHost::EditorWindow : public juce::DocumentWindow, private juce::Timer
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
        startTimerHz (40);
    }
    ~EditorWindow() override { stopTimer(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getKeyCode() == juce::KeyPress::spaceKey && ! k.getModifiers().isAnyModifierKeyDown()) return true;   // (the timer below acts on it, so it happens once)
        return juce::DocumentWindow::keyPressed (k);
    }
    void closeButtonPressed() override
    {
        auto* self = this;
        juce::MessageManager::callAsync ([h = &host, self]() { h->closeEditorsFor (self->insert); });
    }
    PluginHost& host;
    Vst3Insert* insert;
private:
    bool spaceWasDown = false;
    void timerCallback() override
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::spaceKey) && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyModifierKeyDown();
        if (down && ! spaceWasDown && isVisible() && isForegroundWindow (*this))           // pressed (not held) while this window is the one in front
            if (auto* t = ToolWindow::lastTransportWindow().getComponent())
                if (t->isVisible())
                    if (auto* c = t->getContentComponent()) c->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        spaceWasDown = down;
    }
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
        {
            static_cast<Vst3Insert*> (p.get())->getPlugin().setStateInformation (mb.getData(), (int) mb.getSize());
            static_cast<Vst3Insert*> (p.get())->stateWasSet();
        }
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

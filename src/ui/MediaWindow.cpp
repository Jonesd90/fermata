#include "MediaWindow.h"

namespace td
{
/** One folder or file. Folders are read the first time they are opened. */
class MediaTreeItem : public juce::TreeViewItem
{
public:
    MediaTreeItem (MediaComponent& o, juce::File f, bool isRootNode = false, bool isTopFolder = false)
        : owner (o), file (std::move (f)), rootNode (isRootNode), top (isTopFolder), dir (isRootNode || file.isDirectory()) {}

    const juce::File& getFile() const { return file; }
    bool mightContainSubItems() override { return dir; }
    juce::String getUniqueName() const override { return rootNode ? juce::String ("media-root") : file.getFullPathName(); }
    int getItemHeight() const override { return 22; }
    int getItemWidth() const override { return owner.contentWidth; }          // the whole width of the tree

    void itemOpennessChanged (bool isNowOpen) override
    {
        if (isNowOpen && getNumSubItems() == 0) populate();
    }
    void populate()
    {
        clearSubItems();
        if (rootNode) return;
        juce::Array<juce::File> kids;
        file.findChildFiles (kids, juce::File::findDirectories, false);
        kids.sort();
        for (auto& k : kids) addSubItem (new MediaTreeItem (owner, k));
        juce::Array<juce::File> files;
        file.findChildFiles (files, juce::File::findFiles, false);
        files.sort();
        for (auto& f : files) addSubItem (new MediaTreeItem (owner, f));
    }
    void paintItem (juce::Graphics& g, int w, int h) override
    {
        if (rootNode) return;
        const bool sel = isSelected();
        if (sel) { g.setColour (theme::selected); g.fillRect (0, 0, w, h); }
        // a small folder / sheet glyph
        auto icon = juce::Rectangle<float> (2.0f, 4.0f, 15.0f, (float) h - 8.0f);
        if (dir) { g.setColour (juce::Colour (0xffd9a441)); g.fillRoundedRectangle (icon.withTrimmedTop (2.0f), 2.0f); g.fillRoundedRectangle (icon.withHeight (4.0f).withWidth (7.0f), 1.5f); }
        else     { g.setColour (juce::Colour (0xffc9d2dc)); g.fillRoundedRectangle (icon.reduced (2.0f, 0.0f), 1.5f);
                   g.setColour (theme::accent); g.drawHorizontalLine ((int) icon.getCentreY(), icon.getX() + 4.0f, icon.getRight() - 4.0f); }
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (13.0f, top ? juce::Font::bold : juce::Font::plain));
        const auto name = top ? file.getFullPathName() : file.getFileName();
        int right = w - 4;
        if (! dir)
        {
            g.setColour (theme::dimText);
            const auto size = juce::File::descriptionOfSizeInBytes (file.getSize());
            g.drawText (size, w - 190, 0, 90, h, juce::Justification::centredRight);
            g.drawText (file.getLastModificationTime().formatted ("%d %b %Y  %H:%M"), w - 96, 0, 94, h, juce::Justification::centredRight);
            right = w - 196;
            g.setColour (theme::text);
        }
        g.drawText (name, 22, 0, juce::jmax (20, right - 22), h, juce::Justification::centredLeft, true);
    }
    void itemDoubleClicked (const juce::MouseEvent&) override
    {
        if (! dir) file.revealToUser(); else setOpen (! isOpen());
    }
    void itemClicked (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu()) return;
        juce::PopupMenu m;
        m.addItem (1, "Show in Explorer");
        m.addItem (2, "Copy the path");
        const auto f = file;
        m.showMenuAsync (juce::PopupMenu::Options(), [f] (int r)
        {
            if (r == 1) f.revealToUser();
            if (r == 2) juce::SystemClipboard::copyTextToClipboard (f.getFullPathName());
        });
    }
    MediaTreeItem* findChildFor (const juce::File& target)
    {
        for (int i = 0; i < getNumSubItems(); ++i)
            if (auto* k = dynamic_cast<MediaTreeItem*> (getSubItem (i)))
                if (k->file == target || (k->dir && target.isAChildOf (k->file))) return k;
        return nullptr;
    }
    MediaComponent& owner; juce::File file; bool rootNode, top, dir;
};

MediaComponent::MediaComponent (AppContext& a) : app (a)
{
    tree.setDefaultOpenness (false);
    tree.setRootItemVisible (false);
    tree.setIndentSize (16);
    tree.setColour (juce::TreeView::backgroundColourId, theme::field);
    tree.setColour (juce::TreeView::linesColourId, theme::grid);
    tree.setMultiSelectEnabled (false);
    addAndMakeVisible (tree);
    refreshButton.onClick = [this] { refresh(); };
    revealButton.setTooltip ("Open the folder of the highlighted file in Windows Explorer");
    revealButton.onClick = [this]
    {
        if (auto* it = dynamic_cast<MediaTreeItem*> (tree.getSelectedItem (0))) { if (it->getFile().isDirectory()) it->getFile().revealToUser(); else it->getFile().revealToUser(); }
        else app.project.projectFile.getParentDirectory().revealToUser();
    };
    addAndMakeVisible (refreshButton); addAndMakeVisible (revealButton);
    info.setFont (juce::FontOptions (12.0f));
    info.setColour (juce::Label::textColourId, theme::dimText);
    info.setText ("Where every file of this project is saved. Click a take or a piece in the Take / Edit windows to find its file here.  Double-click a file to open its folder.", juce::dontSendNotification);
    addAndMakeVisible (info);
    refresh();
    if (app.mediaSelection != juce::File()) selectFile (app.mediaSelection);
    startTimerHz (1);
    setSize (760, 560);
}

MediaComponent::~MediaComponent() { tree.setRootItem (nullptr); }

void MediaComponent::paint (juce::Graphics& g) { g.fillAll (theme::window); }

void MediaComponent::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromBottom (grid::rowH);
    revealButton.setBounds (grid::cell (top.removeFromRight (grid::btnW + 30), 0, 1).withWidth (grid::btnW + 30));
    refreshButton.setBounds (grid::cell (top.removeFromRight (grid::btnW + grid::gap), 0));
    info.setBounds (top);
    r.removeFromBottom (4);
    tree.setBounds (r);
    contentWidth = juce::jmax (300, tree.getWidth() - 64);
    if (root != nullptr) root->treeHasChanged();
}

std::vector<juce::File> MediaComponent::rootFolders() const
{
    std::vector<juce::File> roots;
    const auto proj = app.project.projectFile.getParentDirectory();
    if (proj != juce::File()) roots.push_back (proj);
    auto under = [&] (const juce::File& f) { for (auto& r : roots) if (f.isAChildOf (r) || f == r) return true; return false; };
    auto consider = [&] (const juce::File& f)
    {
        if (f == juce::File() || under (f)) return;
        roots.push_back (f.getParentDirectory());
    };
    for (auto& w : app.project.takeWindows) for (auto& g : w->groups) for (auto& f : g.files) consider (f.file);
    for (auto& e : app.project.edits)
    {
        for (auto& r : e->regions)  for (auto& f : r.files) consider (f.file);
        for (auto& r : e->overdubs) for (auto& f : r.files) consider (f.file);
    }
    for (auto& x : extraRoots) if (! under (x)) roots.push_back (x);
    return roots;
}

juce::String MediaComponent::signature() const
{
    juce::String s;
    for (auto& r : rootFolders())
    {
        s << r.getFullPathName() << ":";
        juce::Array<juce::File> all;
        r.findChildFiles (all, juce::File::findFilesAndDirectories, true);
        s << all.size();
        juce::int64 bytes = 0;
        for (auto& f : all) if (f.existsAsFile()) bytes += f.getSize();
        s << "/" << bytes << ";";
    }
    return s;
}

void MediaComponent::refresh()
{
    std::unique_ptr<juce::XmlElement> state (tree.getOpennessState (true));
    auto sel = tree.getSelectedItem (0) != nullptr ? dynamic_cast<MediaTreeItem*> (tree.getSelectedItem (0))->getFile() : juce::File();
    tree.setRootItem (nullptr);
    root = std::make_unique<MediaTreeItem> (*this, juce::File(), true);
    for (auto& f : rootFolders()) root->addSubItem (new MediaTreeItem (*this, f, false, true));
    tree.setRootItem (root.get());
    for (int i = 0; i < root->getNumSubItems(); ++i) root->getSubItem (i)->setOpen (i == 0);       // the project folder starts open
    if (state != nullptr) tree.restoreOpennessState (*state, true);
    tree.setRootItem (nullptr); tree.setRootItem (root.get());              // (setting the root is what lays the rows out at once)
    lastSignature = signature();
    if (sel != juce::File()) selectFile (sel);
}

void MediaComponent::timerCallback()
{
    if (isShowing() && signature() != lastSignature) refresh();     // a take was recorded, a bounce written...
}

void MediaComponent::selectFile (const juce::File& f)
{
    if (root == nullptr || f == juce::File()) return;
    MediaTreeItem* item = nullptr;
    for (int i = 0; i < root->getNumSubItems(); ++i)
        if (auto* t = dynamic_cast<MediaTreeItem*> (root->getSubItem (i)))
            if (f == t->getFile() || f.isAChildOf (t->getFile())) { item = t; break; }
    if (item == nullptr)                                  // lives somewhere new: give it a root of its own
    {
        extraRoots.push_back (f.getParentDirectory());
        refresh();
        return;
    }
    while (item != nullptr && item->getFile() != f)
    {
        item->setOpen (true);
        if (item->getNumSubItems() == 0) item->populate();
        auto* next = item->findChildFor (f);
        if (next == nullptr) break;
        item = next;
    }
    if (item != nullptr && item->getFile() == f)
    {
        tree.setRootItem (nullptr); tree.setRootItem (root.get());                  // lay the newly opened folders out now
        item->setSelected (true, true, juce::dontSendNotification);
        tree.scrollToKeepItemVisible (item);
        repaint();
    }
}
} // namespace td

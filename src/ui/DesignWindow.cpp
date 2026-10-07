#include "DesignWindow.h"

namespace td
{
juce::TextButton* DesignComponent::Page::addButton (const juce::String& text, std::function<void()> fn)
{
    auto* b = buttons.add (new juce::TextButton (text));
    b->onClick = std::move (fn);
    addAndMakeVisible (b);
    return b;
}

void DesignComponent::Page::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto bottom = r.removeFromBottom (34);
    note.setBounds (r.removeFromBottom (36));
    grid.setBounds (r);
    int x = bottom.getX();
    for (auto* b : buttons)
    {
        const int w = juce::jmax (90, b->getBestWidthForHeight (28) + 10);
        b->setBounds (x, bottom.getY() + 4, w, 28);
        x += w + 6;
    }
}

void DesignComponent::IoPage::resized()
{
    auto r = getLocalBounds().reduced (6);
    note.setBounds (r.removeFromBottom (40));
    auto left = r.removeFromLeft (r.getWidth() / 2 - 3);
    r.removeFromLeft (6);
    inCaption.setBounds (left.removeFromTop (22));  ins.setBounds (left);
    outCaption.setBounds (r.removeFromTop (22));    outs.setBounds (r);
}

DesignComponent::DesignComponent (AppContext& a) : app (a)
{
    buildIoPage(); buildTracksPage(); buildBusPage (intPage, false); buildBusPage (extPage, true); buildMixersPage();
    const auto bg = theme::panel;
    tabs.addTab ("1. I/O", bg, &ioPage, false);
    tabs.addTab ("2. Audio tracks", bg, &tracksPage, false);
    tabs.addTab ("3. Int buses", bg, &intPage, false);
    tabs.addTab ("4. Ext buses", bg, &extPage, false);
    tabs.addTab ("5. Mixers", bg, &mixersPage, false);
    addAndMakeVisible (tabs);
    lockNote.setColour (juce::Label::textColourId, theme::warn);
    addAndMakeVisible (lockNote);
    app.project.structure.addChangeListener (this);
    startTimerHz (4);
    setSize (940, 560);
}

DesignComponent::~DesignComponent() { app.project.structure.removeChangeListener (this); }

void DesignComponent::resized()
{
    auto r = getLocalBounds();
    lockNote.setBounds (r.removeFromBottom (22).reduced (8, 0));
    tabs.setBounds (r);
}

bool DesignComponent::locked() const { return app.engine.isRecording(); }

void DesignComponent::timerCallback()
{
    lockNote.setText (locked() ? "Recording - the project design is locked until you stop." : "", juce::dontSendNotification);
}

void DesignComponent::changeListenerCallback (juce::ChangeBroadcaster*) { refreshAll(); }

void DesignComponent::refreshAll()
{
    ioPage.ins.refresh(); ioPage.outs.refresh(); tracksPage.grid.refresh(); intPage.grid.refresh(); extPage.grid.refresh(); mixersPage.grid.refresh();
}

int DesignComponent::nextFreeInput() const
{
    int next = 0;
    for (auto& t : app.project.tracks)
        for (int c = 0; c < t.channelCount(); ++c) next = juce::jmax (next, t.inputOf (c) + 1);
    return juce::jmin (next, juce::jmax (0, (int) app.project.inputs.size() - 1));
}

juce::StringArray DesignComponent::inputItems (bool withNone) const
{
    juce::StringArray items;
    if (withNone) items.add ("(none)");
    for (int i = 0; i < (int) app.project.inputs.size(); ++i) items.add (app.project.inputLabel (i));
    return items;
}

int DesignComponent::parseInputItem (const juce::String& s)
{
    if (s.isEmpty() || s.startsWith ("(")) return -1;
    return s.upToFirstOccurrenceOf (" - ", false, false).getIntValue() - 1;
}

// -------------------------------------------------------------------- I/O
void DesignComponent::buildIoPage()
{
    auto& p = app.project;
    ioPage.inCaption.setText ("Inputs (from the audio driver)", juce::dontSendNotification);
    ioPage.outCaption.setText ("Outputs (from the audio driver)", juce::dontSendNotification);
    for (auto* l : { &ioPage.inCaption, &ioPage.outCaption }) { l->setFont (juce::FontOptions (15.0f, juce::Font::bold)); ioPage.addAndMakeVisible (l); }

    auto& gi = ioPage.ins;
    gi.setRowCountProvider ([&p] { return (int) p.inputs.size(); });
    gi.addColumn ({ "#", 38, GridEditor::Type::ReadOnly, [] (int r) { return juce::String (r + 1); }, {}, {}, {} });
    gi.addColumn ({ "Driver channel", 150, GridEditor::Type::ReadOnly, [&p] (int r) { return p.inputs[(size_t) r].driverName; }, {}, {}, {} });
    gi.addColumn ({ "Name", 170, GridEditor::Type::Text,
                    [&p] (int r) { return p.inputs[(size_t) r].name; },
                    [this, &p] (int r, const juce::String& v) { if (! locked()) p.setInputName (r, v); else refreshAll(); }, {}, {} });
    gi.addColumn ({ "Used by track", 130, GridEditor::Type::ReadOnly,
                    [&p] (int r)
                    {
                        juce::StringArray names;
                        for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) == r) { names.addIfNotAlreadyThere (t.name); break; }
                        return names.joinIntoString (", ");
                    }, {}, {}, {} });

    auto& go = ioPage.outs;
    go.setRowCountProvider ([&p] { return (int) p.outputs.size(); });
    go.addColumn ({ "#", 38, GridEditor::Type::ReadOnly, [] (int r) { return juce::String (r + 1); }, {}, {}, {} });
    go.addColumn ({ "Driver channel", 150, GridEditor::Type::ReadOnly, [&p] (int r) { return p.outputs[(size_t) r].driverName; }, {}, {}, {} });
    go.addColumn ({ "Name", 170, GridEditor::Type::Text,
                    [&p] (int r) { return p.outputs[(size_t) r].name; },
                    [this, &p] (int r, const juce::String& v) { if (! locked()) { p.outputs[(size_t) r].name = v; p.structureChanged(); } }, {}, {} });
    go.addColumn ({ "Fed by Ext bus", 200, GridEditor::Type::ReadOnly,
                    [&p] (int r)
                    {
                        return p.outputFedBy (r);
                    }, {}, {}, {} });

    ioPage.addAndMakeVisible (ioPage.ins); ioPage.addAndMakeVisible (ioPage.outs);
    ioPage.note.setText ("These lists always match the audio driver chosen in Audio settings (every channel the driver offers is opened). "
                         "Give the channels names here (a mono track on an input takes its name, and that name is shown at the top of its mixer strip); tracks pick their inputs on the Tracks tab, in the main window or at the top of each mixer strip.", juce::dontSendNotification);
    ioPage.note.setJustificationType (juce::Justification::topLeft);
    ioPage.addAndMakeVisible (ioPage.note);
}

// -------------------------------------------------------------------- audio tracks
void DesignComponent::buildTracksPage()
{
    auto& p = app.project;
    auto& g = tracksPage.grid;
    g.setRowCountProvider ([&p] { return (int) p.tracks.size(); });
    g.addColumn ({ "#", 34, GridEditor::Type::ReadOnly, [] (int r) { return juce::String (r + 1); }, {}, {}, {} });
    g.addColumn ({ "Name", 200, GridEditor::Type::Text,
                   [&p] (int r) { return p.tracks[(size_t) r].name; },
                   [this, &p] (int r, const juce::String& v) { if (! locked()) { p.tracks[(size_t) r].name = v; p.structureChanged(); } }, {}, {} });
    g.addColumn ({ "Type", 90, GridEditor::Type::Combo,
                   [&p] (int r) { auto f = p.tracks[(size_t) r].format; return f == TrackFormat::Mono ? "Mono" : f == TrackFormat::Stereo ? "Stereo" : "Surround"; },
                   [this, &p] (int r, const juce::String& v)
                   {
                       if (locked()) { refreshAll(); return; }
                       auto& t = p.tracks[(size_t) r];
                       const int first = juce::jmax (0, t.inputOf (0));
                       t.format = v == "Mono" ? TrackFormat::Mono : v == "Stereo" ? TrackFormat::Stereo : TrackFormat::Surround;
                       p.assignInputsFrom (t, first);            // stereo: first, first+1 ...
                       p.structureChanged();
                   }, { "Mono", "Stereo", "Surround" }, {} });
    g.addColumn ({ "Ch", 40, GridEditor::Type::Text,
                   [&p] (int r) { return juce::String (p.tracks[(size_t) r].channelCount()); },
                   [this, &p] (int r, const juce::String& v)
                   {
                       auto& t = p.tracks[(size_t) r];
                       if (! locked() && t.format == TrackFormat::Surround)
                       {
                           const int first = juce::jmax (0, t.inputOf (0));
                           t.surroundChannels = juce::jlimit (3, kMaxTrackChannels, v.getIntValue());
                           p.assignInputsFrom (t, first);
                           p.structureChanged();
                       }
                       else refreshAll();
                   }, {}, {} });
    g.addColumn ({ "Input (first channel)", 215, GridEditor::Type::Combo,
                   [&p] (int r) { return p.inputLabel (p.tracks[(size_t) r].inputOf (0)); },
                   [this, &p] (int r, const juce::String& v)
                   {
                       if (locked()) { refreshAll(); return; }
                       auto& t = p.tracks[(size_t) r];
                       const int in = parseInputItem (v);
                       if (in >= 0) p.assignInputsFrom (t, in);   // a stereo track fills in the next input as well
                       else t.inputs[0] = -1;
                       p.structureChanged();
                   }, {}, [this] (int) { return inputItems (true); } });
    g.addColumn ({ "Input (second channel)", 215, GridEditor::Type::Combo,
                   [&p] (int r)
                   {
                       auto& t = p.tracks[(size_t) r];
                       if (t.format == TrackFormat::Mono) return juce::String ("-");
                       if (t.format == TrackFormat::Surround) return juce::String ("(follows the first)");
                       return p.inputLabel (t.inputOf (1));
                   },
                   [this, &p] (int r, const juce::String& v)
                   {
                       auto& t = p.tracks[(size_t) r];
                       if (locked() || t.format != TrackFormat::Stereo) { refreshAll(); return; }
                       t.inputs[1] = parseInputItem (v);
                       p.structureChanged();
                   }, {}, [this, &p] (int r)
                   {
                       auto f = p.tracks[(size_t) r].format;
                       if (f == TrackFormat::Mono) return juce::StringArray { "-" };
                       if (f == TrackFormat::Surround) return juce::StringArray { "(follows the first)" };
                       return inputItems (true);
                   } });

    auto add = [this, &p] (TrackFormat f, const char* label)
    {
        if (locked()) return;
        const int first = nextFreeInput();
        juce::String n = juce::String (label) + " " + juce::String ((int) p.tracks.size() + 1);
        if (f == TrackFormat::Mono && juce::isPositiveAndBelow (first, (int) p.inputs.size()) && p.inputs[(size_t) first].name.isNotEmpty())
            n = p.inputs[(size_t) first].name;
        p.addTrack (n, f, first);
        tracksPage.grid.refresh();                                   // show it in the list straight away
        tracksPage.grid.selectRow ((int) p.tracks.size() - 1);
    };
    tracksPage.addButton ("Add mono track",     [add] { add (TrackFormat::Mono, "Mono"); });
    tracksPage.addButton ("Add stereo track",   [add] { add (TrackFormat::Stereo, "Stereo"); });
    tracksPage.addButton ("Add surround track", [add] { add (TrackFormat::Surround, "Surround"); });
    auto move = [this, &p] (int row, int delta)
    {
        if (locked() || row < 0) return;
        const int to = row + delta;
        if (! juce::isPositiveAndBelow (to, (int) p.tracks.size())) return;
        p.moveTrack (row, to);
        tracksPage.grid.selectRow (to);
    };
    auto remove = [this, &p] (int row)
    {
        if (row < 0 || row >= (int) p.tracks.size()) return;
        if (locked()) { showError ("Recording", "The project design is locked while recording."); return; }
        const auto id = p.tracks[(size_t) row].id;
        confirmAsync ("Delete track", "Delete the track '" + p.tracks[(size_t) row].name + "'?\n\nFiles already recorded stay on the disk; only the track is removed from the project.", "Delete",
                      [this, &p, id]
                      {
                          for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].id == id) { p.removeTrack ((int) i); break; }
                          refreshAll();
                      });
    };
    tracksPage.addButton ("Move up",   [this, move] { move (tracksPage.grid.getSelectedRow(), -1); });
    tracksPage.addButton ("Move down", [this, move] { move (tracksPage.grid.getSelectedRow(), 1); });
    tracksPage.addButton ("Delete selected", [this, remove] { remove (tracksPage.grid.getSelectedRow()); });
    g.addColumn ({ "", 52, GridEditor::Type::Button, [] (int) { return juce::String ("Up"); },   [move] (int r, const juce::String&) { move (r, -1); }, {}, {} });
    g.addColumn ({ "", 60, GridEditor::Type::Button, [] (int) { return juce::String ("Down"); }, [move] (int r, const juce::String&) { move (r, 1); }, {}, {} });
    g.addColumn ({ "", 80, GridEditor::Type::Button, [] (int) { return juce::String ("Delete"); }, [remove] (int r, const juce::String&) { remove (r); }, {}, {} });
    g.onDeleteKey = remove;
    g.addContextItems = [move, remove, &p] (int row, juce::PopupMenu& m)
    {
        if (! juce::isPositiveAndBelow (row, (int) p.tracks.size())) return;
        m.addItem ("Move up",   row > 0, false, [move, row] { move (row, -1); });
        m.addItem ("Move down", row + 1 < (int) p.tracks.size(), false, [move, row] { move (row, 1); });
        m.addSeparator();
        m.addItem ("Delete track '" + p.tracks[(size_t) row].name + "'", [remove, row] { remove (row); });
    };
    tracksPage.note.setText ("Pick the input (or inputs) of each track here. Choosing a stereo track's first input fills in the next one too - change the second if you need to. "
                             "Click a row to select it; the Delete button on the row (or the Delete key, or right-click) removes it. Each armed track creates its own file: 005 - Take - violin.wav", juce::dontSendNotification);
    tracksPage.note.setJustificationType (juce::Justification::topLeft);
}

// -------------------------------------------------------------------- Int buses and Ext buses
void DesignComponent::buildBusPage (Page& page, bool external)
{
    auto& p = app.project;
    auto& g = page.grid;
    const juce::String kind = external ? "Ext bus" : "Int bus";
    auto globalIndex = [&p, external] (int r) { const auto v = p.busIndices (external); return juce::isPositiveAndBelow (r, (int) v.size()) ? v[(size_t) r] : -1; };
    g.setRowCountProvider ([&p, external] { return (int) p.busIndices (external).size(); });
    g.addColumn ({ "#", 34, GridEditor::Type::ReadOnly, [] (int r) { return juce::String (r + 1); }, {}, {}, {} });
    g.addColumn ({ "Name", 300, GridEditor::Type::Text,
                   [&p, globalIndex] (int r) { const int i = globalIndex (r); return i >= 0 ? p.buses[(size_t) i].name : juce::String(); },
                   [this, &p, globalIndex] (int r, const juce::String& v) { const int i = globalIndex (r); if (i >= 0 && ! locked()) { p.buses[(size_t) i].name = v; p.structureChanged(); } }, {}, {} });

    page.addButton ("Add " + kind.toLowerCase(), [this, &p, &page, external, kind]
    {
        if (locked()) return;
        p.addBus (kind + " " + juce::String ((int) p.busIndices (external).size() + 1), external);
        page.grid.refresh();                                          // shows at the end of the list, in the order added
        page.grid.selectRow ((int) p.busIndices (external).size() - 1);
    });
    auto move = [this, &p, &page, globalIndex] (int r, int delta)
    {
        if (locked() || r < 0) return;
        const int i = globalIndex (r);
        if (i < 0) return;
        p.moveBus (i, delta);
        page.grid.selectRow (r + delta);
    };
    auto remove = [this, &p, globalIndex, kind] (int r)
    {
        const int i = globalIndex (r);
        if (i < 0) return;
        if (locked()) { showError ("Recording", "The project design is locked while recording."); return; }
        const auto id = p.buses[(size_t) i].id;
        confirmAsync ("Delete " + kind.toLowerCase(), "Delete the " + kind.toLowerCase() + " '" + p.buses[(size_t) i].name + "'?\n\nSends to it are removed with it.", "Delete",
                      [this, &p, id]
                      {
                          for (size_t k = 0; k < p.buses.size(); ++k) if (p.buses[k].id == id) { p.removeBus ((int) k); break; }
                          refreshAll();
                      });
    };
    g.addColumn ({ "", 52, GridEditor::Type::Button, [] (int) { return juce::String ("Up"); },   [move] (int r, const juce::String&) { move (r, -1); }, {}, {} });
    g.addColumn ({ "", 60, GridEditor::Type::Button, [] (int) { return juce::String ("Down"); }, [move] (int r, const juce::String&) { move (r, 1); }, {}, {} });
    g.addColumn ({ "", 80, GridEditor::Type::Button, [] (int) { return juce::String ("Delete"); }, [remove] (int r, const juce::String&) { remove (r); }, {}, {} });
    g.onDeleteKey = remove;
    page.addButton ("Move up",   [&page, move] { move (page.grid.getSelectedRow(), -1); });
    page.addButton ("Move down", [&page, move] { move (page.grid.getSelectedRow(), 1); });
    page.addButton ("Delete selected", [&page, remove] { remove (page.grid.getSelectedRow()); });
    g.addContextItems = [move, remove, globalIndex, &p, &page] (int row, juce::PopupMenu& m)
    {
        const int i = globalIndex (row);
        if (i < 0) return;
        const int count = (int) p.busIndices (p.buses[(size_t) i].external).size();
        m.addItem ("Move up",   row > 0, false, [move, row] { move (row, -1); });
        m.addItem ("Move down", row + 1 < count, false, [move, row] { move (row, 1); });
        m.addSeparator();
        m.addItem ("Delete '" + p.buses[(size_t) i].name + "'", [remove, row] { remove (row); });
    };
    page.note.setText (external
        ? "An Ext Bus can be fed by any audio track or Int Bus (dials at the top of their mixer strips) and MUST go to driver outputs - pick the pair "
          "on the bus's own strip in each mixer - or nowhere at all. Ext buses are the only way out to the audio driver (there is no Main output). Listed in the order you added them; use Up / Down to rearrange."
        : "An Int Bus is fed by dials on audio tracks and other Int Buses. It can go on to audio tracks, other Int Buses or Ext Buses (its own dials), "
          "Listed in the order you added them; use Up / Down to rearrange, Delete to remove.", juce::dontSendNotification);
    page.note.setJustificationType (juce::Justification::topLeft);
}

// -------------------------------------------------------------------- mixers
void DesignComponent::buildMixersPage()
{
    auto& p = app.project;
    auto& g = mixersPage.grid;
    g.setRowCountProvider ([&p] { return (int) p.mixers.size(); });
    g.addColumn ({ "Mixer name", 260, GridEditor::Type::Text,
                   [&p] (int r) { return p.mixers[(size_t) r]->name; },
                   [&p] (int r, const juce::String& v) { p.mixers[(size_t) r]->name = v; p.structureChanged(); }, {}, {} });
    g.addColumn ({ "Outputs (set on each Ext bus in the mixer)", 330, GridEditor::Type::ReadOnly, [&p] (int r) { return p.mixerOutputsText (*p.mixers[(size_t) r]); }, {}, {}, {} });
    mixersPage.addButton ("Add mixer", [this, &p]
    {
        auto& m = p.addMixer ("Cue mixer " + juce::String ((int) p.mixers.size()));
        p.structureChanged();
        mixersPage.grid.refresh();
        mixersPage.grid.selectRow ((int) p.mixers.size() - 1);
    });
    auto remove = [this, &p] (int r)
    {
        if (r < 0 || r >= (int) p.mixers.size()) return;
        if (r == 0) { showError ("Mixers", "The processing mixer cannot be deleted. Only cue mixers can."); return; }
        const auto id = p.mixers[(size_t) r]->id;
        confirmAsync ("Delete mixer", "Delete the mixer '" + p.mixers[(size_t) r]->name + "'?", "Delete",
                      [this, &p, id]
                      {
                          for (size_t i = 0; i < p.mixers.size(); ++i) if (p.mixers[i]->id == id) { p.removeMixer ((int) i); break; }
                          refreshAll();
                      });
    };
    auto open = [this, &p] (int r) { if (r >= 0 && app.showMixer) app.showMixer (p.mixers[(size_t) r]->id); };
    g.addColumn ({ "", 80, GridEditor::Type::Button, [] (int) { return juce::String ("Delete"); }, [remove] (int r, const juce::String&) { remove (r); }, {}, {} });
    g.onDeleteKey = remove;
    mixersPage.addButton ("Delete selected", [this, remove] { remove (mixersPage.grid.getSelectedRow()); });
    mixersPage.addButton ("Open selected mixer", [this, open] { open (mixersPage.grid.getSelectedRow()); });
    g.addContextItems = [remove, open, &p] (int row, juce::PopupMenu& m)
    {
        if (! juce::isPositiveAndBelow (row, (int) p.mixers.size())) return;
        m.addItem ("Open mixer", [open, row] { open (row); });
        m.addItem ("Delete mixer", row > 0, false, [remove, row] { remove (row); });
    };
    mixersPage.note.setText ("Each mixer is completely independent: its own solos, mutes, levels, sends and plug-ins, and its own Ext buses with their own driver outputs "
                             "- so different listeners can hear different mixes. The first mixer is the processing mixer and cannot be deleted; the others are cue mixers.", juce::dontSendNotification);
    mixersPage.note.setJustificationType (juce::Justification::topLeft);
}
} // namespace td

#include <array>
#include <limits>
#include "WaveDraw.h"
#include "TakeWindow.h"
#include "PaintProfile.h"
#include "FixTools.h"
#include "ColourPicker.h"
#include "MarkerStrip.h"
#include "TakeSheetExport.h"

namespace td
{
static constexpr int kRulerH = 34 + markerui::kStripH, kLaneH = 50;
static int kNameW = 150;                          // width of the track-name column (drag its right edge to change it; shared by all take windows)      // (the top 14 pixels of the ruler band are the marker strip)

/** The scrolling content: ruler, one lane of take names, then one row per track. */
/** The little non-modal box for a take (right-click it): IN Bar / OUT Bar side by side, and a Notes box under them (so it can be filled in while recording, with the
    keyboard already in the IN box). Enter in IN goes to OUT, Enter in OUT saves; Tab goes on to Notes (Enter there is a new line, Ctrl + Enter saves). */
class BarsPopup : public juce::Component
{
public:
    BarsPopup (const juce::String& title, int bin, int bout, const juce::String& notes, std::function<void (int, int, const juce::String&)> ok, std::function<void()> more, std::function<void()> close)
        : onOk (std::move (ok)), onMore (std::move (more)), onClose (std::move (close)), origIn (juce::jmax (0, bin)), origOut (juce::jmax (0, bout)), origNotes (notes.trim())
    {
        setSize (400, 196);
        titleLabel.setText (title, juce::dontSendNotification); titleLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold)); titleLabel.setColour (juce::Label::textColourId, theme::text);
        inLabel.setText ("IN Bar", juce::dontSendNotification); outLabel.setText ("OUT Bar", juce::dontSendNotification); notesLabel.setText ("Notes", juce::dontSendNotification);
        for (auto* l : { &inLabel, &outLabel, &notesLabel }) { l->setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); }
        for (auto* e : { &inEd, &outEd })
        {
            e->setInputRestrictions (5, "0123456789"); e->setSelectAllWhenFocused (true); e->setJustification (juce::Justification::centredLeft);
            e->onEscapeKey = [this] { if (onClose) onClose(); };
            addAndMakeVisible (e);
        }
        notesEd.setMultiLine (true, true); notesEd.setReturnKeyStartsNewLine (true); notesEd.setTabKeyUsedAsCharacter (false);
        notesEd.setInputRestrictions (2000); notesEd.setScrollbarsShown (true);
        notesEd.setTextToShowWhenEmpty ("a note about this take (shown when you point at it, and on the take sheet)", theme::text.withAlpha (0.4f));
        notesEd.setText (notes, false);
        notesEd.onEscapeKey = [this] { if (onClose) onClose(); };
        notesEd.onCtrlReturn = [this] { commit(); };
        addAndMakeVisible (notesEd);
        inEd.setText (bin > 0 ? juce::String (bin) : juce::String(), false); outEd.setText (bout > 0 ? juce::String (bout) : juce::String(), false);
        inEd.onReturnKey = [this] { outEd.grabKeyboardFocus(); outEd.selectAll(); };
        outEd.onReturnKey = [this] { commit(); };
        okBtn.onClick = [this] { commit(); };
        moreBtn.onClick = [this] { if (onMore) onMore(); };
        for (auto* b : { &okBtn, &moreBtn }) addAndMakeVisible (b);
        addAndMakeVisible (titleLabel);
        setFocusContainerType (FocusContainerType::keyboardFocusContainer);       // (Tab goes IN -> OUT -> Notes -> the buttons, and round)
    }
    void focusFirst() { inEd.grabKeyboardFocus(); inEd.selectAll(); }
    /** What is typed in the box now; true when it differs from what the box opened with. */
    bool typed (int& bin, int& bout, juce::String& notes) const
    {
        bin = inEd.getText().getIntValue(); bout = outEd.getText().getIntValue(); notes = notesEd.getText().trim();
        return bin != origIn || bout != origOut || notes != origNotes;
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel); g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff19d3b5)); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f, 2.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        titleLabel.setBounds (r.removeFromTop (20)); r.removeFromTop (4);
        auto a = r.removeFromTop (26);                                                                  // IN and OUT side by side
        inLabel.setBounds (a.removeFromLeft (52)); inEd.setBounds (a.removeFromLeft (70)); a.removeFromLeft (22);
        outLabel.setBounds (a.removeFromLeft (62)); outEd.setBounds (a.removeFromLeft (70)); r.removeFromTop (6);
        notesLabel.setBounds (r.removeFromTop (18));
        notesEd.setBounds (r.removeFromTop (70)); r.removeFromTop (8);
        auto c = r.removeFromTop (grid::btnH); okBtn.setBounds (c.removeFromLeft (70)); c.removeFromLeft (6); moreBtn.setBounds (c.removeFromLeft (90));
    }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; } return false; }
private:
    /** A multi-line box where Enter is a new line and Ctrl + Enter saves the whole popup. */
    struct NotesEditor : public juce::TextEditor
    {
        bool keyPressed (const juce::KeyPress& k) override
        {
            if ((k.getKeyCode() == juce::KeyPress::returnKey) && (k.getModifiers().isCtrlDown() || k.getModifiers().isCommandDown())) { if (onCtrlReturn) onCtrlReturn(); return true; }
            return juce::TextEditor::keyPressed (k);
        }
        std::function<void()> onCtrlReturn;
    };
    void commit() { if (onOk) onOk (inEd.getText().getIntValue(), outEd.getText().getIntValue(), notesEd.getText().trim()); }
    juce::Label titleLabel, inLabel, outLabel, notesLabel;
    juce::TextEditor inEd, outEd;
    NotesEditor notesEd;
    juce::TextButton okBtn { "OK" }, moreBtn { "More..." };
    std::function<void (int, int, const juce::String&)> onOk; std::function<void()> onMore, onClose;
    const int origIn, origOut; const juce::String origNotes;
};

class TakeTimeline : public juce::Component, public juce::TooltipClient, private juce::ChangeListener, private juce::Timer
{
public:
    /** Pointing at a take that has notes shows them. */
    juce::String getTooltip() override
    {
        if (auto* g = groupAt (getMouseXYRelative(), true))
            if (g->notes.isNotEmpty()) return "Take " + pad3 (g->number) + " notes (right-click the take to change them):\n" + g->notes;
        return {};
    }

    TakeTimeline (AppContext& a, const juce::Uuid& wid, juce::Viewport& vp)
        : app (a), windowId (wid), viewport (vp)
    {
        formatManager.registerBasicFormats();
        setWantsKeyboardFocus (true);
        startTimerHz (30);
        app.project.addChangeListener (this);
    }
    ~TakeTimeline() override { app.project.removeChangeListener (this); }

    /** The viewport would use the arrow keys to scroll: hand every key straight to the take window instead (it zooms with them). */
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (auto* w = findParentComponentOfClass<TakeWindowComponent>()) return w->keyPressed (k);
        return false;
    }

    double pixelsPerSecond = 12.0;
    int barHighlight = 0;                       // "Search for bar": takes that contain this bar get a blue-green frame (0 = off)
    std::vector<juce::Uuid> selectedTakes;      // takes picked for Bounce Out (click one; Ctrl + click adds or removes) - drawn a little darker
    std::vector<juce::Uuid> selectedTracks;     // track names picked (click; Shift + click: every track in between; Ctrl + click: one more / less) - right-click: remove them
    juce::Uuid trackAnchor;
    /** A click on a track's name: on its own = only this track (or none, if it was the only one); Shift = every track from the last one clicked to
        this one; Ctrl = this one in or out. */
    void clickTrackName (const juce::Uuid& id, const juce::ModifierKeys& mods)
    {
        const auto shown = shownTracks();
        auto rowOfId = [&] (const juce::Uuid& t) { for (size_t i = 0; i < shown.size(); ++i) if (shown[i]->id == t) return (int) i; return -1; };
        const bool ctrl = mods.isCtrlDown() || mods.isCommandDown();
        const int a = rowOfId (trackAnchor), b = rowOfId (id);
        auto has = [this] (const juce::Uuid& t) { return std::find (selectedTracks.begin(), selectedTracks.end(), t) != selectedTracks.end(); };
        if (mods.isShiftDown() && a >= 0 && b >= 0)
        {
            if (! ctrl) selectedTracks.clear();
            for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i) if (! has (shown[(size_t) i]->id)) selectedTracks.push_back (shown[(size_t) i]->id);
        }
        else if (ctrl)
        {
            if (has (id)) selectedTracks.erase (std::remove (selectedTracks.begin(), selectedTracks.end(), id), selectedTracks.end());
            else selectedTracks.push_back (id);
            trackAnchor = id;
        }
        else
        {
            if (! mods.isShiftDown() && selectedTracks.size() == 1 && selectedTracks.front() == id) selectedTracks.clear();
            else selectedTracks.assign (1, id);
            trackAnchor = id;
        }
        repaint();
    }
    bool trackSelected (const juce::Uuid& id) const { return std::find (selectedTracks.begin(), selectedTracks.end(), id) != selectedTracks.end(); }
    bool isTakeSelected (const juce::Uuid& id) const { return std::find (selectedTakes.begin(), selectedTakes.end(), id) != selectedTakes.end(); }
    static constexpr int kHeightLevels = 8;          // 0 = smallest (32 tracks per page) ... 7 = largest (one stereo track per page)
    int heightLevel = 3;
    int rowH = 56;

    /** Row height for a level: geometric steps between "32 rows fill the page" and "one row fills the page, with a peek of the next". */
    void applyHeightLevel()
    {
        const int avail = juce::jmax (120, viewport.getMaximumVisibleHeight() - kRulerH - kLaneH);
        const double rowMin = juce::jmax (10.0, (double) avail / 32.0), rowMax = juce::jmax (rowMin * 2.0, (double) avail - 24.0);
        rowH = (int) std::round (rowMin * std::pow (rowMax / rowMin, (double) heightLevel / (kHeightLevels - 1)));
        updateSize();
        repaint();
    }
    void changeHeightLevel (int delta)
    {
        heightLevel = juce::jlimit (0, kHeightLevels - 1, heightLevel + delta);
        applyHeightLevel();
    }

    /** Zooms in / out around the playhead (where it is recording or playing, otherwise where you put it) and puts that point in the MIDDLE of the
        screen. If the playhead is off screen, the middle of what is shown is the centre instead. */
    void zoom (double factor)
    {
        const int visW = viewport.getMaximumVisibleWidth();
        const int keep = kNameW + (visW - kNameW) / 2;                              // the middle of the time area
        double anchor = anchorSeconds();
        const int off = xOf (anchor) - viewport.getViewPositionX();
        if (off < kNameW || off > visW) anchor = (double) (viewport.getViewPositionX() + keep - kNameW) / pixelsPerSecond;
        pixelsPerSecond = juce::jlimit (juce::jmin (1.0, juce::jmax (0.01, (double) (visW - kNameW) / timelineSeconds())), 800.0, pixelsPerSecond * factor);   // out as far as the whole timeline fits
        updateSize();
        viewport.setViewPosition (juce::jmax (0, xOf (anchor) - keep), viewport.getViewPositionY());
        repaint();
    }

    /** Scrolls sideways (if needed) so that this time can be seen. */
    void scrollTo (double seconds)
    {
        const int x = xOf (seconds), vx = viewport.getViewPositionX(), right = vx + viewport.getMaximumVisibleWidth() - 6;
        if (x < vx + kNameW || x > right) viewport.setViewPosition (juce::jmax (0, x - kNameW - 40), viewport.getViewPositionY());
    }
    /** C: scrolls so that the playhead is in the middle of the window; the zoom stays the same. */
    void centreOnPlayhead()
    {
        const int visW = viewport.getMaximumVisibleWidth();
        viewport.setViewPosition (juce::jmax (0, xOf (anchorSeconds()) - (kNameW + (visW - kNameW) / 2)), viewport.getViewPositionY());
    }

    /** The tracks shown in this take window (the ones you removed from it are left out). */
    std::vector<const TrackDef*> shownTracks() const
    {
        std::vector<const TrackDef*> v;
        auto* w = const_cast<TakeTimeline*> (this)->def();
        for (auto& t : app.project.tracks) if (w == nullptr || (! w->isHidden (t.id) && (w->showLtc || ! t.isLtc))) v.push_back (&t);
        return v;
    }

    /** How long the timeline is: the material (or the playhead, if that is further) and then as much empty room again (at least 10 minutes), so there is
        always space after the end to drag more audio onto. At most 24 hours. */
    double timelineSeconds()
    {
        auto* w = def();
        const double content = juce::jmax (juce::jmax (0.0, w ? w->endSeconds() : 0.0), juce::jmax (livePosition(), w ? w->playheadSeconds : 0.0));
        return juce::jmin (86400.0, content + juce::jmax (600.0, content));
    }
    void updateSize()
    {
        const double end = timelineSeconds();
        // always at least as big as the visible area, so the ruler, the grid and the empty rows fill the whole window
        const int contentW = (int) (end * pixelsPerSecond) + kNameW;
        const int contentH = kRulerH + kLaneH + juce::jmax (1, (int) shownTracks().size()) * rowH + 8;
        setSize (juce::jmax (contentW, viewport.getMaximumVisibleWidth()), juce::jmax (contentH, viewport.getMaximumVisibleHeight()));
    }

    /** The top band (marker strip, ruler, take banners, In / Out flags) stays at the top of the window however far you scroll down:
        the picture is painted once where it is, then the band is painted again over the top of the visible part. */
    void paint (juce::Graphics& g) override
    {
        BusyLabel busyLabel ("drawing a take window");
        const auto tPaint0 = paintprof::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
        headerPass = false;
        paintAll (g);
        const int sy = viewport.getViewPositionY();
        if (sy > 0)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<int> (0, sy, getWidth(), kRulerH + kLaneH));
            g.setOrigin (0, sy);
            headerPass = true;
            paintAll (g);
            headerPass = false;
        }
        if (lassoMoved)                                              // Alt + drag: the lasso
        {
            const auto lr = lassoRect();
            g.setColour (theme::accent.withAlpha (0.13f)); g.fillRect (lr);
            g.setColour (theme::accent.withAlpha (0.9f));  g.drawRect (lr, 1);
        }
        if (moveActive && moveMoved) paintMoveGhost (g);             // Ctrl + Shift + drag: where the clips will go
        if (paintprof::enabled()) paintprof::paintDone ("take window", juce::Time::getMillisecondCounterHiRes() - tPaint0, g.getClipBounds().getWidth(), g.getClipBounds().getHeight());
    }
    // ---- Alt + drag: a lasso; every take it touches is selected (whole takes)
    bool lassoActive = false, lassoMoved = false; juce::Point<int> lassoFrom, lassoTo;
    // ---- Ctrl + Shift + drag up or down: the clips the selected takes have on one track moved onto another track (the takes stay where they are in time;
    // an import that put the audio on new tracks of its own). Takes the mouse is on; the selected ones if it is one of them. A click without a drag is a Ctrl + click.
    bool moveActive = false, moveMoved = false; int moveRow0 = -1, moveRow = -1, moveDownY = 0;
    juce::Uuid moveFrom, moveClickTake; std::vector<juce::Uuid> moveTakes; juce::Point<int> moveMouse; juce::Rectangle<int> moveLabelArea;
    /** Ctrl + A: every take of the window (not the one being recorded). */
    void selectAllTakes()
    {
        selectedTakes.clear();
        if (auto* w = def())
            for (auto& grp : w->groups) if (! (app.engine.isRecording() && grp.id == app.engine.currentTakeId())) selectedTakes.push_back (grp.id);
        repaint();
    }
    juce::Rectangle<int> lassoRect() const
    {
        return juce::Rectangle<int>::leftTopRightBottom (juce::jmin (lassoFrom.x, lassoTo.x), juce::jmin (lassoFrom.y, lassoTo.y),
                                                         juce::jmax (lassoFrom.x, lassoTo.x) + 1, juce::jmax (lassoFrom.y, lassoTo.y) + 1);
    }
    /** The takes that 'box' touches: its banner or any of its tracks' rows. */
    std::vector<juce::Uuid> takesIn (juce::Rectangle<int> box)
    {
        std::vector<juce::Uuid> out;
        auto* w = def();
        if (w == nullptr) return out;
        const auto shown = shownTracks();
        std::map<juce::Uuid, int> rowOf;
        for (size_t i = 0; i < shown.size(); ++i) rowOf[shown[i]->id] = (int) i;
        const int top = kRulerH + kLaneH;
        for (auto& grp : w->groups)
        {
            if (app.engine.isRecording() && grp.id == app.engine.currentTakeId()) continue;      // (the take being recorded cannot be selected, as with a click)
            const int x0 = xOf (grp.startSeconds), x1 = x0 + juce::jmax (4, (int) (grp.lengthSeconds() * pixelsPerSecond));
            if (x1 < box.getX() || x0 > box.getRight()) continue;
            bool hit = juce::Rectangle<int> (x0, kRulerH, x1 - x0, kLaneH).intersects (box);
            for (auto& f : grp.files)
                if (auto it = rowOf.find (f.trackId); ! hit && it != rowOf.end()) hit = juce::Rectangle<int> (x0, top + it->second * rowH, x1 - x0, rowH).intersects (box);
            if (hit) out.push_back (grp.id);
        }
        return out;
    }
    // ---- the corner above the first track's name: what Ctrl / Alt / Shift do here, while they are held
    int shownMods = 0;
    static juce::StringArray modifierHints (int bits)
    {
        using namespace modhint;
        switch (bits)
        {
            case ctrl:         return { "Click a take or a track's name: add it to the selection or take it out", "Ctrl + A: select every take", "Ctrl + Z / Ctrl + Y: undo / redo" };
            case alt:          return { "Drag: lasso - selects every take it touches" };
            case ctrl | alt:   case ctrl | alt | shift:
                               return { "Drag over a take: mark 1 - 2 on only the tracks you drag over" };
            case alt | shift:  return { "Drag: lasso - selects every take it touches" };
            case shift:        return { "Click a track's name: select every track from the last one clicked", "Left / Right arrow: nudge the playhead by 10 ms", "4: send the marked part to the edit's playhead as an INSERT (the rest moves later; 4 alone replaces)" };
            case ctrl | shift: return { "Drag a clip up or down: the selected takes' clips on that track go onto another track (they stay where they are in time)", "Ctrl + Shift + Z: redo" };
            default:           return {};
        }
    }
    bool headerPass = false;
    int hdrShift = 0;
    int stickyShift (const juce::MouseEvent& e) const
    {
        const int sy = viewport.getViewPositionY();
        return (sy > 0 && e.y >= sy && e.y < sy + kRulerH + kLaneH) ? sy : 0;
    }
    juce::MouseEvent shifted (const juce::MouseEvent& e, int by) const { return by == 0 ? e : e.withNewPosition (e.position - juce::Point<float> (0.0f, (float) by)); }
    void mouseDown (const juce::MouseEvent& e0) override { hdrShift = stickyShift (e0); mouseDownImpl (shifted (e0, hdrShift)); }
    void mouseDrag (const juce::MouseEvent& e0) override { mouseDragImpl (shifted (e0, hdrShift)); }
    void mouseUp (const juce::MouseEvent& e0) override { const int by = hdrShift; hdrShift = 0; mouseUpImpl (shifted (e0, by)); }
    void mouseMove (const juce::MouseEvent& e0) override { mouseMoveImpl (shifted (e0, stickyShift (e0))); }

    void paintAll (juce::Graphics& g)
    {
        g.fillAll (theme::wave);
        auto* w = def();
        if (w == nullptr) return;
        const auto tracks = headerPass ? std::vector<const TrackDef*>() : shownTracks();
        const int top = kRulerH + kLaneH;
        const int rowsBottom = top + juce::jmax (1, (int) tracks.size()) * rowH;

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            g.setColour ((i & 1) ? theme::waveAlt : theme::wave);
            g.fillRect (0, top + (int) i * rowH, getWidth(), rowH);
            theme::paintStaff (g, { 0, top + (int) i * rowH, getWidth(), rowH });                                         // (the Manuscript UI: staves)
        }
        for (int i = (int) tracks.size(), y = top + (int) tracks.size() * rowH; y < getHeight(); ++i, y += rowH)       // empty rows carry on to the bottom of the window
        {
            g.setColour (((i & 1) ? theme::waveAlt : theme::wave).withMultipliedBrightness (0.97f));
            g.fillRect (0, y, getWidth(), rowH);
        }

        paintprof::Scope profGrid ("take window", "grid");
        g.setColour (theme::ruler); g.fillRect (0, 0, getWidth(), kRulerH);
        g.setColour (juce::Colours::black.withAlpha (0.22f)); g.fillRect (0, 0, getWidth(), markerui::kStripH);          // the marker strip
        const double step = pixelsPerSecond >= 200 ? 0.5 : pixelsPerSecond >= 60 ? 1.0 : pixelsPerSecond >= 24 ? 5.0 : pixelsPerSecond >= 8 ? 10.0 : pixelsPerSecond >= 3 ? 60.0 : 300.0;
        // fainter unlabelled lines between the labelled ones: 5 s -> every 1 s, 1 s -> every 0.5 s, then 0.25 s (two zoom steps), then 0.1 s however far in
        const double minor = pixelsPerSecond >= 450 ? 0.1 : pixelsPerSecond >= 200 ? 0.25 : pixelsPerSecond >= 60 ? 0.5 : pixelsPerSecond >= 24 ? 1.0 : pixelsPerSecond >= 8 ? 5.0 : pixelsPerSecond >= 3 ? 10.0 : 60.0;
        const int minorPer = juce::jmax (1, (int) std::lround (step / minor));
        for (int n = 0; (double) n * minor * pixelsPerSecond < getWidth(); ++n)
        {
            if (n % minorPer == 0) continue;
            g.setColour (theme::grid.withAlpha (0.45f)); g.drawVerticalLine (kNameW + (int) ((double) n * minor * pixelsPerSecond), (float) kRulerH - 6, (float) getHeight());
        }
        g.setFont (11.0f);
        for (double s = 0; s * pixelsPerSecond < getWidth(); s += step)
        {
            const int x = kNameW + (int) (s * pixelsPerSecond);
            g.setColour (theme::grid); g.drawVerticalLine (x, (float) kRulerH - 12, (float) getHeight());
            g.setColour (theme::dimText); g.setFont (12.0f); g.drawText (formatTimeShort (s, 0), x + 3, 3 + markerui::kStripH, 64, 16, juce::Justification::left);
        }
        g.setColour (theme::border); g.fillRect (0, kRulerH - 1, getWidth(), 1);
        profGrid.next ("takes");

        const bool recordingHere = app.engine.isRecording() && w->findGroup (app.engine.currentTakeId()) != nullptr;
        const double playhead = app.playheadSeconds();
        struct UseFlag { int x, y0, y1; bool isIn; int number; bool snip = false; };
        std::vector<UseFlag> useFlags;
        // Only what is inside the clip is drawn: a take wholly left or right of it is skipped, and so are the rows above and below it. With 60 takes
        // of 20 tracks in a window this is what keeps the 30 Hz meter repaint (a 150 px column) from costing as much as the whole picture.
        const auto clip = g.getClipBounds();
        std::map<juce::Uuid, int> rowOf; std::vector<juce::Colour> rowColour;          // track -> its row, and each row's colour (looked up once per paint, not per clip)
        for (size_t i = 0; i < tracks.size(); ++i) { rowOf[tracks[i]->id] = (int) i; rowColour.push_back (chan::of (app.project, tracks[i]->id)); }
        const auto rowOfTrack = [&] (const juce::Uuid& id) { auto it = rowOf.find (id); return it != rowOf.end() ? it->second : -1; };
        const bool lanesVisible = clip.getY() < top;                                      // the banner strip and the flags are above the rows
        for (auto& grp : w->groups)
        {
            const bool live = recordingHere && grp.id == app.engine.currentTakeId();
            const bool finishing = ! live && app.engine.isFinishing (grp.id);   // stopped, its files still being finished on the disk: its live waveform stays until they are
            const double len = live ? app.engine.recordedSeconds() : grp.lengthSeconds();
            const int x0 = xOf (grp.startSeconds);
            const int wpx = juce::jmax (4, (int) (len * pixelsPerSecond));
            if (! live && (x0 - 60 > clip.getRight() || x0 + wpx + 60 < clip.getX())) continue;        // (60 px: the flags and the fade corners reach outside the take)

            auto lane = juce::Rectangle<int> (x0, kRulerH + 16, wpx, 20);      // under the row of marks 1 / 2 flags and above the row of In / Out flags
            const bool picked = isTakeSelected (grp.id);
            if (lanesVisible)
            {
                const auto plate = live ? juce::Colour (0xffb03030) : (picked ? theme::accent.darker (0.45f) : theme::accent);
                g.setColour (plate);
                g.fillRect (lane);
                g.setColour (theme::manuscript() ? theme::inkOn (plate) : juce::Colours::white); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                const bool hasNote = grp.notes.isNotEmpty() && lane.getWidth() >= 44;
                g.drawText (w->displayName (grp), lane.reduced (6, 0).withTrimmedRight (hasNote ? 16 : 0), juce::Justification::centredLeft, true);
                if (hasNote)                                    // a little page with a turned corner: this take has notes (point at it to read them)
                {
                    const auto pg = juce::Rectangle<float> ((float) lane.getRight() - 16.0f, (float) lane.getY() + 4.0f, 11.0f, 13.0f);
                    juce::Path page; page.startNewSubPath (pg.getX(), pg.getY()); page.lineTo (pg.getRight() - 3.5f, pg.getY()); page.lineTo (pg.getRight(), pg.getY() + 3.5f);
                    page.lineTo (pg.getRight(), pg.getBottom()); page.lineTo (pg.getX(), pg.getBottom()); page.closeSubPath();
                    g.setColour (juce::Colour (0xfffff3b0)); g.fillPath (page);
                    g.setColour (juce::Colour (0xff6b5a10));
                    for (int ln = 0; ln < 3; ++ln) g.fillRect (pg.getX() + 2.0f, pg.getY() + 4.5f + (float) ln * 2.8f, ln == 2 ? 4.0f : 7.0f, 1.0f);
                }
            }
            if (! live && lanesVisible)                     // the take's fades (25 ms unless changed): drag the top corners
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                auto corner = [&] (bool left, double fadeSec)
                {
                    const float px = juce::jmax (0.0f, (float) (fadeSec * pixelsPerSecond));
                    const float xe = left ? (float) lane.getX() : (float) lane.getRight(), dir = left ? 1.0f : -1.0f;
                    juce::Path tri; tri.addTriangle (xe, (float) lane.getY(), xe + dir * 8.0f, (float) lane.getY(), xe, (float) lane.getY() + 8.0f);
                    g.fillPath (tri);
                    if (px >= 2.0f) g.drawLine (xe, (float) lane.getBottom() - 1.0f, xe + dir * px, (float) lane.getY() + 1.0f, 1.0f);
                };
                corner (true, grp.fadeInSeconds); corner (false, grp.fadeOutSeconds);
            }

            int firstRow = 1 << 20, lastRow = -1;
            for (auto& tf : grp.files) { const int row = rowOfTrack (tf.trackId); if (row >= 0) { firstRow = juce::jmin (firstRow, row); lastRow = juce::jmax (lastRow, row); } }
            for (auto& tf : grp.files)
            {
                const int row = rowOfTrack (tf.trackId);
                if (row < 0) continue;
                auto r = juce::Rectangle<int> (x0, top + row * rowH + (rowH > 24 ? 3 : 1), wpx, rowH - (rowH > 24 ? 6 : 2));
                if (r.getBottom() < clip.getY() || r.getY() > clip.getBottom()) continue;       // the row is off the screen
                const auto tc = rowColour[(size_t) row];
                g.setColour (live ? juce::Colour (0xff6a3434) : chan::clipFill (tc, picked));      // the track's own colour, darker when the take is selected
                g.fillRect (r);
                if (live) drawLiveWave (g, r.reduced (1), tf.trackId);
                else if (finishing) drawLiveWave (g, r.reduced (1), tf.trackId, grp.id);
                else
                {
                    paintprof::Scope profWave ("take window", "waveforms");
                    setWaveColour (g, chan::waveColour (tc));
                    juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (r.reduced (1));
                    auto* th = thumbnailFor (tf.file);
                    const double total = th != nullptr ? th->getTotalLength() : 0.0;
                    const auto wcd = app.waveColourOf (tf.file);
                    if (total > 0.0 && ! directWave.draw (g, r.reduced (1), tf.file, 0.0, total, app.waveZoom, wcd.get(), grp.sampleRate))
                        drawThumbnailEnvelope (g, r.reduced (1), *th, 0.0, total, app.waveZoom, wcd.get());
                }
                if (! live)                                                                          // parts sent out with "Export for Processing": no waveform, a hatched block
                    for (auto& wp : grp.waiting)
                        if (wp.active() && grp.sampleRate > 0 && (wp.tracks.empty() || std::find (wp.tracks.begin(), wp.tracks.end(), tf.trackId) != wp.tracks.end()))
                        {
                            const int wx0 = x0 + (int) ((double) wp.from / grp.sampleRate * pixelsPerSecond), wx1 = x0 + (int) ((double) wp.to / grp.sampleRate * pixelsPerSecond);
                            auto wr = juce::Rectangle<int> (wx0, r.getY(), juce::jmax (3, wx1 - wx0), r.getHeight()).getIntersection (r);
                            g.setColour (theme::window); g.fillRect (wr.reduced (0, 1));
                            drawWaitingBlock (g, wr.reduced (1), wp.name, theme::text);
                        }
                g.setColour (live ? theme::text.withAlpha (0.7f) : tc.withAlpha (picked ? 1.0f : 0.85f));
                g.drawRect (r, picked ? 2 : 1);
                // "DUD   LOST 0.3 s   Bar 17 - 39" along the bottom of the take (DUD in red; LOST: a disk overrun while it was recorded, that much silence is in
                // the files; CUT SHORT: a file stopped taking data, the drive was full or gone)
                const juce::String lost = grp.writeFailed ? juce::String ("CUT SHORT") : grp.lostSamples > 0 ? "LOST " + lostText (grp.lostSamples, grp.sampleRate) : juce::String();
                if ((grp.barIn > 0 || grp.dud || lost.isNotEmpty() || grp.roomTone) && r.getWidth() > 50 && r.getHeight() >= 22)
                {
                    auto bars = barsLabel (grp.barIn, grp.barOut);
                    if (grp.roomTone) bars = (grp.roomToneIn >= 0.0 ? "ROOM TONE " + juce::String (grp.roomToneIn, 1) + " - " + juce::String (grp.roomToneOut, 1) + " s" : juce::String ("ROOM TONE")) + (bars.isNotEmpty() ? "   " + bars : juce::String());
                    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
                    const auto font = g.getCurrentFont();
                    const int dudW = grp.dud ? juce::GlyphArrangement::getStringWidthInt (font, "DUD") + ((bars.isNotEmpty() || lost.isNotEmpty()) ? 10 : 0) : 0;
                    const int lostW = lost.isNotEmpty() ? juce::GlyphArrangement::getStringWidthInt (font, lost) + (bars.isNotEmpty() ? 10 : 0) : 0;
                    const int barsW = bars.isNotEmpty() ? juce::GlyphArrangement::getStringWidthInt (font, bars) : 0;
                    const int tw = juce::jmin (r.getWidth() - 4, dudW + lostW + barsW + 10);
                    auto strip = juce::Rectangle<int> (r.getX() + 2, r.getBottom() - 16, tw, 14);
                    g.setColour (juce::Colour (0xcc000000)); g.fillRect (strip);
                    auto t = strip.reduced (5, 0);
                    if (grp.dud) { g.setColour (juce::Colour (0xffff3b3b)); g.drawText ("DUD", t.removeFromLeft (dudW), juce::Justification::centredLeft); }
                    if (lost.isNotEmpty()) { g.setColour (grp.writeFailed ? juce::Colour (0xffff3b3b) : juce::Colour (0xffffa030)); g.drawText (lost, t.removeFromLeft (lostW), juce::Justification::centredLeft); }
                    if (bars.isNotEmpty()) { g.setColour (juce::Colour (0xff3ee0c0)); g.drawText (bars, t, juce::Justification::centredLeft); }
                }
            }

            if (lastRow < 0) continue;
            const int y0 = top + firstRow * rowH, y1 = top + (lastRow + 1) * rowH;
            if (barHighlight > 0 && barsContain (grp.barIn, grp.barOut, barHighlight))           // "Search for bar": this take contains the bar
            {
                g.setColour (juce::Colour (0xff19d3b5)); g.drawRect (juce::Rectangle<int> (x0, y0, wpx, y1 - y0).expanded (3), 3);
                g.setColour (juce::Colour (0xff1e8fff)); g.drawRect (juce::Rectangle<int> (x0, y0, wpx, y1 - y0).expanded (1), 1);
            }

            // edit marks (keys 1 and 2)
            if (w->editTake == grp.id)
            {
                const juce::Colour cin (0xff1e6fd9), cout (0xffe08a00);
                if (w->editIn >= 0 && w->editOut > w->editIn)
                {
                    g.setColour (juce::Colour (0x2a3b8cff));
                    const int mx = x0 + (int) (w->editIn * pixelsPerSecond), mw = (int) ((w->editOut - w->editIn) * pixelsPerSecond);
                    if (w->editTracks.empty()) g.fillRect (juce::Rectangle<int> (mx, y0, mw, y1 - y0));
                    else for (auto& tf : grp.files) { const int row = rowOfTrack (tf.trackId); if (row >= 0 && w->editUses (tf.trackId)) g.fillRect (juce::Rectangle<int> (mx, kRulerH + kLaneH + row * rowH, mw, rowH)); }
                }
                auto flag = [&] (double sec, juce::Colour c, const char* t, bool left)
                {
                    const int x = x0 + (int) (sec * pixelsPerSecond);
                    g.setColour (c); g.fillRect (x - 1, kRulerH, 2, y1 - kRulerH);
                    const int fw = 48; juce::Rectangle<int> f (left ? x : x - fw, kRulerH, fw, 13);
                    g.fillRect (f); g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                    g.drawText (t, f, juce::Justification::centred);
                };
                if (w->editIn >= 0)  flag (w->editIn, cin, "Mark 1", true);
                if (w->editOut >= 0) flag (w->editOut, cout, "Mark 2", false);
            }
            // where this take has been used: "In 3" where a piece of it starts in the edit, "Out 4" where it ends (drawn after all the takes, below)
            if (auto* usedEdit = app.project.findEdit (w->targetEdit))
                for (size_t ri = 0; ri < usedEdit->regions.size(); ++ri)
                {
                    const auto& rg = usedEdit->regions[ri];
                    if (rg.takeId != grp.id || rg.sampleRate <= 0) continue;
                    useFlags.push_back ({ x0 + (int) ((double) rg.srcIn / rg.sampleRate * pixelsPerSecond), y0, y1, true, usedEdit->editNumberAt ((int) ri), usedEdit->isSnipJoin ((int) ri) });     // the same numbers as the edit window shows
                    useFlags.push_back ({ x0 + (int) ((double) rg.srcOut / rg.sampleRate * pixelsPerSecond), y0, y1, false, usedEdit->editNumberAt ((int) ri + 1), usedEdit->isSnipJoin ((int) ri + 1) });
                }
            // playhead
            if (shownPhX != kNoLine && playhead >= 0 && app.playInfo.kind != AppContext::PlayInfo::Kind::Edit && ! app.playInfo.windowTimeline && app.playInfo.windowId == windowId && app.playInfo.id == grp.id)
                theme::paintPlayhead (g, shownPhX, kRulerH, y1 - kRulerH);    // (where movePlayhead last put it)
            if (live)                                           // while recording the playhead is the end of the growing take
            {
                g.setColour (juce::Colour (0xffd32f2f));
                g.fillRect (x0 + (int) (len * pixelsPerSecond), kRulerH, 2, y1 - kRulerH);
            }
        }

        profGrid.next ("flags and names");
        // The In / Out flags of every take: all the Out flags first and the In flags over them, so an In flag is never hidden under the Out flag of the
        // piece before it. A join with no edit number ("fx", only a crossfade) gets a small grey tag, "I fx" / "O fx", just big enough for its text.
        for (int pass = 0; pass < 2; ++pass)
            for (auto& u : useFlags)
            {
                if (u.isIn != (pass == 1)) continue;
                if (u.number > 0)
                {
                    const auto c = u.isIn ? juce::Colour (0xff1a9c8a) : juce::Colour (0xff8a5cd0);
                    g.setColour (c.withAlpha (0.55f)); g.fillRect (u.x, u.y0, 1, u.y1 - u.y0);
                    const int fw = 38; juce::Rectangle<int> f (u.isIn ? u.x : u.x - fw, kRulerH + kLaneH - 13, fw, 11);       // its own row at the bottom of the strip, clear of mark 1 / 2s
                    g.setColour (c); g.fillRect (f);
                    g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                    g.drawText ((u.isIn ? "In " : "Out ") + juce::String (u.number), f, juce::Justification::centred);
                }
                else
                {
                    const juce::Colour grey (0xff7d848c);
                    g.setColour (grey.withAlpha (0.45f)); g.fillRect (u.x, u.y0, 1, u.y1 - u.y0);
                    const juce::String t (u.snip ? (u.isIn ? "I sn" : "O sn") : (u.isIn ? "I fx" : "O fx"));
                    const juce::FontOptions fo (8.5f, juce::Font::bold);
                    const int fw = juce::GlyphArrangement::getStringWidthInt (fo, t) + 4;
                    juce::Rectangle<int> f (u.isIn ? u.x : u.x - fw, kRulerH + kLaneH - 12, fw, 10);
                    g.setColour (grey); g.fillRect (f);
                    g.setColour (juce::Colours::white); g.setFont (fo);
                    g.drawText (t, f, juce::Justification::centred, false);
                }
            }

        if (shownPhX != kNoLine && playhead >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowTimeline && app.playInfo.windowId == windowId)
            theme::paintPlayhead (g, shownPhX, kRulerH, getHeight() - kRulerH);    // the whole window is playing: one moving line across every row
        {   // the playhead: set by clicking the ruler; Space plays from here, IN / OUT are marked here
            const int px = xOf (w->playheadSeconds);
            g.setColour (theme::text); g.fillRect (px, kRulerH, 1, getHeight() - kRulerH);
            juce::Path tri; tri.addTriangle ((float) px - 6.0f, (float) kRulerH - 12.0f, (float) px + 7.0f, (float) kRulerH - 12.0f, (float) px + 0.5f, (float) kRulerH - 1.0f);
            g.fillPath (tri);
        }

        if (w->showMarkers) markerui::paintFlags (g, w->markers, [this] (double s) { return xOf (s); }, viewport.getViewPositionX() + kNameW, getHeight(), markers.draggedId());
        const int sx = viewport.getViewPositionX();
        for (size_t i = 0; i < tracks.size() && sx < clip.getRight() && sx + kNameW > clip.getX(); ++i)
        {
            auto r = juce::Rectangle<int> (sx, top + (int) i * rowH, kNameW, rowH);
            if (r.getBottom() < clip.getY() || r.getY() > clip.getBottom()) continue;
            const auto tc = rowColour[(size_t) i];
            g.setColour (theme::panel); g.fillRect (r);
            g.setColour (tc.withAlpha (0.20f)); g.fillRect (r);                              // the name plate carries the track's colour
            g.setColour (tc); g.fillRect (r.removeFromLeft (6));
            g.setColour (tc.darker (0.3f).withAlpha (0.8f)); g.fillRect (sx, top + (int) i * rowH + rowH - 1, kNameW, 1);
            if (trackSelected (tracks[i]->id))                                               // a selected name (right-click: remove the selected tracks)
            {
                const auto sel = juce::Rectangle<int> (sx, top + (int) i * rowH, kNameW, rowH);
                g.setColour (juce::Colour (0xff29b6ff).withAlpha (0.22f)); g.fillRect (sel);
                g.setColour (juce::Colour (0xff29b6ff)); g.drawRect (sel.reduced (1), 2);
            }
            g.setColour (theme::text); g.setFont (juce::jlimit (9.0f, 14.0f, (float) rowH - 4.0f));
            const bool tall = rowH >= 44;
            g.drawText (tracks[i]->name, tall ? r.reduced (8, 0).withTrimmedRight (36).withTrimmedTop (22).withTrimmedBottom (14) : r.withTrimmedLeft (46).withTrimmedRight (38), juce::Justification::centredLeft, true);
            if (auto* st = stripOf (tracks[i]->id))       // M / S at the top left, above the name (the same flags as the mixer strip)
            {
                auto drawBtn = [&] (juce::Rectangle<int> br, const char* t, bool on, juce::Colour c)
                {
                    g.setColour (on ? c : juce::Colour (0xff2a2f36)); g.fillRoundedRectangle (br.toFloat(), 3.0f);
                    g.setColour (theme::border); g.drawRoundedRectangle (br.toFloat(), 3.0f, 1.0f);
                    g.setColour (on ? juce::Colours::black : theme::text); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                    g.drawText (t, br, juce::Justification::centred, false);
                };
                drawBtn (muteRect (sx, top + (int) i * rowH), "M", st->mute.get(), juce::Colour (0xffe05a3a));
                drawBtn (soloRect (sx, top + (int) i * rowH), "S", st->solo.get(), juce::Colour (0xfff2c200));
            }
            g.setColour (theme::dimText); g.setFont (11.0f);
            if (tall)
            g.drawText (tracks[i]->format == TrackFormat::Mono ? "mono" : tracks[i]->format == TrackFormat::Stereo ? "stereo" : juce::String (tracks[i]->channelCount()) + "-ch",
                        r.reduced (8, 0).withTrimmedTop (30), juce::Justification::centredLeft);
            {   // little horizontal level meters of the signal arriving on this track's inputs: one bar per channel (a stereo track shows left over right)
                const bool big = rowH >= 44;
                const int nch = juce::jlimit (1, kMaxMeterCh, tracks[i]->channelCount());
                const float totalH = big ? 9.0f : 6.0f, gap = 1.0f, barH = (totalH - gap * (float) (nch - 1)) / (float) nch;
                const float y0 = (float) (top + (int) i * rowH + rowH) - totalH - 2.0f;
                const auto& tm = meterOf (tracks[i]->id);
                for (int c = 0; c < nch; ++c)
                {
                    const juce::Rectangle<float> mr ((float) sx + 14.0f, y0 + (float) c * (barH + gap), (float) kNameW - 14.0f - 44.0f, barH);
                    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRect (mr);
                    const float lv = tm.level[(size_t) c], hv = tm.hold[(size_t) c];
                    const float db = juce::Decibels::gainToDecibels (lv, -80.0f);
                    const float fo = LevelMeter::frac (LevelMeter::kOrangeDb), fr = LevelMeter::frac (LevelMeter::kRedDb), f = LevelMeter::frac (db);
                    auto seg = [&] (float a, float b, juce::Colour col) { b = juce::jmin (b, f); if (b > a) { g.setColour (col); g.fillRect (mr.getX() + mr.getWidth() * a, mr.getY(), mr.getWidth() * (b - a), mr.getHeight()); } };
                    seg (0.0f, fo, juce::Colour (0xff3fbf6a)); seg (fo, fr, juce::Colour (0xffe0a800)); seg (fr, 1.0f, juce::Colour (0xffe53935));
                    const float hf = LevelMeter::frac (juce::Decibels::gainToDecibels (hv, -80.0f));
                    if (hf > 0.0f) { g.setColour (juce::Colours::white.withAlpha (0.8f)); g.fillRect (mr.getX() + mr.getWidth() * hf - 1.0f, mr.getY(), 1.5f, mr.getHeight()); }
                }
            }
            {   // the ARM switch of this track: lit red = it records
                const auto ar = armRect (top + (int) i * rowH).toFloat();
                const bool on = tracks[i]->isArmed();
                g.setColour (on ? juce::Colour (0xffd32f2f) : theme::field); g.fillRoundedRectangle (ar, 3.0f);
                g.setColour (on ? juce::Colour (0xffff8a80) : theme::border); g.drawRoundedRectangle (ar.reduced (0.5f), 3.0f, 1.0f);
                g.setColour (on ? juce::Colours::white : theme::dimText); g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.drawText ("ARM", ar.toNearestInt(), juce::Justification::centred);
            }
        }
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, rowsBottom, kNameW, juce::jmax (0, getHeight() - rowsBottom));     // the name column carries on to the bottom
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, 0, kNameW, top);
        if (shownMods != 0 && (headerPass || viewport.getViewPositionY() == 0)) modhint::paint (g, { sx, 0, kNameW, top }, shownMods, modifierHints (shownMods));
    }

    void mouseDownImpl (const juce::MouseEvent& e)
    {
        dragId = {}; dragging = false; markDragTake = {}; markActive = false; lassoActive = lassoMoved = false; moveActive = moveMoved = false;
        keepAndCloseBarsPopup();                                       // (a click elsewhere keeps what was typed; Esc is the way to throw it away)
        grabKeyboardFocus();
        auto* w = def();
        if (w == nullptr) return;
        markers.setNameWidth (viewport.getViewPositionX() + kNameW);
        markers.yOffset = hdrShift;
        if (w->showMarkers && markers.mouseDown (e)) return;                                 // the marker strip along the top: right-click for Add Marker / Clear All; drag a marker
        flagDrag = 0;
        if (! e.mods.isPopupMenu() && (flagDrag = editFlagAt (*w, e.getPosition())) != 0) return;       // grab an marks 1 / 2 flag to move it
        if (e.mods.isPopupMenu() && e.x >= kNameW)                         // right-click on a part waiting for corrected audio: re-link it
            if (auto* wg = groupAt (e.getPosition()))
                if (wg->sampleRate > 0)
                {
                    const auto smp = (juce::int64) std::llround ((double) (e.x - xOf (wg->startSeconds)) / pixelsPerSecond * wg->sampleRate);
                    for (size_t wi = 0; wi < wg->waiting.size(); ++wi)
                        if (wg->waiting[wi].active() && smp >= wg->waiting[wi].from && smp < wg->waiting[wi].to && e.y >= kRulerH + kLaneH)
                        {
                            const auto gid = wg->id; const auto wname = wg->waiting[wi].name; const auto wfrom = wg->waiting[wi].from;
                            juce::PopupMenu pm;
                            pm.addSectionHeader ("Waiting for corrected audio: " + wname);
                            pm.addItem (1, "Re-link corrected audio");
                            pm.addItem (2, "Stop waiting: keep the original audio");
                            const auto wid = windowId;
                            juce::Component::SafePointer<juce::Component> self (this);
                            pm.showMenuAsync (juce::PopupMenu::Options(), [this, self, gid, wid, wfrom] (int r)
                            {
                                if (self == nullptr) return;
                                if (r == 1) fixtools::relinkTake (app, wid, gid, wfrom, self.getComponent());
                                else if (r == 2)
                                {
                                    if (auto* tw = app.project.findTakeWindow (wid)) if (auto* gg = tw->findGroup (gid))
                                    { gg->waiting.erase (std::remove_if (gg->waiting.begin(), gg->waiting.end(), [wfrom] (const WaitingPiece& x) { return x.from == wfrom; }), gg->waiting.end()); app.project.changed(); repaint(); }
                                }
                            });
                            return;
                        }
                }
        if (e.mods.isPopupMenu() && e.x >= kNameW)                         // right-click on a take, even one still being recorded: its bars
            if (auto* bg = groupAt (e.getPosition(), true))
            {
                if (e.y < kRulerH + kLaneH) { showBars (bg->id, e.getPosition()); return; }          // the banner: the bars it starts and ends in
                const auto shown = shownTracks();                                                       // the audio: a menu with that and Re-draw waveform
                const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
                const juce::Uuid tid = juce::isPositiveAndBelow (row, (int) shown.size()) ? shown[(size_t) row]->id : juce::Uuid::null();
                std::vector<juce::File> files;
                for (auto& f : bg->files) if (f.trackId == tid) files.push_back (f.file);
                if (files.empty()) for (auto& f : bg->files) files.push_back (f.file);
                juce::PopupMenu pm;
                pm.addItem (1, "Re-draw waveform");
                pm.addItem (2, "Bars it starts and ends in, and notes...");
                pm.addSeparator();
                // the room tone: the take (or the part of it between marks 1 and 2) that holds nothing but the room, for the denoiser (one take per take window)
                const bool marksHere = w->editTake == bg->id && w->editIn >= 0.0 && w->editOut > w->editIn;
                pm.addItem (3, bg->roomTone && bg->roomToneIn < 0.0 ? "This whole take is the room tone  (marked)" : "This whole take is the room tone", true, bg->roomTone && bg->roomToneIn < 0.0);
                pm.addItem (4, "Marks 1 - 2 of this take are the room tone" + (marksHere ? "  (" + juce::String (w->editIn, 1) + " - " + juce::String (w->editOut, 1) + " s)" : juce::String ("  (set marks 1 / 2 in it first)")), marksHere, bg->roomTone && bg->roomToneIn >= 0.0);
                if (bg->roomTone) pm.addItem (5, "Not the room tone");
                // the edit window's visual guide (2.102): this track's clip - marks 1 - 2 of it when they are set in this take
                bool hasClip = false; juce::String clipTrack;
                for (auto& f : bg->files) if (f.trackId == tid) { hasClip = true; clipTrack = f.trackName; }
                if (auto* t = app.project.findTrack (tid)) clipTrack = t->name;
                pm.addSeparator();
                pm.addItem (6, "Use as edit window visual guide track  (" + (hasClip ? clipTrack + (marksHere ? ", marks 1 - 2" : ", the whole take") : juce::String ("right-click a track's clip")) + ")", hasClip);
                const auto gid = bg->id; const auto pos = e.getPosition();
                juce::Component::SafePointer<juce::Component> self (this);
                pm.showMenuAsync (juce::PopupMenu::Options(), [this, self, files, gid, pos, tid] (int r)
                {
                    if (self == nullptr) return;
                    if (r == 1) app.redrawWaveforms (files);
                    else if (r == 2) showBars (gid, pos);
                    else if (r >= 3 && r <= 5) markRoomTone (gid, r);
                    else if (r == 6)
                    {
                        juce::Uuid eid;
                        const auto err = app.setEditGuide (windowId, gid, tid, &eid);
                        if (err.isNotEmpty()) { showError ("Visual guide", err); return; }
                        if (app.showEditBehind) app.showEditBehind (eid, this);
                        juce::String where = "the edit";
                        if (auto* ed = app.project.findEdit (eid)) where = "\"" + ed->name + "\"";
                        app.setNotice ("The guide is in " + where + ", above its tracks: drag it sideways to line it up; its S plays it on its own.");
                    }
                });
                return;
            }
        const bool inLane = e.y >= kRulerH && e.y < kRulerH + kLaneH;
        const int sx = viewport.getViewPositionX();
        if (! e.mods.isPopupMenu() && std::abs (e.x - (sx + kNameW)) <= 4 && e.y >= kRulerH) { widthDrag = true; return; }          // the right edge of the name column: drag to resize
        if (! e.mods.isPopupMenu() && e.x >= sx && e.x < sx + kNameW && e.y >= kRulerH + kLaneH)    // click on a track's M / S switch
        {
            const auto shown = shownTracks();
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) shown.size()))
                if (auto* st = stripOf (shown[(size_t) row]->id))
                {
                    const int rowTop = kRulerH + kLaneH + row * rowH;
                    if (muteRect (sx, rowTop).expanded (1).contains (e.getPosition())) { st->mute.set (! st->mute.get()); app.project.markDirty(); repaint(); return; }
                    if (soloRect (sx, rowTop).expanded (1).contains (e.getPosition())) { st->solo.set (! st->solo.get()); app.project.markDirty(); repaint(); return; }
                }
        }
        if (! e.mods.isPopupMenu() && e.x >= sx && e.x < sx + kNameW && e.y >= kRulerH + kLaneH)    // click on a track's ARM switch
        {
            const auto shown = shownTracks();
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) shown.size()) && armRect (kRulerH + kLaneH + row * rowH).contains (e.getPosition()))
            {
                if (! app.engine.isRecording())
                {
                    for (auto& t : app.project.tracks) if (t.id == shown[(size_t) row]->id) t.setArmed (! t.isArmed());
                    app.project.changed();
                }
                return;
            }
        }
        if (! e.mods.isPopupMenu() && ! e.mods.isAltDown() && e.x >= sx && e.x < sx + kNameW && e.y >= kRulerH + kLaneH)   // a click on a track's name: select it
        {
            const auto shown = shownTracks();
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) shown.size())) clickTrackName (shown[(size_t) row]->id, e.mods);
            return;
        }
        if (e.mods.isPopupMenu() && e.x >= sx && e.x < sx + kNameW && e.y >= kRulerH + kLaneH)      // right-click on a track name
        {
            const auto shown = shownTracks();
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) shown.size())) showTrackMenu (shown[(size_t) row]->id, shown[(size_t) row]->name);
            return;
        }
        if (e.y < kRulerH && e.x >= kNameW && ! e.mods.isPopupMenu())      // the ruler places the playhead (and nothing else does)
        {
            scrubbing = true; scrubMoved = false;
            setPlayheadFromMouse (*w, e.x);
            seekIfPlaying (*w);                                          // playing: carry on from the spot that was clicked
            return;
        }
        if (! e.mods.isPopupMenu())                                        // the top corners of a take: drag the left one right for a longer fade-in, the right one left for a longer fade-out
        {
            bool isIn = true;
            if (auto* fg = fadeHandleAt (*w, e.getPosition(), isIn))
            {
                if (app.isPlaying()) app.stopPlayback();
                fadeDrag = isIn ? 1 : 2; fadeTake = fg->id; fadeBase = isIn ? fg->fadeInSeconds : fg->fadeOutSeconds;
                if (! (selectedTakes.size() == 1 && selectedTakes[0] == fg->id)) selectedTakes.assign (1, fg->id);
                repaint(); return;
            }
        }
        const bool ctrlDown = e.mods.isCommandDown() || e.mods.isCtrlDown();
        if (! e.mods.isPopupMenu() && e.mods.isAltDown() && ! ctrlDown && e.y >= kRulerH + kLaneH && e.x >= sx + kNameW)    // Alt + drag: a lasso
        {
            lassoActive = true; lassoMoved = false; lassoFrom = lassoTo = e.getPosition();
            return;
        }
        if (! inLane && e.getNumberOfClicks() > 1 && e.x >= sx + kNameW && ! e.mods.isPopupMenu())       // double-click on a take: straight into its bars and notes -
            if (auto* any = groupAt (e.getPosition(), true)) { showBars (any->id, e.getPosition()); return; }   // also the take being recorded (as the right-click)
        if (auto* grp = groupAt (e.getPosition(), e.mods.isPopupMenu()))
        {
            if (inLane && e.getNumberOfClicks() > 1) { showSettings (grp->id, false); return; }
            if (ctrlDown && e.mods.isShiftDown() && ! e.mods.isAltDown() && e.y >= kRulerH + kLaneH)    // Ctrl + Shift + drag: its clips onto another track
            {
                const auto shown = shownTracks();
                const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
                if (juce::isPositiveAndBelow (row, (int) shown.size()))
                {
                    moveFrom = shown[(size_t) row]->id; moveRow0 = moveRow = row; moveDownY = e.y; moveMouse = e.getPosition();
                    moveClickTake = grp->id; moveLabelArea = {};
                    moveTakes.clear();
                    if (isTakeSelected (grp->id))
                    {
                        for (auto& id : selectedTakes)
                            if (auto* sg = w->findGroup (id))
                                for (auto& f : sg->files) if (f.trackId == moveFrom) { moveTakes.push_back (id); break; }
                    }
                    else moveTakes.push_back (grp->id);
                    moveActive = true; moveMoved = false;
                    beginDragAutoRepeat (40);                                // (so the window keeps scrolling while the mouse is held at the top or bottom)
                    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
                    return;
                }
            }
            if (ctrlDown && ! e.mods.isAltDown())                          // Ctrl + click: add this take to the selection, or take it out again
            {
                auto it = std::find (selectedTakes.begin(), selectedTakes.end(), grp->id);
                if (it != selectedTakes.end()) selectedTakes.erase (it); else selectedTakes.push_back (grp->id);
                repaint();
                return;
            }
            if (! (selectedTakes.size() == 1 && selectedTakes[0] == grp->id)) { selectedTakes.assign (1, grp->id); repaint(); }
            {   // show where this take's file is in the Media window (the file of the track row that was clicked, else the first one)
                const auto shown = shownTracks();
                const int row = e.y >= kRulerH + kLaneH ? (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH) : -1;
                const juce::Uuid tid = juce::isPositiveAndBelow (row, (int) shown.size()) ? shown[(size_t) row]->id : juce::Uuid::null();
                const TakeFile* hit = nullptr;
                for (auto& f : grp->files) if (f.trackId == tid) hit = &f;
                if (hit == nullptr && ! grp->files.empty()) hit = &grp->files.front();
                if (hit != nullptr) app.revealInMedia (hit->file);
            }
            if (inLane)
            {
                // takes stay where they were recorded, in the order they were made: they cannot be slid left or right here
            }
            else if (e.y >= kRulerH + kLaneH && e.x >= kNameW && e.getNumberOfClicks() < 2 && ! (app.engine.isRecording() && grp->id == app.engine.currentTakeId()))
            {
                markDragTake = grp->id; markAnchor = juce::jmax (0.0, (double) (e.x - kNameW) / pixelsPerSecond - grp->startSeconds); markStartX = e.x;
                markAlt = e.mods.isAltDown() && ctrlDown; markRow0 = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);      // Ctrl + Alt + drag: only the track(s) the mouse covers
            }
            // clicking a take only selects it: the playhead stays where it is
        }
        else if (e.y >= kRulerH && e.x >= kNameW && ! (e.mods.isCommandDown() || e.mods.isCtrlDown()) && ! selectedTakes.empty())
        {
            selectedTakes.clear();                                         // a click on an empty patch deselects every take
            repaint();
        }
    }

    void mouseDragImpl (const juce::MouseEvent& e)
    {
        auto* w = def();
        if (w == nullptr) return;
        if (widthDrag) { kNameW = juce::jlimit (110, 700, e.x - viewport.getViewPositionX()); markers.setNameWidth (viewport.getViewPositionX() + kNameW); updateSize(); repaint(); return; }
        if (moveActive) { dragMove (e); return; }
        if (lassoActive)
        {
            const auto before = lassoMoved ? lassoRect() : juce::Rectangle<int>();
            lassoTo = e.getPosition();
            if (! lassoMoved && lassoTo.getDistanceFrom (lassoFrom) < 4) return;
            lassoMoved = true;
            auto picked = takesIn (lassoRect());
            if (picked != selectedTakes) { selectedTakes = std::move (picked); repaint(); }       // (the takes change colour: all of it)
            else repaint (before.getUnion (lassoRect()).expanded (2));
            return;
        }
        if (markers.mouseDrag (e)) return;
        if (fadeDrag != 0)
        {
            if (auto* fg = w->findGroup (fadeTake))
            {
                const double dx = e.getDistanceFromDragStartX() / pixelsPerSecond;
                const double len = juce::jlimit (0.0, fg->lengthSeconds() * 0.5, fadeBase + (fadeDrag == 1 ? dx : -dx));
                (fadeDrag == 1 ? fg->fadeInSeconds : fg->fadeOutSeconds) = len;
                app.project.markDirty(); repaint();
            }
            return;
        }
        if (flagDrag != 0)
        {
            if (auto* eg = w->findGroup (w->editTake))
            {
                double sec = juce::jlimit (0.0, eg->lengthSeconds(), (double) (e.x - xOf (eg->startSeconds)) / pixelsPerSecond);
                if (sec * pixelsPerSecond < 5.0) sec = 0.0;                                     // a flag snaps to the very start or end of the take
                if ((eg->lengthSeconds() - sec) * pixelsPerSecond < 5.0) sec = eg->lengthSeconds();
                if (flagDrag == 1) w->editIn  = w->editOut >= 0 ? juce::jmin (sec, w->editOut - 0.001) : sec;
                else               w->editOut = w->editIn  >= 0 ? juce::jmax (sec, w->editIn + 0.001)  : sec;
                app.project.markDirty(); repaint();
            }
            return;
        }
        if (! markDragTake.isNull())
        {
            if (auto* grp = w->findGroup (markDragTake))
            {
                if (! markActive && std::abs (e.x - markStartX) < 4) return;
                markActive = true;
                const double now = juce::jlimit (0.0, grp->lengthSeconds(), (double) (e.x - kNameW) / pixelsPerSecond - grp->startSeconds);
                w->editTake = grp->id; w->editIn = juce::jmin (markAnchor, now); w->editOut = juce::jmax (markAnchor, now);
                w->editTracks.clear();
                if (markAlt)
                {
                    const int r1 = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH), lo = juce::jmin (markRow0, r1), hi = juce::jmax (markRow0, r1);
                    for (auto& tf : grp->files) { const int row = trackRow (tf.trackId); if (row >= lo && row <= hi) w->editTracks.push_back (tf.trackId); }
                }
                app.project.markDirty(); repaint();
            }
            return;
        }
        if (dragging)
        {
            if (auto* grp = w->findGroup (dragId))
            {
                grp->startSeconds = juce::jmax (0.0, std::round ((dragOriginSeconds + e.getDistanceFromDragStartX() / pixelsPerSecond) * 10.0) / 10.0);
                repaint();
            }
        }
        else if (scrubbing)                                   // dragging along the ruler moves the playhead
        {
            setPlayheadFromMouse (*w, e.x);
            scrubMoved = true;
        }
    }

    void mouseMoveImpl (const juce::MouseEvent& e)
    {
        auto* w = def(); bool isIn = true;
        if (std::abs (e.x - (viewport.getViewPositionX() + kNameW)) <= 4 && e.y >= kRulerH) { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
        setMouseCursor (w != nullptr && (fadeHandleAt (*w, e.getPosition(), isIn) != nullptr || editFlagAt (*w, e.getPosition()) != 0) ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUpImpl (const juce::MouseEvent& e)
    {
        if (widthDrag) { widthDrag = false; setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); return; }
        if (moveActive)
        {
            moveActive = false;
            const bool moved = moveMoved; moveMoved = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint();
            if (! moved)                                                     // Ctrl + Shift + click: as Ctrl + click, the take in or out of the selection
            {
                auto it = std::find (selectedTakes.begin(), selectedTakes.end(), moveClickTake);
                if (it != selectedTakes.end()) selectedTakes.erase (it); else selectedTakes.push_back (moveClickTake);
                return;
            }
            const auto shown = shownTracks();
            if (moveRow != moveRow0 && juce::isPositiveAndBelow (moveRow, (int) shown.size())) moveClips (moveFrom, shown[(size_t) moveRow]->id, moveTakes);
            return;
        }
        if (lassoActive)
        {
            if (! lassoMoved) selectedTakes = takesIn (lassoRect());          // Alt + click (no drag): the take under the mouse, or none
            lassoActive = lassoMoved = false;
            repaint(); return;
        }
        if (markers.mouseUp (e)) return;
        if (markActive) { markActive = false; app.project.changed(); }
        if (flagDrag != 0) { flagDrag = 0; app.project.changed(); }
        markDragTake = {};
        if (fadeDrag != 0) { fadeDrag = 0; app.project.changed(); }
        if (dragging)
        {
            if (auto* w = def()) w->setPlayhead (w->playheadSeconds);      // the cursor follows the playhead, whichever take it is over now
            app.project.changed(); updateSize();
        }
        if (scrubbing && scrubMoved) if (auto* w = def()) seekIfPlaying (*w);       // after dragging along the ruler: carry on from where it was let go
        dragging = false; scrubbing = false; scrubMoved = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    /** Where marks 1 / 2 are set: the live position while recording or playing here, otherwise the playhead. Returns the take and the time inside it. */
    const TakeGroup* markPoint (double& secondsInTake) const
    {
        auto* w = const_cast<TakeTimeline*> (this)->def();
        if (w == nullptr) return nullptr;
        const double live = livePosition();
        const double at = live >= 0.0 ? live : w->playheadSeconds;
        const TakeGroup* g = nullptr;
        if (app.engine.isRecording() && live >= 0.0) g = w->findGroup (app.engine.currentTakeId());
        else if (live >= 0.0 && app.playInfo.windowId == windowId && ! app.playInfo.windowTimeline) g = w->findGroup (app.playInfo.id);
        else g = w->takeAt (at);
        if (g == nullptr) return nullptr;
        secondsInTake = juce::jmax (0.0, at - g->startSeconds);
        return g;
    }

    /** What the window's time readout shows: the live position (recording here, or playing here, moving smoothly), otherwise the playhead. */
    double clockSeconds() const
    {
        const double live = livePosition (true);
        if (live >= 0.0) return live;
        auto* w = const_cast<TakeTimeline*> (this)->def();
        return w != nullptr ? w->playheadSeconds : 0.0;
    }

private:
    TakeWindowDef* def() const { return app.project.findTakeWindow (windowId); }
    /** The ARM switch of the track row that starts at y (it stays at the left edge while the window is scrolled sideways). */
    bool widthDrag = false;
    juce::Rectangle<int> muteRect (int sx, int rowTop) const { return { sx + 10, rowTop + 3, 18, 16 }; }
    juce::Rectangle<int> soloRect (int sx, int rowTop) const { return { sx + 30, rowTop + 3, 18, 16 }; }
    StripState* stripOf (const juce::Uuid& trackId) const          // the processing mixer's strip: the take window plays through it
    {
        return app.project.mixers.empty() ? nullptr : app.project.mixers.front()->stripFor (trackId);
    }
    juce::Rectangle<int> armRect (int rowTop) const
    {
        const int sx = viewport.getViewPositionX();
        return { sx + kNameW - 36, rowTop + juce::jmax (2, (rowH - 18) / 2), 32, 18 };
    }
    int xOf (double seconds) const { return kNameW + (int) (seconds * pixelsPerSecond); }
    markerui::Controller markers { *this, kNameW,
        [this] () -> std::vector<Marker>* { auto* d = def(); return d != nullptr ? &d->markers : nullptr; },
        [this] (int x) { return juce::jmax (0.0, (double) (x - kNameW) / pixelsPerSecond); },
        [this] (double s) { return xOf (s); },
        [this] { app.project.markDirty(); } };

    bool scrubMoved = false;
    /** While a take of this window is playing, moving the playhead makes the music carry on from the new spot (no need to stop first). */
    void seekIfPlaying (TakeWindowDef& w)
    {
        if (! app.isPlaying() || app.engine.isRecording()) return;
        if (app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != windowId) return;
        const auto err = app.playTakeWindow (windowId, w.playheadSeconds);
        if (err.isNotEmpty()) showError ("Play", err);
    }

    void setPlayheadFromMouse (TakeWindowDef& w, int x)
    {
        w.setPlayhead (juce::jmax (0.0, (double) (x - kNameW) / pixelsPerSecond));
        repaint();
        app.project.changed();
    }

    /** Where the playhead is right now: the end of the growing take while recording, the moving position while playing here, otherwise where you put it.
        smooth: the playing position moves on between the audio blocks (for drawing). */
    double livePosition (bool smooth = false) const
    {
        auto* w = const_cast<TakeTimeline*> (this)->def();
        if (w == nullptr) return -1.0;
        if (app.engine.isRecording())
        {
            if (auto* g = w->findGroup (app.engine.currentTakeId())) return g->startSeconds + app.engine.recordedSeconds();
        }
        else
        {
            const double ph = smooth ? app.playheadSecondsSmooth() : app.playheadSeconds();
            if (ph >= 0.0 && app.playInfo.kind != AppContext::PlayInfo::Kind::Edit && app.playInfo.windowId == windowId)
            {
                if (app.playInfo.windowTimeline) return ph;
                if (auto* g = w->findGroup (app.playInfo.id)) return g->startSeconds + ph;
            }
        }
        return -1.0;
    }
    double anchorSeconds() const
    {
        const double live = livePosition();
        if (live >= 0.0) return live;
        auto* w = const_cast<TakeTimeline*> (this)->def();
        return w != nullptr ? w->playheadSeconds : 0.0;
    }

    int trackRow (const juce::Uuid& id) const
    {
        const auto shown = shownTracks();
        for (size_t i = 0; i < shown.size(); ++i) if (shown[i]->id == id) return (int) i;
        return -1;
    }

    TakeGroup* groupAt (juce::Point<int> p, bool includeLive = false)
    {
        auto* w = def();
        if (w == nullptr || p.x < kNameW) return nullptr;
        const int top = kRulerH + kLaneH;
        for (auto it = w->groups.rbegin(); it != w->groups.rend(); ++it)
        {
            const bool live = app.engine.isRecording() && it->id == app.engine.currentTakeId();
            if (live && ! includeLive) continue;             // no editing marks on a take that is still being recorded (but its bars can be entered)
            const int x0 = xOf (it->startSeconds);
            const int x1 = x0 + juce::jmax (4, (int) ((live ? app.engine.recordedSeconds() : it->lengthSeconds()) * pixelsPerSecond));
            if (p.x < x0 || p.x > x1) continue;
            if (p.y >= kRulerH && p.y < top) return &*it;
            for (auto& f : it->files)
            {
                const int row = trackRow (f.trackId);
                if (row >= 0 && p.y >= top + row * rowH && p.y < top + (row + 1) * rowH) return &*it;
            }
        }
        return nullptr;
    }

    /** The waveform of the take being recorded, drawn from the overview the audio thread keeps (updates as you record). */
    void drawLiveWave (juce::Graphics& g, juce::Rectangle<int> r, const juce::Uuid& trackId, const juce::Uuid& groupId = juce::Uuid::null())
    {
        const juce::int16* data = nullptr; int bins = 0, binSamples = 0, nc = 1;
        if (! app.engine.getLivePeaks (trackId, data, bins, binSamples, &nc, groupId) || bins <= 0 || nc <= 0 || app.engine.getSampleRate() <= 0) return;
        const double binSeconds = (double) binSamples / app.engine.getSampleRate();
        const float laneH = (float) r.getHeight() / (float) nc;
        juce::Graphics::ScopedSaveState clipState (g); g.reduceClipRegion (r);
        setWaveColour (g, juce::Colour (0xffffd6cc));
        for (int c = 0; c < nc; ++c)                      // one lane per channel, like the finished waveform
        {
            const float mid = (float) r.getY() + laneH * ((float) c + 0.5f), half = laneH * 0.5f - 1.0f;
            std::vector<float> tops, bots;
            for (int x = 0; x < r.getWidth(); ++x)
            {
                const int b0 = (int) ((double) x / pixelsPerSecond / binSeconds);
                if (b0 >= bins) break;
                const int b1 = juce::jmin (bins, juce::jmax (b0 + 1, (int) ((double) (x + 1) / pixelsPerSecond / binSeconds)));
                int lo = 0, hi = 0;
                for (int b = b0; b < b1; ++b)
                {
                    lo = juce::jmin (lo, (int) data[((size_t) b * (size_t) nc + (size_t) c) * 2]);
                    hi = juce::jmax (hi, (int) data[((size_t) b * (size_t) nc + (size_t) c) * 2 + 1]);
                }
                tops.push_back (mid - (float) hi / 32767.0f * half * app.waveZoom);
                bots.push_back (mid - (float) lo / 32767.0f * half * app.waveZoom);
            }
            fillEnvelope (g, (float) r.getX(), tops, bots, nullptr, laneH);
        }
    }

    // ---- moving clips from one track to another (Ctrl + Shift + drag, and the track name's menu)
    juce::Rectangle<int> moveStrip (int row) const { return { 0, kRulerH + kLaneH + row * rowH, getWidth(), rowH }; }
    /** What dropping now would do: "" and the line by the mouse, or why it cannot. */
    juce::String moveWhy (const std::vector<const TrackDef*>& shown)
    {
        if (moveRow == moveRow0 || ! juce::isPositiveAndBelow (moveRow, (int) shown.size())) return {};
        return app.project.canMoveTakeClips (windowId, moveFrom, shown[(size_t) moveRow]->id, moveTakes);
    }
    juce::String moveText (const std::vector<const TrackDef*>& shown, const juce::String& why) const
    {
        if (moveRow == moveRow0 || ! juce::isPositiveAndBelow (moveRow, (int) shown.size())) return "Drag up or down onto another track";
        if (why.isNotEmpty()) return why.upToFirstOccurrenceOf (" Nothing was moved.", false, false);
        const auto* from = app.project.findTrack (moveFrom);
        const int n = (int) moveTakes.size();
        return "Move " + juce::String (n) + (n == 1 ? " clip: " : " clips: ") + (from != nullptr ? from->name : juce::String()) + "  >  " + shown[(size_t) moveRow]->name;
    }
    juce::Rectangle<int> moveLabelRect (const juce::String& text) const
    {
        const int sx = viewport.getViewPositionX(), sy = viewport.getViewPositionY();
        const int w = juce::jmin (640, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f, juce::Font::bold), text) + 20), h = 24;
        const int x = juce::jlimit (sx + kNameW + 4, juce::jmax (sx + kNameW + 4, sx + viewport.getMaximumVisibleWidth() - w - 4), moveMouse.x + 16);
        const int y = juce::jlimit (sy + kRulerH + kLaneH + 2, juce::jmax (sy + kRulerH + kLaneH + 2, sy + viewport.getMaximumVisibleHeight() - h - 2), moveMouse.y - h - 8);
        return { x, y, w, h };
    }
    /** The mouse moved (or is held at the top or bottom of the rows: the window scrolls, and this comes again every 40 ms). */
    void dragMove (const juce::MouseEvent& e)
    {
        const auto shown = shownTracks();
        if (shown.empty()) return;
        const int sy = viewport.getViewPositionY(), visH = viewport.getMaximumVisibleHeight();
        const int rowsTop = sy + kRulerH + kLaneH, rowsBottom = sy + visH;           // the rows that can be seen (the band at the top covers the ones under it)
        const auto oldArea = moveStrip (moveRow).getUnion (moveLabelArea);
        const int y = juce::jlimit (rowsTop, juce::jmax (rowsTop, rowsBottom - 1), e.y);
        const int row = juce::jlimit (0, (int) shown.size() - 1, (y - kRulerH - kLaneH) / juce::jmax (1, rowH));
        const juce::Point<int> mouse (e.x, y);
        const bool wasMoved = moveMoved;
        if (! moveMoved && (std::abs (e.y - moveDownY) > 3 || row != moveRow0)) moveMoved = true;
        if (moveMoved && (! wasMoved || row != moveRow || mouse != moveMouse))       // (the auto-repeat calls this every 40 ms: only what changed is drawn again)
        {
            moveRow = row; moveMouse = mouse;
            moveLabelArea = moveLabelRect (moveText (shown, moveWhy (shown))).expanded (2);
            repaint (oldArea); repaint (moveStrip (moveRow).getUnion (moveLabelArea));
        }
        // held at the top or bottom of the rows: scroll that way, faster the further out (at most a row each time)
        int dy = 0;
        if (e.y < rowsTop + 10) dy = -juce::jlimit (4, juce::jmax (4, rowH), (rowsTop + 10 - e.y) / 2);
        else if (e.y > rowsBottom - 10) dy = juce::jlimit (4, juce::jmax (4, rowH), (e.y - (rowsBottom - 10)) / 2);
        if (dy != 0) viewport.setViewPosition (viewport.getViewPositionX(), juce::jlimit (0, juce::jmax (0, getHeight() - visH), sy + dy));
    }
    void paintMoveGhost (juce::Graphics& g)
    {
        auto* w = def();
        const auto shown = shownTracks();
        if (w == nullptr || ! juce::isPositiveAndBelow (moveRow, (int) shown.size())) return;
        const int top = kRulerH + kLaneH, sx = viewport.getViewPositionX(), sy = viewport.getViewPositionY();
        const auto why = moveWhy (shown);
        const auto col = moveRow == moveRow0 ? theme::dimText : why.isEmpty() ? juce::Colour (0xff29b6ff) : juce::Colour (0xffff5a52);
        const int y = top + moveRow * rowH;
        {
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (juce::Rectangle<int> (0, sy + top, getWidth(), juce::jmax (0, getHeight() - sy - top)));      // (not over the band at the top)
            for (auto& id : moveTakes)
                if (auto* grp = w->findGroup (id))
                {
                    const auto r = juce::Rectangle<int> (xOf (grp->startSeconds), y + 2, juce::jmax (4, (int) (grp->lengthSeconds() * pixelsPerSecond)), juce::jmax (4, rowH - 4));
                    g.setColour (col.withAlpha (0.28f)); g.fillRect (r);
                    g.setColour (col); g.drawRect (r, 2);
                }
            g.setColour (col.withAlpha (0.18f)); g.fillRect (juce::Rectangle<int> (sx, y, kNameW, rowH));
            g.setColour (col); g.drawRect (juce::Rectangle<int> (sx, y, kNameW, rowH), 2);
        }
        const auto text = moveText (shown, why);
        const auto lr = moveLabelRect (text);
        g.setColour (juce::Colour (0xee101418)); g.fillRoundedRectangle (lr.toFloat(), 4.0f);
        g.setColour (col); g.drawRoundedRectangle (lr.toFloat().reduced (0.5f), 4.0f, 1.0f);
        g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (text, lr.reduced (10, 0), juce::Justification::centredLeft, true);
    }
    /** Every take of this window that has a clip on this track. */
    std::vector<juce::Uuid> takesWithClipOn (const juce::Uuid& trackId)
    {
        std::vector<juce::Uuid> v;
        if (auto* w = def())
            for (auto& grp : w->groups)
                for (auto& f : grp.files) if (f.trackId == trackId) { v.push_back (grp.id); break; }
        return v;
    }
    /** The clips 'takes' have on 'from' go onto 'to' (Project::moveTakeClips): what it did on the status line, or why not in a message. */
    bool moveClips (const juce::Uuid& from, const juce::Uuid& to, const std::vector<juce::Uuid>& takes, bool quiet = false)
    {
        if (app.engine.isRecording()) { showError ("Move clips", "Stop recording first."); return false; }
        if (app.isPlaying()) app.stopPlayback();
        juce::String why; int pieces = 0;
        const int n = app.project.moveTakeClips (windowId, from, to, takes, why, &pieces);
        if (n == 0) { if (! quiet) showError ("Move clips", why); return false; }
        if (! quiet)
        {
            const auto* t = app.project.findTrack (to);
            app.setNotice ("Moved " + juce::String (n) + (n == 1 ? " clip onto " : " clips onto ") + (t != nullptr ? t->name : juce::String ("the track"))
                           + (pieces > 0 ? " (and " + juce::String (pieces) + (pieces == 1 ? " piece" : " pieces") + " of the edits made from them)" : juce::String()) + ". Ctrl + Z puts them back.");
        }
        updateSize(); repaint();
        return true;
    }
    /** The tracks of this window with clips whose name names another track ("VivienneSPOT1" -> "SPOT1", "Rorate Caeli SPOT4 19 18" -> "SPOT4"), each with that track. */
    std::vector<std::pair<juce::Uuid, juce::Uuid>> nameMatchPairs()
    {
        std::vector<std::pair<juce::Uuid, juce::Uuid>> v;
        for (auto* t : shownTracks())
            if (! t->isLtc && ! takesWithClipOn (t->id).empty())
                if (auto* m = app.project.trackNamedAtEndOf (t->id)) v.push_back ({ t->id, m->id });
        return v;
    }
    /** All of them at once (asked first, with the list): what could not be moved is listed afterwards. */
    void moveAllByName()
    {
        if (app.engine.isRecording()) { showError ("Move clips", "Stop recording first."); return; }
        const auto pairs = nameMatchPairs();
        if (pairs.empty()) return;
        auto nameOf = [this] (const juce::Uuid& id) { auto* t = app.project.findTrack (id); return t != nullptr ? t->name : juce::String ("?"); };
        // one line per track the clips go onto, in the project's order ("AB   <   Rorate Caeli AB 20 19 and 20 more": an import can make hundreds)
        std::map<juce::Uuid, juce::StringArray> from;
        for (auto& pr : pairs) from[pr.second].add (nameOf (pr.first));
        juce::StringArray lines;
        for (auto& t : app.project.tracks)
            if (auto it = from.find (t.id); it != from.end())
                lines.add (t.name + "   <   " + it->second[0] + (it->second.size() > 1 ? " and " + juce::String (it->second.size() - 1) + " more" : juce::String()));
        const auto list = lines.size() <= 30 ? lines.joinIntoString ("\n") : lines.joinIntoString ("\n", 0, 28) + "\n... and " + juce::String (lines.size() - 28) + " more";
        juce::Component::SafePointer<TakeTimeline> safe (this);
        confirmAsync ("Move clips by name", "Move the clips of " + juce::String ((int) pairs.size()) + " tracks onto the tracks named in their names? The takes stay where they "
                      "are in time (the pieces of edits made from them go along).\n\n" + list + "\n\nCtrl + Z puts them all back.", "Move them",
                      [safe, pairs, nameOf]
                      {
                          if (safe == nullptr) return;
                          if (safe->app.engine.isRecording()) { showError ("Move clips", "Stop recording first."); return; }
                          int done = 0; juce::StringArray notMoved; std::vector<juce::Uuid> emptied;
                          for (auto& pr : pairs)
                          {
                              const auto why = safe->app.project.canMoveTakeClips (safe->windowId, pr.first, pr.second, safe->takesWithClipOn (pr.first));
                              if (why.isNotEmpty()) { notMoved.add (nameOf (pr.first) + ": " + why.upToFirstOccurrenceOf (" Nothing was moved.", false, false)); continue; }
                              if (! safe->moveClips (pr.first, pr.second, safe->takesWithClipOn (pr.first), true)) continue;
                              ++done;
                              if (! safe->app.project.trackHasAudio (pr.first)) emptied.push_back (pr.first);     // (nothing left on it anywhere: it can go)
                          }
                          safe->app.setNotice ("Moved the clips of " + juce::String (done) + (done == 1 ? " track" : " tracks") + " onto the tracks named in their names. Ctrl + Z puts them back.");
                          const auto notMovedText = notMoved.isEmpty() ? juce::String()
                                                                       : "Moved " + juce::String (done) + " of " + juce::String ((int) pairs.size()) + ". Not moved:\n\n" + notMoved.joinIntoString ("\n");
                          // the tracks the clips came from, now empty: the import's wrongly named tracks, which can be deleted at once
                          if (! emptied.empty())
                              safe->askToDeleteTracks (emptied, "Move clips by name",
                                                       (notMovedText.isNotEmpty() ? notMovedText + "\n\n"
                                                                                  : "Moved the clips of " + juce::String (done) + (done == 1 ? " track" : " tracks") + " (Ctrl + Z puts them back).\n\n")
                                                       + (emptied.size() == 1 ? "The track they came from is empty now. " : "The tracks they came from are empty now. "),
                                                       "Keep them");
                          else if (notMovedText.isNotEmpty()) showError ("Move clips by name", notMovedText);
                      });
    }
    /** Asks, then deletes these tracks from the whole project - only ones with no audio on them anywhere (Project::removeEmptyTracks), so nothing is
        lost. 'lead' starts the question; 'noText' is the button that leaves them. */
    void askToDeleteTracks (const std::vector<juce::Uuid>& ids, const juce::String& title, const juce::String& lead, const juce::String& noText = "Cancel")
    {
        juce::StringArray names;
        for (auto& id : ids) if (auto* t = app.project.findTrack (id)) names.add (t->name);
        if (names.isEmpty()) return;
        const int n = names.size();
        const auto list = n <= 16 ? names.joinIntoString (", ") : names.joinIntoString (", ", 0, 14) + " and " + juce::String (n - 14) + " more";
        juce::Component::SafePointer<TakeTimeline> safe (this);
        confirmAsync (title, lead + (n == 1 ? "Delete the track '" + names[0] + "' from the project?" : "Delete these " + juce::String (n) + " tracks from the project?\n\n" + list)
                      + "\n\nThere is no audio on " + (n == 1 ? "it" : "them") + " in any take window or edit, so nothing is lost, and the audio files on the disk "
                      "are not touched. Deleting tracks cannot be undone with Ctrl + Z.",
                      n == 1 ? juce::String ("Delete the track") : "Delete " + juce::String (n) + " tracks",
                      [safe, ids]
                      {
                          if (safe == nullptr) return;
                          if (safe->app.engine.isRecording()) { showError ("Delete tracks", "Stop recording first."); return; }
                          const int gone = safe->app.project.removeEmptyTracks (ids);
                          if (safe == nullptr) return;
                          for (auto& id : ids) safe->selectedTracks.erase (std::remove (safe->selectedTracks.begin(), safe->selectedTracks.end(), id), safe->selectedTracks.end());
                          safe->app.setNotice ("Deleted " + juce::String (gone) + (gone == 1 ? " track" : " tracks") + " from the project.");
                          safe->updateSize(); safe->repaint();
                      }, noText);
    }
    /** Can this track be deleted from the project from here: not the LTC track, and no audio on it anywhere? */
    bool deletable (const juce::Uuid& trackId)
    {
        const auto* t = app.project.findTrack (trackId);
        return t != nullptr && ! t->isLtc && ! app.project.trackHasAudio (trackId);
    }

    /** Right-click on a track name: remove the track from this take window only. */
    void showTrackMenu (const juce::Uuid& trackId, const juce::String& trackName)
    {
        juce::PopupMenu m;
        const auto where = juce::Rectangle<int> (juce::Desktop::getMousePosition(), juce::Desktop::getMousePosition()).expanded (4);
        m.addItem ("Colour...", [this, trackId, where] { showColourPickerAt (app, trackId, where); });
        m.addSeparator();
        if (const auto withClips = takesWithClipOn (trackId); ! withClips.empty() && ! app.engine.isRecording())       // its clips onto another track
        {
            juce::Component::SafePointer<TakeTimeline> safe (this);
            const int n = (int) withClips.size();
            const juce::String clips = juce::String (n) + (n == 1 ? " clip" : " clips");
            if (const auto* match = app.project.trackNamedAtEndOf (trackId))
            {
                const auto mid = match->id;
                m.addItem ("Move its " + clips + " onto " + match->name, [safe, trackId, mid] { if (safe != nullptr) safe->moveClips (trackId, mid, safe->takesWithClipOn (trackId)); });
            }
            juce::PopupMenu to;
            for (auto* t : shownTracks())
                if (t->id != trackId && ! t->isLtc) { const auto tid = t->id; to.addItem (t->name, [safe, trackId, tid] { if (safe != nullptr) safe->moveClips (trackId, tid, safe->takesWithClipOn (trackId)); }); }
            m.addSubMenu ("Move its " + clips + " onto another track", to);
            if (const auto pairs = nameMatchPairs(); pairs.size() > 1)
                m.addItem ("Move the clips of all " + juce::String ((int) pairs.size()) + " tracks named like this onto the tracks named in their names...", [safe] { if (safe != nullptr) safe->moveAllByName(); });
            m.addSeparator();
        }
        if (trackSelected (trackId) && selectedTracks.size() > 1)
        {
            const auto ids = selectedTracks;
            const int n = (int) ids.size();
            m.addItem ("Remove the " + juce::String (n) + " selected tracks from this take window...", [this, ids, n]
            {
                if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
                juce::StringArray names;
                for (auto& id : ids) if (auto* t = app.project.findTrack (id)) names.add (t->name);
                confirmAsync ("Remove tracks from this take window",
                              "Remove these " + juce::String (n) + " tracks from this take window?\n\n" + names.joinIntoString (", ") + "\n\nTheir clips disappear from this window only. "
                              "Other take windows, the mixers, the edits and the audio files on the disk are not touched. Recording a track again into this window brings it back.",
                              "Remove " + juce::String (n) + " tracks", [this, ids]
                              {
                                  if (auto* w = def())
                                  {
                                      for (auto& id : ids) w->removeTrackClips (id);
                                      selectedTracks.clear(); app.project.changed(); updateSize(); repaint();
                                  }
                              });
            });
            if (! app.engine.isRecording() && std::all_of (ids.begin(), ids.end(), [this] (const juce::Uuid& id) { return deletable (id); }))      // (empty everywhere)
                m.addItem ("Delete the " + juce::String (n) + " selected tracks from the project...",
                           [safe = juce::Component::SafePointer<TakeTimeline> (this), ids] { if (safe != nullptr) safe->askToDeleteTracks (ids, "Delete tracks", {}); });
            m.addItem ("Select none", [this] { selectedTracks.clear(); repaint(); });
            m.addSeparator();
        }
        m.addItem ("Remove track '" + trackName + "' from this take window...", [this, trackId, trackName]
        {
            if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
            confirmAsync ("Remove track from this take window",
                          "Remove '" + trackName + "' from this take window?\n\nIts clips disappear from this window only. Other take windows, the mixers, "
                          "the edits and the audio files on the disk are not touched. Recording the track again into this window brings it back.",
                          "Remove", [this, trackId]
                          {
                              if (auto* w = def())
                              {
                                  w->removeTrackClips (trackId);
                                  selectedTracks.erase (std::remove (selectedTracks.begin(), selectedTracks.end(), trackId), selectedTracks.end());
                                  app.project.changed(); updateSize(); repaint();
                              }
                          });
        });
        if (! app.engine.isRecording() && deletable (trackId))          // nothing on it anywhere (an import's wrongly named track, once its clips are moved)
            m.addItem ("Delete track '" + trackName + "' from the project...",
                       [safe = juce::Component::SafePointer<TakeTimeline> (this), trackId] { if (safe != nullptr) safe->askToDeleteTracks ({ trackId }, "Delete track", {}); });
        m.showMenuAsync (juce::PopupMenu::Options());
    }

    /** D: the selected takes are marked as duds (nothing good in them), or the mark is taken off again when they all already are. */
    void toggleDud()
    {
        auto* w = def();
        if (w == nullptr) return;
        auto ids = selectedTakes;
        if (ids.empty() && app.engine.isRecording() && w->findGroup (app.engine.currentTakeId()) != nullptr) ids.push_back (app.engine.currentTakeId());     // while recording: the take being recorded
        if (ids.empty()) return;
        bool allDud = true;
        for (auto& id : ids) if (auto* g = w->findGroup (id)) if (! g->dud) allDud = false;
        for (auto& id : ids) if (auto* g = w->findGroup (id)) g->dud = ! allDud;
        app.project.changed(); repaint();
    }

    /** Right-click on a take (also one that is still being recorded): a small box with IN Bar and OUT Bar. It does not block anything, so Record / Stop still work;
        the cursor is already in the IN box. Enter in IN goes to OUT, Enter in OUT saves, Esc closes. "More..." leads to the rename / remove menu. */
    void showBars (const juce::Uuid& gid, juce::Point<int> where)
    {
        auto* w = def();
        if (w == nullptr) return;
        auto* g = w->findGroup (gid);
        auto* host = viewport.getParentComponent();
        if (g == nullptr || host == nullptr) return;
        keepAndCloseBarsPopup();                                       // (another take's box was open: what was typed there is kept)
        barsGroup = gid;
        juce::Component::SafePointer<TakeTimeline> safe (this);
        barsPopup.reset (new BarsPopup ("Take " + pad3 (g->number) + " - bars it starts and ends in, and notes", g->barIn, g->barOut, g->notes,
            [safe, gid] (int bin, int bout, const juce::String& notes)
            {
                if (safe == nullptr) return;
                if (bin <= 0) bin = bout = 0; else if (bout <= 0) bout = bin;
                if (bout < bin) { showError ("Bars", "The OUT Bar must not be before the IN Bar."); return; }
                safe->applyBars (gid, bin, bout);
                safe->applyNotes (gid, notes);
                safe->closeBarsPopup();
            },
            [safe, gid] { if (safe == nullptr) return; safe->closeBarsPopup(); safe->showSettings (gid, true); },
            [safe] { if (safe != nullptr) safe->closeBarsPopup(); }));
        host->addAndMakeVisible (barsPopup.get());
        auto pt = host->getLocalPoint (this, where);
        barsPopup->setTopLeftPosition (juce::jlimit (0, juce::jmax (0, host->getWidth() - barsPopup->getWidth()), pt.x),
                                       juce::jlimit (0, juce::jmax (0, host->getHeight() - barsPopup->getHeight()), pt.y));
        barsPopup->toFront (true);
        barsPopup->focusFirst();
    }
    /** The box closes because of a click elsewhere or another take's box: the notes typed are kept, and the bars when they make sense
        (a take's notes are often typed while it is recorded; losing them to a stray click was too easy). Esc still closes it without saving. */
    void keepAndCloseBarsPopup()
    {
        if (barsPopup == nullptr) return;
        int bin = 0, bout = 0; juce::String notes;
        if (barsPopup->typed (bin, bout, notes))
        {
            if (bin <= 0) bin = bout = 0; else if (bout <= 0) bout = bin;
            if (bout >= bin) applyBars (barsGroup, bin, bout);
            applyNotes (barsGroup, notes);
        }
        closeBarsPopup();
    }
    void closeBarsPopup()
    {
        if (barsPopup == nullptr) return;
        auto* raw = barsPopup.release();
        raw->setVisible (false);
        juce::MessageManager::callAsync ([raw] { delete raw; });     // not while one of its own callbacks is still running
        grabKeyboardFocus();
    }
    void applyBars (const juce::Uuid& gid, int bin, int bout)
    {
        auto* w2 = def(); if (w2 == nullptr) return;
        auto* g2 = w2->findGroup (gid); if (g2 == nullptr) return;
        g2->barIn = bin; g2->barOut = bout;
        for (auto& e : app.project.edits)
        {
            for (auto& reg : e->regions)  if (reg.takeId == gid) { reg.barIn = bin; reg.barOut = bout; }
            for (auto& reg : e->overdubs) if (reg.takeId == gid) { reg.barIn = bin; reg.barOut = bout; }
        }
        app.project.changed(); repaint();
    }
    /** The take's notes (empty clears them). */
    void applyNotes (const juce::Uuid& gid, const juce::String& notes)
    {
        auto* w2 = def(); if (w2 == nullptr) return;
        auto* g2 = w2->findGroup (gid); if (g2 == nullptr || g2->notes == notes) return;
        g2->notes = notes;
        app.project.changed(); repaint();
    }
    std::unique_ptr<BarsPopup> barsPopup;
    juce::Uuid barsGroup;                                              // the take the box is about

    /** 3: the whole take is the room tone, 4: marks 1 - 2 of it are, 5: it is not. Only one take of a window can be the room tone. */
    void markRoomTone (const juce::Uuid& gid, int how)
    {
        auto* w = def(); if (w == nullptr) return;
        auto* g = w->findGroup (gid); if (g == nullptr) return;
        if (how == 5) { g->roomTone = false; g->roomToneIn = g->roomToneOut = -1.0; }
        else
        {
            for (auto& o : w->groups) { o.roomTone = false; o.roomToneIn = o.roomToneOut = -1.0; }
            g->roomTone = true;
            if (how == 4 && w->editTake == gid && w->editIn >= 0.0 && w->editOut > w->editIn) { g->roomToneIn = w->editIn; g->roomToneOut = w->editOut; }
            else { g->roomToneIn = g->roomToneOut = -1.0; }
        }
        app.project.changed(); repaint();
    }

    void showSettings (const juce::Uuid& gid, bool asMenu)
    {
        juce::PopupMenu m;
        m.addItem (1, "Take settings (rename)...");
        m.addItem (2, "Remove take from this window (the files stay on disk, and any edit using it keeps working)");
        auto safe = juce::Component::SafePointer<TakeTimeline> (this);
        auto choose = [safe, gid] (int r) { if (safe != nullptr && r != 0) safe->handleMenu (r, gid); };
        if (asMenu) m.showMenuAsync (juce::PopupMenu::Options(), choose);
        else choose (1);
    }

    void handleMenu (int result, const juce::Uuid& gid)
    {
        auto* w = def();
        if (w == nullptr) return;
        auto* g = w->findGroup (gid);
        if (g == nullptr) return;
        if (result == 2)
        {
            if (w->cursorTake == gid) w->cursorTake = juce::Uuid::null();
            w->groups.erase (std::remove_if (w->groups.begin(), w->groups.end(), [&] (const TakeGroup& x) { return x.id == gid; }), w->groups.end());
            app.project.changed(); updateSize(); repaint();
            return;
        }
        auto* aw = new juce::AlertWindow ("Take settings", "Take " + pad3 (g->number) + " - leave the name empty to use the piece title \"" + w->name + "\".\n"
                                          "Every file of this take is renamed to match.", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("label", g->label, "Take name");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto safe = juce::Component::SafePointer<TakeTimeline> (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, gid] (int r)
        {
            if (r == 1 && safe != nullptr)
                if (auto* w2 = safe->def())
                {
                    if (! w2->setLabel (gid, aw->getTextEditorContents ("label")))
                        showError ("Rename", "Some files could not be renamed (a file with that name may already exist).");
                    if (auto* g2 = w2->findGroup (gid))
                        for (auto& e : safe->app.project.edits)
                            for (auto& reg : e->regions)
                                if (reg.takeId == gid) reg.takeName = w2->displayName (*g2);
                    safe->app.project.changed();
                }
            delete aw;
        }));
    }

    WaveThumb* thumbnailFor (const juce::File& f)
    {
        syncRedraw();
        const auto key = f.getFullPathName();
        auto it = thumbs.find (key);
        if (it == thumbs.end())
        {
            auto t = std::make_unique<WaveThumb> (formatManager, thumbPool);
            t->addChangeListener (this);
            if (wavestore::loadThumbnail (*t, wavesFolder(), f)) saver.markDone (key);      // saved in the Waveforms folder: no need to read the audio again
            else { t->load (f); saver.markPending (key); }
            it = thumbs.emplace (key, std::move (t)).first;
        }
        return it->second.get();
    }
    juce::File wavesFolder() const { return app.project.projectFolder().getChildFile ("Waveforms"); }
    wavestore::ThumbSaver saver;
    int redrawSeen = 0;
    /** 'Re-draw waveform' was used somewhere: forget the thumbnails and audio held for those files, so they are read again. */
    void syncRedraw()
    {
        if (app.redrawStamp == redrawSeen) return;
        for (auto& e : app.redrawLog) if (e.first > redrawSeen) { thumbs.erase (e.second); saver.forget (e.second); directWave.forget (e.second); }
        redrawSeen = app.redrawStamp;
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source != &app.project) { thumbsChanged = true; return; }      // a waveform was read a bit further: shown at the next tick (see timerCallback)
        BusyLabel busyLabel ("updating a take window after a change");
        const auto t0 = paintprof::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
        paintprof::Scope prof ("take timeline change", "save thumbnails");
        saver.saveFinished (thumbs, wavesFolder());
        pruneLeft = thumbs.size();                                         // look again, a few at a time, for waveforms whose audio file has gone
        prof.next ("update size"); updateSize();
        repaint();
        prof.finish();
        if (paintprof::enabled()) paintprof::paintDone ("take timeline change", juce::Time::getMillisecondCounterHiRes() - t0);
    }
    // Waveforms being read report every little step; before, each step redrew the whole window AND asked the disk about every audio file the
    // window had shown (thousands of questions, many times a second, after a big import). Now the window is redrawn at most about 6 times a second
    // while they come in, and the check for files that have gone is spread out, 16 files per tick, and only after the project has changed.
    bool thumbsChanged = false; juce::uint32 lastThumbRefresh = 0;
    size_t pruneLeft = 0; juce::String pruneAfter;
    void refreshThumbsAndPrune()
    {
        if (thumbsChanged && juce::Time::getMillisecondCounter() - lastThumbRefresh >= 150)
        {
            thumbsChanged = false; lastThumbRefresh = juce::Time::getMillisecondCounter();
            saver.saveFinished (thumbs, wavesFolder()); repaint();
        }
        for (int n = 0; n < 16 && pruneLeft > 0; ++n, --pruneLeft)
        {
            auto it = pruneAfter.isEmpty() ? thumbs.begin() : thumbs.upper_bound (pruneAfter);
            if (it == thumbs.end()) it = thumbs.begin();
            if (it == thumbs.end()) { pruneLeft = 0; break; }
            pruneAfter = it->first;
            if (! juce::File (it->first).existsAsFile()) { thumbs.erase (it); repaint(); }
        }
    }

    static constexpr int kMaxMeterCh = 8;
    struct TrackMeter { std::array<float, kMaxMeterCh> level {}, hold {}; std::array<juce::uint32, kMaxMeterCh> holdAt {}; };
    std::map<juce::String, TrackMeter> trackMeters;
    TrackMeter& meterOf (const juce::Uuid& id) { return trackMeters[id.toString()]; }
    /** Reads the live input peaks (shared, about 30 times a second) for every track shown and lets the bars fall back smoothly. */
    void updateMeters()
    {
        const auto now = juce::Time::getMillisecondCounter();
        for (auto* t : shownTracks())
        {
            auto& m = meterOf (t->id);
            float playL = 0.0f, playR = 0.0f;                    // while a take plays back the bars show what the mixer strip measures (it always keeps measuring, so it is read every time)
            if (auto* st = stripOf (t->id)) { playL = st->livePkL.take(); playR = st->livePkR.take(); }
            const bool playing = app.isPlaying() && ! app.engine.isRecording();
            for (int c = 0; c < juce::jmin (kMaxMeterCh, t->channelCount()); ++c)
            {
                float pk = 0.0f;
                const int in = t->inputOf (c);
                if (playing) pk = t->channelCount() > 1 ? (c == 0 ? playL : playR) : juce::jmax (playL, playR);
                else if (juce::isPositiveAndBelow (in, (int) app.inPeaks.size())) pk = app.inPeaks[(size_t) in];
                auto& lv = m.level[(size_t) c]; auto& hv = m.hold[(size_t) c]; auto& ha = m.holdAt[(size_t) c];
                lv = juce::jmax (pk, lv * 0.86f);
                if (pk >= hv || now - ha > 1500) { hv = pk >= hv ? pk : lv; ha = now; }
            }
        }
        repaint (viewport.getViewPositionX(), kRulerH + kLaneH, kNameW, juce::jmax (1, (int) shownTracks().size()) * rowH);
    }

    void timerCallback() override
    {
        if (const int held = modhint::heldFor (*this); held != shownMods)      // the corner box: what the keys being held do here
        {
            shownMods = held;
            repaint (viewport.getViewPositionX(), viewport.getViewPositionY(), kNameW, kRulerH + kLaneH);
        }
        updateMeters();
        refreshThumbsAndPrune();
        if (app.engine.isRecording() || app.isPlaying()) updateSize();
        if (app.engine.isRecording())                                        // the growing take is drawn as it comes in: only its own columns are repainted
        {
            followPlayhead();
            auto* w = def(); auto* g = w != nullptr ? w->findGroup (app.engine.currentTakeId()) : nullptr;
            if (g == nullptr) repaint();
            else { const int x0 = xOf (g->startSeconds) - 4, x1 = xOf (g->startSeconds + app.engine.recordedSeconds()) + 8; repaint (x0, 0, juce::jmax (12, x1 - x0), getHeight()); }
        }
        else if (app.isPlaying()) followPlayhead();                          // the playhead moves by itself with the screen (movePlayhead)
        else followActive = false;
    }

    /** Called at every refresh of the screen (VBlankAttachment): puts the moving playhead where the music is now, redrawing only the thin strip it
        leaves and the one it moves to. Redrawing every waveform 30 times a second is what made it stutter with many tracks. (While recording the
        whole window is redrawn by the timer anyway, with the end of the growing take as its playhead.) */
    static constexpr int kNoLine = std::numeric_limits<int>::min();
    void movePlayhead()
    {
        const double live = app.engine.isRecording() || ! app.isPlaying() ? -1.0 : livePosition (true);
        const int x = live >= 0.0 ? xOf (live) : kNoLine;
        if (x == shownPhX) return;
        const int h = juce::jmax (1, getHeight() - kRulerH);
        if (shownPhX != kNoLine) repaint (theme::playheadStrip (shownPhX, kRulerH, h));      // (the whole reach of the line and its glow, or a trail of glow is left behind it)
        if (x != kNoLine) repaint (theme::playheadStrip (x, kRulerH, h));
        shownPhX = x;
    }
    int shownPhX = kNoLine;

    /** The playhead (recording or playing here) moves left to right; when it reaches the right edge the view turns a whole page,
        putting the playhead at the left again, instead of scrolling continuously. */
    void followPlayhead()
    {
        double at = livePosition();
        if (at < 0.0) { followActive = false; return; }
        const int x = xOf (at), vx = viewport.getViewPositionX(), right = vx + viewport.getMaximumVisibleWidth() - 6;
        const bool outside = x > right || x < vx + kNameW;
        if (! followActive)                                     // just started: make sure it can be seen
        {
            followActive = true;
            if (outside) viewport.setViewPosition (juce::jmax (0, x - kNameW - 12), viewport.getViewPositionY());
        }
        else if (x > right && lastFollowX <= right)             // it just crossed the right edge: turn the page
            viewport.setViewPosition (juce::jmax (0, x - kNameW - 12), viewport.getViewPositionY());
        lastFollowX = x;
    }

    AppContext& app;
    juce::Uuid windowId;
    juce::Viewport& viewport;
    juce::AudioFormatManager formatManager;
    WaveThumb::Pool thumbPool;
    DirectWaveCache directWave { formatManager };
    std::map<juce::String, std::unique_ptr<WaveThumb>> thumbs;
    juce::Uuid dragId; bool dragging = false; double dragOriginSeconds = 0.0;
    int flagDrag = 0;                                      // 1 = dragging the Edit IN flag, 2 = the Edit OUT flag
    /** Which Edit flag (1 = IN, 2 = OUT) is under this point: the little label at the top of the take or the line itself. */
    int editFlagAt (TakeWindowDef& w, juce::Point<int> p) const
    {
        if (p.y < kRulerH || p.y >= kRulerH + 14) return 0;
        auto* eg = w.findGroup (w.editTake);
        if (eg == nullptr) return 0;
        const int gx = xOf (eg->startSeconds);
        if (w.editIn >= 0)  { const int x = gx + (int) (w.editIn * pixelsPerSecond);  if (p.x >= x - 3 && p.x <= x + 48) return 1; }
        if (w.editOut >= 0) { const int x = gx + (int) (w.editOut * pixelsPerSecond); if (p.x >= x - 48 && p.x <= x + 3) return 2; }
        return 0;
    }
    juce::Uuid markDragTake; bool markActive = false, markAlt = false; double markAnchor = 0.0; int markStartX = 0, markRow0 = 0;     // dragging over a take's tracks sets Edit IN (mouse down) and OUT (mouse up)
    int fadeDrag = 0; juce::Uuid fadeTake; double fadeBase = 0.0;      // dragging a take's fade-in (1) or fade-out (2) from its top corner

    /** The take whose top corner (within 12 px of its start or end, in the lane) is under p. A take being recorded has no handles. */
    TakeGroup* fadeHandleAt (TakeWindowDef& w, juce::Point<int> p, bool& isIn)
    {
        if (p.y < kRulerH || p.y >= kRulerH + kLaneH || p.x <= kNameW) return nullptr;
        for (auto& grp : w.groups)
        {
            if (app.engine.isRecording() && grp.id == app.engine.currentTakeId()) continue;
            const int x0 = xOf (grp.startSeconds), wpx = juce::jmax (4, (int) (grp.lengthSeconds() * pixelsPerSecond)), zone = juce::jmin (12, wpx / 2);
            if (p.x >= x0 && p.x < x0 + zone)              { isIn = true;  return &grp; }
            if (p.x >= x0 + wpx - zone && p.x <= x0 + wpx) { isIn = false; return &grp; }
        }
        return nullptr;
    }
    bool followActive = false; int lastFollowX = 0; bool scrubbing = false;
    juce::VBlankAttachment vblank { this, [this] { movePlayhead(); } };     // (last, so everything it uses exists first)

    friend class TakeWindowComponent;
};

/** Viewport that redraws the sticky track names when scrolled. */
struct ScrollRepaintViewport : public juce::Viewport
{
    void visibleAreaChanged (const juce::Rectangle<int>&) override { if (auto* c = getViewedComponent()) c->repaint(); }
};

TakeWindowComponent::TakeWindowComponent (AppContext& a, const juce::Uuid& wid) : app (a), windowId (wid)
{
    auto* vp = new ScrollRepaintViewport();
    timeline.reset (new TakeTimeline (app, windowId, *vp));
    vp->setViewedComponent (timeline.get(), false);
    vp->setScrollBarsShown (true, true);
    addAndMakeVisible (vp);
    viewportOwner.reset (vp);
    setWantsKeyboardFocus (true);
    keysKeeper = std::make_unique<KeepKeysOnTimeline> (*vp, *timeline);

    auto* w = def();
    nameLabel.setText ("Piece / Take Window Name:", juce::dontSendNotification);
    addAndMakeVisible (nameBlock);
    addAndMakeVisible (takeBox);
    nameEditor.setText (w ? w->name : "", false);
    auto commitName = [this]
    {
        if (auto* d = def())
            if (d->name != nameEditor.getText() && nameEditor.getText().isNotEmpty())
            {
                d->name = nameEditor.getText();     // new takes go into a folder with the new name
                app.project.structureChanged();
            }
    };
    nameEditor.onFocusLost = commitName;
    nameEditor.onReturnKey = [this, commitName] { commitName(); timeline->grabKeyboardFocus(); };     // Enter: confirm and leave the box: the keyboard goes back to the timeline (its keys work at once)

    recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb3261e));
    recordButton.setBoxLook (true);                         // the same size and shape as the Next Take box beside it
    recordButton.onClick = [this] { app.toggleRecord (windowId); };
    armAllButton.setTooltip ("Arm every track of this window for recording (you can also click ARM on a track's name)");
    armNoneButton.setTooltip ("Disarm every track");
    armAllButton.onClick  = [this] { if (! app.engine.isRecording()) { for (auto& t : app.project.tracks) t.setArmed (true);  app.project.changed(); } };
    armNoneButton.onClick = [this] { if (! app.engine.isRecording()) { for (auto& t : app.project.tracks) t.setArmed (false); app.project.changed(); } };
    addAndMakeVisible (armAllButton); addAndMakeVisible (armNoneButton);
    addAndMakeVisible (ltcButton); ltcButton.setWantsKeyboardFocus (false);
    ltcButton.setTooltip ("Show or hide the LTC (timecode) track of this window. The LTC is recorded with every take on the track named LTC; choose its input with the LTC IN lamp in the top bar.");
    ltcButton.onClick = [this] { if (auto* w = def()) { w->showLtc = ! w->showLtc; app.project.changed(); updateLtcButton(); } };
    updateLtcButton();
    armAllButton.setWantsKeyboardFocus (false); armNoneButton.setWantsKeyboardFocus (false);
    zoomInButton.onClick = [this] { timeline->zoom (1.5); };
    zoomOutButton.onClick = [this] { timeline->zoom (1.0 / 1.5); };
    playButton.onClick = [this] { togglePlay(); };
    startButton.setTooltip ("Go to the start of the window [Home]. While playing, play carries on from there.");
    backButton.setTooltip ("Back 5 seconds");
    fwdButton.setTooltip ("Forward 5 seconds");
    endTransportButton.setTooltip ("Go to the end of the last take [End]. You can play on beyond it: the silence after the takes plays too.");
    startButton.onClick = [this] { goTo (0.0); };
    backButton.onClick = [this] { jumpBy (-5.0); };
    fwdButton.onClick = [this] { jumpBy (5.0); };
    endTransportButton.onClick = [this] { goToEnd(); };
    for (auto* b : { &startButton, &backButton, &fwdButton, &endTransportButton }) { addAndMakeVisible (b); b->setWantsKeyboardFocus (false); }
    toEditButton.onClick = [this] { toEdit(false); };
    toEditAtButton.onClick = [this] { toEdit (true, juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown()); };
    toEditAtButton.setTooltip ("Puts the marked part at the edit window's playhead and REPLACES the same length of what is there: the rest of the edit does not move. "
                               "Shift + click (or Shift + 4) INSERTS it instead: what is there is cut in two and everything after it moves later.");
    editInButton.onClick = [this] { editMarkIn(); };
    editOutButton.onClick = [this] { editMarkOut(); };
    overdubBox.setTooltip ("Pieces sent to the edit are laid OVER it and play together with what is already there");
    overdubBox.onClick = [this] { if (auto* w = def()) { w->overdub = overdubBox.getToggleState(); app.project.changed(); } };
    toEditAtButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    overdubBox.setWantsKeyboardFocus (false);
    playheadMode.setTooltip ("Playhead behaviour. Lit: the playhead moves with the time, so after Stop the next Play carries on from where you stopped. "
                             "Off: it jumps back to where playback began. (The same setting in every window.)");
    playheadMode.onClick = [this] { app.setPlayheadFollows (! app.playheadFollows); };
    playheadMode.setToggleState (app.playheadFollows, juce::dontSendNotification);
    playheadMode.setWantsKeyboardFocus (false);
    addAndMakeVisible (playheadMode);
    waveColourBtn.setTooltip ("WaveColour. Lit: the waveforms are coloured by their sound. The hue is the pitch (red low, green middle, blue high); vivid colour is a clear note, pale colour is noise; BLACK is a thump below 100 Hz, such as a kicked mic stand. Off: ordinary waveforms. (The same in every window.)");
    waveColourBtn.onClick = [this] { app.setWaveColour (! app.waveColour); };
    waveColourBtn.setToggleState (app.waveColour, juce::dontSendNotification);
    waveColourBtn.setWantsKeyboardFocus (false);
    addAndMakeVisible (waveColourBtn);
    loopToggle.setTooltip ("Loop: when lit, Play repeats the marked area (the marks 1 and 2) over and over until you press Stop. Key L.");
    loopToggle.onClick = [this] { loopChanged(); };
    loopToggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (loopToggle);
    if (auto* w0 = def()) overdubBox.setToggleState (w0->overdub, juce::dontSendNotification);
    addAndMakeVisible (overdubBox);
    markersToggle.setTooltip ("Show or hide your own markers (the flags on the top line). Right-click that line to add one.");
    if (auto* w1 = def()) markersToggle.setToggleState (w1->showMarkers, juce::dontSendNotification);
    markersToggle.onClick = [this] { if (auto* w2 = def()) { w2->showMarkers = markersToggle.getToggleState(); app.project.markDirty(); timeline->repaint(); } };
    markersToggle.setWantsKeyboardFocus (false); addAndMakeVisible (markersToggle);
    editWindowButton.onClick = [this] { openEditWindow(); };
    bounceButton.onClick = [this] { openBounce(); };
    importButton.setTooltip ("Bring in the takes of a session recorded in another program (multichannel / polyphonic WAV, or one mono / stereo file per track). "
                             "You can also drag the files, or their folder, onto this window.");
    importButton.onClick = [this] { fixtools::showImportMenu (app, windowId, &importButton); };
    barSearchButton.setTooltip ("Search for bar: type a bar number and every take here that contains it (from its IN Bar to its OUT Bar, set by right-clicking a take) is framed in blue-green, so you can see which takes have that bar. Click again to clear.");
    barSearchButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff19a08a));
    barSearchButton.onClick = [this] { toggleBarSearch(); };
    pitchButton.setTooltip ("Two tabs. Pitch curve: draw a line of pitch against time over the part marked with mark 1 and mark 2 (+100 cents at the top, -100 at the bottom), for a note that drifts flat or sharp; audition it, then accept or revert. Pitch process: shifts the marked part on every track of the take by semitones and / or cents.");
    // With no 1 / 2 flags set, a take that was selected whole (Ctrl + click) is loaded in full into the process window.
    auto withWholeTake = [this] (std::function<void()> run)
    {
        auto* w = def(); bool temp = false; juce::Uuid oldTake; double oldIn = -1.0, oldOut = -1.0;
        if (w != nullptr && (w->findGroup (w->editTake) == nullptr || w->editIn < 0.0 || w->editOut <= w->editIn) && ! timeline->selectedTakes.empty())
            if (auto* g = w->findGroup (timeline->selectedTakes.front()))
            {
                oldTake = w->editTake; oldIn = w->editIn; oldOut = w->editOut;
                w->editTake = g->id; w->editIn = 0.0; w->editOut = g->lengthSeconds(); w->editTracks.clear(); temp = true;
            }
        run();
        if (temp) if (auto* w2 = def()) { w2->editTake = oldTake; w2->editIn = oldIn; w2->editOut = oldOut; }
    };
    pitchButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::pitchTabsTake (app, windowId, this); }); };
    repairButton.setTooltip ("Shows the marked part (Mark 1 / Mark 2) of all tracks as one spectrogram; draw a box round a noise (drag its edges to adjust it) and rebuild it from the clean sound next to it (ReTouch), take the background noise out of it with the room tone (Denoise), or use De-click to find and mend clicks");
    repairButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::repairTake (app, windowId, this); }); };
    exportProcButton.setTooltip ("Export for Processing: sends the marked part (Mark 1 / Mark 2, all tracks) to the Processing Media folder to be corrected in other software (e.g. iZotope RX). Re-link it afterwards with a right-click on the 'Waiting for corrected audio' block");
    exportProcButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::exportForProcessingTake (app, windowId, this); }); };
    alignButton.setTooltip ("Align spot mics: after a run-through, measure how much earlier each spot mic hears the music than the main pair, and delay it to line up "
                            "(every mixer; the files are not changed). Uses this take window's take between marks 1 and 2, or the whole take under the playhead.");
    alignButton.onClick = [this] { if (app.showAlign) app.showAlign (false, windowId); };
    takeSheetButton.setTooltip ("Take sheet: every take of this window with its time, length, bars, markers and duds, as a PDF to print or send (and a CSV for a spreadsheet)");
    takeSheetButton.onClick = [this] { exportTakeSheet (app, windowId); };
    undoFixButton.setTooltip ("Undo the last pitch correction, repair, de-click, denoise or export for processing (the original audio files were never touched)");
    undoFixButton.onClick = [this] { fixtools::undoLastFix (app); };
    bounceButton.setTooltip ("Make a master file of the marked take (or the marked part of it) through the processing mixer");
    toEditButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    recordButton.setIcon (Icon::Record, "Record [R]"); playButton.setIcon (Icon::Play, "Play / Stop [Space]"); startButton.setIcon (Icon::Start, "Return to start [Home]"); backButton.setIcon (Icon::Rewind, "Rewind 5 seconds");
    fwdButton.setIcon (Icon::Forward, "Forward 5 seconds"); endTransportButton.setIcon (Icon::End, "Go to the end [End]"); bounceButton.setIcon (Icon::Bounce, "Bounce Out");
    pitchButton.setIcon (Icon::Pitch, "Pitch & Pitch curve"); repairButton.setIcon (Icon::Repair, "ReTouch, DeClick & Denoise"); undoFixButton.setIcon (Icon::Undo, "Undo fix");
    editInButton.setIcon (Icon::Mark1, "Mark 1 [1]"); editOutButton.setIcon (Icon::Mark2, "Mark 2 [2]");
    toEditButton.setIcon (Icon::ToEdit, "To edit [3]"); toEditAtButton.setIcon (Icon::ToPlayhead, "To playhead [4]"); editWindowButton.setIcon (Icon::EditWin, "Edit window");
    overdubBox.setIcon (Icon::Overdub, "Overdub"); markersToggle.setIcon (Icon::Flag, "Markers");
    armAllButton.setIcon (Icon::ArmAll, "Arm all"); armNoneButton.setIcon (Icon::ArmNone, "Arm none");
    importButton.setIcon (Icon::ImportTakes, "Import takes"); exportProcButton.setIcon (Icon::ImportMulti, "Export for Processing"); barSearchButton.setIcon (Icon::Search, "Search for bar");
    alignButton.setIcon (Icon::Align, "Align spot mics"); takeSheetButton.setIcon (Icon::TakeSheet, "Take sheet");
    for (auto* t : { &overdubBox, &markersToggle }) { t->setClickingTogglesState (true); t->setColour (juce::TextButton::buttonOnColourId, theme::accent); }
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &zoomInButton, &zoomOutButton, &playButton,
                                                              &editInButton, &editOutButton, &toEditButton, &toEditAtButton, &editWindowButton, &bounceButton, &importButton, &pitchButton, &repairButton, &exportProcButton, &undoFixButton, &barSearchButton, &alignButton, &takeSheetButton, &lengthClock, &statusLabel, &markLabel })
        addAndMakeVisible (c);
    for (auto* b : std::initializer_list<juce::Button*> { &recordButton, &zoomInButton, &zoomOutButton, &playButton,
                                                          &editInButton, &editOutButton, &toEditButton, &toEditAtButton, &editWindowButton, &bounceButton, &importButton, &pitchButton, &repairButton, &exportProcButton, &undoFixButton, &barSearchButton, &alignButton, &takeSheetButton })
        b->setWantsKeyboardFocus (false);       // keep the keyboard shortcuts working after a button click
    // the take's length: a quieter box of the same kind as the time readout (fixed digit cells, 30 times a second, so it counts as smoothly);
    // grey with softer digits, always h:m:s.ms, dashes when nothing is recording here
    lengthClock.setLengthStyle ("TAKE LENGTH");
    lengthClock.follow ([this] { return hereRecording() ? app.engine.recordedSeconds() : 0.0; },
                        [this] { return hereRecording() ? TimeReadout::State::recording : TimeReadout::State::stopped; });
    statusLabel.setColour (juce::Label::textColourId, theme::warn);

    // the time readout: where this window's playhead is (moving while it plays or records here); white when stopped, blue while anything plays, red while recording
    addAndMakeVisible (clock);
    clock.follow ([this] { return timeline != nullptr ? timeline->clockSeconds() : 0.0; },
                  [this] { return app.engine.isRecording() ? TimeReadout::State::recording : app.isPlaying() ? TimeReadout::State::playing : TimeReadout::State::stopped; });
    clock.onJump = [this] (double s) { goTo (s); };
    clock.onFormatChanged = [this] { resized(); };
    markLabel.setColour (juce::Label::textColourId, theme::text);

    sendCaption.setText ("To edit goes into:", juce::dontSendNotification);
    nameBlock.addPair (sendCaption, sendBox);                  // "To edit goes into:" and its box under the window's name, under a line, as wide as the name box
    nameBlock.setTopAligned (true);                            // the column starts at the very top, every row as close as it can be
    for (auto* cap : { &nameLabel, &sendCaption })            // the captions' words at the very top of their row
    {
        cap->setJustificationType (juce::Justification::topLeft);
        cap->setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    }
    sendBox.setWantsKeyboardFocus (false);                      // changed only by clicking: the arrow keys stay the window's (zoom, track heights)
    sendBox.onChange = [this]
    {
        auto* w = def();
        if (w == nullptr || updatingSendBox) return;
        const int id = sendBox.getSelectedId();
        if (id == 1000)
        {
            auto& e = app.makeEdit ("Edit " + juce::String ((int) app.project.edits.size() + 1));
            w->targetEdit = e.id;
            if (app.showEdit) app.showEdit (e.id);
        }
        else if (juce::isPositiveAndBelow (id - 1, (int) app.project.edits.size())) w->targetEdit = app.project.edits[(size_t) id - 1]->id;
        app.project.changed();
        fillSendBox();
    };
    fillSendBox();
    timeline->updateSize();
    app.project.addChangeListener (this);
    startTimerHz (10);
    setSize (juce::jlimit (900, 1700, juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.getWidth() - 60), 520);
}

TakeWindowComponent::~TakeWindowComponent() { app.clearTransportHandler (this); app.project.removeChangeListener (this); }

bool TakeWindowComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) { juce::File x (f); if (x.isDirectory() || fixtools::isAudioFile (x)) return true; }
    return false;
}

void TakeWindowComponent::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> list; for (auto& f : files) list.add (juce::File (f));
    fixtools::importTakes (app, windowId, list, this);
}

void TakeWindowComponent::fillSendBox()
{
    updatingSendBox = true;
    sendBox.clear (juce::dontSendNotification);
    int sel = 0, i = 1;
    for (auto& e : app.project.edits)
    {
        sendBox.addItem (e->name, i);
        if (def() != nullptr && e->id == def()->targetEdit) sel = i;
        ++i;
    }
    sendBox.addItem ("(new edit...)", 1000);
    sendBox.setSelectedId (sel != 0 ? sel : 1000, juce::dontSendNotification);
    if (sel == 0) sendBox.setText ("(a new edit is made on first use)", juce::dontSendNotification);
    updatingSendBox = false;
}

void TakeWindowComponent::updateLtcButton()
{
    auto* w = def();
    const bool on = w != nullptr && w->showLtc;
    ltcButton.setButtonText (on ? juce::String::fromUTF8 ("\xE2\x96\xBC LTC") : juce::String::fromUTF8 ("\xE2\x96\xB6 LTC"));
    ltcButton.setToggleState (on, juce::dontSendNotification);
}

void TakeWindowComponent::resized()
{
    auto r = getLocalBounds().reduced (6);
    using grid::FlowItem;
    constexpr int IW = 34, IH = 26;                               // the picture buttons
    const int clockW = clock.getIdealWidth (kClockH) + 4;
    // the picture buttons: to the right of the time readout, on as many lines as they need (two fit in the readout's height)
    const std::vector<FlowItem> items
    {
        { &startButton, IW, IH, 2 }, { &backButton, IW, IH, 2 }, { &playButton, IW, IH, 2 }, { &fwdButton, IW, IH, 2 }, { &endTransportButton, IW, IH, 6 }, { &playheadMode, 34, grid::btnH, 2 }, { &waveColourBtn, 34, grid::btnH, 2 }, { &loopToggle, 34, grid::btnH, 14 },
        { &editInButton, IW, IH, 2 }, { &editOutButton, IW, IH, 6 }, { &toEditButton, IW, IH, 2 }, { &toEditAtButton, IW, IH, 2 }, { &overdubBox, IW, IH, 6 }, { &markersToggle, IW, IH, 14 },
        { &editWindowButton, IW, IH, 2 }, { &bounceButton, IW, IH, 14 },
        { &ltcButton, 74, grid::btnH, 14 }, { &armAllButton, IW, IH, 2 }, { &armNoneButton, IW, IH, 14 },
        { &importButton, IW, IH, 6 }, { &pitchButton, IW, IH, 2 }, { &repairButton, IW, IH, 6 }, { &exportProcButton, IW, IH, 2 }, { &undoFixButton, IW, IH, 14 }, { &barSearchButton, IW, IH, 14 }, { &alignButton, IW, IH, 2 }, { &takeSheetButton, IW, IH, 14 },
        { &zoomOutButton, 34 }, { &zoomInButton, 34 }
    };
    // the head, left to right: the window's name and where "To edit" goes (one column, a line between them), Next Take, REC, the take's length, the time readout
    constexpr int colW = 200;
    const int y0 = r.getY() + 4;                                  // (the 58-high boxes sit 4 below the top, like a line of the old flowing bar)
    int x = r.getX();
    nameBlock.setBounds (x, getLocalBounds().getY() + 1, colW, nameBlock.idealHeight()); x += colW + 10;     // (the column starts right at the top of the window, a row above the boxes)
    takeBox.setBounds (x, y0, 84, 58);                            x += 84 + 6;
    recordButton.setBounds (x, y0, 84, 58);                       x += 84 + 6;
    const int tlH = 44, tlW = lengthClock.getIdealWidth (tlH);     // the take's length: smaller than the readout beside it, its bottom level with it
    lengthClock.setBounds (x, y0 + kClockH - tlH, tlW, tlH);      x += tlW + 6;
    clock.setBounds (x, y0, clockW, kClockH);                     x += clockW + 14;
    int headH = juce::jmax (nameBlock.getBottom(), y0 + kClockH + 4) - r.getY();
    const int btnRight = r.getRight() - grid::cornerReserve;      // (clear of the pin and sun / moon buttons in the corner)
    if (btnRight - x >= 300)                                  // the buttons to the right of the readout, a group never split between two lines
        headH = juce::jmax (headH, grid::flow (juce::Rectangle<int> (x, r.getY(), btnRight - x, headH), items, true, nullptr, true));
    else                                                          // a narrow window: the buttons go under the head, across the whole width
        headH += grid::flow (juce::Rectangle<int> (r.getX(), r.getY() + headH, r.getWidth(), 0), items, true, nullptr, true);
    // the marks (or the playhead) on the same line as the "To edit goes into" box, under Next Take, REC and the readouts: the row it had of its own
    // under the head is the timeline's now. A recording problem is shown in the same place while there is one.
    {
        const auto sb = sendBox.getBoundsInParent() + nameBlock.getPosition();
        const auto row = juce::Rectangle<int> (takeBox.getX(), sb.getY(), juce::jmax (100, clock.getRight() - takeBox.getX()), sb.getHeight());
        markLabel.setBounds (row); statusLabel.setBounds (row);
    }
    r.removeFromTop (headH + 4);
    if (viewportOwner != nullptr) { viewportOwner->setBounds (r); if (timeline != nullptr) timeline->applyHeightLevel(); }
}

// ----------------------------------------------------------------------------- editing actions
/** P: with a take selected, marks 1 and 2 go to its very start and end, sample-exact. */
void TakeWindowComponent::markWholeSelected()
{
    auto* w = def();
    if (w == nullptr) return;
    const TakeGroup* g = nullptr;
    for (auto& id : timeline->selectedTakes) { g = w->findGroup (id); if (g != nullptr) break; }
    if (g == nullptr) { showError ("Marks 1 and 2", "Click a take first (in the grey bar above its tracks), then press P to put marks 1 and 2 round all of it."); return; }
    w->editTake = g->id; w->editIn = 0.0; w->editOut = g->lengthSeconds(); w->editTracks.clear();
    app.project.changed();
}

void TakeWindowComponent::editMarkIn()
{
    auto* w = def();
    if (w == nullptr) return;
    double sec = 0.0;
    auto* g = timeline->markPoint (sec);
    if (g == nullptr) { showError ("Mark 1", "Put the playhead inside a take first (click the ruler at the top), or press 1 while a take plays."); return; }
    if (w->editTake != g->id) { w->editTake = g->id; w->editOut = -1.0; }
    w->editIn = sec;
    if (w->editOut >= 0 && w->editOut <= w->editIn) w->editOut = -1.0;
    app.project.changed();
}

void TakeWindowComponent::editMarkOut()
{
    auto* w = def();
    if (w == nullptr) return;
    double sec = 0.0;
    auto* g = timeline->markPoint (sec);
    if (g == nullptr) { showError ("Mark 2", "Put the playhead inside a take first (click the ruler at the top), or press 2 while a take plays."); return; }
    if (w->editTake != g->id) { w->editTake = g->id; w->editIn = -1.0; }
    w->editOut = sec;
    if (w->editIn >= 0 && w->editIn >= w->editOut) w->editIn = -1.0;
    app.project.changed();
}

void TakeWindowComponent::toEdit (bool atEditPlayhead, bool insert)
{
    auto* w = def();
    if (w == nullptr) return;
    const bool marksOk = w->findGroup (w->editTake) != nullptr && w->editIn >= 0.0 && w->editOut > w->editIn;
    if (marksOk && app.project.findEdit (w->targetEdit) == nullptr)        // the first piece sent from this window: the edit does not exist yet, so it gets its name now
    {
        auto* aw = new juce::AlertWindow ("Name the edit", "This is the first piece you send from \"" + w->name + "\".\nWhat should the edit be called?", juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor ("name", w->name + " - EDIT", "Edit name");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto safe = juce::Component::SafePointer<TakeWindowComponent> (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, atEditPlayhead, insert] (int r)
        {
            if (r == 1 && safe != nullptr)
                if (auto* w2 = safe->def())
                    if (safe->app.project.findEdit (w2->targetEdit) == nullptr)
                    {
                        auto nm = aw->getTextEditorContents ("name").trim();
                        if (nm.isEmpty()) nm = w2->name + " - EDIT";
                        auto& e = safe->app.project.addEdit (nm, safe->windowId);
                        w2->targetEdit = e.id;
                        safe->app.project.changed();
                        safe->sendToEdit (atEditPlayhead, insert);
                    }
            delete aw;
        }), false);
        return;
    }
    sendToEdit (atEditPlayhead, insert);
}

void TakeWindowComponent::sendToEdit (bool atEditPlayhead, bool insert)
{
    juce::Uuid editId;
    auto err = app.copyMarkedToEdit (windowId, atEditPlayhead, &editId, insert);
    if (err.isNotEmpty()) { showError ("To edit", err); return; }
    // You stay in this window: send all the takes you want, then go and edit. The edit window is never opened, restored or raised by this; it just
    // picks the new material up from the project whenever it is open.
    if (app.showEditBehind) app.showEditBehind (editId, this);
}

void TakeWindowComponent::togglePlay()
{
    if (app.isPlaying()) { app.stopPlayback (true); return; }
    auto* w = def();
    if (w == nullptr) return;
    if (loopToggle.getToggleState())                                    // loop on and an area marked: go round it
    {
        juce::Uuid lt; double li = 0.0, lo = 0.0;
        if (markedRange (lt, li, lo)) { const auto err = playLoop (lt, li, lo); if (err.isNotEmpty()) showError ("Play", err); return; }
        app.setNotice ("Loop is on, but no area is marked (marks 1 and 2): playing normally.");
    }
    // the whole window plays from the playhead, wherever it is: over a take, in a gap, or after the last take (silent, it just carries on)
    auto err = app.playTakeWindow (windowId, w->playheadSeconds);
    if (err.isNotEmpty()) showError ("Play", err);
}

void TakeWindowComponent::playMarked()
{
    auto* w = def();
    if (w == nullptr) return;
    if (app.isPlaying()) app.stopPlayback();
    const bool useEdit = w->findGroup (w->editTake) != nullptr && w->editIn >= 0;
    const auto take = w->editTake; (void) useEdit;
    const double in = w->editIn, out = w->editOut;
    if (w->findGroup (take) == nullptr || in < 0) { showError ("Play marked", "Mark an IN point (and an OUT point) first."); return; }
    auto err = app.playTake (windowId, take, in, out > in ? out : -1.0, loopToggle.getToggleState() && out > in);
    if (err.isNotEmpty()) showError ("Play", err);
}

bool TakeWindowComponent::markedRange (juce::Uuid& take, double& in, double& out) const
{
    auto* w = app.project.findTakeWindow (windowId);
    if (w == nullptr) return false;
    const bool useEdit = w->findGroup (w->editTake) != nullptr && w->editIn >= 0;
    take = w->editTake; in = w->editIn; out = w->editOut; (void) useEdit;
    return w->findGroup (take) != nullptr && in >= 0 && out > in;
}

void TakeWindowComponent::toggleLoop()
{
    loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification);
    loopChanged();
}

int TakeWindowComponent::deskActState (const juce::String& what) const
{
    if (what == "mark1" || what == "mark2" || what == "align") return 0;
    if (what == "search")  return searchBar > 0 ? 1 : 0;
    if (what == "markers") { auto* w = def(); return w != nullptr && w->showMarkers ? 1 : 0; }
    return -1;
}

void TakeWindowComponent::deskAct (const juce::String& what)
{
    if (what == "mark1") editMarkIn();
    else if (what == "mark2") editMarkOut();
    else if (what == "align") { if (app.showAlign) app.showAlign (false, windowId); }
    else if (what == "search") toggleBarSearch();
    else if (what == "markers") markersToggle.triggerClick();
}

void TakeWindowComponent::loopChanged()
{
    const bool on = loopToggle.getToggleState();
    const bool mine = app.isPlaying() && ! app.engine.isRecording() && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowId == windowId;
    juce::Uuid take; double in = 0.0, out = 0.0;
    if (on && ! markedRange (take, in, out)) { app.setNotice ("Loop: mark an area first (marks 1 and 2, or [P] for a whole take): the loop plays the area between them."); return; }
    if (! mine) return;
    if (on) { const auto err = playLoop (take, in, out); if (err.isNotEmpty()) showError ("Play", err); }
    else app.engine.releasePlaybackLoop();                               // switched off: plays straight on past mark 2, through the window
}

juce::String TakeWindowComponent::playLoop (const juce::Uuid& take, double in, double out)
{
    // on the window's own timeline (like Play), so that switching Loop off carries on through the window as Play would
    auto* w = def();
    auto* g = w != nullptr ? w->findGroup (take) : nullptr;
    if (g == nullptr) return "That take no longer exists";
    return app.playTakeWindow (windowId, g->startSeconds + in, g->startSeconds + out, take);
}

void TakeWindowComponent::openBounce()
{
    auto* w = def();
    if (w == nullptr || ! app.showBounce) return;
    BounceContext c;
    c.kind = BounceContext::Kind::Take; c.id = windowId;
    for (auto& g : w->groups) if (timeline->isTakeSelected (g.id)) c.selected.push_back (g.id);     // only takes that still exist
    const auto takeId = (! w->editTake.isNull()) ? w->editTake : w->cursorTake;
    if (c.selected.size() > 1) c.defaultName = w->name;                                              // each file then gets " - take NNN"
    else if (c.selected.size() == 1) { if (auto* g = w->findGroup (c.selected[0])) c.defaultName = w->name + " - take " + pad3 (g->number); }
    else if (auto* g = w->findGroup (takeId)) c.defaultName = w->name + " - take " + pad3 (g->number);
    else if (! w->groups.empty()) c.defaultName = w->name;                                           // nothing picked: still open it, 'All takes' works
    else { showError ("Bounce Out", "There are no takes in this window yet."); return; }
    app.showBounce (c);
}

void TakeWindowComponent::openEditWindow()
{
    auto* w = def();
    EditDef* e = w != nullptr ? app.project.findEdit (w->targetEdit) : nullptr;
    if (e == nullptr)
    {
        e = &app.project.addEdit ((w ? w->name : juce::String ("Piece")) + " - EDIT", windowId);
        if (w) w->targetEdit = e->id;
        app.project.changed();
    }
    if (app.showEdit) app.showEdit (e->id);
}

double TakeWindowComponent::transportNow() const
{
    auto* w = def();
    if (w == nullptr) return 0.0;
    if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowId == windowId && app.playInfo.windowTimeline)
        return juce::jmax (0.0, app.playheadSeconds());
    return w->playheadSeconds;
}

void TakeWindowComponent::goTo (double seconds)
{
    auto* w = def();
    if (w == nullptr || app.engine.isRecording()) return;
    seconds = juce::jmax (0.0, seconds);
    w->setPlayhead (seconds);
    if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowId == windowId)
    {
        const auto err = app.playTakeWindow (windowId, seconds);
        if (err.isNotEmpty()) showError ("Play", err);
    }
    app.project.changed();
    if (timeline != nullptr) timeline->scrollTo (seconds);
}

void TakeWindowComponent::goToEnd() { if (auto* w = def()) goTo (w->endSeconds()); }

void TakeWindowComponent::nudgeCursor (double seconds)
{
    auto* w = def();
    if (w == nullptr) return;
    w->setPlayhead (w->playheadSeconds + seconds);
    app.project.changed();
}

bool TakeWindowComponent::keyPressed (const juce::KeyPress& k)
{
    {   // Ctrl + A: select every take (then Ctrl + Shift + drag moves all their clips on a track onto another track)
        const auto km = k.getModifiers();
        if ((km.isCtrlDown() || km.isCommandDown()) && ! km.isAltDown() && ! km.isShiftDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a')) { timeline->selectAllTakes(); return true; }
    }
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (k == juce::KeyPress::spaceKey)                                  // Space plays (or stops playing) from the playhead. A recording is stopped by AppContext,
    {                                                                   // which watches Space itself, from whichever window
        if (spaceIsForRecording()) return true;
        togglePlay(); return true;
    }
    if (c == 'r' && ! k.getModifiers().isAnyModifierKeyDown())          // R starts recording into this window (AppContext may already have done it: then nothing more happens)
    {
        if (! app.engine.isRecording()) app.toggleRecord (windowId);
        return true;
    }
    if (k == juce::KeyPress::homeKey) { goTo (0.0);  return true; }
    if (k == juce::KeyPress::endKey)  { goToEnd();   return true; }
    if (c == 'p')                       { markWholeSelected(); return true; }
    if (c == 'd' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->toggleDud(); return true; }
    if (c == 'l' && ! k.getModifiers().isAnyModifierKeyDown()) { loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification); loopChanged(); return true; }
    if (c == 'c' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->centreOnPlayhead(); return true; }
    if (c == '.' || c == '>')           { app.changeWaveZoom (1.4f);        return true; }     // . bigger waveforms
    if (c == ',' || c == '<')           { app.changeWaveZoom (1.0f / 1.4f); return true; }     // , smaller
    if (isShiftFour (k))                { toEdit (true, true); return true; }                   // Shift + 4: the same, but an INSERT (the rest of the edit moves later)
    if (! k.getModifiers().isAnyModifierKeyDown())
    {
        if (c == '1') { editMarkIn();  return true; }
        if (c == '2') { editMarkOut(); return true; }
        if (c == '5') { if (auto* w5 = def()) { w5->editTake = juce::Uuid::null(); w5->editIn = w5->editOut = -1.0; w5->editTracks.clear(); app.project.changed(); } return true; }     // 5: take both Edit flags away
        if (c == '3') { toEdit (false); return true; }
        if (c == '4') { toEdit (true);  return true; }                  // 4: put it at the edit's playhead over the same length of what is there
    }
    // Right = zoom in, Left = zoom out (time). Shift + arrow nudges the cursor instead.
    {   // (KeyPress == a key code is false whenever a modifier is held: Shift + arrow has to be asked for by its code, or it never arrives here)
        const auto km = k.getModifiers();
        const bool onlyShift = km.isShiftDown() && ! km.isCtrlDown() && ! km.isCommandDown() && ! km.isAltDown();
        if (onlyShift && k.getKeyCode() == juce::KeyPress::leftKey)  { nudgeCursor (-0.01); return true; }
        if (onlyShift && k.getKeyCode() == juce::KeyPress::rightKey) { nudgeCursor (0.01);  return true; }
    }
    if (k == juce::KeyPress::leftKey)   { timeline->zoom (1.0 / 1.4); return true; }
    if (k == juce::KeyPress::rightKey)  { timeline->zoom (1.4);       return true; }
    // Up = all tracks smaller, Down = all tracks bigger (8 steps)
    if (k == juce::KeyPress::upKey)     { timeline->changeHeightLevel (-1); return true; }
    if (k == juce::KeyPress::downKey)   { timeline->changeHeightLevel (+1); return true; }
    return false;
}

void TakeWindowComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto t0 = paintprof::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    fillSendBox(); repaint();
    if (paintprof::enabled()) paintprof::paintDone ("take window change", juce::Time::getMillisecondCounterHiRes() - t0);
}

void TakeWindowComponent::timerCallback()
{
    if (auto* fc = juce::Component::getCurrentlyFocusedComponent(); fc != nullptr && isParentOf (fc)) app.setTransportHandler (this, [this] { togglePlay(); });   // the Stream Deck's Play uses the window you last worked in
    const bool rec = app.engine.isRecording();
    if (auto* tw = dynamic_cast<juce::TopLevelWindow*> (getTopLevelComponent())) if (tw->isActiveWindow() && ! app.engine.isRecording()) app.recordTarget = windowId;
    { int n = 1; const bool r = app.takeBoxInfo (n); if (auto* d = def(); d != nullptr && ! r) n = d->nextNumber; takeBox.set (r, n); }
    const bool hereRec = rec && def() != nullptr && def()->findGroup (app.engine.currentTakeId()) != nullptr;
    recordButton.setButtonText (rec ? "STOP [Space]" : "REC [R]");
    if (playheadMode.getToggleState() != app.playheadFollows) playheadMode.setToggleState (app.playheadFollows, juce::dontSendNotification);
    if (waveColourBtn.getToggleState() != app.waveColour) { waveColourBtn.setToggleState (app.waveColour, juce::dontSendNotification); repaint(); }
    if (hereRec) { recStart = def()->findGroup (app.engine.currentTakeId()) != nullptr ? def()->findGroup (app.engine.currentTakeId())->startSeconds : recStart; wasRecordingHere = true; }
    else if (wasRecordingHere && ! rec)                                 // a recording just ended: park the playhead at the start of that take, so Space reviews it
    {
        wasRecordingHere = false;
        if (auto* w = def()) { w->setPlayhead (recStart); app.project.changed(); }
    }
    recordButton.setEnabled (! rec || hereRec);
    clock.setSampleRate (app.engine.getSampleRate());
    {
        juce::String st;
        if (hereRec)
        {
            if (const auto problem = app.engine.recordingProblem(); problem.isNotEmpty()) st = "RECORDING HAS STOPPED REACHING THE DISK - " + problem;
            else if (app.engine.overruns() > 0) st = "DISK OVERRUN - " + lostText (app.engine.samplesLost(), app.engine.getSampleRate()) + " of audio lost (silence in its place)";
        }
        statusLabel.setText (st, juce::dontSendNotification);
        statusLabel.setVisible (st.isNotEmpty()); markLabel.setVisible (st.isEmpty());          // (they share the line under the boxes: a problem comes first)
    }
    playButton.setButtonText (app.isPlaying() ? "Stop [Space]" : "Play [Space]");

    juce::String m;
    if (auto* w = def())
    {
        auto fmt = [] (double v) { return v >= 0 ? formatTimeShort (v, 3) : juce::String ("--"); };
        if (overdubBox.getToggleState() != w->overdub) overdubBox.setToggleState (w->overdub, juce::dontSendNotification);
        if (auto* ge = w->findGroup (w->editTake))
        {
            m = "EDIT " + w->displayName (*ge) + ":  Mark 1 " + fmt (w->editIn) + "   Mark 2 " + fmt (w->editOut);
            if (w->editIn >= 0 && w->editOut > w->editIn) m += "   (" + juce::String (w->editOut - w->editIn, 2) + " s)";
        }
        else if (auto* gc = w->findGroup (w->cursorTake))
            m = "Playhead: " + w->displayName (*gc) + "  " + formatTimeShort (w->cursorSeconds, 3);
        else m = "Playhead: " + formatTimeShort (w->playheadSeconds, 3) + "  (between takes)";
    }
    markLabel.setText (m, juce::dontSendNotification);
}
} // namespace td


namespace td
{
void TakeWindowComponent::setBarSearch (int bar)
{
    searchBar = bar;
    if (timeline != nullptr) { timeline->barHighlight = bar; timeline->repaint(); }
    if (bar <= 0) { barSearchButton.setIcon (Icon::Search, "Search for bar"); barSearchButton.setToggleState (false, juce::dontSendNotification); return; }
    int n = 0;
    if (auto* w = app.project.findTakeWindow (windowId))
        for (auto& g : w->groups) if (barsContain (g.barIn, g.barOut, bar)) ++n;
    barSearchButton.setIcon (Icon::Search, "Bar " + juce::String (bar) + ": " + juce::String (n) + (n == 1 ? " take" : " takes") + " (click to clear)");
    app.setNotice ("Bar " + juce::String (bar) + ": " + juce::String (n) + (n == 1 ? " take has it" : " takes have it") + " (framed in blue-green). Click the magnifying glass again to clear.");
    barSearchButton.setToggleState (true, juce::dontSendNotification);
}

void TakeWindowComponent::toggleBarSearch()
{
    if (searchBar > 0) { setBarSearch (0); return; }
    auto* aw = new juce::AlertWindow ("Search for bar", "Which bar do you need? Every take that contains it will be framed.", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("bar", {}, "Bar");
    if (auto* te = aw->getTextEditor ("bar")) te->setInputRestrictions (5, "0123456789");
    aw->addButton ("Search", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TakeWindowComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r)
    {
        const int bar = aw->getTextEditorContents ("bar").trim().getIntValue();
        delete aw;
        if (safe != nullptr && r == 1 && bar > 0) safe->setBarSearch (bar);
    }));
    if (auto* te = aw->getTextEditor ("bar")) te->grabKeyboardFocus();
}
} // namespace td

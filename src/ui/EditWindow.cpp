#include "WaveDraw.h"
#include "EditWindow.h"
#include "PaintProfile.h"
#include <set>
#include <map>
#include <limits>
#include "FixTools.h"
#include "ColourPicker.h"
#include "VolumeDialog.h"
#include "MarkerStrip.h"
#include "FeedbackUi.h"

namespace td
{
/** How many different takes the edit is made from (a take used in several pieces counts once). */
static int takesInEdit (const EditDef& e) { std::set<juce::String> t; for (auto& r : e.regions) t.insert (r.takeId.toString()); return (int) t.size(); }
static constexpr int kInsH = 14;                                           // the strip with the flag that shows where the next take goes
static constexpr int kFbH = 14;                                            // the strip with the listeners' comment flags (its own line, under the markers)
static constexpr int kInsY = markerui::kStripH + kFbH;                     // where the insert strip starts
static constexpr int kRulerTop = kInsY + kInsH;                            // where the ruler's numbers start
static constexpr int kRulerH = 22 + kRulerTop, kLaneH = 38;                // (the top band: the marker strip, then the feedback strip, then the insert flag, then the ruler)
static int kNameW = 210;                        // width of the track-name column (drag its right edge to change it; shared by all edit windows)

class EditTimeline : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    EditTimeline (AppContext& a, const juce::Uuid& eid, juce::Viewport& vp)
        : app (a), editId (eid), viewport (vp)
    {
        formatManager.registerBasicFormats();
        setWantsKeyboardFocus (true);
        startTimerHz (30);
        app.project.addChangeListener (this);
    }
    ~EditTimeline() override { app.project.removeChangeListener (this); }

    double pixelsPerSecond = 12.0;
    int rowH = 52;                              // height of one track row (changed with the up / down arrow keys)
    juce::Uuid selectedRegion = juce::Uuid::null();
    /** Where edit point k stands on the timeline (k = 0 start of the first piece, k = n end of the last): the middle of the crossfade, or of the gap. */
    static juce::int64 editPointSample (const EditDef& e, int k)
    {
        const int n = (int) e.regions.size();
        if (n == 0) return 0;
        if (k <= 0) return (e.regions.front().fadeInBegin() + e.regions.front().fadeInFinish()) / 2;           // the middle of the fade, wherever it has been moved to
        if (k >= n) return (e.regions.back().fadeOutBegin() + e.regions.back().fadeOutFinish()) / 2;
        return (e.regions[(size_t) k - 1].fadeOutFinish() + e.regions[(size_t) k].fadeInBegin()) / 2;
    }
    juce::Uuid moveRegion;                     // the piece that Ctrl + click selected whole: it can be dragged sideways (until another click elsewhere)
    std::vector<juce::Uuid> selectedRegions;   // the pieces selected for Bounce Out (Ctrl + click adds more)
    bool isSelected (const juce::Uuid& id) const { return std::find (selectedRegions.begin(), selectedRegions.end(), id) != selectedRegions.end() || id == selectedRegion; }
    int selectedJoin = -1;                 // the selected edit point: 0 = the start of the edit, k = the join before regions[k], n = the end of the edit; -1 = none
    bool slipLeft = false, slipRight = false, shuffleMode = false;
    double cursorSeconds = -1.0;
    juce::String selectedFeedback;              // the key of the first comment of the flag that was clicked last

    EditDef* edit() const { return app.project.findEdit (editId); }

    /** One row of the window: the track / bus / VCA it belongs to, and which of that node's rows it is (0 = its own row, 1.. = the extra lane rows under it). */
    struct RowInfo { juce::Uuid node = juce::Uuid::null(); int lane = 0; NodeKind kind = NodeKind::None; int block = 0; };
    mutable std::vector<RowInfo> rowInfo;                              // (one for each of rowTracks(), in the same order)
    /** The rows of this edit, top to bottom. Without the Automation button: one row for every audio track (the edit's own list and order). With it: each audio track followed by
        its extra lane rows, then - under all the audio tracks - every Int Bus, Ext Bus and VCA, each followed by its extra lane rows. An extra lane row has no id (nothing but
        its lane lives there): rowInfo says which node it belongs to. Refreshed on every call, so it is never stale. */
    const std::vector<TrackDef>& rowTracks() const
    {
        rowsCache.clear(); rowInfo.clear();
        auto* e = edit();
        if (e == nullptr) return rowsCache;
        int block = 0;
        auto addNode = [&] (const TrackDef& t, NodeKind kind, bool withLanes)
        {
            const int n = withLanes ? e->laneRowCount (t.id) : 1;
            for (int k = 0; k < n; ++k)
            {
                TrackDef r = t; if (k > 0) r.id = juce::Uuid::null();
                rowsCache.push_back (r); rowInfo.push_back ({ t.id, k, kind, block });
            }
            ++block;
        };
        for (auto* t : app.project.editTracks (*e)) addNode (*t, NodeKind::Track, e->automationOn);
        if (e->automationOn)
        {
            for (int pass = 0; pass < 2; ++pass)                       // the Int Buses, then the Ext Buses
                for (auto& b : app.project.buses)
                    if (b.external == (pass == 1))
                    {
                        TrackDef t; t.id = b.id; t.name = b.name; t.format = b.external ? TrackFormat::Stereo : TrackFormat::Mono;      // (Mono: an Int Bus's pan is a single position, like a mono track's)
                        addNode (t, b.external ? NodeKind::ExtBus : NodeKind::IntBus, true);
                    }
            for (auto& v : app.project.vcas) { TrackDef t; t.id = v.id; t.name = "VCA " + v.label(); t.format = TrackFormat::Stereo; addNode (t, NodeKind::Vca, false); }
        }
        return rowsCache;
    }
    RowInfo infoOf (int row) const { rowTracks(); return juce::isPositiveAndBelow (row, (int) rowInfo.size()) ? rowInfo[(size_t) row] : RowInfo(); }
    /** An audio track's own row (the one with its audio): the only kind of row where pieces are selected, marked and moved and the track's name is picked up. */
    bool isTrackRow (int row) const { const auto i = infoOf (row); return i.kind == NodeKind::Track && i.lane == 0 && ! i.node.isNull(); }
    /** The row (of the audio tracks) at y: an extra lane row, a bus or a VCA row gives the audio track row it is not - this is the row of the track the mouse is nearest to above. */
    int trackRowAtY (int y) const
    {
        rowTracks();
        int row = juce::jlimit (0, juce::jmax (0, (int) rowInfo.size() - 1), (y - kRulerH - kLaneH) / juce::jmax (1, rowH));
        while (row > 0 && ! (rowInfo[(size_t) row].kind == NodeKind::Track && rowInfo[(size_t) row].lane == 0)) --row;
        return row;
    }
    /** The first row after the rows of the audio track whose own row is 'row' (its extra lane rows included). */
    int rowAfterBlock (int row) const { rowTracks(); int r = row + 1; while (r < (int) rowInfo.size() && rowInfo[(size_t) r].node == rowInfo[(size_t) row].node) ++r; return r; }
    bool lanesOn (const EditDef& e) const { return e.automationOn; }
    mutable std::vector<TrackDef> rowsCache;
    juce::Uuid clipMoveRegion, clipMoveTrack; int clipMoveToRow = -1;   // Shift + drag: one track's clip of a piece moves to another track, staying at the same time
    int rowDragFrom = -1, rowDragTo = -1;       // dragging a track's name up or down to move the track (the edit's order only)
    std::vector<juce::Uuid> selectedTracks;     // track names picked (click; Shift + click: every track in between; Ctrl + click: one more / less) - right-click: remove them
    juce::Uuid trackAnchor;
    bool trackSelected (const juce::Uuid& id) const { return std::find (selectedTracks.begin(), selectedTracks.end(), id) != selectedTracks.end(); }
    /** A click on a track's name: on its own = only this track (or none, if it was the only one); Shift = every track from the last one clicked to
        this one; Ctrl = this one in or out. */
    void clickTrackName (const juce::Uuid& id, const juce::ModifierKeys& mods)
    {
        const auto& rows = rowTracks();
        auto rowOfId = [&] (const juce::Uuid& t) { for (size_t i = 0; i < rows.size(); ++i) if (rows[i].id == t) return (int) i; return -1; };
        const bool ctrl = mods.isCtrlDown() || mods.isCommandDown();
        const int a = rowOfId (trackAnchor), b = rowOfId (id);
        if (mods.isShiftDown() && a >= 0 && b >= 0)
        {
            if (! ctrl) selectedTracks.clear();
            std::vector<juce::Uuid> add; for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i) add.push_back (rows[(size_t) i].id);
            for (auto& t : add) if (! trackSelected (t)) selectedTracks.push_back (t);
        }
        else if (ctrl)
        {
            if (trackSelected (id)) selectedTracks.erase (std::remove (selectedTracks.begin(), selectedTracks.end(), id), selectedTracks.end());
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

    /** The viewport would use the arrow keys to scroll: hand every key straight to the edit window instead (it zooms with them). */
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (auto* w = findParentComponentOfClass<EditWindowComponent>()) return w->keyPressed (k);
        return false;
    }

    /** Zooms in / out around the playhead (where it is playing, otherwise the cursor) and puts that point in the MIDDLE of the screen (as far as the start
        of the timeline allows: near the start the playhead stays toward the left, and drifts back there as you zoom out). If the playhead is off screen, the
        middle of what is shown is the centre instead. */
    void zoom (double f)
    {
        const int visW = viewport.getMaximumVisibleWidth();
        const int keep = kNameW + (visW - kNameW) / 2;
        double centre = app.playheadSeconds();
        if (! (centre >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)) centre = cursorSeconds;
        const int off = xOf (centre) - viewport.getViewPositionX();
        if (centre < 0 || off < kNameW || off > visW) centre = (double) (viewport.getViewPositionX() + keep - kNameW) / pixelsPerSecond;
        pixelsPerSecond = juce::jlimit (juce::jmin (1.0, juce::jmax (0.01, (double) (visW - kNameW) / timelineSeconds())), 800.0, pixelsPerSecond * f);   // out as far as the whole timeline fits
        updateSize();
        viewport.setViewPosition (juce::jmax (0, xOf (centre) - keep), viewport.getViewPositionY());
        repaint();
    }
    /** Scrolls sideways (if needed) so that this time can be seen. */
    void scrollTo (double seconds)
    {
        const int x = xOf (seconds), vx = viewport.getViewPositionX(), right = vx + viewport.getMaximumVisibleWidth() - 6;
        if (x < vx + kNameW || x > right) viewport.setViewPosition (juce::jmax (0, x - kNameW - 40), viewport.getViewPositionY());
    }
    /** C: scrolls so that the playhead (where it is playing, otherwise the cursor) is in the middle of the window; the zoom stays the same. */
    void centreOnPlayhead()
    {
        double t = app.playheadSeconds();
        if (! (t >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)) t = cursorSeconds;
        if (t < 0) return;
        const int visW = viewport.getMaximumVisibleWidth();
        viewport.setViewPosition (juce::jmax (0, xOf (t) - (kNameW + (visW - kNameW) / 2)), viewport.getViewPositionY());
    }
    /** Vertical zoom: taller or shorter track rows (factor > 1 = taller). */
    void zoomVertical (double f)
    {
        const int maxH = juce::jmax (60, viewport.getMaximumVisibleHeight() - kRulerH - kLaneH);
        rowH = juce::jlimit (12, maxH, (int) std::lround ((double) rowH * f + (f > 1.0 ? 1.0 : -1.0)));
        updateSize(); repaint();
    }

    /** How long the timeline is: the material (or the playhead / cursor, if that is further) and then as much empty room again (at least 10 minutes), so
        there is always space after the end to drag more audio onto. At most 24 hours. */
    double timelineSeconds()
    {
        auto* e = edit();
        const double content = juce::jmax (juce::jmax (0.0, e ? e->lengthSeconds() : 0.0, app.playheadSeconds()), juce::jmax (e ? e->playheadSeconds : 0.0, cursorSeconds));
        return juce::jmin (86400.0, content + juce::jmax (600.0, content));
    }
    void updateSize()
    {
        const double end = timelineSeconds();
        // always at least as big as the visible area, so the ruler, grid and empty rows fill the whole window
        const int contentW = (int) (end * pixelsPerSecond) + kNameW;
        const int contentH = kRulerH + kLaneH + juce::jmax (1, (int) rowTracks().size()) * rowH + 8;
        setSize (juce::jmax (contentW, viewport.getMaximumVisibleWidth()), juce::jmax (contentH, viewport.getMaximumVisibleHeight()));
    }

    /** The top band (markers, feedback flags, insert flag, ruler, piece names and edit points) stays at the top of the window however far you scroll down:
        the whole picture is painted once where it is, then the band is painted again over the top of the visible part. */
    void paint (juce::Graphics& g) override
    {
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
        if (paintprof::enabled()) paintprof::paintDone ("edit window", juce::Time::getMillisecondCounterHiRes() - tPaint0, g.getClipBounds().getWidth(), g.getClipBounds().getHeight());
    }
    // ---- Alt + drag: a lasso; every piece it touches is selected (whole pieces, overdubs too)
    bool lassoActive = false, lassoMoved = false; juce::Point<int> lassoFrom, lassoTo;
    juce::Rectangle<int> lassoRect() const
    {
        return juce::Rectangle<int>::leftTopRightBottom (juce::jmin (lassoFrom.x, lassoTo.x), juce::jmin (lassoFrom.y, lassoTo.y),
                                                         juce::jmax (lassoFrom.x, lassoTo.x) + 1, juce::jmax (lassoFrom.y, lassoTo.y) + 1);
    }
    /** The pieces (and overdubs) that 'box' touches: the rows of the tracks each one has audio on, over the time it covers. */
    std::vector<juce::Uuid> piecesIn (const EditDef& ed, juce::Rectangle<int> box) const
    {
        std::vector<juce::Uuid> out;
        const double rate = juce::jmax (1.0, ed.sampleRate);
        const auto& rt = rowTracks();
        std::map<juce::Uuid, int> rowOf;
        for (size_t i = 0; i < rt.size(); ++i) if (! rt[i].id.isNull()) rowOf[rt[i].id] = (int) i;
        const int top = kRulerH + kLaneH;
        auto touches = [&] (const EditRegion& r)
        {
            const int x0 = xOf ((double) r.startSample / rate), x1 = juce::jmax (x0 + 2, xOf ((double) r.endSample() / rate));
            if (x1 < box.getX() || x0 > box.getRight()) return false;
            for (auto& f : r.files)
                if (auto it = rowOf.find (f.trackId); it != rowOf.end() && juce::Rectangle<int> (x0, top + it->second * rowH, x1 - x0, rowH).intersects (box)) return true;
            return false;
        };
        for (auto& r : ed.regions)  if (touches (r)) out.push_back (r.id);
        for (auto& r : ed.overdubs) if (touches (r)) out.push_back (r.id);
        return out;
    }
    void applyLasso (const EditDef& ed)
    {
        selectedRegions = piecesIn (ed, lassoRect());
        selectedRegion = selectedRegions.size() == 1 ? selectedRegions.front() : juce::Uuid::null();      // (one piece: as if it had been clicked)
        moveRegion = juce::Uuid::null(); selectedJoin = -1;
    }
    // ---- the corner above the first track's name: what Ctrl / Alt / Shift do here, while they are held
    int shownMods = 0;
    juce::StringArray modifierHints (int bits) const
    {
        using namespace modhint;
        auto* e = edit();
        if (e != nullptr && lanesOn (*e))                      // automation showing: a click in a row works on the automation
            switch (bits)
            {
                case ctrl:         return { "Click an automation point: delete it", "Ctrl + Z / Ctrl + Y: undo / redo" };
                case shift:        return { "Click an automation point: delete it" };
                case alt:          return { "While dragging an automation point: fine steps" };
                case ctrl | shift: return { "Ctrl + Shift + Z: redo" };
                default:           return {};
            }
        switch (bits)
        {
            case ctrl:         return { "Click a piece: select it whole (then drag it to move it); Ctrl + click more pieces to add them", "Click a track's name: add it to the selection or take it out", "Ctrl + Z / Ctrl + Y: undo / redo" };
            case alt:  case alt | shift:
                               return { "Drag: lasso - selects every piece it touches" };
            case ctrl | alt:   return { "Drag over audio: mark 1 - 2 on only the tracks you drag over" };
            case shift:        return { "Drag a track's clip: move it to another track (same time)", "Click a track's name: select every track from the last one clicked" };
            case ctrl | alt | shift:
                               return { "Drag a track's clip: move it to another track (same time)" };
            case ctrl | shift: return { "Ctrl + Shift + Z: redo" };
            default:           return {};
        }
    }
    bool headerPass = false;
    int hdrShift = 0;                          // while the mouse works in the sticky band: how far down it has been moved
    int stickyShift (const juce::MouseEvent& e) const
    {
        const int sy = viewport.getViewPositionY();
        return (sy > 0 && e.y >= sy && e.y < sy + kRulerH + kLaneH) ? sy : 0;
    }
    juce::MouseEvent shifted (const juce::MouseEvent& e, int by) const { return by == 0 ? e : e.withNewPosition (e.position - juce::Point<float> (0.0f, (float) by)); }

    void paintAll (juce::Graphics& g)
    {
        g.fillAll (theme::wave);
        auto* e = edit();
        if (e == nullptr) return;
        const auto tracks = headerPass ? std::vector<TrackDef>() : rowTracks();
        const auto info = headerPass ? std::vector<RowInfo>() : rowInfo;               // (what each row is: see rowTracks)
        const int top = kRulerH + kLaneH;
        const int rowsEnd = top + juce::jmax (1, (int) tracks.size()) * rowH;

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            g.setColour ((info[i].block & 1) ? theme::waveAlt : theme::wave);          // (a track and its extra lane rows share a shade)
            g.fillRect (0, top + (int) i * rowH, getWidth(), rowH);
            theme::paintStaff (g, { 0, top + (int) i * rowH, getWidth(), rowH });                                         // (the Manuscript UI: staves)
        }
        for (int i = (int) tracks.size(), y = top + (int) tracks.size() * rowH, blk = info.empty() ? 0 : info.back().block + 1; y < getHeight(); ++i, ++blk, y += rowH)       // empty rows carry on to the bottom of the window
        {
            g.setColour (((blk & 1) ? theme::waveAlt : theme::wave).withMultipliedBrightness (0.97f));
            g.fillRect (0, y, getWidth(), rowH);
        }
        g.setColour (theme::ruler); g.fillRect (0, 0, getWidth(), kRulerH);
        g.setColour (juce::Colours::black.withAlpha (0.22f)); g.fillRect (0, 0, getWidth(), markerui::kStripH);          // the marker strip
        g.setColour (theme::manuscript() ? theme::accent.withAlpha (0.10f) : juce::Colour (0x2635b7c9)); g.fillRect (0, markerui::kStripH, getWidth(), kFbH);   // the feedback strip (its own line, tinted)
        const double step = pixelsPerSecond >= 200 ? 0.5 : pixelsPerSecond >= 60 ? 1.0 : pixelsPerSecond >= 24 ? 5.0 : pixelsPerSecond >= 8 ? 10.0 : pixelsPerSecond >= 3 ? 60.0 : 300.0;
        // fainter unlabelled lines between the labelled ones: 5 s -> every 1 s, 1 s -> every 0.5 s, then 0.25 s (two zoom steps), then 0.1 s however far in
        const double minor = pixelsPerSecond >= 450 ? 0.1 : pixelsPerSecond >= 200 ? 0.25 : pixelsPerSecond >= 60 ? 0.5 : pixelsPerSecond >= 24 ? 1.0 : pixelsPerSecond >= 8 ? 5.0 : pixelsPerSecond >= 3 ? 10.0 : 60.0;
        const int minorPer = juce::jmax (1, (int) std::lround (step / minor));
        for (int n = 0; (double) n * minor * pixelsPerSecond < getWidth(); ++n)
        {
            if (n % minorPer == 0) continue;
            g.setColour (theme::grid.withAlpha (0.45f)); g.drawVerticalLine (xOf ((double) n * minor), (float) kRulerH - 6, (float) getHeight());
        }
        g.setFont (11.0f);
        for (double s = 0; s * pixelsPerSecond < getWidth(); s += step)
        {
            const int x = xOf (s);
            g.setColour (theme::grid); g.drawVerticalLine (x, (float) kRulerH - 8, (float) getHeight());
            g.setColour (theme::dimText); g.drawText (formatTimeShort (s, 0), x + 3, 2 + kRulerTop, 60, 16, juce::Justification::left);
        }

        if (e->isEmpty())
        {
            g.setColour (theme::dimText); g.setFont (16.0f);
            g.drawText ("Empty. In a take window: press 1 and 2 to mark the part you want, then 3 (or the 'To edit' button).",
                        kNameW + 20, top + 10, 900, 24, juce::Justification::left);
        }

        const double rate = juce::jmax (1.0, e->sampleRate);
        // Only what is inside the clip is drawn: a piece wholly left or right of it is skipped, and so are the rows above and below it (the meters'
        // repaint, several times a second, is a narrow column: it must not cost as much as the whole picture).
        const auto clip = g.getClipBounds();
        const bool lanesVisible = clip.getY() < top;
        std::vector<juce::Colour> rowColour; for (auto& t : tracks) rowColour.push_back (chan::of (app.project, t.id));
        paintprof::Scope profPieces ("edit window", "pieces");
        for (size_t k = 0; k < e->regions.size(); ++k)
        {
            const auto& r = e->regions[k];
            const int x0 = xOf ((double) r.startSample / rate);
            const int wpx = juce::jmax (3, (int) ((double) r.length() / rate * pixelsPerSecond));
            const int fadeReach = (int) (juce::jmax (r.inEnd - r.inStart, r.outEnd - r.outStart) * pixelsPerSecond) + 60;      // (the fades and handles reach outside the piece)
            if (x0 - fadeReach > clip.getRight() || x0 + wpx + fadeReach < clip.getX()) continue;
            const bool selected = isSelected (r.id);

            auto lane = juce::Rectangle<int> (x0, kRulerH + 11, wpx, kLaneH - 13);   // below the flags, so the title has a line to itself
            if (lanesVisible)
            {
                const auto plate = selected ? juce::Colour (0xffc26500) : theme::accent;
                g.setColour (plate);
                g.fillRect (lane);
                g.setColour (theme::manuscript() ? theme::inkOn (plate) : juce::Colours::white); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
                g.drawText (r.takeName + "   " + formatTimeShort ((double) r.srcIn / rate, 2) + " - " + formatTimeShort ((double) r.srcOut / rate, 2),
                            lane.reduced (6, 0), juce::Justification::centredLeft, true);
                drawFadeHandles (g, lane, r, rate, k == 0, k + 1 == e->regions.size());
            }

            for (size_t ti = 0; ti < tracks.size(); ++ti)
            {
                if (tracks[ti].id.isNull()) continue;                                              // (an extra lane row: no audio)
                auto rr = juce::Rectangle<int> (x0, top + (int) ti * rowH + 3, wpx, rowH - 6);
                if (rr.getBottom() < clip.getY() || rr.getY() > clip.getBottom()) continue;       // the row is off the screen
                const RegionFile* rf = nullptr;
                for (auto& f : r.files) if (f.trackId == tracks[ti].id) rf = &f;
                if (rf == nullptr) continue;
                juce::int64 srcA = r.srcIn, srcB = r.srcOut;
                if (tracks[ti].isLtc)                                   // LTC is cut hard: from the first sample of the piece's fade-in, to one sample before the next piece's fade-in
                {
                    const auto lb = r.fadeInBegin(), le = ltcEndOf (r, k + 1 < e->regions.size() ? &e->regions[k + 1] : nullptr);
                    if (le <= lb) continue;
                    const int lx0 = xOf ((double) lb / rate);
                    rr = juce::Rectangle<int> (lx0, rr.getY(), juce::jmax (1, xOf ((double) le / rate) - lx0), rr.getHeight());
                    srcA = r.srcIn + (lb - r.startSample); srcB = r.srcIn + (le - r.startSample);
                }
                const auto tc = rowColour[ti];
                g.setColour (selected ? chan::clipFill (tc, true) : chan::clipFill (tc));
                g.fillRect (rr);
                if (r.waiting.active() && (r.waiting.tracks.empty() || std::find (r.waiting.tracks.begin(), r.waiting.tracks.end(), rf->trackId) != r.waiting.tracks.end()))
                    drawWaitingBlock (g, rr.reduced (1), r.waiting.name, theme::text);
                else
                {
                    setWaveColour (g, chan::waveColour (tc));
                    juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (rr.reduced (1));
                    const auto wcd = app.waveColourOf (rf->file);
                    if (! directWave.draw (g, rr.reduced (1), rf->file, (double) (srcA - rf->fileStart) / rate, (double) (srcB - rf->fileStart) / rate, app.waveZoom, wcd.get(), rate))
                        if (auto* th = thumbnailFor (rf->file)) drawThumbnailEnvelope (g, rr.reduced (1), *th, (double) (srcA - rf->fileStart) / rate, (double) (srcB - rf->fileStart) / rate, app.waveZoom, wcd.get());
                }
                g.setColour (selected ? juce::Colour (0xffc26500) : tc.withAlpha (0.9f));         // the selected piece gets an orange frame
                g.drawRect (rr, selected ? 2 : 1);
            }

            // this piece's fades: fade-in (yellow) and fade-out (orange)
            auto shade = [&] (juce::int64 a, juce::int64 b, juce::Colour c)
            {
                if (b <= a) return;
                g.setColour (c);
                g.fillRect (juce::Rectangle<int> (xOf ((double) a / rate), top, juce::jmax (2, xOf ((double) b / rate) - xOf ((double) a / rate)), rowsEnd - top));
            };
            shade (r.fadeInBegin(), r.fadeInFinish(), selectedJoin == (int) k ? juce::Colour (0x77ffd24a) : juce::Colour (0x44ffd24a));
            shade (r.fadeOutBegin(), r.fadeOutFinish(), selectedJoin == (int) k + 1 ? juce::Colour (0x77ff9d2e) : juce::Colour (0x44ff9d2e));
        }

        profPieces.next ("edit points and the rest");
        // The edit points: every fade of the edit, numbered in order along the timeline. Edit 1 is where the first piece starts, the last one is where
        // the final piece fades out, and each join between two pieces is one edit. (Overdubs do not count.) The flag stands in the middle of the crossfade
        // (or of the gap, when the fades do not overlap). Click one to select it; double-click or T opens the Trim window on it.
        int editNumber = 0;                                                                     // joins made by an offline fix (pitch, repair, de-click, export for processing) are not edits: no number
        for (int k = 0; k <= (int) e->regions.size() && ! e->regions.empty(); ++k)
        {
            const int x = xOf ((double) editPointSample (*e, k) / rate);
            const bool js = selectedJoin == k;
            if (e->isFixJoin (k))
            {
                g.setColour (js ? juce::Colour (0xffc26500) : theme::dimText.withAlpha (0.7f));
                g.fillRect (x, kRulerH, 1, rowsEnd - kRulerH);
                juce::Path tri; tri.addTriangle ((float) x - 4, (float) kRulerH, (float) x + 4, (float) kRulerH, (float) x, (float) kRulerH + 6);
                g.fillPath (tri);
                const auto badge = juce::Rectangle<int> (x - 8, kRulerH - 13, 16, 11);          // a small grey "fx" tag instead of a number
                g.setColour (js ? juce::Colour (0xffc26500) : theme::dimText.withAlpha (0.55f)); g.fillRoundedRectangle (badge.toFloat(), 3.0f);
                g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
                g.drawText ("fx", badge, juce::Justification::centred);
                continue;
            }
            ++editNumber;
            g.setColour (js ? juce::Colour (0xffc26500) : theme::text);
            g.fillRect (x - 1, kRulerH, 2, rowsEnd - kRulerH);
            juce::Path tri; tri.addTriangle ((float) x - 6, (float) kRulerH, (float) x + 6, (float) kRulerH, (float) x, (float) kRulerH + 9);
            g.fillPath (tri);
            const auto badge = juce::Rectangle<int> (x - 11, kRulerH - 16, 22, 15);              // the number, above the flag
            g.setColour (js ? juce::Colour (0xffc26500) : theme::accent); g.fillRoundedRectangle (badge.toFloat(), 3.0f);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (juce::String (editNumber), badge, juce::Justification::centred);
        }

        // overdubs: narrower pieces in the middle of each track row, laid over the edit
        for (auto& r : e->overdubs)
        {
            const int x0 = xOf ((double) r.startSample / rate);
            const int wpx = juce::jmax (3, (int) ((double) r.length() / rate * pixelsPerSecond));
            const bool selected = isSelected (r.id);
            auto lane = juce::Rectangle<int> (x0, kRulerH + 14, wpx, kLaneH - 18);
            g.setColour (selected ? juce::Colour (0xffc26500) : juce::Colour (0xff6a4cc2)); g.fillRect (lane);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText ("OVERDUB  " + r.takeName, lane.reduced (4, 0), juce::Justification::centredLeft, true);
            drawFadeHandles (g, lane, r, rate, true, true);
            for (size_t ti = 0; ti < tracks.size(); ++ti)
            {
                if (tracks[ti].id.isNull()) continue;                                              // (an extra lane row: no audio)
                const RegionFile* rf = nullptr;
                for (auto& f : r.files) if (f.trackId == tracks[ti].id) rf = &f;
                if (rf == nullptr) continue;
                auto rr = overdubBand (top + (int) ti * rowH, x0, wpx);
                const auto tc = chan::of (app.project, tracks[ti].id);
                g.setColour (selected ? chan::clipFill (tc, true) : chan::clipFill (tc)); g.fillRect (rr);
                {
                    setWaveColour (g, chan::waveColour (tc));
                    juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (rr.reduced (1));
                    const auto wcd = app.waveColourOf (rf->file);
                    if (! directWave.draw (g, rr.reduced (1), rf->file, (double) (r.srcIn - rf->fileStart) / rate, (double) (r.srcOut - rf->fileStart) / rate, app.waveZoom, wcd.get(), rate))
                        if (auto* th = thumbnailFor (rf->file)) drawThumbnailEnvelope (g, rr.reduced (1), *th, (double) (r.srcIn - rf->fileStart) / rate, (double) (r.srcOut - rf->fileStart) / rate, app.waveZoom, wcd.get());
                }
                g.setColour (selected ? juce::Colour (0xffc26500) : juce::Colour (0xff6a4cc2)); g.drawRect (rr, selected ? 2 : 1);
            }
        }
        // volume changes: a teal line where each one starts, with the glide shaded
        {
            auto marks = [&] (const EditRegion& r)
            {
                for (auto& gc : r.gains)
                {
                    if (gc.at == 0 && gc.ramp == 0.0) { g.setColour (juce::Colour (0xff1aa39a)); g.fillRect (xOf ((double) r.startSample / rate), kRulerH + kLaneH - 4, 7, 3); continue; }
                    const int x = xOf ((double) (r.startSample + gc.at) / rate);
                    const int gw = (int) (gc.ramp * pixelsPerSecond);
                    if (gw > 1) { g.setColour (juce::Colour (0x331aa39a)); g.fillRect (x, top, gw, rowsEnd - top); }
                    g.setColour (juce::Colour (0xff1aa39a)); g.fillRect (x, top, 1, rowsEnd - top);
                    juce::Path tri; tri.addTriangle ((float) x - 4, (float) top, (float) x + 5, (float) top, (float) x, (float) top + 7); g.fillPath (tri);
                }
            };
            for (auto& r : e->regions)  marks (r);
            for (auto& r : e->overdubs) marks (r);
        }

        // where the next take will be placed: a flag in its own strip under the markers (drag it; it snaps to the starts and ends of the pieces)
        if (app.showInsertFlag)
        {
            const juce::int64 at = e->insertIndex < 0 || e->insertIndex >= (int) e->regions.size()
                                       ? (e->regions.empty() ? 0 : e->regions.back().endSample()) : e->regions[(size_t) e->insertIndex].startSample;
            int x = xOf ((double) at / rate);
            bool snapped = true;
            if (insertDrag) { x = insertShownX; snapped = insertSnapped; }
            const int sy = kInsY;
            g.setColour (juce::Colours::black.withAlpha (0.12f)); g.fillRect (0, sy, getWidth(), kInsH);
            const auto orange = juce::Colour (0xffc26500);
            g.setColour (snapped ? orange : orange.withAlpha (0.55f));
            for (int y = sy + kInsH; y < rowsEnd; y += 8) g.fillRect (x - 1, y, 2, 4);
            g.fillRect (x - 1, sy, 2, kInsH);
            auto flag = juce::Rectangle<int> (x + 1, sy + 1, 52, kInsH - 3);
            g.fillRect (flag);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawText ("INSERT", flag, juce::Justification::centred, false);
            g.setColour (snapped ? orange : orange.withAlpha (0.55f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("next take goes here", x + 4, rowsEnd - 16, 130, 14, juce::Justification::left);
        }
        if (shownPhX != kNoLine)                                     // the moving playhead, where movePlayhead last put it
            theme::paintPlayhead (g, shownPhX, kRulerH, rowsEnd - kRulerH);
        if (cursorSeconds >= 0)
        {
            g.setColour (theme::text); g.fillRect (xOf (cursorSeconds), kRulerH, 1, rowsEnd - kRulerH);
        }
        if (e->playheadSeconds > 0)                                  // where key 4 / an overdub in the take window will be placed
        {
            const int x = xOf (e->playheadSeconds);
            g.setColour (juce::Colour (0xff6a4cc2)); g.fillRect (x, kRulerTop, 1, kRulerH - kRulerTop);
            juce::Path tri; tri.addTriangle ((float) x - 5, (float) kRulerTop, (float) x + 6, (float) kRulerTop, (float) x, (float) kRulerTop + 9.0f); g.fillPath (tri);
        }
        if (e->fixIn >= 0 || e->fixOut >= 0)                         // keys 1 / 2: the part to correct or repair
        {
            if (e->fixIn >= 0 && e->fixOut > e->fixIn)
            {
                g.setColour (juce::Colour (0x22ffa21f));
                if (e->fixTracks.empty()) g.fillRect (xOf (e->fixIn), kRulerH, xOf (e->fixOut) - xOf (e->fixIn), rowsEnd - kRulerH);
                else                                                                          // Alt + drag: only the rows of the chosen tracks
                {
                    g.fillRect (xOf (e->fixIn), kRulerH, xOf (e->fixOut) - xOf (e->fixIn), kLaneH);
                    for (size_t i = 0; i < tracks.size(); ++i)
                        if (e->fixUses (tracks[i].id)) { g.setColour (juce::Colour (0x44ffa21f)); g.fillRect (xOf (e->fixIn), top + (int) i * rowH, xOf (e->fixOut) - xOf (e->fixIn), rowH); }
                }
            }
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            if (e->fixIn >= 0)  { g.setColour (juce::Colour (0xff1e6fd9)); g.fillRect (xOf (e->fixIn), 0, 2, rowsEnd);  g.drawText ("1",  xOf (e->fixIn) + 4, 16, 30, 14, juce::Justification::left); }
            if (e->fixOut >= 0) { g.setColour (juce::Colour (0xffe08a00)); g.fillRect (xOf (e->fixOut) - 1, 0, 2, rowsEnd); g.drawText ("2", xOf (e->fixOut) - 34, 16, 30, 14, juce::Justification::right); }
        }

        if (lanesOn (*e))
            for (size_t i = 0; i < tracks.size(); ++i) paintLane (g, *e, info[i], top + (int) i * rowH);
        if (lanesOn (*e) && autoDragIndex >= 0)                                    // the value of the point being set or moved, in a little box beside it
            if (auto* L = e->findLane (autoDragLane); L != nullptr && juce::isPositiveAndBelow (autoDragIndex, (int) L->pts.size()))
            {
                const auto& pt = L->pts[(size_t) autoDragIndex];
                const double rt = juce::jmax (1.0, e->sampleRate);
                const int rowTop = top + autoDragRow * rowH;
                const juce::String txt = autoValueText (*L, pt.value) + "   " + juce::String ((double) pt.time / rt, 2) + " s";
                const int bw = 120, bh = 20;
                const float cx = (float) xOf ((double) pt.time / rt), cy = yOfValue (*L, pt.value, rowTop);
                auto box = juce::Rectangle<float> ((float) bw, (float) bh).withCentre ({ cx + 14.0f + (float) bw * 0.5f, cy - 18.0f });
                if (box.getRight() > (float) getWidth() - 2.0f) box.setX (cx - 14.0f - (float) bw);
                if (box.getY() < (float) rowTop) box.setY ((float) rowTop + 2.0f);
                g.setColour (juce::Colour (0xf0101418)); g.fillRoundedRectangle (box, 4.0f);
                g.setColour (juce::Colour (0xffffc83d)); g.drawRoundedRectangle (box, 4.0f, 1.2f);
                g.setFont (juce::FontOptions (12.5f, juce::Font::bold)); g.setColour (juce::Colours::white);
                g.drawText (txt, box.toNearestInt(), juce::Justification::centred, false);
            }

        const int sx = viewport.getViewPositionX();
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& inf = info[i];
            const int rowTop = top + (int) i * rowH;
            const bool extra = inf.lane > 0;                                                // one of the lane rows under a track / bus: no audio, no name picking
            const bool trackRow = inf.kind == NodeKind::Track && ! extra;
            auto r = juce::Rectangle<int> (sx, rowTop, kNameW, rowH);
            const auto tc = chan::of (app.project, inf.node);
            g.setColour (theme::panel); g.fillRect (r);
            g.setColour (tc.withAlpha (extra ? 0.12f : 0.20f)); g.fillRect (r);
            g.setColour (tc); g.fillRect (r.removeFromLeft (extra ? 3 : 6));
            g.setColour (tc.darker (0.3f).withAlpha (0.8f)); g.fillRect (sx, rowTop + rowH - 1, kNameW, 1);
            if (trackRow && trackSelected (inf.node))                                       // a selected name (right-click: remove the selected tracks)
            {
                const auto sel = juce::Rectangle<int> (sx, rowTop, kNameW, rowH);
                g.setColour (juce::Colour (0xff29b6ff).withAlpha (0.22f)); g.fillRect (sel);
                g.setColour (juce::Colour (0xff29b6ff)); g.drawRect (sel.reduced (1), 2);
            }
            g.setColour (theme::text); g.setFont (14.0f);
            if (lanesOn (*e))
            {
                const bool boxes = inf.kind != NodeKind::Vca || extra;                      // + and - boxes: not for a VCA (its fader is all it has)
                auto nameArea = r.reduced (8, 0);
                nameArea.removeFromBottom (juce::jmin (20, nameArea.getHeight() / 2 + 2));      // (the label and padlock live there)
                if (extra)                                                                  // an extra lane row: the owner's name, small and dim, so it is clear whose lane it is
                {
                    g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.5f));
                    g.drawText (tracks[i].name, nameArea.withTrimmedRight (60).withHeight (juce::jmin (18, nameArea.getHeight())).translated (0, 2), juce::Justification::centredLeft, true);
                    g.setColour (theme::text);
                }
                else
                    g.drawFittedText (tracks[i].name, nameArea.withTrimmedRight (rowH >= 40 || ! boxes ? 8 : 56).withTrimmedLeft (rowH >= 40 ? 0 : 42).withTrimmedTop (rowH >= 40 ? 20 : 0), juce::Justification::centredLeft, 2, 1.0f);
                const auto shown = e->laneShown (inf.node, inf.lane);
                const auto* lane = e->findLane (inf.node, shown.param, shown.mixer);
                const bool locked = lane != nullptr && lane->locked;
                auto lockR = lockRect (sx, rowTop);
                auto labelR = paramRect (sx, rowTop);
                g.setColour (theme::border); g.drawRoundedRectangle (labelR.toFloat(), 3.0f, 1.0f);
                g.setColour (theme::text); g.setFont (juce::FontOptions (12.0f));
                g.drawText (paramLabel (*e, inf.node, inf.lane) + "  v", labelR.reduced (4, 0), juce::Justification::centredLeft, true);
                drawPadlock (g, lockR.toFloat(), locked, locked ? juce::Colour (0xffe0b800) : theme::dimText);
                if (boxes)
                {
                    const bool canTake = extra || e->laneRowCount (inf.node) > 1;           // - : takes this row away (on the node's own row: the last extra row)
                    if (! extra) drawLaneBox (g, plusRect (sx, rowTop), true, true);
                    drawLaneBox (g, minusRect (sx, rowTop), false, canTake);
                }
            }
            else
            {
                g.drawFittedText (tracks[i].name, r.reduced (8, 2).withTrimmedRight (8).withTrimmedLeft (rowH >= 40 ? 0 : 42).withTrimmedTop (rowH >= 40 ? 20 : 0).withTrimmedBottom (rowH >= 40 ? 10 : 0), juce::Justification::centredLeft, 2, 1.0f);
                paintStripBits (g, tracks[i].id, sx, rowTop, rowH >= 40);
            }
            if (lanesOn (*e)) paintStripBits (g, tracks[i].id, sx, rowTop, false);
        }
        if (e->showMarkers) markerui::paintFlags (g, e->markers, [this] (double s) { return xOf (s); }, sx + kNameW, getHeight(), markers.draggedId());
        if (e->showFeedback && ! e->feedback.empty()) feedbackui::paintFlags (g, e->feedback, [this] (double s) { return xOf (s); }, markerui::kStripH, sx + kNameW, getHeight(), selectedFeedback);
        g.setColour (juce::Colour (0x55ffffff)); g.fillRect (sx + kNameW - 2, kRulerH + kLaneH, 2, juce::jmax (0, getHeight() - kRulerH - kLaneH));
        if (! clipMoveRegion.isNull() && clipMoveToRow >= 0)                           // Shift + drag: the row the clip will go to
        {
            g.setColour (juce::Colour (0xffffc83d)); g.drawRect (juce::Rectangle<int> (sx + kNameW, top + clipMoveToRow * rowH, getWidth() - sx - kNameW, rowH), 3);
        }
        if (rowDragFrom >= 0 && rowDragTo >= 0)                                        // where the dragged track will land
        {
            const int y = top + (rowDragTo > rowDragFrom ? rowAfterBlock (rowDragTo) : rowDragTo) * rowH;      // (below the track's lane rows when it moves down)
            g.setColour (juce::Colour (0xffffc83d)); g.fillRect (sx, y - 2, getWidth() - sx, 4);
        }
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, rowsEnd, kNameW, juce::jmax (0, getHeight() - rowsEnd));
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, 0, kNameW, top);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));                                                       // what each line of the band is
        g.setColour (e->showMarkers ? markerui::flagColour() : theme::dimText.withAlpha (0.6f));
        g.drawText (e->showMarkers ? "Markers" : "Markers (hidden)", sx + 6, 0, kNameW - 12, markerui::kStripH, juce::Justification::centredLeft, true);
        g.setColour (e->showFeedback ? juce::Colour (0xff35b7c9) : theme::dimText.withAlpha (0.6f));
        g.drawText (e->showFeedback ? "Feedback" : "Feedback (hidden)", sx + 6, markerui::kStripH, kNameW - 12, kFbH, juce::Justification::centredLeft, true);
        if (shownMods != 0 && (headerPass || viewport.getViewPositionY() == 0))         // the keys being held: what they do here (under those two lines)
            modhint::paint (g, { sx, markerui::kStripH + kFbH, kNameW, top - markerui::kStripH - kFbH }, shownMods, modifierHints (shownMods));
    }

    void mouseDown (const juce::MouseEvent& e0) override { hdrShift = stickyShift (e0); mouseDownImpl (shifted (e0, hdrShift)); }
    void mouseDrag (const juce::MouseEvent& e0) override { mouseDragImpl (shifted (e0, hdrShift)); }
    void mouseUp (const juce::MouseEvent& e0) override { const int by = hdrShift; hdrShift = 0; mouseUpImpl (shifted (e0, by)); }
    void mouseMove (const juce::MouseEvent& e0) override { mouseMoveImpl (shifted (e0, stickyShift (e0))); }

    void mouseDownImpl (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();
        dragging = false; markDrag = false; markActive = false; lassoActive = lassoMoved = false;
        auto* ed = this->edit();
        if (ed == nullptr) return;
        markers.setNameWidth (viewport.getViewPositionX() + kNameW);
        if (e.y >= markerui::kStripH && e.y < markerui::kStripH + kFbH && e.x > viewport.getViewPositionX() + kNameW && ed->showFeedback && ! ed->feedback.empty())          // a comment's flag: click to read the thread
        {
            auto xo = [this] (double s) { return xOf (s); };
            const auto th = feedbackui::threadAt (ed->feedback, e.x, xo);
            if (! th.empty())
            {
                selectedFeedback = ed->feedback[(size_t) th.front()].key; repaint();
                const int fx = xOf (ed->feedback[(size_t) th.front()].seconds);
                const auto area = localAreaToGlobal (juce::Rectangle<int> (fx, markerui::kStripH + hdrShift, feedbackui::flagWidth (feedbackui::flagText (ed->feedback, th)), kFbH));
                juce::Component::SafePointer<EditTimeline> safe (this);
                feedbackui::showThread ([this] { return edit(); }, th, area,
                                        [this, safe] { if (safe == nullptr) return; app.project.markDirty(); repaint(); },
                                        [this, safe] (double s) { if (safe == nullptr) return; if (auto* w = findParentComponentOfClass<EditWindowComponent>()) w->goToSeconds (s); });
                return;
            }
        }
        markers.yOffset = hdrShift;
        if (ed->showMarkers && markers.mouseDown (e)) return;              // the marker strip along the top: right-click for Add Marker / Clear All; drag a marker
        if (app.showInsertFlag && ! e.mods.isPopupMenu() && e.x >= kNameW && e.y >= kInsY && e.y < kInsY + kInsH)       // the insert flag: drag it to where the next take should go
        {
            insertDrag = true; insertDragMove (e.x);
            return;
        }
        const double rate = juce::jmax (1.0, ed->sampleRate);
        if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown() && ! e.mods.isPopupMenu() && ! (e.x > kNameW && e.y >= kRulerH + kLaneH))   // a click anywhere but on the audio lets go of the selected pieces
        { selectedRegion = juce::Uuid::null(); selectedRegions.clear(); moveRegion = juce::Uuid::null(); repaint(); }
        {
            const int sxv = viewport.getViewPositionX();
            if (std::abs (e.x - (sxv + kNameW)) <= 4 && e.y >= kRulerH && ! e.mods.isPopupMenu()) { widthDrag = true; return; }
            if (e.y >= kRulerH + kLaneH && ! e.mods.isPopupMenu() && e.x >= sxv && e.x < sxv + kNameW)
            {
                const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
                if (juce::isPositiveAndBelow (row, (int) rowTracks().size()))
                {
                    const int rowTop = kRulerH + kLaneH + row * rowH;
                    if (const auto ms = msOf (rowTracks()[(size_t) row].id); ms.mute != nullptr)
                    {
                        if (muteRect (sxv, rowTop).expanded (1).contains (e.getPosition())) { ms.mute->set (! ms.mute->get()); app.project.markDirty(); repaint(); return; }
                        if (soloRect (sxv, rowTop).expanded (1).contains (e.getPosition())) { ms.solo->set (! ms.solo->get()); app.project.markDirty(); repaint(); return; }
                    }
                }
            }
        }
        if (lanesOn (*ed) && automationMouseDown (e, *ed)) return;
        if (e.mods.isPopupMenu() && e.x >= viewport.getViewPositionX() && e.x < viewport.getViewPositionX() + kNameW && e.y >= kRulerH + kLaneH)   // right-click on a track name: colour, remove
        {
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) rowTracks().size()))
            {
                const auto id = rowTracks()[(size_t) row].id;
                const auto name = rowTracks()[(size_t) row].name;
                const auto pos = juce::Desktop::getMousePosition();
                const auto where = juce::Rectangle<int> (pos, pos).expanded (4);
                if (! isTrackRow (row)) { if (! id.isNull()) showColourPickerAt (app, id, where); return; }      // (a bus / VCA row: only its colour; an extra lane row: nothing)
                juce::PopupMenu m;
                m.addItem ("Colour...", [this, id, where] { showColourPickerAt (app, id, where); });
                m.addSeparator();
                juce::Component::SafePointer<EditTimeline> safe (this);
                auto removeIds = [safe] (std::vector<juce::Uuid> ids)
                {
                    if (safe == nullptr) return;
                    if (auto* w = safe->findParentComponentOfClass<EditWindowComponent>())
                    {
                        w->removeTracks (ids);
                        safe->selectedTracks.clear(); safe->updateSize(); safe->repaint();
                    }
                };
                if (trackSelected (id) && selectedTracks.size() > 1)
                {
                    const auto ids = selectedTracks;
                    m.addItem ("Remove the " + juce::String ((int) ids.size()) + " selected tracks from this edit (their audio leaves it; Undo brings it back)", [removeIds, ids] { removeIds (ids); });
                    m.addItem ("Select none", [this] { selectedTracks.clear(); repaint(); });
                    m.addSeparator();
                }
                m.addItem ("Remove track '" + name + "' from this edit (its audio leaves it; Undo brings it back)", [removeIds, id] { removeIds ({ id }); });
                m.showMenuAsync (juce::PopupMenu::Options());
            }
            return;
        }
        if (e.y >= kRulerH + kLaneH)                                       // an extra lane row, a bus or a VCA has no audio to select or move and no name to pick up
            if (const int rr = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH); juce::isPositiveAndBelow (rr, (int) rowTracks().size()) && ! isTrackRow (rr)) return;
        if (! e.mods.isPopupMenu() && e.x >= viewport.getViewPositionX() && e.x < viewport.getViewPositionX() + kNameW && e.y >= kRulerH + kLaneH)   // a track's name: drag it to another row, or click it to select it
        {
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) rowTracks().size()))
            {
                if (e.mods.isShiftDown() || e.mods.isCtrlDown() || e.mods.isCommandDown()) { clickTrackName (rowTracks()[(size_t) row].id, e.mods); return; }
                rowDragFrom = row; rowDragTo = row; return;
            }
        }
        EditRegion* fadeHit = nullptr; bool fadeIn = true;
        if (! e.mods.isPopupMenu() && e.mods.isAltDown() && ! (e.mods.isCommandDown() || e.mods.isCtrlDown())
            && e.y >= kRulerH + kLaneH && e.x >= viewport.getViewPositionX() + kNameW
            && ! fadeHandleAt (*ed, e.getPosition(), rate, fadeHit, fadeIn))                 // Alt + drag: a lasso (the playhead stays where it is; a fade corner is still a fade)
        {
            lassoActive = true; lassoMoved = false; lassoFrom = lassoTo = e.getPosition();
            return;
        }
        const double t = (e.x - kNameW) / pixelsPerSecond;
        cursorSeconds = juce::jmax (0.0, t);
        if (e.x > kNameW && e.y >= kRulerH && ! e.mods.isPopupMenu()) ed->playheadSeconds = cursorSeconds;     // the edit's own playhead: key 4 in a take window places a piece here
        if (e.x > kNameW && e.y < kRulerH && ! e.mods.isPopupMenu() && app.isPlaying() && ! app.engine.isRecording()       // clicking the ruler while this edit plays: carry on from there
            && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)
        {
            const auto err = app.playEdit (editId, cursorSeconds);
            if (err.isNotEmpty()) showError ("Play", err);
            repaint(); return;
        }
        if (e.mods.isPopupMenu() && e.x > kNameW && e.y >= kRulerH + kLaneH) { showPieceMenu (e, *ed, t, rate); return; }

        // the top corners at the two ends of a piece: drag the left one right for a longer fade-in, the right one left for a longer fade-out
        {
            EditRegion* fr = nullptr; bool isIn = true;
            if (e.x > kNameW && fadeHandleAt (*ed, e.getPosition(), rate, fr, isIn))
            {
                if (app.isPlaying()) app.stopPlayback();
                fadeDrag = isIn ? 1 : 2; fadeId = fr->id;
                fadeBase = isIn ? juce::jmax (0.0, fr->inEnd) : juce::jmax (0.0, -fr->outStart);
                selectedRegion = fr->id; selectedRegions = { fr->id }; selectedJoin = -1;
                repaint(); return;
            }
        }

        // a click on a join marker selects the join (double-click, or the T key, opens the trim window)
        int bestJoin = -1; double bestDist = 8.0;
        if (e.y < kRulerH + 14)                                     // the flags are at the top (and their numbers just above)
            for (int k = 0; k <= (int) ed->regions.size() && ! ed->regions.empty(); ++k)
            {
                const double d = std::abs (xOf ((double) editPointSample (*ed, k) / rate) - e.x);
                if (d < bestDist) { bestDist = d; bestJoin = k; }
            }
        if (bestJoin >= 0 && e.x > kNameW && e.y >= kRulerH - 16)
        {
            const bool atEnd = bestJoin >= (int) ed->regions.size();
            selectedJoin = bestJoin; selectedRegion = ed->regions[(size_t) (atEnd ? bestJoin - 1 : bestJoin)].id;
            if (e.getNumberOfClicks() > 1 && app.showTrim) app.showTrim (editId, selectedRegion, atEnd);
            repaint(); return;
        }
        selectedJoin = -1;

        // the ruler sets where the next take goes (the nearest join)
        if (e.y < kRulerH)
        {
            int bestIdx = (int) ed->regions.size(); double bd = std::abs (xOf ((double) ed->lengthSamples() / rate) - e.x);
            for (size_t k = 0; k < ed->regions.size(); ++k)
            {
                const double d = std::abs (xOf ((double) ed->regions[k].startSample / rate) - e.x);
                if (d < bd) { bd = d; bestIdx = (int) k; }
            }
            ed->insertIndex = bestIdx >= (int) ed->regions.size() ? -1 : bestIdx;
            app.project.changed();
            repaint(); return;
        }

        // the banner at the top of a piece: drag it along to shuffle it past its neighbours, like swapping cards in a deck
        if (! e.mods.isPopupMenu() && e.getNumberOfClicks() < 2 && e.x > kNameW && e.y >= kRulerH + 14 && e.y < kRulerH + kLaneH - 2 && ed->regions.size() > 1 && overdubAt (*ed, e.getPosition(), rate) == nullptr)
        {
            const juce::int64 sm = (juce::int64) (juce::jmax (0.0, t) * rate);
            for (auto& r : ed->regions)
                if (sm >= r.startSample && sm < r.endSample())
                {
                    shuffleId = r.id; shuffleGrab = sm - r.startSample; shuffleSample = sm; shuffleMoved = false; shuffleStartX = e.x;
                    selectedRegion = r.id; selectedRegions = { r.id }; selectedJoin = -1;
                    repaint(); return;
                }
        }

        // Shift + drag on a clip of one track: move that clip to another track (it keeps its place on the timeline)
        if (e.mods.isShiftDown() && ! e.mods.isPopupMenu() && e.x > kNameW && e.y >= kRulerH + kLaneH)
        {
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            const auto& rt = rowTracks();
            if (juce::isPositiveAndBelow (row, (int) rt.size()))
            {
                const juce::int64 sm = (juce::int64) (juce::jmax (0.0, t) * rate);
                EditRegion* hit = overdubAt (*ed, e.getPosition(), rate);
                if (hit == nullptr) for (auto& r : ed->regions) if (sm >= r.startSample && sm < r.endSample()) hit = &r;
                if (hit != nullptr)
                {
                    bool has = false; for (auto& f : hit->files) if (f.trackId == rt[(size_t) row].id) has = true;
                    if (has)
                    {
                        if (app.isPlaying()) app.stopPlayback();
                        clipMoveRegion = hit->id; clipMoveTrack = rt[(size_t) row].id; clipMoveToRow = row;
                        selectedRegion = hit->id; selectedRegions = { hit->id }; selectedJoin = -1;
                        setMouseCursor (juce::MouseCursor::DraggingHandCursor); repaint(); return;
                    }
                }
            }
        }

        // otherwise select the piece under the mouse and start sliding it
        const bool trackMark = (e.mods.isCommandDown() || e.mods.isCtrlDown()) && e.mods.isAltDown();     // Ctrl + Alt + drag: marks on chosen tracks only
        const bool additive = (e.mods.isCommandDown() || e.mods.isCtrlDown()) && ! trackMark;
        const auto previous = selectedRegion;
        selectedRegion = juce::Uuid::null();
        const juce::int64 sample = (juce::int64) (juce::jmax (0.0, t) * rate);
        for (auto& r : ed->regions) if (sample >= r.startSample && sample < r.endSample()) selectedRegion = r.id;
        if (auto* od = overdubAt (*ed, e.getPosition(), rate)) selectedRegion = od->id;          // an overdub sits on top
        // A plain drag over audio always marks a part of it: mark 1 where the mouse went down, mark 2 where it is let go.
        // To MOVE a piece, Ctrl + click it (that selects the whole piece); once it is selected the Ctrl key can be let go and the piece dragged sideways.
        const bool hitArmed = ! selectedRegion.isNull() && selectedRegion == moveRegion;
        const bool canMove = (additive || hitArmed) && ! trackMark;
        const bool wantMarkDrag = ! canMove && e.x > kNameW && e.y >= kRulerH + kLaneH && ! e.mods.isPopupMenu() && e.getNumberOfClicks() < 2;
        if (! additive && ! hitArmed) moveRegion = juce::Uuid::null();
        if (additive)                                        // Ctrl + click: select the whole piece (more than one for 'Selected' in Bounce Out) and arm it for moving
        {
            if ((! previous.isNull()) && std::find (selectedRegions.begin(), selectedRegions.end(), previous) == selectedRegions.end()) selectedRegions.push_back (previous);
            if ((! selectedRegion.isNull()))
            {
                auto it = std::find (selectedRegions.begin(), selectedRegions.end(), selectedRegion);
                if (it != selectedRegions.end()) { selectedRegions.erase (it); selectedRegion = juce::Uuid::null(); }
                else selectedRegions.push_back (selectedRegion);
            }
            moveRegion = selectedRegion;
            if (selectedRegion.isNull()) { repaint(); return; }
        }
        else
        {
            selectedRegions.clear();
            if ((! selectedRegion.isNull())) selectedRegions.push_back (selectedRegion);
        }
        if (! selectedRegion.isNull())                                   // show where this piece's file is in the Media window
            if (auto* sr = ed->findAny (selectedRegion))
            {
                const int row = e.y >= kRulerH + kLaneH ? (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH) : -1;
                const juce::Uuid tid = juce::isPositiveAndBelow (row, (int) rowTracks().size()) ? rowTracks()[(size_t) row].id : juce::Uuid::null();
                const RegionFile* hit = nullptr;
                for (auto& f : sr->files) if (f.trackId == tid) hit = &f;
                if (hit == nullptr && ! sr->files.empty()) hit = &sr->files.front();
                if (hit != nullptr) app.revealInMedia (hit->file);
            }
        const int idx = ed->indexOf (selectedRegion);
        if (wantMarkDrag)
        {
            markDrag = true; markAnchor = juce::jmax (0.0, t); markStartX = e.x;
            markAlt = trackMark;                                           // Ctrl + Alt + drag: only the track(s) the mouse covers are marked
            markRow0 = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
        }
        else if (auto* od = canMove && ed->isOverdub (selectedRegion) ? ed->findAny (selectedRegion) : nullptr)
        {
            if (app.isPlaying()) app.stopPlayback();
            dragOverdub = true; overdubBase = od->startSample;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }
        else if (canMove && idx >= 0 && e.x > kNameW)
        {
            if (app.isPlaying()) app.stopPlayback();
            dragging = true; dragIndex = idx; baseRegions = ed->regions;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }
        if (e.getNumberOfClicks() > 1 && e.x > kNameW && ! additive && ! selectedRegion.isNull())     // double-click on a piece: trim its fades
        {
            dragging = false; dragOverdub = false; baseRegions.clear();
            if (ed->isOverdub (selectedRegion))
                showError ("Trim", "An overdub lies on top of other audio, so it has no crossfade to trim. Pieces of the edit itself can be trimmed.");
            else
            {
                const int j = idx > 0 ? idx : (idx == 0 && ed->regions.size() > 1 ? 1 : -1);      // the join before the piece (its in-fade); the first piece: the join after it
                if (j > 0 && app.showTrim) app.showTrim (editId, ed->regions[(size_t) j].id, false);
                else if (idx == 0 && app.showTrim) app.showTrim (editId, ed->regions[0].id, false);     // a lone piece: its fade-in at the start of the edit
            }
        }
        repaint();
    }

    void mouseDragImpl (const juce::MouseEvent& e)
    {
        auto* ed = edit();
        if (ed == nullptr) return;
        if (lassoActive)
        {
            const auto before = lassoMoved ? lassoRect() : juce::Rectangle<int>();
            lassoTo = e.getPosition();
            if (! lassoMoved && lassoTo.getDistanceFrom (lassoFrom) < 4) return;
            lassoMoved = true;
            const auto had = selectedRegions;
            applyLasso (*ed);
            if (had != selectedRegions) repaint();                              // (the pieces change colour: all of it)
            else repaint (before.getUnion (lassoRect()).expanded (2));
            return;
        }
        if (markers.mouseDrag (e)) return;
        if (insertDrag) { insertDragMove (e.x); return; }
        if (! shuffleId.isNull())
        {
            if (! shuffleMoved && std::abs (e.x - shuffleStartX) < 5) return;
            if (! shuffleMoved) { shuffleMoved = true; if (app.isPlaying()) app.stopPlayback(); setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
            const double rt = juce::jmax (1.0, ed->sampleRate);
            shuffleSample = (juce::int64) (juce::jmax (0.0, (e.x - kNameW) / pixelsPerSecond) * rt);
            for (int guard = 0; shuffleMode && guard < 64; ++guard)       // (shuffle on) the piece trades places with a neighbour once its middle has passed the neighbour's middle
            {
                const int i = ed->indexOf (shuffleId);
                if (i < 0) break;
                const auto& me = ed->regions[(size_t) i];
                const juce::int64 mid = shuffleSample - shuffleGrab + me.length() / 2;
                int dir = 0;
                if (i + 1 < (int) ed->regions.size() && mid > ed->regions[(size_t) i + 1].startSample + ed->regions[(size_t) i + 1].length() / 2) dir = 1;
                else if (i > 0 && mid < ed->regions[(size_t) i - 1].startSample + ed->regions[(size_t) i - 1].length() / 2) dir = -1;
                if (dir == 0 || ed->shuffleRegion (i, dir) < 0) break;
            }
            // and the piece goes where the mouse takes it: it can leave a stretch of silence next to its neighbours (with Slip Left / Slip Right on, the pieces on that side go with it)
            if (const int i = ed->indexOf (shuffleId); i >= 0)
            {
                const juce::int64 want = juce::jmax ((juce::int64) 0, shuffleSample - shuffleGrab);
                ed->slideRegion (i, want - ed->regions[(size_t) i].startSample, slipLeft, slipRight);
            }
            ed->settleGapFades();
            app.project.markDirty(); updateSize(); repaint();
            return;
        }
        if (widthDrag) { kNameW = juce::jlimit (110, 700, e.x - viewport.getViewPositionX()); updateSize(); repaint(); return; }
        if (autoDragIndex >= 0) { automationDrag (e, *ed); return; }
        if (! clipMoveRegion.isNull())
        {
            clipMoveToRow = trackRowAtY (e.y);                                       // (only an audio track's own row can be the target)
            repaint(); return;
        }
        if (rowDragFrom >= 0)
        {
            rowDragTo = trackRowAtY (e.y);                                           // (the track the mouse is on, or the last one above it: the buses and VCAs under the tracks keep their place)
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor); repaint(); return;
        }
        if (fadeDrag != 0)
        {
            if (auto* r = ed->findAny (fadeId))
            {
                const double rt = juce::jmax (1.0, ed->sampleRate);
                const double dx = e.getDistanceFromDragStartX() / pixelsPerSecond;
                const double len = juce::jlimit (0.0, (double) r->length() / rt, fadeBase + (fadeDrag == 1 ? dx : -dx));
                if (fadeDrag == 1) { r->inStart = 0.0; r->inEnd = len; } else { r->outStart = -len; r->outEnd = 0.0; }
                const int idx = ed->indexOf (fadeId);
                if (idx >= 0) ed->clampFades (idx);
                app.project.markDirty(); repaint();
            }
            return;
        }
        if (markDrag)
        {
            if (! markActive && std::abs (e.x - markStartX) < 4) return;
            if (! markActive) { markActive = true; selectedRegion = juce::Uuid::null(); selectedRegions.clear(); }     // marking, not selecting: the next drag marks again too
            const double now = juce::jmax (0.0, (e.x - kNameW) / pixelsPerSecond);
            ed->fixIn = juce::jmin (markAnchor, now); ed->fixOut = juce::jmax (markAnchor, now);
            ed->fixTracks.clear();
            if (markAlt)
            {
                const auto& rt = rowTracks();
                const int n = (int) rt.size();
                const int r1 = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
                const int lo = juce::jlimit (0, juce::jmax (0, n - 1), juce::jmin (markRow0, r1)), hi = juce::jlimit (0, juce::jmax (0, n - 1), juce::jmax (markRow0, r1));
                if (n > 0) for (int r = lo; r <= hi; ++r) if (rowInfo[(size_t) r].kind == NodeKind::Track && rowInfo[(size_t) r].lane == 0) ed->fixTracks.push_back (rt[(size_t) r].id);
            }
            app.project.markDirty(); repaint();
            return;
        }
        if (dragOverdub)                                                  // an overdub moves freely: nothing else in the edit moves
        {
            if (auto* od = ed->findAny (selectedRegion))
            {
                const double rt = juce::jmax (1.0, ed->sampleRate);
                od->startSample = juce::jmax ((juce::int64) 0, overdubBase + (juce::int64) std::llround (e.getDistanceFromDragStartX() / pixelsPerSecond * rt));
                app.project.markDirty(); updateSize(); repaint();
            }
            return;
        }
        if (! dragging) return;
        const double rate = juce::jmax (1.0, ed->sampleRate);
        EditDef tmp; tmp.sampleRate = ed->sampleRate; tmp.regions = baseRegions;
        tmp.slideRegion (dragIndex, (juce::int64) std::llround (e.getDistanceFromDragStartX() / pixelsPerSecond * rate), slipLeft, slipRight);
        for (size_t i = 0; i < ed->regions.size() && i < tmp.regions.size(); ++i) ed->regions[i].startSample = tmp.regions[i].startSample;
        app.project.markDirty();
        updateSize();
        repaint();
    }

    void mouseMoveImpl (const juce::MouseEvent& e)
    {
        auto* ed = edit(); EditRegion* fr = nullptr; bool isIn = true;
        if (std::abs (e.x - (viewport.getViewPositionX() + kNameW)) <= 4 && e.y >= kRulerH) { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
        const bool over = ed != nullptr && e.x > kNameW && fadeHandleAt (*ed, e.getPosition(), juce::jmax (1.0, ed->sampleRate), fr, isIn);
        setMouseCursor (over ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        (void) isIn;
    }

    void mouseUpImpl (const juce::MouseEvent& e)
    {
        if (lassoActive)
        {
            if (auto* ed = edit(); ed != nullptr && ! lassoMoved) applyLasso (*ed);       // Alt + click (no drag): the piece under the mouse, or none
            lassoActive = lassoMoved = false;
            repaint(); return;
        }
        if (markers.mouseUp (e)) return;
        if (insertDrag)
        {
            insertDrag = false;
            if (auto* ed = edit()) { ed->insertIndex = insertDragIndex >= (int) ed->regions.size() ? -1 : insertDragIndex; app.project.changed(); }
            repaint(); return;
        }
        if (! shuffleId.isNull())
        {
            const bool moved = shuffleMoved;
            shuffleId = juce::Uuid::null(); shuffleMoved = false;
            if (moved) if (auto* ed = edit()) ed->settleGapFades();
            setMouseCursor (juce::MouseCursor::NormalCursor);
            if (moved) app.project.changed();
            repaint(); return;
        }
        if (widthDrag) { widthDrag = false; setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); return; }
        if (! clipMoveRegion.isNull())
        {
            finishClipMove();
            clipMoveRegion = juce::Uuid::null(); clipMoveToRow = -1;
            setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); return;
        }
        if (rowDragFrom >= 0)
        {
            const int from = rowDragFrom, to = rowDragTo; rowDragFrom = rowDragTo = -1;
            if (from == to && ! e.mouseWasDraggedSinceMouseDown() && juce::isPositiveAndBelow (from, (int) rowTracks().size()))   // a click (no drag): select the track
            {
                clickTrackName (rowTracks()[(size_t) from].id, e.mods);
                setMouseCursor (juce::MouseCursor::NormalCursor); return;
            }
            if (auto* ed = edit(); ed != nullptr && from != to && from >= 0 && to >= 0)
            {
                const auto fromNode = infoOf (from).node, toNode = infoOf (to).node;
                app.project.materialiseEditTracks (*ed);
                // the list may hold ids that no longer exist: move by what is shown
                std::vector<juce::Uuid> shown; for (auto* t : app.project.editTracks (*ed)) shown.push_back (t->id);
                const auto fromIt = std::find (shown.begin(), shown.end(), fromNode), toIt = std::find (shown.begin(), shown.end(), toNode);      // (rows and tracks are not the same count when lane rows are shown)
                if (fromIt != shown.end() && toIt != shown.end())
                {
                    const int fi = (int) (fromIt - shown.begin()), ti = (int) (toIt - shown.begin());
                    const auto id = shown[(size_t) fi]; shown.erase (shown.begin() + fi); shown.insert (shown.begin() + ti, id);
                    ed->trackIds = shown; app.project.changed();
                }
            }
            setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); return;
        }
        if (markActive) { markActive = false; app.project.changed(); }
        markDrag = false;
        if (autoDragIndex >= 0) { autoDragIndex = -1; autoDragLane = juce::Uuid::null(); app.project.changed(); }
        if (fadeDrag != 0) { fadeDrag = 0; app.project.changed(); }
        if (dragOverdub) { dragOverdub = false; app.project.changed(); }
        if (dragging) { dragging = false; baseRegions.clear(); if (auto* ed = edit()) ed->settleGapFades(); app.project.changed(); }
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    /** Dragging the insert flag: it follows the mouse, sticking to the start / end of a piece when it comes close; let go and it settles on the nearest one. */
    void insertDragMove (int x)
    {
        auto* ed = edit();
        if (ed == nullptr) return;
        const double rate = juce::jmax (1.0, ed->sampleRate);
        const int n = (int) ed->regions.size();
        int best = n; double bd = 1.0e9;
        for (int k = 0; k <= n; ++k)
        {
            const juce::int64 at = k >= n ? (n == 0 ? 0 : ed->regions.back().endSample()) : ed->regions[(size_t) k].startSample;
            const double d = std::abs (xOf ((double) at / rate) - x);
            if (d < bd) { bd = d; best = k; }
        }
        insertDragIndex = best;
        insertSnapped = bd <= 18.0;
        const juce::int64 at = best >= n ? (n == 0 ? 0 : ed->regions.back().endSample()) : ed->regions[(size_t) best].startSample;
        insertShownX = insertSnapped ? xOf ((double) at / rate) : juce::jmax (kNameW, x);
        repaint();
    }

    /** Shift + drag released: the clip goes to the track under the mouse (if that track already has a clip in the same piece, the two swap tracks). */
    void finishClipMove()
    {
        auto* ed = edit();
        if (ed == nullptr) return;
        auto* r = ed->findAny (clipMoveRegion);
        const auto rt = rowTracks();
        if (r == nullptr || ! juce::isPositiveAndBelow (clipMoveToRow, (int) rt.size())) return;
        const auto& target = rt[(size_t) clipMoveToRow];
        if (target.id == clipMoveTrack || target.id.isNull() || app.project.kindOf (target.id) != NodeKind::Track) return;
        int fi = -1, fj = -1;
        for (int i = 0; i < (int) r->files.size(); ++i) { if (r->files[(size_t) i].trackId == clipMoveTrack) fi = i; if (r->files[(size_t) i].trackId == target.id) fj = i; }
        const auto* srcTrack = app.project.findTrack (clipMoveTrack);
        if (fi < 0 || srcTrack == nullptr) return;
        if (r->files[(size_t) fi].numChannels != target.channelCount() || (fj >= 0 && r->files[(size_t) fj].numChannels != srcTrack->channelCount()))
        {
            app.setNotice ("That clip cannot go there: a mono clip goes on a mono track and a stereo clip on a stereo track (the tracks must have the same number of channels).");
            return;
        }
        r->files[(size_t) fi].trackId = target.id; r->files[(size_t) fi].trackName = target.name;
        if (fj >= 0) { r->files[(size_t) fj].trackId = srcTrack->id; r->files[(size_t) fj].trackName = srcTrack->name; }
        app.project.changed();
    }

private:
    // ------------------------------------------------------------------ automation
    // What each row shows (which lane) is kept by the edit (EditDef::laneRows) and saved with the project; the lanes themselves are the edit's (EditDef::lanes) and play whatever the rows show.
    juce::Uuid autoDragLane = juce::Uuid::null(); int autoDragIndex = -1; int autoDragRow = 0; float autoPrevY = 0.0f;
    juce::Rectangle<int> plusRect (int sx, int rowTop) const  { return { sx + kNameW - 52, rowTop + 3, 20, 16 }; }
    juce::Rectangle<int> minusRect (int sx, int rowTop) const { return { sx + kNameW - 28, rowTop + 3, 20, 16 }; }
    /** The small box with a + or a - in it (a - that has nothing to take away is dim). */
    static void drawLaneBox (juce::Graphics& g, juce::Rectangle<int> r, bool plus, bool enabled)
    {
        const float a = enabled ? 1.0f : 0.35f;
        g.setColour (juce::Colour (0xff2a2f36).withMultipliedAlpha (a)); g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (theme::border.withMultipliedAlpha (a)); g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);
        g.setColour (theme::text.withMultipliedAlpha (a));
        const auto c = r.toFloat().getCentre();
        g.drawLine (c.x - 4.5f, c.y, c.x + 4.5f, c.y, 1.8f);
        if (plus) g.drawLine (c.x, c.y - 4.5f, c.x, c.y + 4.5f, 1.8f);
    }
    juce::Rectangle<int> muteRect (int sx, int rowTop) const { return { sx + 10, rowTop + 3, 18, 16 }; }
    juce::Rectangle<int> soloRect (int sx, int rowTop) const { return { sx + 30, rowTop + 3, 18, 16 }; }
    StripState* stripOf (const juce::Uuid& trackId) const
    {
        auto* m = mixerFor (juce::Uuid::null());
        return m != nullptr ? m->stripFor (trackId) : nullptr;
    }
    /** The mute / solo flags and meter values of a row: an audio track's strip, an Int Bus's strip, or a VCA (which has no meter). */
    struct MsRef { Flag* mute = nullptr; Flag* solo = nullptr; Param* pkL = nullptr; Param* pkR = nullptr; bool pkTake = false; };
    MsRef msOf (const juce::Uuid& id) const
    {
        MsRef r;
        auto* m = mixerFor (juce::Uuid::null());
        if (m == nullptr) return r;
        switch (app.project.kindOf (id))
        {
            case NodeKind::Track:  { auto* s = m->stripFor (id); r = { &s->mute, &s->solo, &s->livePkL, &s->livePkR, true }; break; }
            case NodeKind::IntBus: { auto* b = m->busFor (id); r = { &b->mute, &b->solo, &b->meterL, &b->meterR, false }; break; }      // (the mixer's own meter value is only read here, never reset)
            case NodeKind::Vca:    if (auto* v = app.project.findVca (id)) { auto& s = m->vcas[juce::jlimit (0, kMaxVcas - 1, v->number - 1)]; r = { &s.mute, &s.solo, nullptr, nullptr, false }; } break;
            default: break;
        }
        return r;
    }
    /** Mute / Solo buttons (the same flags as on the linked mixer) and a small horizontal meter. */
    void paintStripBits (juce::Graphics& g, const juce::Uuid& trackId, int sx, int rowTop, bool withMeter)
    {
        const auto st = msOf (trackId);
        if (st.mute == nullptr) return;
        auto drawBtn = [&] (juce::Rectangle<int> r, const char* t, bool on, juce::Colour c)
        {
            g.setColour (on ? c : juce::Colour (0xff2a2f36)); g.fillRoundedRectangle (r.toFloat(), 3.0f);
            g.setColour (theme::border); g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);
            g.setColour (on ? juce::Colours::black : theme::text); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (t, r, juce::Justification::centred, false);
        };
        drawBtn (muteRect (sx, rowTop), "M", st.mute->get(), juce::Colour (0xffe05a3a));
        drawBtn (soloRect (sx, rowTop), "S", st.solo->get(), juce::Colour (0xfff2c200));
        if (! withMeter) return;
        auto mr = juce::Rectangle<float> ((float) sx + 12.0f, (float) (rowTop + rowH - 11), (float) kNameW - 24.0f, 6.0f);
        g.setColour (juce::Colour (0xff15181c)); g.fillRoundedRectangle (mr, 2.0f);
        auto lvl = [&] (float lin) { const float db = juce::Decibels::gainToDecibels (lin, -60.0f); return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        const auto shown = shownLevel.find (trackId.toString());
        const float v = lvl (shown != shownLevel.end() ? shown->second : 0.0f);                    // (the level the timer keeps: it falls back when nothing plays)
        g.setColour (v > 0.95f ? juce::Colour (0xffe03030) : v > 0.85f ? juce::Colour (0xffe0b800) : juce::Colour (0xff3fbf5f));
        g.fillRoundedRectangle (mr.withWidth (mr.getWidth() * v), 2.0f);
    }
    /** The meters show what the mixer strip measures while it runs; when nothing plays (stopped or paused) the strip's last value stays in it, so the bar falls back to zero smoothly instead, as a live meter does when the signal stops. Returns true while any bar is still moving. */
    std::map<juce::String, float> shownLevel;
    bool updateMeterFall()
    {
        const bool running = app.isPlaying() || app.engine.isRecording();
        bool moving = false;
        for (auto& t : rowTracks())
        {
            auto& v = shownLevel[t.id.toString()];
            float target = 0.0f;
            if (const auto st = msOf (t.id); st.pkL != nullptr) { const float pk = st.pkTake ? juce::jmax (st.pkL->take(), st.pkR->take()) : juce::jmax (st.pkL->get(), st.pkR->get()); if (running) target = pk; }     // the peak since the last look: as quick as the mixer's meters
            v = juce::jmax (target, v * 0.86f);
            if (v < 1.0e-4f) v = 0.0f; else moving = true;
        }
        return moving;
    }
    juce::Rectangle<int> lockRect (int sx, int rowTop) const { return { sx + kNameW - 24, rowTop + rowH - 22, 18, 18 }; }
    juce::Rectangle<int> paramRect (int sx, int rowTop) const { return { sx + 12, rowTop + rowH - 22, kNameW - 44, 18 }; }

    MixerState* mixerFor (const juce::Uuid& id) const
    {
        if (id.isNull())                                             // no mixer named: this Edit's own mixer (the processing mixer if it has none)
        {
            if (auto* own = app.project.mixerOfEdit (editId)) return own;
            return app.project.mixers.empty() ? nullptr : app.project.mixers.front().get();
        }
        for (auto& m : app.project.mixers) if (m->id == id) return m.get();
        return nullptr;
    }
    /** The plug-in a plug-in lane controls (null when the slot is empty or it is not a plug-in lane) and the number of the parameter. */
    InsertProcessor* processorOfLane (const AutoLane& lane, int& idx) const
    {
        int slot = 0; idx = 0;
        if (! autoparam::parsePlugin (lane.param, slot, idx) || slot >= kNumSlots) return nullptr;
        auto* m = mixerFor (lane.mixerId); if (m == nullptr) return nullptr;
        auto* sls = slotsOf (m, lane.trackId);
        return sls != nullptr ? sls[slot].getProcessorUnsafe() : nullptr;
    }
    /** The insert slots of a track, an Int Bus or an Ext Bus in a mixer (null for anything else: a VCA has none). */
    InsertSlot* slotsOf (MixerState* m, const juce::Uuid& node) const
    {
        if (m == nullptr) return nullptr;
        switch (app.project.kindOf (node))
        {
            case NodeKind::Track:  return m->stripFor (node)->slots;
            case NodeKind::IntBus:
            case NodeKind::ExtBus: return m->busFor (node)->slots;
            default: return nullptr;
        }
    }
    /** How many positions the lane's plug-in parameter has when it is a switch or a selector (a bypass: 2); 0 for a smooth control and for every other lane. Such a lane
        only takes its own positions (a point snaps to the nearest, there is no slope between two points) - it is what a Bypass lane must do. */
    int stepsOfLane (const AutoLane& lane) const
    {
        if (! lane.isPlugin()) return 0;
        int idx = 0;
        auto* pr = processorOfLane (lane, idx);
        return pr != nullptr && idx < pr->numParams() ? pr->paramSteps (idx) : 0;
    }
    juce::String nodeName (const juce::Uuid& id) const
    {
        for (auto& b : app.project.buses) if (b.id == id) return b.name;
        for (auto& t : app.project.tracks) if (t.id == id) return t.name;
        return "?";
    }
    /** "Fader", "Pan", "Send: Reverb", "Plug-in: <name> / <parameter>". */
    juce::String describeParam (const juce::String& param, const juce::Uuid& trackId, const juce::Uuid& mixerId) const
    {
        if (param == autoparam::pan) return "Pan";
        if (param == autoparam::gain) return "Gain";
        if (autoparam::isSend (param)) return "Send: " + nodeName (autoparam::sendDest (param));
        int slot = 0, idx = 0;
        if (autoparam::parsePlugin (param, slot, idx))
        {
            if (auto* m = mixerFor (mixerId))
            {
                InsertSlot* sls = slotsOf (m, trackId);
                if (sls != nullptr && slot < kNumSlots)
                    if (auto* pr = sls[slot].getProcessorUnsafe())
                        return pr->getName() + ": " + pr->paramName (idx);
            }
            return "Plug-in " + juce::String (slot + 1) + " (empty)";
        }
        return "Fader";
    }

    /** What the row shows, as the label in its name cell. */
    juce::String paramLabel (const EditDef& ed, const juce::Uuid& node, int laneIdx)
    {
        const auto s = ed.laneShown (node, laneIdx);
        juce::String n = describeParam (s.param, node, s.mixer);
        if (! s.mixer.isNull()) for (auto& m : app.project.mixers) if (m->id == s.mixer) n += " (" + m->name + ")";
        return n;
    }
    static void drawPadlock (juce::Graphics& g, juce::Rectangle<float> r, bool locked, juce::Colour c)
    {
        g.setColour (c);
        auto body = r.withTrimmedTop (r.getHeight() * 0.42f).reduced (r.getWidth() * 0.12f, 0.0f);
        g.fillRoundedRectangle (body, 2.0f);
        const float w = body.getWidth() * 0.56f, cx = body.getCentreX() + (locked ? 0.0f : w * 0.55f);
        juce::Path sh;
        const float top = r.getY() + 1.0f, bot = body.getY() + 0.5f;
        sh.startNewSubPath (cx - w / 2, bot);
        sh.lineTo (cx - w / 2, top + w / 2);
        sh.addCentredArc (cx, top + w / 2, w / 2, w / 2, 0.0f, -juce::MathConstants<float>::pi, 0.0f, false);
        if (locked) sh.lineTo (cx + w / 2, bot);
        else sh.lineTo (cx + w / 2, top + w / 2 + 1.0f);
        g.strokePath (sh, juce::PathStrokeType (1.8f));
    }

    float yOfValue (const AutoLane& lane, float v, int rowTop) const
    {
        const float m = 5.0f, h = (float) rowH - 2 * m;
        float f;
        if (lane.param == autoparam::pan) f = (v + 1.0f) * 0.5f;
        else if (lane.isPlugin()) f = juce::jlimit (0.0f, 1.0f, v);
        else f = (juce::jlimit (-60.0f, 12.0f, v) + 60.0f) / 72.0f;
        return (float) rowTop + m + (1.0f - f) * h;
    }
    /** The value as text for the box that follows a point while it is dragged: dB for faders / gains / sends, L / C / R for pan, a number for plug-in parameters. */
    juce::String autoValueText (const AutoLane& lane, float v) const
    {
        if (const int steps = stepsOfLane (lane); steps >= 2)           // a switch or selector: its position and the plug-in's word for it ("Off", "Bypassed")
        {
            int idx = 0;
            const float q = AutoLane::snapToSteps (v, steps);
            const juce::String word = processorOfLane (lane, idx) != nullptr ? processorOfLane (lane, idx)->paramText (idx, q) : juce::String();
            const int pos = (int) std::lround (q * (float) (steps - 1)) + 1;
            return word.isNotEmpty() ? word + "  (" + juce::String (pos) + " of " + juce::String (steps) + ")" : juce::String (pos) + " of " + juce::String (steps);
        }
        return autoValueTextPlain (lane, v);
    }
    static juce::String autoValueTextPlain (const AutoLane& lane, float v)
    {
        if (lane.param == autoparam::pan)
        {
            const int pct = (int) std::lround (std::abs (v) * 100.0f);
            return pct == 0 ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String (pct);
        }
        if (lane.isPlugin()) return juce::String (v, 3);
        if (v <= AutoLane::kFaderOff + 0.5f) return "-inf dB";
        return juce::String (v, 1) + " dB";
    }
    /** Where the mouse is, as a value; rounded to 0.1 dB (pan 0.01, plug-in 0.001) so a figure like -4.5 dB can be hit exactly. Alt = ten times finer movement. */
    float draggedValue (const AutoLane& lane, float y, int rowTop, bool fine, float current)
    {
        float v = valueOfY (lane, y, rowTop);
        const int steps = stepsOfLane (lane);
        if (steps >= 2) { autoPrevY = y; return AutoLane::snapToSteps (v, steps); }       // a switch or selector (a bypass): only its own positions; no fine mode, the nearest one under the mouse
        if (fine) v = current + (valueOfY (lane, y, rowTop) - valueOfY (lane, autoPrevY, rowTop)) * 0.1f;
        autoPrevY = y;
        if (lane.param == autoparam::pan) return juce::jlimit (-1.0f, 1.0f, std::round (v * 100.0f) / 100.0f);
        if (lane.isPlugin()) return juce::jlimit (0.0f, 1.0f, std::round (v * 1000.0f) / 1000.0f);
        if (v <= AutoLane::kFaderOff + 0.5f) return AutoLane::kFaderOff;
        return std::round (v * 10.0f) / 10.0f;
    }
    float valueOfY (const AutoLane& lane, float y, int rowTop) const
    {
        const float m = 5.0f, h = (float) rowH - 2 * m;
        const float f = juce::jlimit (0.0f, 1.0f, 1.0f - (y - (float) rowTop - m) / juce::jmax (1.0f, h));
        if (lane.param == autoparam::pan) return f * 2.0f - 1.0f;
        if (lane.isPlugin()) return f;
        const float db = f * 72.0f - 60.0f;
        return db <= -59.5f ? AutoLane::kFaderOff : db;                       // the very bottom is 'off'
    }

    void paintLane (juce::Graphics& g, EditDef& ed, const RowInfo& inf, int rowTop)
    {
        const double rate = juce::jmax (1.0, ed.sampleRate);
        const auto s = ed.laneShown (inf.node, inf.lane);
        const auto* lane = ed.findLane (inf.node, s.param, s.mixer);
        const bool locked = lane != nullptr && lane->locked;
        const float x0 = (float) kNameW, x1 = (float) getWidth();
        g.setColour (juce::Colours::black.withAlpha (0.18f)); g.fillRect (juce::Rectangle<float> (x0, (float) rowTop, x1 - x0, (float) rowH));
        const AutoLane dummy = [&] { AutoLane d; d.param = s.param; return d; }();
        const AutoLane& L = lane != nullptr ? *lane : dummy;
        const juce::Colour line = locked ? juce::Colour (0xff9aa0a6) : juce::Colour (0xffffc83d);
        auto px = [&] (juce::int64 time) { return (float) xOf ((double) time / rate); };
        const float zeroY = yOfValue (L, L.defaultValue(), rowTop);
        if (L.pts.empty())
        {
            const float dash[] = { 4.0f, 4.0f };
            g.setColour (line.withAlpha (0.35f)); g.drawDashedLine (juce::Line<float> (x0, zeroY, x1, zeroY), dash, 2, 1.0f);
            return;
        }
        const int steps = stepsOfLane (L);                                  // a switch or selector: drawn as the steps it really has (the value holds until the next point)
        auto shown = [&] (float v) { return steps >= 2 ? AutoLane::snapToSteps (v, steps) : v; };
        if (steps >= 2 && steps <= 16)                                      // a faint guide at every position it can take
        {
            g.setColour (line.withAlpha (0.16f));
            for (int k = 0; k < steps; ++k) g.fillRect (x0, std::round (yOfValue (L, (float) k / (float) (steps - 1), rowTop)), x1 - x0, 1.0f);
        }
        juce::Path p;
        p.startNewSubPath (x0, yOfValue (L, shown (L.pts.front().value), rowTop));
        for (size_t k = 0; k < L.pts.size(); ++k)
        {
            const auto& pt = L.pts[k];
            if (steps >= 2 && k > 0) p.lineTo (px (pt.time), yOfValue (L, shown (L.pts[k - 1].value), rowTop));      // along at the old value, then straight up or down at the point
            p.lineTo (px (pt.time), yOfValue (L, shown (pt.value), rowTop));
        }
        p.lineTo (x1, yOfValue (L, shown (L.pts.back().value), rowTop));
        g.setColour (line); g.strokePath (p, juce::PathStrokeType (1.6f));
        for (size_t k = 0; k < L.pts.size(); ++k)
        {
            const auto& pt = L.pts[k];
            const juce::Point<float> c (px (pt.time), yOfValue (L, shown (pt.value), rowTop));
            const auto r = juce::Rectangle<float> (10, 10).withCentre (c);
            if (pt.floating) { g.setColour (juce::Colour (0xffff7a2f)); g.drawEllipse (r, 2.0f); }       // its audio went: check it
            else { g.setColour (line); g.fillEllipse (r); g.setColour (juce::Colours::black.withAlpha (0.6f)); g.drawEllipse (r, 1.0f); }
        }
    }

    /** Returns true when the click was for the automation (the lanes, their parameter label and padlock). */
    bool automationMouseDown (const juce::MouseEvent& e, EditDef& ed)
    {
        const int top = kRulerH + kLaneH;
        if (e.y < top) return false;
        const int row = (e.y - top) / juce::jmax (1, rowH);
        if (! juce::isPositiveAndBelow (row, (int) rowTracks().size())) return false;
        const TrackDef t = rowTracks()[(size_t) row];                                    // (copies: rowTracks() is rebuilt on every call)
        const RowInfo inf = rowInfo[(size_t) row];
        const int rowTop = top + row * rowH;
        const int sx = viewport.getViewPositionX();
        const auto s = ed.laneShown (inf.node, inf.lane);
        if (e.x < sx + kNameW)
        {
            if (lockRect (sx, rowTop).expanded (3).contains (e.getPosition()))
            {
                auto& l = ed.addLane (inf.node, s.param, s.mixer);
                l.locked = ! l.locked; app.project.changed(); repaint(); return true;
            }
            if (paramRect (sx, rowTop).contains (e.getPosition())) { showLaneMenu (ed, t, inf); return true; }
            if (inf.kind != NodeKind::Vca || inf.lane > 0)                               // + : one more lane row under this one's rows; - : takes this row away (on the node's own row, the last extra row)
            {
                const bool pan = inf.kind == NodeKind::IntBus || (inf.kind == NodeKind::Track && t.channelCount() == 1);
                const bool gain = inf.kind == NodeKind::Track || inf.kind == NodeKind::IntBus;
                if (inf.lane == 0 && plusRect (sx, rowTop).expanded (2).contains (e.getPosition()))
                {
                    if (! ed.addLaneRow (inf.node, pan, gain)) app.setNotice ("That is as many lane rows as one track or bus can have (" + juce::String (EditDef::kMaxLaneRows) + ").");
                    app.project.changed(); updateSize(); repaint(); return true;
                }
                if (minusRect (sx, rowTop).expanded (2).contains (e.getPosition()))
                {
                    const int rowToTake = inf.lane > 0 ? inf.lane : ed.laneRowCount (inf.node) - 1;
                    if (rowToTake >= 1)
                    {
                        const auto gone = ed.laneShown (inf.node, rowToTake);
                        const auto* l = ed.findLane (inf.node, gone.param, gone.mixer);
                        const bool stillPlays = l != nullptr && ! l->pts.empty();
                        ed.removeLaneRow (inf.node, rowToTake);
                        if (stillPlays) app.setNotice ("The row is gone but its automation is not: \"" + describeParam (gone.param, inf.node, gone.mixer) + "\" still plays. Show it in a row again (the label on the left of a row) to see or change it.");
                        app.project.changed(); updateSize(); repaint();
                    }
                    return true;
                }
            }
            return false;
        }
        auto* lane = ed.findLane (inf.node, s.param, s.mixer);
        const double rate = juce::jmax (1.0, ed.sampleRate);
        if (lane != nullptr)
        {
            for (int k = 0; k < (int) lane->pts.size(); ++k)
            {
                const auto& pt = lane->pts[(size_t) k];
                const float dx = (float) xOf ((double) pt.time / rate) - (float) e.x, dy = yOfValue (*lane, AutoLane::snapToSteps (pt.value, stepsOfLane (*lane)), rowTop) - (float) e.y;     // (a switch's point sits on its nearest position)
                if (dx * dx + dy * dy > 8.0f * 8.0f) continue;
                if (lane->locked) return true;                                          // locked: nothing can be changed
                if (e.mods.isPopupMenu() || e.mods.isShiftDown() || e.mods.isCtrlDown())
                {
                    lane->pts.erase (lane->pts.begin() + k); app.project.changed(); repaint(); return true;
                }
                autoDragLane = lane->id; autoDragIndex = k; autoDragRow = row; autoPrevY = (float) e.y; repaint(); return true;
            }
            if (lane->locked) return true;
        }
        if (e.mods.isPopupMenu()) return true;
        auto& l = ed.addLane (inf.node, s.param, s.mixer);
        if (s.param == autoparam::pan && t.channelCount() != 1) return true;         // pan is for mono tracks
        const auto time = (juce::int64) std::llround (juce::jmax (0.0, (double) (e.x - kNameW) / pixelsPerSecond) * rate);
        autoPrevY = (float) e.y;
        autoDragIndex = ed.addPoint (l, time, draggedValue (l, (float) e.y, rowTop, false, 0.0f));
        autoDragLane = l.id; autoDragRow = row;
        app.project.markDirty(); repaint();
        return true;
    }

    void automationDrag (const juce::MouseEvent& e, EditDef& ed)
    {
        auto* lane = ed.findLane (autoDragLane);
        if (lane == nullptr || lane->locked) return;
        const double rate = juce::jmax (1.0, ed.sampleRate);
        const int rowTop = kRulerH + kLaneH + autoDragRow * rowH;
        const auto time = (juce::int64) std::llround (juce::jmax (0.0, (double) (e.x - kNameW) / pixelsPerSecond) * rate);
        const float cur = juce::isPositiveAndBelow (autoDragIndex, (int) lane->pts.size()) ? lane->pts[(size_t) autoDragIndex].value : 0.0f;
        autoDragIndex = ed.movePoint (*lane, autoDragIndex, time, draggedValue (*lane, (float) e.y, rowTop, e.mods.isAltDown(), cur));
        app.project.markDirty(); repaint();
    }

    void showLaneMenu (EditDef& ed, const TrackDef& t, const RowInfo& inf)
    {
        const auto node = inf.node; const int laneIdx = inf.lane;
        const auto s = ed.laneShown (node, laneIdx);
        juce::PopupMenu m, mixers;
        m.addSectionHeader ("Automate (this row shows)");
        const auto kind = inf.kind;
        const bool isVca = kind == NodeKind::Vca, isExt = kind == NodeKind::ExtBus;
        auto marked = [&] (const juce::String& text, const juce::String& param)           // a lane that already has points says so: the row can show it again whatever else was shown
        {
            const auto* l = ed.findLane (node, param, s.mixer);
            return l != nullptr && ! l->pts.empty() ? text + "   (automated)" : text;
        };
        if (! isVca && ! isExt) m.addItem (3, marked ("Gain (into the mixer, before the inserts and fader)", autoparam::gain), true, s.param == autoparam::gain);
        m.addItem (1, marked (isVca ? "Fader (adds to the faders of everything it controls)" : "Fader (through the mixer, after the inserts)", autoparam::fader), true, s.param == autoparam::fader);
        if (! isVca && ! isExt) m.addItem (2, marked (kind == NodeKind::IntBus ? juce::String ("Pan") : "Pan" + juce::String (t.channelCount() == 1 ? "" : " (mono tracks only)"), autoparam::pan), t.channelCount() == 1, s.param == autoparam::pan);
        // bus / track sends the strip really has (in the mixer chosen below), and the automatable parameters of its plug-ins
        std::vector<juce::String> keys;
        juce::PopupMenu sends, plugins;
        if (auto* mx = mixerFor (s.mixer); mx != nullptr && ! isVca)
        {
            SendList* sendList = kind == NodeKind::IntBus ? &mx->busFor (node)->sends : kind == NodeKind::Track ? &mx->stripFor (node)->sends : nullptr;       // (an Ext Bus has no sends)
            InsertSlot* slotArr = slotsOf (mx, node);
            if (sendList != nullptr)
                for (auto& sd : sendList->pool)
                {
                    keys.push_back (autoparam::send (sd->dest));
                    sends.addItem (1000 + (int) keys.size() - 1, marked (nodeName (sd->dest), keys.back()), true, s.param == keys.back());
                }
            for (int sl = 0; slotArr != nullptr && sl < kNumSlots; ++sl)
            {
                auto* pr = slotArr[sl].getProcessorUnsafe();
                if (pr == nullptr) continue;
                juce::PopupMenu one, page; int inPage = 0, pageStart = 0;
                auto flush = [&]
                {
                    if (inPage > 0) one.addSubMenu (inPage == 1 ? juce::String (pageStart + 1) : juce::String (pageStart + 1) + " - " + juce::String (pageStart + inPage), page);
                    page = juce::PopupMenu(); inPage = 0;
                };
                int total = 0;
                for (int i = 0; i < pr->numParams() && keys.size() < 5000; ++i)
                {
                    if (! pr->paramAutomatable (i)) continue;
                    if (inPage == 0) pageStart = total;
                    keys.push_back (autoparam::plugin (sl, i)); ++total;
                    page.addItem (1000 + (int) keys.size() - 1, marked (pr->paramName (i), keys.back()), true, s.param == keys.back());
                    if (++inPage == 25) flush();
                }
                flush();
                if (total > 0) plugins.addSubMenu ("Insert " + juce::String (sl + 1) + ": " + pr->getName(), one);
            }
        }
        if (! isExt) m.addSubMenu ("Bus sends", sends, sends.getNumItems() > 0);
        m.addSubMenu ("Plug-in parameters", plugins, plugins.getNumItems() > 0);
        m.addSeparator();
        {
            auto* own = app.project.mixerOfEdit (ed.id);
            mixers.addItem (99, (own != nullptr ? own->name : juce::String ("Processing mixer")) + "  (this edit's mixer, default)", true, s.mixer.isNull());
            for (int i = 0; i < app.project.cueEnd(); ++i)           // the processing mixer and the cue mixers (the other Edits' mixers are not offered)
            {
                const auto& mx = *app.project.mixers[(size_t) i];
                mixers.addItem (100 + i, mx.name + (i == 0 ? "  (processing mixer)" : "  (cue mixer)"), true, s.mixer == mx.id);
            }
        }
        m.addSubMenu ("Drives mixer", mixers);
        m.addSeparator();
        auto* lane = ed.findLane (node, s.param, s.mixer);
        m.addItem (5, "Clear this lane's points", lane != nullptr && ! lane->pts.empty() && ! lane->locked);
        const auto editId = ed.id;
        m.showMenuAsync (juce::PopupMenu::Options(), [this, node, laneIdx, editId, keys] (int r)
        {
            auto* e2 = app.project.findEdit (editId);
            if (r == 0 || e2 == nullptr) return;
            auto sel = e2->laneShown (node, laneIdx);
            if (r == 1) sel.param = autoparam::fader;
            else if (r == 2) sel.param = autoparam::pan;
            else if (r == 3) sel.param = autoparam::gain;
            else if (r >= 1000) { const auto i = (size_t) (r - 1000); if (i < keys.size()) sel.param = keys[i]; }
            else if (r == 99) sel.mixer = juce::Uuid::null();
            else if (r >= 100) { const auto i = (size_t) (r - 100); if (i < app.project.mixers.size()) sel.mixer = app.project.mixers[i]->id; }
            else if (r == 5) { if (auto* l = e2->findLane (node, sel.param, sel.mixer)) { l->pts.clear(); app.project.changed(); } repaint(); return; }
            e2->setLaneShown (node, laneIdx, sel);                       // (only what the row shows changes: the lanes behind keep their points and keep playing)
            app.project.changed();
            repaint();
        });
    }

    int xOf (double seconds) const { return kNameW + (int) (seconds * pixelsPerSecond); }
    markerui::Controller markers { *this, kNameW,
        [this] () -> std::vector<Marker>* { auto* d = edit(); return d != nullptr ? &d->markers : nullptr; },
        [this] (int x) { return juce::jmax (0.0, (double) (x - kNameW) / pixelsPerSecond); },
        [this] (double s) { return xOf (s); },
        [this] { app.project.markDirty(); } };

    /** Every piece starts and ends with a short fade (25 ms unless changed). Its two outer corners are handles; the fade is drawn as a white slope. */
    void drawFadeHandles (juce::Graphics& g, juce::Rectangle<int> lane, const EditRegion& r, double rate, bool showIn, bool showOut)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        auto corner = [&] (bool left, double fadeSec, bool show)
        {
            if (! show) return;
            const float px = juce::jmax (0.0f, (float) (fadeSec * pixelsPerSecond));
            const float xe = left ? (float) lane.getX() : (float) lane.getRight();
            const float dir = left ? 1.0f : -1.0f;
            juce::Path tri; tri.addTriangle (xe, (float) lane.getY(), xe + dir * 8.0f, (float) lane.getY(), xe, (float) lane.getY() + 8.0f);
            g.fillPath (tri);
            if (px >= 2.0f) g.drawLine (xe, (float) lane.getBottom() - 1.0f, xe + dir * px, (float) lane.getY() + 1.0f, 1.0f);
        };
        (void) rate;
        corner (true,  r.inEnd - r.inStart, showIn);
        corner (false, r.outEnd - r.outStart, showOut);
    }

    /** The fade handle under p: top lane, within 12 px of the outer ends (start of the first piece, end of the last piece, both ends of an overdub). */
    bool fadeHandleAt (EditDef& ed, juce::Point<int> p, double rate, EditRegion*& hit, bool& isIn)
    {
        if (p.y < kRulerH || p.y >= kRulerH + kLaneH) return false;
        auto test = [&] (EditRegion& r, bool in, bool out) -> bool
        {
            const int x0 = xOf ((double) r.startSample / rate), wpx = juce::jmax (3, (int) ((double) r.length() / rate * pixelsPerSecond));
            const int zone = juce::jmin (12, wpx / 2);
            if (in  && p.x >= x0 && p.x < x0 + zone)               { hit = &r; isIn = true;  return true; }
            if (out && p.x >= x0 + wpx - zone && p.x <= x0 + wpx)  { hit = &r; isIn = false; return true; }
            return false;
        };
        for (auto it = ed.overdubs.rbegin(); it != ed.overdubs.rend(); ++it) if (test (*it, true, true)) return true;
        const size_t n = ed.regions.size();
        for (size_t k = 0; k < n; ++k) if (test (ed.regions[k], k == 0, k + 1 == n)) return true;
        return false;
    }

    /** The narrow band an overdub occupies in the middle of a track's row. */
    juce::Rectangle<int> overdubBand (int rowTop, int x0, int wpx) const { return { x0, rowTop + rowH / 4, wpx, juce::jmax (4, rowH / 2) }; }

    /** The overdub whose band is under 'p' (its own tracks' rows only), or null. */
    EditRegion* overdubAt (EditDef& ed, juce::Point<int> p, double rate)
    {
        const int top = kRulerH + kLaneH;
        const int row = (p.y - top) / juce::jmax (1, rowH);
        for (auto it = ed.overdubs.rbegin(); it != ed.overdubs.rend(); ++it)
        {
            const int x0 = xOf ((double) it->startSample / rate), wpx = juce::jmax (3, (int) ((double) it->length() / rate * pixelsPerSecond));
            if (p.y >= kRulerH + 14 && p.y < kRulerH + kLaneH - 4) { if (p.x >= x0 && p.x < x0 + wpx) return &*it; continue; }     // its little lane at the top
            if (! juce::isPositiveAndBelow (row, (int) rowTracks().size())) continue;
            bool has = false; for (auto& f : it->files) if (f.trackId == rowTracks()[(size_t) row].id) has = true;
            if (has && overdubBand (top + row * rowH, x0, wpx).contains (p)) return &*it;
        }
        return nullptr;
    }

    /** Right-click on a piece: volume changes (and deleting an overdub). */
    void showPieceMenu (const juce::MouseEvent& e, EditDef& ed, double t, double rate)
    {
        EditRegion* piece = overdubAt (ed, e.getPosition(), rate);
        const juce::int64 sample = (juce::int64) (juce::jmax (0.0, t) * rate);
        if (piece == nullptr) for (auto& r : ed.regions) if (sample >= r.startSample && sample < r.endSample()) piece = &r;
        if (piece == nullptr) return;
        selectedRegion = piece->id; selectedRegions = { piece->id }; selectedJoin = -1; repaint();
        const auto rid = piece->id; const auto eid = editId; const juce::int64 rel = juce::jlimit ((juce::int64) 0, piece->length(), sample - piece->startSample);
        const bool isOver = ed.isOverdub (rid), hasGains = ! piece->gains.empty();
        juce::PopupMenu m;
        m.addSectionHeader (piece->takeName + (isOver ? "  (overdub)" : ""));
        // the area between marks 1 and 2 for all tracks or the ones chosen with Alt + drag
        juce::int64 rangeFrom = 0, rangeTo = 0;
        if (ed.fixIn >= 0 && ed.fixOut > ed.fixIn) { rangeFrom = (juce::int64) std::llround (ed.fixIn * rate); rangeTo = (juce::int64) std::llround (ed.fixOut * rate); }
        const bool haveRange = rangeTo > rangeFrom && ! isOver;
        m.addItem (1, "Change volume from here to next edit...");
        m.addItem (7, "Change volume between the 1/2 marks...", haveRange);
        m.addItem (2, "Change volume between fades (this take only)...");
        m.addSeparator();
        m.addItem (3, "Remove all volume changes in this piece", hasGains);
        if (isOver) { m.addSeparator(); m.addItem (4, "Delete this overdub"); }
        std::vector<juce::File> redrawFiles;                                          // the file of the track that was clicked (else every file of the piece)
        {
            const int row = e.y >= kRulerH + kLaneH ? (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH) : -1;
            const auto& rt = rowTracks();
            const juce::Uuid tid = juce::isPositiveAndBelow (row, (int) rt.size()) ? rt[(size_t) row].id : juce::Uuid::null();
            for (auto& f : piece->files) if (f.trackId == tid) redrawFiles.push_back (f.file);
            if (redrawFiles.empty()) for (auto& f : piece->files) redrawFiles.push_back (f.file);
        }
        m.addSeparator();
        m.addItem (8, "Re-draw waveform");
        if (piece->waiting.active())
        {
            m.addSeparator();
            m.addItem (5, "Re-link corrected audio  (" + piece->waiting.name + ")");
            m.addItem (6, "Stop waiting: keep the original audio");
        }
        auto* appPtr = &app;
        juce::Component::SafePointer<juce::Component> self (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [appPtr, eid, rid, rel, self, rangeFrom, rangeTo, redrawFiles] (int r)
        {
            if (r == 8) { appPtr->redrawWaveforms (redrawFiles); return; }
            auto* ed2 = appPtr->project.findEdit (eid);
            auto* p2 = ed2 ? ed2->findAny (rid) : nullptr;
            if (p2 == nullptr) return;
            if (r == 1) showVolumeChange (*appPtr, eid, rid, rel);
            else if (r == 2) showVolumeChange (*appPtr, eid, rid, 0);
            else if (r == 3) { p2->gains.clear(); p2->normaliseIn = false; appPtr->project.changed(); }
            else if (r == 7) showRangeVolumeChange (*appPtr, eid, rangeFrom, rangeTo);
            else if (r == 5) fixtools::relinkEdit (*appPtr, eid, rid, self.getComponent());
            else if (r == 6) { p2->waiting = WaitingPiece(); appPtr->project.changed(); }
            else if (r == 4) { if (appPtr->isPlaying()) appPtr->stopPlayback(); ed2->removeOverdub (rid); appPtr->project.changed(); }
        });
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
        saver.saveFinished (thumbs, wavesFolder()); updateSize(); repaint();
    }
    void timerCallback() override
    {
        if (const int held = modhint::heldFor (*this); held != shownMods)      // the corner box: what the keys being held do here
        {
            shownMods = held;
            repaint (viewport.getViewPositionX(), viewport.getViewPositionY(), kNameW, kRulerH + kLaneH);
        }
        if (auto* ed = edit())                      // the playhead moved by itself (Stop leaves it where playback got to): the cursor follows
            if (std::abs (ed->playheadSeconds - lastHead) > 1.0e-9) { lastHead = ed->playheadSeconds; cursorSeconds = ed->playheadSeconds; repaint(); }
        // Waveforms being read (after an import, or the first time a project is shown) report every little step. Redrawing the whole window for each
        // one kept the program busy; now it is redrawn at most about 6 times a second while they come in.
        if (thumbsChanged && juce::Time::getMillisecondCounter() - lastThumbRefresh >= 150)
        {
            thumbsChanged = false; lastThumbRefresh = juce::Time::getMillisecondCounter();
            saver.saveFinished (thumbs, wavesFolder()); repaint();
        }
        const bool meterMoving = updateMeterFall();
        // While it plays only the meters are redrawn here: the playhead moves by itself with the screen (movePlayhead), redrawing just the thin strip
        // it leaves and the one it moves to. Redrawing every waveform 30 times a second is what made it stutter with many tracks.
        if (app.isPlaying()) { updateSize(); followPlayhead(); repaint (viewport.getViewPositionX(), kRulerH + kLaneH, kNameW, getHeight()); }
        else if (app.engine.isRecording() || meterMoving || ++meterTick % 3 == 0) repaint (viewport.getViewPositionX(), kRulerH + kLaneH, kNameW, getHeight());
        else followActive = false;
    }
    bool thumbsChanged = false; juce::uint32 lastThumbRefresh = 0;

    /** Called at every refresh of the screen (VBlankAttachment): puts the moving playhead where the music is now. Only the strip under its old place
        and under its new one are redrawn, so it can move smoothly at the screen's own rate however many tracks the window shows. */
    static constexpr int kNoLine = std::numeric_limits<int>::min();
    void movePlayhead()
    {
        double ph = -1.0;
        if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId) ph = app.playheadSecondsSmooth();
        const int x = ph >= 0.0 ? xOf (ph) : kNoLine;
        if (x == shownPhX) return;
        const int h = juce::jmax (1, getHeight() - kRulerH);
        if (shownPhX != kNoLine) repaint (theme::playheadStrip (shownPhX, kRulerH, h));      // (the whole reach of the line and its glow, or a trail of glow is left behind it)
        if (x != kNoLine) repaint (theme::playheadStrip (x, kRulerH, h));
        shownPhX = x;
    }
    int shownPhX = kNoLine;
    double lastHead = 0.0;
    bool widthDrag = false; int meterTick = 0;
    bool followActive = false; int lastFollowX = 0;
    /** While this edit plays: when the playhead reaches the right edge the view turns a whole page (also beyond the end of the audio). */
    void followPlayhead()
    {
        const double ph = app.playheadSeconds();
        if (! (ph >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)) { followActive = false; return; }
        const int x = xOf (ph), vx = viewport.getViewPositionX(), right = vx + viewport.getMaximumVisibleWidth() - 6;
        const bool outside = x > right || x < vx + kNameW;
        if (! followActive) { followActive = true; if (outside) viewport.setViewPosition (juce::jmax (0, x - kNameW - 12), viewport.getViewPositionY()); }
        else if (x > right && lastFollowX <= right) viewport.setViewPosition (juce::jmax (0, x - kNameW - 12), viewport.getViewPositionY());
        lastFollowX = x;
    }

    AppContext& app;
    juce::Uuid editId;
    juce::Viewport& viewport;
    juce::AudioFormatManager formatManager;
    WaveThumb::Pool thumbPool;
    DirectWaveCache directWave { formatManager };
    std::map<juce::String, std::unique_ptr<WaveThumb>> thumbs;
    bool dragging = false; int dragIndex = -1; std::vector<EditRegion> baseRegions;
    bool markDrag = false, markActive = false, markAlt = false; double markAnchor = 0.0; int markStartX = 0, markRow0 = 0;     // dragging over audio that is not selected sets marks 1 and 2
    bool dragOverdub = false; juce::int64 overdubBase = 0;
    bool insertDrag = false, insertSnapped = true; int insertShownX = 0, insertDragIndex = 0;                 // the insert flag being dragged
    juce::Uuid shuffleId; juce::int64 shuffleGrab = 0, shuffleSample = 0; bool shuffleMoved = false; int shuffleStartX = 0;     // a piece being shuffled by its banner
    int fadeDrag = 0; juce::Uuid fadeId; double fadeBase = 0.0;      // dragging a piece's fade-in (1) or fade-out (2) from its corner
    juce::VBlankAttachment vblank { this, [this] { movePlayhead(); } };     // (last, so everything it uses exists first)
};

struct EditViewport : public juce::Viewport
{
    void visibleAreaChanged (const juce::Rectangle<int>&) override { if (auto* c = getViewedComponent()) c->repaint(); }
};

EditWindowComponent::EditWindowComponent (AppContext& a, const juce::Uuid& id) : app (a), editId (id)
{
    getProperties().set (ownsRKeyProperty(), true);         // R here butts the selected pieces together: it does not start a recording
    auto* vp = new EditViewport();
    timeline.reset (new EditTimeline (app, editId, *vp));
    vp->setViewedComponent (timeline.get(), false);
    vp->setScrollBarsShown (true, true);
    addAndMakeVisible (vp);
    viewportOwner.reset (vp);
    setWantsKeyboardFocus (true);
    keysKeeper = std::make_unique<KeepKeysOnTimeline> (*vp, *timeline);

    nameCaption.setText ("Piece / Edit Window Name:", juce::dontSendNotification);
    addAndMakeVisible (nameBlock); addAndMakeVisible (infoLabel);

    // the time readout: where this edit's playhead is (moving while the edit plays); white when stopped, blue while anything plays, red while recording
    addAndMakeVisible (clock);
    clock.follow ([this]
                  {
                      if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId) return juce::jmax (0.0, app.playheadSecondsSmooth());
                      return transportNow();
                  },
                  [this] { return app.engine.isRecording() ? TimeReadout::State::recording : app.isPlaying() ? TimeReadout::State::playing : TimeReadout::State::stopped; });
    clock.onJump = [this] (double s) { goTo (s); };
    clock.onFormatChanged = [this] { resized(); };
    nameEditor.setText (edit() ? edit()->name : "", false);
    auto commitEditName = [this]
    {
        if (auto* e = edit()) if (nameEditor.getText().isNotEmpty() && e->name != nameEditor.getText()) { e->name = nameEditor.getText(); if (app.project.syncEditMixers()) app.project.structureChanged(); else app.project.changed(); }
    };
    nameEditor.onFocusLost = commitEditName;
    nameEditor.onReturnKey = [this, commitEditName] { commitEditName(); timeline->grabKeyboardFocus(); };     // Enter: the keyboard goes back to the timeline (its keys work at once)
    infoLabel.setColour (juce::Label::textColourId, theme::text);

    tracksButton.setTooltip ("The tracks (audio rows) of THIS edit: add one, remove one, or show every project track again. Drag a track's name up or down to change the order. Each track has its own mixer strip.");
    tracksButton.onClick = [this] { showTracksMenu(); };
    importButton.setTooltip ("Bring a multitrack recording (a multichannel / polyphonic WAV, or one mono / stereo file per track) into THIS edit, e.g. an already edited programme to mix. "
                             "You can also drag the files onto this window. Each track goes on its own row.");
    importButton.onClick = [this] { fixtools::showImportMenu (app, juce::Uuid::null(), &importButton, editId); };
    addAndMakeVisible (importButton);
    feedbackButton.setTooltip ("Comments from listeners: import the file they sent back (or a marker list exported from a review website) and see their comments as flags along the top of this timeline. The file they send back ends in .fmnotes; a file with all the pieces puts each piece's comments on its own edit. Make the listening copy in Mastering, on the Listening copy tab.");
    feedbackButton.onClick = [this] { showFeedbackMenu(); };
    addAndMakeVisible (feedbackButton); feedbackButton.setWantsKeyboardFocus (false);
    markersToggle.setTooltip ("Show or hide your own markers (the amber flags on the top line). Right-click that line to add one.");
    feedbackToggle.setTooltip ("Show or hide the listeners' comment flags (the teal flags on the line under the markers).");
    if (auto* e0 = edit()) { markersToggle.setToggleState (e0->showMarkers, juce::dontSendNotification); feedbackToggle.setToggleState (e0->showFeedback, juce::dontSendNotification); }
    markersToggle.onClick = [this] { if (auto* e1 = edit()) { e1->showMarkers = markersToggle.getToggleState(); app.project.markDirty(); timeline->repaint(); } };
    feedbackToggle.onClick = [this] { if (auto* e1 = edit()) { e1->showFeedback = feedbackToggle.getToggleState(); app.project.markDirty(); timeline->repaint(); } };
    for (auto* b : { &markersToggle, &feedbackToggle }) { addAndMakeVisible (b); b->setWantsKeyboardFocus (false); }
    addAndMakeVisible (tracksButton);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<EditWindowComponent> (this)] { if (safe != nullptr) safe->checkMixerStrips(); });
    playButton.onClick = [this] { togglePlay(); };
    startButton.setTooltip ("Go to the start of the edit [Home]. While playing, play carries on from there.");
    backButton.setTooltip ("Back 5 seconds");
    fwdButton.setTooltip ("Forward 5 seconds");
    endTransportButton.setTooltip ("Go to the end of the last piece [End]. You can play on beyond it: the silence after the audio plays too.");
    startButton.onClick = [this] { goTo (0.0); };
    backButton.onClick = [this] { jumpBy (-5.0); };
    fwdButton.onClick = [this] { jumpBy (5.0); };
    endTransportButton.onClick = [this] { goToEnd(); };
    for (auto* b : { &startButton, &backButton, &fwdButton, &endTransportButton }) { addAndMakeVisible (b); b->setWantsKeyboardFocus (false); }
    trimButton.onClick = [this] { trimSelected(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    leftButton.onClick = [this] { moveSelected (-1); };
    rightButton.onClick = [this] { moveSelected (1); };
    endButton.onClick = [this] { if (auto* e = edit()) { e->insertIndex = -1; app.project.changed(); } };
    bounceButton.onClick = [this] { openBounce(); };
    automationButton.setClickingTogglesState (true);
    automationButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc28a00));
    automationButton.setTooltip ("Automation: shows the automation rows - draw fader, pan, gain, send and plug-in moves on every track, Int Bus, Ext Bus and VCA (the buses and VCAs are listed under the tracks). "
                                 "Click to add a point, drag to move it, right-click / Shift-click to delete. The label on the left of a row chooses what it shows; the + box adds another row under a track or bus "
                                 "and the - box takes one away. What is drawn plays (and is part of Bounce Out) whether the rows are shown or not. The points stay on the same music when the edit changes. Restart playback to hear changes.");
    automationButton.setToggleState (edit() != nullptr && edit()->automationOn, juce::dontSendNotification);
    automationButton.onClick = [this] { if (auto* e = edit()) { e->automationOn = automationButton.getToggleState(); app.project.changed(); timeline->updateSize(); timeline->repaint(); } };       // (the rows change: the buses and VCAs come under the tracks, or go)
    // With no 1 / 2 flags set, a piece that was selected whole (Ctrl + click) is loaded in full into the process window.
    auto withWholePiece = [this] (std::function<void()> run)
    {
        auto* e = edit(); bool temp = false;
        if (e != nullptr && (e->fixIn < 0.0 || e->fixOut <= e->fixIn) && ! timeline->selectedRegion.isNull() && ! e->isOverdub (timeline->selectedRegion))
            if (auto* r = e->findAny (timeline->selectedRegion))
            {
                const double rt = e->sampleRate > 0 ? e->sampleRate : 48000.0;
                e->fixIn = (double) r->startSample / rt; e->fixOut = (double) r->endSample() / rt; e->fixTracks.clear(); temp = true;
            }
        run();
        if (temp) if (auto* e2 = edit()) { e2->fixIn = -1.0; e2->fixOut = -1.0; }
    };
    pitchButton.setTooltip ("Two tabs. Pitch curve: draw a line of pitch against time between marks 1 and 2 (+100 cents at the top, -100 at the bottom), for a note that drifts flat or sharp; audition it, then accept or revert. "
                            "Pitch process: shifts the audio between the marks by semitones and / or cents (it can also process extra audio at each end, up to 9 s, so the edit points can be moved outward later).");
    pitchButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::pitchTabsEdit (app, editId, this); }); };
    repairButton.setTooltip ("Shows the audio between marks 1 and 2 of all tracks as one spectrogram. Draw a box round a noise (drag its edges to adjust it) and rebuild it from the clean sound next to it (ReTouch), turn it up or down (Gain), "
                             "take the background noise out of it with the room tone (Denoise), or use De-click to find and mend clicks. Key: Enter, with marks 1 and 2 set.");
    repairButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::repairEdit (app, editId, this, false); }); };
    roomToneButton.setTooltip ("Room tone: tells Fermata which part of the recording holds nothing but the room, on every track - a take marked as the room tone in a take window (right-click the take), "
                               "or marks 1 - 2 of this edit. Every track's noise is learned from it once and kept with the project; the ReTouch and Noise mode windows then denoise any box you draw with it.");
    roomToneButton.onClick = [this] { fixtools::roomToneEdit (app, editId, this); };
    noiseModeButton.setTooltip ("Noise mode: the whole edit in one spectral window (built once at several zoom levels) with Gain, ReTouch, De-click and the denoiser all to hand while it plays. "
                                "Each fix goes into the edit at once and the next works on the result; Undo takes one off; Write back to clip keeps them (the window stays open); Finished closes it. No marks needed.");
    noiseModeButton.onClick = [this] { fixtools::noiseModeEdit (app, editId, this); };
    reharmoniserButton.setTooltip ("ReHarmoniSer: retunes single out-of-tune notes in the audio between marks 1 and 2 of all tracks, every track in exactly the same way; check it by ear, then Write back to clip");
    reharmoniserButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::reharmoniserEdit (app, editId, this); }); };
    alignButton.setTooltip ("Align spot mics: measure how much earlier each spot mic hears the music than the main pair, on this edit (between marks 1 and 2, "
                            "or all of it), and delay it to line up - in every mixer; the files are not changed. Useful in post when there was no time in the session.");
    alignButton.onClick = [this] { if (app.showAlign) app.showAlign (true, editId); };
    exportProcButton.setTooltip ("Export for Processing: sends the part between marks 1 and 2 (all tracks) to the Processing Media folder to be corrected in other software (e.g. iZotope RX). Re-link it afterwards with a right-click on the 'Waiting for corrected audio' block");
    exportProcButton.onClick = [this] { fixtools::exportForProcessingEdit (app, editId, this); };
    undoFixButton.setTooltip ("Undo the last pitch correction, repair, de-click, denoise or export for processing (the original audio files were never touched)");
    undoFixButton.onClick = [this] { fixtools::undoLastFix (app); };
    bounceButton.setTooltip ("Make a master file of this edit through its own mixer (or another mixer)");
    bounceButton.setColour (juce::TextButton::buttonOnColourId, theme::accent);        // lit (gold) only while the Bounce Out window is open, which is also while it is bouncing (see timerCallback)
    mixerButton.setTooltip ("Opens the mixer of this edit. The edit always plays through it (and is bounced through it), so each piece can have its own mix. It started as a copy of the processing mixer");
    ltcButton.setTooltip ("Show or hide the LTC (timecode) track of this edit. The LTC pieces never fade or crossfade: each starts hard, and stops one sample before the next piece's fade-in begins.");
    ltcButton.onClick = [this]
    {
        if (auto* e = edit())
        {
            e->showLtc = ! e->showLtc;
            ltcButton.setButtonText (e->showLtc ? juce::String::fromUTF8 ("\xE2\x96\xBC LTC") : juce::String::fromUTF8 ("\xE2\x96\xB6 LTC"));
            app.project.changed(); timeline->repaint();
        }
    };
    if (auto* e0 = edit()) if (e0->showLtc) ltcButton.setButtonText (juce::String::fromUTF8 ("\xE2\x96\xBC LTC"));
    mixerButton.onClick = [this] { if (auto* m = app.project.mixerOfEdit (editId)) { if (app.showMixer) app.showMixer (m->id); } };
    zoomInButton.onClick = [this] { timeline->zoom (1.5); };
    zoomOutButton.onClick = [this] { timeline->zoom (1.0 / 1.5); };
    slipLeftToggle.setTooltip ("When you slide a piece, every piece BEFORE it slides with it (off: they stay where they are).");
    slipRightToggle.setTooltip ("When you slide a piece, every piece AFTER it slides with it (off: they stay where they are).");
    shuffleToggle.setTooltip ("Shuffle on: dragging a piece by its top banner past a neighbour swaps the two, like cards in a deck. Off: a piece goes wherever you drop it, leaving silence if you want.");
    insertFlagToggle.setToggleState (app.showInsertFlag, juce::dontSendNotification);
    insertFlagToggle.setTooltip ("Show or hide the INSERT flag (where the next take will be placed). Hide it while you are only working on fades.");
    insertFlagToggle.onClick = [this] { app.setShowInsertFlag (insertFlagToggle.getToggleState()); };
    shuffleToggle.onClick = [this] { timeline->shuffleMode = shuffleToggle.getToggleState(); };
    slipLeftToggle.setToggleState (app.trimSlipLeft, juce::dontSendNotification);          // the same Slip Left / Slip Right setting as the trim window: on by default, remembered
    slipRightToggle.setToggleState (app.trimSlipRight, juce::dontSendNotification);
    timeline->slipLeft = app.trimSlipLeft; timeline->slipRight = app.trimSlipRight;
    slipLeftToggle.onClick  = [this] { timeline->slipLeft = slipLeftToggle.getToggleState(); app.setTrimSlip (slipLeftToggle.getToggleState(), slipRightToggle.getToggleState()); };
    slipRightToggle.onClick = [this] { timeline->slipRight = slipRightToggle.getToggleState(); app.setTrimSlip (slipLeftToggle.getToggleState(), slipRightToggle.getToggleState()); };
    for (auto* b : std::initializer_list<juce::Button*> { &playButton, &trimButton, &deleteButton, &leftButton, &rightButton, &endButton, &zoomInButton, &zoomOutButton, &bounceButton, &mixerButton, &ltcButton, &automationButton, &pitchButton, &repairButton, &roomToneButton, &noiseModeButton, &reharmoniserButton, &exportProcButton, &undoFixButton, &alignButton,
                                                          &slipLeftToggle, &slipRightToggle, &shuffleToggle, &insertFlagToggle })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false);
    }
    trimButton.setColour (juce::TextButton::buttonOnColourId, theme::accent);          // lit (gold) only while this edit's Trim window is open (see timerCallback)
    playButton.setIcon (Icon::Play, "Play / Stop [Space]"); startButton.setIcon (Icon::Start, "Return to start [Home]"); backButton.setIcon (Icon::Rewind, "Rewind 5 seconds");
    fwdButton.setIcon (Icon::Forward, "Forward 5 seconds"); endTransportButton.setIcon (Icon::End, "Go to the end [End]");
    trimButton.setIcon (Icon::Trim, "Trim Window [T]"); deleteButton.setIcon (Icon::Delete, "Delete piece"); leftButton.setIcon (Icon::Earlier, "Move earlier"); rightButton.setIcon (Icon::Later, "Move later");
    bounceButton.setIcon (Icon::Bounce, "Bounce Out"); mixerButton.setIcon (Icon::Mixer, "Edit Mixer"); automationButton.setIcon (Icon::Automation, "Automation");
    pitchButton.setIcon (Icon::Pitch, "Pitch & Pitch curve"); repairButton.setIcon (Icon::Repair, "ReTouch, DeClick & Denoise [Enter]"); reharmoniserButton.setIcon (Icon::Reharm, "ReHarmoniSer [H]"); alignButton.setIcon (Icon::Align, "Align spot mics");
    roomToneButton.setIcon (Icon::RoomTone, "Room tone"); noiseModeButton.setIcon (Icon::NoiseMode, "Noise mode");
    undoFixButton.setIcon (Icon::Undo, "Undo fix"); tracksButton.setIcon (Icon::Tracks, "Tracks"); feedbackButton.setIcon (Icon::Feedback, "Feedback");
    endButton.setIcon (Icon::NextAtEnd, "Next goes at end"); exportProcButton.setIcon (Icon::ExportProc, "Export for Processing"); importButton.setIcon (Icon::ImportMulti, "Multitrack import");
    markersToggle.setIcon (Icon::Markers, "Markers"); feedbackToggle.setIcon (Icon::FeedbackMarkers, "Feedback markers");
    slipLeftToggle.setIcon (Icon::SlipLeft, "Slip Left"); slipRightToggle.setIcon (Icon::SlipRight, "Slip Right"); shuffleToggle.setIcon (Icon::Shuffle, "Shuffle"); insertFlagToggle.setIcon (Icon::InsertFlag, "Insert flag");
    for (auto* t : { &markersToggle, &feedbackToggle, &slipLeftToggle, &slipRightToggle, &shuffleToggle, &insertFlagToggle })
    { t->setClickingTogglesState (true); t->setColour (juce::TextButton::buttonOnColourId, theme::accent); }
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
    loopToggle.setTooltip ("Loop: when lit, Play repeats the area between marks 1 and 2 (the same marks the pitch and repair tools use) over and over until you press Stop. Key L.");
    loopToggle.onClick = [this] { loopChanged(); };
    loopToggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (loopToggle);

    timeline->updateSize();
    app.project.addChangeListener (this);
    startTimerHz (10);
    setSize (juce::jlimit (900, 1700, juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.getWidth() - 60), 420);
}

EditWindowComponent::~EditWindowComponent() { app.clearTransportHandler (this); app.project.removeChangeListener (this); }

void EditWindowComponent::resized()
{
    auto r = getLocalBounds().reduced (6);
    using grid::FlowItem;
    constexpr int IW = 34, IH = 26;                               // the picture buttons
    const int clockW = clock.getIdealWidth (kClockH) + 4;
    // the picture buttons: to the right of the time readout, on as many lines as they need (two fit in the readout's height)
    const std::vector<FlowItem> items
    {
        { &startButton, IW, IH, 2 }, { &backButton, IW, IH, 2 }, { &playButton, IW, IH, 2 }, { &fwdButton, IW, IH, 2 }, { &endTransportButton, IW, IH, 6 }, { &playheadMode, 34, grid::btnH, 2 }, { &waveColourBtn, 34, grid::btnH, 2 }, { &loopToggle, 34, grid::btnH, 14 },
        { &trimButton, IW, IH, 2 }, { &deleteButton, IW, IH, 14 },
        { &leftButton, IW, IH, 2 }, { &rightButton, IW, IH, 6 }, { &endButton, IW, IH, 14 },
        { &bounceButton, IW, IH, 2 }, { &mixerButton, IW, IH, 6 }, { &ltcButton, 74, grid::btnH, 14 },
        { &automationButton, IW, IH, 14 },
        { &pitchButton, IW, IH, 2 }, { &repairButton, IW, IH, 2 }, { &roomToneButton, IW, IH, 2 }, { &noiseModeButton, IW, IH, 2 }, { &reharmoniserButton, IW, IH, 10 }, { &exportProcButton, IW + 8, IH, 2 }, { &undoFixButton, IW, IH, 14 }, { &alignButton, IW, IH, 14 },
        { &tracksButton, IW, IH, 2 }, { &importButton, IW + 8, IH, 14 }, { &feedbackButton, IW, IH, 8 }, { &markersToggle, IW, IH, 2 }, { &feedbackToggle, IW + 8, IH, 14 },
        { &slipLeftToggle, IW + 8, IH, 2 }, { &slipRightToggle, IW + 8, IH, 6 }, { &shuffleToggle, IW, IH, 2 }, { &insertFlagToggle, IW, IH, 14 },
        { &zoomOutButton, 34 }, { &zoomInButton, 34 }
    };
    // the head: the edit's name (its caption above it), then the time readout; the buttons to the right of the readout, a group never split between two lines
    const int y0 = r.getY() + 4;
    int x = r.getX();
    nameBlock.setBounds (x, y0, 240, nameBlock.idealHeight()); x += 240 + 10;
    clock.setBounds (x, y0, clockW, kClockH);                  x += clockW + 14;
    int headH = juce::jmax (nameBlock.getBottom(), y0 + kClockH + 4) - r.getY();
    const int btnRight = r.getRight() - grid::cornerReserve;      // (clear of the pin and sun / moon buttons in the corner)
    if (btnRight - x >= 300)
        headH = juce::jmax (headH, grid::flow (juce::Rectangle<int> (x, r.getY(), btnRight - x, headH), items, true, nullptr, true));
    else                                                       // a narrow window: the buttons go under the head, across the whole width
        headH += grid::flow (juce::Rectangle<int> (r.getX(), r.getY() + headH, r.getWidth(), 0), items, true, nullptr, true);
    r.removeFromTop (headH + 2);
    {
        auto row = r.removeFromTop (22);
        infoLabel.setBounds (row.reduced (4, 0));
    }
    r.removeFromTop (4);
    if (viewportOwner != nullptr) { viewportOwner->setBounds (r); if (timeline != nullptr) timeline->updateSize(); }
}

bool EditWindowComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) { juce::File x (f); if (x.isDirectory() || fixtools::isAudioFile (x)) return true; }
    return false;
}

void EditWindowComponent::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> list; for (auto& f : files) list.add (juce::File (f));
    fixtools::importTakes (app, juce::Uuid::null(), list, this, editId);
}

double EditWindowComponent::transportNow() const
{
    auto* e = edit();
    if (e == nullptr) return 0.0;
    if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId) return juce::jmax (0.0, app.playheadSeconds());
    return timeline->cursorSeconds >= 0 ? timeline->cursorSeconds : e->playheadSeconds;
}

void EditWindowComponent::goTo (double seconds)
{
    auto* e = edit();
    if (e == nullptr || app.engine.isRecording()) return;
    seconds = juce::jmax (0.0, seconds);
    e->playheadSeconds = seconds; timeline->cursorSeconds = seconds;
    if (app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)
    {
        const auto err = app.playEdit (editId, seconds);
        if (err.isNotEmpty()) showError ("Play", err);
    }
    app.project.changed();
    timeline->scrollTo (seconds);
}

void EditWindowComponent::goToEnd() { if (auto* e = edit()) goTo (e->lengthSeconds()); }

// ----------------------------------------------------------------------------- listeners' comments
void EditWindowComponent::showFeedbackMenu()
{
    auto* e = edit(); if (e == nullptr) return;
    int open = 0, done = 0; for (auto& n : e->feedback) (n.done ? done : open)++;
    juce::PopupMenu m;
    m.addItem (1, "Import comments from a file...");
    m.addSeparator();
    m.addItem (3, "Mark every comment as done", open > 0);
    m.addItem (4, "Take away the comments marked done (" + juce::String (done) + ")", done > 0);
    m.addItem (5, "Take away all comments (" + juce::String ((int) e->feedback.size()) + ")...", ! e->feedback.empty());
    juce::Component::SafePointer<EditWindowComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&feedbackButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto* ed = safe->edit(); if (ed == nullptr) return;
        auto& app = safe->app;
        if (r == 1) safe->importFeedback();
        else if (r == 3) { for (auto& n : ed->feedback) n.done = true; app.project.markDirty(); safe->timeline->repaint(); }
        else if (r == 4)
        {
            ed->feedback.erase (std::remove_if (ed->feedback.begin(), ed->feedback.end(), [] (const FeedbackNote& n) { return n.done; }), ed->feedback.end());
            app.project.markDirty(); safe->timeline->repaint();
        }
        else if (r == 5)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle ("Take away all comments").withMessage ("Take every listener comment off this edit? (The files they came from are not touched; you can import them again.)")
                .withButton ("Take away").withButton ("Cancel").withAssociatedComponent (safe.getComponent()),
                [safe] (int result)
                {
                    if (safe == nullptr || result != 1) return;
                    if (auto* ed2 = safe->edit()) { ed2->feedback.clear(); safe->app.project.markDirty(); safe->timeline->repaint(); }
                });
        }
    });
}

void EditWindowComponent::importFeedback()
{
    auto* st = app.props.getUserSettings();
    juce::File start (st != nullptr ? st->getValue ("feedbackFolder") : juce::String());
    if (! start.isDirectory()) start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    feedbackChooser = std::make_unique<juce::FileChooser> ("Choose the comments file(s) sent back by listeners (or exported from a review website)", start, "*.fmnotes;*.json;*.txt;*.csv;*.tsv");
    juce::Component::SafePointer<EditWindowComponent> safe (this);
    feedbackChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto files = fc.getResults(); files.removeIf ([] (const juce::File& f) { return f == juce::File(); });
        if (files.isEmpty()) return;
        if (auto* st2 = safe->app.props.getUserSettings()) { st2->setValue ("feedbackFolder", files[0].getParentDirectory().getFullPathName()); safe->app.props.saveIfNeeded(); }
        bool needFps = false;
        for (auto& f : files)                                                    // JSON (our own listening copy) has plain seconds; a marker list has timecode with frames
        {
            juce::FileInputStream in (f); char c[64] = {}; const int n = in.failedToOpen() ? 0 : in.read (c, 63);
            const juce::String head = juce::String::fromUTF8 (c, n).trimStart().trimCharactersAtStart (juce::String::charToString (0xFEFF)).trimStart();
            if (! head.startsWithChar ('{') && ! head.startsWithChar ('[')) needFps = true;
        }
        double fps = 25.0;
        if (auto* st2 = safe->app.props.getUserSettings()) fps = st2->getDoubleValue ("feedbackFps", 25.0);
        if (! needFps) { safe->importFeedbackFiles (files, fps); return; }
        auto* w = new juce::AlertWindow ("Timecode in this file", "The times in this file are timecode: hours:minutes:seconds:FRAMES. "
                                         "How many frames a second does the website use? (It only moves a comment by less than a second, so a good guess is fine. 25 is usual in the UK and Europe.)", juce::MessageBoxIconType::QuestionIcon);
        const char* names[] = { "24", "25", "29.97", "30", "50", "60" }; const double vals[] = { 24, 25, 29.97, 30, 50, 60 };
        juce::StringArray items; int sel = 2; for (int i = 0; i < 6; ++i) { items.add (names[i]); if (std::abs (vals[i] - fps) < 0.01) sel = i + 1; }
        w->addComboBox ("fps", items, "Frames per second");
        w->getComboBoxComponent ("fps")->setSelectedId (sel, juce::dontSendNotification);
        w->addButton ("Import", 1, juce::KeyPress (juce::KeyPress::returnKey)); w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, files, vals] (int r)
        {
            const int id = w->getComboBoxComponent ("fps")->getSelectedId();
            if (safe == nullptr || r != 1) return;
            const double f = vals[juce::jlimit (0, 5, id - 1)];
            if (auto* st3 = safe->app.props.getUserSettings()) { st3->setValue ("feedbackFps", f); safe->app.props.saveIfNeeded(); }
            safe->importFeedbackFiles (files, f);
        }), true);
    });
}

void EditWindowComponent::importFeedbackFiles (juce::Array<juce::File> files, double frameRate)
{
    juce::String report; int total = 0; bool toOthers = false;
    std::vector<juce::Uuid> replaced;                         // edits whose old comments have been superseded by this import (the first file of a batch replaces, the rest add)
    std::map<juce::String, std::vector<juce::File>> kept;     // the files read for each edit, saved in the project's comments folder
    for (auto& f : files)
    {
        juce::MemoryBlock data;
        if (! f.loadFileAsData (data)) { report += f.getFileName() + ": could not be read.\n"; continue; }
        feedback::Parsed parsed;
        const auto err = feedback::parse (data.getData(), data.getSize(), frameRate, f.getFileName(), parsed);
        if (err.isNotEmpty()) { report += f.getFileName() + ": " + err + "\n"; continue; }
        const bool several = ! parsed.parts.empty();                  // a listening copy that held all the pieces: each piece's comments go to its own edit
        std::vector<feedback::Parsed*> groups;
        if (several) for (auto& part : parsed.parts) groups.push_back (&part); else groups.push_back (&parsed);
        for (auto* gp : groups)
        {
            auto& p = *gp;
            const auto label = f.getFileName() + (several ? " - " + p.piece : juce::String());
            EditDef* target = several ? nullptr : edit();
            juce::String where;
            if (p.editId.isNotEmpty())
                if (auto* te = app.project.findEdit (juce::Uuid (p.editId))) { if (te != target && ! several) where = " (they were made on the edit '" + te->name + "', so they went there)"; target = te; }
            if (target == nullptr && several)
                for (auto& e2 : app.project.edits) if (e2->name == p.piece) { target = e2.get(); break; }
            if (target == nullptr)
            {
                if (several) report += label + ": " + juce::String ((int) p.notes.size()) + " comment" + (p.notes.size() == 1 ? "" : "s") + " not imported: the edit they were made on is no longer in this project.\n";
                continue;
            }
            const double offset = p.hasOffset ? p.offset : (target->sampleRate > 0.0 ? (double) target->firstSample() / target->sampleRate : 0.0);
            const double len = target->lengthSeconds();
            int late = 0;
            for (auto& n : p.notes) { n.seconds += offset; if (len > 0.0 && n.seconds > len + 2.0) ++late; }
            int updated = 0;
            if (std::find (replaced.begin(), replaced.end(), target->id) == replaced.end())
            {   // a new set of comments supersedes the old one; a tick ("done") on a comment that is still in the new set is kept
                replaced.push_back (target->id);
                auto oldNotes = target->feedback; target->feedback.clear();
                for (auto& n : p.notes) for (auto& o : oldNotes) if (o.key == n.key && o.done) n.done = true;
            }
            kept[target->id.toString()].push_back (f);
            const int added = feedback::merge (target->feedback, p.notes, &updated);
            target->showFeedback = true;
            total += added;
            if (target != edit()) toOthers = true;
            report += label + ": " + juce::String (added) + " new comment" + (added == 1 ? "" : "s") + " from " + juce::String ((int) p.notes.size()) + " in the file" + where + (several ? " (on the edit '" + target->name + "')" : juce::String())
                    + (updated > 0 ? ", " + juce::String (updated) + " already there and updated" : juce::String())
                    + (p.format == "marker list" ? ". Timecode read at " + juce::String (frameRate) + " frames a second" : juce::String())
                    + (late > 0 ? ". " + juce::String (late) + " fall after the end of the edit" : juce::String()) + ".\n";
        }
    }
    for (auto& id : replaced) if (auto* te = app.project.findEdit (id)) app.project.saveCommentsCopy (*te, kept[id.toString()]);
    app.project.markDirty();
    if (timeline != nullptr) timeline->repaint();
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (total > 0 ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon)
        .withTitle ("Comments").withMessage (report.trim() + (total > 0 ? "\n\nThe flags are along the top of the timeline. Click one to read it." + juce::String (toOthers ? " Comments on other pieces are on their own edits: open those edits to see them." : "") : juce::String())).withButton ("OK").withAssociatedComponent (this), nullptr);
}

void EditWindowComponent::showTracksMenu()
{
    auto* e = edit();
    if (e == nullptr) return;
    juce::PopupMenu m;
    m.addSectionHeader ("Tracks of this edit");
    m.addItem (1, "Add a new mono track");
    m.addItem (2, "Add a new stereo track");
    const auto shown = app.project.editTracks (*e);
    bool anyOther = false;
    juce::PopupMenu add;
    for (size_t i = 0; i < app.project.tracks.size(); ++i)
    {
        bool in = false; for (auto* t : shown) if (t->id == app.project.tracks[i].id) in = true;
        if (! in) { add.addItem (1000 + (int) i, app.project.tracks[i].name); anyOther = true; }
    }
    m.addSubMenu ("Add a track the project already has", add, anyOther);
    juce::PopupMenu rem;
    for (size_t i = 0; i < shown.size(); ++i) rem.addItem (2000 + (int) i, shown[i]->name);
    m.addSubMenu ("Remove a track (its audio leaves this edit; Undo brings it back)", rem, shown.size() > 0);
    m.addSeparator();
    m.addItem (3, "Show every project track, in the project's order", ! e->trackIds.empty());
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&tracksButton), [this] (int r)
    {
        auto* ed = edit();
        if (ed == nullptr || r == 0) return;
        if (r == 3) { ed->trackIds.clear(); app.project.changed(); return; }
        if (r == 1 || r == 2)
        {
            app.project.materialiseEditTracks (*ed);
            auto& t = app.project.addTrack ("Track " + juce::String ((int) app.project.tracks.size() + 1), r == 2 ? TrackFormat::Stereo : TrackFormat::Mono, 0);
            t.inputs.fill (-1);                                         // an edit track has no microphone: it only carries audio
            ed->trackIds.push_back (t.id);
            app.project.changed(); return;
        }
        if (r >= 1000 && r < 2000)
        {
            const int i = r - 1000;
            if (juce::isPositiveAndBelow (i, (int) app.project.tracks.size()))
            {
                app.project.materialiseEditTracks (*ed);
                ed->trackIds.push_back (app.project.tracks[(size_t) i].id); app.project.changed();
            }
            return;
        }
        if (r >= 2000)
        {
            const auto sh = app.project.editTracks (*ed);
            const int i = r - 2000;
            if (juce::isPositiveAndBelow (i, (int) sh.size())) removeTrackFromEdit (sh[(size_t) i]->id);
        }
    });
}

void EditWindowComponent::removeTrackFromEdit (const juce::Uuid& trackId)
{
    auto* e = edit();
    if (e == nullptr) return;
    app.project.materialiseEditTracks (*e);
    e->trackIds.erase (std::remove (e->trackIds.begin(), e->trackIds.end(), trackId), e->trackIds.end());
    auto strip = [&] (std::vector<EditRegion>& rs)
    {
        for (auto& r : rs)
            for (int fi = (int) r.files.size() - 1; fi >= 0; --fi)
                if (r.files[(size_t) fi].trackId == trackId)
                {
                    r.files.erase (r.files.begin() + fi);
                    for (auto& g : r.gains) if (fi < (int) g.db.size()) g.db.erase (g.db.begin() + fi);       // the volume changes are lined up with the files
                }
    };
    strip (e->regions); strip (e->overdubs);
    e->lanes.erase (std::remove_if (e->lanes.begin(), e->lanes.end(), [&] (const AutoLane& l) { return l.trackId == trackId; }), e->lanes.end());
    app.project.changed();
}

void EditWindowComponent::checkMixerStrips()
{
    auto* e = edit();
    if (e == nullptr || app.isPlaying()) return;
    const auto missing = app.project.missingTracksOf (*e);
    if (missing.empty() || stripAskOpen) return;
    stripAskOpen = true;
    juce::String names; for (auto& m : missing) names << (names.isEmpty() ? "" : ", ") << m.second.first;
    const auto id = editId;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
        .withTitle ("More audio tracks than mixer strips")
        .withMessage ("There are more audio tracks in this edit (" + names + ") than there are mixer strips for them. Would you like to add the missing strips?\n\n"
                      "Yes: the tracks come back (without a microphone input), with a strip in every mixer, and play.\n"
                      "No: the audio of those tracks stays silent.")
        .withButton ("Yes, add the strips").withButton ("No, leave them silent")
        .withAssociatedComponent (this),
        [safe = juce::Component::SafePointer<EditWindowComponent> (this), this, id, missing] (int result)
        {
            if (safe == nullptr) return;
            stripAskOpen = false;
            if (result != 1) { skipStripAsk = true; return; }
            for (auto& m : missing) app.project.restoreTrack (m.first, m.second.first, m.second.second);
            if (auto* ed = app.project.findEdit (id)) if (! ed->trackIds.empty()) for (auto& m : missing) ed->trackIds.push_back (m.first);
            app.project.changed();
        });
}

void EditWindowComponent::togglePlay()
{
    if (app.isPlaying()) { app.stopPlayback(); return; }
    auto* e = edit();
    if (e == nullptr) return;
    if (! skipStripAsk) checkMixerStrips();
    double a = 0.0, b = 0.0;
    if (loopToggle.getToggleState() && loopRange (a, b))             // loop on and marks 1 / 2 set: go round that area
    {
        const auto err = app.playEdit (editId, a, b, true, true);
        if (err.isNotEmpty()) showError ("Play", err);
        return;
    }
    if (loopToggle.getToggleState()) app.setNotice ("Loop is on, but marks 1 and 2 are not both set: playing normally.");
    auto err = app.playEdit (editId, timeline->cursorSeconds >= 0 ? timeline->cursorSeconds : 0.0);
    if (err.isNotEmpty()) showError ("Play", err);
}

bool EditWindowComponent::loopRange (double& from, double& to) const
{
    auto* e = app.project.findEdit (editId);
    if (e == nullptr || e->fixIn < 0.0 || e->fixOut <= e->fixIn) return false;
    from = e->fixIn; to = e->fixOut;
    return true;
}

void EditWindowComponent::loopChanged()
{
    const bool on = loopToggle.getToggleState();
    const bool mine = app.isPlaying() && ! app.engine.isRecording() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId;
    double a = 0.0, b = 0.0;
    if (on && ! loopRange (a, b)) { app.setNotice ("Loop: set marks 1 and 2 first (the loop plays the area between them)."); return; }
    if (! mine) return;
    if (on) { const auto err = app.playEdit (editId, a, b, true, true); if (err.isNotEmpty()) showError ("Play", err); }        // switched on while playing: start going round now
    else app.engine.releasePlaybackLoop();                                                                                  // switched off: plays straight on past mark 2
}

/** R: the selected piece is moved so it starts exactly where the piece before it ends (everything after it moves with it), ready to be trimmed. */
void EditWindowComponent::buttSelected()
{
    auto* e = edit();
    if (e == nullptr) return;
    const int ri = e->indexOf (timeline->selectedRegion);
    if (ri < 0) { app.setNotice ("Click a piece first, then press R to butt it up against the piece before it"); return; }
    if (ri == 0) { app.setNotice ("This is the first piece: there is nothing before it to butt up against"); return; }
    const auto d = e->closeGap (ri);
    app.setNotice (d == 0 ? "It already meets the piece before it" : "Moved up against the piece before it (" + juce::String (std::abs (d) / juce::jmax (1.0, e->sampleRate) * 1000.0, 0) + " ms " + (d < 0 ? "earlier" : "later") + ")");
    if (d != 0) { app.project.changed(); timeline->repaint(); }
}

void EditWindowComponent::trimSelected()
{
    auto* e = edit();
    if (e == nullptr) return;
    int idx = timeline->selectedJoin;                 // an edit point (a numbered flag) that was clicked: 0 = the start of the edit, n = its end
    const int n = (int) e->regions.size();
    if (idx < 0)                          // a piece is selected: trim the join before it (or after it, for the first)
    {
        const int ri = e->indexOf (timeline->selectedRegion);
        idx = ri > 0 ? ri : (ri == 0 ? (n > 1 ? 1 : 0) : -1);
    }
    if (idx < 0 || idx > n || n == 0) { showError ("Trim", "Click one of the numbered flags at the top of the edit (a join, or the start or end of the edit), then press T."); return; }
    if (app.showTrim) app.showTrim (editId, e->regions[(size_t) (idx == n ? n - 1 : idx)].id, idx == n);
}

/** Key 4 here: take the part marked with 1 and 2 in a take window of this edit and lay it down at this edit's playhead. */
void EditWindowComponent::placeFromTakeWindow()
{
    TakeWindowDef* best = nullptr;
    for (auto& w : app.project.takeWindows)
        if (w->targetEdit == editId && w->findGroup (w->editTake) != nullptr && w->editIn >= 0.0 && w->editOut > w->editIn)
            if (best == nullptr || w->id == app.recordTarget) best = w.get();                // prefer the take window used last
    if (best == nullptr)
    {
        showError ("Place at playhead [4]", "Nothing is waiting to be placed. In a take window, press 1 and 2 to mark the part you want (it must send to this edit), "
                                            "then click here where it should go and press 4.");
        return;
    }
    auto err = app.copyMarkedToEdit (best->id, true, nullptr);
    if (err.isNotEmpty()) showError ("Place at playhead [4]", err);
}

void EditWindowComponent::deleteSelected()
{
    auto* e = edit();
    if (e == nullptr) return;
    if (e->isOverdub (timeline->selectedRegion))
    {
        if (app.isPlaying()) app.stopPlayback();
        e->removeOverdub (timeline->selectedRegion);
        timeline->selectedRegion = juce::Uuid::null(); timeline->selectedRegions.clear();
        app.project.changed();
        return;
    }
    const int i = e->indexOf (timeline->selectedRegion);
    if (i < 0) return;
    if (app.isPlaying()) app.stopPlayback();
    e->removeRegion (i);
    timeline->selectedRegion = juce::Uuid::null(); timeline->selectedJoin = -1;
    app.project.changed();
}

void EditWindowComponent::moveSelected (int delta)
{
    auto* e = edit();
    if (e == nullptr) return;
    const int i = e->indexOf (timeline->selectedRegion);
    if (i < 0) return;
    if (app.isPlaying()) app.stopPlayback();
    e->moveRegion (i, i + delta);
    app.project.changed();
}

/** 1 / 2: the part to pitch-correct or repair starts / ends at the playhead (or the cursor when nothing plays). */
/** A click on any empty part of the window (not a button) lets go of the selected pieces. */
void EditWindowComponent::mouseDown (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    grabKeyboardFocus();
    if (timeline != nullptr)
    {
        timeline->selectedRegion = juce::Uuid::null(); timeline->selectedRegions.clear(); timeline->moveRegion = juce::Uuid::null(); timeline->selectedJoin = -1;
        timeline->repaint();
    }
}

void EditWindowComponent::setFixMark (bool in)
{
    auto* e = edit();
    if (e == nullptr) return;
    double t = app.playheadSeconds();
    if (! (t >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)) t = timeline->cursorSeconds;
    if (t < 0) { showError (in ? "Mark 1" : "Mark 2", "Click on the timeline (or play the edit) to say where, then press " + juce::String (in ? "1" : "2") + "."); return; }
    (in ? e->fixIn : e->fixOut) = t;
    if (e->fixIn >= 0 && e->fixOut >= 0 && e->fixOut <= e->fixIn) (in ? e->fixOut : e->fixIn) = -1.0;
    app.project.changed();
}

/** P: with pieces selected, marks 1 and 2 go to the very start of the first and the very end of the last of them (sample-exact). */
void EditWindowComponent::markWholeSelected()
{
    auto* e = edit();
    if (e == nullptr) return;
    double from = 1e12, to = -1.0;
    auto take = [&] (const EditRegion& r)
    {
        if (! timeline->isSelected (r.id) || r.sampleRate <= 0) return;
        from = juce::jmin (from, (double) r.startSample / r.sampleRate);
        to   = juce::jmax (to,   (double) r.endSample()  / r.sampleRate);
    };
    for (auto& r : e->regions)  take (r);
    for (auto& r : e->overdubs) take (r);
    if (to < 0) { showError ("Marks 1 and 2", "Click a piece first (or Ctrl + click several), then press P to put marks 1 and 2 round all of it."); return; }
    e->fixIn = from; e->fixOut = to;
    app.project.changed();
}

void EditWindowComponent::openBounce()
{
    auto* e = edit();
    if (e == nullptr || ! app.showBounce) return;
    if (e->isEmpty()) { showError ("Bounce Out", "The edit is empty."); return; }
    BounceContext c;
    c.kind = BounceContext::Kind::Edit; c.id = editId; c.defaultName = e->name;
    for (auto& r : e->regions)  if (timeline->isSelected (r.id)) c.selected.push_back (r.id);
    for (auto& r : e->overdubs) if (timeline->isSelected (r.id)) c.selected.push_back (r.id);
    app.showBounce (c);
}

bool EditWindowComponent::keyPressed (const juce::KeyPress& k)
{
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (k == juce::KeyPress::homeKey) { goTo (0.0); return true; }
    if (k == juce::KeyPress::endKey)  { goToEnd();  return true; }
    if (c == 'p') { markWholeSelected(); return true; }
    if ((c == '1' || c == '2') && ! k.getModifiers().isAnyModifierKeyDown()) { setFixMark (c == '1'); return true; }
    if (c == '5' && ! k.getModifiers().isAnyModifierKeyDown())            // 5: take the 1 / 2 flags away; a piece selected whole is then what the process windows load
    {
        if (auto* e = edit()) { e->fixIn = -1.0; e->fixOut = -1.0; e->fixTracks.clear(); app.project.changed(); }
        return true;
    }
    if (c == '.' || c == '>') { app.changeWaveZoom (1.4f);        return true; }          // . bigger waveforms
    if (c == ',' || c == '<') { app.changeWaveZoom (1.0f / 1.4f); return true; }          // , smaller
    if (k == juce::KeyPress::spaceKey) { if (! spaceIsForRecording()) togglePlay(); return true; }
    if (k == juce::KeyPress::leftKey)  { timeline->zoom (1.0 / 1.4);          return true; }     // arrows zoom: left / right = horizontal,
    if (k == juce::KeyPress::rightKey) { timeline->zoom (1.4);                return true; }     // up / down = vertical (same keys as the take window)
    if (k == juce::KeyPress::upKey)    { timeline->zoomVertical (1.0 / 1.25); return true; }
    if (k == juce::KeyPress::downKey)  { timeline->zoomVertical (1.25);       return true; }
    if (c == 'c' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->centreOnPlayhead(); return true; }
    if (c == 'l' && ! k.getModifiers().isAnyModifierKeyDown()) { loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification); loopChanged(); return true; }
    if (c == 't') { trimSelected(); return true; }
    if (k == juce::KeyPress::returnKey || c == 'h')              // Enter: Repair & DeClick; H: ReHarmoniSer: both for the part between marks 1 and 2
    {
        auto* e = edit();
        if (e == nullptr || e->fixIn < 0.0 || e->fixOut <= e->fixIn) return false;
        if (k == juce::KeyPress::returnKey) repairButton.onClick(); else reharmoniserButton.onClick();
        return true;
    }
    if (c == 'r' && ! k.getModifiers().isAnyModifierKeyDown()) { buttSelected(); return true; }
    if (c == '4' && ! k.getModifiers().isAnyModifierKeyDown()) { placeFromTakeWindow(); return true; }     // same as key 4 in a take window
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    return false;
}

void EditWindowComponent::changeListenerCallback (juce::ChangeBroadcaster*) { repaint(); }

void EditWindowComponent::timerCallback()
{
    if (auto* fc = juce::Component::getCurrentlyFocusedComponent(); fc != nullptr && isParentOf (fc)) app.setTransportHandler (this, [this] { togglePlay(); });
    if (insertFlagToggle.getToggleState() != app.showInsertFlag) insertFlagToggle.setToggleState (app.showInsertFlag, juce::dontSendNotification);
    if (slipLeftToggle.getToggleState() != app.trimSlipLeft)   { slipLeftToggle.setToggleState (app.trimSlipLeft, juce::dontSendNotification);   timeline->slipLeft = app.trimSlipLeft; }
    if (slipRightToggle.getToggleState() != app.trimSlipRight) { slipRightToggle.setToggleState (app.trimSlipRight, juce::dontSendNotification); timeline->slipRight = app.trimSlipRight; }
    playButton.setButtonText (app.isPlaying() ? "Stop [Space]" : "Play [Space]");
    if (playheadMode.getToggleState() != app.playheadFollows) playheadMode.setToggleState (app.playheadFollows, juce::dontSendNotification);
    if (waveColourBtn.getToggleState() != app.waveColour) { waveColourBtn.setToggleState (app.waveColour, juce::dontSendNotification); repaint(); }
    if (app.windowIsOpen)         // Trim and Bounce Out are lit only while their window is open (closing the Bounce Out window also ends a bounce that is running)
    {
        const bool trimOpen = app.windowIsOpen ("trim:" + editId.toString()), bounceOpen = app.windowIsOpen ("bounce:" + editId.toString());
        if (trimButton.getToggleState() != trimOpen)     trimButton.setToggleState (trimOpen, juce::dontSendNotification);
        if (bounceButton.getToggleState() != bounceOpen) bounceButton.setToggleState (bounceOpen, juce::dontSendNotification);
    }
    clock.setSampleRate (app.engine.getSampleRate());
    if (auto* e = edit())
        infoLabel.setText (juce::String (takesInEdit (*e)) + (takesInEdit (*e) == 1 ? " Take" : " Takes") + (e->overdubs.empty() ? juce::String() : ", " + juce::String ((int) e->overdubs.size()) + " overdub(s)") + ", " + formatTimeShort (e->lengthSeconds(), 0)
                           + (e->fixIn >= 0 ? "   [1] " + formatTimeShort (e->fixIn, 2) : juce::String())
                           + (e->fixOut >= 0 ? "   [2] " + formatTimeShort (e->fixOut, 2) : juce::String())
                           + (e->regions.size() > 1 ? "   -   click a join and press T (or double-click it) to trim.  Drag a piece to slide it.  Right-click a piece for its volume." : ""), juce::dontSendNotification);
}
} // namespace td

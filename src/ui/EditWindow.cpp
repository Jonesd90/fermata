#include "WaveDraw.h"
#include "EditWindow.h"
#include <set>
#include "FixTools.h"
#include "ColourPicker.h"
#include "VolumeDialog.h"

namespace td
{
/** How many different takes the edit is made from (a take used in several pieces counts once). */
static int takesInEdit (const EditDef& e) { std::set<juce::String> t; for (auto& r : e.regions) t.insert (r.takeId.toString()); return (int) t.size(); }
static constexpr int kRulerH = 22, kLaneH = 38, kNameW = 150;

class EditTimeline : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    EditTimeline (AppContext& a, const juce::Uuid& eid, juce::Viewport& vp)
        : app (a), editId (eid), viewport (vp), thumbCache (64)
    {
        formatManager.registerBasicFormats();
        setWantsKeyboardFocus (true);
        startTimerHz (15);
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
        if (k <= 0) return e.regions.front().startSample;
        if (k >= n) return e.regions.back().endSample();
        return (e.regions[(size_t) k - 1].fadeOutFinish() + e.regions[(size_t) k].fadeInBegin()) / 2;
    }
    juce::Uuid moveRegion;                     // the piece that Ctrl + click selected whole: it can be dragged sideways (until another click elsewhere)
    std::vector<juce::Uuid> selectedRegions;   // the pieces selected for Bounce Out (Ctrl + click adds more)
    bool isSelected (const juce::Uuid& id) const { return std::find (selectedRegions.begin(), selectedRegions.end(), id) != selectedRegions.end() || id == selectedRegion; }
    int selectedJoin = -1;                 // the selected edit point: 0 = the start of the edit, k = the join before regions[k], n = the end of the edit; -1 = none
    bool slipLeft = false, slipRight = false;
    double cursorSeconds = -1.0;

    EditDef* edit() const { return app.project.findEdit (editId); }

    /** The track rows of this edit, top to bottom (the edit's own list and order). Refreshed on every call, so it is never stale. */
    const std::vector<TrackDef>& rowTracks() const
    {
        rowsCache.clear();
        if (auto* e = edit()) for (auto* t : app.project.editTracks (*e)) rowsCache.push_back (*t);
        return rowsCache;
    }
    mutable std::vector<TrackDef> rowsCache;
    juce::Uuid clipMoveRegion, clipMoveTrack; int clipMoveToRow = -1;   // Shift + drag: one track's clip of a piece moves to another track, staying at the same time
    int rowDragFrom = -1, rowDragTo = -1;       // dragging a track's name up or down to move the track (the edit's order only)

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

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wave);
        auto* e = edit();
        if (e == nullptr) return;
        const auto tracks = rowTracks();
        const int top = kRulerH + kLaneH;
        const int rowsEnd = top + juce::jmax (1, (int) tracks.size()) * rowH;

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            g.setColour ((i & 1) ? theme::waveAlt : theme::wave);
            g.fillRect (0, top + (int) i * rowH, getWidth(), rowH);
        }
        for (int i = (int) tracks.size(), y = top + (int) tracks.size() * rowH; y < getHeight(); ++i, y += rowH)       // empty rows carry on to the bottom of the window
        {
            g.setColour (((i & 1) ? theme::waveAlt : theme::wave).withMultipliedBrightness (0.97f));
            g.fillRect (0, y, getWidth(), rowH);
        }
        g.setColour (theme::ruler); g.fillRect (0, 0, getWidth(), kRulerH);
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
            g.setColour (theme::dimText); g.drawText (formatTime (s).substring (3, 8), x + 3, 2, 60, 16, juce::Justification::left);
        }

        if (e->isEmpty())
        {
            g.setColour (theme::dimText); g.setFont (16.0f);
            g.drawText ("Empty. In a take window: press 1 and 2 to mark the part you want, then 3 (or the 'To edit' button).",
                        kNameW + 20, top + 10, 900, 24, juce::Justification::left);
        }

        const double rate = juce::jmax (1.0, e->sampleRate);
        for (size_t k = 0; k < e->regions.size(); ++k)
        {
            const auto& r = e->regions[k];
            const int x0 = xOf ((double) r.startSample / rate);
            const int wpx = juce::jmax (3, (int) ((double) r.length() / rate * pixelsPerSecond));
            const bool selected = isSelected (r.id);

            auto lane = juce::Rectangle<int> (x0, kRulerH + 11, wpx, kLaneH - 13);   // below the flags, so the title has a line to itself
            g.setColour (selected ? juce::Colour (0xffc26500) : theme::accent);
            g.fillRect (lane);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            g.drawText (r.takeName + "   " + formatTime ((double) r.srcIn / rate).substring (3, 11) + " - " + formatTime ((double) r.srcOut / rate).substring (3, 11),
                        lane.reduced (6, 0), juce::Justification::centredLeft, true);

            drawFadeHandles (g, lane, r, rate, k == 0, k + 1 == e->regions.size());

            for (size_t ti = 0; ti < tracks.size(); ++ti)
            {
                const RegionFile* rf = nullptr;
                for (auto& f : r.files) if (f.trackId == tracks[ti].id) rf = &f;
                if (rf == nullptr) continue;
                auto rr = juce::Rectangle<int> (x0, top + (int) ti * rowH + 3, wpx, rowH - 6);
                const auto tc = chan::of (app.project, tracks[ti].id);
                g.setColour (selected ? chan::clipFill (tc, true) : chan::clipFill (tc));
                g.fillRect (rr);
                if (r.waiting.active() && (r.waiting.tracks.empty() || std::find (r.waiting.tracks.begin(), r.waiting.tracks.end(), rf->trackId) != r.waiting.tracks.end()))
                    drawWaitingBlock (g, rr.reduced (1), r.waiting.name, theme::text);
                else
                {
                    setWaveColour (g, chan::waveColour (tc));
                    juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (rr.reduced (1));
                    const auto wcd = app.waveColourOf (rf->file);
                    if (! directWave.draw (g, rr.reduced (1), rf->file, (double) (r.srcIn - rf->fileStart) / rate, (double) (r.srcOut - rf->fileStart) / rate, app.waveZoom, wcd.get()))
                        if (auto* th = thumbnailFor (rf->file)) drawThumbnailEnvelope (g, rr.reduced (1), *th, (double) (r.srcIn - rf->fileStart) / rate, (double) (r.srcOut - rf->fileStart) / rate, app.waveZoom, wcd.get());
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
                    if (! directWave.draw (g, rr.reduced (1), rf->file, (double) (r.srcIn - rf->fileStart) / rate, (double) (r.srcOut - rf->fileStart) / rate, app.waveZoom, wcd.get()))
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

        // where the next take will be placed
        {
            const juce::int64 at = e->insertIndex < 0 || e->insertIndex >= (int) e->regions.size()
                                       ? (e->regions.empty() ? 0 : e->regions.back().endSample()) : e->regions[(size_t) e->insertIndex].startSample;
            const int x = xOf ((double) at / rate);
            g.setColour (juce::Colour (0xffc26500));
            for (int y = kRulerH; y < rowsEnd; y += 8) g.fillRect (x - 1, y, 2, 4);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("next take goes here", x + 4, rowsEnd - 16, 130, 14, juce::Justification::left);
        }

        const double ph = app.playheadSeconds();
        if (ph >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)
        {
            g.setColour (theme::playhead); g.fillRect (xOf (ph), kRulerH, 2, rowsEnd - kRulerH);
        }
        if (cursorSeconds >= 0)
        {
            g.setColour (theme::text); g.fillRect (xOf (cursorSeconds), kRulerH, 1, rowsEnd - kRulerH);
        }
        if (e->playheadSeconds > 0)                                  // where key 4 / an overdub in the take window will be placed
        {
            const int x = xOf (e->playheadSeconds);
            g.setColour (juce::Colour (0xff6a4cc2)); g.fillRect (x, 0, 1, kRulerH);
            juce::Path tri; tri.addTriangle ((float) x - 5, 0.0f, (float) x + 6, 0.0f, (float) x, 9.0f); g.fillPath (tri);
        }
        if (e->markIn >= 0 || e->markOut >= 0)                      // I / O points (used by Bounce Out)
        {
            if (e->markIn >= 0 && e->markOut > e->markIn)
            {
                g.setColour (juce::Colour (0x221f7a46)); g.fillRect (xOf (e->markIn), kRulerH, xOf (e->markOut) - xOf (e->markIn), rowsEnd - kRulerH);
            }
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            if (e->markIn >= 0)  { g.setColour (juce::Colour (0xff1f7a46)); g.fillRect (xOf (e->markIn), 0, 2, rowsEnd);  g.drawText ("IN",  xOf (e->markIn) + 4, 2, 30, 14, juce::Justification::left); }
            if (e->markOut >= 0) { g.setColour (juce::Colour (0xffb3261e)); g.fillRect (xOf (e->markOut) - 1, 0, 2, rowsEnd); g.drawText ("OUT", xOf (e->markOut) - 34, 2, 30, 14, juce::Justification::right); }
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

        if (e->automationOn)
            for (size_t i = 0; i < tracks.size(); ++i) paintLane (g, *e, (int) i, top + (int) i * rowH);
        if (e->automationOn && autoDragIndex >= 0)                                    // the value of the point being set or moved, in a little box beside it
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
            auto r = juce::Rectangle<int> (sx, top + (int) i * rowH, kNameW, rowH);
            const auto tc = chan::of (app.project, tracks[i].id);
            g.setColour (theme::panel); g.fillRect (r);
            g.setColour (tc.withAlpha (0.20f)); g.fillRect (r);
            g.setColour (tc); g.fillRect (r.removeFromLeft (6));
            g.setColour (tc.darker (0.3f).withAlpha (0.8f)); g.fillRect (sx, top + (int) i * rowH + rowH - 1, kNameW, 1);
            g.setColour (theme::text); g.setFont (14.0f);
            if (e->automationOn)
            {
                auto nameArea = r.reduced (8, 0);
                auto bottom = nameArea.removeFromBottom (juce::jmin (20, nameArea.getHeight() / 2 + 2));
                g.drawText (tracks[i].name, nameArea, juce::Justification::centredLeft, true);
                const auto* lane = e->findLane (tracks[i].id, selOf (tracks[i].id).param, selOf (tracks[i].id).mixer);
                const bool locked = lane != nullptr && lane->locked;
                auto lockR = lockRect (sx, top + (int) i * rowH);
                auto labelR = paramRect (sx, top + (int) i * rowH);
                g.setColour (theme::border); g.drawRoundedRectangle (labelR.toFloat(), 3.0f, 1.0f);
                g.setColour (theme::text); g.setFont (juce::FontOptions (12.0f));
                g.drawText (paramLabel (*e, tracks[i].id) + "  v", labelR.reduced (4, 0), juce::Justification::centredLeft, true);
                drawPadlock (g, lockR.toFloat(), locked, locked ? juce::Colour (0xffe0b800) : theme::dimText);
                (void) bottom;
            }
            else g.drawText (tracks[i].name, r.reduced (8, 0), juce::Justification::centredLeft, true);
        }
        if (! clipMoveRegion.isNull() && clipMoveToRow >= 0)                           // Shift + drag: the row the clip will go to
        {
            g.setColour (juce::Colour (0xffffc83d)); g.drawRect (juce::Rectangle<int> (sx + kNameW, top + clipMoveToRow * rowH, getWidth() - sx - kNameW, rowH), 3);
        }
        if (rowDragFrom >= 0 && rowDragTo >= 0)                                        // where the dragged track will land
        {
            const int y = top + (rowDragTo > rowDragFrom ? rowDragTo + 1 : rowDragTo) * rowH;
            g.setColour (juce::Colour (0xffffc83d)); g.fillRect (sx, y - 2, getWidth() - sx, 4);
        }
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, rowsEnd, kNameW, juce::jmax (0, getHeight() - rowsEnd));
        g.setColour (theme::panel.withAlpha (0.96f)); g.fillRect (sx, 0, kNameW, top);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        dragging = false; markDrag = false; markActive = false;
        auto* ed = this->edit();
        if (ed == nullptr) return;
        const double rate = juce::jmax (1.0, ed->sampleRate);
        if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown() && ! e.mods.isPopupMenu() && ! (e.x > kNameW && e.y >= kRulerH + kLaneH))   // a click anywhere but on the audio lets go of the selected pieces
        { selectedRegion = juce::Uuid::null(); selectedRegions.clear(); moveRegion = juce::Uuid::null(); repaint(); }
        if (ed->automationOn && automationMouseDown (e, *ed)) return;
        if (e.mods.isPopupMenu() && e.x >= viewport.getViewPositionX() && e.x < viewport.getViewPositionX() + kNameW && e.y >= kRulerH + kLaneH)   // right-click on a track name: its colour
        {
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) rowTracks().size()))
            {
                const auto id = rowTracks()[(size_t) row].id;
                const auto pos = juce::Desktop::getMousePosition();
                showColourPickerAt (app, id, juce::Rectangle<int> (pos, pos).expanded (4));
            }
            return;
        }
        if (! e.mods.isPopupMenu() && e.x >= viewport.getViewPositionX() && e.x < viewport.getViewPositionX() + kNameW && e.y >= kRulerH + kLaneH)   // a track's name: drag it to another row
        {
            const int row = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);
            if (juce::isPositiveAndBelow (row, (int) rowTracks().size())) { rowDragFrom = row; rowDragTo = row; return; }
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
        const bool additive = e.mods.isCommandDown() || e.mods.isCtrlDown();
        const auto previous = selectedRegion;
        selectedRegion = juce::Uuid::null();
        const juce::int64 sample = (juce::int64) (juce::jmax (0.0, t) * rate);
        for (auto& r : ed->regions) if (sample >= r.startSample && sample < r.endSample()) selectedRegion = r.id;
        if (auto* od = overdubAt (*ed, e.getPosition(), rate)) selectedRegion = od->id;          // an overdub sits on top
        // A plain drag over audio always marks a part of it: mark 1 where the mouse went down, mark 2 where it is let go.
        // To MOVE a piece, Ctrl + click it (that selects the whole piece); once it is selected the Ctrl key can be let go and the piece dragged sideways.
        const bool hitArmed = ! selectedRegion.isNull() && selectedRegion == moveRegion;
        const bool canMove = additive || hitArmed;
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
            markAlt = e.mods.isAltDown();                                  // Alt + drag: only the track(s) the mouse covers are marked
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

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* ed = edit();
        if (ed == nullptr) return;
        if (autoDragIndex >= 0) { automationDrag (e, *ed); return; }
        if (! clipMoveRegion.isNull())
        {
            clipMoveToRow = juce::jlimit (0, juce::jmax (0, (int) rowTracks().size() - 1), (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH));
            repaint(); return;
        }
        if (rowDragFrom >= 0)
        {
            rowDragTo = juce::jlimit (0, juce::jmax (0, (int) rowTracks().size() - 1), (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH));
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
                if (n > 0) for (int r = lo; r <= hi; ++r) ed->fixTracks.push_back (rt[(size_t) r].id);
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

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto* ed = edit(); EditRegion* fr = nullptr; bool isIn = true;
        const bool over = ed != nullptr && e.x > kNameW && fadeHandleAt (*ed, e.getPosition(), juce::jmax (1.0, ed->sampleRate), fr, isIn);
        setMouseCursor (over ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        (void) isIn;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! clipMoveRegion.isNull())
        {
            finishClipMove();
            clipMoveRegion = juce::Uuid::null(); clipMoveToRow = -1;
            setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); return;
        }
        if (rowDragFrom >= 0)
        {
            const int from = rowDragFrom, to = rowDragTo; rowDragFrom = rowDragTo = -1;
            if (auto* ed = edit(); ed != nullptr && from != to && from >= 0 && to >= 0)
            {
                app.project.materialiseEditTracks (*ed);
                // the list may hold ids that no longer exist: move by what is shown
                std::vector<juce::Uuid> shown; for (auto* t : app.project.editTracks (*ed)) shown.push_back (t->id);
                if (from < (int) shown.size() && to < (int) shown.size())
                {
                    const auto id = shown[(size_t) from]; shown.erase (shown.begin() + from); shown.insert (shown.begin() + to, id);
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
        if (dragging) { dragging = false; baseRegions.clear(); app.project.changed(); }
        setMouseCursor (juce::MouseCursor::NormalCursor);
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
        if (target.id == clipMoveTrack) return;
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
    struct LaneSel { juce::String param { autoparam::fader }; juce::Uuid mixer = juce::Uuid::null(); };
    std::map<juce::String, LaneSel> laneSel;                       // per track: which lane is shown
    juce::Uuid autoDragLane = juce::Uuid::null(); int autoDragIndex = -1; int autoDragRow = 0; float autoPrevY = 0.0f;

    LaneSel& selOf (const juce::Uuid& trackId) { return laneSel[trackId.toString()]; }
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
                if (auto* st = m->stripFor (trackId); st != nullptr && slot < kNumSlots)
                    if (auto* pr = st->slots[slot].getProcessorUnsafe())
                        return pr->getName() + ": " + pr->paramName (idx);
            return "Plug-in " + juce::String (slot + 1) + " (empty)";
        }
        return "Fader";
    }

    juce::String paramLabel (const EditDef& ed, const juce::Uuid& trackId)
    {
        auto& s = selOf (trackId);
        juce::String n = describeParam (s.param, trackId, s.mixer);
        if (! s.mixer.isNull()) for (auto& m : app.project.mixers) if (m->id == s.mixer) n += " (" + m->name + ")";
        (void) ed; return n;
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
    static juce::String autoValueText (const AutoLane& lane, float v)
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

    void paintLane (juce::Graphics& g, EditDef& ed, int row, int rowTop)
    {
        const auto& t = rowTracks()[(size_t) row];
        const double rate = juce::jmax (1.0, ed.sampleRate);
        auto& s = selOf (t.id);
        const auto* lane = ed.findLane (t.id, s.param, s.mixer);
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
        juce::Path p;
        p.startNewSubPath (x0, yOfValue (L, L.pts.front().value, rowTop));
        for (auto& pt : L.pts) p.lineTo (px (pt.time), yOfValue (L, pt.value, rowTop));
        p.lineTo (x1, yOfValue (L, L.pts.back().value, rowTop));
        g.setColour (line); g.strokePath (p, juce::PathStrokeType (1.6f));
        for (size_t k = 0; k < L.pts.size(); ++k)
        {
            const auto& pt = L.pts[k];
            const juce::Point<float> c (px (pt.time), yOfValue (L, pt.value, rowTop));
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
        const auto& tracks = rowTracks();
        if (! juce::isPositiveAndBelow (row, (int) tracks.size())) return false;
        const auto& t = tracks[(size_t) row];
        const int rowTop = top + row * rowH;
        const int sx = viewport.getViewPositionX();
        auto& s = selOf (t.id);
        if (e.x < sx + kNameW)
        {
            if (lockRect (sx, rowTop).expanded (3).contains (e.getPosition()))
            {
                auto& l = ed.addLane (t.id, s.param, s.mixer);
                l.locked = ! l.locked; app.project.changed(); repaint(); return true;
            }
            if (paramRect (sx, rowTop).contains (e.getPosition())) { showLaneMenu (ed, t); return true; }
            return false;
        }
        auto* lane = ed.findLane (t.id, s.param, s.mixer);
        const double rate = juce::jmax (1.0, ed.sampleRate);
        if (lane != nullptr)
        {
            for (int k = 0; k < (int) lane->pts.size(); ++k)
            {
                const auto& pt = lane->pts[(size_t) k];
                const float dx = (float) xOf ((double) pt.time / rate) - (float) e.x, dy = yOfValue (*lane, pt.value, rowTop) - (float) e.y;
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
        auto& l = ed.addLane (t.id, s.param, s.mixer);
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

    void showLaneMenu (EditDef& ed, const TrackDef& t)
    {
        auto& s = selOf (t.id);
        juce::PopupMenu m, mixers;
        m.addSectionHeader ("Automate");
        m.addItem (3, "Gain (into the mixer, before the inserts and fader)", true, s.param == autoparam::gain);
        m.addItem (1, "Fader (through the mixer, after the inserts)", true, s.param == autoparam::fader);
        m.addItem (2, "Pan" + juce::String (t.channelCount() == 1 ? "" : " (mono tracks only)"), t.channelCount() == 1, s.param == autoparam::pan);
        // bus / track sends the strip really has (in the mixer chosen below), and the automatable parameters of its plug-ins
        std::vector<juce::String> keys;
        juce::PopupMenu sends, plugins;
        if (auto* mx = mixerFor (s.mixer))
            if (auto* st = mx->stripFor (t.id))
            {
                for (auto& sd : st->sends.pool)
                {
                    keys.push_back (autoparam::send (sd->dest));
                    sends.addItem (1000 + (int) keys.size() - 1, nodeName (sd->dest), true, s.param == keys.back());
                }
                for (int sl = 0; sl < kNumSlots; ++sl)
                {
                    auto* pr = st->slots[sl].getProcessorUnsafe();
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
                        page.addItem (1000 + (int) keys.size() - 1, pr->paramName (i), true, s.param == keys.back());
                        if (++inPage == 25) flush();
                    }
                    flush();
                    if (total > 0) plugins.addSubMenu ("Insert " + juce::String (sl + 1) + ": " + pr->getName(), one);
                }
            }
        m.addSubMenu ("Bus sends", sends, sends.getNumItems() > 0);
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
        auto* lane = ed.findLane (t.id, s.param, s.mixer);
        m.addItem (5, "Clear this lane's points", lane != nullptr && ! lane->pts.empty() && ! lane->locked);
        const auto tid = t.id; const auto editId = ed.id;
        m.showMenuAsync (juce::PopupMenu::Options(), [this, tid, editId, keys] (int r)
        {
            auto* e2 = app.project.findEdit (editId);
            if (r == 0 || e2 == nullptr) return;
            auto& sel = selOf (tid);
            if (r == 1) sel.param = autoparam::fader;
            else if (r == 2) sel.param = autoparam::pan;
            else if (r == 3) sel.param = autoparam::gain;
            else if (r >= 1000) { const auto i = (size_t) (r - 1000); if (i < keys.size()) sel.param = keys[i]; }
            else if (r == 99) sel.mixer = juce::Uuid::null();
            else if (r >= 100) { const auto i = (size_t) (r - 100); if (i < app.project.mixers.size()) sel.mixer = app.project.mixers[i]->id; }
            else if (r == 5) { if (auto* l = e2->findLane (tid, sel.param, sel.mixer)) { l->pts.clear(); app.project.changed(); } }
            repaint();
        });
    }

    int xOf (double seconds) const { return kNameW + (int) (seconds * pixelsPerSecond); }

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
        // the area between marks 1 and 2 (or the I and O flags), for all tracks or the ones chosen with Alt + drag
        juce::int64 rangeFrom = 0, rangeTo = 0;
        if (ed.fixIn >= 0 && ed.fixOut > ed.fixIn) { rangeFrom = (juce::int64) std::llround (ed.fixIn * rate); rangeTo = (juce::int64) std::llround (ed.fixOut * rate); }
        else if (ed.markIn >= 0 && ed.markOut > ed.markIn) { rangeFrom = (juce::int64) std::llround (ed.markIn * rate); rangeTo = (juce::int64) std::llround (ed.markOut * rate); }
        const bool haveRange = rangeTo > rangeFrom && ! isOver;
        m.addItem (7, "Volume change between the marks...", haveRange);
        m.addSeparator();
        m.addItem (1, "Volume change from here on...");
        m.addItem (2, "Volume of the whole piece...");
        m.addItem (3, "Remove all volume changes in this piece", hasGains);
        if (isOver) { m.addSeparator(); m.addItem (4, "Delete this overdub"); }
        if (piece->waiting.active())
        {
            m.addSeparator();
            m.addItem (5, "Re-link corrected audio  (" + piece->waiting.name + ")");
            m.addItem (6, "Stop waiting: keep the original audio");
        }
        auto* appPtr = &app;
        juce::Component::SafePointer<juce::Component> self (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [appPtr, eid, rid, rel, self, rangeFrom, rangeTo] (int r)
        {
            auto* ed2 = appPtr->project.findEdit (eid);
            auto* p2 = ed2 ? ed2->findAny (rid) : nullptr;
            if (p2 == nullptr) return;
            if (r == 1) showVolumeChange (*appPtr, eid, rid, rel);
            else if (r == 2) showVolumeChange (*appPtr, eid, rid, 0);
            else if (r == 3) { p2->gains.clear(); appPtr->project.changed(); }
            else if (r == 7) showRangeVolumeChange (*appPtr, eid, rangeFrom, rangeTo);
            else if (r == 5) fixtools::relinkEdit (*appPtr, eid, rid, self.getComponent());
            else if (r == 6) { p2->waiting = WaitingPiece(); appPtr->project.changed(); }
            else if (r == 4) { if (appPtr->isPlaying()) appPtr->stopPlayback(); ed2->removeOverdub (rid); appPtr->project.changed(); }
        });
    }

    juce::AudioThumbnail* thumbnailFor (const juce::File& f)
    {
        const auto key = f.getFullPathName();
        auto it = thumbs.find (key);
        if (it == thumbs.end())
        {
            auto t = std::make_unique<juce::AudioThumbnail> (128, formatManager, thumbCache);
            t->addChangeListener (this);
            t->setSource (new juce::FileInputSource (f));
            it = thumbs.emplace (key, std::move (t)).first;
        }
        return it->second.get();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateSize(); repaint(); }
    void timerCallback() override
    {
        if (auto* ed = edit())                      // the playhead moved by itself (Stop leaves it where playback got to): the cursor follows
            if (std::abs (ed->playheadSeconds - lastHead) > 1.0e-9) { lastHead = ed->playheadSeconds; cursorSeconds = ed->playheadSeconds; repaint(); }
        if (app.isPlaying()) { updateSize(); followPlayhead(); repaint(); }
        else followActive = false;
    }
    double lastHead = 0.0;
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
    juce::AudioThumbnailCache thumbCache;
    DirectWaveCache directWave { formatManager };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
    bool dragging = false; int dragIndex = -1; std::vector<EditRegion> baseRegions;
    bool markDrag = false, markActive = false, markAlt = false; double markAnchor = 0.0; int markStartX = 0, markRow0 = 0;     // dragging over audio that is not selected sets marks 1 and 2
    bool dragOverdub = false; juce::int64 overdubBase = 0;
    int fadeDrag = 0; juce::Uuid fadeId; double fadeBase = 0.0;      // dragging a piece's fade-in (1) or fade-out (2) from its corner
};

struct EditViewport : public juce::Viewport
{
    void visibleAreaChanged (const juce::Rectangle<int>&) override { if (auto* c = getViewedComponent()) c->repaint(); }
};

EditWindowComponent::EditWindowComponent (AppContext& a, const juce::Uuid& id) : app (a), editId (id)
{
    auto* vp = new EditViewport();
    timeline.reset (new EditTimeline (app, editId, *vp));
    vp->setViewedComponent (timeline.get(), false);
    vp->setScrollBarsShown (true, true);
    addAndMakeVisible (vp);
    viewportOwner.reset (vp);
    setWantsKeyboardFocus (true);
    keysKeeper = std::make_unique<KeepKeysOnTimeline> (*vp, *timeline);

    nameCaption.setText ("Edit:", juce::dontSendNotification);
    addAndMakeVisible (nameCaption); addAndMakeVisible (nameEditor); addAndMakeVisible (infoLabel);
    nameEditor.setText (edit() ? edit()->name : "", false);
    auto commitEditName = [this]
    {
        if (auto* e = edit()) if (nameEditor.getText().isNotEmpty() && e->name != nameEditor.getText()) { e->name = nameEditor.getText(); if (app.project.syncEditMixers()) app.project.structureChanged(); else app.project.changed(); }
    };
    nameEditor.onFocusLost = commitEditName;
    nameEditor.onReturnKey = [commitEditName] { commitEditName(); juce::Component::unfocusAllComponents(); };
    infoLabel.setColour (juce::Label::textColourId, theme::text);

    tracksButton.setTooltip ("The tracks (audio rows) of THIS edit: add one, remove one, or show every project track again. Drag a track's name up or down to change the order. Each track has its own mixer strip.");
    tracksButton.onClick = [this] { showTracksMenu(); };
    importButton.setTooltip ("Bring a multitrack recording (a multichannel / polyphonic WAV, or one mono / stereo file per track) into THIS edit, e.g. an already edited programme to mix. "
                             "You can also drag the files onto this window. Each track goes on its own row.");
    importButton.onClick = [this] { fixtools::showImportMenu (app, juce::Uuid::null(), &importButton, editId); };
    addAndMakeVisible (importButton);
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
    automationButton.setTooltip ("Automation: draw fader and pan moves on each track (click to add a point, drag to move it, right-click / Shift-click to delete). "
                                 "The points stay on the same music when the edit changes. Plays from this window and is part of Bounce Out. Restart playback to hear changes.");
    automationButton.setToggleState (edit() != nullptr && edit()->automationOn, juce::dontSendNotification);
    automationButton.onClick = [this] { if (auto* e = edit()) { e->automationOn = automationButton.getToggleState(); app.project.changed(); timeline->repaint(); } };
    pitchButton.setTooltip ("Pitch correction: shifts the audio between marks 1 and 2 on every track, by semitones and / or cents. Can also process extra audio at each end (up to 9 s) so the edit points can be moved outward later");
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
    pitchButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::pitchEdit (app, editId, this); }); };
    pitchCurveButton.setTooltip ("Pitch curve: draw a line of pitch against time between marks 1 and 2 (+100 cents at the top, -100 at the bottom), for a note that drifts flat or sharp; audition it, then accept or revert");
    pitchCurveButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::pitchCurveEdit (app, editId, this); }); };
    repairButton.setTooltip ("Spectral Repair: shows the audio between marks 1 and 2 of all tracks as one spectrogram; draw a box round a noise (drag its edges to adjust it) and rebuild it from the clean sound next to it");
    repairButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::repairEdit (app, editId, this, false); }); };
    declickButton.setTooltip ("De-Click: the same window as Spectral Repair, but it finds and mends clicks (the spectrogram helps you see them)");
    declickButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::repairEdit (app, editId, this, true); }); };
    reharmoniserButton.setTooltip ("ReHarmoniSer: retunes single out-of-tune notes in the audio between marks 1 and 2 of all tracks, every track in exactly the same way; check it by ear, then Write back to clip");
    reharmoniserButton.onClick = [this, withWholePiece] { withWholePiece ([this] { fixtools::reharmoniserEdit (app, editId, this); }); };
    exportProcButton.setTooltip ("Export for Processing: sends the part between marks 1 and 2 (all tracks) to the Processing Media folder to be corrected in other software (e.g. iZotope RX). Re-link it afterwards with a right-click on the 'Waiting for corrected audio' block");
    exportProcButton.onClick = [this] { fixtools::exportForProcessingEdit (app, editId, this); };
    undoFixButton.setTooltip ("Undo the last pitch correction, repair, de-click or export for processing (the original audio files were never touched)");
    undoFixButton.onClick = [this] { fixtools::undoLastFix (app); };
    bounceButton.setTooltip ("Make a master file of this edit through its own mixer (or another mixer)");
    bounceButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    mixerButton.setTooltip ("Opens the mixer of this edit. The edit always plays through it (and is bounced through it), so each piece can have its own mix. It started as a copy of the processing mixer");
    mixerButton.onClick = [this] { if (auto* m = app.project.mixerOfEdit (editId)) { if (app.showMixer) app.showMixer (m->id); } };
    zoomInButton.onClick = [this] { timeline->zoom (1.5); };
    zoomOutButton.onClick = [this] { timeline->zoom (1.0 / 1.5); };
    slipLeftToggle.setTooltip ("When you slide a piece, every piece BEFORE it slides with it (off: they stay where they are).");
    slipRightToggle.setTooltip ("When you slide a piece, every piece AFTER it slides with it (off: they stay where they are).");
    slipLeftToggle.onClick  = [this] { timeline->slipLeft = slipLeftToggle.getToggleState(); };
    slipRightToggle.onClick = [this] { timeline->slipRight = slipRightToggle.getToggleState(); };
    for (auto* b : std::initializer_list<juce::Button*> { &playButton, &trimButton, &deleteButton, &leftButton, &rightButton, &endButton, &zoomInButton, &zoomOutButton, &bounceButton, &mixerButton, &automationButton, &pitchButton, &pitchCurveButton, &repairButton, &declickButton, &reharmoniserButton, &exportProcButton, &undoFixButton,
                                                          &slipLeftToggle, &slipRightToggle })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false);
    }
    trimButton.setColour (juce::TextButton::buttonColourId, theme::accent);
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
    const std::vector<FlowItem> items
    {
        { &nameCaption, 45 }, { &nameEditor, 240, grid::btnH, 14 },
        { &startButton, 40, grid::btnH, 2 }, { &backButton, 40, grid::btnH, 2 }, { &playButton }, { &fwdButton, 40, grid::btnH, 2 }, { &endTransportButton, 40, grid::btnH, 6 }, { &playheadMode, 34, grid::btnH, 2 }, { &waveColourBtn, 34, grid::btnH, 2 }, { &loopToggle, 34, grid::btnH, 14 },
        { &trimButton }, { &deleteButton, grid::btnW, grid::btnH, 14 },
        { &leftButton }, { &rightButton }, { &endButton, grid::btnW, grid::btnH, 14 },
        { &bounceButton }, { &mixerButton, grid::btnW, grid::btnH, 14 },
        { &automationButton, grid::btnW, grid::btnH, 14 },
        { &pitchButton }, { &pitchCurveButton, grid::btnW + 24 }, { &repairButton, grid::btnW + 30 }, { &declickButton }, { &reharmoniserButton, grid::btnW + 24 }, { &exportProcButton, grid::btnW + 50 }, { &undoFixButton, grid::btnW, grid::btnH, 14 },
        { &tracksButton }, { &importButton, grid::btnW, grid::btnH, 14 },
        { &slipLeftToggle, 95 }, { &slipRightToggle, 105, grid::btnH, 14 },
        { &zoomOutButton, 34 }, { &zoomInButton, 34 }
    };
    r.removeFromTop (grid::flow (r, items));
    infoLabel.setBounds (r.removeFromTop (22).reduced (4, 0));
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
        const auto err = app.playEdit (editId, a, b, true);
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
    if (on) { const auto err = app.playEdit (editId, a, b, true); if (err.isNotEmpty()) showError ("Play", err); }        // switched on while playing: start going round now
    else app.engine.setPlaybackLooping (false);                                                                             // switched off: finishes this pass and stops
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

/** I / O: the edit's IN / OUT point goes to the playhead (or, when nothing plays, to the cursor). */
void EditWindowComponent::setMark (bool in)
{
    auto* e = edit();
    if (e == nullptr) return;
    double t = app.playheadSeconds();
    if (! (t >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId)) t = timeline->cursorSeconds;
    if (t < 0) { showError (in ? "IN" : "OUT", "Click on the timeline (or play the edit) to say where, then press " + juce::String (in ? "I" : "O") + "."); return; }
    (in ? e->markIn : e->markOut) = t;
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

/** P: with pieces selected, the bounce IN and OUT go to the very start of the first and the very end of the last of them. */
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
    if (to < 0) { showError ("Bounce marks", "Click a piece first (or Ctrl + click several), then press P to mark all of it."); return; }
    e->markIn = from; e->markOut = to;
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
    if (c == 'i') { setMark (true);  return true; }
    if (c == 'o') { setMark (false); return true; }
    if (c == 'p') { markWholeSelected(); return true; }
    if ((c == '1' || c == '2') && ! k.getModifiers().isAnyModifierKeyDown()) { setFixMark (c == '1'); return true; }
    if (c == '5' && ! k.getModifiers().isAnyModifierKeyDown())            // 5: take both flags away; a piece selected whole is then what the process windows load
    {
        if (auto* e = edit()) { e->fixIn = -1.0; e->fixOut = -1.0; e->fixTracks.clear(); app.project.changed(); }
        return true;
    }
    if (c == '.' || c == '>') { app.changeWaveZoom (1.4f);        return true; }          // . bigger waveforms
    if (c == ',' || c == '<') { app.changeWaveZoom (1.0f / 1.4f); return true; }          // , smaller
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (k == juce::KeyPress::leftKey)  { timeline->zoom (1.0 / 1.4);          return true; }     // arrows zoom: left / right = horizontal,
    if (k == juce::KeyPress::rightKey) { timeline->zoom (1.4);                return true; }     // up / down = vertical (same keys as the take window)
    if (k == juce::KeyPress::upKey)    { timeline->zoomVertical (1.0 / 1.25); return true; }
    if (k == juce::KeyPress::downKey)  { timeline->zoomVertical (1.25);       return true; }
    if (c == 'c' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->centreOnPlayhead(); return true; }
    if (c == 'l' && ! k.getModifiers().isAnyModifierKeyDown()) { loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification); loopChanged(); return true; }
    if (c == 't') { trimSelected(); return true; }
    if (c == '4' && ! k.getModifiers().isAnyModifierKeyDown()) { placeFromTakeWindow(); return true; }     // same as key 4 in a take window
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    return false;
}

void EditWindowComponent::changeListenerCallback (juce::ChangeBroadcaster*) { repaint(); }

void EditWindowComponent::timerCallback()
{
    if (auto* fc = juce::Component::getCurrentlyFocusedComponent(); fc != nullptr && isParentOf (fc)) app.setTransportHandler (this, [this] { togglePlay(); });
    playButton.setButtonText (app.isPlaying() ? "Stop [Space]" : "Play [Space]");
    if (playheadMode.getToggleState() != app.playheadFollows) playheadMode.setToggleState (app.playheadFollows, juce::dontSendNotification);
    if (waveColourBtn.getToggleState() != app.waveColour) { waveColourBtn.setToggleState (app.waveColour, juce::dontSendNotification); repaint(); }
    if (auto* e = edit())
        infoLabel.setText (juce::String (takesInEdit (*e)) + (takesInEdit (*e) == 1 ? " Take" : " Takes") + (e->overdubs.empty() ? juce::String() : ", " + juce::String ((int) e->overdubs.size()) + " overdub(s)") + ", " + formatTime (e->lengthSeconds()).substring (3, 8)
                           + (e->markIn >= 0 ? "   IN " + formatTime (e->markIn).substring (3, 11) : juce::String())
                           + (e->markOut >= 0 ? "   OUT " + formatTime (e->markOut).substring (3, 11) : juce::String())
                           + (e->fixIn >= 0 ? "   [1] " + formatTime (e->fixIn).substring (3, 11) : juce::String())
                           + (e->fixOut >= 0 ? "   [2] " + formatTime (e->fixOut).substring (3, 11) : juce::String())
                           + (e->regions.size() > 1 ? "   -   click a join and press T (or double-click it) to trim.  Drag a piece to slide it.  Right-click a piece for its volume." : ""), juce::dontSendNotification);
}
} // namespace td

#include <array>
#include "WaveDraw.h"
#include "TakeWindow.h"
#include "FixTools.h"
#include "ColourPicker.h"

namespace td
{
static constexpr int kRulerH = 34, kLaneH = 26, kNameW = 150;

/** The scrolling content: ruler, one lane of take names, then one row per track. */
/** The little non-modal box for a take's IN Bar / OUT Bar (so it can be filled in while recording, with the keyboard already in the IN box). */
class BarsPopup : public juce::Component
{
public:
    BarsPopup (const juce::String& title, int bin, int bout, std::function<void (int, int)> ok, std::function<void()> more, std::function<void()> close)
        : onOk (std::move (ok)), onMore (std::move (more)), onClose (std::move (close))
    {
        setSize (270, 136);
        titleLabel.setText (title, juce::dontSendNotification); titleLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold)); titleLabel.setColour (juce::Label::textColourId, theme::text);
        inLabel.setText ("IN Bar", juce::dontSendNotification); outLabel.setText ("OUT Bar", juce::dontSendNotification);
        for (auto* l : { &inLabel, &outLabel }) { l->setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); }
        for (auto* e : { &inEd, &outEd })
        {
            e->setInputRestrictions (5, "0123456789"); e->setSelectAllWhenFocused (true); e->setJustification (juce::Justification::centredLeft);
            e->onEscapeKey = [this] { if (onClose) onClose(); };
            addAndMakeVisible (e);
        }
        inEd.setText (bin > 0 ? juce::String (bin) : juce::String(), false); outEd.setText (bout > 0 ? juce::String (bout) : juce::String(), false);
        inEd.onReturnKey = [this] { outEd.grabKeyboardFocus(); outEd.selectAll(); };
        outEd.onReturnKey = [this] { commit(); };
        okBtn.onClick = [this] { commit(); };
        moreBtn.onClick = [this] { if (onMore) onMore(); };
        for (auto* b : { &okBtn, &moreBtn }) addAndMakeVisible (b);
        addAndMakeVisible (titleLabel);
    }
    void focusFirst() { inEd.grabKeyboardFocus(); inEd.selectAll(); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel); g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff19d3b5)); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f, 2.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        titleLabel.setBounds (r.removeFromTop (20)); r.removeFromTop (4);
        auto a = r.removeFromTop (26); inLabel.setBounds (a.removeFromLeft (70)); inEd.setBounds (a.removeFromLeft (90)); r.removeFromTop (4);
        auto b = r.removeFromTop (26); outLabel.setBounds (b.removeFromLeft (70)); outEd.setBounds (b.removeFromLeft (90)); r.removeFromTop (6);
        auto c = r.removeFromTop (grid::btnH); okBtn.setBounds (c.removeFromLeft (70)); c.removeFromLeft (6); moreBtn.setBounds (c.removeFromLeft (90));
    }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; } return false; }
private:
    void commit() { if (onOk) onOk (inEd.getText().getIntValue(), outEd.getText().getIntValue()); }
    juce::Label titleLabel, inLabel, outLabel;
    juce::TextEditor inEd, outEd;
    juce::TextButton okBtn { "OK" }, moreBtn { "More..." };
    std::function<void (int, int)> onOk; std::function<void()> onMore, onClose;
};

class TakeTimeline : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    TakeTimeline (AppContext& a, const juce::Uuid& wid, juce::Viewport& vp)
        : app (a), windowId (wid), viewport (vp), thumbCache (64)
    {
        formatManager.registerBasicFormats();
        setWantsKeyboardFocus (true);
        startTimerHz (20);
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
    bool isTakeSelected (const juce::Uuid& id) const { return std::find (selectedTakes.begin(), selectedTakes.end(), id) != selectedTakes.end(); }
    static constexpr int kHeightLevels = 8;          // 0 = smallest (32 tracks per page) ... 7 = largest (one stereo track per page)
    int heightLevel = 3;
    int rowH = 56;

    /** Row height for a level: geometric steps between "32 rows fill the page" and "one row fills the page, with a peek of the next". */
    void applyHeightLevel()
    {
        const int avail = juce::jmax (120, viewport.getMaximumVisibleHeight() - kRulerH - kLaneH);
        const double small = juce::jmax (10.0, (double) avail / 32.0), large = juce::jmax (small * 2.0, (double) avail - 24.0);
        rowH = (int) std::round (small * std::pow (large / small, (double) heightLevel / (kHeightLevels - 1)));
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
        for (auto& t : app.project.tracks) if (w == nullptr || ! w->isHidden (t.id)) v.push_back (&t);
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

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wave);
        auto* w = def();
        if (w == nullptr) return;
        const auto tracks = shownTracks();
        const int top = kRulerH + kLaneH;
        const int rowsBottom = top + juce::jmax (1, (int) tracks.size()) * rowH;

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
        g.setFont (11.0f);
        for (double s = 0; s * pixelsPerSecond < getWidth(); s += step)
        {
            const int x = kNameW + (int) (s * pixelsPerSecond);
            g.setColour (theme::grid); g.drawVerticalLine (x, (float) kRulerH - 12, (float) getHeight());
            g.setColour (theme::dimText); g.setFont (12.0f); g.drawText (formatTime (s).substring (3, 8), x + 3, 3, 64, 16, juce::Justification::left);
        }
        g.setColour (theme::border); g.fillRect (0, kRulerH - 1, getWidth(), 1);

        const bool recordingHere = app.engine.isRecording() && w->findGroup (app.engine.currentTakeId()) != nullptr;
        const double playhead = app.playheadSeconds();
        for (auto& grp : w->groups)
        {
            const bool live = recordingHere && grp.id == app.engine.currentTakeId();
            const double len = live ? app.engine.recordedSeconds() : grp.lengthSeconds();
            const int x0 = xOf (grp.startSeconds);
            const int wpx = juce::jmax (4, (int) (len * pixelsPerSecond));

            auto lane = juce::Rectangle<int> (x0, kRulerH + 2, wpx, kLaneH - 4);
            const bool picked = isTakeSelected (grp.id);
            g.setColour (live ? juce::Colour (0xffb03030) : (picked ? theme::accent.darker (0.45f) : theme::accent));
            g.fillRect (lane);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (w->displayName (grp), lane.reduced (6, 0), juce::Justification::centredLeft, true);
            if (! live)                                     // the take's fades (25 ms unless changed): drag the top corners
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
            for (auto& tf : grp.files)
            {
                const int row = trackRow (tf.trackId);
                if (row < 0) continue;
                firstRow = juce::jmin (firstRow, row); lastRow = juce::jmax (lastRow, row);
                auto r = juce::Rectangle<int> (x0, top + row * rowH + (rowH > 24 ? 3 : 1), wpx, rowH - (rowH > 24 ? 6 : 2));
                const auto tc = chan::of (app.project, tf.trackId);
                g.setColour (live ? juce::Colour (0xff6a3434) : chan::clipFill (tc, picked));      // the track's own colour, darker when the take is selected
                g.fillRect (r);
                if (live) drawLiveWave (g, r.reduced (1), tf.trackId);
                else
                {
                    setWaveColour (g, chan::waveColour (tc));
                    juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (r.reduced (1));
                    auto* th = thumbnailFor (tf.file);
                    const double total = th != nullptr ? th->getTotalLength() : 0.0;
                    const auto wcd = app.waveColourOf (tf.file);
                    if (total > 0.0 && ! directWave.draw (g, r.reduced (1), tf.file, 0.0, total, app.waveZoom, wcd.get()))
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
                if ((grp.barIn > 0 || grp.dud) && r.getWidth() > 50 && r.getHeight() >= 22)       // "DUD   Bar 17 - 39" along the bottom of the take (DUD in red)
                {
                    const auto bars = barsLabel (grp.barIn, grp.barOut);
                    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
                    const auto font = g.getCurrentFont();
                    const int dudW = grp.dud ? juce::GlyphArrangement::getStringWidthInt (font, "DUD") + (bars.isNotEmpty() ? 10 : 0) : 0;
                    const int barsW = bars.isNotEmpty() ? juce::GlyphArrangement::getStringWidthInt (font, bars) : 0;
                    const int tw = juce::jmin (r.getWidth() - 4, dudW + barsW + 10);
                    auto strip = juce::Rectangle<int> (r.getX() + 2, r.getBottom() - 16, tw, 14);
                    g.setColour (juce::Colour (0xcc000000)); g.fillRect (strip);
                    auto t = strip.reduced (5, 0);
                    if (grp.dud) { g.setColour (juce::Colour (0xffff3b3b)); g.drawText ("DUD", t.removeFromLeft (dudW), juce::Justification::centredLeft); }
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

            // marked region
            if (w->markTake == grp.id)
            {
                if (w->markIn >= 0 && w->markOut > w->markIn)
                {
                    g.setColour (juce::Colour (0x3350e070));
                    g.fillRect (juce::Rectangle<int> (x0 + (int) (w->markIn * pixelsPerSecond), y0, (int) ((w->markOut - w->markIn) * pixelsPerSecond), y1 - y0));
                }
                if (w->markIn >= 0)  { g.setColour (juce::Colour (0xff1f9d4a)); g.fillRect (x0 + (int) (w->markIn * pixelsPerSecond) - 1, kRulerH, 3, y1 - kRulerH); }
                if (w->markOut >= 0) { g.setColour (juce::Colour (0xffd32f2f)); g.fillRect (x0 + (int) (w->markOut * pixelsPerSecond) - 1, kRulerH, 3, y1 - kRulerH); }
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
                    else for (auto& tf : grp.files) { const int row = trackRow (tf.trackId); if (row >= 0 && w->editUses (tf.trackId)) g.fillRect (juce::Rectangle<int> (mx, kRulerH + kLaneH + row * rowH, mw, rowH)); }
                }
                auto flag = [&] (double sec, juce::Colour c, const char* t, bool left)
                {
                    const int x = x0 + (int) (sec * pixelsPerSecond);
                    g.setColour (c); g.fillRect (x - 1, kRulerH, 2, y1 - kRulerH);
                    const int fw = 48; juce::Rectangle<int> f (left ? x : x - fw, kRulerH, fw, 13);
                    g.fillRect (f); g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                    g.drawText (t, f, juce::Justification::centred);
                };
                if (w->editIn >= 0)  flag (w->editIn, cin, "Edit IN", true);
                if (w->editOut >= 0) flag (w->editOut, cout, "Edit OUT", false);
            }
            // where this take has been used: a small flag "In 3" where a piece of it starts in the edit and "Out 4" where it ends, numbered like the edit points
            if (auto* usedEdit = app.project.findEdit (w->targetEdit))
                for (size_t ri = 0; ri < usedEdit->regions.size(); ++ri)
                {
                    const auto& rg = usedEdit->regions[ri];
                    if (rg.takeId != grp.id || rg.sampleRate <= 0) continue;
                    auto used = [&] (double sec, bool isIn, int number)
                    {
                        const int x = x0 + (int) (sec * pixelsPerSecond);
                        const auto c = isIn ? juce::Colour (0xff1a9c8a) : juce::Colour (0xff8a5cd0);
                        g.setColour (c.withAlpha (0.55f)); g.fillRect (x, y0, 1, y1 - y0);
                        const int fw = 38; juce::Rectangle<int> f (isIn ? x : x - fw, y0 - 12 < kRulerH + 14 ? kRulerH + 14 : y0 - 12, fw, 11);
                        g.setColour (c); g.fillRect (f);
                        g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                        g.drawText ((isIn ? "In " : "Out ") + juce::String (number), f, juce::Justification::centred);
                    };
                    used ((double) rg.srcIn / rg.sampleRate, true, (int) ri + 1);
                    used ((double) rg.srcOut / rg.sampleRate, false, (int) ri + 2);
                }
            // playhead
            if (playhead >= 0 && app.playInfo.kind != AppContext::PlayInfo::Kind::Edit && ! app.playInfo.windowTimeline && app.playInfo.windowId == windowId && app.playInfo.id == grp.id)
            {
                g.setColour (theme::playhead);
                g.fillRect (x0 + (int) (playhead * pixelsPerSecond), kRulerH, 2, y1 - kRulerH);
            }
            if (live)                                           // while recording the playhead is the end of the growing take
            {
                g.setColour (juce::Colour (0xffd32f2f));
                g.fillRect (x0 + (int) (len * pixelsPerSecond), kRulerH, 2, y1 - kRulerH);
            }
        }

        if (playhead >= 0 && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowTimeline && app.playInfo.windowId == windowId)
        {                                                                          // the whole window is playing: one moving line across every row
            g.setColour (theme::playhead); g.fillRect (xOf (playhead), kRulerH, 2, getHeight() - kRulerH);
        }
        {   // the playhead: set by clicking the ruler; Space plays from here, IN / OUT are marked here
            const int px = xOf (w->playheadSeconds);
            g.setColour (theme::text); g.fillRect (px, kRulerH, 1, getHeight() - kRulerH);
            juce::Path tri; tri.addTriangle ((float) px - 6.0f, (float) kRulerH - 12.0f, (float) px + 7.0f, (float) kRulerH - 12.0f, (float) px + 0.5f, (float) kRulerH - 1.0f);
            g.fillPath (tri);
        }

        const int sx = viewport.getViewPositionX();
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            auto r = juce::Rectangle<int> (sx, top + (int) i * rowH, kNameW, rowH);
            const auto tc = chan::of (app.project, tracks[i]->id);
            g.setColour (theme::panel); g.fillRect (r);
            g.setColour (tc.withAlpha (0.20f)); g.fillRect (r);                              // the name plate carries the track's colour
            g.setColour (tc); g.fillRect (r.removeFromLeft (6));
            g.setColour (tc.darker (0.3f).withAlpha (0.8f)); g.fillRect (sx, top + (int) i * rowH + rowH - 1, kNameW, 1);
            g.setColour (theme::text); g.setFont (juce::jlimit (9.0f, 14.0f, (float) rowH - 4.0f));
            const bool tall = rowH >= 44;
            g.drawText (tracks[i]->name, r.reduced (8, 0).withTrimmedRight (36).withTrimmedBottom (tall ? 14 : 0), juce::Justification::centredLeft, true);
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
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragId = {}; dragging = false; markDragTake = {}; markActive = false;
        closeBarsPopup();
        grabKeyboardFocus();
        auto* w = def();
        if (w == nullptr) return;
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
            if (auto* bg = groupAt (e.getPosition(), true)) { showBars (bg->id, e.getPosition()); return; }
        const bool inLane = e.y >= kRulerH && e.y < kRulerH + kLaneH;
        const int sx = viewport.getViewPositionX();
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
        if (auto* grp = groupAt (e.getPosition(), e.mods.isPopupMenu()))
        {
            if (inLane && e.getNumberOfClicks() > 1) { showSettings (grp->id, false); return; }
            if (e.mods.isCommandDown() || e.mods.isCtrlDown())              // Ctrl + click: add this take to the selection, or take it out again
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
                markAlt = e.mods.isAltDown(); markRow0 = (e.y - kRulerH - kLaneH) / juce::jmax (1, rowH);      // Alt + drag: only the track(s) the mouse covers
            }
            // clicking a take only selects it: the playhead stays where it is
        }
        else if (e.y >= kRulerH && e.x >= kNameW && ! (e.mods.isCommandDown() || e.mods.isCtrlDown()) && ! selectedTakes.empty())
        {
            selectedTakes.clear();                                         // a click on an empty patch deselects every take
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* w = def();
        if (w == nullptr) return;
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

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto* w = def(); bool isIn = true;
        setMouseCursor (w != nullptr && fadeHandleAt (*w, e.getPosition(), isIn) != nullptr ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (markActive) { markActive = false; app.project.changed(); }
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

    /** Where Edit IN / OUT are set: the live position while recording or playing here, otherwise the playhead. Returns the take and the time inside it. */
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

private:
    TakeWindowDef* def() const { return app.project.findTakeWindow (windowId); }
    /** The ARM switch of the track row that starts at y (it stays at the left edge while the window is scrolled sideways). */
    juce::Rectangle<int> armRect (int rowTop) const
    {
        const int sx = viewport.getViewPositionX();
        return { sx + kNameW - 36, rowTop + juce::jmax (2, (rowH - 18) / 2), 32, 18 };
    }
    int xOf (double seconds) const { return kNameW + (int) (seconds * pixelsPerSecond); }

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

    /** Where the playhead is right now: the end of the growing take while recording, the moving position while playing here, otherwise where you put it. */
    double livePosition() const
    {
        auto* w = const_cast<TakeTimeline*> (this)->def();
        if (w == nullptr) return -1.0;
        if (app.engine.isRecording())
        {
            if (auto* g = w->findGroup (app.engine.currentTakeId())) return g->startSeconds + app.engine.recordedSeconds();
        }
        else
        {
            const double ph = app.playheadSeconds();
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
    void drawLiveWave (juce::Graphics& g, juce::Rectangle<int> r, const juce::Uuid& trackId)
    {
        const juce::int16* data = nullptr; int bins = 0, binSamples = 0, nc = 1;
        if (! app.engine.getLivePeaks (trackId, data, bins, binSamples, &nc) || bins <= 0 || nc <= 0 || app.engine.getSampleRate() <= 0) return;
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
            fillEnvelope (g, (float) r.getX(), tops, bots);
        }
    }

    /** Right-click on a track name: remove the track from this take window only. */
    void showTrackMenu (const juce::Uuid& trackId, const juce::String& trackName)
    {
        juce::PopupMenu m;
        const auto where = juce::Rectangle<int> (juce::Desktop::getMousePosition(), juce::Desktop::getMousePosition()).expanded (4);
        m.addItem ("Colour...", [this, trackId, where] { showColourPickerAt (app, trackId, where); });
        m.addSeparator();
        m.addItem ("Remove track '" + trackName + "' from this take window...", [this, trackId, trackName]
        {
            if (app.engine.isRecording()) { showError ("Recording", "Stop recording first."); return; }
            confirmAsync ("Remove track from this take window",
                          "Remove '" + trackName + "' from this take window?\n\nIts clips disappear from this window only. Other take windows, the mixers, "
                          "the edits and the audio files on the disk are not touched. Recording the track again into this window brings it back.",
                          "Remove", [this, trackId]
                          {
                              if (auto* w = def()) { w->removeTrackClips (trackId); app.project.changed(); updateSize(); repaint(); }
                          });
        });
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
        closeBarsPopup();
        juce::Component::SafePointer<TakeTimeline> safe (this);
        barsPopup.reset (new BarsPopup ("Take " + pad3 (g->number) + " - bars it starts and ends in", g->barIn, g->barOut,
            [safe, gid] (int bin, int bout)
            {
                if (safe == nullptr) return;
                if (bin <= 0) bin = bout = 0; else if (bout <= 0) bout = bin;
                if (bout < bin) { showError ("Bars", "The OUT Bar must not be before the IN Bar."); return; }
                safe->applyBars (gid, bin, bout);
                safe->closeBarsPopup();
            },
            [safe, gid] { if (safe == nullptr) return; safe->closeBarsPopup(); safe->showSettings (gid, true, true); },
            [safe] { if (safe != nullptr) safe->closeBarsPopup(); }));
        host->addAndMakeVisible (barsPopup.get());
        auto pt = host->getLocalPoint (this, where);
        barsPopup->setTopLeftPosition (juce::jlimit (0, juce::jmax (0, host->getWidth() - barsPopup->getWidth()), pt.x),
                                       juce::jlimit (0, juce::jmax (0, host->getHeight() - barsPopup->getHeight()), pt.y));
        barsPopup->toFront (true);
        barsPopup->focusFirst();
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
    std::unique_ptr<BarsPopup> barsPopup;

    void showSettings (const juce::Uuid& gid, bool asMenu, bool fromBars = false)
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
            if (w->markTake == gid) { w->markTake = juce::Uuid::null(); w->markIn = w->markOut = -1.0; }
            if (w->cursorTake == gid) w->cursorTake = juce::Uuid::null();
            w->groups.erase (std::remove_if (w->groups.begin(), w->groups.end(), [&] (const TakeGroup& x) { return x.id == gid; }), w->groups.end());
            app.project.changed(); updateSize(); repaint();
            return;
        }
        auto* aw = new juce::AlertWindow ("Take settings", "Take " + pad3 (g->number) + " - leave the name empty to use the piece title \"" + w->name + "\".\n"
                                          "Every file of this take is renamed to match.", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("label", g->label, "Take name");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        for (auto it = thumbs.begin(); it != thumbs.end();)
            if (! juce::File (it->first).existsAsFile()) it = thumbs.erase (it); else ++it;
        updateSize();
        repaint();
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
            for (int c = 0; c < juce::jmin (kMaxMeterCh, t->channelCount()); ++c)
            {
                float pk = 0.0f;
                const int in = t->inputOf (c);
                if (juce::isPositiveAndBelow (in, (int) app.inPeaks.size())) pk = app.inPeaks[(size_t) in];
                auto& lv = m.level[(size_t) c]; auto& hv = m.hold[(size_t) c]; auto& ha = m.holdAt[(size_t) c];
                lv = juce::jmax (pk, lv * 0.8f);
                if (pk >= hv || now - ha > 1500) { hv = pk >= hv ? pk : lv; ha = now; }
            }
        }
        repaint (viewport.getViewPositionX(), kRulerH + kLaneH, kNameW, juce::jmax (1, (int) shownTracks().size()) * rowH);
    }

    void timerCallback() override
    {
        updateMeters();
        if (app.engine.isRecording() || app.isPlaying()) updateSize();
        if (app.engine.isRecording() || app.isPlaying()) { followPlayhead(); repaint(); }
        else followActive = false;
    }

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
    juce::AudioThumbnailCache thumbCache;
    DirectWaveCache directWave { formatManager };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
    juce::Uuid dragId; bool dragging = false; double dragOriginSeconds = 0.0;
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
    nameLabel.setText ("Piece:", juce::dontSendNotification);
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (nameEditor);
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
    nameEditor.onReturnKey = [this, commitName] { commitName(); juce::Component::unfocusAllComponents(); };     // Enter: confirm and leave the box, so the keys (R, Space, ...) work again

    recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb3261e));
    recordButton.onClick = [this] { app.toggleRecord (windowId); };
    armAllButton.setTooltip ("Arm every track of this window for recording (you can also click ARM on a track's name)");
    armNoneButton.setTooltip ("Disarm every track");
    armAllButton.onClick  = [this] { if (! app.engine.isRecording()) { for (auto& t : app.project.tracks) t.setArmed (true);  app.project.changed(); } };
    armNoneButton.onClick = [this] { if (! app.engine.isRecording()) { for (auto& t : app.project.tracks) t.setArmed (false); app.project.changed(); } };
    addAndMakeVisible (armAllButton); addAndMakeVisible (armNoneButton);
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
    playMarkedButton.onClick = [this] { playMarked(); };
    inButton.onClick = [this] { markIn(); };
    outButton.onClick = [this] { markOut(); };
    toEditButton.onClick = [this] { toEdit(false); };
    toEditAtButton.onClick = [this] { toEdit(true); };
    editInButton.onClick = [this] { editMarkIn(); };
    editOutButton.onClick = [this] { editMarkOut(); };
    overdubBox.setTooltip ("Pieces sent to the edit are laid OVER it and play together with what is already there");
    overdubBox.onClick = [this] { if (auto* w = def()) { w->overdub = overdubBox.getToggleState(); app.project.changed(); } };
    editInButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1e6fd9));
    editOutButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe08a00));
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
    loopToggle.setTooltip ("Loop: when lit, Play and Play marked repeat the marked area (Edit IN / OUT [1] [2] if set, otherwise Bounce IN / OUT [I] [O]) over and over until you press Stop. Key L.");
    loopToggle.onClick = [this] { loopChanged(); };
    loopToggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (loopToggle);
    if (auto* w0 = def()) overdubBox.setToggleState (w0->overdub, juce::dontSendNotification);
    addAndMakeVisible (overdubBox);
    editWindowButton.onClick = [this] { openEditWindow(); };
    bounceButton.onClick = [this] { openBounce(); };
    importButton.setTooltip ("Bring in the takes of a session recorded in another program (multichannel / polyphonic WAV, or one mono / stereo file per track). "
                             "You can also drag the files, or their folder, onto this window.");
    importButton.onClick = [this] { fixtools::showImportMenu (app, windowId, &importButton); };
    barSearchButton.setTooltip ("Search for bar: type a bar number and every take here that contains it (from its IN Bar to its OUT Bar, set by right-clicking a take) is framed in blue-green, so you can see which takes have that bar. Click again to clear.");
    barSearchButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff19a08a));
    barSearchButton.onClick = [this] { toggleBarSearch(); };
    pitchButton.setTooltip ("Pitch correction: shifts the part marked with Edit IN [1] and Edit OUT [2] on every track of the take, by semitones and / or cents");
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
    pitchButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::pitchTake (app, windowId, this); }); };
    pitchCurveButton.setTooltip ("Pitch curve: draw a line of pitch against time over the marked part (+100 cents at the top, -100 at the bottom), for a note that drifts flat or sharp; audition it, then accept or revert");
    pitchCurveButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::pitchCurveTake (app, windowId, this); }); };
    repairButton.setTooltip ("Spectral Repair: shows the marked part (Edit IN [1] / Edit OUT [2]) of all tracks as one spectrogram; draw a box round a noise (drag its edges to adjust it) and rebuild it from the clean sound next to it");
    repairButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::repairTake (app, windowId, this, false); }); };
    declickButton.setTooltip ("De-Click: the same window as Spectral Repair, but it finds and mends clicks (the spectrogram helps you see them)");
    declickButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::repairTake (app, windowId, this, true); }); };
    exportProcButton.setTooltip ("Export for Processing: sends the marked part (Edit IN [1] / Edit OUT [2], all tracks) to the Processing Media folder to be corrected in other software (e.g. iZotope RX). Re-link it afterwards with a right-click on the 'Waiting for corrected audio' block");
    exportProcButton.onClick = [this, withWholeTake] { withWholeTake ([this] { fixtools::exportForProcessingTake (app, windowId, this); }); };
    undoFixButton.setTooltip ("Undo the last pitch correction, repair, de-click or export for processing (the original audio files were never touched)");
    undoFixButton.onClick = [this] { fixtools::undoLastFix (app); };
    bounceButton.setTooltip ("Make a master file of the marked take (or the marked part of it) through the processing mixer");
    inButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    outButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb3261e));
    toEditButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &zoomInButton, &zoomOutButton, &playButton, &playMarkedButton,
                                                              &inButton, &outButton, &editInButton, &editOutButton, &toEditButton, &toEditAtButton, &editWindowButton, &bounceButton, &importButton, &pitchButton, &pitchCurveButton, &repairButton, &declickButton, &exportProcButton, &undoFixButton, &barSearchButton, &timeLabel, &statusLabel, &markLabel })
        addAndMakeVisible (c);
    for (auto* b : std::initializer_list<juce::Button*> { &recordButton, &zoomInButton, &zoomOutButton, &playButton, &playMarkedButton,
                                                          &inButton, &outButton, &editInButton, &editOutButton, &toEditButton, &toEditAtButton, &editWindowButton, &bounceButton, &importButton, &pitchButton, &pitchCurveButton, &repairButton, &declickButton, &exportProcButton, &undoFixButton, &barSearchButton })
        b->setWantsKeyboardFocus (false);       // keep the keyboard shortcuts working after a button click
    timeLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    statusLabel.setColour (juce::Label::textColourId, theme::warn);
    markLabel.setColour (juce::Label::textColourId, theme::text);

    sendCaption.setText ("To edit goes into:", juce::dontSendNotification);
    addAndMakeVisible (sendCaption); addAndMakeVisible (sendBox);
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

void TakeWindowComponent::resized()
{
    auto r = getLocalBounds().reduced (6);
    using grid::FlowItem;
    const std::vector<FlowItem> items
    {
        { &nameLabel, 45 }, { &nameEditor, 200, grid::btnH, 10 }, { &takeBox, 84, 58, 14 },
        { &recordButton, grid::btnW, grid::btnH, 6 }, { &timeLabel, 150, 28, 14 },
        { &startButton, 40, grid::btnH, 2 }, { &backButton, 40, grid::btnH, 2 }, { &playButton }, { &fwdButton, 40, grid::btnH, 2 }, { &endTransportButton, 40, grid::btnH, 6 }, { &playMarkedButton }, { &playheadMode, 34, grid::btnH, 2 }, { &waveColourBtn, 34, grid::btnH, 2 }, { &loopToggle, 84, grid::btnH, 14 },
        { &inButton }, { &outButton, grid::btnW, grid::btnH, 14 },
        { &editInButton }, { &editOutButton }, { &toEditButton }, { &toEditAtButton }, { &overdubBox, grid::btnW - 10, grid::btnH, 14 },
        { &editWindowButton }, { &bounceButton, grid::btnW, grid::btnH, 14 },
        { &sendCaption, 120, grid::btnH, 2 }, { &sendBox, 200, grid::btnH, 14 },
        { &armAllButton }, { &armNoneButton, grid::btnW, grid::btnH, 14 },
        { &importButton }, { &pitchButton }, { &pitchCurveButton, grid::btnW + 24 }, { &repairButton, grid::btnW + 30 }, { &declickButton }, { &exportProcButton, grid::btnW + 50 }, { &undoFixButton, grid::btnW, grid::btnH, 14 }, { &barSearchButton, grid::btnW + 14, grid::btnH, 14 },
        { &zoomOutButton, 34 }, { &zoomInButton, 34 }
    };
    r.removeFromTop (grid::flow (r, items));                           // everything on one line when there is room, otherwise it spills onto more
    auto info = r.removeFromTop (22);
    statusLabel.setBounds (info.removeFromRight (juce::jmin (330, info.getWidth() / 3)));
    markLabel.setBounds (info.reduced (4, 0));
    r.removeFromTop (4);
    if (viewportOwner != nullptr) { viewportOwner->setBounds (r); if (timeline != nullptr) timeline->applyHeightLevel(); }
}

// ----------------------------------------------------------------------------- editing actions
void TakeWindowComponent::markIn()
{
    auto* w = def();
    if (w == nullptr) return;
    double sec = 0.0;
    auto* g = timeline->markPoint (sec);          // the live position while recording or playing, otherwise the playhead
    if (g == nullptr) { showError ("Mark IN", "Click the ruler at the top, inside a take, to place the playhead first (or press I while a take plays)."); return; }
    if (w->markTake != g->id) { w->markTake = g->id; w->markOut = -1.0; }
    w->markIn = sec;
    if (w->markOut >= 0 && w->markOut <= w->markIn) w->markOut = -1.0;
    app.project.changed();
}

void TakeWindowComponent::markOut()
{
    auto* w = def();
    if (w == nullptr) return;
    double sec = 0.0;
    auto* g = timeline->markPoint (sec);
    if (g == nullptr) { showError ("Mark OUT", "Click the ruler at the top, inside a take, to place the playhead first (or press O while a take plays)."); return; }
    if (w->markTake != g->id) { w->markTake = g->id; w->markIn = -1.0; }
    w->markOut = sec;
    if (w->markIn >= 0 && w->markIn >= w->markOut) w->markIn = -1.0;
    app.project.changed();
}

/** P: with a take selected, the bounce IN and OUT go to its very start and end. */
void TakeWindowComponent::markWholeSelected()
{
    auto* w = def();
    if (w == nullptr) return;
    const TakeGroup* g = nullptr;
    for (auto& id : timeline->selectedTakes) { g = w->findGroup (id); if (g != nullptr) break; }
    if (g == nullptr) { showError ("Bounce marks", "Click a take first (in the grey bar above its tracks), then press P to mark all of it."); return; }
    w->markTake = g->id; w->markIn = 0.0; w->markOut = g->lengthSeconds();
    app.project.changed();
}

void TakeWindowComponent::editMarkIn()
{
    auto* w = def();
    if (w == nullptr) return;
    double sec = 0.0;
    auto* g = timeline->markPoint (sec);
    if (g == nullptr) { showError ("Edit IN", "Put the playhead inside a take first (click the ruler at the top), or press 1 while a take plays."); return; }
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
    if (g == nullptr) { showError ("Edit OUT", "Put the playhead inside a take first (click the ruler at the top), or press 2 while a take plays."); return; }
    if (w->editTake != g->id) { w->editTake = g->id; w->editIn = -1.0; }
    w->editOut = sec;
    if (w->editIn >= 0 && w->editIn >= w->editOut) w->editIn = -1.0;
    app.project.changed();
}

void TakeWindowComponent::toEdit (bool atEditPlayhead)
{
    auto* w = def();
    if (w == nullptr) return;
    const bool marksOk = w->findGroup (w->editTake) != nullptr && w->editIn >= 0.0 && w->editOut > w->editIn;
    if (marksOk && app.project.findEdit (w->targetEdit) == nullptr)        // the first piece sent from this window: the edit does not exist yet, so it gets its name now
    {
        auto* aw = new juce::AlertWindow ("Name the edit", "This is the first piece you send from \"" + w->name + "\".\nWhat should the edit be called?", juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor ("name", w->name + " - edit", "Edit name");
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto safe = juce::Component::SafePointer<TakeWindowComponent> (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw, atEditPlayhead] (int r)
        {
            if (r == 1 && safe != nullptr)
                if (auto* w2 = safe->def())
                    if (safe->app.project.findEdit (w2->targetEdit) == nullptr)
                    {
                        auto nm = aw->getTextEditorContents ("name").trim();
                        if (nm.isEmpty()) nm = w2->name + " - edit";
                        auto& e = safe->app.project.addEdit (nm, safe->windowId);
                        w2->targetEdit = e.id;
                        safe->app.project.changed();
                        safe->sendToEdit (atEditPlayhead);
                    }
            delete aw;
        }), false);
        return;
    }
    sendToEdit (atEditPlayhead);
}

void TakeWindowComponent::sendToEdit (bool atEditPlayhead)
{
    juce::Uuid editId;
    auto err = app.copyMarkedToEdit (windowId, atEditPlayhead, &editId);
    if (err.isNotEmpty()) { showError ("To edit", err); return; }
    // You stay in this window: send all the takes you want, then go and edit. The edit's window is made sure to exist (opened behind this one the
    // first time) but it never comes to the front and never takes the keyboard.
    if (app.showEditBehind) app.showEditBehind (editId, this);
}

void TakeWindowComponent::togglePlay()
{
    if (app.isPlaying()) { app.stopPlayback(); return; }
    auto* w = def();
    if (w == nullptr) return;
    if (loopToggle.getToggleState())                                    // loop on and an area marked: go round it
    {
        juce::Uuid lt; double li = 0.0, lo = 0.0;
        if (markedRange (lt, li, lo)) { const auto err = app.playTake (windowId, lt, li, lo, true); if (err.isNotEmpty()) showError ("Play", err); return; }
        app.setNotice ("Loop is on, but no area is marked (IN and OUT): playing normally.");
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
    const auto take = useEdit ? w->editTake : w->markTake;
    const double in = useEdit ? w->editIn : w->markIn, out = useEdit ? w->editOut : w->markOut;
    if (w->findGroup (take) == nullptr || in < 0) { showError ("Play marked", "Mark an IN point (and an OUT point) first."); return; }
    auto err = app.playTake (windowId, take, in, out > in ? out : -1.0, loopToggle.getToggleState() && out > in);
    if (err.isNotEmpty()) showError ("Play", err);
}

bool TakeWindowComponent::markedRange (juce::Uuid& take, double& in, double& out) const
{
    auto* w = app.project.findTakeWindow (windowId);
    if (w == nullptr) return false;
    const bool useEdit = w->findGroup (w->editTake) != nullptr && w->editIn >= 0;
    take = useEdit ? w->editTake : w->markTake;
    in = useEdit ? w->editIn : w->markIn; out = useEdit ? w->editOut : w->markOut;
    return w->findGroup (take) != nullptr && in >= 0 && out > in;
}

void TakeWindowComponent::loopChanged()
{
    const bool on = loopToggle.getToggleState();
    const bool mine = app.isPlaying() && ! app.engine.isRecording() && app.playInfo.kind == AppContext::PlayInfo::Kind::Take && app.playInfo.windowId == windowId;
    juce::Uuid take; double in = 0.0, out = 0.0;
    if (on && ! markedRange (take, in, out)) { app.setNotice ("Loop: mark an area first (Edit IN / OUT [1] [2], or Bounce IN / OUT [I] [O]): the loop plays the area between them."); return; }
    if (! mine) return;
    if (on) { const auto err = app.playTake (windowId, take, in, out, true); if (err.isNotEmpty()) showError ("Play", err); }
    else app.engine.setPlaybackLooping (false);
}

void TakeWindowComponent::openBounce()
{
    auto* w = def();
    if (w == nullptr || ! app.showBounce) return;
    BounceContext c;
    c.kind = BounceContext::Kind::Take; c.id = windowId;
    for (auto& g : w->groups) if (timeline->isTakeSelected (g.id)) c.selected.push_back (g.id);     // only takes that still exist
    const auto takeId = (! w->markTake.isNull()) ? w->markTake : w->cursorTake;
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
        e = &app.project.addEdit ((w ? w->name : juce::String ("Piece")) + " - edit", windowId);
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
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (k == juce::KeyPress::spaceKey)                                  // Space stops a recording; otherwise it plays (or stops playing) from the playhead
    {
        if (app.engine.isRecording()) { if (hereRecording()) app.toggleRecord (windowId); return true; }
        togglePlay(); return true;
    }
    if (c == 'r' && ! k.getModifiers().isAnyModifierKeyDown())          // R starts recording into this window
    {
        if (! app.engine.isRecording()) app.toggleRecord (windowId);
        return true;
    }
    if (k == juce::KeyPress::homeKey) { goTo (0.0);  return true; }
    if (k == juce::KeyPress::endKey)  { goToEnd();   return true; }
    if (c == 'i')                       { markIn();  return true; }
    if (c == 'o')                       { markOut(); return true; }
    if (c == 'p')                       { markWholeSelected(); return true; }
    if (c == 'd' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->toggleDud(); return true; }
    if (c == 'l' && ! k.getModifiers().isAnyModifierKeyDown()) { loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification); loopChanged(); return true; }
    if (c == 'c' && ! k.getModifiers().isAnyModifierKeyDown()) { timeline->centreOnPlayhead(); return true; }
    if (c == '.' || c == '>')           { app.changeWaveZoom (1.4f);        return true; }     // . bigger waveforms
    if (c == ',' || c == '<')           { app.changeWaveZoom (1.0f / 1.4f); return true; }     // , smaller
    if (! k.getModifiers().isAnyModifierKeyDown())
    {
        if (c == '1') { editMarkIn();  return true; }
        if (c == '2') { editMarkOut(); return true; }
        if (c == '5') { if (auto* w5 = def()) { w5->editTake = juce::Uuid::null(); w5->editIn = w5->editOut = -1.0; w5->editTracks.clear(); app.project.changed(); } return true; }     // 5: take both Edit flags away
        if (c == '3') { toEdit (false); return true; }
        if (c == '4') { toEdit (true);  return true; }
    }
    // Right = zoom in, Left = zoom out (time). Shift + arrow nudges the cursor instead.
    if (k == juce::KeyPress::leftKey)   { if (k.getModifiers().isShiftDown()) nudgeCursor (-0.01); else timeline->zoom (1.0 / 1.4); return true; }
    if (k == juce::KeyPress::rightKey)  { if (k.getModifiers().isShiftDown()) nudgeCursor (0.01);  else timeline->zoom (1.4);       return true; }
    // Up = all tracks smaller, Down = all tracks bigger (8 steps)
    if (k == juce::KeyPress::upKey)     { timeline->changeHeightLevel (-1); return true; }
    if (k == juce::KeyPress::downKey)   { timeline->changeHeightLevel (+1); return true; }
    return false;
}

void TakeWindowComponent::changeListenerCallback (juce::ChangeBroadcaster*) { fillSendBox(); repaint(); }

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
    timeLabel.setText (hereRec ? formatTime (app.engine.recordedSeconds()) : juce::String ("--:--:--.---"), juce::dontSendNotification);
    statusLabel.setText (hereRec && app.engine.overruns() > 0 ? "DISK OVERRUN - audio may have been lost!" : "", juce::dontSendNotification);
    playButton.setButtonText (app.isPlaying() ? "Stop [Space]" : "Play [Space]");

    juce::String m;
    if (auto* w = def())
    {
        auto fmt = [] (double v) { return v >= 0 ? formatTime (v).substring (3) : juce::String ("--"); };
        if (overdubBox.getToggleState() != w->overdub) overdubBox.setToggleState (w->overdub, juce::dontSendNotification);
        if (auto* ge = w->findGroup (w->editTake))
        {
            m = "EDIT " + w->displayName (*ge) + ":  Edit IN " + fmt (w->editIn) + "   Edit OUT " + fmt (w->editOut);
            if (w->editIn >= 0 && w->editOut > w->editIn) m += "   (" + juce::String (w->editOut - w->editIn, 2) + " s)";
            if (w->findGroup (w->markTake) != nullptr) m += "      BOUNCE  I " + fmt (w->markIn) + "  O " + fmt (w->markOut);
        }
        else if (auto* g = w->findGroup (w->markTake))
        {
            m = "BOUNCE " + w->displayName (*g) + ":  IN " + (w->markIn >= 0 ? formatTime (w->markIn).substring (3) : juce::String ("--"))
                + "   OUT " + (w->markOut >= 0 ? formatTime (w->markOut).substring (3) : juce::String ("--"));
            if (w->markIn >= 0 && w->markOut > w->markIn) m += "   (" + juce::String (w->markOut - w->markIn, 2) + " s)";
        }
        else if (auto* gc = w->findGroup (w->cursorTake))
            m = "Playhead: " + w->displayName (*gc) + "  " + formatTime (w->cursorSeconds).substring (3);
        else m = "Playhead: " + formatTime (w->playheadSeconds).substring (3) + "  (between takes)";
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
    if (bar <= 0) { barSearchButton.setButtonText ("Search for bar"); barSearchButton.setToggleState (false, juce::dontSendNotification); return; }
    int n = 0;
    if (auto* w = app.project.findTakeWindow (windowId))
        for (auto& g : w->groups) if (barsContain (g.barIn, g.barOut, bar)) ++n;
    barSearchButton.setButtonText ("Bar " + juce::String (bar) + ": " + juce::String (n) + (n == 1 ? " take" : " takes"));
    barSearchButton.setToggleState (true, juce::dontSendNotification);
}

void TakeWindowComponent::toggleBarSearch()
{
    if (searchBar > 0) { setBarSearch (0); return; }
    auto* aw = new juce::AlertWindow ("Search for bar", "Which bar do you need? Every take that contains it will be framed.", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("bar", {}, "Bar");
    if (auto* te = aw->getTextEditor ("bar")) te->setInputRestrictions (5, "0123456789");
    aw->addButton ("Search", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

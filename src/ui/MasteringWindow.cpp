#include "MasteringWindow.h"

namespace td
{
struct MasterField
{
    juce::Label caption;
    juce::TextEditor editor;
    std::function<juce::String*()> ref;
};

namespace
{
juce::String lengthText (juce::int64 samples, double rate)
{
    if (samples <= 0 || rate <= 0) return "empty";
    const double s = (double) samples / rate;
    const int m = (int) (s / 60.0);
    return juce::String (m) + ":" + juce::String (s - m * 60.0, 1).paddedLeft ('0', 4);
}
juce::String secondsText (int sectors) { return juce::String ((double) sectors / 75.0, 2) + " s"; }
void styleCaption (juce::Label& l, const juce::String& text, bool bold = false)
{
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (13.0f, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, bold ? theme::text : theme::dimText);
}
const int kRowH = 30;
} // namespace

// ============================================================================= the list of edits (order, tick, name)
struct MasterListEntry { juce::Uuid id; bool include = true; juce::String name, detail, status; bool bad = false; };

class MasterListPanel : public juce::Component
{
public:
    std::function<void (const juce::Uuid&)> onSelect;
    std::function<void (const juce::Uuid&, bool)> onInclude;
    std::function<void (const juce::Uuid&, const juce::String&)> onRename;
    std::function<void (int, int)> onMove;

    explicit MasterListPanel (bool namesEditable) : editable (namesEditable)
    {
        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }
    void setEntries (const std::vector<MasterListEntry>& e, const juce::Uuid& sel)
    {
        selected = sel;
        rows.clear();
        int i = 0;
        for (auto& en : e) { auto* r = rows.add (new Row (*this, en, i++, editable)); holder.addAndMakeVisible (r); }
        layoutRows();
    }
    void setSelected (const juce::Uuid& id) { selected = id; for (auto* r : rows) r->repaint(); }
    void resized() override { viewport.setBounds (getLocalBounds()); layoutRows(); }
    void paint (juce::Graphics& g) override { g.setColour (theme::wave); g.fillRect (getLocalBounds()); g.setColour (theme::border); g.drawRect (getLocalBounds(), 1); }
    void layoutRows()
    {
        const int w = juce::jmax (100, viewport.getMaximumVisibleWidth());
        holder.setSize (w, juce::jmax (viewport.getHeight(), rows.size() * kRowH));
        int y = 0;
        for (auto* r : rows) { r->setBounds (0, y, w, kRowH); y += kRowH; }
    }
    int count() const { return rows.size(); }
    juce::Uuid selected;

private:
    class Row : public juce::Component
    {
    public:
        Row (MasterListPanel& o, const MasterListEntry& e, int idx, bool editable) : owner (o), entry (e), index (idx)
        {
            box.setToggleState (entry.include, juce::dontSendNotification);
            box.onClick = [this] { if (owner.onInclude) owner.onInclude (entry.id, box.getToggleState()); };
            addAndMakeVisible (box);
            if (editable)
            {
                nameEd.setText (entry.name, false);
                nameEd.setSelectAllWhenFocused (false);
                nameEd.onTextChange = [this] { if (owner.onRename) owner.onRename (entry.id, nameEd.getText()); };
                nameEd.addMouseListener (this, false);
                addAndMakeVisible (nameEd);
            }
            else
            {
                nameLabel.setText (entry.name, juce::dontSendNotification);
                nameLabel.setInterceptsMouseClicks (false, false);
                addAndMakeVisible (nameLabel);
            }
        }
        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromLeft (20);
            box.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (22, 22));
            r.removeFromLeft (28);                                                       // the number
            detailWidth = 84;
            r.removeFromRight (detailWidth + 6 + statusWidth + 4);
            nameEd.setBounds (r.reduced (0, 3)); nameLabel.setBounds (r);
        }
        void paint (juce::Graphics& g) override
        {
            const bool sel = owner.selected == entry.id;
            g.setColour (sel ? theme::selected : (index % 2 ? theme::rowAlt : theme::row)); g.fillRect (getLocalBounds());
            g.setColour (theme::dimText);
            for (int k = 0; k < 3; ++k) { g.fillRect (6, 9 + k * 5, 10, 2); }                        // the grip
            g.setFont (juce::FontOptions (13.0f));
            g.setColour (entry.include ? theme::text : theme::dimText);
            g.drawText (juce::String (index + 1), 46, 0, 26, getHeight(), juce::Justification::centredRight);
            g.setColour (theme::dimText);
            g.drawText (entry.detail, getWidth() - detailWidth - 6, 0, detailWidth, getHeight(), juce::Justification::centredRight);
            if (entry.status.isNotEmpty())                                                       // the state of this piece's render
            {
                g.setFont (juce::FontOptions (11.5f));
                g.setColour (entry.bad ? theme::warn : theme::dimText);
                g.drawText (entry.status, getWidth() - detailWidth - 6 - statusWidth - 4, 0, statusWidth, getHeight(), juce::Justification::centredRight);
            }
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.eventComponent == this && e.x < 20) { dragging = true; baseY = getY(); toFront (false); }
            else if (owner.onSelect) owner.onSelect (entry.id);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging) return;
            setTopLeftPosition (0, juce::jlimit (0, juce::jmax (0, (owner.count() - 1) * kRowH), baseY + e.getDistanceFromDragStartY()));
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! dragging) return;
            dragging = false;
            const int target = juce::jlimit (0, owner.count() - 1, index + (int) std::lround ((double) e.getDistanceFromDragStartY() / kRowH));
            juce::Component::SafePointer<MasterListPanel> safe (&owner);
            const int from = index;
            juce::MessageManager::callAsync ([safe, from, target]
            {
                if (safe == nullptr) return;
                if (target != from && safe->onMove) safe->onMove (from, target); else safe->layoutRows();
            });
        }
        MasterListPanel& owner;
        MasterListEntry entry;
        int index = 0, detailWidth = 84, statusWidth = 110, baseY = 0;
        bool dragging = false;
        juce::ToggleButton box;
        juce::TextEditor nameEd;
        juce::Label nameLabel;
    };

    bool editable;
    juce::Viewport viewport;
    struct Holder : juce::Component { } holder;
    juce::OwnedArray<Row> rows;
};

// ============================================================================= the timeline of the disc
struct MasterBlock { int clipIndex = 0; juce::Uuid id; juce::String title; int start = 0, end = 0, gap = 0, number = 1, index00 = -1, index01 = 0; };       // start = where the audio begins; index01 = the PQ start flag

/** The whole disc as one picture, as wide as the window: the ruler, the PQ flags, the clips with the pauses between them and the playhead.
    It draws only what is in view, so it can be zoomed right in on a single pause and still show a 79 minute disc. */
class MasterTimeline : public juce::Component, private juce::ScrollBar::Listener
{
public:
    std::function<void (const juce::Uuid&)> onSelect;
    std::function<void (int clipIndex, int gap)> onGap;
    std::function<void (int clipIndexA, int clipIndexB)> onSwap;
    /** A PQ flag was dragged: kind 1 = INDEX 01 of the clip, 0 = its INDEX 00, 2 = End of CD; 'sector' is where the flag was dropped (absolute). */
    std::function<void (int kind, int clipIndex, int sector)> onFlag;
    std::function<void (int kind, int clipIndex)> onFlagReset;          // double-click: back to the automatic place
    std::function<void (const juce::Uuid&, int sector)> onFlagClicked;  // a click on a flag: select that track and put the playhead on the flag
    std::function<void (double seconds)> onPlayhead;          // the user put the playhead somewhere (seconds from the start of the disc)

    MasterTimeline()
    {
        bar.setAutoHide (false); bar.addListener (this); addAndMakeVisible (bar);
        setWantsKeyboardFocus (true);
    }
    void setData (std::vector<MasterBlock> b, int leadOutSector, const juce::Uuid& sel)
    {
        blocks = std::move (b); leadOut = leadOutSector; selected = sel;
        clampView(); updateBar(); repaint();
    }
    const std::map<juce::String, MasterEnvelope>* envelopes = nullptr;      // owned by the Mastering window
    double playheadSeconds() const { return playhead; }
    /** follow: scroll the picture so the playhead stays in view (while playing). */
    void setPlayhead (double seconds, bool follow = false)
    {
        playhead = juce::jmax (0.0, seconds);
        if (follow)
        {
            const double vis = visibleSeconds();
            if (playhead < viewStart || playhead > viewStart + vis * 0.92) { viewStart = juce::jmax (0.0, playhead - vis * 0.08); clampView(); updateBar(); }
        }
        repaint();
    }
    /** Zooms in (factor > 1) or out (factor < 1), keeping the point 'focusSeconds' where it is on the screen. */
    void zoomAround (double factor, double focusSeconds)
    {
        const double screenX = xOfSec (focusSeconds);
        pxPerSecond = juce::jlimit (0.2, 1500.0, pxPerSecond * factor);
        viewStart = focusSeconds - (screenX - kLeft) / pxPerSecond;
        clampView(); updateBar(); repaint();
    }
    /** Zooms around a moment and puts it in the MIDDLE of the picture (as far as the start allows, so near the start it stays toward the left). */
    void zoomCentred (double factor, double focusSeconds)
    {
        pxPerSecond = juce::jlimit (0.2, 1500.0, pxPerSecond * factor);
        viewStart = juce::jmax (0.0, focusSeconds - visibleSeconds() * 0.5);
        clampView(); updateBar(); repaint();
    }
    void fit()
    {
        pxPerSecond = juce::jlimit (0.2, 1500.0, (double) juce::jmax (100, getWidth() - kLeft - 14) / juce::jmax (10.0, totalSeconds()));
        viewStart = 0.0; updateBar(); repaint();
    }
    /** Scrolls so that the given moment is in the middle of the picture (the zoom stays the same). */
    void centreOn (double seconds)
    {
        viewStart = juce::jmax (0.0, seconds - visibleSeconds() * 0.5);
        clampView(); updateBar(); repaint();
    }
    void showSeconds (double seconds)                                    // makes sure a moment is on screen
    {
        if (seconds < viewStart || seconds > viewStart + visibleSeconds()) { viewStart = juce::jmax (0.0, seconds - visibleSeconds() * 0.3); clampView(); updateBar(); repaint(); }
    }

    void resized() override
    {
        bar.setBounds (0, getHeight() - kBarH, getWidth(), kBarH);
        clampView(); updateBar();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wave);
        const int rulerH = kRulerH, W = getWidth();
        g.setColour (theme::ruler); g.fillRect (0, 0, W, rulerH);
        const int flagY = 2, flagY2 = 18, top = rulerH + 42, h = getHeight() - kBarH - top - 6;
        // ruler: the step is the smallest one that leaves at least 80 pixels between two marks
        static const double steps[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200 };
        double step = 1200; for (double st : steps) if (st * pxPerSecond >= 80.0) { step = st; break; }
        g.setFont (juce::FontOptions (11.0f));
        for (double t = std::floor (viewStart / step) * step; xOfSec (t) < W; t += step)
        {
            if (t < 0) continue;
            const float x = xOfSec (t);
            g.setColour (theme::grid); g.drawVerticalLine ((int) x, (float) rulerH - 6.0f, (float) (top + h));
            const int whole = (int) t; const double frac = t - whole;
            g.setColour (theme::dimText);
            g.drawText (juce::String (whole / 60) + ":" + juce::String (whole % 60).paddedLeft ('0', 2) + (step < 1.0 ? "." + juce::String ((int) std::lround (frac * 10.0)) : juce::String()), (int) x + 3, 2, 70, rulerH - 4, juce::Justification::centredLeft);
        }
        int pos = 0;
        for (auto& b : blocks)
        {
            if (b.start > pos)                                                                  // the pause before the track
            {
                auto gr = juce::Rectangle<float> (xOfSector (pos), (float) top, xOfSector (b.start) - xOfSector (pos), (float) h);
                g.setColour (theme::window.withAlpha (0.8f)); g.fillRect (gr);
                g.setColour (theme::dimText.withAlpha (0.5f)); g.drawRect (gr, 1.0f);
                if (gr.getWidth() > 44) { g.setFont (juce::FontOptions (11.0f)); g.setColour (theme::dimText); g.drawText (b.number == 1 ? "lead-in " + secondsText (b.gap) : secondsText (b.gap), gr, juce::Justification::centred); }
            }
            auto r = juce::Rectangle<float> (xOfSector (b.start), (float) top, xOfSector (b.end) - xOfSector (b.start), (float) h);
            if (r.getRight() >= 0 && r.getX() <= (float) W)
            {
                const auto& pal = chan::palette();
                juce::Colour c (pal.empty() ? 0xff3b82c4u : pal[(size_t) (b.number - 1) % pal.size()]);
                const bool sel = selected == b.id;
                g.setColour (chan::clipFill (c, sel)); g.fillRect (r);
                g.setColour (sel ? juce::Colours::white : c.brighter (0.3f)); g.drawRect (r, sel ? 2.0f : 1.0f);
                auto inside = r.withLeft (juce::jmax (r.getX(), 0.0f)).withRight (juce::jmin (r.getRight(), (float) W));       // the text stays visible while the clip runs off the screen
                g.setColour (chan::textOn (chan::clipFill (c, sel)));
                if (envelopes != nullptr && h > 70)                                                  // the audio of the edit, as an outline in the box
                {
                    auto it = envelopes->find (b.id.toString());
                    if (it != envelopes->end() && ! it->second.bins.empty())
                    {
                        const auto& bins = it->second.bins; const int n = (int) bins.size();
                        const float w = r.getWidth() * (float) it->second.fraction;
                        if (w > 2.0f)
                        {
                            const float midY = r.getY() + 40.0f + (r.getHeight() - 44.0f) * 0.5f, half = (r.getHeight() - 44.0f) * 0.5f;
                            // one smooth filled outline (not a line per pixel): where the picture is zoomed in so far that a slice covers many pixels, the levels are joined by straight
                            // lines between the middles of the slices; where a pixel covers several slices, its loudest one is used
                            const int x0 = juce::jmax (0, (int) r.getX() - 1), x1 = juce::jmin (W, (int) r.getRight() + 1);
                            auto levelAt = [&] (float xpix) -> float
                            {
                                const float u0 = (xpix - r.getX()) / w * (float) n, u1 = (xpix + 1.0f - r.getX()) / w * (float) n;
                                if (u1 - u0 >= 1.0f)                                          // many slices under one pixel: the loudest
                                {
                                    const int i0 = juce::jlimit (0, n - 1, (int) u0), i1 = juce::jlimit (i0, n - 1, (int) u1);
                                    float m = 0.0f; for (int i = i0; i <= i1; ++i) m = juce::jmax (m, bins[(size_t) i]);
                                    return m;
                                }
                                const float mid = 0.5f * (u0 + u1) - 0.5f;                     // one slice under many pixels: a straight line from one slice's middle to the next
                                const int i0 = juce::jlimit (0, n - 1, (int) std::floor (mid)), i1 = juce::jlimit (0, n - 1, i0 + 1);
                                const float fr = juce::jlimit (0.0f, 1.0f, mid - (float) i0);
                                return bins[(size_t) i0] * (1.0f - fr) + bins[(size_t) i1] * fr;
                            };
                            const float xEnd = r.getX() + w;
                            std::vector<float> hs; int xs = x0;
                            for (int x = x0; x < x1 && (float) x < xEnd; ++x) hs.push_back (juce::jmax (0.6f, juce::jmin (1.0f, levelAt ((float) x)) * half));
                            if (hs.size() > 1)
                            {
                                juce::Path poly;
                                poly.startNewSubPath ((float) xs, midY - hs[0]);
                                for (size_t i = 1; i < hs.size(); ++i) poly.lineTo ((float) xs + (float) i, midY - hs[i]);
                                for (size_t i = hs.size(); i-- > 0;) poly.lineTo ((float) xs + (float) i, midY + hs[i]);
                                poly.closeSubPath();
                                g.setColour (chan::waveColour (c).withAlpha (0.9f)); g.fillPath (poly);
                            }
                        }
                    }
                }
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (juce::String (b.number) + "  " + b.title, inside.reduced (6, 4).withHeight (20), juce::Justification::centredLeft, true);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (juce::String ((double) (b.end - b.start) / 75.0, 1) + " s   starts " + sectorsToMsf (b.start), inside.reduced (6, 4).withTrimmedTop (20).withHeight (16), juce::Justification::centredLeft, true);
            }
            if (b.start < pos)                                                                  // an overlap with the track before: shaded
            {
                auto ov = juce::Rectangle<float> (xOfSector (b.start), (float) top, xOfSector (pos) - xOfSector (b.start), (float) h);
                g.setColour (juce::Colour (0x55ff9d2e)); g.fillRect (ov);
                if (ov.getWidth() > 44) { g.setFont (juce::FontOptions (11.0f, juce::Font::bold)); g.setColour (juce::Colours::white); g.drawText ("overlap " + secondsText (pos - b.start), ov, juce::Justification::centred); }
            }
            pos = juce::jmax (pos, b.end);
        }
        // the PQ flags: INDEX 01 (the start of each track), INDEX 00 (the start of its pause) and the lead-out
        // rows: the upper one has the starts (INDEX 01, INDEX 00) and the lead-out; the lower one has the CD start and the end of every track (flag pointing back, over the track it ends)
        flagHits.clear();
        auto flag = [&] (int sector, const juce::String& text, juce::Colour c, bool hollow, int yRow, bool pointsBack, int kind, int clipIndex)
        {
            const float xf = xOfSector (sector);
            if (xf < -400.0f || xf > (float) W + 400.0f) return;
            const int x = (int) xf;
            const int tw = juce::jmax (18, (int) juce::GlyphArrangement::getStringWidth (juce::FontOptions (11.0f, juce::Font::bold), text) + 10);
            g.setColour (c); g.drawVerticalLine (x, (float) (rulerH + yRow), (float) (top + h));
            if (kind >= 0) flagHits.push_back ({ juce::Rectangle<int> (pointsBack ? x - tw : x - 2, rulerH + yRow, tw + 2, 14), kind, clipIndex, sector });
            juce::Path p;                                                                         // a pennant: a rectangle with a notched end
            const float fy = (float) (rulerH + yRow), fh = 14.0f;
            if (! pointsBack)
            {
                p.addRectangle ((float) x, fy, (float) tw - 5.0f, fh);
                p.addTriangle ((float) (x + tw - 5), fy, (float) (x + tw), fy + fh * 0.5f, (float) (x + tw - 5), fy + fh);
            }
            else
            {
                p.addRectangle ((float) (x - tw + 5), fy, (float) tw - 5.0f, fh);
                p.addTriangle ((float) (x - tw + 5), fy, (float) (x - tw), fy + fh * 0.5f, (float) (x - tw + 5), fy + fh);
            }
            if (hollow) { g.setColour (theme::wave); g.fillPath (p); g.setColour (c); g.strokePath (p, juce::PathStrokeType (1.2f)); }
            else { g.setColour (c); g.fillPath (p); g.setColour (juce::Colours::white); }
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (text, pointsBack ? x - tw + 3 : x + 3, (int) fy, tw - 6, (int) fh, juce::Justification::centredLeft, false);
        };
        if (! blocks.empty()) flag (0, "CD start  00:00:00", juce::Colour (0xff8a8f98), false, flagY2, false, -1, 0);
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const auto& b = blocks[i];
            const double room = ((i + 1 < blocks.size() ? (double) blocks[i + 1].index01 : (double) leadOut) - b.index01) * pxPerSector();
            flag (b.index01, room > 150.0 ? "INDEX 01  T" + juce::String (b.number) + "  " + sectorsToMsf (b.index01) : room > 70.0 ? "T" + juce::String (b.number) + " " + sectorsToMsf (b.index01) : "T" + juce::String (b.number),
                  selected == b.id ? juce::Colour (0xff2fa5d8) : juce::Colour (0xff1f8f4e), false, flagY, false, 1, b.clipIndex);
            // the end of a track IS the INDEX 00 of the next one (SADiE: "the end of Track 1 is coded as Track 2, Index 0"); the last track ends at the lead-out
            const bool lastTrack = i + 1 == blocks.size();
            const int endAt = lastTrack ? leadOut : (blocks[i + 1].index00 >= 0 ? blocks[i + 1].index00 : blocks[i + 1].index01);
            const double len = (double) (endAt - b.index01) * pxPerSector();
            flag (endAt, len > 150.0 ? "End T" + juce::String (b.number) + "  " + sectorsToMsf (endAt) : len > 60.0 ? "End T" + juce::String (b.number) : "E" + juce::String (b.number),
                  juce::Colour (0xff7e57c2), false, flagY2, true, lastTrack ? 2 : 0, lastTrack ? 0 : blocks[i + 1].clipIndex);
        }
        if (! blocks.empty()) flag (leadOut, "End of CD  " + sectorsToMsf (leadOut), theme::playhead, false, flagY, true, 2, 0);
        // the playhead
        const float px = xOfSec (playhead);
        if (px >= -8.0f && px <= (float) W + 8.0f)
        {
            g.setColour (juce::Colours::white); g.drawVerticalLine ((int) px, 0.0f, (float) (getHeight() - kBarH));
            juce::Path tri; tri.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 10.0f);
            g.setColour (juce::Colour (0xfff2f2f2)); g.fillPath (tri);
            g.setColour (juce::Colours::black.withAlpha (0.55f)); g.strokePath (tri, juce::PathStrokeType (1.0f));
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        dragIndex = -1; scrubbing = false; flagDrag = -1;
        if (e.y < kRulerH) { scrubbing = true; placePlayhead (e.x); return; }
        for (auto it = flagHits.rbegin(); it != flagHits.rend(); ++it)                         // a PQ flag: click to select / put the playhead on it, drag to move it, double-click to put it back
            if (it->r.expanded (2).contains (e.getPosition()))
            {
                flagDrag = it->kind; flagClip = it->clipIndex;
                if (e.getNumberOfClicks() > 1) { if (onFlagReset) onFlagReset (it->kind, it->clipIndex); flagDrag = -1; return; }
                for (auto& b : blocks) if (b.clipIndex == it->clipIndex && it->kind != 2 && onFlagClicked) onFlagClicked (b.id, it->sector);
                if (it->kind == 2 && onFlagClicked) onFlagClicked (juce::Uuid::null(), it->sector);
                setPlayhead ((double) it->sector / 75.0); if (onPlayhead) onPlayhead (playhead);
                return;
            }
        for (size_t i = 0; i < blocks.size(); ++i)
            if (e.x >= (int) xOfSector (blocks[i].start) && e.x <= (int) xOfSector (blocks[i].end)) dragIndex = (int) i;        // (where tracks overlap, the later one is on top)
        placePlayhead (e.x);                                                                     // a click anywhere puts the playhead there
        if (dragIndex < 0) return;
        dragId = blocks[(size_t) dragIndex].id; dragStartX = e.x; dragStartGap = blocks[(size_t) dragIndex].gap;
        if (onSelect) onSelect (dragId);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (scrubbing) { placePlayhead (e.x); return; }
        if (flagDrag >= 0)
        {
            const int sector = juce::jmax (0, (int) std::lround (secOfX ((float) e.x) * 75.0));
            if (onFlag) onFlag (flagDrag, flagClip, sector);
            return;
        }
        if (dragIndex < 0 || dragIndex >= (int) blocks.size()) return;
        const auto& b = blocks[(size_t) dragIndex];
        if (b.id != dragId) return;
        // a track can be dragged back over the one before it (they overlap and are mixed together on the disc) but never past that track's start, and never by more than 20 s
        int minGap = 150;
        if (b.number > 1)
        {
            const auto& prev = blocks[(size_t) dragIndex - 1];
            minGap = juce::jmax (-kMaxOverlapSectors, -(prev.end - prev.start) + 1);
        }
        const int raw = dragStartGap + (int) std::lround ((double) (e.x - dragStartX) / pxPerSector());
        const int gap = juce::jmax (minGap, raw);
        if (gap != b.gap && onGap) { onGap (b.clipIndex, gap); rebase (e.x); }          // the picture moved under the mouse: measure from here again
    }
    void mouseUp (const juce::MouseEvent&) override { dragIndex = -1; scrubbing = false; flagDrag = -1; }
    void mouseMove (const juce::MouseEvent& e) override
    {
        bool over = false;
        for (auto& f : flagHits) if (f.r.expanded (2).contains (e.getPosition())) over = true;
        setMouseCursor (over ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown() || e.mods.isCommandDown()) { zoomAround (w.deltaY > 0 ? 1.25 : 0.8, playhead); if (onViewMoved) onViewMoved(); return; }
        viewStart -= (w.deltaY != 0.0f ? w.deltaY : w.deltaX) * visibleSeconds() * 0.4;
        clampView(); updateBar(); repaint();
    }
    std::function<void()> onViewMoved;
private:
    static constexpr int kLeft = 12, kRulerH = 24, kBarH = 14;
    void scrollBarMoved (juce::ScrollBar*, double newStart) override { viewStart = newStart; repaint(); }
    double totalSeconds() const { return (double) (leadOut + 150) / 75.0; }
    double visibleSeconds() const { return (double) juce::jmax (50, getWidth() - kLeft - 12) / pxPerSecond; }
    void clampView() { viewStart = juce::jlimit (0.0, juce::jmax (0.0, totalSeconds() - visibleSeconds() * 0.25), viewStart); }
    void updateBar()
    {
        bar.setRangeLimits (0.0, juce::jmax (totalSeconds(), viewStart + visibleSeconds()));
        bar.setCurrentRange (viewStart, visibleSeconds(), juce::dontSendNotification);
    }
    void placePlayhead (int x)
    {
        playhead = juce::jmax (0.0, secOfX ((float) x)); repaint();
        if (onPlayhead) onPlayhead (playhead);
    }
    void rebase (int x)                                                                         // after the layout changed under the mouse
    {
        dragIndex = -1;
        for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i].id == dragId) { dragIndex = (int) i; dragStartGap = blocks[i].gap; if (x >= 0) dragStartX = x; }
    }
    double pxPerSector() const { return pxPerSecond / 75.0; }
    float xOfSec (double sec) const { return (float) kLeft + (float) ((sec - viewStart) * pxPerSecond); }
    float xOfSector (int sector) const { return xOfSec ((double) sector / 75.0); }
    double secOfX (float x) const { return viewStart + ((double) x - kLeft) / pxPerSecond; }
    std::vector<MasterBlock> blocks;
    int leadOut = 150, dragIndex = -1, dragStartX = 0, dragStartGap = 0;
    struct FlagHit { juce::Rectangle<int> r; int kind, clipIndex, sector; };
    std::vector<FlagHit> flagHits;                      // where the flags were drawn the last time (for clicking and dragging them)
    int flagDrag = -1, flagClip = 0;
    bool scrubbing = false;
    juce::Uuid selected, dragId;
    double pxPerSecond = 6.0, viewStart = 0.0, playhead = 0.0;
    juce::ScrollBar bar { false };
};

// ============================================================================= the PQ list
struct MasterPqRow { juce::Uuid id; juce::String number, title, index00, index01, length, pause, isrc, flags; };

class MasterPqTable : public juce::Component
{
public:
    std::function<void (const juce::Uuid&)> onSelect;
    void setRows (std::vector<MasterPqRow> r, const juce::String& leadOutText, const juce::Uuid& sel)
    {
        rows = std::move (r); leadOut = leadOutText; selected = sel;
        setSize (getWidth(), juce::jmax (60, 24 + ((int) rows.size() + 1) * 20 + 4));
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wave);
        const int w = getWidth();
        const int cols[] = { 40, 0, 86, 86, 78, 104, 128, 70 };                                   // title takes what is left
        int fixed = 0; for (int c : cols) fixed += c;
        const int titleW = juce::jmax (120, w - fixed);
        auto drawRow = [&] (int y, const juce::StringArray& cells, bool header, bool sel, int idx)
        {
            g.setColour (header ? theme::ruler : sel ? theme::selected : (idx % 2 ? theme::rowAlt : theme::row)); g.fillRect (0, y, w, 20);
            g.setColour (header ? theme::text : theme::dimText.interpolatedWith (theme::text, 0.6f));
            g.setFont (juce::FontOptions (12.0f, header ? juce::Font::bold : juce::Font::plain));
            int x = 6;
            for (int c = 0; c < cells.size(); ++c)
            {
                const int cw = c == 1 ? titleW : cols[c];
                g.drawText (cells[c], x, y, cw - 6, 20, juce::Justification::centredLeft, true);
                x += cw;
            }
        };
        drawRow (0, { "No.", "Title", "INDEX 00", "INDEX 01", "Length", "Pause", "ISRC", "Flags" }, true, false, 0);
        int y = 20, i = 0;
        for (auto& r : rows) { drawRow (y, { r.number, r.title, r.index00, r.index01, r.length, r.pause, r.isrc, r.flags }, false, selected == r.id, i++); y += 20; }
        g.setColour (theme::text); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("Lead-out (end of disc) at " + leadOut, 6, y + 2, w - 12, 20, juce::Justification::centredLeft);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = (e.y - 20) / 20;
        if (juce::isPositiveAndBelow (i, (int) rows.size()) && onSelect) onSelect (rows[(size_t) i].id);
    }
private:
    std::vector<MasterPqRow> rows; juce::String leadOut; juce::Uuid selected;
};

// ============================================================================= the window
MasteringComponent::MasteringComponent (AppContext& a) : app (a), renderProgress (renderProgressValue), vmProgress (progressValue), ddpProgress (progressValue)
{
    auto cap = [this] (juce::Label& l, const juce::String& t, bool bold = false) { styleCaption (l, t, bold); addChildComponent (l); };
    for (auto* b : { &vmTab, &ddpTab }) { b->setClickingTogglesState (false); b->setColour (juce::TextButton::buttonOnColourId, theme::accent); addAndMakeVisible (b); }
    vmTab.onClick = [this] { setView (0); };
    ddpTab.onClick = [this] { setView (1); };
    vmTab.setTooltip ("Export the edits as audio files: WAV, AIFF, FLAC, Ogg Vorbis or MP3, with levels, names and tags");
    ddpTab.setTooltip ("Lay the edits out as a CD: pauses, automatic PQ points, CD-Text, and a DDP master for the factory");
    styleCaption (renderCaption, "Render from:", true); styleCaption (tailCaption, "Tail:", true);
    addAndMakeVisible (renderCaption); addAndMakeVisible (tailCaption);
    addAndMakeVisible (mixerBox); addAndMakeVisible (sourceBox); addAndMakeVisible (tailBox);
    mixerBox.setTooltip ("The mixer whose levels, effects and automation are used. Normally the processing mixer.");
    sourceBox.setTooltip ("The output that is rendered: normally the stereo Ext bus that goes to the monitors.");
    tailBox.setTooltip ("Silence added after the last audio, run through the mixer so reverb tails and effects can ring out.");
    for (int s = 0; s <= 10; ++s) tailBox.addItem (juce::String (s) + " s", s + 1);
    tailBox.onChange = [this] { if (updating) return; def().tailSeconds = tailBox.getSelectedId() - 1; touched(); refreshDdp(); scheduleRecheck(); };
    mixerBox.onChange = [this] { if (updating) return; const int i = mixerBox.getSelectedId() - 1; if (juce::isPositiveAndBelow (i, (int) mixerIds.size())) { def().mixerId = mixerIds[(size_t) i]; touched(); scheduleRecheck(); } };
    sourceBox.onChange = [this] { if (updating) return; const int i = sourceBox.getSelectedId() - 1; if (juce::isPositiveAndBelow (i, (int) sourceIds.size())) { def().sourceId = sourceIds[(size_t) i]; touched(); scheduleRecheck(); } };

    // the renders: one line under the tabs, always visible
    renderStatus.setFont (juce::FontOptions (13.0f)); addAndMakeVisible (renderStatus);
    addAndMakeVisible (renderProgress);
    prevRenderBtn.setTooltip ("Every piece keeps its two latest renders. This switches the selected piece to the one before the newest (Play and Export then use it), and back again. "
                              "Use it when a change to the edit or its mixer turned out worse.");
    renderAgainBtn.setTooltip ("Throws away the idea that the renders are up to date and renders every ticked piece again (into the older of its two slots).");
    prevRenderBtn.onClick = [this]
    {
        const auto id = selectedPiece();
        auto it = renders.find (id.toString());
        if (id.isNull() || it == renders.end()) return;
        MasterRenders store (MasterRenders::folderFor (app.project));
        store.setUsePrevious (id, ! it->second.previous);
        checkRenders();
    };
    renderAgainBtn.onClick = [this] { checkRenders (true); };
    addAndMakeVisible (prevRenderBtn); addAndMakeVisible (renderAgainBtn);
    prevRenderBtn.setEnabled (false);

    // ---------------- Virtual Master
    vmList = std::make_unique<MasterListPanel> (true);
    addChildComponent (*vmList);
    vmList->onSelect = [this] (const juce::Uuid& id) { def().selectedItem = id; vmList->setSelected (id); refreshFields(); updateRenderButtons(); };
    vmList->onInclude = [this] (const juce::Uuid& id, bool on) { if (auto* it = def().findItem (id)) { it->include = on; touched(); scheduleRecheck(); } };
    vmList->onRename = [this] (const juce::Uuid& id, const juce::String& t) { if (auto* it = def().findItem (id)) { it->fileName = t; touched(); } };
    vmList->onMove = [this] (int from, int to) { moveItem (from, to, false); };
    for (auto* b : { &vmAll, &vmNone, &vmUp, &vmDown }) addChildComponent (*b);
    vmAll.onClick  = [this] { for (auto& it : def().items) if (auto* e = editOf (it.editId)) it.include = ! e->isEmpty(); touched(); refreshVm(); scheduleRecheck(); };
    vmNone.onClick = [this] { for (auto& it : def().items) it.include = false; touched(); refreshVm(); };
    vmUp.onClick   = [this] { for (int i = 0; i < (int) def().items.size(); ++i) if (def().items[(size_t) i].editId == def().selectedItem && i > 0) { moveItem (i, i - 1, false); break; } };
    vmDown.onClick = [this] { for (int i = 0; i < (int) def().items.size(); ++i) if (def().items[(size_t) i].editId == def().selectedItem && i + 1 < (int) def().items.size()) { moveItem (i, i + 1, false); break; } };
    cap (fmtCaption, "File format", true); cap (rateCaption, "Sample rate"); cap (bitsCaption, "Bit depth"); cap (ditherCaption, "Dither"); cap (qualityCaption, "Quality");
    cap (levelCaption, "Peak level", true); cap (albumCaption, "Details for every file", true); cap (fileCaption, "Details of the selected file", true);
    cap (nameCaption, "Files", true); cap (folderCaption, "Folder");
    for (auto* c : { &formatBox, &rateBox, &bitsBox, &ditherBox, &qualityBox }) vmHolder.addChildComponent (*c);
    for (auto* l : { &fmtCaption, &rateCaption, &bitsCaption, &ditherCaption, &qualityCaption, &levelCaption, &albumCaption, &fileCaption, &nameCaption, &folderCaption }) vmHolder.addChildComponent (*l);
    formatBox.addItem ("WAV", 1); formatBox.addItem ("AIFF", 2); formatBox.addItem ("FLAC (lossless, smaller)", 3); formatBox.addItem ("Ogg Vorbis (lossy)", 4); formatBox.addItem ("MP3 (lossy, needs LAME)", 5);
    {
        const char* names[] = { "Same as the edit", "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "176400 Hz", "192000 Hz" };
        for (int i = 0; i < 7; ++i) rateBox.addItem (names[i], i + 1);
    }
    ditherBox.addItem ("None", 1); ditherBox.addItem ("TPDF (recommended)", 2);
    ditherBox.setTooltip ("Dither hides the rounding error when the bit depth is reduced. Use it whenever you go down to 16 or 24 bit.");
    formatBox.onChange = [this]
    {
        if (updating) return;
        def().vm.format = (MasterFormat) (formatBox.getSelectedId() - 1);
        if (def().vm.format == MasterFormat::Aiff || def().vm.format == MasterFormat::Flac) { if (def().vm.bitDepth == 32) def().vm.bitDepth = 24; }
        if (def().vm.format == MasterFormat::Mp3 && def().vm.sampleRate > 48000.0) def().vm.sampleRate = 44100.0;
        touched(); refreshFormatControls();
    };
    rateBox.onChange = [this]
    {
        if (updating) return;
        const double rates[] = { 0, 44100, 48000, 88200, 96000, 176400, 192000 };
        def().vm.sampleRate = rates[juce::jlimit (0, 6, rateBox.getSelectedId() - 1)]; touched();
    };
    bitsBox.onChange = [this] { if (updating) return; const int v[] = { 16, 24, 32 }; def().vm.bitDepth = v[juce::jlimit (0, 2, bitsBox.getSelectedId() - 1)]; touched(); refreshFormatControls(); };
    ditherBox.onChange = [this] { if (updating) return; def().vm.dither = ditherBox.getSelectedId() == 2 ? DitherMode::Tpdf : DitherMode::Off; touched(); };
    qualityBox.onChange = [this]
    {
        if (updating) return;
        auto& vm = def().vm; const int id = qualityBox.getSelectedId();
        if (vm.format == MasterFormat::Mp3) { if (id <= 6) { const int k[] = { 320, 256, 224, 192, 160, 128 }; vm.mp3Kbps = k[id - 1]; } else { vm.mp3Kbps = 0; vm.mp3Vbr = id - 7; } }
        else if (vm.format == MasterFormat::Flac) vm.flacLevel = id - 1;
        else vm.oggQuality = id - 1;
        touched();
    };
    for (auto* t : { &normToggle, &overallToggle, &individualToggle, &numberToggle }) vmHolder.addChildComponent (*t);
    cap (alsoCaption, "Also export every file as (all in one go)", true); vmHolder.addChildComponent (alsoCaption);
    {
        const char* an[] = { "WAV", "AIFF", "FLAC", "Ogg Vorbis", "MP3" };
        for (int f = 0; f < 5; ++f)
        {
            auto& t = alsoToggle[f]; t.setButtonText (an[f]); vmHolder.addChildComponent (t);
            t.setTooltip ("Tick to make this format as well in the same export, using the settings above (bit depth, quality) and the same names");
            t.onClick = [this, f] { if (updating) return; auto& m = def().vm.extraFormats; if (alsoToggle[f].getToggleState()) m |= (1 << f); else m &= ~(1 << f); touched(); };
        }
    }
    vmHolder.addChildComponent (peakEditor); vmHolder.addChildComponent (peakUnit); vmHolder.addChildComponent (folderEditor); vmHolder.addChildComponent (chooseButton);
    overallToggle.setRadioGroupId (4711); individualToggle.setRadioGroupId (4711);
    styleCaption (peakUnit, "dBFS");
    peakEditor.setInputRestrictions (6, "-0123456789.");
    normToggle.onClick = [this] { def().vm.normalise = normToggle.getToggleState(); touched(); refreshFormatControls(); };
    overallToggle.onClick = [this] { def().vm.peakTogether = true; touched(); };
    individualToggle.onClick = [this] { def().vm.peakTogether = false; touched(); };
    numberToggle.onClick = [this] { def().vm.numberFiles = numberToggle.getToggleState(); touched(); };
    peakEditor.onTextChange = [this] { def().vm.peakDb = juce::jlimit (-60.0f, 0.0f, peakEditor.getText().getFloatValue()); touched(); };
    folderEditor.onTextChange = [this] { def().vm.folder = folderEditor.getText(); touched(); };
    chooseButton.onClick = [this] { chooseFolder (false); };
    auto album = [this] (const char* c, std::function<juce::String*()> r) { addField (vmHolder, albumFields, c, std::move (r)); };
    album ("Album",         [this] { return &def().vm.album.album; });
    album ("Album artist",  [this] { return &def().vm.album.albumArtist; });
    album ("Artist",        [this] { return &def().vm.album.artist; });
    album ("Composer",      [this] { return &def().vm.album.composer; });
    album ("Genre",         [this] { return &def().vm.album.genre; });
    album ("Year",          [this] { return &def().vm.album.year; });
    album ("Copyright",     [this] { return &def().vm.album.copyright; });
    album ("Comment",       [this] { return &def().vm.album.comment; });
    auto sel = [this] (const char* c, std::function<juce::String*()> r) { addField (vmHolder, fileFields, c, std::move (r)); };
    auto selItem = [this] { return def().findItem (def().selectedItem); };
    sel ("File name", [this, selItem] { auto* i = selItem(); return i ? &i->fileName : nullptr; });
    sel ("Title",    [this, selItem] { auto* i = selItem(); return i ? &i->tags.title : nullptr; });
    sel ("Artist",   [this, selItem] { auto* i = selItem(); return i ? &i->tags.artist : nullptr; });
    sel ("Composer", [this, selItem] { auto* i = selItem(); return i ? &i->tags.composer : nullptr; });
    sel ("ISRC",     [this, selItem] { auto* i = selItem(); return i ? &i->tags.isrc : nullptr; });
    sel ("Comment",  [this, selItem] { auto* i = selItem(); return i ? &i->tags.comment : nullptr; });
    vmSettings.setViewedComponent (&vmHolder, false); vmSettings.setScrollBarsShown (true, false);
    addChildComponent (vmSettings);
    addChildComponent (vmProgress); addChildComponent (vmStatus); addChildComponent (exportButton); addChildComponent (showButton);
    vmStatus.setFont (juce::FontOptions (13.0f));
    exportButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    exportButton.onClick = [this] { startVmExport(); };
    showButton.onClick = [this] { if (lastOutput.exists()) lastOutput.revealToUser(); };
    showButton.setVisible (false);

    // ---------------- DDP builder
    ddpList = std::make_unique<MasterListPanel> (false);
    addChildComponent (*ddpList);
    ddpList->onSelect = [this] (const juce::Uuid& id) { def().selectedClip = id; refreshDdp(); updateRenderButtons(); };
    ddpList->onInclude = [this] (const juce::Uuid& id, bool on) { if (auto* c = def().findClip (id)) { c->include = on; touched(); refreshDdp (false); scheduleRecheck(); } };
    ddpList->onMove = [this] (int from, int to) { moveItem (from, to, true); };
    timeline = std::make_unique<MasterTimeline>();
    addChildComponent (*timeline);
    timeline->onPlayhead = [this] (double sec) { setPlayhead (sec, true); };
    for (auto* c : std::initializer_list<juce::Component*> { &playBtn, &gapPlayBtn, &startBtn, &timeLabel }) addChildComponent (*c);
    timeLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold)); timeLabel.setColour (juce::Label::textColourId, theme::text);
    playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
    playBtn.onClick = [this] { togglePlay(); };
    gapPlayBtn.onClick = [this] { playGap(); };
    startBtn.onClick = [this] { for (auto& t : computePq (def().ddp, clipFrames()).tracks) if (t.editId == def().selectedClip) { setPlayhead ((double) t.index01 / 75.0, false); timeline->showSeconds ((double) t.index01 / 75.0 - 2.0); } };
    playBtn.setTooltip ("Plays the disc from the playhead, with the pauses between the tracks, through the processing mixer (Space does the same). Click the ruler or the tracks to put the playhead somewhere.");
    gapPlayBtn.setTooltip ("Plays the selected track's pause: 3 seconds before it, the silence, and 3 seconds of the track itself.");
    startBtn.setTooltip ("Moves the playhead to the start (INDEX 01) of the selected track.");
    timeline->onSelect = [this] (const juce::Uuid& id) { def().selectedClip = id; ddpList->setSelected (id); refreshDdp(); };
    timeline->onGap = [this] (int clipIndex, int gap) { if (juce::isPositiveAndBelow (clipIndex, (int) def().ddp.clips.size())) { def().ddp.clips[(size_t) clipIndex].gapSectors = gap; touched(); refreshDdp (false); } };
    timeline->onSwap = [this] (int a, int b) { swapClips (a, b); };
    timeline->onFlag = [this] (int kind, int clipIndex, int sector) { moveFlag (kind, clipIndex, sector); };
    timeline->onFlagReset = [this] (int kind, int clipIndex) { resetFlag (kind, clipIndex); };
    timeline->onFlagClicked = [this] (const juce::Uuid& id, int) { if (! id.isNull() && def().selectedClip != id) { def().selectedClip = id; touched(); refreshDdp (true); } };
    pqTable = std::make_unique<MasterPqTable>();
    pqView.setViewedComponent (pqTable.get(), false); pqView.setScrollBarsShown (true, false);
    addChildComponent (pqView);
    pqTable->onSelect = [this] (const juce::Uuid& id) { def().selectedClip = id; ddpList->setSelected (id); refreshDdp(); };
    cap (discCaption, "Disc (CD-Text and catalogue number)", true); cap (clipCaption, "Selected track (CD-Text)", true); cap (pqCaption, "PQ list (made automatically from the positions)", true);
    cap (gapCaption, "Pause before this track (s)"); cap (allGapsCaption, "Set every pause to"); cap (ddpFolderCaption, "Folder"); styleCaption (ddpPeakUnit, "dBFS"); addChildComponent (ddpPeakUnit);
    warnings.setFont (juce::FontOptions (12.5f)); warnings.setColour (juce::Label::textColourId, theme::warn); warnings.setJustificationType (juce::Justification::topLeft); addChildComponent (warnings);
    ddpStatus.setFont (juce::FontOptions (13.0f)); addChildComponent (ddpStatus);
    for (auto* c : std::initializer_list<juce::Component*> { &gapEditor, &allGapsBox, &autoPq, &isrcBtn, &zoomIn, &zoomOut, &zoomFit, &ddpUp, &ddpDown, &ddpChoose, &ddpExport, &ddpShow, &ddpCheck, &preToggle, &copyToggle,
                                                              &zipToggle, &cueToggle, &ddpNorm, &ddpTogether, &ddpFolderEditor, &ddpPeakEditor, &ddpProgress }) addChildComponent (*c);
    gapEditor.setInputRestrictions (8, "0123456789.-");
    gapEditor.onReturnKey = [this] { applyGapToSelected (gapEditor.getText()); };
    gapEditor.onFocusLost = [this] { applyGapToSelected (gapEditor.getText()); };
    gapEditor.setTooltip ("Silence before the selected track, in seconds (the first track always starts at 2 s or later). A minus number (or dragging the track back over the one before it) overlaps the two: they are mixed together on the disc. You can also drag a track left and right in the timeline.");
    { const double v[] = { 0, 1, 1.5, 2, 3, 4, 5 }; allGapsBox.addItem ("Keep positions", 1); for (int i = 0; i < 7; ++i) allGapsBox.addItem ("Set every pause to " + juce::String (v[i], v[i] == 1.5 ? 1 : 0) + " s", i + 2); allGapsBox.setSelectedId (1, juce::dontSendNotification); }
    allGapsBox.setTooltip ("Keep positions: Auto PQ makes the PQ points from where the tracks are now and moves nothing. The other choices first set the pause before every track to that length.");
    autoPq.setTooltip ("Makes the PQ points (track starts, pauses, lead-out) from the tracks as they are placed, and puts any PQ flags you dragged back to the audio, every time you press it. With 'Keep positions' no track moves. "
                       "The PQ points always follow the positions: if an edit gets longer or shorter later, the tracks after it move and every pause stays as it is.");
    isrcBtn.onClick = [this] { openIsrcTool(); };
    isrcBtn.setTooltip ("Fills in the ISRC of every track in one go: type the country, label (registrant) and year codes and the 5-digit number of the FIRST track, and the other tracks get the numbers that follow (+1 each).");
    autoPq.onClick = [this] { const double v[] = { 0, 1, 1.5, 2, 3, 4, 5 }; const int id = allGapsBox.getSelectedId(); if (id <= 1) autoPqInPlace(); else setAllGaps (v[juce::jlimit (0, 6, id - 2)]); };
    zoomIn.onClick = [this] { zoomKey (true); };
    zoomOut.onClick = [this] { zoomKey (false); };
    zoomIn.setTooltip ("Zoom in on the playhead (Right arrow key)"); zoomOut.setTooltip ("Zoom out from the playhead (Left arrow key)");
    zoomFit.onClick = [this] { timeline->fit(); };
    ddpUp.onClick = [this] { for (int i = 0; i < (int) def().ddp.clips.size(); ++i) if (def().ddp.clips[(size_t) i].editId == def().selectedClip && i > 0) { moveItem (i, i - 1, true); break; } };
    ddpDown.onClick = [this] { for (int i = 0; i < (int) def().ddp.clips.size(); ++i) if (def().ddp.clips[(size_t) i].editId == def().selectedClip && i + 1 < (int) def().ddp.clips.size()) { moveItem (i, i + 1, true); break; } };
    preToggle.onClick = [this] { if (auto* c = def().findClip (def().selectedClip)) { c->preEmphasis = preToggle.getToggleState(); touched(); refreshDdp (false); } };
    copyToggle.onClick = [this] { if (auto* c = def().findClip (def().selectedClip)) { c->copyPermitted = copyToggle.getToggleState(); touched(); refreshDdp (false); } };
    zipToggle.onClick = [this] { def().ddp.makeZip = zipToggle.getToggleState(); touched(); };
    cueToggle.onClick = [this] { def().ddp.makeCue = cueToggle.getToggleState(); touched(); };
    ddpNorm.onClick = [this] { def().ddp.normalise = ddpNorm.getToggleState(); touched(); };
    ddpTogether.onClick = [this] { def().ddp.peakTogether = ddpTogether.getToggleState(); touched(); };
    ddpTogether.setTooltip ("On: one gain for every track (the loudest reaches the level), so the balance between the tracks is kept. Off: each track reaches the level on its own.");
    ddpPeakEditor.setInputRestrictions (6, "-0123456789.");
    ddpPeakEditor.onTextChange = [this] { def().ddp.peakDb = juce::jlimit (-60.0f, 0.0f, ddpPeakEditor.getText().getFloatValue()); touched(); };
    ddpFolderEditor.onTextChange = [this] { def().ddp.folder = ddpFolderEditor.getText(); touched(); };
    ddpChoose.onClick = [this] { chooseFolder (true); };
    ddpExport.setColour (juce::TextButton::buttonColourId, theme::accent);
    ddpExport.onClick = [this] { startDdpExport(); };
    ddpShow.onClick = [this] { if (lastOutput.exists()) lastOutput.revealToUser(); };
    ddpShow.setVisible (false);
    ddpCheck.setTooltip ("Reads the DDP that was made last and checks its sizes, track order, CD-Text and checksums.");
    ddpCheck.onClick = [this]
    {
        const auto name = sanitiseForFile (def().ddp.title.trim().isNotEmpty() ? def().ddp.title.trim() : juce::String ("CD master"));
        const auto folder = juce::File (def().ddp.folder).getChildFile (name + " DDP");
        juce::String rep; const bool ok = ddp::verifyFileset (folder, rep);
        ddpStatus.setColour (juce::Label::textColourId, ok ? theme::text : theme::warn);
        ddpStatus.setText (folder.exists() ? rep : "There is no DDP folder yet at " + folder.getFullPathName(), juce::dontSendNotification);
    };
    auto disc = [this] (const char* c, std::function<juce::String*()> r) { addField (*this, discFields, c, std::move (r)); };
    disc ("Album title", [this] { return &def().ddp.title; });
    disc ("Performer",   [this] { return &def().ddp.performer; });
    disc ("UPC / EAN",   [this] { return &def().ddp.upc; });
    disc ("Songwriter",  [this] { return &def().ddp.songwriter; });
    disc ("Composer",    [this] { return &def().ddp.composer; });
    disc ("Arranger",    [this] { return &def().ddp.arranger; });
    for (auto* f : discFields) { f->editor.onTextChange = [this, f] { if (auto* p = f->ref()) { *p = f->editor.getText(); touched(); refreshDdp (false); } }; }
    auto clip = [this] (const char* c, std::function<juce::String*()> r) { addField (rightHolder, clipFields, c, std::move (r)); };
    auto selClip = [this] { return def().findClip (def().selectedClip); };
    clip ("Title",      [selClip] { auto* i = selClip(); return i ? &i->title : nullptr; });
    clip ("Performer",  [selClip] { auto* i = selClip(); return i ? &i->performer : nullptr; });
    clip ("Songwriter", [selClip] { auto* i = selClip(); return i ? &i->songwriter : nullptr; });
    clip ("Composer",   [selClip] { auto* i = selClip(); return i ? &i->composer : nullptr; });
    clip ("Arranger",   [selClip] { auto* i = selClip(); return i ? &i->arranger : nullptr; });
    clip ("ISRC",       [selClip] { auto* i = selClip(); return i ? &i->isrc : nullptr; });
    addChildComponent (rightView); rightView.setViewedComponent (&rightHolder, false); rightView.setScrollBarsShown (true, false);
    for (auto* c : std::initializer_list<juce::Component*> { &clipCaption, &gapCaption, &gapEditor, &preToggle, &copyToggle }) rightHolder.addChildComponent (*c);
    for (auto* f : clipFields) { f->editor.onTextChange = [this, f] { if (auto* p = f->ref()) { *p = f->editor.getText(); touched(); refreshDdp (false); } }; }
    for (auto* f : discFields) { f->caption.setVisible (false); f->editor.setVisible (false); }
    for (auto* f : clipFields) { f->caption.setVisible (false); f->editor.setVisible (false); }

    app.project.addChangeListener (this);
    view = def().view;
    def().sync (app.project.edits);
    refreshAll (true);
    setView (view);
    scheduleRecheck (300);                                                  // the first look at the renders, just after the window has opened
    startTimerHz (30);
    setWantsKeyboardFocus (true);
    for (auto* c : getChildren()) if (dynamic_cast<juce::Button*> (c) != nullptr) c->setWantsKeyboardFocus (false);     // a click on a button must not take the arrow keys away
    setSize (1320, 720);
}

MasteringComponent::~MasteringComponent()
{
    stopPlay();
    app.project.removeChangeListener (this);
    job.reset();
    renderJob.reset();
}

MasterField* MasteringComponent::addField (juce::Component& parent, juce::OwnedArray<MasterField>& list, const juce::String& caption, std::function<juce::String*()> ref, int maxChars)
{
    auto* f = list.add (new MasterField());
    f->ref = std::move (ref);
    styleCaption (f->caption, caption);
    if (maxChars > 0) f->editor.setInputRestrictions (maxChars);
    f->editor.setMultiLine (false);
    f->editor.onTextChange = [this, f] { if (auto* p = f->ref()) { *p = f->editor.getText(); touched(); } };
    parent.addAndMakeVisible (f->caption); parent.addAndMakeVisible (f->editor);
    return f;
}

// ----------------------------------------------------------------------------- model helpers
/** The loudest level in each small slice of the whole edit (all its pieces, up to four files of each), worked out in the background and remembered until the edit changes. */
static std::vector<float> computeEnvelope (std::vector<EditRegion> regions, juce::int64 total, double rate, int bins)
{
    std::vector<float> out ((size_t) bins, 0.0f);
    if (total <= 0 || rate <= 0.0) return out;
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> readers;
    const double binLen = (double) total / (double) bins;
    for (auto& r : regions)
    {
        const int b0 = juce::jlimit (0, bins - 1, (int) ((double) r.startSample / binLen)), b1 = juce::jlimit (0, bins - 1, (int) ((double) r.endSample() / binLen));
        for (size_t fi = 0; fi < r.files.size() && fi < 4; ++fi)
        {
            auto& rf = r.files[fi];
            auto& rd = readers[rf.file.getFullPathName()];
            if (rd == nullptr) rd.reset (fm.createReaderFor (rf.file));
            if (rd == nullptr) continue;
            for (int b = b0; b <= b1; ++b)
            {
                const juce::int64 ta = juce::jmax (r.startSample, (juce::int64) std::llround ((double) b * binLen)), tb = juce::jmin (r.endSample(), (juce::int64) std::llround ((double) (b + 1) * binLen));
                if (tb <= ta) continue;
                const juce::int64 fs = (r.srcIn - rf.fileStart) + (ta - r.startSample);
                if (fs < 0 || fs >= rd->lengthInSamples) continue;
                juce::Range<float> lv[2];
                rd->readMaxLevels (fs, juce::jmin (tb - ta, rd->lengthInSamples - fs), lv, juce::jmin (2, (int) rd->numChannels));
                for (int c = 0; c < juce::jmin (2, (int) rd->numChannels); ++c)
                    out[(size_t) b] = juce::jmax (out[(size_t) b], juce::jmax (std::abs (lv[c].getStart()), std::abs (lv[c].getEnd())));
            }
        }
    }
    return out;
}

void MasteringComponent::requestEnvelope (const juce::Uuid& editId)
{
    auto* e = editOf (editId);
    if (e == nullptr || e->isEmpty() || e->sampleRate <= 0.0) return;
    juce::int64 hash = (juce::int64) e->regions.size() * 31 + (juce::int64) e->overdubs.size();
    auto mix = [&hash] (juce::int64 v) { hash = hash * 1099511628211LL + v; };
    for (auto* list : { &e->regions, &e->overdubs })
        for (auto& r : *list)
        {
            mix (r.startSample); mix (r.srcIn); mix (r.srcOut);
            for (auto& f : r.files) { mix (f.fileStart); mix ((juce::int64) f.file.getFullPathName().hashCode64()); mix (f.file.getSize()); }
        }
    const auto id = editId.toString(), key = juce::String (hash) + "/" + juce::String (def().tailSeconds);
    auto have = envelopeKeys.find (id);
    if ((have != envelopeKeys.end() && have->second == key) || envelopePending.count (id + key) > 0) return;
    envelopePending.insert (id + key);
    std::vector<EditRegion> regs = e->regions; regs.insert (regs.end(), e->overdubs.begin(), e->overdubs.end());
    const juce::int64 total = e->lengthSamples();
    const double rate = e->sampleRate;
    const double master = (double) juce::jmax ((juce::int64) 1, masterLengthSamples (*e, def().tailSeconds));
    const int bins = juce::jlimit (400, 60000, (int) ((double) total / rate * 40.0));      // 40 slices a second: smooth even when zoomed in
    juce::Component::SafePointer<MasteringComponent> safe (this);
    juce::Thread::launch ([safe, regs, total, rate, master, bins, id, key]
    {
        auto env = computeEnvelope (regs, total, rate, bins);
        const double fraction = juce::jlimit (0.05, 1.0, (double) total / master);
        juce::MessageManager::callAsync ([safe, env, id, key, fraction]
        {
            if (safe == nullptr) return;
            safe->envelopePending.erase (id + key);
            safe->envelopes[id] = { env, fraction }; safe->envelopeKeys[id] = key;
            if (safe->timeline != nullptr) safe->timeline->repaint();
        });
    });
}

const EditDef* MasteringComponent::editOf (const juce::Uuid& id) const { return const_cast<Project&> (app.project).findEdit (id); }
juce::String MasteringComponent::editName (const juce::Uuid& id) const { auto* e = editOf (id); return e != nullptr ? e->name : juce::String ("(deleted edit)"); }

std::vector<juce::int64> MasteringComponent::clipFrames() const
{
    std::vector<juce::int64> out;
    for (auto& c : const_cast<MasteringComponent*> (this)->def().ddp.clips)
    {
        auto* e = editOf (c.editId);
        out.push_back (e == nullptr || e->isEmpty() ? 0 : convertedLength (masterLengthSamples (*e, const_cast<MasteringComponent*> (this)->def().tailSeconds), e->sampleRate, 44100.0));
    }
    return out;
}

juce::String MasteringComponent::signature() const
{
    juce::String s;
    for (auto& e : app.project.edits) if (e != nullptr) s << e->id.toString() << "|" << e->name << "|" << e->lengthSamples() << "|" << e->firstSample() << "|" << e->sampleRate << ";";
    for (auto& b : app.project.buses) s << b.id.toString() << b.name << (b.external ? "E" : "I") << ";";
    for (auto& t : app.project.tracks) s << t.id.toString() << t.name << ";";
    for (auto& m : app.project.mixers) s << m->id.toString() << m->name << ";";
    return s;
}

void MasteringComponent::changeListenerCallback (juce::ChangeBroadcaster*) { refreshAll (false); }
void MasteringComponent::timerCallback()
{
    pollPlayback();
    // the renders are looked at when the window opens, when it comes to the front again (you may have changed an edit or its mixer meanwhile), and shortly after the choice of pieces changes
    {
        bool active = false;
        if (auto* tl = dynamic_cast<juce::TopLevelWindow*> (getTopLevelComponent())) active = tl == juce::TopLevelWindow::getActiveTopLevelWindow();
        if (active && ! wasActive) scheduleRecheck (150);
        wasActive = active;
        if (recheckDue != 0 && juce::Time::getMillisecondCounter() >= recheckDue && isShowing())
        {
            if (job != nullptr || renderJob != nullptr) recheckDue = juce::Time::getMillisecondCounter() + 500;       // busy: look again afterwards
            else { recheckDue = 0; checkRenders(); }
        }
    }
    if (renderJob != nullptr) renderProgressValue = renderJob->getProgress();
    if (job != nullptr)
    {
        progressValue = job->getProgress();
        const auto st = job->getStatus();
        (lastWasDisc ? ddpStatus : vmStatus).setText (st, juce::dontSendNotification);
    }
}

void MasteringComponent::refreshAll (bool force)
{
    def().sync (app.project.edits);
    const auto sig = signature();
    if (! force && sig == lastSignature) return;
    lastSignature = sig;
    refreshSource();
    refreshVm();
    refreshDdp();
}

void MasteringComponent::refreshSource()
{
    updating = true;
    auto& p = app.project;
    mixerBox.clear (juce::dontSendNotification); mixerIds.clear();
    int sel = 1, i = 0;
    mixerBox.addItem ("Each piece through its own mixer", ++i); mixerIds.push_back (juce::Uuid::null());       // the default: every Edit has a mixer of its own
    int mk = 0;
    for (auto& m : p.mixers)
    {
        const auto label = m->editId.isNull() ? (mk == 0 ? juce::String ("All pieces through the processing mixer: ") : juce::String ("All pieces through the cue mixer: "))
                                              : juce::String ("All pieces through the edit mixer: ");
        mixerBox.addItem (label + m->name, ++i); mixerIds.push_back (m->id); if (m->id == def().mixerId) sel = i;
        ++mk;
    }
    mixerBox.setSelectedId (sel, juce::dontSendNotification);
    sourceBox.clear (juce::dontSendNotification); sourceIds.clear();
    int ssel = 0, k = 0, firstExt = 0;
    for (auto& b : p.buses) if (b.external) { sourceBox.addItem (b.name + "  (Ext bus)", ++k); sourceIds.push_back (b.id); if (firstExt == 0) firstExt = k; if (b.id == def().sourceId) ssel = k; }
    for (auto& b : p.buses) if (! b.external) { sourceBox.addItem (b.name + "  (Int bus)", ++k); sourceIds.push_back (b.id); if (b.id == def().sourceId) ssel = k; }
    for (auto& t : p.tracks) { sourceBox.addItem (t.name + "  (track)", ++k); sourceIds.push_back (t.id); if (t.id == def().sourceId) ssel = k; }
    sourceBox.setSelectedId (ssel > 0 ? ssel : firstExt, juce::dontSendNotification);
    tailBox.setSelectedId (juce::jlimit (0, 10, (int) std::lround (def().tailSeconds)) + 1, juce::dontSendNotification);
    updating = false;
}

void MasteringComponent::refreshVm()
{
    std::vector<MasterListEntry> es;
    for (auto& it : def().items)
    {
        auto* e = editOf (it.editId);
        MasterListEntry en; en.id = it.editId; en.include = it.include && e != nullptr && ! e->isEmpty();
        en.name = it.fileName.isNotEmpty() ? it.fileName : (e != nullptr ? e->name : juce::String ("(deleted)"));
        en.detail = e != nullptr ? lengthText (masterLengthSamples (*e, def().tailSeconds), e->sampleRate) : juce::String();
        if (auto r = renders.find (it.editId.toString()); r != renders.end() && en.include) { en.status = r->second.text; en.bad = r->second.bad; }
        es.push_back (en);
    }
    vmList->setEntries (es, def().selectedItem);
    refreshFormatControls();
    refreshFields();
}

void MasteringComponent::rebuildQualityBox()
{
    qualityBox.clear (juce::dontSendNotification);
    auto& vm = def().vm;
    if (vm.format == MasterFormat::Mp3)
    {
        const char* names[] = { "320 kbps (best, constant)", "256 kbps", "224 kbps", "192 kbps", "160 kbps", "128 kbps",
                                "Variable V0 (about 245 kbps)", "Variable V1 (about 225 kbps)", "Variable V2 (about 190 kbps)", "Variable V3 (about 175 kbps)", "Variable V4 (about 165 kbps)" };
        for (int i = 0; i < 11; ++i) qualityBox.addItem (names[i], i + 1);
        int id = 1; const int k[] = { 320, 256, 224, 192, 160, 128 };
        if (vm.mp3Kbps == 0) id = 7 + juce::jlimit (0, 4, vm.mp3Vbr); else for (int i = 0; i < 6; ++i) if (k[i] == vm.mp3Kbps) id = i + 1;
        qualityBox.setSelectedId (id, juce::dontSendNotification);
    }
    else if (vm.format == MasterFormat::Flac)
    {
        juce::FlacAudioFormat flacFmt; const auto q = flacFmt.getQualityOptions();
        for (int i = 0; i < q.size(); ++i) qualityBox.addItem (q[i], i + 1);
        qualityBox.setSelectedId (juce::jlimit (0, juce::jmax (0, q.size() - 1), vm.flacLevel) + 1, juce::dontSendNotification);
    }
    else if (vm.format == MasterFormat::Ogg)
    {
        juce::OggVorbisAudioFormat oggFmt; const auto q = oggFmt.getQualityOptions();
        for (int i = 0; i < q.size(); ++i) qualityBox.addItem (q[i], i + 1);
        qualityBox.setSelectedId (juce::jlimit (0, q.size() - 1, vm.oggQuality) + 1, juce::dontSendNotification);
    }
}

void MasteringComponent::refreshFormatControls()
{
    updating = true;
    auto& vm = def().vm;
    formatBox.setSelectedId ((int) vm.format + 1, juce::dontSendNotification);
    const double rates[] = { 0, 44100, 48000, 88200, 96000, 176400, 192000 };
    int rid = 1; for (int i = 0; i < 7; ++i) if (std::abs (rates[i] - vm.sampleRate) < 1.0) rid = i + 1;
    rateBox.setSelectedId (rid, juce::dontSendNotification);
    const bool mp3 = vm.format == MasterFormat::Mp3;
    for (int i = 4; i <= 7; ++i) rateBox.setItemEnabled (i, ! mp3);                       // MP3 stops at 48 kHz
    bitsBox.clear (juce::dontSendNotification);
    const bool lossy = vm.format == MasterFormat::Ogg || mp3;
    if (lossy) { bitsBox.addItem ("(not used)", 1); bitsBox.setSelectedId (1, juce::dontSendNotification); }
    else
    {
        bitsBox.addItem ("16 bit", 1); bitsBox.addItem ("24 bit", 2);
        if (vm.format == MasterFormat::Wav) bitsBox.addItem ("32 bit float", 3);
        bitsBox.setSelectedId (vm.bitDepth == 16 ? 1 : vm.bitDepth == 32 && vm.format == MasterFormat::Wav ? 3 : 2, juce::dontSendNotification);
    }
    bitsBox.setEnabled (! lossy);
    ditherBox.setSelectedId (vm.dither == DitherMode::Tpdf ? 2 : 1, juce::dontSendNotification);
    ditherBox.setEnabled (! (vm.format == MasterFormat::Ogg || (vm.format == MasterFormat::Wav && vm.bitDepth == 32)));
    rebuildQualityBox();
    const bool hasQuality = (lossy || vm.format == MasterFormat::Flac) && view == 0;
    qualityBox.setVisible (hasQuality); qualityCaption.setVisible (hasQuality);
    qualityCaption.setText (vm.format == MasterFormat::Flac ? "Compression" : "Quality", juce::dontSendNotification);
    for (int f = 0; f < 5; ++f) alsoToggle[f].setToggleState ((vm.extraFormats & (1 << f)) != 0, juce::dontSendNotification);
    normToggle.setToggleState (vm.normalise, juce::dontSendNotification);
    overallToggle.setToggleState (vm.peakTogether, juce::dontSendNotification); individualToggle.setToggleState (! vm.peakTogether, juce::dontSendNotification);
    overallToggle.setEnabled (vm.normalise); individualToggle.setEnabled (vm.normalise); peakEditor.setEnabled (vm.normalise);
    peakEditor.setText (juce::String (vm.peakDb, 1), false);
    numberToggle.setToggleState (vm.numberFiles, juce::dontSendNotification);
    if (vm.folder.trim().isEmpty()) vm.folder = app.project.masteredFolder().getFullPathName();           // the default place: the project's Bounced Media / Mastered Audio folder
    folderEditor.setText (vm.folder, false);
    if (mp3 && view == 0)
    {
        static int lameState = -1;                                                           // looked up once: starting a program to look is slow
        if (lameState < 0) lameState = findLame().isNotEmpty() ? 1 : 0;
        vmStatus.setColour (juce::Label::textColourId, lameState ? theme::text : theme::warn);
        vmStatus.setText (lameState ? "MP3: the LAME encoder was found." : "MP3 needs the free LAME encoder: put lame.exe in the same folder as Fermata.", juce::dontSendNotification);
    }
    updating = false;
    resized();
}

void MasteringComponent::refreshFields()
{
    updating = true;
    for (auto* list : { &albumFields, &fileFields, &discFields, &clipFields })
        for (auto* f : *list) { auto* p = f->ref(); f->editor.setText (p != nullptr ? *p : juce::String(), false); f->editor.setEnabled (p != nullptr); }
    updating = false;
}

void MasteringComponent::refreshDdp (bool reloadFields)
{
    auto& d = def().ddp;
    const auto frames = clipFrames();
    const auto L = computePq (d, frames);
    std::vector<MasterListEntry> es;
    for (size_t i = 0; i < d.clips.size(); ++i)
    {
        auto* e = editOf (d.clips[i].editId);
        MasterListEntry en; en.id = d.clips[i].editId; en.include = d.clips[i].include && frames[i] > 0;
        en.name = d.clips[i].title.isNotEmpty() ? d.clips[i].title : (e != nullptr ? e->name : juce::String ("(deleted)"));
        en.detail = frames[i] > 0 ? lengthText (frames[i], 44100.0) : juce::String ("empty");
        if (auto r = renders.find (d.clips[i].editId.toString()); r != renders.end() && en.include) { en.status = r->second.text; en.bad = r->second.bad; }
        es.push_back (en);
    }
    ddpList->setEntries (es, def().selectedClip);
    std::vector<MasterBlock> blocks; std::vector<MasterPqRow> rows;
    for (auto& t : L.tracks)
    {
        const auto& c = d.clips[(size_t) t.clipIndex];
        const auto title = c.title.isNotEmpty() ? c.title : editName (c.editId);
        MasterBlock b; b.clipIndex = t.clipIndex; b.id = t.editId; b.title = title; b.start = t.audioStart; b.index01 = t.index01; b.end = t.endSector; b.gap = t.gapSectors; b.number = t.number; b.index00 = t.index00;
        blocks.push_back (b);
        MasterPqRow r; r.id = t.editId; r.number = juce::String (t.number); r.title = title;
        r.index00 = t.index00 >= 0 ? sectorsToMsf (t.index00) : juce::String ("-"); r.index01 = sectorsToMsf (t.index01);
        r.length = sectorsToMsf (t.lengthSectors); r.pause = t.number == 1 ? "lead-in " + secondsText (t.gapSectors) : t.gapSectors < 0 ? "overlap " + secondsText (-t.gapSectors) : secondsText (t.gapSectors);
        r.isrc = c.isrc; r.flags = juce::String (c.preEmphasis ? "PRE " : "") + (c.copyPermitted ? "COPY" : "");
        rows.push_back (r);
    }
    timeline->envelopes = &envelopes;
    for (auto& c : d.clips) requestEnvelope (c.editId);
    timeline->setData (blocks, L.leadOut, def().selectedClip);
    updateTimeLabel();
    pqTable->setSize (juce::jmax (300, pqView.getMaximumVisibleWidth()), pqTable->getHeight());
    pqTable->setRows (rows, sectorsToMsf (L.leadOut) + "   (an 80 minute disc holds 79:57:00)", def().selectedClip);
    juce::String w; for (int i = 0; i < juce::jmin (4, L.warnings.size()); ++i) w << (i ? "\n" : "") << "! " << L.warnings[i];
    if (L.warnings.size() > 4) w << "\n... and " << (L.warnings.size() - 4) << " more";
    warnings.setText (w, juce::dontSendNotification);
    // the selected track
    updating = true;
    int selGap = 0; bool selIsFirst = false, selFound = false;
    for (auto& t : L.tracks) if (t.editId == def().selectedClip) { selGap = t.gapSectors; selIsFirst = t.number == 1; selFound = true; }
    gapEditor.setEnabled (selFound);
    if (! gapEditor.hasKeyboardFocus (false)) gapEditor.setText (selFound ? juce::String ((double) selGap / 75.0, 2) : juce::String(), false);
    (void) selIsFirst;
    if (auto* c = def().findClip (def().selectedClip)) { preToggle.setToggleState (c->preEmphasis, juce::dontSendNotification); copyToggle.setToggleState (c->copyPermitted, juce::dontSendNotification); }
    preToggle.setEnabled (def().findClip (def().selectedClip) != nullptr); copyToggle.setEnabled (preToggle.isEnabled());
    zipToggle.setToggleState (d.makeZip, juce::dontSendNotification); cueToggle.setToggleState (d.makeCue, juce::dontSendNotification);
    ddpNorm.setToggleState (d.normalise, juce::dontSendNotification); ddpTogether.setToggleState (d.peakTogether, juce::dontSendNotification);
    ddpPeakEditor.setText (juce::String (d.peakDb, 1), false); if (d.folder.trim().isEmpty()) d.folder = app.project.masteredFolder().getFullPathName();
    ddpFolderEditor.setText (d.folder, false);
    ddpTogether.setEnabled (d.normalise); ddpPeakEditor.setEnabled (d.normalise);
    updating = false;
    if (reloadFields) refreshFields();
    resized();
}

// ----------------------------------------------------------------------------- actions on the lists
void MasteringComponent::moveItem (int from, int to, bool disc)
{
    if (disc)
    {
        auto& v = def().ddp.clips;
        if (! juce::isPositiveAndBelow (from, (int) v.size()) || ! juce::isPositiveAndBelow (to, (int) v.size())) return;
        auto c = v[(size_t) from]; v.erase (v.begin() + from); v.insert (v.begin() + to, c);
        touched(); refreshDdp (false);
    }
    else
    {
        auto& v = def().items;
        if (! juce::isPositiveAndBelow (from, (int) v.size()) || ! juce::isPositiveAndBelow (to, (int) v.size())) return;
        auto c = v[(size_t) from]; v.erase (v.begin() + from); v.insert (v.begin() + to, c);
        touched(); refreshVm();
    }
}

void MasteringComponent::swapClips (int a, int b)
{
    auto& v = def().ddp.clips;
    if (! juce::isPositiveAndBelow (a, (int) v.size()) || ! juce::isPositiveAndBelow (b, (int) v.size()) || a == b) return;
    std::swap (v[(size_t) a], v[(size_t) b]);
    std::swap (v[(size_t) a].gapSectors, v[(size_t) b].gapSectors);                      // the pauses stay with their places on the disc
    std::swap (v[(size_t) a].index01Shift, v[(size_t) b].index01Shift); std::swap (v[(size_t) a].index00Shift, v[(size_t) b].index00Shift);
    touched(); refreshDdp (false);
}

void MasteringComponent::moveFlag (int kind, int clipIndex, int sector)
{
    auto& clips = def().ddp.clips;
    if (kind != 2 && ! juce::isPositiveAndBelow (clipIndex, (int) clips.size())) return;
    const auto L = computePq (def().ddp, clipFrames());
    if (kind == 2)
    {
        if (L.tracks.empty()) return;
        def().ddp.endPadSectors = juce::jmax (0, sector - L.tracks.back().endSector);
    }
    else
    {
        const PqTrack* t = nullptr; for (auto& x : L.tracks) if (x.clipIndex == clipIndex) t = &x;
        if (t == nullptr) return;
        auto& c = clips[(size_t) clipIndex];
        if (kind == 1) c.index01Shift = sector - t->audioStart;
        else c.index00Shift = sector - t->audioStart;          // the end of the previous track: free, but never after INDEX 01
    }
    touched(); refreshDdp (false);
}

void MasteringComponent::resetFlag (int kind, int clipIndex)
{
    auto& clips = def().ddp.clips;
    if (kind == 2) def().ddp.endPadSectors = 0;
    else if (juce::isPositiveAndBelow (clipIndex, (int) clips.size())) { if (kind == 1) clips[(size_t) clipIndex].index01Shift = 0; else clips[(size_t) clipIndex].index00Shift = kIndexAuto; }
    touched(); refreshDdp (false);
}

void MasteringComponent::applyGapToSelected (const juce::String& text)
{
    if (updating) return;
    auto* c = def().findClip (def().selectedClip);
    if (c == nullptr || text.trim().isEmpty()) return;
    const bool firstOne = ! def().ddp.clips.empty() && &def().ddp.clips.front() == c;
    c->gapSectors = juce::jmax (firstOne ? 150 : -kMaxOverlapSectors, (int) std::llround (text.getDoubleValue() * 75.0));
    touched(); refreshDdp (false);
}

void MasteringComponent::setAllGaps (double seconds)
{
    bool first = true;
    for (auto& c : def().ddp.clips)
    {
        if (! c.include) continue;
        c.gapSectors = first ? 150 : sectorsFromSeconds (seconds);
        c.index01Shift = 0; c.index00Shift = kIndexAuto;                    // a clean set: the PQ flags go back to the audio
        first = false;
    }
    def().ddp.endPadSectors = 0;
    touched(); refreshDdp (false);
}

void MasteringComponent::chooseFolder (bool disc)
{
    const auto start = juce::File (disc ? def().ddp.folder : def().vm.folder);
    chooser = std::make_unique<juce::FileChooser> (disc ? "Choose the folder for the DDP" : "Choose the folder for the files", start.exists() ? start : app.project.audioFolder().getParentDirectory());
    juce::Component::SafePointer<MasteringComponent> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe, disc] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        const auto path = fc.getResult().getFullPathName();
        if (disc) { safe->def().ddp.folder = path; safe->ddpFolderEditor.setText (path, false); } else { safe->def().vm.folder = path; safe->folderEditor.setText (path, false); }
        safe->touched();
    });
}

// ----------------------------------------------------------------------------- exporting
void MasteringComponent::setBusy (bool busy)
{
    for (juce::Component* c : std::initializer_list<juce::Component*> { &formatBox, &rateBox, &bitsBox, &ditherBox, &qualityBox, &normToggle, &overallToggle, &individualToggle, &numberToggle, &alsoCaption, &alsoToggle[0], &alsoToggle[1], &alsoToggle[2], &alsoToggle[3], &alsoToggle[4], &peakEditor,
                                                                         &folderEditor, &chooseButton, &mixerBox, &sourceBox, &tailBox, &vmAll, &vmNone, &vmUp, &vmDown, &autoPq, &allGapsBox, &ddpChoose,
                                                                         &ddpFolderEditor, &zipToggle, &cueToggle, &ddpNorm, &ddpTogether, &ddpPeakEditor, &ddpUp, &ddpDown, &gapEditor, &ddpCheck })
        c->setEnabled (! busy);
    exportButton.setButtonText (busy && ! lastWasDisc ? "Cancel" : "Export");
    ddpExport.setButtonText (busy && lastWasDisc ? "Cancel" : "Make DDP");
    exportButton.setEnabled (! busy || ! lastWasDisc); ddpExport.setEnabled (! busy || lastWasDisc);
    if (! busy) refreshFormatControls();
}

void MasteringComponent::startVmExport()
{
    if (job != nullptr) { job->cancel(); vmStatus.setText ("Cancelling...", juce::dontSendNotification); return; }
    if (renderJob != nullptr) { vmStatus.setColour (juce::Label::textColourId, theme::warn); vmStatus.setText ("The pieces are being rendered: try again when the line at the top says the renders are up to date.", juce::dontSendNotification); return; }
    if (app.engine.isRecording()) { showError ("Mastering", "Stop recording first."); return; }
    MasterJobSpec spec;
    const auto err = makeFilesSpec (app.project, def(), spec);
    if (err.isNotEmpty()) { vmStatus.setColour (juce::Label::textColourId, theme::warn); vmStatus.setText (err, juce::dontSendNotification); return; }
    if (auto* st = app.props.getUserSettings()) { st->setValue ("masterFolder", def().vm.folder); app.props.saveIfNeeded(); }
    startJob (spec, false);
}

void MasteringComponent::startDdpExport()
{
    if (job != nullptr) { job->cancel(); ddpStatus.setText ("Cancelling...", juce::dontSendNotification); return; }
    if (renderJob != nullptr) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText ("The pieces are being rendered: try again when the line at the top says the renders are up to date.", juce::dontSendNotification); return; }
    stopPlay();
    if (app.engine.isRecording()) { showError ("Mastering", "Stop recording first."); return; }
    MasterJobSpec spec;
    auto err = makeDiscSpec (app.project, def(), spec);
    if (err.isEmpty())
    {
        const auto L = computePq (def().ddp, clipFrames());
        if (L.tracks.size() > 99) err = "A CD holds 99 tracks at most.";
        else if (L.leadOut > kMaxCdSectors) err = "The disc is longer than 79:57. Leave something out or shorten a pause.";
        else if (! isValidUpc (def().ddp.upc)) err = "The UPC / EAN must be 13 digits with a correct check digit (or leave it empty).";
        else for (auto& t : L.tracks) if (! isValidIsrc (def().ddp.clips[(size_t) t.clipIndex].isrc)) { err = "Track " + juce::String (t.number) + ": the ISRC must look like GB-AJY-14-12345 (or be empty)."; break; }
    }
    if (err.isNotEmpty()) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText (err, juce::dontSendNotification); return; }
    startJob (spec, true);
}

void MasteringComponent::startJob (const MasterJobSpec& spec, bool disc)
{
    lastWasDisc = disc;
    auto& status = disc ? ddpStatus : vmStatus;
    status.setColour (juce::Label::textColourId, theme::text);
    status.setText ("Starting...", juce::dontSendNotification);
    (disc ? ddpShow : showButton).setVisible (false);
    progressValue = 0.0;
    setBusy (true);
    juce::Component::SafePointer<MasteringComponent> safe (this);
    job = std::make_unique<MasterExportJob> (app.project, spec, [safe, disc] (MasterExportResult r) { if (safe != nullptr) safe->jobFinished (r, disc); });
}

void MasteringComponent::jobFinished (const MasterExportResult& r, bool disc)
{
    job.reset();
    setBusy (false);
    scheduleRecheck (300);                                               // (the export made or reused the renders: show how they stand)
    auto& status = disc ? ddpStatus : vmStatus;
    progressValue = r.error.isEmpty() && ! r.cancelled ? 1.0 : 0.0;
    if (r.cancelled) { status.setColour (juce::Label::textColourId, theme::text); status.setText ("Cancelled.", juce::dontSendNotification); return; }
    if (r.error.isNotEmpty()) { status.setColour (juce::Label::textColourId, theme::warn); status.setText (r.error, juce::dontSendNotification); return; }
    status.setColour (juce::Label::textColourId, r.clipped ? theme::warn : theme::text);
    if (disc)
        status.setText ("Done. Loudest peak " + juce::String (r.peakDb, 1) + " dBFS" + (r.clipped ? " (CLIPPED: lower the level)" : "") + ".\n" + r.report, juce::dontSendNotification);
    else
        status.setText ("Done: " + juce::String (r.files.size()) + " file(s), loudest peak " + juce::String (r.peakDb, 1) + " dBFS"
                        + (r.clipped ? "   -  THIS CLIPPED. Tick 'Set the peak level' or lower the faders." : "") + "\n" + juce::File (r.files[0]).getFileName() + (r.files.size() > 1 ? "  ..." : ""), juce::dontSendNotification);
    if (! r.files.isEmpty()) { lastOutput = juce::File (r.files[0]); (disc ? ddpShow : showButton).setVisible (true); }
    resized();
}

// ----------------------------------------------------------------------------- layout
void MasteringComponent::setView (int v)
{
    if (v != 1) stopPlay();
    view = v; def().view = v;
    vmTab.setToggleState (v == 0, juce::dontSendNotification); ddpTab.setToggleState (v == 1, juce::dontSendNotification);
    vmTab.setColour (juce::TextButton::buttonColourId, v == 0 ? theme::accent : theme::button);
    ddpTab.setColour (juce::TextButton::buttonColourId, v == 1 ? theme::accent : theme::button);
    const bool vm = v == 0, dd = v == 1;
    for (juce::Component* c : std::initializer_list<juce::Component*> { vmList.get(), &vmAll, &vmNone, &vmUp, &vmDown, &vmSettings, &vmProgress, &vmStatus, &exportButton })
        c->setVisible (vm);
    showButton.setVisible (vm && showButton.isVisible());
    for (juce::Component* c : std::initializer_list<juce::Component*> { &fmtCaption, &rateCaption, &bitsCaption, &ditherCaption, &levelCaption, &albumCaption, &fileCaption, &nameCaption, &folderCaption,
                                                                         &formatBox, &rateBox, &bitsBox, &ditherBox, &normToggle, &overallToggle, &individualToggle, &numberToggle, &alsoCaption, &alsoToggle[0], &alsoToggle[1], &alsoToggle[2], &alsoToggle[3], &alsoToggle[4], &peakEditor, &peakUnit,
                                                                         &folderEditor, &chooseButton })
        c->setVisible (vm);
    for (auto* f : albumFields) { f->caption.setVisible (vm); f->editor.setVisible (vm); }
    for (auto* f : fileFields) { f->caption.setVisible (vm); f->editor.setVisible (vm); }
    for (juce::Component* c : std::initializer_list<juce::Component*> { ddpList.get(), &rightView, timeline.get(), &playBtn, &gapPlayBtn, &startBtn, &timeLabel, &pqView, &discCaption, &clipCaption, &pqCaption, &gapCaption, &allGapsCaption, &ddpFolderCaption, &ddpPeakUnit,
                                                                         &warnings, &ddpStatus, &gapEditor, &allGapsBox, &autoPq, &isrcBtn, &zoomIn, &zoomOut, &zoomFit, &ddpUp, &ddpDown, &ddpChoose, &ddpExport,
                                                                         &ddpCheck, &preToggle, &copyToggle, &zipToggle, &cueToggle, &ddpNorm, &ddpTogether, &ddpFolderEditor, &ddpPeakEditor, &ddpProgress })
        c->setVisible (dd);
    ddpShow.setVisible (dd && ddpShow.isVisible());
    for (auto* f : discFields) { f->caption.setVisible (dd); f->editor.setVisible (dd); }
    for (auto* f : clipFields) { f->caption.setVisible (dd); f->editor.setVisible (dd); }
    refreshFormatControls();
    updateRenderButtons();
    resized();
}

void MasteringComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    g.setColour (theme::panel); g.fillRect (0, 0, getWidth(), 76);
    g.setColour (theme::border); g.fillRect (0, 45, getWidth(), 1); g.fillRect (0, 75, getWidth(), 1);
}

void MasteringComponent::resized()
{
    auto all = getLocalBounds();
    {
        auto bar = all.removeFromTop (46).reduced (8, 0);
        vmTab.setBounds (bar.removeFromLeft (150).withSizeKeepingCentre (146, grid::btnH + 4));
        ddpTab.setBounds (bar.removeFromLeft (150).withSizeKeepingCentre (146, grid::btnH + 4));
        auto r = bar.removeFromRight (juce::jmin (bar.getWidth(), 640));
        tailBox.setBounds (r.removeFromRight (70).withSizeKeepingCentre (70, grid::btnH));
        tailCaption.setBounds (r.removeFromRight (42));
        r.removeFromRight (8);
        sourceBox.setBounds (r.removeFromRight (210).withSizeKeepingCentre (210, grid::btnH));
        r.removeFromRight (6);
        mixerBox.setBounds (r.removeFromRight (200).withSizeKeepingCentre (200, grid::btnH));
        renderCaption.setBounds (r.removeFromRight (86));
    }
    {
        auto strip = all.removeFromTop (30).reduced (8, 3);                        // the renders
        renderAgainBtn.setBounds (strip.removeFromRight (120)); strip.removeFromRight (6);
        prevRenderBtn.setBounds (strip.removeFromRight (170)); strip.removeFromRight (10);
        { const int pw = juce::jmin (220, strip.getWidth() / 3); renderProgress.setBounds (strip.removeFromRight (pw).withSizeKeepingCentre (pw, 16)); strip.removeFromRight (10); }
        renderStatus.setBounds (strip);
    }
    all.reduce (10, 10);
    if (view == 0) layoutVm (all); else layoutDdp (all);
}

static void layoutFieldGrid (juce::OwnedArray<MasterField>& fields, juce::Rectangle<int> area, int cols, int captionW, int& usedHeight)
{
    const int rows = (fields.size() + cols - 1) / cols;
    const int cw = area.getWidth() / juce::jmax (1, cols);
    for (int i = 0; i < fields.size(); ++i)
    {
        const int cx = area.getX() + (i % cols) * cw, cy = area.getY() + (i / cols) * 30;
        fields[i]->caption.setBounds (cx, cy, captionW, 26);
        fields[i]->editor.setBounds (cx + captionW, cy + 1, cw - captionW - 10, 25);
    }
    usedHeight = rows * 30;
}

void MasteringComponent::layoutVm (juce::Rectangle<int> area)
{
    auto left = area.removeFromLeft (juce::jmax (380, area.getWidth() * 48 / 100));
    area.removeFromLeft (12);
    auto btns = left.removeFromBottom (32);
    left.removeFromBottom (6);
    vmList->setBounds (left);
    for (auto* b : { &vmAll, &vmNone, &vmUp, &vmDown }) b->setBounds (btns.removeFromLeft (grid::btnW).withSizeKeepingCentre (grid::btnW - 4, grid::btnH + 2));

    // the export controls at the bottom: the progress bar and the buttons share one row
    auto bottom = area.removeFromBottom (74);
    area.removeFromBottom (6);
    vmStatus.setBounds (bottom.removeFromTop (38));
    {
        auto buttons = bottom.removeFromBottom (32);
        exportButton.setBounds (buttons.removeFromRight (140).reduced (2));
        showButton.setBounds (buttons.removeFromRight (150).reduced (2));
        buttons.removeFromRight (8);
        vmProgress.setBounds (buttons.withSizeKeepingCentre (buttons.getWidth(), 18));
    }

    vmSettings.setBounds (area);
    const int w = juce::jmax (300, vmSettings.getMaximumVisibleWidth());
    const bool twoCols = w >= 760;
    const int colW = twoCols ? w / 2 : w;
    int x0 = 0, y = 4;
    auto row = [&] (int h) { auto r = juce::Rectangle<int> (x0 + 6, y, colW - 12, h); y += h + 4; return r; };
    auto labelled = [&] (juce::Label& l, juce::ComboBox& c)
    {
        auto r = row (grid::btnH + 2);
        l.setBounds (r.removeFromLeft (110)); c.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 280)));
    };
    fmtCaption.setBounds (x0 + 6, y, colW - 12, 20); y += 24;
    { auto r1 = row (grid::btnH + 2); formatBox.setBounds (r1.removeFromLeft (juce::jmin (r1.getWidth(), 280))); }
    labelled (rateCaption, rateBox);
    labelled (bitsCaption, bitsBox);
    labelled (ditherCaption, ditherBox);
    if (qualityBox.isVisible()) labelled (qualityCaption, qualityBox); else { qualityCaption.setBounds (0, 0, 0, 0); qualityBox.setBounds (0, 0, 0, 0); }
    y += 6;
    levelCaption.setBounds (row (20));
    { auto r = row (grid::btnH + 2); normToggle.setBounds (r.removeFromLeft (170)); peakEditor.setBounds (r.removeFromLeft (64)); peakUnit.setBounds (r.removeFromLeft (54).withTrimmedLeft (6)); }
    overallToggle.setBounds (row (22).withTrimmedLeft (24)); individualToggle.setBounds (row (22).withTrimmedLeft (24));
    y += 6;
    nameCaption.setBounds (row (20));
    numberToggle.setBounds (row (22));
    alsoCaption.setBounds (row (20));
    { auto r = row (24); const int cw = juce::jmax (60, r.getWidth() / 5); for (int f = 0; f < 5; ++f) alsoToggle[f].setBounds (r.removeFromLeft (cw)); }
    { auto r = row (grid::btnH + 2); folderCaption.setBounds (r.removeFromLeft (60)); chooseButton.setBounds (r.removeFromRight (136)); r.removeFromRight (4); folderEditor.setBounds (r); }
    y += 6;
    int yLeft = y;
    if (twoCols) { x0 = colW; y = 4; }                         // the tags go in a second column when the window is wide
    albumCaption.setBounds (row (20));
    { int used = 0; layoutFieldGrid (albumFields, juce::Rectangle<int> (x0 + 6, y, colW - 12, 400), 1, 110, used); y += used + 6; }
    fileCaption.setBounds (row (20));
    { int used = 0; layoutFieldGrid (fileFields, juce::Rectangle<int> (x0 + 6, y, colW - 12, 400), 1, 110, used); y += used + 6; }
    vmHolder.setSize (w, juce::jmax (yLeft, y) + 6);
}

void MasteringComponent::layoutDdp (juce::Rectangle<int> area)
{
    // disc fields along the top (two rows of three)
    discCaption.setBounds (area.removeFromTop (20));
    { const int cols = area.getWidth() >= 1180 ? 6 : 3; int used = 0; layoutFieldGrid (discFields, area.removeFromTop (cols == 6 ? 30 : 60), cols, cols == 6 ? 78 : 90, used); }
    area.removeFromTop (6);

    // how tall each band is: when the window is short the timeline and the bottom block give way first, so the buttons never disappear
    const int transportH = grid::btnH + 6;
    int timelineH = 210, bottomH = 124;
    const int middleWanted = 210;
    int spare = area.getHeight() - (timelineH + 4 + transportH + 4 + bottomH + 6 + middleWanted);
    if (spare < 0) { const int cut = juce::jmin (-spare, timelineH - 130); timelineH -= cut; spare += cut; }
    if (spare < 0) { const int cut = juce::jmin (-spare, bottomH - 112); bottomH -= cut; spare += cut; }

    // the timeline: the full width of the window
    timeline->setBounds (area.removeFromTop (timelineH));
    area.removeFromTop (4);

    // the transport row: play, the gap, the playhead's time, Auto PQ and the zoom buttons (all in one place that never scrolls away)
    {
        auto tb = area.removeFromTop (transportH);
        playBtn.setBounds (tb.removeFromLeft (100).reduced (2, 1)); tb.removeFromLeft (4);
        gapPlayBtn.setBounds (tb.removeFromLeft (130).reduced (2, 1)); tb.removeFromLeft (4);
        startBtn.setBounds (tb.removeFromLeft (120).reduced (2, 1)); tb.removeFromLeft (14);
        zoomIn.setBounds (tb.removeFromRight (40).reduced (2, 1)); zoomOut.setBounds (tb.removeFromRight (40).reduced (2, 1)); zoomFit.setBounds (tb.removeFromRight (50).reduced (2, 1));
        tb.removeFromRight (12);
        autoPq.setBounds (tb.removeFromRight (96).reduced (2, 1)); tb.removeFromRight (4);
        isrcBtn.setBounds (tb.removeFromRight (84).reduced (2, 1)); tb.removeFromRight (4);
        allGapsBox.setBounds (tb.removeFromRight (210).reduced (2, 1));
        allGapsCaption.setBounds (0, 0, 0, 0);
        timeLabel.setBounds (tb);
    }
    area.removeFromTop (4);

    // the bottom: warnings, options, folder, progress, buttons
    auto bottom = area.removeFromBottom (bottomH);
    area.removeFromBottom (6);
    {
        // one row of options (level, zip, cue) with the folder beside it, then the messages, then the progress bar with the buttons
        auto r1 = bottom.removeFromTop (grid::btnH + 6);
        ddpNorm.setBounds (r1.removeFromLeft (170)); ddpPeakEditor.setBounds (r1.removeFromLeft (64)); ddpPeakUnit.setBounds (r1.removeFromLeft (54).withTrimmedLeft (6));
        r1.removeFromLeft (10); ddpTogether.setBounds (r1.removeFromLeft (210));
        zipToggle.setBounds (r1.removeFromLeft (130)); cueToggle.setBounds (r1.removeFromLeft (150));
        r1.removeFromLeft (10);
        ddpFolderCaption.setBounds (r1.removeFromLeft (60)); ddpChoose.setBounds (r1.removeFromRight (136)); r1.removeFromRight (4); ddpFolderEditor.setBounds (r1.reduced (0, 1));
        bottom.removeFromTop (4);
        auto btn = bottom.removeFromBottom (32);
        ddpExport.setBounds (btn.removeFromRight (140).reduced (2)); ddpCheck.setBounds (btn.removeFromRight (130).reduced (2)); ddpShow.setBounds (btn.removeFromRight (150).reduced (2));
        btn.removeFromRight (8);
        ddpProgress.setBounds (btn.withSizeKeepingCentre (btn.getWidth(), 16));
        bottom.removeFromBottom (4);
        auto wr = bottom.removeFromLeft (bottom.getWidth() / 2);
        warnings.setBounds (wr); ddpStatus.setBounds (bottom.reduced (6, 0));
    }

    // the middle row: the list of tracks, the PQ list, the selected track
    auto left = area.removeFromLeft (260);
    area.removeFromLeft (10);
    auto lbtn = left.removeFromBottom (32); left.removeFromBottom (6);
    ddpList->setBounds (left);
    ddpUp.setBounds (lbtn.removeFromLeft (125).reduced (1, 3)); ddpDown.setBounds (lbtn.removeFromLeft (125).reduced (1, 3));

    rightView.setBounds (area.removeFromRight (320));
    area.removeFromRight (10);
    {
        const int w = juce::jmax (200, rightView.getMaximumVisibleWidth());
        int y = 0;
        clipCaption.setBounds (0, y, w, 20); y += 22;
        { int used = 0; layoutFieldGrid (clipFields, juce::Rectangle<int> (0, y, w, (int) clipFields.size() * 30), 1, 84, used); y += used + 4; }
        { juce::Rectangle<int> r (0, y, w, grid::btnH + 4); gapCaption.setBounds (r.removeFromLeft (170)); gapEditor.setBounds (r.removeFromLeft (70).withTrimmedBottom (2)); y += grid::btnH + 8; }
        { juce::Rectangle<int> r (0, y, w, 26); preToggle.setBounds (r.removeFromLeft (130)); copyToggle.setBounds (r); y += 30; }
        rightHolder.setSize (w, y + 4);
    }

    pqCaption.setBounds (area.removeFromTop (20));
    pqView.setBounds (area);
    pqTable->setSize (juce::jmax (300, pqView.getMaximumVisibleWidth()), pqTable->getHeight());
    if (! fitted && timeline->getWidth() > 200) { fitted = true; timeline->fit(); }
}

// ----------------------------------------------------------------------------- the playhead and the audition
std::vector<AppContext::DiscPlayItem> MasteringComponent::discItems() const
{
    std::vector<AppContext::DiscPlayItem> items;
    const auto L = computePq (const_cast<MasteringComponent*> (this)->def().ddp, clipFrames());
    for (auto& t : L.tracks) items.push_back ({ t.editId, (double) t.audioStart / 75.0 });
    return items;
}

void MasteringComponent::setPlayhead (double seconds, bool fromUser)
{
    playheadSec = juce::jmax (0.0, seconds);
    timeline->setPlayhead (playheadSec);
    updateTimeLabel();
    if (fromUser && discPlaying) startPlay (playheadSec, -1.0);                       // clicking while it plays: carry on from there
}

void MasteringComponent::updateTimeLabel()
{
    const int sector = (int) std::floor (playheadSec * 75.0);
    juce::String where;
    const auto L = computePq (def().ddp, clipFrames());
    if (L.tracks.empty()) where = "no tracks";
    else if (sector >= L.leadOut) where = "after the end of the disc";
    else
    {
        where = "lead-in";
        for (auto& t : L.tracks)
        {
            if (sector >= t.audioStart && sector < t.endSector) { where = "track " + juce::String (t.number) + "  +" + sectorsToMsf (sector - t.audioStart); break; }
            if (sector < t.audioStart) { where = "pause before track " + juce::String (t.number) + "  (" + juce::String ((double) (t.audioStart - sector) / 75.0, 2) + " s to go)"; break; }
        }
    }
    timeLabel.setText ("Playhead  " + sectorsToMsf (sector) + "   -   " + where + (discPlaying ? "     (playing)" : ""), juce::dontSendNotification);
}

void MasteringComponent::startPlay (double fromSec, double toSec)
{
    if (app.engine.isRecording()) { showError ("Mastering", "Stop recording first."); return; }
    const bool continuing = discPlaying;                                        // (clicking in the timeline while it plays: the renders were checked when it started)
    if (discPlaying) { app.stopPlayback(); discPlaying = false; }
    if (job != nullptr) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText ("Wait for the export to finish before playing.", juce::dontSendNotification); return; }
    // the disc is played from the renders (what you hear is what the export contains): look at them first, and wait if some have to be made
    pendingPlay.on = false;
    if (renderJob != nullptr || (! continuing && ! checkRenders()))
    {
        if (renderJob == nullptr) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText (renderStatus.getText(), juce::dontSendNotification); }
        if (renderJob != nullptr) { pendingPlay = { true, fromSec, toSec }; ddpStatus.setColour (juce::Label::textColourId, theme::text); ddpStatus.setText ("Rendering... playback starts when the renders are ready.", juce::dontSendNotification); }
        return;
    }
    std::vector<AppContext::RenderPlayItem> items;
    for (auto& di : discItems())
    {
        auto r = renders.find (di.editId.toString());
        if (r == renders.end() || ! r->second.file.existsAsFile()) continue;
        items.push_back ({ r->second.file, di.startSeconds, r->second.rate, r->second.frames });
    }
    const auto err = app.playRenders (items, fromSec, toSec, playToken, previewOutFirst());
    if (err.isNotEmpty()) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText (err, juce::dontSendNotification); return; }
    playStartSec = fromSec; playheadSec = fromSec; discPlaying = true;
    playBtn.setButtonText ("Stop");
    updateTimeLabel();
}

void MasteringComponent::stopPlay()
{
    if (! discPlaying) return;
    const bool mine = app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Other && app.playInfo.id == playToken;
    const double pos = mine ? app.playheadSeconds() : -1.0;
    if (mine) app.stopPlayback();
    discPlaying = false;
    playheadSec = app.playheadFollows && pos >= 0.0 ? pos : playStartSec;
    playBtn.setButtonText ("Play");
    timeline->setPlayhead (playheadSec);
    updateTimeLabel();
}

void MasteringComponent::togglePlay()
{
    if (discPlaying) stopPlay(); else startPlay (playheadSec, -1.0);
}

void MasteringComponent::playGap()
{
    for (auto& t : computePq (def().ddp, clipFrames()).tracks)
        if (t.editId == def().selectedClip)
        {
            const double gapStart = (double) juce::jmin (t.audioStart, t.audioStart - t.gapSectors) / 75.0;
            const double from = juce::jmax (0.0, gapStart - 3.0), to = (double) t.audioStart / 75.0 + 3.0;
            timeline->showSeconds (from);
            startPlay (from, to);
            return;
        }
    ddpStatus.setColour (juce::Label::textColourId, theme::warn);
    ddpStatus.setText ("Click a track first: the gap before it is what is played.", juce::dontSendNotification);
}

void MasteringComponent::autoPqInPlace()
{
    bool first = true, moved = false;
    int flagsPutBack = 0;
    for (auto& c : def().ddp.clips)                                                          // every press starts again: the PQ flags go back to the audio, wherever they were dragged
    {
        if (c.index01Shift != 0) ++flagsPutBack;
        if (c.index00Shift != kIndexAuto) ++flagsPutBack;
        c.index01Shift = 0; c.index00Shift = kIndexAuto;
    }
    if (def().ddp.endPadSectors != 0) ++flagsPutBack;
    def().ddp.endPadSectors = 0;                                                             // ... and so does the End of CD flag
    for (auto& c : def().ddp.clips)
    {
        if (! c.include) continue;
        if (first && c.gapSectors < 150) { c.gapSectors = 150; moved = true; }               // the only thing the Red Book insists on: the first track starts at 2 s or later
        first = false;
    }
    touched(); refreshDdp (false);
    timeline->repaint();
    const auto L = computePq (def().ddp, clipFrames());
    int overlaps = 0; for (auto& t : L.tracks) if (t.number > 1 && t.gapSectors < 0) ++overlaps;
    ddpStatus.setColour (juce::Label::textColourId, theme::text);
    ddpStatus.setText ("Auto PQ: every track now starts (INDEX 01) where its audio starts and ends where its audio ends, with the tracks left exactly where they are. "
                       + juce::String ((int) L.tracks.size()) + " track(s), end of CD at " + sectorsToMsf (L.leadOut) + ". "
                       + (flagsPutBack > 0 ? juce::String (flagsPutBack) + " flag(s) that had been moved were put back. " : juce::String ("The flags were already in place. "))
                       + (overlaps > 0 ? juce::String (overlaps) + " overlap(s): the track after starts under the end of the one before. " : juce::String())
                       + (moved ? "Only the first track was moved, to start at 2 s." : "No track was moved."), juce::dontSendNotification);
    juce::Logger::writeToLog ("Auto PQ (keep positions): " + juce::String ((int) L.tracks.size()) + " tracks, " + juce::String (flagsPutBack) + " flags put back, " + juce::String (overlaps) + " overlaps, end " + sectorsToMsf (L.leadOut));
}

/** The ISRC tool: country, registrant and year codes, and the number of the first track; the rest follow (+1 each, or all the same). Also puts them on the matching Virtual Master files. */
void MasteringComponent::openIsrcTool()
{
    auto& clips = def().ddp.clips;
    const auto frames = clipFrames();
    std::vector<size_t> order;                                                    // the tracks that are on the disc, in disc order
    for (size_t i = 0; i < clips.size(); ++i) if (clips[i].include && i < frames.size() && frames[i] > 0) order.push_back (i);
    if (order.empty()) { ddpStatus.setColour (juce::Label::textColourId, theme::warn); ddpStatus.setText ("There are no tracks on the disc to give ISRCs to.", juce::dontSendNotification); return; }
    juce::String cc, reg, yy, num;
    const auto first = compactIsrc (clips[order.front()].isrc);
    if (first.length() == 12) { cc = first.substring (0, 2); reg = first.substring (2, 5); yy = first.substring (5, 7); num = first.substring (7); }
    else { yy = juce::String (juce::Time::getCurrentTime().getYear() % 100).paddedLeft ('0', 2); }
    auto* aw = new juce::AlertWindow ("ISRCs for the tracks",
                                      "An ISRC looks like GB-XXX-YY-NNNNN: country, label (registrant), year, then the track's own number. Type the first track's number and the other " + juce::String ((int) order.size() - 1) + " track(s) follow in disc order.",
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("cc", cc, "Country code (2 letters, e.g. GB)");
    aw->addTextEditor ("reg", reg, "Registrant / label code (3 letters or numbers)");
    aw->addTextEditor ("yy", yy, "Year code (2 digits)");
    aw->addTextEditor ("num", num, "Number of the FIRST track (5 digits, e.g. 00121)");
    aw->addComboBox ("mode", { "Sequential: each track after the first is one higher", "The same number on every track" }, "Numbering");
    aw->addButton ("Fill in the tracks", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::AlertWindow> awp (aw);
    juce::Component::SafePointer<MasteringComponent> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([awp, safe, order] (int r)
    {
        if (r != 1 || awp == nullptr || safe == nullptr) return;
        auto cc = awp->getTextEditorContents ("cc").trim().toUpperCase(), reg = awp->getTextEditorContents ("reg").trim().toUpperCase();
        auto yy = awp->getTextEditorContents ("yy").trim(), num = awp->getTextEditorContents ("num").trim();
        const bool sequential = awp->getComboBoxComponent ("mode")->getSelectedItemIndex() == 0;
        auto fail = [&] (const juce::String& why) { safe->ddpStatus.setColour (juce::Label::textColourId, theme::warn); safe->ddpStatus.setText ("ISRCs not changed: " + why, juce::dontSendNotification); };
        const auto allOf = [] (const juce::String& t, auto pred) { for (auto ch : t) if (! pred (ch)) return false; return true; };
        const auto isLetter = [] (juce::juce_wchar c) { return c >= 'A' && c <= 'Z'; };
        const auto isDigit = [] (juce::juce_wchar c) { return c >= '0' && c <= '9'; };
        if (cc.length() != 2 || ! allOf (cc, isLetter)) { fail ("the country code is two letters (for example GB)."); return; }
        if (reg.length() != 3 || ! allOf (reg, [&] (juce::juce_wchar c) { return isLetter (c) || isDigit (c); })) { fail ("the registrant (label) code is three letters or numbers."); return; }
        if (yy.length() != 2 || ! allOf (yy, isDigit)) { fail ("the year code is two digits (for example 25)."); return; }
        if (num.length() == 0 || num.length() > 5 || ! allOf (num, isDigit)) { fail ("the number of the first track is up to five digits (for example 00121)."); return; }
        const int start = num.getIntValue();
        if (sequential && start + (int) order.size() - 1 > 99999) { fail ("the numbers would run past 99999."); return; }
        auto& clips = safe->def().ddp.clips;
        juce::String firstCode, lastCode;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const auto code = cc + "-" + reg + "-" + yy + "-" + juce::String (start + (sequential ? (int) k : 0)).paddedLeft ('0', 5);
            clips[order[k]].isrc = code;
            if (auto* item = safe->def().findItem (clips[order[k]].editId)) item->tags.isrc = code;       // the Virtual Master file of the same edit gets it too
            if (k == 0) firstCode = code;
            lastCode = code;
        }
        safe->touched(); safe->refreshDdp (true);
        safe->ddpStatus.setColour (juce::Label::textColourId, theme::text);
        safe->ddpStatus.setText ("ISRCs filled in for " + juce::String ((int) order.size()) + " track(s): " + firstCode + (order.size() > 1 ? "  to  " + lastCode : juce::String()) + ".", juce::dontSendNotification);
    }));
}

void MasteringComponent::centrePlayhead()
{
    timeline->centreOn (playheadSec);
}

void MasteringComponent::zoomKey (bool in)
{
    timeline->zoomCentred (in ? 1.35 : 1.0 / 1.35, playheadSec);
}

bool MasteringComponent::keyPressed (const juce::KeyPress& k)
{
    if (view != 1) return false;
    if (k.getModifiers().isAnyModifierKeyDown()) return false;
    if (k == juce::KeyPress::rightKey) { zoomKey (true); return true; }                 // Right: zoom in on the playhead
    if (k == juce::KeyPress::leftKey)  { zoomKey (false); return true; }                // Left: zoom out
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (k == juce::KeyPress ('c') || k == juce::KeyPress ('C')) { centrePlayhead(); return true; }
    if (k == juce::KeyPress::homeKey)  { setPlayhead (0.0, true); timeline->showSeconds (0.0); return true; }
    return false;
}

// ----------------------------------------------------------------------------- the renders
std::vector<juce::Uuid> MasteringComponent::includedEdits() const
{
    std::vector<juce::Uuid> ids;
    auto add = [&] (const juce::Uuid& id)
    {
        auto* e = editOf (id);
        if (e != nullptr && ! e->isEmpty() && std::find (ids.begin(), ids.end(), id) == ids.end()) ids.push_back (id);
    };
    for (auto& it : def().items) if (it.include) add (it.editId);
    for (auto& c : def().ddp.clips) if (c.include) add (c.editId);
    return ids;
}

bool MasteringComponent::checkRenders (bool force)
{
    if (job != nullptr || renderJob != nullptr) { scheduleRecheck (500); return false; }
    if (app.engine.isRecording()) return false;
    RenderParams rp;
    const auto perr = makeRenderParams (app.project, def(), rp);
    const auto ids = includedEdits();
    renders.clear();
    if (perr.isNotEmpty() || ids.empty())
    {
        renderStatus.setColour (juce::Label::textColourId, perr.isNotEmpty() ? theme::warn : theme::dimText);
        renderStatus.setText (perr.isNotEmpty() ? perr : juce::String ("Tick the pieces to render: each one is rendered through its own mixer."), juce::dontSendNotification);
        updateRenderButtons(); refreshVm(); refreshDdp (false);
        pendingPlay.on = false;
        return false;
    }
    auto batch = std::make_unique<MasterRenderBatch> (app.project, ids, rp, force);
    if (batch->getPrepareError().isNotEmpty())
    {
        renderStatus.setColour (juce::Label::textColourId, theme::warn); renderStatus.setText (batch->getPrepareError(), juce::dontSendNotification);
        pendingPlay.on = false;
        updateRenderButtons();
        return false;
    }
    auto fill = [&] (const MasterRenderBatch& b, bool afterRender)
    {
        for (int i = 0; i < b.total(); ++i)
        {
            RenderView v; v.file = b.fileFor (i); v.rate = b.rateFor (i); v.frames = b.framesFor (i); v.backup = b.hasBackup (i); v.previous = b.isPrevious (i);
            if (b.needsRender (i)) { v.text = afterRender ? "not rendered" : "rendering..."; v.bad = afterRender; }
            else v.text = v.previous ? "previous version" : "rendered";
            renders[b.editAt (i).toString()] = v;
        }
    };
    fill (*batch, false);
    if (batch->toRender() == 0)
    {
        renderProgressValue = 0.0;
        renderStatus.setColour (juce::Label::textColourId, theme::text);
        renderStatus.setText ("Renders are up to date (" + juce::String (batch->total()) + (batch->total() == 1 ? " piece)." : " pieces)."), juce::dontSendNotification);
        updateRenderButtons(); refreshVm(); refreshDdp (false);
        return true;
    }
    renderProgressValue = 0.0;
    renderStatus.setColour (juce::Label::textColourId, theme::text);
    renderStatus.setText ("Rendering " + juce::String (batch->toRender()) + " of " + juce::String (batch->total()) + " pieces through their mixers...", juce::dontSendNotification);
    updateRenderButtons(); refreshVm(); refreshDdp (false);
    juce::Component::SafePointer<MasteringComponent> safe (this);
    renderJob = std::make_unique<MasterRenderJob> (std::move (batch), [safe] (juce::String err) { if (safe != nullptr) safe->renderFinished (err); });
    return false;
}

void MasteringComponent::renderFinished (const juce::String& error)
{
    renders.clear();
    if (renderJob != nullptr)
    {
        const auto& b = renderJob->batchRef();
        for (int i = 0; i < b.total(); ++i)
        {
            RenderView v; v.file = b.fileFor (i); v.rate = b.rateFor (i); v.frames = b.framesFor (i); v.backup = b.hasBackup (i); v.previous = b.isPrevious (i);
            v.text = b.needsRender (i) || ! v.file.existsAsFile() ? "not rendered" : (v.previous ? "previous version" : "rendered");
            v.bad = v.text == "not rendered";
            renders[b.editAt (i).toString()] = v;
        }
    }
    renderJob.reset();
    renderProgressValue = error.isEmpty() ? 1.0 : 0.0;
    const bool ok = error.isEmpty();
    renderStatus.setColour (juce::Label::textColourId, ok ? theme::text : theme::warn);
    renderStatus.setText (ok ? juce::String ("Renders are up to date.") : (error == "Cancelled." ? juce::String ("Rendering was cancelled.") : "Rendering stopped: " + error), juce::dontSendNotification);
    updateRenderButtons(); refreshVm(); refreshDdp (false);
    if (pendingPlay.on) { const auto pp = pendingPlay; pendingPlay.on = false; if (ok) startPlay (pp.from, pp.to); }
    else if (ok) scheduleRecheck (400);                                  // (something may have changed while it was rendering)
}

void MasteringComponent::updateRenderButtons()
{
    const auto id = selectedPiece();
    auto it = renders.find (id.toString());
    const bool have = ! id.isNull() && it != renders.end() && renderJob == nullptr && job == nullptr;
    prevRenderBtn.setEnabled (have && (it->second.backup || it->second.previous));
    prevRenderBtn.setButtonText (have && it->second.previous ? "Use newest render" : "Use previous render");
    renderAgainBtn.setEnabled (renderJob == nullptr && job == nullptr);
}

int MasteringComponent::previewOutFirst() const
{
    auto& p = app.project;
    if (p.mixers.empty()) return 0;
    const auto& pm = *p.mixers.front();
    auto outOf = [&] (const juce::Uuid& busId) { for (auto& b : pm.busPool) if (b->busId == busId) return b->outFirst.load(); return -1; };
    if (p.kindOf (def().sourceId) == NodeKind::ExtBus) { const int f = outOf (def().sourceId); if (f >= 0) return f; }
    for (auto& b : p.buses) if (b.external) { const int f = outOf (b.id); if (f >= 0) return f; }
    return 0;
}

void MasteringComponent::pollPlayback()
{
    if (! discPlaying) return;
    const bool mine = app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Other && app.playInfo.id == playToken;
    if (mine && ! app.engine.playbackFinished())
    {
        playheadSec = app.playheadSeconds();
        timeline->setPlayhead (playheadSec, true);
        updateTimeLabel();
        return;
    }
    const double last = playheadSec;
    if (mine) app.stopPlayback();
    discPlaying = false;
    playheadSec = app.playheadFollows ? last : playStartSec;
    playBtn.setButtonText ("Play");
    timeline->setPlayhead (playheadSec);
    updateTimeLabel();
}
} // namespace td

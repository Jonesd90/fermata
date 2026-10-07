#include "TrimWindow.h"

namespace td
{
static constexpr int kRulerH = 22, kGap = 16, kHit = 7;

/** Draws the two waveforms and the crossfade, and handles all the mouse work. */
class TrimWaves : public juce::Component, private juce::Timer
{
public:
    explicit TrimWaves (TrimComponent& o) : owner (o) { startTimerHz (30); }

    void timerCallback() override { if (owner.isAuditioning() || wasPlaying) { wasPlaying = owner.isAuditioning(); repaint(); } }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wave);
        const auto rf = owner.refs();
        if (! rf.ok())
        {
            g.setColour (theme::warn); g.setFont (16.0f);
            g.drawText ("This join no longer exists (the edit changed).", getLocalBounds(), juce::Justification::centred);
            return;
        }
        // At the very start of an edit there is only a piece coming IN (B), at the very end only one going OUT (A).
        const bool hasA = rf.A != nullptr, hasB = rf.B != nullptr;
        const EditRegion& A = hasA ? *rf.A : *rf.B; const EditRegion& B = hasB ? *rf.B : *rf.A;
        const double rate = B.sampleRate;
        const int w = getWidth(), h = getHeight();
        const auto lanes = laneRects();
        const double viewSamples = owner.viewSeconds * rate;
        const double centre = (double) owner.centreSample;
        auto xAt = [&] (double t) { return (float) (w * 0.5 + (t - centre) * w / viewSamples); };
        const double J = hasB ? (double) B.startSample : (double) A.endSample();

        ensureCache (A, B, hasA, hasB, w, viewSamples);

        // ---- ruler: milliseconds relative to the join mark
        g.setColour (theme::ruler); g.fillRect (0, 0, w, kRulerH);
        double stepMs = 1000.0;
        for (double s : { 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0 }) if (s * 0.001 * w / owner.viewSeconds > 70.0) { stepMs = s; break; }
        const double leftMs = (centre - viewSamples * 0.5 - J) / rate * 1000.0, rightMs = (centre + viewSamples * 0.5 - J) / rate * 1000.0;
        g.setFont (11.0f);
        for (double ms = std::floor (leftMs / stepMs) * stepMs; ms <= rightMs + stepMs; ms += stepMs)
        {
            const float x = xAt (J + ms * 0.001 * rate);
            if (x < -60 || x > w + 60) continue;
            g.setColour (theme::grid); g.drawVerticalLine ((int) x, (float) kRulerH - 6, (float) h);
            g.setColour (theme::dimText); g.drawText ((ms > 0 ? "+" : "") + juce::String ((int) std::round (ms)) + " ms", (int) x + 3, 3, 90, 14, juce::Justification::left);
        }

        // ---- lanes
        if (hasA) drawLane (g, lanes.a, cacheA, A, centre, viewSamples, juce::Colour (0xffc4511a), "A  (out)   " + A.takeName);
        else      drawEmptyLane (g, lanes.a, "Nothing comes before this: it is the START of the edit (only the fade-in below can be trimmed)");
        if (hasB) drawLane (g, lanes.b, cacheB, B, centre, viewSamples, juce::Colour (0xff1f6f8b), "B  (in)   " + B.takeName);
        else      drawEmptyLane (g, lanes.b, "Nothing comes after this: it is the END of the edit (only the fade-out above can be trimmed)");

        // ---- crossfade windows and curves
        const juce::int64 ob = A.fadeOutBegin(), oe = A.fadeOutFinish(), ib = B.fadeInBegin(), ie = B.fadeInFinish();
        auto shade = [&] (juce::int64 a, juce::int64 b, juce::Rectangle<int> lane, juce::Colour c)
        {
            const float x0 = xAt ((double) a), x1 = xAt ((double) b);
            g.setColour (c);
            g.fillRect (juce::Rectangle<float> (x0, (float) lane.getY(), juce::jmax (2.0f, x1 - x0), (float) lane.getHeight()));
        };
        if (hasA) shade (ob, oe, lanes.a, juce::Colour (0x55e8891a));
        if (hasB) shade (ib, ie, lanes.b, juce::Colour (0x552aa0bf));

        auto curve = [&] (juce::int64 a, juce::int64 b, juce::Rectangle<int> lane, bool fadeIn, juce::Colour c)
        {
            const float x0 = xAt ((double) a), x1 = xAt ((double) b);
            g.setColour (c);
            if (b <= a)
            {
                g.drawVerticalLine ((int) x0, (float) lane.getY(), (float) lane.getBottom());     // a hard cut
                return;
            }
            juce::Path p;
            const int steps = 40;
            for (int i = 0; i <= steps; ++i)
            {
                const float x = x0 + (x1 - x0) * (float) i / steps;
                const float gain = EditRegion::fadeGainFor (A.curve, (float) i / steps, fadeIn);
                const float y = (float) lane.getBottom() - gain * (float) lane.getHeight();
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.strokePath (p, juce::PathStrokeType (2.0f));
        };
        if (hasA) curve (ob, oe, lanes.a, false, juce::Colour (0xffc2410c));
        if (hasB) curve (ib, ie, lanes.b, true,  juce::Colour (0xff0b5c78));

        // handles (the corners you can grab)
        auto handle = [&] (juce::int64 t, float y, juce::Colour c, bool diamond)
        {
            const float x = xAt ((double) t);
            g.setColour (c);
            if (diamond) { juce::Path d; d.addTriangle (x - 5, y, x, y - 5, x + 5, y); d.addTriangle (x - 5, y, x, y + 5, x + 5, y); g.fillPath (d); }
            else g.fillRect (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
            g.setColour (theme::wellDark); g.drawRect (x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.0f);
        };
        const float yA = (float) lanes.a.getY() + 8.0f, yB = (float) lanes.b.getBottom() - 8.0f, yM = (float) (lanes.a.getBottom() + lanes.b.getY()) * 0.5f;
        if (hasA) { handle (ob, yA, juce::Colour (0xffc2410c), false); handle (oe, yA, juce::Colour (0xffc2410c), false); }
        if (hasB) { handle (ib, yB, juce::Colour (0xff0b5c78), false); handle (ie, yB, juce::Colour (0xff0b5c78), false); }
        if (hasA && hasB) { handle (juce::jmin (ob, ib), yM, theme::text, true); handle (juce::jmax (oe, ie), yM, theme::text, true); }

        // ---- markers: A's out point, B's in point, and the join mark (the reference, it never moves by itself)
        const float ax = xAt ((double) A.endSample()), bx = xAt (hasB ? (double) B.startSample : J);
        g.setColour (juce::Colour (0xffc2410c));
        if (hasA) for (int y = lanes.a.getY(); y < lanes.a.getBottom(); y += 6) g.fillRect (ax - 1.0f, (float) y, 2.0f, 3.0f);
        g.setColour (juce::Colour (0xff1f6f8b));
        if (hasB) for (int y = lanes.b.getY(); y < lanes.b.getBottom(); y += 6) g.fillRect (bx - 1.0f, (float) y, 2.0f, 3.0f);
        g.setColour (theme::text);
        const float jx = xAt (J);
        g.fillRect (jx - 0.5f, (float) kRulerH, 1.0f, (float) (h - kRulerH));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (hasA && hasB ? "join" : hasB ? "start" : "end", (int) jx + 4, h - 16, 40, 14, juce::Justification::left);

        // ---- playhead while auditioning
        if (owner.isAuditioning())
        {
            const double ph = owner.getApp().playheadSeconds();
            if (ph >= 0)
            {
                g.setColour (theme::playhead);
                g.fillRect (xAt (ph * rate) - 1.0f, (float) kRulerH, 2.0f, (float) (h - kRulerH));
                wasPlaying = true;
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* ed = owner.edit();
        const int k = owner.joinIndex();
        mode = Mode::None;
        if (ed == nullptr || k < 0) return;
        mode = hit (e.getPosition());
        if (mode == Mode::None)
        {
            const auto lanes = laneRects();                          // the dark band between the two waveforms: drag to look at another part (nothing is edited)
            if (e.y >= lanes.a.getBottom() && e.y < lanes.b.getY()) { mode = Mode::Pan; panBase = owner.centreSample; }
            return;
        }
        regions0 = ed->regions;
        owner.getApp().stopPlayback();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* ed = owner.edit();
        const int k = owner.joinIndex();
        if (ed != nullptr && k >= 0 && mode == Mode::Pan)
        {
            const double rt = ed->sampleRate > 0 ? ed->sampleRate : 48000.0;
            const double dS = e.getDistanceFromDragStartX() * owner.viewSeconds * rt / juce::jmax (1, getWidth());
            owner.centreSample = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, ed->lengthSamples()), panBase - (juce::int64) std::llround (dS));   // drag right: the view moves left
            owner.viewMoved();
            return;
        }
        if (ed == nullptr || k < 0 || mode == Mode::None || regions0.size() != ed->regions.size()) return;
        const double rate = ed->sampleRate > 0 ? ed->sampleRate : 48000.0;
        const bool hasA = k >= 1, hasB = k < (int) ed->regions.size();
        if ((! hasA || ! hasB) && (mode == Mode::SlipA || mode == Mode::SlipB || mode == Mode::MidStart || mode == Mode::MidEnd || mode == Mode::MidBody)) return;
        const double dSamples = e.getDistanceFromDragStartX() * owner.viewSeconds * rate / juce::jmax (1, getWidth());
        const double dSec = dSamples / rate;

        if (mode == Mode::SlipA || mode == Mode::SlipB)
        {
            EditDef tmp; tmp.sampleRate = ed->sampleRate; tmp.regions = regions0;
            if (mode == Mode::SlipA) tmp.slipOut (k, (juce::int64) std::llround (dSamples), owner.slipLeft());
            else                     tmp.slipIn  (k, (juce::int64) std::llround (dSamples), owner.slipRight());
            ed->regions = tmp.regions;
        }
        else
        {
            auto& A = ed->regions[(size_t) (hasA ? k - 1 : k)]; auto& B = ed->regions[(size_t) (hasB ? k : k - 1)];
            const auto& A0 = regions0[(size_t) (hasA ? k - 1 : k)]; const auto& B0 = regions0[(size_t) (hasB ? k : k - 1)];
            auto start = [] (double& s, double& en, double s0, double e0, double d) { s = juce::jmin (s0 + d, e0); en = e0; };
            auto finish = [] (double& s, double& en, double s0, double e0, double d) { s = s0; en = juce::jmax (e0 + d, s0); };
            auto body = [] (double& s, double& en, double s0, double e0, double d) { s = s0 + d; en = e0 + d; };
            switch (mode)
            {
                case Mode::OutStart: start  (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); break;
                case Mode::OutEnd:   finish (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); break;
                case Mode::OutBody:  body   (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); break;
                case Mode::InStart:  start  (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                case Mode::InEnd:    finish (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                case Mode::InBody:   body   (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                case Mode::MidStart: start  (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); start  (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                case Mode::MidEnd:   finish (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); finish (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                case Mode::MidBody:  body   (A.outStart, A.outEnd, A0.outStart, A0.outEnd, dSec); body   (B.inStart, B.inEnd, B0.inStart, B0.inEnd, dSec); break;
                default: break;
            }
            if (hasA) ed->clampFades (k - 1);
            if (hasB) ed->clampFades (k);
        }
        owner.dragChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (mode != Mode::None && mode != Mode::Pan) owner.dragFinished();
        mode = Mode::None;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto lanes = laneRects();
        if (hit (e.getPosition()) == Mode::None && e.y >= lanes.a.getBottom() && e.y < lanes.b.getY()) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); return; }
        switch (hit (e.getPosition()))
        {
            case Mode::SlipA: case Mode::SlipB:                       setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
            case Mode::OutStart: case Mode::OutEnd: case Mode::InStart: case Mode::InEnd: case Mode::MidStart: case Mode::MidEnd:
                                                                       setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
            case Mode::OutBody: case Mode::InBody: case Mode::MidBody: setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
            default:                                                   setMouseCursor (juce::MouseCursor::NormalCursor); break;
        }
    }

    void invalidate() { cacheKey = {}; repaint(); }
    float extraZoom = 1.0f;                                  // the . and , keys

private:
    enum class Mode { None, Pan, SlipA, SlipB, OutStart, OutEnd, OutBody, InStart, InEnd, InBody, MidStart, MidEnd, MidBody };
    struct Lanes { juce::Rectangle<int> a, b; };
    struct Cache { std::vector<float> lo, hi; };
    struct Key { int flags = 0; int w = -1; double view = 0; juce::int64 t0 = 0, aStart = 0, aIn = 0, bStart = 0, bIn = 0; int stamp = -1;
                 bool operator== (const Key& o) const { return flags == o.flags && w == o.w && view == o.view && t0 == o.t0 && aStart == o.aStart && aIn == o.aIn && bStart == o.bStart && bIn == o.bIn && stamp == o.stamp; } };

    Lanes laneRects() const
    {
        const int laneH = (getHeight() - kRulerH - kGap) / 2;
        return { juce::Rectangle<int> (0, kRulerH, getWidth(), laneH), juce::Rectangle<int> (0, kRulerH + laneH + kGap, getWidth(), laneH) };
    }

    /** What would a mouse press at p grab? */
    Mode hit (juce::Point<int> p) const
    {
        const auto rf = owner.refs();
        if (! rf.ok()) return Mode::None;
        const bool hasA = rf.A != nullptr, hasB = rf.B != nullptr;
        const EditRegion& A = hasA ? *rf.A : *rf.B; const EditRegion& B = hasB ? *rf.B : *rf.A;
        const double rate = B.sampleRate, viewSamples = owner.viewSeconds * rate, centre = (double) owner.centreSample;
        auto xAt = [&] (double t) { return (float) (getWidth() * 0.5 + (t - centre) * getWidth() / viewSamples); };
        const auto lanes = laneRects();
        const float ob = xAt ((double) A.fadeOutBegin()), oe = xAt ((double) A.fadeOutFinish()), ib = xAt ((double) B.fadeInBegin()), ie = xAt ((double) B.fadeInFinish());
        const float x = (float) p.x;
        auto near = [&] (float a) { return std::abs (x - a) <= kHit; };
        auto inside = [&] (float a, float b) { return x >= juce::jmin (a, b) - kHit && x <= juce::jmax (a, b) + kHit; };

        const int yA = lanes.a.getCentreY(), yB = lanes.b.getCentreY();
        if (p.y < lanes.a.getY() || p.y >= lanes.b.getBottom()) return Mode::None;

        if (! hasA && p.y < lanes.b.getY()) return Mode::None;         // at the start of the edit only the in-fade (lower lane) can be grabbed
        if (! hasB && p.y >= lanes.a.getBottom()) return Mode::None;   // at the end only the out-fade (upper lane)
        if (p.y < yA || (! hasB))                                      // top half of A: the out-fade on its own
        {
            if (! hasA) return Mode::None;
            if (oe - ob > 1.0f) { if (near (ob)) return Mode::OutStart; if (near (oe)) return Mode::OutEnd; if (x > ob && x < oe) return Mode::OutBody; }
            else if (near (ob)) return Mode::OutBody;
        }
        else if (p.y >= yB || ! hasA)                                  // bottom half of B: the in-fade on its own
        {
            if (ie - ib > 1.0f) { if (near (ib)) return Mode::InStart; if (near (ie)) return Mode::InEnd; if (x > ib && x < ie) return Mode::InBody; }
            else if (near (ib)) return Mode::InBody;
        }
        else if (hasA && hasB)                                         // in between: both fades together
        {
            const float s0 = juce::jmin (ob, ib), s1 = juce::jmax (ob, ib), e0 = juce::jmin (oe, ie), e1 = juce::jmax (oe, ie);
            if (near (ob) || near (ib)) return Mode::MidStart;
            if (near (oe) || near (ie)) return Mode::MidEnd;
            if (inside (s0, e1) && x > s0 && x < e1 && (p.y >= lanes.a.getBottom() - 6 || p.y <= lanes.b.getY() + 6 || (p.y > lanes.a.getBottom() && p.y < lanes.b.getY())))
                return Mode::MidBody;
            juce::ignoreUnused (s1, e0);
        }
        // not on a fade handle: dragging a waveform slides that audio under the join
        if (hasA && hasB && lanes.a.contains (p)) return Mode::SlipA;
        if (hasA && hasB && lanes.b.contains (p)) return Mode::SlipB;
        return Mode::None;
    }

    static void readMono (juce::AudioFormatReader* r, juce::int64 start, int n, std::vector<float>& out)
    {
        out.assign ((size_t) n, 0.0f);
        if (r == nullptr || n <= 0) return;
        const juce::int64 s = juce::jmax ((juce::int64) 0, start), e2 = juce::jmin (r->lengthInSamples, start + n);
        if (e2 <= s) return;
        float* chans[1] = { out.data() + (s - start) };
        r->read (chans, 1, s, (int) (e2 - s));
    }

    void ensureCache (const EditRegion& A, const EditRegion& B, bool hasA, bool hasB, int w, double viewSamples)
    {
        Key key; key.flags = (hasA ? 1 : 0) + (hasB ? 2 : 0);
        key.w = w; key.view = viewSamples; key.t0 = (juce::int64) std::floor ((double) owner.centreSample - viewSamples * 0.5);
        key.aStart = A.startSample; key.aIn = A.srcIn; key.bStart = B.startSample; key.bIn = B.srcIn; key.stamp = owner.readersStamp();
        if (key == cacheKey) return;
        cacheKey = key;
        const int n = juce::jlimit (16, 4000000, (int) viewSamples);
        std::vector<float> buf;
        auto build = [&] (juce::AudioFormatReader* r, const EditRegion& reg, juce::int64 fileStart, Cache& c)
        {
            readMono (r, key.t0 + (reg.srcIn - reg.startSample) - fileStart, n, buf);
            c.lo.assign ((size_t) juce::jmax (1, w), 0.0f); c.hi = c.lo;
            for (int x = 0; x < w; ++x)
            {
                const int i0 = (int) ((juce::int64) x * n / juce::jmax (1, w)), i1 = juce::jmax (i0 + 1, (int) ((juce::int64) (x + 1) * n / juce::jmax (1, w)));
                float lo = buf[(size_t) juce::jmin (i0, n - 1)], hi = lo;
                for (int i = i0; i < i1 && i < n; ++i) { lo = juce::jmin (lo, buf[(size_t) i]); hi = juce::jmax (hi, buf[(size_t) i]); }
                c.lo[(size_t) x] = lo; c.hi[(size_t) x] = hi;
            }
        };
        auto blank = [&] (Cache& c) { c.lo.assign ((size_t) juce::jmax (1, w), 0.0f); c.hi = c.lo; };
        if (hasA) build (owner.readerA(), A, owner.fileStartA(), cacheA); else blank (cacheA);
        if (hasB) build (owner.readerB(), B, owner.fileStartB(), cacheB); else blank (cacheB);
        float peak = 0.05f;
        for (auto* c : { &cacheA, &cacheB }) for (size_t i = 0; i < c->lo.size(); ++i) peak = juce::jmax (peak, std::abs (c->lo[i]), std::abs (c->hi[i]));
        scale = 0.92f / peak;
    }

    void drawLane (juce::Graphics& g, juce::Rectangle<int> lane, const Cache& c, const EditRegion& r, double centre, double viewSamples,
                   juce::Colour colour, const juce::String& title)
    {
        g.setColour (theme::waveAlt); g.fillRect (lane);
        juce::Graphics::ScopedSaveState keepInside (g);
        g.reduceClipRegion (lane);                                                   // an enlarged waveform stays in its own lane
        const float mid = (float) lane.getCentreY(), half = (float) lane.getHeight() * 0.5f - 3.0f;
        g.setColour (theme::grid.withAlpha (0.6f)); g.drawHorizontalLine ((int) mid, 0.0f, (float) lane.getWidth());
        const int w = lane.getWidth();
        const double t0 = centre - viewSamples * 0.5;
        for (int x = 0; x < w && (size_t) x < c.lo.size(); ++x)
        {
            const float lo = c.lo[(size_t) x] * scale * extraZoom * half, hi = c.hi[(size_t) x] * scale * extraZoom * half;
            g.setColour (colour.withAlpha (0.22f));                                  // the whole recording, faint
            g.drawVerticalLine (lane.getX() + x, mid - hi, mid - lo + 1.0f);
            const float gain = r.gainAt ((juce::int64) (t0 + (x + 0.5) * viewSamples / w));
            if (gain > 0.0f)                                                         // what you will hear, scaled by the fades
            {
                g.setColour (colour);
                g.drawVerticalLine (lane.getX() + x, mid - hi * gain, mid - lo * gain + 1.0f);
            }
        }
        g.setColour (theme::text.withAlpha (0.85f)); g.setFont (12.0f);
        g.drawText (title, lane.reduced (8, 3), juce::Justification::topLeft);
    }

    void drawEmptyLane (juce::Graphics& g, juce::Rectangle<int> lane, const juce::String& text)
    {
        g.setColour (theme::waveAlt.withAlpha (0.5f)); g.fillRect (lane);
        g.setColour (theme::dimText); g.setFont (13.0f);
        g.drawText (text, lane.reduced (10, 3), juce::Justification::centred);
    }

    TrimComponent& owner;
    Mode mode = Mode::None; juce::int64 panBase = 0;
    std::vector<EditRegion> regions0;
    Cache cacheA, cacheB; Key cacheKey; float scale = 1.0f;
    bool wasPlaying = false;
};

// ============================================================================
TrimComponent::TrimComponent (AppContext& a, const juce::Uuid& eid, const juce::Uuid& rid, bool atEnd) : app (a), editId (eid), regionBId (rid), endEdge (atEnd)
{
    formats.registerBasicFormats();
    waves.reset (new TrimWaves (*this));
    waves->extraZoom = app.waveZoom;
    addAndMakeVisible (*waves);
    setWantsKeyboardFocus (true);

    for (auto* l : { &infoLabel, &fadeLabel, &trackCaption, &curveCaption, &rollCaption }) addAndMakeVisible (l);
    infoLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    fadeLabel.setColour (juce::Label::textColourId, theme::warn);
    trackCaption.setText ("Track shown:", juce::dontSendNotification);
    curveCaption.setText ("Fade shape:", juce::dontSendNotification);
    rollCaption.setText ("Pre-roll / post-roll (s):", juce::dontSendNotification);

    for (int c = 0; c < (int) FadeCurve::Count; ++c) curveBox.addItem (fadeCurveName ((FadeCurve) c), c + 1);
    curveBox.onChange = [this]
    {
        if (updatingControls) return;
        const auto rf = refs();
        if (! rf.ok()) return;
        const auto c = (FadeCurve) juce::jlimit (0, (int) FadeCurve::Count - 1, curveBox.getSelectedId() - 1);
        if (rf.A != nullptr) rf.A->curve = c;
        if (rf.B != nullptr) rf.B->curve = c;
        app.project.changed();
    };
    trackBox.onChange = [this] { if (! updatingControls) { refreshReaders(); waves->invalidate(); } };

    for (auto* s : { &preSlider, &postSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        s->setRange (0.2, 8.0, 0.1);
    }
    preSlider.setValue (1.5, juce::dontSendNotification); postSlider.setValue (1.5, juce::dontSendNotification);

    auto makeGroup = [this] (Group& g, const juce::String& title, std::function<void (double)> fn)
    {
        g.label.setText (title, juce::dontSendNotification);
        addAndMakeVisible (g.label);
        for (double ms : { -10.0, -1.0, 1.0, 10.0 })
        {
            auto* b = g.buttons.add (new juce::TextButton ((ms > 0 ? "+" : "") + juce::String ((int) ms) + " ms"));
            b->onClick = [fn, ms] { fn (ms); };
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
    };
    makeGroup (gA, "Slide A (out audio):",   [this] (double ms) { slideA (ms); });
    makeGroup (gB, "Slide B (in audio):",    [this] (double ms) { slideB (ms); });
    makeGroup (gJ, "Move the join (cut):",   [this] (double ms) { moveJoinBy (ms); });

    slipLeftToggle.setTooltip ("Sliding A: every piece to the LEFT of A slides with it. Off: they stay where they are.");
    slipRightToggle.setTooltip ("Sliding B: every piece to the RIGHT of B slides with it. Off: they stay where they are.");

    zoomIn.onClick = [this] { zoomBy (1.0 / 1.5); };
    zoomOut.onClick = [this] { zoomBy (1.5); };
    centreButton.onClick = [this] { recentre(); };
    auditionButton.onClick = [this] { audition(); };
    origA.onClick = [this] { playOriginal (true); };
    origB.onClick = [this] { playOriginal (false); };
    stopButton.onClick = [this] { stopAll(); };
    resetFade.onClick = [this] { resetFades(); };
    acceptButton.onClick = [this] { accept(); };
    revertButton.onClick = [this] { revert(); };
    prevButton.onClick = [this] { step (-1); };
    nextButton.onClick = [this] { step (1); };
    prevButton.setTooltip ("Keep what you did on this fade and go to the fade on the LEFT (the previous join). Key: [");
    nextButton.setTooltip ("Keep what you did on this fade and go to the fade on the RIGHT (the next join), so you can work through the whole piece. Key: ]");
    copyOutIn.onClick = [this] { copyFade (true); };
    copyInOut.onClick = [this] { copyFade (false); };
    acceptButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
    auditionButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    for (auto* b : std::initializer_list<juce::Button*> { &zoomIn, &zoomOut, &centreButton, &resetFade, &acceptButton, &revertButton, &prevButton, &nextButton,
                                                          &auditionButton, &origA, &origB, &stopButton, &copyOutIn, &copyInOut, &loopToggle, &slipLeftToggle, &slipRightToggle })
    {
        addAndMakeVisible (b);
        b->setWantsKeyboardFocus (false);
    }
    addAndMakeVisible (trackBox); addAndMakeVisible (curveBox); addAndMakeVisible (preSlider); addAndMakeVisible (postSlider);
    trackBox.setWantsKeyboardFocus (false); curveBox.setWantsKeyboardFocus (false);
    preSlider.setWantsKeyboardFocus (false); postSlider.setWantsKeyboardFocus (false);

    takeSnapshot();
    recentre();
    refreshControls();
    refreshReaders();
    app.project.addChangeListener (this);
    startTimerHz (15);
    setSize (1100, 700);
}

TrimComponent::~TrimComponent()
{
    app.project.removeChangeListener (this);
    stopAll();
}

/** The join (0 = the start of the edit, n = its end) with the piece going out (A) and the piece coming in (B); either can be missing at the two ends. */
TrimComponent::Refs TrimComponent::refs() const
{
    Refs r; r.e = edit();
    if (r.e == nullptr) return r;
    const int n = (int) r.e->regions.size(), idx = r.e->indexOf (regionBId);
    if (idx < 0 || (endEdge && idx != n - 1)) return r;
    r.k = endEdge ? idx + 1 : idx;
    r.A = r.k >= 1 ? &r.e->regions[(size_t) r.k - 1] : nullptr;
    r.B = r.k < n  ? &r.e->regions[(size_t) r.k]     : nullptr;
    return r;
}

int TrimComponent::joinIndex() const { return refs().k; }

void TrimComponent::takeSnapshot()
{
    if (auto* e = edit()) snapshot = e->regions;
}

void TrimComponent::recentre()
{
    const auto rf = refs();
    if (rf.ok()) centreSample = rf.B != nullptr ? rf.B->startSample : rf.A->endSample();
    waves->invalidate();
}

void TrimComponent::zoomBy (double factor)
{
    viewSeconds = juce::jlimit (0.02, 12.0, viewSeconds * factor);
    waves->invalidate();
}

void TrimComponent::describe()
{
    const auto rf = refs();
    if (! rf.ok()) return;
    const bool hasA = rf.A != nullptr, hasB = rf.B != nullptr;
    const auto& R = hasB ? *rf.B : *rf.A;
    const double rate = R.sampleRate, J = hasB ? (double) rf.B->startSample : (double) rf.A->endSample();
    auto ms = [&] (juce::int64 t) { return juce::String (((double) t - J) / rate * 1000.0, 1); };
    const int n = (int) rf.e->regions.size();
    const juce::String where = formatTime (J / rate).substring (3, 12);
    if (hasA && hasB)
    {
        infoLabel.setText ("Edit point " + juce::String (rf.k + 1) + " of " + juce::String (n + 1) + ":   " + rf.A->takeName + "   ->   " + rf.B->takeName + "    at " + where, juce::dontSendNotification);
        fadeLabel.setText ("Out-fade " + ms (rf.A->fadeOutBegin()) + " .. " + ms (rf.A->fadeOutFinish()) + " ms     In-fade " + ms (rf.B->fadeInBegin()) + " .. " + ms (rf.B->fadeInFinish())
                           + " ms     (relative to the join mark)", juce::dontSendNotification);
    }
    else if (hasB)
    {
        infoLabel.setText ("Edit point 1 of " + juce::String (n + 1) + ":   the START of the edit, fading in " + rf.B->takeName + "    at " + where, juce::dontSendNotification);
        fadeLabel.setText ("In-fade " + ms (rf.B->fadeInBegin()) + " .. " + ms (rf.B->fadeInFinish()) + " ms     (relative to the start of the piece)", juce::dontSendNotification);
    }
    else
    {
        infoLabel.setText ("Edit point " + juce::String (n + 1) + " of " + juce::String (n + 1) + ":   the END of the edit, " + rf.A->takeName + " fading out    at " + where, juce::dontSendNotification);
        fadeLabel.setText ("Out-fade " + ms (rf.A->fadeOutBegin()) + " .. " + ms (rf.A->fadeOutFinish()) + " ms     (relative to the end of the piece)", juce::dontSendNotification);
    }
}

void TrimComponent::refreshControls()
{
    const auto rf = refs();
    updatingControls = true;
    if (rf.ok())
    {
        const bool hasA = rf.A != nullptr, hasB = rf.B != nullptr;
        describe();
        curveBox.setSelectedId ((int) (hasB ? rf.B->curve : rf.A->curve) + 1, juce::dontSendNotification);

        // tracks that exist in the pieces shown
        std::vector<int> ids;
        for (size_t ti = 0; ti < app.project.tracks.size(); ++ti)
        {
            bool inA = ! hasA, inB = ! hasB;
            if (hasA) for (auto& f : rf.A->files) inA = inA || f.trackId == app.project.tracks[ti].id;
            if (hasB) for (auto& f : rf.B->files) inB = inB || f.trackId == app.project.tracks[ti].id;
            if (inA && inB) ids.push_back ((int) ti + 1);
        }
        bool same = trackBox.getNumItems() == (int) ids.size();
        for (int i = 0; same && i < trackBox.getNumItems(); ++i) same = trackBox.getItemId (i) == ids[(size_t) i];
        if (! same)
        {
            const auto current = trackBox.getSelectedId();
            trackBox.clear (juce::dontSendNotification);
            for (int id : ids) trackBox.addItem (app.project.tracks[(size_t) id - 1].name, id);
            trackBox.setSelectedId (current > 0 && trackBox.indexOfItemId (current) >= 0 ? current : (ids.empty() ? 0 : ids.front()), juce::dontSendNotification);
        }
        prevButton.setEnabled (rf.k > 0);
        nextButton.setEnabled (rf.k < (int) rf.e->regions.size());
        const bool both = hasA && hasB;
        for (auto* b : std::initializer_list<juce::Component*> { &copyOutIn, &copyInOut, &origA, &origB, &slipLeftToggle, &slipRightToggle })
            b->setEnabled (both || b == &origA || b == &origB);
        origA.setEnabled (hasA); origB.setEnabled (hasB);
        for (auto* g : { &gA, &gB, &gJ })
            for (auto* b : g->buttons) b->setEnabled (both);
    }
    updatingControls = false;
}

void TrimComponent::refreshReaders()
{
    const auto rf = refs();
    rdA.reset(); rdB.reset(); fsA = fsB = 0;
    ++stamp;
    if (! rf.ok()) return;
    readersForA = rf.A != nullptr ? rf.A->id : juce::Uuid::null(); readersForB = rf.B != nullptr ? rf.B->id : juce::Uuid::null(); readersTrack = trackBox.getSelectedId();
    const int ti = readersTrack - 1;
    if (! juce::isPositiveAndBelow (ti, (int) app.project.tracks.size())) return;
    const auto tid = app.project.tracks[(size_t) ti].id;
    auto open = [&] (const EditRegion& r, juce::int64& fileStart) -> std::unique_ptr<juce::AudioFormatReader>
    {
        fileStart = 0;
        for (auto& f : r.files) if (f.trackId == tid) { fileStart = f.fileStart; return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (f.file)); }
        return nullptr;
    };
    if (rf.A != nullptr) rdA = open (*rf.A, fsA);
    if (rf.B != nullptr) rdB = open (*rf.B, fsB);
}

void TrimComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshControls();
    waves->extraZoom = app.waveZoom;
    const auto rf = refs();
    if (rf.ok() && ((rf.A != nullptr ? rf.A->id : juce::Uuid::null()) != readersForA || (rf.B != nullptr ? rf.B->id : juce::Uuid::null()) != readersForB || trackBox.getSelectedId() != readersTrack))
        refreshReaders();
    waves->invalidate();
}

void TrimComponent::viewMoved() { waves->invalidate(); }

void TrimComponent::dragChanged()
{
    app.project.markDirty();
    waves->repaint();
    describe();
}

void TrimComponent::dragFinished() { app.project.changed(); }

void TrimComponent::timerCallback()
{
    const bool playing = app.isPlaying();
    if (wasAuditioning && ! playing && loopToggle.getToggleState()) audition();    // loop
    wasAuditioning = wasAuditioning && playing;
    auditionButton.setButtonText (isAuditioning() ? "Stop  [F2]" : "Audition  [F2]");
}

// ------------------------------------------------------------------ editing actions
void TrimComponent::slideA (double ms)
{
    const auto rf = refs();
    if (! rf.ok() || rf.A == nullptr || rf.B == nullptr) return;
    auto* e = rf.e; const int k = rf.k;
    e->slipOut (k, (juce::int64) std::llround (ms * 0.001 * e->regions[(size_t) k].sampleRate), slipLeft());
    app.project.changed();
}

void TrimComponent::slideB (double ms)
{
    const auto rf = refs();
    if (! rf.ok() || rf.A == nullptr || rf.B == nullptr) return;
    auto* e = rf.e; const int k = rf.k;
    e->slipIn (k, (juce::int64) std::llround (ms * 0.001 * e->regions[(size_t) k].sampleRate), slipRight());
    app.project.changed();
}

void TrimComponent::moveJoinBy (double ms)
{
    const auto rf = refs();
    if (! rf.ok() || rf.A == nullptr || rf.B == nullptr) return;
    auto* e = rf.e; const int k = rf.k;
    e->moveJoin (k, (juce::int64) std::llround (ms * 0.001 * e->regions[(size_t) k].sampleRate));
    recentre();
    app.project.changed();
}

void TrimComponent::resetFades()
{
    const auto rf = refs();
    if (! rf.ok()) return;
    if (rf.A != nullptr && rf.B != nullptr) rf.e->setDefaultJoinFades (rf.k);
    else if (rf.B != nullptr) { rf.B->inStart = 0.0; rf.B->inEnd = kDefaultEdgeFade; rf.e->clampFades (rf.k); }              // the start of the edit: the default short fade-in
    else { rf.A->outStart = -kDefaultEdgeFade; rf.A->outEnd = 0.0; rf.e->clampFades (rf.k - 1); }                           // the end: the default short fade-out
    if (rf.A != nullptr) rf.A->curve = FadeCurve::EqualPower;
    if (rf.B != nullptr) rf.B->curve = FadeCurve::EqualPower;
    app.project.changed();
}

void TrimComponent::copyFade (bool outToIn)
{
    const auto rf = refs();
    if (! rf.ok() || rf.A == nullptr || rf.B == nullptr) return;
    auto* e = rf.e; const int k = rf.k;
    if (outToIn) e->copyOutToIn (k); else e->copyInToOut (k);
    app.project.changed();
}

void TrimComponent::audition()
{
    const auto rf = refs();
    if (! rf.ok()) return;
    auto* e = rf.e;
    if (isAuditioning()) { stopAll(); return; }
    const double rate = e->sampleRate;
    juce::int64 begin = std::numeric_limits<juce::int64>::max(), finish = 0;
    if (rf.A != nullptr) { begin = juce::jmin (begin, rf.A->fadeOutBegin()); finish = juce::jmax (finish, rf.A->fadeOutFinish()); }
    if (rf.B != nullptr) { begin = juce::jmin (begin, rf.B->fadeInBegin());  finish = juce::jmax (finish, rf.B->fadeInFinish()); }
    const double from = juce::jmax (0.0, ((double) begin - preRoll() * rate) / rate);
    const double to = juce::jmin (e->lengthSeconds(), ((double) finish + postRoll() * rate) / rate);
    app.stopPlayback();
    auto err = app.playEdit (editId, from, to);
    if (err.isEmpty()) app.playInfo.noFollow = true;          // auditioning a join must not move the edit window's playhead
    if (err.isNotEmpty()) { wasAuditioning = false; showError ("Audition", err); return; }
    wasAuditioning = true;
}

void TrimComponent::playOriginal (bool outgoing)
{
    const auto rf = refs();
    if (! rf.ok() || (outgoing ? rf.A : rf.B) == nullptr) return;
    const auto& r = outgoing ? *rf.A : *rf.B;
    const juce::int64 centre = outgoing ? r.srcOut : r.srcIn;
    app.stopPlayback();
    wasAuditioning = false;
    auto err = app.playRegionOriginal (editId, r.id, centre - (juce::int64) (preRoll() * r.sampleRate), centre + (juce::int64) (postRoll() * r.sampleRate));
    if (err.isNotEmpty()) showError ("Play", err);
}

void TrimComponent::stopAll() { wasAuditioning = false; app.stopPlayback(); }

void TrimComponent::accept()
{
    stopAll();
    takeSnapshot();
    app.project.changed();
    const auto eid = editId;
    if (app.closeTrim) app.closeTrim (eid);          // closes this window (the component is deleted later)
    if (app.showEdit) app.showEdit (eid);            // back to the edit window
}

void TrimComponent::revert()
{
    auto* e = edit();
    if (e == nullptr || snapshot.size() != e->regions.size()) return;
    stopAll();
    e->regions = snapshot;
    e->clampAll();
    app.project.changed();
}

void TrimComponent::gotoJoin (const juce::Uuid& rid, bool atEnd)
{
    auto* e = edit();
    if (e == nullptr || e->indexOf (rid) < 0) return;
    stopAll();
    takeSnapshot();
    regionBId = rid; endEdge = atEnd;
    recentre();
    refreshControls();
    refreshReaders();
    waves->invalidate();
}

void TrimComponent::step (int delta)
{
    const auto rf = refs();
    if (! rf.ok()) return;
    const int n = (int) rf.e->regions.size(), to = rf.k + delta;
    if (to < 0 || to > n) return;
    app.project.changed();                            // what was done on this join is accepted
    if (to == n) gotoJoin (rf.e->regions.back().id, true);
    else gotoJoin (rf.e->regions[(size_t) to].id, false);
}

bool TrimComponent::keyPressed (const juce::KeyPress& k)
{
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (k == juce::KeyPress::F2Key) { audition(); return true; }
    if (k == juce::KeyPress::spaceKey) { if (app.isPlaying()) { stopAll(); } return true; }      // Space stops whatever is playing (an audition or the original): ready to listen again
    if (k == juce::KeyPress::returnKey) { accept(); return true; }
    if (k == juce::KeyPress::rightKey) { zoomBy (1.0 / 1.25); return true; }      // right = zoom in
    if (k == juce::KeyPress::leftKey)  { zoomBy (1.25); return true; }            // left = zoom out
    if (c == '.' || c == '>') { app.changeWaveZoom (1.4f);        return true; }          // . bigger waveforms
    if (c == ',' || c == '<') { app.changeWaveZoom (1.0f / 1.4f); return true; }          // , smaller
    if (c == '[') { step (-1); return true; }                                     // accept, then the fade on the left
    if (c == ']') { step (1);  return true; }                                     // accept, then the fade on the right
    if (c == 'a') { copyFade (true);  return true; }
    if (c == 'z') { copyFade (false); return true; }
    return false;
}

void TrimComponent::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& d)
{
    zoomBy (d.deltaY > 0 ? 1.0 / 1.25 : 1.25);
}

void TrimComponent::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto head = r.removeFromTop (24);
    infoLabel.setBounds (head);
    fadeLabel.setBounds (r.removeFromTop (20));
    auto controls = r.removeFromBottom (232);
    waves->setBounds (r);

    auto row = [&controls] (int h) { auto x = controls.removeFromTop (h); controls.removeFromTop (3); return x; };

    auto r1 = row (28);
    trackCaption.setBounds (r1.removeFromLeft (85)); trackBox.setBounds (r1.removeFromLeft (150));
    r1.removeFromLeft (10);
    curveCaption.setBounds (r1.removeFromLeft (75)); curveBox.setBounds (r1.removeFromLeft (215));
    r1.removeFromLeft (10);
    zoomIn.setBounds (r1.removeFromLeft (75).reduced (2)); zoomOut.setBounds (r1.removeFromLeft (75).reduced (2));
    centreButton.setBounds (r1.removeFromLeft (115).reduced (2));
    resetFade.setBounds (r1.removeFromLeft (130).reduced (2));

    auto r2 = row (32);
    acceptButton.setBounds (r2.removeFromLeft (130).reduced (2));
    revertButton.setBounds (r2.removeFromLeft (125).reduced (2));
    r2.removeFromLeft (14);
    prevButton.setBounds (r2.removeFromLeft (200).reduced (2));
    nextButton.setBounds (r2.removeFromLeft (200).reduced (2));
    r2.removeFromLeft (14);
    slipLeftToggle.setBounds (r2.removeFromLeft (95));
    slipRightToggle.setBounds (r2.removeFromLeft (105));

    auto r3 = row (30);
    copyOutIn.setBounds (r3.removeFromLeft (190).reduced (2));
    copyInOut.setBounds (r3.removeFromLeft (190).reduced (2));

    for (auto* g : { &gA, &gB, &gJ })
    {
        auto rr = row (28);
        g->label.setBounds (rr.removeFromLeft (170));
        for (auto* b : g->buttons) b->setBounds (rr.removeFromLeft (75).reduced (2));
    }

    auto r6 = row (32);
    auditionButton.setBounds (r6.removeFromLeft (150).reduced (2));
    origA.setBounds (r6.removeFromLeft (135).reduced (2));
    origB.setBounds (r6.removeFromLeft (135).reduced (2));
    stopButton.setBounds (r6.removeFromLeft (65).reduced (2));
    loopToggle.setBounds (r6.removeFromLeft (70));
    rollCaption.setBounds (r6.removeFromLeft (160));
    preSlider.setBounds (r6.removeFromLeft (200));
    postSlider.setBounds (r6.removeFromLeft (200));
}
} // namespace td

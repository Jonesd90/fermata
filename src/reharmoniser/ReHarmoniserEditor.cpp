#include "ReHarmoniserEditor.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace rehar
{
namespace
{
const juce::Colour cBg (0xff15120e), cPanel (0xff241f18), cPanel2 (0xff2f281e), cAmber (0xffffb43c), cAmberDim (0xff9a6c1c), cText (0xffe9dcc3),
                   cLine (0xff4b402f), cGreen (0xff62d27e), cRed (0xffff5a4a), cKeyW (0xffe6dcc6), cKeyB (0xff2a251c);

juce::Font lcdFont (float h) { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain)); }
juce::Font uiFont (float h, bool bold = false) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }

juce::String fmtTime (double s)
{
    const int m = (int) (s / 60.0); const double r = s - m * 60.0;
    return m > 0 ? juce::String (m) + ":" + juce::String (r, 1).paddedLeft ('0', 4) : juce::String (r, 2) + " s";
}

const char* kNames[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
bool isBlackKey (int midi) { const int n = ((midi % 12) + 12) % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

void styleButton (juce::TextButton& b, juce::Colour c = cPanel2)
{
    b.setColour (juce::TextButton::buttonColourId, c); b.setColour (juce::TextButton::textColourOffId, cText); b.setColour (juce::TextButton::textColourOnId, cAmber);
    b.setWantsKeyboardFocus (false);
}
void styleLabel (juce::Label& l, juce::Colour c = cText, float size = 13.0f) { l.setColour (juce::Label::textColourId, c); l.setFont (uiFont (size)); l.setMinimumHorizontalScale (0.8f); }
void styleSlider (juce::Slider& s, double lo, double hi, double step, double v, const juce::String& suffix = {})
{
    s.setSliderStyle (juce::Slider::LinearHorizontal); s.setRange (lo, hi, step); s.setValue (v, juce::dontSendNotification); s.setScrollWheelEnabled (false);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20); s.setTextValueSuffix (suffix);
    s.setColour (juce::Slider::trackColourId, cAmberDim); s.setColour (juce::Slider::thumbColourId, cAmber); s.setColour (juce::Slider::textBoxTextColourId, cAmber);
    s.setColour (juce::Slider::textBoxBackgroundColourId, cBg); s.setColour (juce::Slider::textBoxOutlineColourId, cLine); s.setColour (juce::Slider::backgroundColourId, cBg);
    s.setWantsKeyboardFocus (false);
}
} // namespace

// ================================================================================================== the background worker
class ReHarmoniserEditor::Worker : private juce::Thread
{
public:
    Worker() : juce::Thread ("Re-HarmoniSer job"), gen (std::make_shared<std::atomic<int>> (0)) {}
    ~Worker() override { cancelFlag = true; ++(*gen); stopThread (60000); }
    void start (std::function<juce::String (Worker&)> w, std::function<void (const juce::String&)> d)
    {
        cancelFlag = true; ++(*gen); stopThread (60000);
        work = std::move (w); done = std::move (d); myGen = gen->load(); cancelFlag = false; frac = 0.0;
        { juce::ScopedLock l (lock); stageText.clear(); }
        startThread();
    }
    void cancel() { cancelFlag = true; }
    bool cancelled() const { return cancelFlag.load(); }
    bool running() const { return isThreadRunning(); }
    double progress() const { return frac.load(); }
    juce::String stage() const { juce::ScopedLock l (lock); return stageText; }
    Hooks hooks()
    {
        Hooks h;
        h.progress = [this] (const char* s, double f) { { juce::ScopedLock l (lock); stageText = s; } frac = f; };
        h.cancelled = [this] { return cancelFlag.load(); };
        return h;
    }
private:
    void run() override
    {
        juce::String err;
        try { err = work (*this); }
        catch (const Cancelled&) { err = "Cancelled."; }
        catch (const std::exception& e) { err = e.what(); }
        catch (...) { err = "Something went wrong."; }
        auto g = gen; const int my = myGen; auto cb = done;
        juce::MessageManager::callAsync ([g, my, cb, err] { if (g->load() == my && cb) cb (err); });
    }
    std::function<juce::String (Worker&)> work; std::function<void (const juce::String&)> done;
    std::shared_ptr<std::atomic<int>> gen; int myGen = 0;
    std::atomic<bool> cancelFlag { false }; std::atomic<double> frac { 0.0 };
    juce::CriticalSection lock; juce::String stageText;
};

// ================================================================================================== the spectrogram view
const juce::uint32 kVoiceCols[] = { 0xffff6b6b, 0xffffd166, 0xff06d6a0, 0xff4cc9f0, 0xffc77dff, 0xfff4a261, 0xffa8dadc, 0xffe5989b };

struct ReHarmoniserEditor::VoiceRowC : public juce::Component
{
    juce::ToggleButton tick; juce::Colour col; juce::String text; double strength = 0.0;
    VoiceRowC() { tick.setWantsKeyboardFocus (false); tick.setColour (juce::ToggleButton::tickColourId, cAmber); addAndMakeVisible (tick); }
    void resized() override { tick.setBounds (0, 0, 26, getHeight()); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (col); g.fillRoundedRectangle (30.0f, 5.0f, 14.0f, (float) getHeight() - 10.0f, 3.0f);
        g.setColour (cText); g.setFont (uiFont (12.5f)); g.drawText (text, 50, 0, 70, getHeight(), juce::Justification::centredLeft, false);
        g.setColour (cAmberDim); g.fillRect (124, 8, (int) (10 + 70 * strength), getHeight() - 16);
    }
};

class ReHarmoniserEditor::View : public juce::Component
{
public:
    static constexpr int kKeys = 58, kRuler = 22;
    explicit View (ReHarmoniserEditor& o) : ed (o)
    {
        const double sx[] = { 0.0, 0.28, 0.55, 0.8, 1.0 };
        const juce::uint32 sc[] = { 0xff0b0907, 0xff3a1d0a, 0xffa8540c, 0xffffb030, 0xfffff4d6 };
        for (int i = 0; i < 256; ++i)
        {
            const double u = i / 255.0; int k = 0; while (k < 3 && u > sx[k + 1]) ++k;
            const double f = juce::jlimit (0.0, 1.0, (u - sx[k]) / (sx[k + 1] - sx[k]));
            lut[(size_t) i] = juce::Colour (sc[k]).interpolatedWith (juce::Colour (sc[k + 1]), (float) f);
        }
    }
    double vt0 = 0.0, vt1 = 10.0, vm0 = 43.0, vm1 = 88.0;      // what is in view: seconds, and pitch as MIDI numbers (bottom, top)
    double lineOp = 0.18, gainDb = 0.0;
    bool hasSel = false; double sT0 = 0, sT1 = 0, sM0 = 0, sM1 = 0;      // the box: seconds, MIDI (fractional)
    bool dirty = true;

    juce::Rectangle<int> plot() const { return getLocalBounds().withTrimmedLeft (kKeys).withTrimmedTop (kRuler); }
    double xT (int x) const { auto p = plot(); return vt0 + (double) (x - p.getX()) / juce::jmax (1, p.getWidth()) * (vt1 - vt0); }
    int tX (double t) const { auto p = plot(); return p.getX() + (int) std::lround ((t - vt0) / juce::jmax (1.0e-9, vt1 - vt0) * p.getWidth()); }
    double yM (int y) const { auto p = plot(); return vm0 + (double) (p.getBottom() - y) / juce::jmax (1, p.getHeight()) * (vm1 - vm0); }
    int mY (double m) const { auto p = plot(); return p.getBottom() - (int) std::lround ((m - vm0) / (vm1 - vm0) * p.getHeight()); }
    double hzM (double hz) const { return midiOf (juce::jmax (1.0, hz), ed.a4); }

    void clampView()
    {
        const double dur = juce::jmax (0.1, ed.dur);
        double span = juce::jlimit (juce::jmin (0.2, dur), dur, vt1 - vt0);
        vt0 = juce::jlimit (0.0, juce::jmax (0.0, dur - span), vt0); vt1 = vt0 + span;
        double ps = juce::jlimit (6.0, 96.0, vm1 - vm0);
        vm0 = juce::jlimit (12.0, 108.0 - ps, vm0); vm1 = vm0 + ps;
        dirty = true; repaint();
    }
    void zoomTime (double factor, double anchorT)
    {
        const double span = (vt1 - vt0) * factor, f = (anchorT - vt0) / juce::jmax (1.0e-9, vt1 - vt0);
        vt0 = anchorT - f * span; vt1 = vt0 + span; clampView();
    }
    void zoomPitch (double factor, double anchorM)
    {
        const double span = (vm1 - vm0) * factor, f = (anchorM - vm0) / juce::jmax (1.0e-9, vm1 - vm0);
        vm0 = anchorM - f * span; vm1 = vm0 + span; clampView();
    }
    void fitAll() { vt0 = 0; vt1 = ed.dur; clampView(); }
    /** The playhead went past the right edge: turn a page. */
    void follow (double t)
    {
        if (t < vt0 || t > vt1) { const double span = vt1 - vt0; vt0 = t - 0.05 * span; vt1 = vt0 + span; clampView(); }
    }
    void showNote (double t0, double t1, double hz)
    {
        const double pad = juce::jmax (1.0, (t1 - t0) * 0.6);
        if (t0 - 0.5 < vt0 || t1 + 0.5 > vt1) { const double span = juce::jmax (vt1 - vt0, t1 - t0 + 2 * pad); vt0 = t0 - pad; vt1 = vt0 + span; }
        const double m = hzM (hz);
        if (m + 8 > vm1 || m - 8 < vm0) { const double ps = vm1 - vm0; vm0 = m - ps / 2; vm1 = vm0 + ps; }
        clampView();
    }
    void setSelection (double t0, double t1, double m0, double m1) { hasSel = true; sT0 = t0; sT1 = t1; sM0 = m0; sM1 = m1; repaint(); }
    void clearSelection() { hasSel = false; repaint(); }

    void paint (juce::Graphics& g) override;

    void rebuildImage (juce::Rectangle<int> pl);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    ReHarmoniserEditor& ed; juce::Image image; std::array<juce::Colour, 256> lut;
    enum class Mode { none, box, keys, ruler, brush } mode = Mode::none;
    juce::Point<int> downPos, lastStroke; double anchorT = 0, anchorM = 0, panM0 = 0, panM1 = 0; bool moved = false;
};

void ReHarmoniserEditor::View::rebuildImage (juce::Rectangle<int> pl)
{
    dirty = false;
    const int W = pl.getWidth(), H = pl.getHeight();
    if (ed.mix == nullptr || W < 2 || H < 2) { image = juce::Image(); return; }
    const auto& mx = *ed.mix;
    image = juce::Image (juce::Image::RGB, W, H, false);
    juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
    std::vector<int> b0 ((size_t) H); std::vector<float> fr ((size_t) H);
    for (int y = 0; y < H; ++y)
    {
        const double m = vm0 + (double) (H - 1 - y) / (double) (H - 1) * (vm1 - vm0);
        const double bp = midiHz (m, ed.a4) / mx.df;
        if (bp < 0.0 || bp >= (double) mx.nb - 1.0) { b0[(size_t) y] = -1; continue; }
        b0[(size_t) y] = (int) bp; fr[(size_t) y] = (float) (bp - std::floor (bp));
    }
    const float add = (float) (gainDb / 70.0 * 255.0);
    for (int x = 0; x < W; ++x)
    {
        const double t = vt0 + ((double) x + 0.5) / (double) W * (vt1 - vt0);
        const size_t f = (size_t) juce::jlimit (0, (int) mx.nf - 1, (int) (t / mx.hopSec));
        const uint8_t* col = mx.v.data() + f * mx.nb;
        for (int y = 0; y < H; ++y)
        {
            const int b = b0[(size_t) y];
            float v = 0.0f;
            if (b >= 0) v = (float) col[b] * (1.0f - fr[(size_t) y]) + (float) col[b + 1] * fr[(size_t) y];
            bd.setPixelColour (x, y, lut[(size_t) juce::jlimit (0, 255, (int) (v + add))]);
        }
    }
}

void ReHarmoniserEditor::View::paint (juce::Graphics& g)
{
    g.fillAll (cBg);
    auto pl = plot();
    if (ed.mix == nullptr)
    {
        g.setColour (cAmber); g.setFont (lcdFont (15.0f));
        g.drawText (ed.analysing ? "Analysing the audio..." : "No audio loaded", pl, juce::Justification::centred);
    }
    else
    {
        if (dirty || image.getWidth() != pl.getWidth() || image.getHeight() != pl.getHeight()) rebuildImage (pl);
        if (image.isValid()) g.drawImageAt (image, pl.getX(), pl.getY());
    }
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (pl);
        // the piano-note lines
        for (int m = (int) std::ceil (vm0); m <= (int) std::floor (vm1); ++m)
        {
            const int y = mY ((double) m);
            const float a = (float) juce::jmin (0.9, lineOp * (m % 12 == 0 ? 2.2 : 1.0));
            if (m == 69) g.setColour (cAmber.withAlpha ((float) juce::jmin (0.9, lineOp * 2.5 + 0.1))); else g.setColour (juce::Colours::white.withAlpha (a));
            g.drawHorizontalLine (y, (float) pl.getX(), (float) pl.getRight());
        }
        // applied corrections: a thin green frame round each
        for (auto& e : ed.edits)
        {
            if (! e.enabled) continue;
            const int x0 = tX (e.t0), x1 = tX (e.t1), y0 = mY (hzM (e.fHi)), y1 = mY (hzM (e.fLo));
            g.setColour (cGreen.withAlpha (0.8f)); g.drawRect (juce::Rectangle<int> (x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0)), 1);
        }
        // the measured line (white) and the planned line (green) of the note being worked on, with the first overtones fainter
        if (ed.cand.has && ! ed.cand.ts.empty())
        {
            const auto& c = ed.cand;
            for (int h = 1; h <= 4; ++h)
            {
                juce::Path meas, plan; bool first = true;
                for (size_t i = 0; i < c.ts.size(); ++i)
                {
                    const float x = (float) tX (c.ts[i]);
                    const float y1 = (float) mY (hzM (c.f0[i] * h)), y2 = (float) mY (hzM (c.f0[i] * std::pow (2.0, c.plan.off[i] / 1200.0) * h));
                    if (first) { meas.startNewSubPath (x, y1); plan.startNewSubPath (x, y2); first = false; } else { meas.lineTo (x, y1); plan.lineTo (x, y2); }
                }
                g.setColour (juce::Colours::white.withAlpha (h == 1 ? 0.95f : 0.35f)); g.strokePath (meas, juce::PathStrokeType (h == 1 ? 1.6f : 1.0f));
                g.setColour (cGreen.withAlpha (h == 1 ? 0.95f : 0.4f)); g.strokePath (plan, juce::PathStrokeType (h == 1 ? 1.6f : 1.0f));
            }
        }
        // the box being drawn / the note box
        if (hasSel)
        {
            const int x0 = tX (juce::jmin (sT0, sT1)), x1 = tX (juce::jmax (sT0, sT1)), y0 = mY (juce::jmax (sM0, sM1)), y1 = mY (juce::jmin (sM0, sM1));
            const juce::Rectangle<int> r (x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0));
            g.setColour (cAmber.withAlpha (0.12f)); g.fillRect (r);
            g.setColour (cAmber); g.drawRect (r, 1);
        }
        // the voices found on the note
        if (ed.vcand.has)
        {
            const auto& vs = ed.vcand.vs;
            for (size_t j = 0; j < vs.C.size(); ++j)
            {
                juce::Path pth; bool first = true; float ex = 0, ey = 0;
                for (size_t k = 0; k < vs.ts.size() && k < vs.C[j].size(); ++k)
                {
                    ex = (float) tX (vs.ts[k]); ey = (float) mY (hzM (vs.fT * std::pow (2.0, vs.C[j][k] / 1200.0) * ed.partial));
                    if (first) { pth.startNewSubPath (ex, ey); first = false; } else pth.lineTo (ex, ey);
                }
                g.setColour (juce::Colour (kVoiceCols[j % 8])); g.strokePath (pth, juce::PathStrokeType (j < vs.strength.size() && vs.strength[j] > 0.8 ? 3.0f : 2.0f));
                if (! first && j < ed.voiceRows.size()) g.drawText (ed.voiceRows[j]->text, (int) ex + 6, (int) ey - 8, 60, 16, juce::Justification::centredLeft, false);
            }
        }
        // erase strokes: the pending one, and (faint) the ones already applied
        {
            auto drawStroke = [&] (const Brush& b, float alpha)
            {
                const double ppm = (double) pl.getHeight() / juce::jmax (1.0, vm1 - vm0), pps = (double) pl.getWidth() / juce::jmax (1.0e-6, vt1 - vt0);
                const float rx = (float) (b.rt * pps), ry = (float) (b.rc / 100.0 * ppm);
                juce::Path line; bool first = true; juce::Point<float> last (-1000.0f, -1000.0f);
                g.setColour (cRed.withAlpha (alpha * 0.35f));
                for (auto& pt : b.pts)
                {
                    const float x = (float) tX (pt.first), y = (float) mY (hzM (pt.second));
                    if (first) { line.startNewSubPath (x, y); first = false; } else line.lineTo (x, y);
                    if (std::abs (x - last.x) + std::abs (y - last.y) > juce::jmax (4.0f, ry * 0.5f)) { g.fillEllipse (x - rx, y - ry, 2.0f * rx, 2.0f * ry); last = { x, y }; }
                }
                g.setColour (cRed.withAlpha (alpha)); g.strokePath (line, juce::PathStrokeType (1.6f));
            };
            for (auto& e : ed.edits) if (e.isBrush && e.enabled) drawStroke (e.brush, 0.35f);
            if (ed.bcand.has || ! ed.bcand.b.pts.empty()) drawStroke (ed.bcand.b, 0.95f);
        }
        // loop markers, the cue and the playhead
        if (ed.loopIn >= 0 && ed.loopOut > ed.loopIn)
        {
            g.setColour (cGreen.withAlpha (0.12f)); g.fillRect (juce::Rectangle<int> (tX (ed.loopIn), pl.getY(), tX (ed.loopOut) - tX (ed.loopIn), pl.getHeight()));
        }
        auto mark = [&] (double t, juce::Colour c) { if (t >= vt0 && t <= vt1) { g.setColour (c); g.drawVerticalLine (tX (t), (float) pl.getY(), (float) pl.getBottom()); } };
        if (ed.loopIn >= 0) mark (ed.loopIn, cGreen);
        if (ed.loopOut >= 0) mark (ed.loopOut, cGreen);
        mark (ed.cueT, cText.withAlpha (0.7f));
        if (ed.playT >= 0) mark (ed.playT, cRed);
    }
    // the ruler
    g.setColour (cPanel); g.fillRect (0, 0, getWidth(), kRuler);
    {
        const double span = vt1 - vt0; const double steps[] = { 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 };
        double st = steps[12]; for (double s : steps) if (s / span * pl.getWidth() >= 70.0) { st = s; break; }
        g.setFont (lcdFont (11.0f));
        for (double t = std::ceil (vt0 / st) * st; t <= vt1; t += st)
        {
            const int x = tX (t); g.setColour (cLine); g.drawVerticalLine (x, (float) kRuler - 6, (float) kRuler);
            g.setColour (cAmber); g.drawText (fmtTime (t), x + 3, 0, 80, kRuler, juce::Justification::centredLeft, false);
        }
    }
    // the keys
    g.setColour (cPanel); g.fillRect (0, kRuler, kKeys, getHeight() - kRuler);
    {
        juce::Graphics::ScopedSaveState ss (g); g.reduceClipRegion (juce::Rectangle<int> (0, kRuler, kKeys, getHeight() - kRuler));
        for (int m = (int) std::floor (vm0) - 1; m <= (int) std::ceil (vm1) + 1; ++m)
        {
            const int yt = mY ((double) m + 0.5), yb = mY ((double) m - 0.5);
            const juce::Rectangle<int> r (0, yt, isBlackKey (m) ? kKeys - 18 : kKeys - 2, juce::jmax (1, yb - yt));
            g.setColour (isBlackKey (m) ? cKeyB : cKeyW); g.fillRect (r.reduced (0, 0));
            g.setColour (cLine); g.drawHorizontalLine (yb, 0.0f, (float) kKeys);
            if (! isBlackKey (m) && r.getHeight() >= 11)
            {
                g.setColour (juce::Colour (0xff3a3326)); g.setFont (lcdFont (10.0f));
                if (m % 12 == 0 || r.getHeight() >= 16) g.drawText (kNames[((m % 12) + 12) % 12] + juce::String (m / 12 - 1), r.withTrimmedLeft (2), juce::Justification::centredLeft, false);
            }
        }
    }
}

void ReHarmoniserEditor::View::mouseDown (const juce::MouseEvent& e)
{
    downPos = e.getPosition(); moved = false; ed.grabKeyboardFocus();
    if (e.x < kKeys && e.y >= kRuler) { mode = Mode::keys; panM0 = vm0; panM1 = vm1; anchorM = yM (e.y); return; }
    if (e.y < kRuler) { mode = Mode::ruler; ed.cueT = juce::jlimit (0.0, ed.dur, xT (e.x)); repaint(); return; }
    if (ed.brushMode() && ed.mix != nullptr)
    {
        mode = Mode::brush; ed.strokeBegin(); lastStroke = e.getPosition();
        ed.strokePoint (juce::jlimit (0.0, ed.dur, xT (e.x)), midiHz (yM (e.y), ed.a4));
        return;
    }
    mode = Mode::box; anchorT = xT (e.x); anchorM = yM (e.y);
}

void ReHarmoniserEditor::View::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3) moved = true;
    if (mode == Mode::keys)
    {
        const double dm = (double) (e.y - downPos.y) / juce::jmax (1, plot().getHeight()) * (panM1 - panM0);
        vm0 = panM0 + dm; vm1 = panM1 + dm; clampView();
    }
    else if (mode == Mode::ruler) { ed.cueT = juce::jlimit (0.0, ed.dur, xT (e.x)); repaint(); }
    else if (mode == Mode::brush)
    {
        if (e.getPosition().getDistanceFrom (lastStroke) >= 3) { lastStroke = e.getPosition(); ed.strokePoint (juce::jlimit (0.0, ed.dur, xT (e.x)), midiHz (yM (e.y), ed.a4)); }
    }
    else if (mode == Mode::box && moved)
    {
        hasSel = true; sT0 = anchorT; sT1 = juce::jlimit (0.0, ed.dur, xT (e.x)); sM0 = anchorM; sM1 = yM (e.y); repaint();
    }
}

void ReHarmoniserEditor::View::mouseUp (const juce::MouseEvent&)
{
    if (mode == Mode::keys && ! moved)
    {
        const int m = (int) std::lround (anchorM);
        if (ed.host.playTone) ed.host.playTone (midiHz (m, ed.a4), ed.keyRow.s.getValue());
    }
    else if (mode == Mode::brush) ed.strokeEnd();
    else if (mode == Mode::box && moved && hasSel)
    {
        ed.selectBox (juce::jmin (sT0, sT1), juce::jmax (sT0, sT1), midiHz (juce::jmin (sM0, sM1), ed.a4), midiHz (juce::jmax (sM0, sM1), ed.a4));
    }
    mode = Mode::none;
}

void ReHarmoniserEditor::View::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < kRuler) fitAll(); else if (e.x >= kKeys) ed.deselect();
}

void ReHarmoniserEditor::View::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const double d = w.deltaY != 0 ? w.deltaY : w.deltaX;
    if (e.mods.isCtrlDown()) zoomPitch (d > 0 ? 0.8 : 1.25, yM (e.y));
    else if (e.mods.isShiftDown() || w.deltaX != 0) { const double span = vt1 - vt0; vt0 -= d * span * 0.5; vt1 = vt0 + span; clampView(); }
    else zoomTime (d > 0 ? 0.8 : 1.25, xT (e.x));
}

// ================================================================================================== Corrected Notes list
class ReHarmoniserEditor::EditsList : public juce::Component
{
public:
    explicit EditsList (ReHarmoniserEditor& o) : ed (o) {}
    void rebuild()
    {
        rows.clear(); removeAllChildren();
        for (auto& e : ed.edits)
        {
            auto r = std::make_unique<RowC>(); const int id = e.id;
            r->on.setToggleState (e.enabled, juce::dontSendNotification); r->on.setColour (juce::ToggleButton::textColourId, cText); r->on.setColour (juce::ToggleButton::tickColourId, cAmber);
            r->on.setButtonText ({}); r->on.setTooltip ("Switch this correction off and on to compare");
            r->on.onClick = [this, id, p = &r->on]
            {
                for (auto& x : ed.edits) if (x.id == id) x.enabled = p->getToggleState();
                ed.undone.clear(); ed.renderAndSet (false); ed.updateButtons(); ed.view->repaint();
            };
            r->txt.setText (e.label, juce::dontSendNotification); styleLabel (r->txt, cText, 12.0f); r->txt.setInterceptsMouseClicks (false, false);
            r->edit.setButtonText ("Edit"); r->rem.setButtonText ("X"); styleButton (r->edit); styleButton (r->rem);
            r->edit.setTooltip ("Takes it back out of Corrected Notes so you can change it, then Apply correction again");
            r->rem.setTooltip ("Removes this correction");
            r->edit.onClick = [this, id]
            {
                for (size_t i = 0; i < ed.edits.size(); ++i) if (ed.edits[i].id == id)
                { auto copy = ed.edits[i]; ed.edits.erase (ed.edits.begin() + (long) i); if (copy.isVoices) ed.loadVoicesAsPending (copy); else if (copy.isBrush) ed.loadBrushAsPending (copy); else ed.loadAsPending (copy); return; }
            };
            r->rem.onClick = [this, id]
            {
                for (size_t i = 0; i < ed.edits.size(); ++i) if (ed.edits[i].id == id) { ed.edits.erase (ed.edits.begin() + (long) i); break; }
                ed.undone.clear(); ed.rebuildEditsList(); ed.renderAndSet (false); ed.updateButtons(); ed.view->repaint();
            };
            addAndMakeVisible (r->on); addAndMakeVisible (r->txt); addAndMakeVisible (r->edit); addAndMakeVisible (r->rem);
            rows.push_back (std::move (r));
        }
        setSize (getWidth(), juce::jmax (30, (int) rows.size() * 30 + 4));
        resized();
    }
    void resized() override
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto b = juce::Rectangle<int> (0, (int) i * 30 + 2, getWidth(), 28);
            rows[i]->on.setBounds (b.removeFromLeft (26)); rows[i]->rem.setBounds (b.removeFromRight (28).reduced (0, 2)); b.removeFromRight (4);
            rows[i]->edit.setBounds (b.removeFromRight (42).reduced (0, 2)); rows[i]->txt.setBounds (b);
        }
    }
    void paint (juce::Graphics& g) override
    {
        if (rows.empty()) { g.setColour (cAmberDim); g.setFont (uiFont (12.0f)); g.drawText ("None yet.", getLocalBounds().reduced (4, 2), juce::Justification::topLeft); }
    }
private:
    struct RowC { juce::ToggleButton on; juce::Label txt; juce::TextButton edit, rem; };
    ReHarmoniserEditor& ed; std::vector<std::unique_ptr<RowC>> rows;
};

std::shared_ptr<ReHarmoniserEditor::Mix> ReHarmoniserEditor::makeMix (const Spectrogram& sp)
{
    auto m = std::make_shared<Mix>(); m->nf = sp.nf; m->nb = sp.nb; m->df = sp.df; m->hopSec = (double) sp.hop / sp.sr2; m->v.resize (sp.nf * sp.nb);
    const double range = 70.0; std::vector<float> d;
    for (size_t f = 0; f < sp.nf; ++f)
    {
        sp.combineFrame (f, d);
        for (size_t b = 0; b < sp.nb; ++b) m->v[f * sp.nb + b] = (uint8_t) juce::jlimit (0.0, 255.0, ((double) d[b] - (sp.ref - range)) / range * 255.0);
    }
    return m;
}

struct ReHarmoniserEditor::Section
{
    juce::String title; juce::TextButton header; std::vector<juce::Component*> kids; int bodyH = 100;
};

// ================================================================================================== the editor
ReHarmoniserEditor::ReHarmoniserEditor() : bar (progressValue)
{
    setWantsKeyboardFocus (true);
    worker = std::make_unique<Worker>();
    view = std::make_unique<View> (*this); addAndMakeVisible (*view);
    editsList = std::make_unique<EditsList> (*this); editsView.setViewedComponent (editsList.get(), false); editsView.setScrollBarsShown (true, false);
    addAndMakeVisible (editsView);
    bar.setColour (juce::ProgressBar::foregroundColourId, cAmber); bar.setColour (juce::ProgressBar::backgroundColourId, cBg); addChildComponent (bar);
    styleLabel (status, cAmber, 13.0f); status.setFont (lcdFont (12.5f)); status.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (status);
    styleLabel (noteInfo, cAmber, 12.0f); noteInfo.setFont (lcdFont (12.0f)); noteInfo.setJustificationType (juce::Justification::topLeft); noteInfo.setMinimumHorizontalScale (0.7f); addAndMakeVisible (noteInfo);
    styleLabel (tipLabel, cText, 12.0f); tipLabel.setJustificationType (juce::Justification::topLeft); addChildComponent (tipLabel);
    for (auto* b : { &playBtn, &stopBtn, &loopBtn, &originalBtn, &cancelBtn, &downBtn, &upBtn, &remeasureBtn, &tipUse, &tipIgnore, &detectBtn, &auditionBtn, &applyBtn, &deselectBtn, &clearBtn, &undoBtn, &redoBtn,
                     &zoomTIn, &zoomTOut, &zoomPIn, &zoomPOut, &loopClearBtn }) { styleButton (*b); addAndMakeVisible (b); }
    styleButton (writeBtn, juce::Colour (0xff1f7a46)); addAndMakeVisible (writeBtn);
    styleButton (applyBtn, juce::Colour (0xff8a5a10)); styleButton (playBtn, juce::Colour (0xff8a5a10));
    tipUse.setVisible (false); tipIgnore.setVisible (false);
    loopBtn.setClickingTogglesState (true); originalBtn.setClickingTogglesState (true);
    loopBtn.setColour (juce::TextButton::buttonOnColourId, cAmberDim); originalBtn.setColour (juce::TextButton::buttonOnColourId, cAmberDim);

    nameBox.setFont (lcdFont (14.0f)); nameBox.setColour (juce::TextEditor::backgroundColourId, cBg); nameBox.setColour (juce::TextEditor::textColourId, cAmber);
    nameBox.setColour (juce::TextEditor::outlineColourId, cLine); nameBox.setText ("", false); nameBox.setTooltip ("The note it should be, for example F5, Ab4, C#4, Bb3");
    nameBox.onReturnKey = [this] { const int m = parseNote (nameBox.getText()); if (m > 0 && cand.has) { cand.midi = m; midiLocked = true; replan(); } updateInfo(); };
    nameBox.onFocusLost = nameBox.onReturnKey;
    addAndMakeVisible (nameBox); styleLabel (nameCap, cAmberDim, 12.0f); addAndMakeVisible (nameCap);
    downBtn.onClick = [this] { if (cand.has) { --cand.midi; midiLocked = true; replan(); } };
    upBtn.onClick = [this] { if (cand.has) { ++cand.midi; midiLocked = true; replan(); } };
    remeasureBtn.onClick = [this] { if (cand.has) selectBox (cand.t0, cand.t1, cand.fLo, cand.fHi); };
    tipUse.onClick = [this] { if (cand.hint > 1) { partial = cand.hint; partialBox.setSelectedId (partial, juce::dontSendNotification); measure(); } };
    tipIgnore.onClick = [this] { cand.hint = 1; updateInfo(); };

    // sections: Ensemble ReCentre, Note ReShape, Corrected Notes
    const char* titles[] = { "Ensemble ReCentre", "Note ReShape", "Section ReFinement", "Erase ReBrush", "See mixer", "Corrected Notes" };
    for (int i = 0; i < 6; ++i)
    {
        auto s = std::make_unique<Section>(); s->title = titles[i]; s->header.setButtonText (titles[i]); styleButton (s->header, cPanel);
        s->header.setColour (juce::TextButton::textColourOffId, cAmber); addAndMakeVisible (s->header);
        const int idx = i; s->header.onClick = [this, idx] { openSection (openIdx == idx ? -1 : idx); };
        sections.push_back (std::move (s));
    }
    // -- Ensemble ReCentre
    styleLabel (a4Cap, cText, 12.0f); addAndMakeVisible (a4Cap);
    a4Box.setFont (lcdFont (14.0f)); a4Box.setColour (juce::TextEditor::backgroundColourId, cBg); a4Box.setColour (juce::TextEditor::textColourId, cAmber); a4Box.setColour (juce::TextEditor::outlineColourId, cLine);
    a4Box.setInputRestrictions (7, "0123456789."); a4Box.setText ("440.0", false);
    a4Box.setTooltip ("The pitch the written notes sit at. Choirs often drift away from A=440 and are tuned to themselves, so measure it from the singing.");
    a4Box.onReturnKey = [this] { const double v = a4Box.getText().getDoubleValue(); if (v > 400 && v < 480) { a4 = v; refMode.setSelectedId (3, juce::dontSendNotification); refModeId = 3; if (cand.has && ! midiLocked) cand.midi = (int) std::lround (midiOf (median (cand.f0), a4)); replan(); view->dirty = true; view->repaint(); } };
    a4Box.onFocusLost = [this] { a4Box.setText (juce::String (a4, 1), false); };
    addAndMakeVisible (a4Box); detectBtn.onClick = [this] { detectReference(); };
    refMode.addItem ("Around each note (+/- 10 s)", 1); refMode.addItem ("Whole audio", 2); refMode.addItem ("Typed by hand", 3); refMode.setSelectedId (1, juce::dontSendNotification);
    refMode.onChange = [this] { refModeId = refMode.getSelectedId(); };
    refMode.setColour (juce::ComboBox::backgroundColourId, cBg); refMode.setColour (juce::ComboBox::textColourId, cText); refMode.setColour (juce::ComboBox::outlineColourId, cLine);
    addAndMakeVisible (refMode); styleLabel (refInfo, cAmber, 12.0f); refInfo.setFont (lcdFont (12.0f)); addAndMakeVisible (refInfo);
    sections[0]->kids = { &a4Cap, &a4Box, &detectBtn, &refMode, &refInfo }; sections[0]->bodyH = 112;
    // -- Note ReShape
    auto row = [this] (Row& r, const juce::String& cap, double lo, double hi, double step, double v, const juce::String& suffix, const juce::String& tip)
    {
        r.cap.setText (cap, juce::dontSendNotification); styleLabel (r.cap, cText, 12.0f); r.cap.setTooltip (tip); styleSlider (r.s, lo, hi, step, v, suffix);
        r.s.setTooltip (tip); r.s.onValueChange = [this] { sliderChanged(); }; addAndMakeVisible (r.cap); addAndMakeVisible (r.s);
    };
    row (moveRow, "Move whole note", -100, 100, 1, 0, " c", "Moves the whole note up or down by this many cents (hundredths of a semitone).");
    row (snapRow, "Snap to note", 0, 100, 1, 0, " %", "0% = leave as sung, 100% = dead on the written note.");
    row (strengthRow, "Strength", 0, 100, 5, 100, " %", "Overall amount of the planned change that is applied.");
    row (keepRow, "Keep wobble", 0, 100, 5, 0, " %", "When snapping: how much of the natural wobble (vibrato) stays. 0% = straight line.");
    row (smoothRow, "Wobble faster than", 0.05, 1.5, 0.05, 0.25, " s", "Slower drift than this is treated as pitch; faster is treated as wobble.");
    row (easeRow, "Ease in", 0, 1, 0.05, 0.1, " s", "The correction fades in over this time after the start of the box.");
    row (holdRow, "Tail hold", 0, 1.5, 0.05, 0.25, " s", "Keep correcting the room's ring-out for this long after the end of the box.");
    row (bwRow, "Band width", 20, 120, 5, 45, " Hz", "Width of the band taken around each overtone. Wider removes more of the old pitch but can catch neighbouring voices.");
    partialBox.addItem ("Box is round: the note itself", 1); partialBox.addItem ("its 2nd line (octave up)", 2); partialBox.addItem ("its 3rd line", 3); partialBox.addItem ("its 4th line", 4);
    partialBox.setSelectedId (1, juce::dontSendNotification); partialBox.onChange = [this] { partial = partialBox.getSelectedId(); if (cand.has) measure(); };
    partialBox.setColour (juce::ComboBox::backgroundColourId, cBg); partialBox.setColour (juce::ComboBox::textColourId, cText); partialBox.setColour (juce::ComboBox::outlineColourId, cLine);
    partialBox.setTooltip ("Which overtone you drew the box round. Leave on the note itself if you drew round the lowest line of the note.");
    addAndMakeVisible (partialBox);
    matchBox.setToggleState (true, juce::dontSendNotification); matchBox.setColour (juce::ToggleButton::textColourId, cText); matchBox.setColour (juce::ToggleButton::tickColourId, cAmber);
    matchBox.onClick = [this] { sliderChanged(); }; matchBox.setWantsKeyboardFocus (false); addAndMakeVisible (matchBox);
    advancedBtn.setColour (juce::ToggleButton::textColourId, cAmber); advancedBtn.setColour (juce::ToggleButton::tickColourId, cAmber); advancedBtn.setWantsKeyboardFocus (false);
    advancedBtn.onClick = [this] { layoutSections(); }; addAndMakeVisible (advancedBtn);
    auditionBtn.setTooltip ("Hears the correction that is not applied yet. It only gets ready: press Space or Play to listen."); auditionBtn.onClick = [this] { audition(); };
    applyBtn.onClick = [this] { applyCorrection(); }; deselectBtn.onClick = [this] { deselect(); }; clearBtn.onClick = [this] { clearChanges(); };
    undoBtn.onClick = [this] { undoStep(); }; redoBtn.onClick = [this] { redoStep(); };
    // -- View
    row (linesRow, "Note lines", 0, 100, 5, 18, " %", "How clearly the horizontal lines for each piano note show on the picture (the C lines are a little stronger).");
    row (gainRow, "Colour gain", -40, 40, 1, 0, " dB", "Brighter / darker picture.");
    linesRow.s.onValueChange = [this] { view->lineOp = linesRow.s.getValue() / 100.0; view->repaint(); };
    gainRow.s.onValueChange = [this] { view->gainDb = gainRow.s.getValue(); view->dirty = true; view->repaint(); };
    zoomTIn.onClick = [this] { view->zoomTime (0.7, 0.5 * (view->vt0 + view->vt1)); }; zoomTOut.onClick = [this] { view->zoomTime (1.4, 0.5 * (view->vt0 + view->vt1)); };
    zoomPIn.onClick = [this] { view->zoomPitch (0.7, 0.5 * (view->vm0 + view->vm1)); }; zoomPOut.onClick = [this] { view->zoomPitch (1.4, 0.5 * (view->vm0 + view->vm1)); };
    loopClearBtn.onClick = [this] { loopIn = loopOut = -1.0; view->repaint(); };
    styleLabel (zoomCap, cText, 12.0f); addAndMakeVisible (zoomCap);
    sections[1]->kids = { &moveRow.cap, &moveRow.s, &snapRow.cap, &snapRow.s, &advancedBtn, &auditionBtn, &applyBtn, &deselectBtn, &clearBtn, &undoBtn, &redoBtn };
    sections[5]->kids = { &editsView };
    // -- See mixer: what the picture shows (not what you hear)
    seeView.setViewedComponent (&seeHolder, false); seeView.setScrollBarsShown (true, false); addChildComponent (seeView);
    sections[4]->kids = { &seeView };
    // -- Key level
    row (keyRow, "Key level", -50, 0, 1, -20, " dB", "How loud the piano-key notes are when you click a key on the left of the picture.");
    // -- Erase ReBrush
    brushHelp.setText ("Paint over a sound you want gone (a cough, a squeak, one stray voice): hold the mouse down and drag along it on the picture. Then Audition, and Erase (apply).", juce::dontSendNotification);
    styleLabel (brushHelp, cText, 11.5f); brushHelp.setMinimumHorizontalScale (1.0f); addChildComponent (brushHelp);
    row (brushTRow, "Time radius", 0.05, 1.0, 0.01, 0.2, " s", "How far along the time the brush reaches either side of the line you painted.");
    row (brushCRow, "Pitch radius", 20, 300, 5, 60, " c", "How far up and down (in cents) the brush reaches from the line you painted. 100 cents = one semitone.");
    row (brushAmtRow, "Amount", 5, 100, 5, 100, " %", "How much of the sound is taken out. 100% = as much as possible.");
    for (auto* r : { &brushTRow, &brushCRow, &brushAmtRow }) { r->cap.setVisible (false); r->s.setVisible (false); r->s.onValueChange = [this] { brushChanged(); }; }
    brushHarm.setToggleState (true, juce::dontSendNotification); brushHarm.setColour (juce::ToggleButton::textColourId, cText); brushHarm.setColour (juce::ToggleButton::tickColourId, cAmber);
    brushHarm.setWantsKeyboardFocus (false); brushHarm.onClick = [this] { brushChanged(); }; addChildComponent (brushHarm);
    for (auto* b : { &brushAuditionBtn, &brushApplyBtn, &brushDiscardBtn }) { styleButton (*b); addChildComponent (b); }
    brushAuditionBtn.onClick = [this] { if (bcand.has && ! busy) renderAndSet (true, [this] { setStatus ("Ready: press Space (or Play) to hear it. Original plays without the changes."); }); };
    brushApplyBtn.onClick = [this] { applyBrush(); }; brushDiscardBtn.onClick = [this] { discardBrush(); };
    // -- Section ReFinement
    voiceHelp.setText ("Several singers on one note and some are out of tune: draw a box round the note first, press Find voices, tick the voices to move, hear it with Preview move, then Apply correction. Voices closer than about 10 cents cannot be told apart.", juce::dontSendNotification);
    styleLabel (voiceHelp, cText, 11.5f); voiceHelp.setMinimumHorizontalScale (1.0f); addChildComponent (voiceHelp);
    voiceTarget.addItem ("Move them to the written note", 1); voiceTarget.addItem ("Move them to the strongest voice", 2); voiceTarget.setSelectedId (1, juce::dontSendNotification);
    voiceTarget.setColour (juce::ComboBox::backgroundColourId, cBg); voiceTarget.setColour (juce::ComboBox::textColourId, cText); voiceTarget.setColour (juce::ComboBox::outlineColourId, cLine);
    voiceTarget.onChange = [this] { if (vcand.has && vcand.preview) auditionStale = true; };
    addChildComponent (voiceTarget);
    for (auto* b : { &voiceFindBtn, &voicePreviewBtn, &voiceApplyBtn, &voiceDiscardBtn }) { styleButton (*b); addChildComponent (b); }
    voiceFindBtn.setTooltip ("Pick a note first (draw a box round it). Finds the separate voices on it (needs about half a second or more).");
    voicePreviewBtn.setTooltip ("Hear the ticked voices moved (the others are left alone). Press Apply correction to keep it.");
    voiceFindBtn.onClick = [this] { findVoices(); }; voicePreviewBtn.onClick = [this] { previewVoices(); };
    voiceApplyBtn.onClick = [this] { applyVoices(); }; voiceDiscardBtn.onClick = [this] { discardVoices(); };
    sections[2]->kids = { &voiceHelp, &voiceTarget, &voiceFindBtn, &voicePreviewBtn, &voiceApplyBtn, &voiceDiscardBtn };
    sections[3]->kids = { &brushHelp, &brushTRow.cap, &brushTRow.s, &brushCRow.cap, &brushCRow.s, &brushAmtRow.cap, &brushAmtRow.s, &brushHarm, &brushAuditionBtn, &brushApplyBtn, &brushDiscardBtn };
    // transport
    playBtn.onClick = [this] { togglePlay(); }; stopBtn.onClick = [this] { if (host.stop) host.stop(); };
    loopBtn.onClick = [this] { looping = loopBtn.getToggleState(); };
    originalBtn.setTooltip ("Plays the audio with none of the changes. Press again to hear them. Greyed out until something has been changed.");
    originalBtn.onClick = [this] { toggleOriginal(); };
    writeBtn.onClick = [this] { writeBack(); }; cancelBtn.onClick = [this] { if (worker->running()) worker->cancel(); if (host.cancel) host.cancel(); };
    writeBtn.setTooltip ("Puts all the corrected notes back into the clip and closes this window");
    openSection (1); updateButtons(); updateInfo();
    setSize (1280, 800);
    startTimerHz (30);
}

ReHarmoniserEditor::~ReHarmoniserEditor() { stopTimer(); if (worker) worker->cancel(); }

int ReHarmoniserEditor::appliedCount() const { int n = 0; for (auto& e : edits) if (e.enabled) ++n; return n; }
bool ReHarmoniserEditor::hasAppliedChanges() const { return appliedCount() > 0; }
juce::String ReHarmoniserEditor::noteName (int midi) const { return juce::String (kNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1); }

int ReHarmoniserEditor::parseNote (const juce::String& s)
{
    auto t = s.trim(); if (t.isEmpty()) return -1;
    const char c = (char) juce::CharacterFunctions::toUpperCase (t[0]);
    static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };        // A B C D E F G
    if (c < 'A' || c > 'G') return -1;
    int n = base[c - 'A']; int i = 1;
    if (i < t.length() && (t[i] == '#')) { ++n; ++i; } else if (i < t.length() && (t[i] == 'b' || t[i] == 'B') && i + 1 < t.length()) { --n; ++i; }
    const auto oct = t.substring (i); if (oct.isEmpty() || ! oct.containsOnly ("-0123456789")) return -1;
    return 12 * (oct.getIntValue() + 1) + n;
}

void ReHarmoniserEditor::setStatus (const juce::String& m, bool error)
{
    lastStatus = m; status.setText (m, juce::dontSendNotification); status.setColour (juce::Label::textColourId, error ? cRed : cAmber);
    if (error && host.log) host.log ("Re-HarmoniSer: " + m);
}

void ReHarmoniserEditor::startJob (const juce::String& stage, std::function<juce::String (Worker&)> work, std::function<void (const juce::String&)> done)
{
    busy = true; progressValue = 0.0; bar.setVisible (true); setStatus (stage); updateButtons();
    juce::Component::SafePointer<ReHarmoniserEditor> self (this);
    worker->start (std::move (work), [self, done] (const juce::String& err)
    {
        if (self == nullptr) return;
        self->busy = false; self->bar.setVisible (false);
        if (done) done (err);
        self->updateButtons();
    });
}

// ---------------------------------------------------------------------------------------------------- loading and analysis
void ReHarmoniserEditor::setAudio (std::shared_ptr<const Block> audio, int sampleRate)
{
    orig = std::move (audio); sr = sampleRate;
    if (orig == nullptr || orig->empty() || (*orig)[0].empty() || sr <= 0) { setStatus ("There is no audio to work on.", true); return; }
    dur = (double) (*orig)[0].size() / sr;
    rebuildSeeStrips(); spec = nullptr;
    view->vt0 = 0; view->vt1 = dur; view->vm0 = 43; view->vm1 = 88; view->clampView();
    startAnalysis();
}

void ReHarmoniserEditor::startAnalysis()
{
    analysing = true; mix = nullptr; view->repaint();
    auto audio = orig; const int rate = sr;
    auto result = std::make_shared<std::pair<std::shared_ptr<Mix>, double>>();
    auto specOut = std::make_shared<std::shared_ptr<Spectrogram>>();
    startJob ("Analysing the audio", [audio, rate, result, specOut] (Worker& w) -> juce::String
    {
        auto sp = std::make_shared<Spectrogram>(); sp->build (spansOf (*audio), rate, Spectrogram::Detail::Balanced, w.hooks());
        *result = { makeMix (*sp), sp->ref }; *specOut = sp;
        return {};
    },
    [this, result, specOut] (const juce::String& err)
    {
        analysing = false;
        if (err.isEmpty()) { spec = *specOut; if (seeDirty) seeDirtyAt = 0; }
        if (err.isNotEmpty()) { setStatus (err == "Cancelled." ? err : "Could not analyse the audio: " + err, err != "Cancelled."); view->repaint(); return; }
        mix = result->first; refDb = result->second; view->dirty = true; view->repaint();
        setStatus ("Drag a box round a note on the picture (time across, pitch up). Space plays; click the ruler to put the playhead.");
        updateInfo();
    });
}

// ---------------------------------------------------------------------------------------------------- Note Selection
void ReHarmoniserEditor::selectBox (double t0, double t1, double fLo, double fHi)
{
    if (orig == nullptr || busy) return;
    t0 = juce::jmax (0.0, t0); t1 = juce::jmin (dur, t1);
    if (t1 - t0 < 0.15) { setStatus ("That box is too short. Draw it at least 0.15 s wide.", true); view->clearSelection(); return; }
    fLo /= partial; fHi /= partial;
    if (fHi / fLo < 1.03) { const double m = std::sqrt (fHi * fLo); fLo = m / std::sqrt (1.03); fHi = m * std::sqrt (1.03); }
    if (fHi / fLo > 2.2) { setStatus ("That box covers more than an octave. Draw it closer round the note.", true); view->clearSelection(); return; }
    cand = Cand(); cand.t0 = t0; cand.t1 = t1; cand.fLo = fLo; cand.fHi = fHi;
    midiLocked = false; measure();
}

void ReHarmoniserEditor::measure()
{
    if (orig == nullptr) return;
    const double t0 = cand.t0, t1 = cand.t1, fLo = cand.fLo, fHi = cand.fHi; const int mode = refModeId, part = partial; const double a4in = a4; const Vec vis = seeGainsDb();
    auto audio = orig; const int rate = sr; const double total = dur;
    struct Res { Vec ts, f0; int hint = 1; double a4 = 440.0; bool refFound = false; };
    auto res = std::make_shared<Res>();
    measuring = true; tipLabel.setVisible (false); tipUse.setVisible (false); tipIgnore.setVisible (false);
    startJob ("Measuring the note", [=] (Worker& w) -> juce::String
    {
        auto slice = [&] (double a, double b, double& s0)
        {
            const size_t n = (*audio)[0].size();
            const size_t i0 = (size_t) juce::jmax (0.0, std::floor (a * rate)), i1 = juce::jmin (n, (size_t) std::floor (b * rate));
            Block o (audio->size()); for (size_t c = 0; c < audio->size(); ++c) o[c].assign ((*audio)[c].begin() + (long) i0, (*audio)[c].begin() + (long) i1);
            s0 = (double) i0 / rate; return o;
        };
        auto h = w.hooks(); res->a4 = a4in;
        if (mode == 1 || mode == 2)
        {
            double r0 = mode == 1 ? juce::jmax (0.0, t0 - 10.0) : 0.0, r1 = mode == 1 ? juce::jmin (total, t1 + 10.0) : total;
            if (r1 - r0 >= 2.0)
            {
                double s0r; Block b = slice (r0, r1, s0r);
                try { const auto rp = refPoints (spansOf (b), rate, s0r, {}, 4, h); const auto st = refStats (rp.D, rp.W); if (st.ok) { res->a4 = st.a4; res->refFound = true; } }
                catch (const Cancelled&) { throw; } catch (...) {}
            }
        }
        h.report ("Measuring the note", 0.1);
        double s0; Block seg = slice (t0 - 0.8, t1 + 0.8, s0);
        const auto tk = trackingInputs (spansOf (seg), rate, fLo, fHi, vis);
        std::vector<Span> chans; for (int c : tk.channels) chans.push_back ({ seg[(size_t) c].data(), seg[(size_t) c].size() });
        auto tr = trackNote (chans, tk.weights, rate, s0, t0, t1, fLo, fHi);
        h.report ("Measuring the note", 0.6); h.check();
        res->f0 = refineTrack (chans, tk.weights, rate, s0, tr.ts, tr.f0); res->ts = tr.ts;
        res->hint = part == 1 ? detectPartial (chans, tk.weights, rate, s0, res->ts, res->f0) : 1;
        return {};
    },
    [this, res] (const juce::String& err)
    {
        measuring = false;
        if (err.isNotEmpty()) { cand.has = false; view->clearSelection(); setStatus (err == "Cancelled." ? err : err, err != "Cancelled."); updateInfo(); view->repaint(); return; }
        cand.ts = res->ts; cand.f0 = res->f0; cand.hint = res->hint;
        if (res->refFound) { a4 = res->a4; a4Box.setText (juce::String (a4, 1), false); refInfo.setText ("Measured: A4 " + juce::String (a4, 1) + " Hz", juce::dontSendNotification); view->dirty = true; }
        if (! midiLocked) cand.midi = (int) std::lround (midiOf (median (cand.f0), a4));
        cand.has = true; inHistory = true; syncSliders(); inHistory = false; hist.clear(); hist.push_back ({ params }); histPos = 0;
        replan(); view->setSelection (cand.t0, cand.t1, midiOf (cand.fLo * partial, a4), midiOf (cand.fHi * partial, a4));
        view->showNote (cand.t0, cand.t1, cand.f0.empty() ? cand.fLo : median (cand.f0));
        auditionStale = true; openSection (1); setStatus ("Measured. Change Move whole note or Snap to note, then Audition (it gets ready; press Space to listen) and Apply correction.");
    });
}

void ReHarmoniserEditor::replan()
{
    if (! cand.has) { updateInfo(); updateButtons(); return; }
    params.a4 = a4;
    cand.plan = planCurve (cand.ts, cand.f0, params, cand.midi);
    nameBox.setText (noteName (cand.midi), false);
    if (hostState == HostState::CorrectedWithPending) auditionStale = true;
    updateInfo(); updateButtons(); view->repaint();
}

void ReHarmoniserEditor::updateInfo()
{
    if (orig == nullptr) { noteInfo.setText ("", juce::dontSendNotification); return; }
    if (measuring) { noteInfo.setText ("Measuring the note...", juce::dontSendNotification); return; }
    if (! cand.has) { noteInfo.setText ("Drag a box round a note\non the picture.", juce::dontSendNotification); return; }
    const double med = median (cand.plan.c), after = median (cand.plan.planned);
    Vec wob = cand.plan.wob; double wmax = 0; for (double v : wob) wmax = juce::jmax (wmax, std::abs (v));
    juce::String t = noteName (cand.midi) + "  " + juce::String (midiHz (cand.midi, a4), 1) + " Hz   (" + fmtTime (cand.t0) + " - " + fmtTime (cand.t1) + ")\n"
                   + "Sung " + juce::String (med, 1) + " c   Wobble +/-" + juce::String (wmax, 0) + " c\n"
                   + "Planned " + juce::String (after, 1) + " c   (A4 " + juce::String (a4, 1) + " Hz)";
    if (cand.plan.warning.size() > 0) t += "\n" + juce::String (cand.plan.warning);
    noteInfo.setText (t, juce::dontSendNotification);
    const bool tip = cand.hint > 1 && partial == 1;
    tipLabel.setVisible (tip); tipUse.setVisible (tip); tipIgnore.setVisible (tip);
    if (tip) { tipLabel.setText ("This looks like its " + juce::String (cand.hint) + (cand.hint == 2 ? "nd" : cand.hint == 3 ? "rd" : "th") + " line, not the note itself.", juce::dontSendNotification); tipUse.setButtonText ("Use the note"); }
}

void ReHarmoniserEditor::detectReference()
{
    if (orig == nullptr || busy) return;
    auto audio = orig; const int rate = sr;
    auto res = std::make_shared<RefStats>();
    startJob ("Measuring the choir's pitch", [audio, rate, res] (Worker& w) -> juce::String
    {
        const auto rp = refPoints (spansOf (*audio), rate, 0.0, {}, 4, w.hooks()); *res = refStats (rp.D, rp.W); return {};
    },
    [this, res] (const juce::String& err)
    {
        if (err.isNotEmpty()) { setStatus (err, err != "Cancelled."); refInfo.setText ("", juce::dontSendNotification); return; }
        if (! res->ok) { setStatus ("Could not find enough steady notes to measure the pitch.", true); return; }
        a4 = res->a4; a4Box.setText (juce::String (a4, 1), false); refMode.setSelectedId (3, juce::dontSendNotification); refModeId = 3;
        refInfo.setText ("Measured: A4 " + juce::String (a4, 1) + " Hz", juce::dontSendNotification);
        if (cand.has && ! midiLocked) cand.midi = (int) std::lround (midiOf (median (cand.f0), a4));
        view->dirty = true; replan(); setStatus ("The choir's A4 is " + juce::String (a4, 1) + " Hz. Notes are now measured from that.");
    });
}

void ReHarmoniserEditor::sliderChanged()
{
    if (inHistory) return;
    params.move = moveRow.s.getValue(); params.snap = snapRow.s.getValue() / 100.0; params.strength = strengthRow.s.getValue() / 100.0;
    params.keep = keepRow.s.getValue() / 100.0; params.smooth = smoothRow.s.getValue(); params.ease = easeRow.s.getValue(); params.hold = holdRow.s.getValue();
    params.bw = bwRow.s.getValue(); params.match = matchBox.getToggleState();
    if (cand.has) { hist.resize (histPos + 1); hist.push_back ({ params }); histPos = hist.size() - 1; }
    replan();
}

void ReHarmoniserEditor::syncSliders()
{
    moveRow.s.setValue (params.move, juce::dontSendNotification); snapRow.s.setValue (params.snap * 100.0, juce::dontSendNotification);
    strengthRow.s.setValue (params.strength * 100.0, juce::dontSendNotification); keepRow.s.setValue (params.keep * 100.0, juce::dontSendNotification);
    smoothRow.s.setValue (params.smooth, juce::dontSendNotification); easeRow.s.setValue (params.ease, juce::dontSendNotification);
    holdRow.s.setValue (params.hold, juce::dontSendNotification); bwRow.s.setValue (params.bw, juce::dontSendNotification);
    matchBox.setToggleState (params.match, juce::dontSendNotification);
}

// ---------------------------------------------------------------------------------------------------- rendering and the host
void ReHarmoniserEditor::renderAndSet (bool withPending, std::function<void()> then)
{
    if (orig == nullptr || (busy && analysing)) return;
    std::vector<NoteEdit> list;
    for (auto& e : edits) if (e.enabled) list.push_back (e);
    if (withPending && vcand.has && vcand.preview)
    {
        NoteEdit p; if (makeVoiceEdit (p)) list.push_back (std::move (p));
    }
    if (withPending && bcand.has)
    {
        NoteEdit p; p.isBrush = true; p.brush = bcand.b; list.push_back (std::move (p));
    }
    if (withPending && cand.has)
    {
        NoteEdit p; p.t0 = cand.t0; p.t1 = cand.t1; p.fLo = cand.fLo; p.fHi = cand.fHi; p.midi = cand.midi; p.a4 = a4; p.params = params; p.ts = cand.ts; p.f0 = cand.f0; p.off = cand.plan.off;
        list.push_back (std::move (p));
    }
    originalMode = false; originalBtn.setToggleState (false, juce::dontSendNotification);
    if (list.empty())
    {
        hostState = HostState::Original;
        if (host.setCorrected) host.setCorrected (nullptr, [this, then] (const juce::String& err) { if (err.isNotEmpty()) setStatus (err, true); else if (then) then(); });
        else if (then) then();
        return;
    }
    auto audio = orig; const int rate = sr; const double total = dur;
    auto out = std::make_shared<Block>();
    const auto newState = withPending && (cand.has || bcand.has || (vcand.has && vcand.preview)) ? HostState::CorrectedWithPending : HostState::Corrected;
    startJob ("Correcting the notes", [audio, rate, total, list, out] (Worker& w) -> juce::String
    {
        *out = *audio; auto h = w.hooks();
        for (size_t k = 0; k < list.size(); ++k)
        {
            const auto& e = list[k];
            if (e.isVoices)
            {
                const double a0 = juce::jmax (0.0, e.vs.t0 - 0.8), b0 = juce::jmin (total, e.vs.t1 + 0.8);
                const size_t j0 = (size_t) std::floor (a0 * rate), j1 = juce::jmin ((*out)[0].size(), (size_t) std::floor (b0 * rate));
                if (j1 <= j0 + 16) continue;
                Block sg ((*out).size()); for (size_t c = 0; c < sg.size(); ++c) sg[c].assign ((*out)[c].begin() + (long) j0, (*out)[c].begin() + (long) j1);
                Hooks sb; sb.cancelled = h.cancelled;
                sb.progress = [&] (const char*, double f) { h.report ("Correcting the notes", ((double) k + f) / (double) list.size()); };
                const auto trk = voiceInputs (spansOf (sg), rate, e.vs.fT, e.vis);
                const Block done = retuneVoices (sg, rate, (double) j0 / rate, e.vs.fT, e.vs.ts, e.vs.C, e.offs, e.vs.t0, e.vs.t1, trk, sb);
                for (size_t c = 0; c < done.size(); ++c) std::copy (done[c].begin(), done[c].end(), (*out)[c].begin() + (long) j0);
                continue;
            }
            if (e.isBrush)
            {
                const auto rg = brushRegion (e.brush);
                const double a0 = juce::jmax (0.0, rg.first - 0.3), b0 = juce::jmin (total, rg.second + 0.3);
                const size_t j0 = (size_t) std::floor (a0 * rate), j1 = juce::jmin ((*out)[0].size(), (size_t) std::floor (b0 * rate));
                if (j1 <= j0 + 16) continue;
                Block sg ((*out).size()); for (size_t c = 0; c < sg.size(); ++c) sg[c].assign ((*out)[c].begin() + (long) j0, (*out)[c].begin() + (long) j1);
                Hooks sb; sb.cancelled = h.cancelled;
                sb.progress = [&] (const char*, double f) { h.report ("Correcting the notes", ((double) k + f) / (double) list.size()); };
                const Block done = brushSegment (sg, rate, (double) j0 / rate, e.brush, 10, sb);
                for (size_t c = 0; c < done.size(); ++c) std::copy (done[c].begin(), done[c].end(), (*out)[c].begin() + (long) j0);
                continue;
            }
            const double a = juce::jmax (0.0, e.ts.front() - 0.15 - 0.3), b = juce::jmin (total, e.ts.back() + e.params.hold + e.params.fadeOut + 0.3);
            const size_t i0 = (size_t) std::floor (a * rate), i1 = juce::jmin ((*out)[0].size(), (size_t) std::floor (b * rate));
            if (i1 <= i0 + 16) continue;
            Block seg ((*out).size()); for (size_t c = 0; c < seg.size(); ++c) seg[c].assign ((*out)[c].begin() + (long) i0, (*out)[c].begin() + (long) i1);
            Hooks sub; sub.cancelled = h.cancelled;
            sub.progress = [&] (const char*, double f) { h.report ("Correcting the notes", ((double) k + f) / (double) list.size()); };
            const Block fixed = retuneSegment (seg, rate, (double) i0 / rate, e.ts, e.f0, e.off, e.params, sub);
            for (size_t c = 0; c < fixed.size(); ++c) std::copy (fixed[c].begin(), fixed[c].end(), (*out)[c].begin() + (long) i0);
        }
        return {};
    },
    [this, out, newState, then] (const juce::String& err)
    {
        if (err.isNotEmpty()) { setStatus (err == "Cancelled." ? err : "Could not correct the note: " + err, err != "Cancelled."); return; }
        hostState = newState; auditionStale = false;
        if (host.setCorrected)
        {
            hostBusy = true;
            host.setCorrected (out, [this, then] (const juce::String& e2)
            {
                hostBusy = false; updateButtons();
                if (e2.isNotEmpty()) { setStatus (e2, true); return; }
                if (then) then();
            });
        }
        else if (then) then();
    });
}

void ReHarmoniserEditor::applyCorrection()
{
    if (! cand.has || busy) return;
    NoteEdit e; e.id = nextId++; e.t0 = cand.t0; e.t1 = cand.t1; e.fLo = cand.fLo; e.fHi = cand.fHi; e.midi = cand.midi; e.a4 = a4;
    e.params = params; e.ts = cand.ts; e.f0 = cand.f0; e.off = cand.plan.off;
    e.label = noteName (e.midi) + " at " + fmtTime (e.t0) + "   " + juce::String (median (cand.plan.c), 1) + " > " + juce::String (median (cand.plan.planned), 1) + " c";
    edits.push_back (e); undone.clear();
    cand = Cand(); view->clearSelection(); nameBox.setText ("", false); hist.clear(); histPos = 0;
    rebuildEditsList(); updateInfo();
    renderAndSet (false, [this] { setStatus ("Applied and added to Corrected Notes. Press Space to listen; Original compares."); });
}

void ReHarmoniserEditor::rebuildVoiceRows()
{
    voiceRows.clear();
    if (vcand.has)
        for (size_t j = 0; j < vcand.vs.C.size(); ++j)
        {
            auto r = std::make_unique<VoiceRowC>();
            double mean = 0; for (double c : vcand.vs.C[j]) mean += c; mean /= (double) juce::jmax ((size_t) 1, vcand.vs.C[j].size());
            r->col = juce::Colour (kVoiceCols[j % 8]); r->strength = j < vcand.vs.strength.size() ? vcand.vs.strength[j] : 0.5;
            r->text = (mean >= 0 ? "+" : "-") + juce::String (std::abs (mean), 0) + " c";
            r->tick.setToggleState (std::abs (mean) > 6.0, juce::dontSendNotification);
            r->tick.onClick = [this] { if (vcand.preview) auditionStale = true; updateButtons(); };
            addChildComponent (*r);
            voiceRows.push_back (std::move (r));
        }
    resized(); view->repaint();
}

void ReHarmoniserEditor::findVoices()
{
    if (busy) return;
    if (! cand.has) { setStatus ("Pick a note first: draw a box round it, then press Find voices.", true); return; }
    const double t0 = cand.t0, t1 = cand.t1; const int midi = cand.midi; const double a4v = a4; const Vec vis = seeGainsDb();
    auto audio = orig; const int rate = sr; const double total = dur;
    auto out = std::make_shared<VoiceSet>();
    vcand = VCand(); rebuildVoiceRows();
    startJob ("Finding the voices", [=] (Worker& w) -> juce::String
    {
        const size_t n = (*audio)[0].size();
        const double a = juce::jmax (0.0, t0 - 0.6), b = juce::jmin (total, t1 + 0.6);
        const size_t i0 = (size_t) std::floor (a * rate), i1 = juce::jmin (n, (size_t) std::floor (b * rate));
        Block seg (audio->size()); for (size_t c = 0; c < audio->size(); ++c) seg[c].assign ((*audio)[c].begin() + (long) i0, (*audio)[c].begin() + (long) i1);
        *out = detectVoices (spansOf (seg), rate, (double) i0 / rate, t0, t1, midi, a4v, vis, w.hooks());
        return {};
    },
    [this, out] (const juce::String& err)
    {
        if (err.isNotEmpty()) { setStatus (err, err != "Cancelled."); return; }
        vcand.has = true; vcand.vs = *out; vcand.preview = false;
        rebuildVoiceRows();
        setStatus (juce::String ((int) out->C.size()) + " voice line(s) found. Tick the ones to move (the in-tune ones are left unticked) and press Preview move, then Apply correction.");
    });
}

bool ReHarmoniserEditor::makeVoiceEdit (NoteEdit& e) const
{
    if (! vcand.has || voiceRows.size() != vcand.vs.C.size()) return false;
    const auto means = vcand.vs.means();
    double tgt = 0.0;
    if (voiceTarget.getSelectedId() == 2 && ! vcand.vs.strength.empty())
    {
        size_t best = 0; for (size_t j = 1; j < vcand.vs.strength.size() && j < means.size(); ++j) if (vcand.vs.strength[j] > vcand.vs.strength[best]) best = j;
        tgt = means[best];
    }
    e.isVoices = true; e.vs = vcand.vs; e.vis = seeGainsDb(); e.vtarget = voiceTarget.getSelectedId() == 2 ? 1 : 0;
    e.offs.assign (means.size(), 0.0); e.vticks.assign (means.size(), false);
    bool any = false;
    for (size_t j = 0; j < means.size(); ++j) if (voiceRows[j]->tick.getToggleState()) { e.offs[j] = tgt - means[j]; e.vticks[j] = true; any = true; }
    return any;
}

void ReHarmoniserEditor::previewVoices()
{
    if (busy) return;
    NoteEdit e;
    if (! makeVoiceEdit (e)) { setStatus ("Tick the voices you want to move (press Find voices first).", true); return; }
    vcand.preview = true;
    renderAndSet (true, [this] { setStatus ("Voices moved. Press Space (or Play) to hear it, then Apply correction to keep it. Original compares."); });
}

void ReHarmoniserEditor::applyVoices()
{
    if (busy) return;
    NoteEdit e;
    if (! makeVoiceEdit (e)) { setStatus ("Nothing to apply: press Find voices, tick the voices to move, then Apply correction.", true); return; }
    e.id = nextId++;
    int moved = 0; for (bool b : e.vticks) if (b) ++moved;
    e.t0 = e.vs.t0; e.t1 = e.vs.t1;
    e.label = "Voices at " + fmtTime (e.t0) + "   " + juce::String (moved) + " moved";
    edits.push_back (e); undone.clear();
    vcand = VCand(); rebuildVoiceRows();
    rebuildEditsList(); updateInfo();
    renderAndSet (false, [this] { setStatus ("Voice correction applied and added to Corrected Notes. Press Space to listen; Original compares."); });
}

void ReHarmoniserEditor::discardVoices()
{
    const bool wasHeard = vcand.preview && hostState == HostState::CorrectedWithPending;
    vcand = VCand(); rebuildVoiceRows(); updateButtons();
    if (wasHeard) renderAndSet (cand.has || bcand.has);
}

void ReHarmoniserEditor::loadVoicesAsPending (const NoteEdit& e)
{
    vcand = VCand(); vcand.has = true; vcand.vs = e.vs; vcand.preview = true;
    voiceTarget.setSelectedId (e.vtarget == 1 ? 2 : 1, juce::dontSendNotification);
    rebuildVoiceRows();
    for (size_t j = 0; j < voiceRows.size() && j < e.vticks.size(); ++j) voiceRows[j]->tick.setToggleState (e.vticks[j], juce::dontSendNotification);
    rebuildEditsList(); openSection (2); setStatus ("Taken back out of Corrected Notes. Change the ticks, then Apply correction again.");
    renderAndSet (true);
}

void ReHarmoniserEditor::strokeBegin()
{
    if (busy) return;
    bcand = BCand(); bcand.has = false;
    bcand.b.rt = brushTRow.s.getValue(); bcand.b.rc = brushCRow.s.getValue(); bcand.b.amount = brushAmtRow.s.getValue() / 100.0; bcand.b.harm = brushHarm.getToggleState();
}

void ReHarmoniserEditor::strokePoint (double t, double hz)
{
    bcand.b.pts.push_back ({ t, hz });
    view->repaint();
}

void ReHarmoniserEditor::strokeEnd()
{
    if (bcand.b.pts.empty()) return;
    if (bcand.b.pts.size() == 1) bcand.b.pts.push_back ({ bcand.b.pts[0].first + 0.02, bcand.b.pts[0].second });
    bcand.has = true; auditionStale = true;
    setStatus ("Stroke painted. Change the radius or Amount if you like, then Audition (Space to listen) and Erase (apply).");
    updateButtons(); view->repaint();
}

void ReHarmoniserEditor::brushChanged()
{
    if (! bcand.has) return;
    bcand.b.rt = brushTRow.s.getValue(); bcand.b.rc = brushCRow.s.getValue(); bcand.b.amount = brushAmtRow.s.getValue() / 100.0; bcand.b.harm = brushHarm.getToggleState();
    if (hostState == HostState::CorrectedWithPending) auditionStale = true;
    view->repaint();
}

void ReHarmoniserEditor::applyBrush()
{
    if (! bcand.has || busy) return;
    NoteEdit e; e.id = nextId++; e.isBrush = true; e.brush = bcand.b;
    const auto rg = brushRegion (e.brush);
    e.t0 = rg.first; e.t1 = rg.second;
    e.label = "Erase at " + fmtTime (rg.first) + "   " + juce::String (rg.second - rg.first, 1) + " s";
    edits.push_back (e); undone.clear();
    bcand = BCand(); view->repaint();
    rebuildEditsList(); updateInfo();
    renderAndSet (false, [this] { setStatus ("Erased and added to Corrected Notes. Press Space to listen; Original compares."); });
}

void ReHarmoniserEditor::discardBrush()
{
    const bool wasHeard = hostState == HostState::CorrectedWithPending;
    bcand = BCand(); view->repaint(); updateButtons();
    if (wasHeard) renderAndSet (cand.has);
}

void ReHarmoniserEditor::loadBrushAsPending (const NoteEdit& e)
{
    bcand = BCand(); bcand.has = true; bcand.b = e.brush;
    brushTRow.s.setValue (e.brush.rt, juce::dontSendNotification); brushCRow.s.setValue (e.brush.rc, juce::dontSendNotification);
    brushAmtRow.s.setValue (e.brush.amount * 100.0, juce::dontSendNotification); brushHarm.setToggleState (e.brush.harm, juce::dontSendNotification);
    rebuildEditsList(); openSection (3); setStatus ("Taken back out of Corrected Notes. Change it, then Erase (apply) again.");
    renderAndSet (false);
}

void ReHarmoniserEditor::deselect()
{
    const bool wasHeard = hostState == HostState::CorrectedWithPending;
    cand = Cand(); view->clearSelection(); nameBox.setText ("", false); hist.clear(); histPos = 0; measuring = false;
    updateInfo(); updateButtons();
    if (wasHeard) renderAndSet (false);
}

void ReHarmoniserEditor::clearChanges()
{
    params = Params(); inHistory = true; syncSliders(); inHistory = false;
    if (cand.has) { hist.clear(); hist.push_back ({ params }); histPos = 0; replan(); } else updateButtons();
}

void ReHarmoniserEditor::pushHistory() {}

void ReHarmoniserEditor::undoStep()
{
    if (cand.has && histPos > 0)
    {
        --histPos; params = hist[histPos].p; inHistory = true; syncSliders(); inHistory = false; replan(); return;
    }
    if (! cand.has && ! edits.empty())
    {
        undone.push_back (edits.back()); edits.pop_back(); rebuildEditsList(); renderAndSet (false); updateButtons(); view->repaint();
    }
}

void ReHarmoniserEditor::redoStep()
{
    if (cand.has && histPos + 1 < hist.size())
    {
        ++histPos; params = hist[histPos].p; inHistory = true; syncSliders(); inHistory = false; replan(); return;
    }
    if (! cand.has && ! undone.empty())
    {
        edits.push_back (undone.back()); undone.pop_back(); rebuildEditsList(); renderAndSet (false); updateButtons(); view->repaint();
    }
}

void ReHarmoniserEditor::loadAsPending (const NoteEdit& e)
{
    cand = Cand(); cand.has = true; cand.t0 = e.t0; cand.t1 = e.t1; cand.fLo = e.fLo; cand.fHi = e.fHi; cand.ts = e.ts; cand.f0 = e.f0; cand.midi = e.midi;
    a4 = e.a4; a4Box.setText (juce::String (a4, 1), false); params = e.params; midiLocked = true; partial = 1; partialBox.setSelectedId (1, juce::dontSendNotification);
    inHistory = true; syncSliders(); inHistory = false; hist.clear(); hist.push_back ({ params }); histPos = 0;
    replan(); view->setSelection (cand.t0, cand.t1, midiOf (cand.fLo, a4), midiOf (cand.fHi, a4));
    rebuildEditsList(); openSection (1); setStatus ("Taken back out of Corrected Notes. Change it, then Apply correction again.");
    renderAndSet (false);
}

void ReHarmoniserEditor::rebuildEditsList()
{
    editsList->setSize (juce::jmax (100, editsView.getWidth() - 14), editsList->getHeight()); editsList->rebuild();
    sections[5]->header.setButtonText ("Corrected Notes" + juce::String (edits.empty() ? "" : " (" + juce::String ((int) edits.size()) + ")"));
}

void ReHarmoniserEditor::audition()
{
    if (! cand.has || busy) return;
    renderAndSet (true, [this] { setStatus ("Ready: press Space (or Play) to hear the correction. Original plays without the changes."); });
}

void ReHarmoniserEditor::toggleOriginal()
{
    if (busy) return;
    originalMode = originalBtn.getToggleState();
    if (originalMode)
    {
        if (host.setCorrected) host.setCorrected (nullptr, [this] (const juce::String& e) { if (e.isNotEmpty()) setStatus (e, true); else setStatus ("Original: none of the changes are heard."); });
    }
    else renderAndSet (hostState == HostState::CorrectedWithPending, [this] { setStatus ("The changes are heard again."); });
}

void ReHarmoniserEditor::togglePlay()
{
    if (orig == nullptr || busy || hostBusy) return;
    if (playT >= 0) { if (host.stop) host.stop(); return; }
    // a correction that was changed after the last Audition has to be made again before it can be heard
    if (hostState == HostState::CorrectedWithPending && auditionStale && (cand.has || bcand.has || vcand.has)) { renderAndSet (true, [this] { togglePlay(); }); return; }
    const bool useLoop = looping && loopIn >= 0 && loopOut > loopIn;
    const double from = useLoop ? loopIn : cueT, to = useLoop ? loopOut : dur;
    if (host.play) host.play (from, to, looping);
}

void ReHarmoniserEditor::writeBack()
{
    if (busy || hostBusy) return;
    if (cand.has || bcand.has || (vcand.has && vcand.preview)) { setStatus ("A correction is not applied yet: press Apply correction / Erase (apply), or Deselect / Discard stroke, first.", true); return; }
    if (! hasAppliedChanges()) { setStatus ("Nothing has been changed yet. Press Cancel to close.", true); return; }
    renderAndSet (false, [this] { if (host.writeBack) host.writeBack(); });
}

// ---------------------------------------------------------------------------------------------------- the layout
void ReHarmoniserEditor::setChannelNames (const juce::StringArray& n)
{
    chanNames = n;
    if (orig != nullptr) rebuildSeeStrips();
}

void ReHarmoniserEditor::rebuildSeeStrips()
{
    seeStrips.clear(); seeHolder.removeAllChildren();
    const size_t n = orig != nullptr ? orig->size() : 0;
    for (size_t c = 0; c < n; ++c)
    {
        auto st = std::make_unique<SeeStrip>();
        st->name.setText ((int) c < chanNames.size() ? chanNames[(int) c] : "Ch " + juce::String ((int) c + 1), juce::dontSendNotification);
        styleLabel (st->name, cText, 12.0f);
        styleButton (st->m); styleButton (st->s); st->m.setClickingTogglesState (true); st->s.setClickingTogglesState (true);
        st->m.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb04a3a)); st->s.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc9a227));
        st->m.setTooltip ("Hide this channel from the picture (and from measuring). It does not change what you hear.");
        st->s.setTooltip ("Show only the soloed channels in the picture (and measure with them). It does not change what you hear.");
        styleSlider (st->g, -24, 12, 0.5, 0.0, " dB"); st->g.setDoubleClickReturnValue (true, 0.0);
        st->g.setTooltip ("How bright this channel is in the picture. It does not change what you hear.");
        st->m.onClick = [this] { seeChanged(); }; st->s.onClick = [this] { seeChanged(); }; st->g.onValueChange = [this] { seeChanged(); };
        seeHolder.addAndMakeVisible (st->name); seeHolder.addAndMakeVisible (st->m); seeHolder.addAndMakeVisible (st->s); seeHolder.addAndMakeVisible (st->g);
        seeStrips.push_back (std::move (st));
    }
    layoutSeeStrips();
}

void ReHarmoniserEditor::layoutSeeStrips()
{
    const int w = juce::jmax (150, seeView.getWidth() - (seeView.isVerticalScrollBarShown() ? 14 : 0));
    seeHolder.setSize (w, (int) seeStrips.size() * 28 + 4);
    int y = 2;
    for (auto& st : seeStrips)
    {
        auto r = juce::Rectangle<int> (0, y, w, 26); y += 28;
        st->name.setBounds (r.removeFromLeft (62)); st->m.setBounds (r.removeFromLeft (26).reduced (0, 2)); r.removeFromLeft (2); st->s.setBounds (r.removeFromLeft (26).reduced (0, 2)); r.removeFromLeft (4);
        st->g.setBounds (r);
    }
}

Vec ReHarmoniserEditor::seeGainsDb() const
{
    bool anySolo = false, anyShown = false;
    for (auto& st : seeStrips) if (st->s.getToggleState()) anySolo = true;
    Vec g;
    for (auto& st : seeStrips)
    {
        const bool shown = ! st->m.getToggleState() && (! anySolo || st->s.getToggleState());
        if (shown) anyShown = true;
        g.push_back (shown ? st->g.getValue() : -100.0);
    }
    if (! anyShown) return {};                         // everything hidden: treat as flat rather than a blank picture
    bool flat = true; for (double v : g) if (v != 0.0) flat = false;
    if (flat) return {};
    return g;
}

void ReHarmoniserEditor::seeChanged() { seeDirty = true; seeDirtyAt = juce::Time::getMillisecondCounter(); }

void ReHarmoniserEditor::recombine()
{
    if (spec == nullptr || busy) return;
    seeDirty = false;
    Vec lin;
    { const auto db = seeGainsDb(); lin.assign (spec->gain.size(), 1.0); for (size_t c = 0; c < db.size() && c < lin.size(); ++c) lin[c] = db[c] <= -99.0 ? 0.0 : std::pow (10.0, db[c] / 20.0); }
    auto sp = spec; auto res = std::make_shared<std::pair<std::shared_ptr<Mix>, double>>();
    startJob ("Updating the picture", [sp, lin, res] (Worker&) -> juce::String
    {
        sp->setGains (lin);
        *res = { makeMix (*sp), sp->ref };
        return {};
    },
    [this, res] (const juce::String& err)
    {
        if (err.isNotEmpty()) { setStatus (err, true); return; }
        mix = res->first; refDb = res->second; view->dirty = true; view->repaint();
        setStatus ("The picture is updated. The See mixer only changes the picture, not what you hear.");
    });
}

void ReHarmoniserEditor::openSection (int idx)
{
    openIdx = idx; layoutSections(); repaint();
}

void ReHarmoniserEditor::updateButtons()
{
    const bool idle = ! busy && ! hostBusy;
    const bool haveAudio = orig != nullptr && mix != nullptr;
    playBtn.setEnabled (haveAudio && idle); stopBtn.setEnabled (haveAudio);
    originalBtn.setEnabled (idle && (hasAppliedChanges() || cand.has || bcand.has)); 
    voiceFindBtn.setEnabled (idle && cand.has); voicePreviewBtn.setEnabled (idle && vcand.has); voiceApplyBtn.setEnabled (idle && vcand.has); voiceDiscardBtn.setEnabled (vcand.has);
    brushAuditionBtn.setEnabled (idle && bcand.has); brushApplyBtn.setEnabled (idle && bcand.has); brushDiscardBtn.setEnabled (bcand.has);
    applyBtn.setEnabled (idle && cand.has); auditionBtn.setEnabled (idle && cand.has);
    deselectBtn.setEnabled (cand.has); clearBtn.setEnabled (cand.has);
    undoBtn.setEnabled (idle && ((cand.has && histPos > 0) || (! cand.has && ! edits.empty())));
    redoBtn.setEnabled (idle && ((cand.has && histPos + 1 < hist.size()) || (! cand.has && ! undone.empty())));
    detectBtn.setEnabled (idle && haveAudio); remeasureBtn.setEnabled (idle && cand.has); upBtn.setEnabled (cand.has); downBtn.setEnabled (cand.has);
    writeBtn.setEnabled (idle && haveAudio);
}

void ReHarmoniserEditor::layoutSections()
{
    // (the right-hand panel is laid out by resized(); this just repeats it when a section opens or closes)
    resized();
}

void ReHarmoniserEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto bottom = r.removeFromBottom (64); r.removeFromBottom (6);
    auto side = r.removeFromRight (310); r.removeFromRight (8);
    view->setBounds (r);
    // transport
    auto t = bottom.removeFromTop (30);
    playBtn.setBounds (t.removeFromLeft (80)); t.removeFromLeft (6); stopBtn.setBounds (t.removeFromLeft (66)); t.removeFromLeft (6);
    loopBtn.setBounds (t.removeFromLeft (66)); t.removeFromLeft (16); originalBtn.setBounds (t.removeFromLeft (100)); t.removeFromLeft (16);
    cancelBtn.setBounds (t.removeFromRight (90)); t.removeFromRight (8); writeBtn.setBounds (t.removeFromRight (170)); t.removeFromRight (16);
    bar.setBounds (t.reduced (0, 5));
    status.setBounds (bottom.reduced (0, 2));
    // right-hand panel
    auto s = side;
    // Note Selection
    { auto b = s.removeFromTop (28); upBtn.setBounds (b.removeFromRight (30)); b.removeFromRight (4); downBtn.setBounds (b.removeFromRight (30)); b.removeFromRight (6);
      nameCap.setBounds (b.removeFromLeft (80)); nameBox.setBounds (b.reduced (0, 1)); }
    s.removeFromTop (4);
    noteInfo.setBounds (s.removeFromTop (66));
    { const bool tip = tipLabel.isVisible(); auto b = s.removeFromTop (tip ? 44 : 0);
      if (tip) { tipLabel.setBounds (b.removeFromTop (18)); tipUse.setBounds (b.removeFromLeft (110).reduced (0, 1)); b.removeFromLeft (6); tipIgnore.setBounds (b.removeFromLeft (80).reduced (0, 1)); } }
    remeasureBtn.setBounds (s.removeFromTop (26).removeFromLeft (130)); s.removeFromTop (6);
    // the View block at the bottom
    const int viewH = 150;
    auto viewArea = s.removeFromBottom (viewH); s.removeFromBottom (6);
    { auto a = viewArea; a.removeFromTop (2);
      auto z = a.removeFromTop (24); zoomCap.setBounds (z.removeFromLeft (120)); zoomTIn.setBounds (z.removeFromLeft (28)); z.removeFromLeft (2); zoomTOut.setBounds (z.removeFromLeft (28)); z.removeFromLeft (14);
      zoomPIn.setBounds (z.removeFromLeft (28)); z.removeFromLeft (2); zoomPOut.setBounds (z.removeFromLeft (28));
      a.removeFromTop (3); loopClearBtn.setBounds (a.removeFromTop (24).removeFromLeft (110)); a.removeFromTop (3);
      auto l1 = a.removeFromTop (26); linesRow.cap.setBounds (l1.removeFromLeft (92)); linesRow.s.setBounds (l1);
      auto l2 = a.removeFromTop (26); gainRow.cap.setBounds (l2.removeFromLeft (92)); gainRow.s.setBounds (l2);
      auto l3 = a.removeFromTop (26); keyRow.cap.setBounds (l3.removeFromLeft (92)); keyRow.s.setBounds (l3); }
    // the sections: headers always, the open one's body between them
    auto place = [] (juce::Component& c, juce::Rectangle<int>& a, int h) { c.setBounds (a.removeFromTop (h)); };
    for (auto* k : { (juce::Component*) &a4Cap, (juce::Component*) &a4Box, (juce::Component*) &detectBtn, (juce::Component*) &refMode, (juce::Component*) &refInfo }) k->setVisible (openIdx == 0);
    for (int i = 0; i < (int) sections.size(); ++i)
    {
        auto& sec = *sections[(size_t) i];
        sec.header.setBounds (s.removeFromTop (26)); s.removeFromTop (2);
        const bool open = openIdx == i;
        if (i == 1)
        {
            const bool adv = advancedBtn.getToggleState();
            for (auto* k : sec.kids) k->setVisible (open);
            for (auto* k : { (juce::Component*) &strengthRow.cap, (juce::Component*) &strengthRow.s, (juce::Component*) &keepRow.cap, (juce::Component*) &keepRow.s, (juce::Component*) &smoothRow.cap, (juce::Component*) &smoothRow.s,
                             (juce::Component*) &easeRow.cap, (juce::Component*) &easeRow.s, (juce::Component*) &holdRow.cap, (juce::Component*) &holdRow.s, (juce::Component*) &bwRow.cap, (juce::Component*) &bwRow.s,
                             (juce::Component*) &partialBox, (juce::Component*) &matchBox }) k->setVisible (open && adv);
            if (open)
            {
                const int h = 26 * 2 + 26 + (adv ? 26 * 6 + 26 + 24 : 0) + 30 + 30 + 6;
                auto b = s.removeFromTop (juce::jmin (h, s.getHeight()));
                auto rw = [&] (Row& rr) { auto q = b.removeFromTop (26); rr.cap.setBounds (q.removeFromLeft (112)); rr.s.setBounds (q); };
                rw (moveRow); rw (snapRow);
                advancedBtn.setBounds (b.removeFromTop (26).removeFromLeft (120));
                if (adv) { rw (strengthRow); rw (keepRow); rw (smoothRow); rw (easeRow); rw (holdRow); rw (bwRow); partialBox.setBounds (b.removeFromTop (26).reduced (0, 1)); matchBox.setBounds (b.removeFromTop (24)); }
                b.removeFromTop (4);
                auto b1 = b.removeFromTop (28); auditionBtn.setBounds (b1.removeFromLeft (84)); b1.removeFromLeft (4); applyBtn.setBounds (b1.removeFromLeft (118)); b1.removeFromLeft (4); deselectBtn.setBounds (b1);
                b.removeFromTop (2);
                auto b2 = b.removeFromTop (28); clearBtn.setBounds (b2.removeFromLeft (110)); b2.removeFromLeft (4); undoBtn.setBounds (b2.removeFromLeft (80)); b2.removeFromLeft (4); redoBtn.setBounds (b2);
            }
            s.removeFromTop (open ? 4 : 0);
        }
        else if (i == 0)
        {
            if (open)
            {
                auto b = s.removeFromTop (juce::jmin (sec.bodyH, s.getHeight()));
                auto q = b.removeFromTop (28); a4Cap.setBounds (q.removeFromLeft (92)); detectBtn.setBounds (q.removeFromRight (70)); q.removeFromRight (4); a4Box.setBounds (q.reduced (0, 1));
                b.removeFromTop (3); refMode.setBounds (b.removeFromTop (26)); b.removeFromTop (3); refInfo.setBounds (b.removeFromTop (24));
                s.removeFromTop (4);
            }
        }
        else if (i == 3)
        {
            for (auto* k : sec.kids) k->setVisible (open);
            if (open)
            {
                auto b = s.removeFromTop (juce::jmin (36 + 26 * 3 + 26 + 30 + 8, s.getHeight()));
                brushHelp.setBounds (b.removeFromTop (48)); 
                auto rw = [&] (Row& rr) { auto q = b.removeFromTop (26); rr.cap.setBounds (q.removeFromLeft (92)); rr.s.setBounds (q); };
                rw (brushTRow); rw (brushCRow); rw (brushAmtRow);
                brushHarm.setBounds (b.removeFromTop (24)); b.removeFromTop (4);
                auto b1 = b.removeFromTop (28); brushAuditionBtn.setBounds (b1.removeFromLeft (84)); b1.removeFromLeft (4); brushApplyBtn.setBounds (b1.removeFromLeft (110)); b1.removeFromLeft (4); brushDiscardBtn.setBounds (b1);
                s.removeFromTop (4);
            }
        }
        else if (i == 2)
        {
            for (auto* k : sec.kids) k->setVisible (open);
            for (auto& r : voiceRows) r->setVisible (open);
            if (open)
            {
                auto b = s.removeFromTop (juce::jmin (50 + 28 + 28 + (int) voiceRows.size() * 24 + 34, s.getHeight()));
                voiceHelp.setBounds (b.removeFromTop (50));
                auto b0 = b.removeFromTop (28); voiceFindBtn.setBounds (b0.removeFromLeft (100)); b0.removeFromLeft (4); voicePreviewBtn.setBounds (b0.removeFromLeft (110));
                voiceTarget.setBounds (b.removeFromTop (28).reduced (0, 1));
                for (auto& r : voiceRows) r->setBounds (b.removeFromTop (24));
                b.removeFromTop (4);
                auto b1 = b.removeFromTop (28); voiceApplyBtn.setBounds (b1.removeFromLeft (130)); b1.removeFromLeft (4); voiceDiscardBtn.setBounds (b1.removeFromLeft (90));
                s.removeFromTop (4);
            }
        }
        else if (i == 4)
        {
            seeView.setVisible (open);
            if (open)
            {
                const int want = (int) seeStrips.size() * 28 + 6;
                const int h = juce::jlimit (40, juce::jmax (40, s.getHeight() - 70), want);
                seeView.setBounds (s.removeFromTop (h)); s.removeFromTop (4);
                layoutSeeStrips();
            }
        }
        else
        {
            editsView.setVisible (open);
            if (open)
            {
                const int rest = s.getHeight();
                editsView.setBounds (s.removeFromTop (juce::jmax (40, rest))); editsList->setSize (juce::jmax (100, editsView.getWidth() - 14), editsList->getHeight()); editsList->rebuild();
            }
        }
    }
    juce::ignoreUnused (place);
}

void ReHarmoniserEditor::paint (juce::Graphics& g)
{
    g.fillAll (cPanel);
    g.setColour (cLine); g.drawRect (getLocalBounds(), 1);
}

bool ReHarmoniserEditor::keyPressed (const juce::KeyPress& k)
{
    const auto c = k.getTextCharacter();
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (k.getModifiers().isCtrlDown() && (c == 'z' || c == 'Z' || k.getKeyCode() == 'Z')) { if (k.getModifiers().isShiftDown()) redoStep(); else undoStep(); return true; }
    if (c == '1') { loopIn = playT >= 0 ? playT : cueT; if (loopOut <= loopIn) loopOut = -1.0; view->repaint(); return true; }
    if (c == '2') { loopOut = playT >= 0 ? playT : cueT; view->repaint(); return true; }
    if (c == '5') { loopIn = loopOut = -1.0; view->repaint(); return true; }
    if (c == 'l' || c == 'L') { looping = ! looping; loopBtn.setToggleState (looping, juce::dontSendNotification); return true; }
    if (k == juce::KeyPress::rightKey) { view->zoomTime (0.7, 0.5 * (view->vt0 + view->vt1)); return true; }
    if (k == juce::KeyPress::leftKey) { view->zoomTime (1.4, 0.5 * (view->vt0 + view->vt1)); return true; }
    if (k == juce::KeyPress::downKey) { view->zoomPitch (0.7, 0.5 * (view->vm0 + view->vm1)); return true; }
    if (k == juce::KeyPress::upKey) { view->zoomPitch (1.4, 0.5 * (view->vm0 + view->vm1)); return true; }
    if (k == juce::KeyPress::returnKey && cand.has) { applyCorrection(); return true; }
    return false;
}

void ReHarmoniserEditor::timerCallback()
{
    if (seeDirty && ! busy && spec != nullptr && juce::Time::getMillisecondCounter() - seeDirtyAt > 350) recombine();
    if (busy) { progressValue = worker->progress(); const auto st = worker->stage(); if (st.isNotEmpty()) status.setText (st + "...", juce::dontSendNotification); }
    const double p = host.playPosition ? host.playPosition() : -1.0;
    if (p != playT)
    {
        playT = p;
        if (p >= 0) view->follow (p);
        view->repaint();
    }
}
} // namespace rehar

#include "FixTools.h"
#include "../core/ImportPlan.h"
#include "../core/ProjectCopy.h"
#include <array>
#include <map>
#include <set>

namespace td { namespace fixtools
{
// ======================================================================================================== helpers
static void say (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle (title).withMessage (text).withButton ("OK"), nullptr);
}

static void launchDialog (std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around, bool resizable = false)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content.release());
    o.dialogTitle = title;
    o.dialogBackgroundColour = theme::window;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = resizable;
    o.componentToCentreAround = around;
    o.launchAsync();
}

static void closeDialogOf (juce::Component* c)
{
    if (auto* dw = c->findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
}

bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;wave;bwf;w64;aif;aiff;aifc;flac"); }

static juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager fm; static bool done = false;
    if (! done) { fm.registerBasicFormats(); done = true; }
    return fm;
}

/** The common look of a dialog: a title line, a progress bar and the Apply / Cancel buttons are laid out by each dialog itself. */
class JobDialogBase : public juce::Component, protected juce::Timer
{
public:
    JobDialogBase() : bar (progressValue)
    {
        addChildComponent (bar); addAndMakeVisible (status);
        status.setColour (juce::Label::textColourId, theme::warn); status.setMinimumHorizontalScale (1.0f);
    }
    ~JobDialogBase() override { stopTimer(); job.requestCancel(); }
protected:
    void timerCallback() override { progressValue = (double) job.progress(); }
    void runJob (AudioJob::Work w, std::function<void (const juce::String&)> done)
    {
        bar.setVisible (true); progressValue = 0.0; startTimerHz (10);
        juce::Component::SafePointer<JobDialogBase> self (this);
        job.start (std::move (w), [self, done] (const juce::String& err)
        {
            if (self == nullptr) return;
            self->stopTimer(); self->bar.setVisible (false);
            self->running = false; self->jobEnded (err);
            if (done) done (err);
        });
        running = true;
    }
    virtual void jobEnded (const juce::String&) {}
    AudioJob job; bool running = false;
    double progressValue = 0.0; juce::ProgressBar bar; juce::Label status;
};

// ======================================================================================================== the pitch dialog
struct PitchContext
{
    bool allowPad = false;
    juce::String title, what;
    std::function<juce::String (double cents, double padSeconds, AudioJob&)> work;     // job thread
    std::function<void()> finish;                                                       // message thread, after success
};

class PitchDialog : public JobDialogBase
{
public:
    explicit PitchDialog (PitchContext c) : ctx (std::move (c))
    {
        setSize (470, ctx.allowPad ? 236 : 204);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setJustificationType (juce::Justification::topLeft); intro.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (intro);
        for (auto* l : { &semiCap, &centCap, &padCap, &total }) { addAndMakeVisible (l); l->setColour (juce::Label::textColourId, theme::text); }
        semiCap.setText ("Semitones", juce::dontSendNotification); centCap.setText ("Cents", juce::dontSendNotification);
        padCap.setText ("Process extra, each end", juce::dontSendNotification);
        semi.setSliderStyle (juce::Slider::LinearHorizontal); semi.setRange (-12, 12, 1); semi.setValue (0, juce::dontSendNotification);
        cents.setSliderStyle (juce::Slider::LinearHorizontal); cents.setRange (-100, 100, 1); cents.setValue (0, juce::dontSendNotification);
        for (auto* s : { &semi, &cents }) { s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22); s->setScrollWheelEnabled (false); s->setDoubleClickReturnValue (true, 0.0); s->onValueChange = [this] { updateTotal(); }; addAndMakeVisible (s); }
        semi.setTooltip ("Whole semitones, -12 to +12 (double-click for 0)");
        cents.setTooltip ("Hundredths of a semitone: +100 cents is one semitone up. Use this for a bit that went slightly flat or sharp (double-click for 0)");
        for (int i = 0; i <= 9; ++i) pad.addItem (i == 0 ? juce::String ("none") : juce::String (i) + " s", i + 1);
        pad.setSelectedId (1, juce::dontSendNotification); pad.setTooltip ("Also shifts this much extra audio before and after the marked part, so you can move the edit point later and still be in the corrected audio");
        addChildComponent (padCap); addChildComponent (pad); padCap.setVisible (ctx.allowPad); pad.setVisible (ctx.allowPad);
        apply.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        addAndMakeVisible (apply); addAndMakeVisible (cancel);
        apply.setColour (juce::TextButton::buttonColourId, theme::accent);
        updateTotal();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        { auto t = r.removeFromTop (22); intro.placeRightOf (t); } r.removeFromTop (4);
        auto row = [&] (juce::Component& a, juce::Component& b) { auto x = r.removeFromTop (28); a.setBounds (x.removeFromLeft (150)); b.setBounds (x); r.removeFromTop (4); };
        row (semiCap, semi); row (centCap, cents);
        if (ctx.allowPad) { auto x = r.removeFromTop (28); padCap.setBounds (x.removeFromLeft (170)); pad.setBounds (x.removeFromLeft (100)); r.removeFromTop (4); }
        total.setBounds (r.removeFromTop (24));
        auto bottom = r.removeFromBottom (30);
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); apply.setBounds (bottom.removeFromRight (140));
        auto s = r.removeFromBottom (24); bar.setBounds (s.withTrimmedRight (0)); status.setBounds (r.removeFromBottom (22));
    }
private:
    double totalCents() const { return semi.getValue() * 100.0 + cents.getValue(); }
    void updateTotal()
    {
        const double c = totalCents();
        total.setText ("Total: " + juce::String (c >= 0 ? "+" : "") + juce::String ((int) std::round (c)) + " cents  ("
                       + juce::String (c >= 0 ? "up" : "down") + " " + juce::String (std::abs (c) / 100.0, 2) + " semitones)", juce::dontSendNotification);
        apply.setEnabled (std::abs (c) >= 1.0 && ! running);
    }
    void start()
    {
        const double c = totalCents(), padSec = (double) (pad.getSelectedId() - 1);
        if (std::abs (c) < 1.0) return;
        apply.setEnabled (false); status.setText ("Working...", juce::dontSendNotification);
        auto work = ctx.work; auto fin = ctx.finish;
        juce::Component::SafePointer<PitchDialog> self (this);
        runJob ([work, c, padSec] (AudioJob& j) { return work (c, padSec, j); },
                [self, fin] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty()) { if (fin) fin(); closeDialogOf (self.getComponent()); }
                    else { self->status.setText (err, juce::dontSendNotification); self->updateTotal(); }
                });
    }
    PitchContext ctx;
    InfoNote intro; juce::Label semiCap, centCap, padCap, total;
    juce::Slider semi, cents; juce::ComboBox pad; juce::TextButton apply { "Correct pitch" }, cancel { "Cancel" };
};

// ======================================================================================================== the pitch-curve dialog
struct CurveContext
{
    bool allowPad = false;
    juce::String what;
    double rate = 48000.0;
    juce::int64 length = 0;                                                                // samples in the marked part
    std::function<std::vector<std::vector<float>> (AudioJob&)> load;                        // job thread: the marked part as one (mixed) vector, for the picture behind the line
    std::function<juce::String (const FixSpec&, double padSeconds, AudioJob&)> prepare;     // job thread: makes the corrected files (nothing is changed yet)
    std::function<bool()> apply;                                                            // message thread: puts the prepared files in place, for listening
    std::function<void()> stop, revert, accept;                                             // message thread
    std::function<void (double fromSec, double toSec, bool loop)> play;                     // message thread: plays from 'fromSec' to 'toSec' (seconds into the marked part), looped or not: nothing either side
    std::function<void()> unapply;                                                          // message thread: puts the original files back but keeps the corrected ones, to switch between the two
    std::function<void (bool)> setLooping;                                                  // message thread: loop switched on / off while playing
    std::function<double()> position;                                                       // message thread: seconds from the start of the marked part, or < 0 when this is not playing
    std::function<void (bool)> hold;                                                        // message thread: true while the dialog is open (see AppContext::undoHold)
};

/** The line the user draws: time across, pitch up and down (+100 cents at the top, -100 at the bottom, 0 in the middle). */
class CurveView : public juce::Component
{
public:
    PitchCurve curve;
    std::function<void()> onChanged;
    std::function<void (const juce::String&)> onInfo;

    CurveView() { setWantsKeyboardFocus (true); }
    void setDuration (double s) { duration = juce::jmax (0.01, s); vt0 = 0.0; vt1 = duration; repaint(); }
    // ---- the view: the part of the time that is shown (this is what Play plays) and how many cents are shown up and down
    double visT0() const { return vt0; }
    double visT1() const { return juce::jmin (duration, vt1); }
    void resetView() { vt0 = 0.0; vt1 = duration; kc = PitchCurve::kRange; repaint(); }
    void zoomTime (double factor)
    {
        const double span = juce::jlimit (juce::jmin (0.05, duration), duration, (vt1 - vt0) * factor);
        // around the playhead when it is running across the picture (it goes to the middle, as far as the start allows), otherwise around the middle
        const double mid = (playT >= vt0 && playT <= vt1) ? playT : 0.5 * (vt0 + vt1);
        setWindow (mid - 0.5 * span, span);
    }
    void zoomCents (double factor) { kc = juce::jlimit (5.0, (double) PitchCurve::kRange, kc * factor); repaint(); }
    /** Left / right = zoom in time (right in), up / down = zoom in cents (up in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); return true; }
        if (k == juce::KeyPress::upKey)    { zoomCents (0.7); return true; }
        if (k == juce::KeyPress::downKey)  { zoomCents (1.0 / 0.7); return true; }
        return false;
    }
    void setEnvelope (std::vector<float> e)
    {
        float mx = 1.0e-6f; for (auto v : e) mx = juce::jmax (mx, v);
        for (auto& v : e) v /= mx;
        env = std::move (e); repaint();
    }
    int count() const { return (int) curve.pts.size(); }
    /** The play position (seconds from the start of the marked part); < 0 hides it. */
    void setPlayhead (double t) { if (std::abs (t - playT) > 1.0e-6) { playT = t; repaint(); } }
    void clearPoints() { curve.pts.clear(); selected = -1; edited(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wellDark);
        const auto pl = plot();
        g.setColour (theme::wave); g.fillRect (pl);
        // what is there to correct: the sound itself, faintly
        if (! env.empty())
        {
            const float mid = (float) pl.getCentreY(), half = (float) pl.getHeight() * 0.5f * 0.9f;
            juce::Path p;
            const int n = (int) env.size();
            for (int x = 0; x < pl.getWidth(); ++x)
            {
                const double tt = vt0 + (double) x / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0);
                const float v = env[(size_t) juce::jlimit (0, n - 1, (int) (tt / duration * (double) n))];
                p.addRectangle ((float) (pl.getX() + x), mid - v * half * 0.5f, 1.0f, juce::jmax (1.0f, v * half));
            }
            g.setColour (theme::dimText.withAlpha (0.28f)); g.fillPath (p);
        }
        g.setFont (juce::FontOptions (11.0f));
        for (double c : { 100.0, 50.0, 25.0, 0.0, -25.0, -50.0, -100.0 })
        {
            if (std::abs (c) > kc + 1.0e-9) continue;
            const int y = yOf (c);
            const bool major = c == 100.0 || c == 0.0 || c == -100.0;
            g.setColour (theme::grid.withAlpha (c == 0.0 ? 1.0f : major ? 0.8f : 0.45f)); g.drawHorizontalLine (y, (float) pl.getX(), (float) pl.getRight());
            g.setColour (theme::dimText); g.drawText ((c > 0 ? "+" : "") + juce::String ((int) c), 0, y - 7, pl.getX() - 6, 14, juce::Justification::right);
        }
        g.setColour (theme::dimText); g.drawText ("cents", 0, pl.getY() - 2, pl.getX() - 6, 12, juce::Justification::right);
        {   // the time ruler above the line: drag it to move the view left and right, double-click to see it all
            const juce::Rectangle<int> ruler (pl.getX(), 0, pl.getWidth(), pl.getY() - 2);
            g.setColour (juce::Colours::white.withAlpha (0.07f)); g.fillRect (ruler);
            const double span = vt1 - vt0;
            static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
            double step = 600.0; for (double st : steps) if (st / span * pl.getWidth() >= 70.0) { step = st; break; }
            for (double t = std::ceil (vt0 / step - 1.0e-9) * step; t <= vt1 + 1.0e-9; t += step)
            {
                const int x = xOf (t);
                g.setColour (theme::grid.withAlpha (0.4f)); g.drawVerticalLine (x, (float) ruler.getY() + 12.0f, (float) pl.getBottom());
                g.setColour (theme::dimText); g.drawText (juce::String (t, step < 0.1 ? 2 : step < 1 ? 1 : 0) + " s", x + 2, ruler.getY(), 60, ruler.getHeight(), juce::Justification::centredLeft);
            }
        }
        juce::Graphics::ScopedSaveState clipToPlot (g);
        g.reduceClipRegion (pl);
        // the line: level (dashed) when nothing is drawn, otherwise the points joined, held level before the first and after the last
        juce::Path line;
        if (curve.pts.empty())
        {
            juce::Path flat; flat.startNewSubPath ((float) pl.getX(), (float) yOf (0)); flat.lineTo ((float) pl.getRight(), (float) yOf (0));
            const float dash[] = { 6.0f, 5.0f }; juce::PathStrokeType (2.0f).createDashedStroke (line, flat, dash, 2);
            g.setColour (theme::accent); g.fillPath (line);
            g.setColour (theme::dimText); g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Click to add a point (the first sits at 0 cents, later ones where you click).  Drag points to shape the pitch line.  Double-click (or Delete) removes a point.", pl.reduced (10, 0).withHeight (pl.getHeight() / 2 - 14), juce::Justification::centred);
        }
        else
        {
            line.startNewSubPath ((float) pl.getX(), (float) yOf (curve.pts.front().cents));
            for (auto& p : curve.pts) line.lineTo ((float) xOf (p.t), (float) yOf (p.cents));
            line.lineTo ((float) pl.getRight(), (float) yOf (curve.pts.back().cents));
            g.setColour (theme::accent); g.strokePath (line, juce::PathStrokeType (2.2f));
            for (int i = 0; i < count(); ++i)
            {
                const auto c = juce::Point<float> ((float) xOf (curve.pts[(size_t) i].t), (float) yOf (curve.pts[(size_t) i].cents));
                const bool sel = i == selected;
                const float r = sel ? 7.0f : 5.5f;
                g.setColour (sel ? juce::Colour (0xffffd24a) : theme::text); g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
                g.setColour (theme::wellDark); g.drawEllipse (c.x - r, c.y - r, 2 * r, 2 * r, 1.2f);
            }
        }
        if (playT >= 0.0)                                  // the playhead runs across the line, with a dot on the line itself
        {
            const double t = juce::jlimit (0.0, duration, playT);
            const int x = xOf (t), y = yOf (curve.centsAt (t));
            g.setColour (theme::playhead); g.drawVerticalLine (x, (float) pl.getY(), (float) pl.getBottom());
            g.fillEllipse ((float) x - 5.0f, (float) y - 5.0f, 10.0f, 10.0f);
            g.setColour (theme::wellDark); g.drawEllipse ((float) x - 5.0f, (float) y - 5.0f, 10.0f, 10.0f, 1.2f);
        }
        g.setColour (theme::border); g.drawRect (pl);
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = hit (e.getPosition());
        if (i >= 0) say (curve.pts[(size_t) i]);
        else if (plot().contains (e.getPosition())) say ({ toT (e.x), curve.centsAt (toT (e.x)) }, true);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (e.y < plot().getY() - 2 && e.x >= plot().getX()) { panning = true; panT0 = vt0; return; }          // the ruler
        int i = hit (e.getPosition());
        if (e.mods.isPopupMenu()) { if (i >= 0) removeAt (i); return; }
        if (i < 0)
        {
            if (! plot().contains (e.getPosition())) return;
            PitchCurve::Point p { juce::jlimit (0.0, duration, toT (e.x)), curve.pts.empty() ? 0.0 : snapC (toC (e.y)) };          // the first point anchors at 0 cents; every later one goes where it is clicked (snapping to 0 when close)
            curve.pts.push_back (p); curve.sortPoints();
            for (int k = 0; k < count(); ++k) if (curve.pts[(size_t) k].t == p.t && curve.pts[(size_t) k].cents == p.cents) i = k;
            edited();
        }
        selected = i; dragging = true; repaint();
        if (i >= 0) say (curve.pts[(size_t) i]);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (panning) { setWindow (panT0 - (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, plot().getWidth()) * (vt1 - vt0), vt1 - vt0); return; }
        if (! dragging || selected < 0 || selected >= count()) return;
        const double lo = selected > 0 ? curve.pts[(size_t) selected - 1].t + 0.002 : 0.0;
        const double hi = selected + 1 < count() ? curve.pts[(size_t) selected + 1].t - 0.002 : duration;
        auto& p = curve.pts[(size_t) selected];
        p.t = juce::jlimit (juce::jmin (lo, hi), juce::jmax (lo, hi), toT (e.x)); p.cents = snapC (toC (e.y));       // whole cents; near the middle line it snaps to exactly 0
        say (p); edited();
    }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; panning = false; }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (e.y < plot().getY() - 2) { resetView(); return; } const int i = hit (e.getPosition()); if (i >= 0) removeAt (i); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && selected >= 0 && selected < count()) { removeAt (selected); return true; }
        return zoomKey (k);
    }
private:
    juce::Rectangle<int> plot() const { return getLocalBounds().withTrimmedLeft (54).withTrimmedBottom (8).withTrimmedTop (26).withTrimmedRight (10); }
    int xOf (double t) const { const auto p = plot(); return p.getX() + (int) std::lround ((t - vt0) / (vt1 - vt0) * (double) p.getWidth()); }
    int yOf (double c) const { const auto p = plot(); return p.getCentreY() - (int) std::lround (c / kc * (double) p.getHeight() * 0.5); }
    double toT (int x) const { const auto p = plot(); return juce::jlimit (0.0, duration, vt0 + (double) (x - p.getX()) / (double) juce::jmax (1, p.getWidth()) * (vt1 - vt0)); }
    double toC (int y) const { const auto p = plot(); return juce::jlimit (-PitchCurve::kRange, PitchCurve::kRange, (double) (p.getCentreY() - y) / (double) juce::jmax (1, p.getHeight()) * 2.0 * kc); }
    /** Whole cents; within 1 cent, or within 7 pixels, of the middle line it snaps to exactly 0 (so you are not fighting to hit it, however far you have zoomed). */
    double snapC (double c) const { const double r = std::round (c); return std::abs (r) <= 1.0 || std::abs (yOf (c) - yOf (0.0)) <= 7 ? 0.0 : r; }
    int hit (juce::Point<int> pos) const
    {
        int best = -1; double bd = 11.0 * 11.0;
        for (int i = 0; i < count(); ++i)
        {
            const double dx = xOf (curve.pts[(size_t) i].t) - pos.x, dy = yOf (curve.pts[(size_t) i].cents) - pos.y, d = dx * dx + dy * dy;
            if (d <= bd) { bd = d; best = i; }
        }
        return best;
    }
    void removeAt (int i) { curve.pts.erase (curve.pts.begin() + i); selected = -1; edited(); }
    void edited() { repaint(); if (onChanged) onChanged(); }
    void say (const PitchCurve::Point& p, bool lineOnly = false)
    {
        if (onInfo) onInfo ((lineOnly ? "The line at " : "Point at ") + juce::String (p.t, 2) + " s:  " + juce::String (p.cents >= 0 ? "+" : "") + juce::String (p.cents, lineOnly ? 1 : 0) + " cents");
    }
    void setWindow (double t0, double span)
    {
        span = juce::jlimit (juce::jmin (0.05, duration), duration, span);
        vt0 = juce::jlimit (0.0, juce::jmax (0.0, duration - span), t0); vt1 = vt0 + span; repaint();
    }
    double duration = 1.0, vt0 = 0.0, vt1 = 1.0, kc = PitchCurve::kRange, panT0 = 0.0; std::vector<float> env; int selected = -1; bool dragging = false, panning = false; double playT = -1.0;
};

class CurveDialog : public JobDialogBase
{
public:
    explicit CurveDialog (CurveContext c) : ctx (std::move (c))
    {
        setSize (1000, 560);
        if (ctx.hold) ctx.hold (true);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::dimText);
        view.setDuration ((double) ctx.length / ctx.rate);
        view.onInfo = [this] (const juce::String& t) { info.setText (t, juce::dontSendNotification); };
        view.onChanged = [this] { lineChanged(); };
        for (int i = 0; i <= 9; ++i) pad.addItem (i == 0 ? juce::String ("none") : juce::String (i) + " s", i + 1);
        pad.setSelectedId (1, juce::dontSendNotification);
        pad.setTooltip ("Also corrects this much extra audio before and after the marked part (the line stays level there), so you can move the edit points outward later");
        pad.onChange = [this] { lineChanged(); };
        padCap.setText ("Process extra, each end", juce::dontSendNotification); padCap.setColour (juce::Label::textColourId, theme::text);
        addChildComponent (pad); addChildComponent (padCap); pad.setVisible (ctx.allowPad); padCap.setVisible (ctx.allowPad);
        for (auto* b : { &auditionBtn, &playBtn, &stopBtn, &revertBtn, &acceptBtn, &closeBtn, &resetBtn }) addAndMakeVisible (b);
        addAndMakeVisible (loopBtn); addAndMakeVisible (hearToggle);
        playBtn.setTooltip ("Plays the part you can see (left / right arrows zoom in and out in time, up / down in cents; drag the ruler to move) with the sound as it is now: the original until you Preview, then the corrected sound if 'Hear corrected' is lit. With Loop lit it goes round and round. Key: Space");
        hearToggle.setTooltip ("Lit: you hear the CORRECTED sound. Off: you hear the ORIGINAL. Click it while playing to switch at once and hear the difference, without touching the line. (Available after Preview.)");
        hearToggle.onClick = [this] { hearChanged(); };
        loopBtn.setTooltip ("Repeat the marked part over and over until you press Stop");
        loopBtn.onClick = [this] { const bool on = loopBtn.getToggleState(); if (! playing()) return; if (on) { if (ctx.play) ctx.play (view.visT0(), view.visT1(), true); } else if (ctx.setLooping) ctx.setLooping (false); };
        playBtn.onClick = [this] { if (ctx.stop) ctx.stop(); if (ctx.play) ctx.play (view.visT0(), view.visT1(), loopBtn.getToggleState()); };
        auditionBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        auditionBtn.setButtonText ("Preview");
        auditionBtn.setTooltip ("Calculates the corrected sound (this takes a moment) and plays it. Then use 'Hear corrected' to switch between the corrected and the original sound. Nothing is final until you press Accept.");
        stopBtn.setTooltip ("Stops the playback");
        revertBtn.setTooltip ("Puts the original sound back");
        acceptBtn.setTooltip ("Keeps the corrected sound (Undo fix brings the original back)");
        resetBtn.setTooltip ("Removes every point: the line goes back to level");
        auditionBtn.onClick = [this] { audition(); };
        stopBtn.onClick = [this] { if (ctx.stop) ctx.stop(); };
        revertBtn.onClick = [this] { doRevert ("Back to the original sound."); };
        acceptBtn.onClick = [this] { accept(); };
        resetBtn.onClick = [this] { view.clearPoints(); };
        closeBtn.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        status.setText ("Looking at the sound...", juce::dontSendNotification);
        refresh();
        ticker.owner = this; ticker.startTimerHz (30);
        setWantsKeyboardFocus (true);                                              // Space plays / stops
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<CurveDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        auto load = ctx.load;
        auto data = std::make_shared<std::vector<std::vector<float>>>();
        juce::Component::SafePointer<CurveDialog> self (this);
        runJob ([load, data] (AudioJob& j) -> juce::String { *data = load (j); return j.cancelled() ? juce::String ("Cancelled.") : juce::String(); },
                [self, data] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->loaded = true;
                    if (err.isEmpty() && ! data->empty() && ! data->front().empty())
                    {
                        const auto& x = data->front(); const int bins = 1200;
                        std::vector<float> env ((size_t) bins, 0.0f);
                        for (size_t i = 0; i < x.size(); ++i) { auto& b = env[i * (size_t) bins / x.size()]; b = juce::jmax (b, std::abs (x[i])); }
                        self->view.setEnvelope (std::move (env));
                    }
                    self->status.setText ("Click on the line area to put in points (each starts at 0 cents: drag it up for sharper, down for flatter). Then press Preview, and Space plays.", juce::dontSendNotification);
                    self->refresh();
                });
    }
    ~CurveDialog() override
    {
        ticker.stopTimer(); ticker.owner = nullptr;
        if (ctx.stop) ctx.stop();
        if (tentative && ! accepted && ctx.revert) ctx.revert();       // closing without Accept puts everything back
        if (ctx.hold) ctx.hold (false);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        auto rowC = r.removeFromBottom (32);
        r.removeFromBottom (4);
        bar.setBounds (r.removeFromBottom (22).reduced (2, 2));                           // the progress bar has a full-width row of its own
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        info.setBounds (r.removeFromBottom (20).reduced (2, 0));
        closeBtn.setBounds (rowC.removeFromRight (90)); rowC.removeFromRight (8);
        acceptBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (8);
        revertBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (16);
        stopBtn.setBounds (rowC.removeFromRight (70)); rowC.removeFromRight (8);
        loopBtn.setBounds (rowC.removeFromRight (66)); rowC.removeFromRight (8);
        playBtn.setBounds (rowC.removeFromRight (70)); rowC.removeFromRight (8);
        auditionBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (14);
        hearToggle.setBounds (rowC.removeFromRight (150));
        resetBtn.setBounds (rowC.removeFromLeft (100)); rowC.removeFromLeft (14);
        if (ctx.allowPad) { padCap.setBounds (rowC.removeFromLeft (150)); pad.setBounds (rowC.removeFromLeft (80)); }
        view.setBounds (r);
    }
private:
    void refresh()
    {
        const bool ready = loaded && ! running;
        auditionBtn.setEnabled (ready && ! view.curve.isFlat());
        stopBtn.setEnabled (ready);
        playBtn.setEnabled (ready);
        hearToggle.setEnabled (ready && tentative && ! stale);
        revertBtn.setEnabled (ready && tentative);
        acceptBtn.setEnabled (ready && tentative && ! stale);
        resetBtn.setEnabled (ready && view.count() > 0);
        pad.setEnabled (ready);
    }
    void lineChanged()
    {
        if (tentative && ! stale) { stale = true; status.setText ("The line changed: press Preview to hear it (Accept is available after that).", juce::dontSendNotification); }
        refresh();
    }
    void doRevert (const juce::String& msg)
    {
        if (ctx.stop) ctx.stop();
        if (tentative && ctx.revert) ctx.revert();
        tentative = false; stale = false; corrected = false; hearToggle.setToggleState (false, juce::dontSendNotification);
        status.setText (msg, juce::dontSendNotification); refresh();
    }
    void audition()
    {
        if (running || ! loaded || view.curve.isFlat()) return;
        if (ctx.stop) ctx.stop();
        if (tentative && ctx.revert) ctx.revert();                         // always from the original sound
        tentative = false; stale = false; corrected = false; hearToggle.setToggleState (false, juce::dontSendNotification); refresh();
        FixSpec spec; spec.kind = FixSpec::Kind::Pitch; spec.useCurve = true; spec.curve = view.curve; spec.curve.sortPoints();
        const double padSec = ctx.allowPad ? (double) (pad.getSelectedId() - 1) : 0.0;
        spec.curveZero = padSec * ctx.rate;
        status.setText ("Calculating the corrected sound...", juce::dontSendNotification);
        auditionBtn.setEnabled (false); acceptBtn.setEnabled (false); revertBtn.setEnabled (false); resetBtn.setEnabled (false); pad.setEnabled (false);
        auto prepare = ctx.prepare;
        juce::Component::SafePointer<CurveDialog> self (this);
        runJob ([prepare, spec, padSec] (AudioJob& j) { return prepare (spec, padSec, j); },
                [self] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err == "Cancelled." ? juce::String ("Cancelled. Nothing was changed.") : err, juce::dontSendNotification); self->refresh(); return; }
                    if (self->ctx.apply && self->ctx.apply())
                    {
                        self->tentative = true; self->stale = false; self->corrected = true;
                        self->hearToggle.setToggleState (true, juce::dontSendNotification);
                        if (self->ctx.play) self->ctx.play (self->view.visT0(), self->view.visT1(), self->loopBtn.getToggleState());
                        self->status.setText ("Playing the corrected sound.  'Hear corrected' switches to the original and back; Accept keeps it, Revert goes back, or change the line and Preview again.", juce::dontSendNotification);
                    }
                    else self->status.setText ("The audio changed while it was being calculated, so nothing was changed. Please try again.", juce::dontSendNotification);
                    self->refresh();
                });
    }
    void accept()
    {
        if (! tentative || stale || running) return;
        if (ctx.stop) ctx.stop();
        if (! corrected)                                                // the original was being heard: put the corrected sound back before keeping it
        {
            if (! (ctx.apply && ctx.apply())) { status.setText ("The audio changed, so the corrected sound is no longer available. Press Preview again.", juce::dontSendNotification); tentative = false; refresh(); return; }
            corrected = true;
        }
        if (ctx.accept) ctx.accept();
        accepted = true;
        closeDialogOf (this);
    }
    bool playing() const { return ctx.position && ctx.position() >= 0.0; }
    /** 'Hear corrected': switches between the corrected and the original sound at the same place, so the difference can be heard straight away. */
    void hearChanged()
    {
        const bool wantCorrected = hearToggle.getToggleState();
        if (! tentative || stale) { hearToggle.setToggleState (corrected, juce::dontSendNotification); status.setText ("Press Preview first: there is no corrected sound to listen to yet.", juce::dontSendNotification); return; }
        const bool wasPlaying = playing(); const double at = wasPlaying ? ctx.position() : 0.0;
        if (ctx.stop) ctx.stop();
        if (wantCorrected) { if (! (ctx.apply && ctx.apply())) { status.setText ("The audio changed, so the corrected sound is no longer available. Press Preview again.", juce::dontSendNotification); tentative = false; refresh(); return; } }
        else if (ctx.unapply) ctx.unapply();
        corrected = wantCorrected;
        if (wasPlaying && ctx.play) ctx.play (loopBtn.getToggleState() || at < view.visT0() || at >= view.visT1() ? view.visT0() : at, view.visT1(), loopBtn.getToggleState());      // carries on from the same place (a loop starts again at the beginning)
        status.setText (wantCorrected ? "Hearing the CORRECTED sound." : "Hearing the ORIGINAL sound.", juce::dontSendNotification);
    }
public:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)                                      // Space: play the marked part, or stop
        {
            if (loaded && ! running) { if (playing()) { if (ctx.stop) ctx.stop(); } else if (ctx.play) ctx.play (view.visT0(), view.visT1(), loopBtn.getToggleState()); }
            return true;
        }
        return view.zoomKey (k);
    }
private:
    void tick()
    {
        const double t = ctx.position ? ctx.position() : -1.0;
        view.setPlayhead (t);
        if (t >= 0.0 && ! running) info.setText ("Playing:  " + juce::String (t, 2) + " s,  line " + juce::String (view.curve.centsAt (t) >= 0 ? "+" : "") + juce::String (view.curve.centsAt (t), 1) + " cents", juce::dontSendNotification);
        playBtn.setButtonText (t >= 0.0 ? "Restart" : "Play");
    }
    struct Ticker : juce::Timer { CurveDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    CurveContext ctx;
    InfoNote intro; juce::Label info, padCap;
    CurveView view; juce::ComboBox pad;
    juce::TextButton auditionBtn { "Audition" }, playBtn { "Play" }, stopBtn { "Stop" }, revertBtn { "Revert" }, acceptBtn { "Accept" }, closeBtn { "Cancel" }, resetBtn { "Reset line" };
    juce::ToggleButton loopBtn { "Loop" }, hearToggle { "Hear corrected" };
    bool loaded = false, tentative = false, stale = false, accepted = false, corrected = false;
};

// ======================================================================================================== the repair dialog
/** One change made in the repair window: what to do and where (samples from the start of the loaded area). */
struct RepairOp { FixSpec spec; juce::int64 t0 = 0, t1 = 0; };

struct RepairContext
{
    juce::String title, what;
    double rate = 48000.0;
    juce::int64 length = 0;                                                             // samples in the scanned area
    std::function<std::vector<std::vector<float>> (AudioJob&)> load;                     // job thread: one mono vector per track, 'length' samples
    std::function<juce::String (const std::vector<RepairOp>&, AudioJob&)> prepare;      // job thread: makes the repaired files with ALL these changes, covering the whole loaded area; nothing is changed yet
    std::function<bool()> apply;                                                        // message thread: puts the prepared files in place (so playing / the edit window use them)
    std::function<void()> unapply, discard, accept, stop;                               // message thread: back to the original / delete the prepared files / keep what is in place / stop playing
    std::function<void (double fromSec, double toSec, bool loop)> play;                 // message thread: plays from..to (seconds from the start of the area), optionally looped
    std::function<void (bool)> setLooping, hold;                                        // message thread
    std::function<double()> position;                                                   // message thread: seconds from the start of the area, or < 0 when this is not playing
};

class SpectrogramView : public juce::Component
{
public:
    std::function<void()> onSelection;
    std::function<void (const juce::String&)> onNote;         // a line for the status line (colour roll, scale, view)

    void setData (MergedSpectrogram s, double seconds, bool keepSelection = false)
    {
        const bool same = spec.frames > 0 && std::abs (seconds - duration) < 1.0e-9;
        spec = std::move (s); duration = juce::jmax (0.001, seconds);
        if (! keepSelection) hasSel = false;
        if (! same) resetView();
        dirty = true; repaint();
    }
    bool hasSelection() const { return hasSel; }
    /** Shows the sound around the box that the repair learns from (dotted): 'pct' is a % of the box, 'direction' 0 = left and right, 1 = up and down, 2 = both. */
    void setSurround (bool show, double pct, int direction, double afterWeight = 0.5) { showSurround = show; surroundPct = pct; surroundDir = direction; surroundAfter = juce::jlimit (0.0, 1.0, afterWeight); repaint(); }
    double selT0() const { return juce::jmin (a.x, b.x); }   // seconds
    double selT1() const { return juce::jmax (a.x, b.x); }
    double selF0() const { return juce::jmin (a.y, b.y); }   // Hz
    double selF1() const { return juce::jmax (a.y, b.y); }
    void clearSelection() { hasSel = false; repaint(); if (onSelection) onSelection(); }
    /** Seconds from the start of the area; < 0 hides the playhead. */
    void setPlayhead (double t) { if (std::abs (t - playT) > 1.0e-6) { playT = t; repaint(); } }

    // ---- the view: which part of the time is shown (this is what Play plays), which pitches, and how the pitches are spread up the picture
    double visT0() const { return vt0; }
    double visT1() const { return vt1; }
    void resetView() { vt0 = 0.0; vt1 = duration; fLo = kMinHz; fHi = maxHz(); dirty = true; repaint(); }
    /** factor < 1 zooms in, > 1 zooms out, around the middle of what is shown. */
    void zoomTime (double factor)
    {
        const double span = juce::jlimit (juce::jmin (0.05, duration), duration, (vt1 - vt0) * factor);
        // around the playhead when it is running across the picture (it goes to the middle, as far as the start allows), otherwise around the middle
        const double mid = (playT >= vt0 && playT <= vt1) ? playT : 0.5 * (vt0 + vt1);
        setWindow (mid - 0.5 * span, span);
    }
    void zoomFreq (double factor)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        const double span = juce::jlimit (std::log (1.6), hi - lo, std::log (fHi / fLo) * factor);
        setFreqWindow (0.5 * (std::log (fLo) + std::log (fHi)) - 0.5 * span, span);
    }
    void panFreq (double logShift) { setFreqWindow (std::log (fLo) + logShift, std::log (fHi / fLo)); }
    /** 0 = pitch spread evenly (linear), 1 = every octave the same height (musical); in between is a blend. */
    void setScale (double w) { warp = juce::jlimit (0.0, 1.0, w); dirty = true; repaint(); }
    /** Left / right = zoom in time (right in), up / down = zoom in pitch (up in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); note(); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); note(); return true; }
        if (k == juce::KeyPress::upKey)    { zoomFreq (0.7); return true; }
        if (k == juce::KeyPress::downKey)  { zoomFreq (1.0 / 0.7); return true; }
        return false;
    }
    /** The round colour control (bottom left): each step is another colour map and another range of levels, so quiet noises show up. */
    void rollColour (double amount) { setRoll (rollPos + amount); }
    void setRoll (double p)
    {
        rollPos = std::fmod (std::fmod (p, (double) kPresets) + (double) kPresets, (double) kPresets);
        preset = (int) std::floor (rollPos + 0.5) % kPresets;
        buildLut(); dirty = true; repaint();
        if (onNote) onNote ("Colour: " + juce::String (presetAt (preset).name) + ".  Roll the round control smoothly (drag up / down, or the mouse wheel) to blend between the looks and show quieter sounds; click to jump to the next one; double-click for the standard colours.");
    }

    SpectrogramView() { buildLut(); setMouseCursor (juce::MouseCursor::CrosshairCursor); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wellDark);
        auto plot = plotArea();
        if (dirty || image.getWidth() != plot.getWidth() || image.getHeight() != plot.getHeight()) rebuildImage (plot);
        if (image.isValid()) g.drawImageAt (image, plot.getX(), plot.getY());
        else { g.setColour (juce::Colours::white); g.drawText ("Scanning...", plot, juce::Justification::centred); }
        g.setFont (juce::FontOptions (11.0f));
        // the pitch scale
        {
            static const double ticks[] = { 50, 100, 200, 300, 500, 700, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000, 20000 };
            int lastLabelY = plot.getBottom() + 20;
            for (double f : ticks)
            {
                if (f < fLo || f > fHi) continue;
                const int y = hzToY (f, plot);
                g.setColour (juce::Colours::white.withAlpha (0.14f)); g.drawHorizontalLine (y, (float) plot.getX(), (float) plot.getRight());
                if (lastLabelY - y < 14) continue;
                lastLabelY = y;
                g.setColour (juce::Colours::white.withAlpha (0.8f)); g.drawText (f >= 1000 ? juce::String (f / 1000.0, f >= 10000 || std::fmod (f, 1000.0) == 0.0 ? 0 : 1) + "k" : juce::String ((int) f), 2, y - 7, plot.getX() - 6, 14, juce::Justification::right);
            }
        }
        // the time ruler (above the picture): drag it to move the view left and right, double-click to see everything
        {
            auto ruler = rulerArea();
            g.setColour (juce::Colours::white.withAlpha (0.07f)); g.fillRect (ruler);
            g.setColour (theme::border); g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());
            const double span = vt1 - vt0;
            static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
            double step = 600.0; for (double s : steps) if (s / span * plot.getWidth() >= 70.0) { step = s; break; }
            const double sub = step / 5.0;
            for (double t = std::ceil (vt0 / sub - 1.0e-9) * sub; t <= vt1 + 1.0e-9; t += sub)
            {
                const int x = tToX (t, plot);
                const bool major = std::abs (t / step - std::round (t / step)) < 1.0e-6;
                g.setColour (juce::Colours::white.withAlpha (major ? 0.7f : 0.3f)); g.drawVerticalLine (x, (float) (ruler.getBottom() - (major ? 10 : 5)), (float) (ruler.getBottom() - 1));
                if (! major) continue;
                g.setColour (juce::Colours::white.withAlpha (0.15f)); g.drawVerticalLine (x, (float) plot.getY(), (float) plot.getBottom());
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawText (juce::String (t, step < 0.1 ? 2 : step < 1 ? 1 : 0) + " s", x + 2, ruler.getY(), 60, ruler.getHeight() - 8, juce::Justification::centredLeft);
            }
        }
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (plot);
            if (hasSel && showSurround)
            {
                const double t0 = selT0(), t1 = selT1(), f0 = selF0(), f1 = selF1();
                const double dt = (t1 - t0) * surroundPct / 100.0, df = (f1 - f0) * surroundPct / 100.0;
                auto box = [&] (double ta, double tb, double fa, double fb)
                {
                    ta = juce::jmax (0.0, ta); tb = juce::jmin (duration, tb); fa = juce::jmax (kMinHz, fa); fb = juce::jmin (maxHz(), fb);
                    if (tb <= ta || fb <= fa) return;
                    const int x0 = tToX (ta, plot), x1 = tToX (tb, plot), y0 = hzToY (fb, plot), y1 = hzToY (fa, plot);
                    juce::Rectangle<int> r (x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0));
                    g.setColour (juce::Colour (0x2229d3ff)); g.fillRect (r);
                    juce::Path p; p.addRectangle (r.toFloat());
                    juce::Path dashed; const float pattern[] = { 4.0f, 4.0f };
                    juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, pattern, 2);
                    g.setColour (juce::Colour (0xff29d3ff)); g.fillPath (dashed);
                };
                if (surroundDir != 1) { box (t0 - 2.0 * dt * (1.0 - surroundAfter), t0, f0, f1); box (t1, t1 + 2.0 * dt * surroundAfter, f0, f1); }      // before and after the box: shared out by the before / after weighting (50/50 = equal, 0 = all before)
                if (surroundDir != 0) { box (t0, t1, f1, f1 + df); box (t0, t1, f0 - df, f0); }          // above and below it
            }
            if (hasSel)
            {
                auto r = selRect();
                g.setColour (juce::Colour (0x44ffd24a)); g.fillRect (r);
                g.setColour (juce::Colour (0xffffd24a)); g.drawRect (r, 2);
            }
            if (playT >= 0.0 && duration > 0.0 && playT >= vt0 && playT <= vt1)
            {
                const int x = tToX (playT, plot);
                g.setColour (theme::playhead); g.fillRect (x - 1, plot.getY(), 2, plot.getHeight());
            }
        }
        // the round colour control
        {
            const auto k = knobArea().toFloat(); const auto c = k.getCentre(); const float r = k.getWidth() * 0.5f;
            g.setColour (juce::Colour (0xff22262c)); g.fillEllipse (k);
            g.setColour (theme::border); g.drawEllipse (k.reduced (0.5f), 1.2f);
            for (int i = 0; i < kPresets; ++i)                                        // one dot per colour map, the lit one is the one in use
            {
                const float ang = juce::MathConstants<float>::twoPi * (float) i / (float) kPresets - juce::MathConstants<float>::halfPi;
                g.setColour (i == preset ? juce::Colours::white.withAlpha (0.8f) : juce::Colours::white.withAlpha (0.28f));
                g.fillEllipse (c.x + std::cos (ang) * (r - 3.0f) - 1.4f, c.y + std::sin (ang) * (r - 3.0f) - 1.4f, 2.8f, 2.8f);
            }
            g.setColour (lut[170]); g.fillEllipse (c.x - r * 0.45f, c.y - r * 0.45f, r * 0.9f, r * 0.9f);
            const float ang = juce::MathConstants<float>::twoPi * (float) rollPos / (float) kPresets - juce::MathConstants<float>::halfPi;
            g.setColour (juce::Colours::white); g.drawLine (c.x, c.y, c.x + std::cos (ang) * (r - 4.5f), c.y + std::sin (ang) * (r - 4.5f), 1.8f);
            g.setColour (juce::Colours::white.withAlpha (0.75f)); g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (presetAt (preset).name) + "   (colour roll)", (int) k.getRight() + 8, (int) k.getY(), 300, (int) k.getHeight(), juce::Justification::centredLeft);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        mode = Mode::none;
        if (knobArea().expanded (3).contains (e.getPosition())) { mode = Mode::knob; rollStart = rollPos; return; }
        if (rulerArea().contains (e.getPosition())) { mode = Mode::pan; panT0 = vt0; return; }
        auto pl = plotArea();
        if (e.x < pl.getX() && e.y >= pl.getY() && e.y < pl.getBottom()) { mode = Mode::pitchPan; panF = fLo; return; }
        if (const int ed = edgesAt (e.getPosition()))                    // on an edge or corner of the box: drag it to resize the box
        {
            a = { selT0(), selF0() }; b = { selT1(), selF1() };        // a = lower-left corner, b = upper-right
            mode = Mode::resize; edgeMask = ed; return;
        }
        if (! pl.contains (e.getPosition())) return;
        mode = Mode::select; a = toTF (e.getPosition()); b = a; hasSel = true; repaint();
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int ed = edgesAt (e.getPosition());
        using C = juce::MouseCursor;
        setMouseCursor (ed == 0 ? C (C::CrosshairCursor)
                        : ed == 1 || ed == 2 ? C (C::LeftRightResizeCursor)
                        : ed == 4 || ed == 8 ? C (C::UpDownResizeCursor)
                        : ed == (1 | 4) ? C (C::TopLeftCornerResizeCursor) : ed == (2 | 4) ? C (C::TopRightCornerResizeCursor)
                        : ed == (1 | 8) ? C (C::BottomLeftCornerResizeCursor) : C (C::BottomRightCornerResizeCursor));
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto pl = plotArea();
        if (mode == Mode::select) { b = toTF (e.getPosition()); repaint(); }
        else if (mode == Mode::resize)
        {
            const auto q = toTF (e.getPosition());
            if (edgeMask & 1) a.x = q.x;
            if (edgeMask & 2) b.x = q.x;
            if (edgeMask & 4) b.y = q.y;                                 // the top edge is the highest pitch
            if (edgeMask & 8) a.y = q.y;
            repaint();
        }
        else if (mode == Mode::pan) setWindow (panT0 - (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0), vt1 - vt0);
        else if (mode == Mode::pitchPan) { fLo = panF; panFreq ((double) e.getDistanceFromDragStartY() / (double) juce::jmax (1, pl.getHeight()) * std::log (fHi / fLo)); }
        else if (mode == Mode::knob)
        {
            setRoll (rollStart - (double) e.getDistanceFromDragStartY() / 45.0);       // smooth: about 45 px per colour look
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto m = mode; mode = Mode::none;
        if (m == Mode::knob && e.getDistanceFromDragStart() < 4) { setRoll (std::floor (rollPos + 0.5) + (e.mods.isRightButtonDown() || e.mods.isShiftDown() ? -1 : 1)); return; }
        if (m == Mode::pan || m == Mode::pitchPan) { note(); return; }
        if (m != Mode::select && m != Mode::resize) return;
        if (selT1() - selT0() < 0.002) hasSel = false;
        repaint(); if (onSelection) onSelection();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (knobArea().expanded (3).contains (e.getPosition())) { rollColour ((double) w.deltaY * 2.0); return; }
        Component::mouseWheelMove (e, w);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (knobArea().expanded (3).contains (e.getPosition())) { setRoll (0.0); return; }                 // back to the standard colours
        if (rulerArea().contains (e.getPosition())) { resetView(); note(); return; }                               // everything again
        if (plotArea().contains (e.getPosition())) clearSelection();
    }
private:
    static constexpr double kMinHz = 40.0;
    static constexpr int kPresets = 8;
    enum class Mode { none, select, pan, pitchPan, knob, resize };
    int edgeMask = 0;                                           // while resizing the box: 1 left, 2 right, 4 top, 8 bottom
    struct Stop { float t, r, g, b; };
    struct Preset { const char* name; int palette; float lo, hi, gamma; };
    static const Preset& presetAt (int i)
    {
        static const Preset p[kPresets] = {
            { "Standard",                 0, -100.0f, -15.0f, 1.0f },
            { "Strong sounds only",       0,  -80.0f, -20.0f, 1.0f },
            { "Show quiet sounds",        0, -115.0f, -45.0f, 1.0f },
            { "Show very quiet noise",    2, -125.0f, -65.0f, 0.8f },
            { "Grey",                     1, -100.0f, -15.0f, 1.0f },
            { "Grey, quiet sounds",       1, -120.0f, -50.0f, 0.7f },
            { "Rainbow",                  3, -100.0f, -20.0f, 1.0f },
            { "Rainbow, quiet sounds",    3, -125.0f, -60.0f, 0.85f } };
        return p[juce::jlimit (0, kPresets - 1, i)];
    }
    static juce::Colour ramp (const Stop* s, int n, float t)
    {
        for (int i = 1; i < n; ++i)
            if (t <= s[i].t) { const float f = (t - s[i - 1].t) / (s[i].t - s[i - 1].t); return juce::Colour::fromFloatRGBA (s[i - 1].r + f * (s[i].r - s[i - 1].r), s[i - 1].g + f * (s[i].g - s[i - 1].g), s[i - 1].b + f * (s[i].b - s[i - 1].b), 1.0f); }
        return juce::Colour::fromFloatRGBA (s[n - 1].r, s[n - 1].g, s[n - 1].b, 1.0f);
    }
    /** The look in use: the two neighbouring presets blended by how far the knob is between them, so it changes continuously. */
    Preset blended (const Preset*& pa, const Preset*& pb, float& f) const
    {
        const int i0 = (int) std::floor (rollPos) % kPresets, i1 = (i0 + 1) % kPresets;
        f = (float) (rollPos - std::floor (rollPos));
        pa = &presetAt (i0); pb = &presetAt (i1);
        return Preset { pa->name, pa->palette, pa->lo + f * (pb->lo - pa->lo), pa->hi + f * (pb->hi - pa->hi), pa->gamma + f * (pb->gamma - pa->gamma) };
    }
    static juce::Colour paletteAt (int pal, float t)
    {
        static const Stop heat[] = { { 0.0f, 0, 0, 0.05f }, { 0.25f, 0.2f, 0.0f, 0.4f }, { 0.5f, 0.75f, 0.1f, 0.35f }, { 0.75f, 1.0f, 0.55f, 0.1f }, { 1.0f, 1.0f, 1.0f, 0.8f } };
        static const Stop grey[] = { { 0.0f, 0, 0, 0 }, { 1.0f, 1, 1, 1 } };
        static const Stop ice[]  = { { 0.0f, 0, 0, 0.05f }, { 0.35f, 0, 0.15f, 0.6f }, { 0.7f, 0, 0.75f, 0.95f }, { 1.0f, 1, 1, 1 } };
        static const Stop rain[] = { { 0.0f, 0, 0, 0.15f }, { 0.2f, 0.15f, 0, 0.7f }, { 0.4f, 0, 0.6f, 1.0f }, { 0.6f, 0.1f, 0.9f, 0.3f }, { 0.8f, 1.0f, 0.9f, 0.0f }, { 1.0f, 1.0f, 0.15f, 0.1f } };
        return pal == 0 ? ramp (heat, 5, t) : pal == 1 ? ramp (grey, 2, t) : pal == 2 ? ramp (ice, 4, t) : ramp (rain, 6, t);
    }
    void buildLut()
    {
        const Preset *pa, *pb; float f; blended (pa, pb, f);
        for (int i = 0; i < 256; ++i)
        {
            const float u = (float) i / 255.0f;
            const auto ca = paletteAt (pa->palette, std::pow (u, pa->gamma)), cb = paletteAt (pb->palette, std::pow (u, pb->gamma));
            lut[(size_t) i] = ca.interpolatedWith (cb, f);
        }
    }
    double maxHz() const { return juce::jmin (22000.0, spec.sampleRate * 0.5); }
    juce::Rectangle<int> plotArea() const { return getLocalBounds().withTrimmedLeft (46).withTrimmedTop (22).withTrimmedBottom (30); }
    juce::Rectangle<int> rulerArea() const { auto pl = plotArea(); return { pl.getX(), 0, pl.getWidth(), pl.getY() }; }
    juce::Rectangle<int> knobArea() const { return { 8, getHeight() - 28, 24, 24 }; }
    void note() { if (onNote) onNote ("Showing " + juce::String (vt0, 2) + " - " + juce::String (vt1, 2) + " s (Play plays this part).  Left / right arrows zoom in time, up / down in pitch; drag the ruler to move; double-click the ruler to see it all."); }
    void setWindow (double t0, double span)
    {
        span = juce::jlimit (juce::jmin (0.05, duration), duration, span);
        t0 = juce::jlimit (0.0, juce::jmax (0.0, duration - span), t0);
        vt0 = t0; vt1 = t0 + span; dirty = true; repaint();
    }
    void setFreqWindow (double logLo, double logSpan)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        logSpan = juce::jlimit (std::log (1.6), hi - lo, logSpan);
        logLo = juce::jlimit (lo, hi - logSpan, logLo);
        fLo = std::exp (logLo); fHi = std::exp (logLo + logSpan); dirty = true; repaint();
    }
    // pitch <-> height: a blend of the straight line (even Hz) and the logarithm (even octaves)
    double posOf (double hz) const
    {
        hz = juce::jlimit (fLo, fHi, hz);
        const double lin = (hz - fLo) / (fHi - fLo), lg = std::log (hz / fLo) / std::log (fHi / fLo);
        return (1.0 - warp) * lin + warp * lg;
    }
    double hzOf (double pos) const
    {
        pos = juce::jlimit (0.0, 1.0, pos);
        if (warp > 0.999) return fLo * std::pow (fHi / fLo, pos);
        if (warp < 0.001) return fLo + pos * (fHi - fLo);
        double lo = fLo, hi = fHi;
        for (int i = 0; i < 40; ++i) { const double m = 0.5 * (lo + hi); if (posOf (m) < pos) lo = m; else hi = m; }
        return 0.5 * (lo + hi);
    }
    int tToX (double t, juce::Rectangle<int> pl) const { return pl.getX() + (int) std::lround ((t - vt0) / juce::jmax (1.0e-9, vt1 - vt0) * pl.getWidth()); }
    int hzToY (double hz, juce::Rectangle<int> pl) const { return pl.getY() + (int) std::lround ((1.0 - posOf (hz)) * (pl.getHeight() - 1)); }
    juce::Point<double> toTF (juce::Point<int> p) const
    {
        auto pl = plotArea();
        const double t = juce::jlimit (0.0, duration, vt0 + (double) (p.x - pl.getX()) / juce::jmax (1, pl.getWidth()) * (vt1 - vt0));
        const int yy = juce::jlimit (0, pl.getHeight() - 1, p.y - pl.getY());
        return { t, hzOf (1.0 - (double) yy / (double) juce::jmax (1, pl.getHeight() - 1)) };
    }
    /** Which edges of the box the point is on (1 left, 2 right, 4 top, 8 bottom, or two of them at a corner); 0 = not on the box's border. */
    int edgesAt (juce::Point<int> p) const
    {
        if (! hasSel) return 0;
        const auto r = selRect(); const int m = 6;
        if (! r.expanded (m).contains (p)) return 0;
        int e = 0;
        if (std::abs (p.x - r.getX()) <= m) e |= 1; else if (std::abs (p.x - r.getRight()) <= m) e |= 2;
        if (std::abs (p.y - r.getY()) <= m) e |= 4; else if (std::abs (p.y - r.getBottom()) <= m) e |= 8;
        return e;
    }
    juce::Rectangle<int> selRect() const
    {
        auto pl = plotArea();
        const int x0 = tToX (selT0(), pl), x1 = tToX (selT1(), pl);
        const int y0 = hzToY (selF1(), pl), y1 = hzToY (selF0(), pl);
        return { x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0) };
    }
    /** The picture of what is in view, made at the size it is shown (so zooming in shows more detail, within what was scanned). */
    void rebuildImage (juce::Rectangle<int> pl)
    {
        dirty = false;
        const int W = pl.getWidth(), H = pl.getHeight();
        if (spec.frames <= 0 || W < 2 || H < 2) { image = juce::Image(); return; }
        image = juce::Image (juce::Image::RGB, W, H, false);
        juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
        std::vector<int> rb ((size_t) H); std::vector<float> rf ((size_t) H);
        for (int y = 0; y < H; ++y)
        {
            const double hz = hzOf (1.0 - (double) y / (double) (H - 1));
            rb[(size_t) y] = 0; rf[(size_t) y] = (float) hz;                       // (rf holds the pitch of this row; spec.level() blends the two analysis layers)
        }
        const Preset *qa, *qb; float qf; const Preset P = blended (qa, qb, qf);
        const float lo = P.lo, inv = 255.0f / (P.hi - P.lo);
        for (int x = 0; x < W; ++x)
        {
            const double t = vt0 + ((double) x + 0.5) / (double) W * (vt1 - vt0);
            const double fp = juce::jlimit (0.0, (double) spec.frames - 1.0001, t / duration * (double) (spec.frames - 1));
            const int f0 = (int) fp; const float ff = (float) (fp - f0); const int f1 = juce::jmin (spec.frames - 1, f0 + 1);
            for (int y = 0; y < H; ++y)
            {
                const float d0 = spec.level (f0, (double) rf[(size_t) y]), d1 = spec.level (f1, (double) rf[(size_t) y]);
                const float d = d0 * (1.0f - ff) + d1 * ff;
                const int idx = juce::jlimit (0, 255, (int) ((d - lo) * inv));
                bd.setPixelColour (x, y, lut[(size_t) idx]);
            }
        }
    }
    MergedSpectrogram spec; double duration = 1.0; juce::Image image; bool dirty = true;
    double vt0 = 0.0, vt1 = 1.0, fLo = kMinHz, fHi = 20000.0, warp = 1.0;
    int preset = 0; double rollPos = 0.0, rollStart = 0.0; Mode mode = Mode::none; double panT0 = 0.0, panF = kMinHz;
    std::array<juce::Colour, 256> lut;
    bool hasSel = false; juce::Point<double> a, b; double playT = -1.0;
    bool showSurround = false; double surroundPct = 100.0; int surroundDir = 0; double surroundAfter = 0.5;
};

class RepairDialog : public JobDialogBase
{
public:
    explicit RepairDialog (RepairContext c, bool declickWindow = false) : ctx (std::move (c)), declickOnly (declickWindow)
    {
        setSize (1000, 720);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::text);
        modeBox.addItem ("Spectral repair of the selected box", 1); modeBox.addItem ("Declick: find and mend clicks in the selected time span", 2);
        modeBox.setSelectedId (declickOnly ? 2 : 1, juce::dontSendNotification);          // this window does one thing only; the box stays hidden
        dirBox.addItem ("Left and right (uses the sound before / after)", 1); dirBox.addItem ("Up and down (uses the pitches above / below)", 2); dirBox.addItem ("Both", 3);
        dirBox.setSelectedId (1, juce::dontSendNotification); dirBox.onChange = [this] { refreshControls(); invalidate(); };
        dirBox.setTooltip ("Where the repair finds its material. Left and right suits steady sounds (hum, a held note). Up and down suits short bursts (a click, a cough) with music above and below. Both blends the two.");
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (sensCap, "Click sensitivity"); cap (strengthCap, "Strength"); cap (dirCap, "Direction"); cap (surroundCap, "Surrounding length"); cap (weightCap, "Before  <->  After");
        auto slider = [this] (juce::Slider& sl, double lo, double hi, double step, double v, const juce::String& suffix)
        {
            sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (lo, hi, step); sl.setValue (v, juce::dontSendNotification); sl.setScrollWheelEnabled (false);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22); sl.setTextValueSuffix (suffix);
            sl.onValueChange = [this] { updateSurround(); invalidate(); };
            addAndMakeVisible (sl);
        };
        slider (sens, 1, 10, 1, 6, ""); slider (strength, 0, 100, 1, 100, " %"); slider (surround, 10, 400, 5, 100, " %"); slider (weight, 0, 100, 1, 50, " %");
        strength.setTooltip ("How much of the repair replaces the original sound inside the box. 100 % is a full repair; less keeps some of the original.");
        surround.setTooltip ("How much of the sound around the box is used, as a percentage of the box: its length in time (left and right) and its height in pitch (up and down).");
        weight.setTooltip ("0 uses only the sound before the box, 100 only the sound after it, 50 uses both equally (a cross-fade). Applies to left and right.");
        for (auto* x : std::initializer_list<juce::Component*> { &dirBox, &previewBtn, &compareBtn, &revertBtn, &finishBtn, &close, &playBtn, &stopBtn, &loopBtn, &hearCap, &hearBox, &scaleBox, &detailBox }) addAndMakeVisible (x);
        scaleBox.addItem ("Pitch scale: linear (even Hz)", 1); scaleBox.addItem ("Pitch scale: mostly linear", 2); scaleBox.addItem ("Pitch scale: halfway", 3);
        scaleBox.addItem ("Pitch scale: mostly musical", 4); scaleBox.addItem ("Pitch scale: musical (every octave the same height)", 5);
        scaleBox.setSelectedId (5, juce::dontSendNotification);
        scaleBox.setTooltip ("How the pitches are spread up the picture. Linear: every 1000 Hz takes the same height (the high end gets room, the low end is squashed). Musical: every octave takes the same height (like a piano). The steps in between blend the two.");
        detailBox.addItem ("Bass detail: blended (sharp top, fine bass)", 1); detailBox.addItem ("Bass detail: off (sharpest in time)", 2); detailBox.addItem ("Bass detail: extra fine", 3);
        detailBox.setSelectedId (1, juce::dontSendNotification);
        detailBox.setTooltip ("The picture is worked out twice: with a short window (sharp in time, good for clicks) and with a long one (sharp in pitch, good for bass notes). Blended: the long one is used at the bottom, the short one at the top, and they fade into each other in between (250 Hz to 1.5 kHz). Off: the short one only. Extra fine: an even longer window for the bass (below about 800 Hz).");
        detailBox.onChange = [this] { rescan(); };
        scaleBox.onChange = [this] { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; view.setScale (w[juce::jlimit (0, 4, scaleBox.getSelectedId() - 1)]); };
        view.onNote = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };
        hearCap.setText ("Listen to", juce::dontSendNotification); hearCap.setColour (juce::Label::textColourId, theme::text);
        hearBox.addItem ("Original", 1); hearBox.addItem ("Fixed (after Fix)", 2); hearBox.setSelectedId (1, juce::dontSendNotification);
        hearBox.setTooltip ("Which sound Play plays: the audio as it is, or the audio with the repair applied (available after you press Fix). Press Play while a loop runs to hear the other one.");
        playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        playBtn.setTooltip ("Plays the part of the picture you can see (zoom in with the left / right arrows to play a shorter piece, zoom out to hear more), through the mixers, with a playhead on the picture. No repair is needed: use it to listen for what has to be mended. Key: Space");
        loopBtn.setTooltip ("Repeat what is playing until you press Stop");
        stopBtn.setTooltip ("Stops playing");
        playBtn.onClick = [this] { playPressed(); };
        stopBtn.onClick = [this] { if (ctx.stop) ctx.stop(); };
        loopBtn.onClick = [this] { if (! isPlayingNow()) return; if (loopBtn.getToggleState()) playPressed(); else if (ctx.setLooping) ctx.setLooping (false); };
        hearBox.onChange = [this] { if (isPlayingNow()) playPressed(); };
        ticker.owner = this; ticker.startTimerHz (30);
        if (ctx.hold) ctx.hold (true);
        previewBtn.onClick = [this] { preview(); };
        compareBtn.onClick = [this] { toggleCompare(); };
        revertBtn.onClick = [this] { revert(); };
        finishBtn.onClick = [this] { finish(); };
        close.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        previewBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        finishBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        previewBtn.setTooltip ("Does the fix on the box you drew (or on every click found, in De-Click) and shows the result. It does not play, and nothing is written to the files yet: press Play to listen, and Undo if it did not work. You can then draw a box round another problem and press Fix again: the earlier fixes stay.");
        finishBtn.setTooltip ("Writes all the fixes into the files (everything between the marks is rendered again in one go). The Undo command of the window brings the original back afterwards.");
        close.setTooltip ("Closes the window and puts everything back as it was (nothing is kept).");
        revertBtn.setTooltip ("Takes the last fix off again. Press it again to take off the one before.");
        compareBtn.setTooltip ("Switches the picture between the original and the repaired sound.");
        view.onSelection = [this] { refreshControls(); invalidate(); };
        refreshControls();
        close.setButtonText ("Cancel");
        setWantsKeyboardFocus (true);                                              // Space plays / stops, whatever was clicked last
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RepairDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        status.setText ("Scanning all the tracks...", juce::dontSendNotification);
        auto load = ctx.load; double rate = ctx.rate; double secs = (double) ctx.length / ctx.rate;
        juce::Component::SafePointer<RepairDialog> self (this);
        auto spec = std::make_shared<MergedSpectrogram>();
        origTracks = std::make_shared<std::vector<std::vector<float>>>();
        auto keep = origTracks;
        const int detailN = detailSize();
        runJob ([load, rate, spec, keep, detailN] (AudioJob& j) -> juce::String
                {
                    auto tracks = load (j);
                    if (j.cancelled()) return "Cancelled.";
                    if (tracks.empty()) return "There is no audio here.";
                    *spec = SpectralRepair::spectrogram (tracks, rate, 1600, detailN);
                    *keep = std::move (tracks);
                    return spec->frames > 0 ? juce::String() : juce::String ("The marked area is too short to show (it needs at least about 0.05 s).");
                },
                [self, spec, secs] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); return; }
                    self->origSpec = std::make_shared<MergedSpectrogram> (*spec);
                    self->view.setData (std::move (*spec), secs);
                    self->status.setText (self->declickOnly
                        ? "Press Space (or Play) to listen to what is in view (left / right arrows zoom in time, up / down in pitch, drag the ruler to move). Clicks show as thin vertical lines. Drag a box to limit the search to that stretch of time (drag its edges to resize it), or leave it to search everything. Set the sensitivity and press Fix, then Play to listen; Undo if it did not work. Write back to file when you are happy.  Double-click the picture to clear the box."
                        : "Press Space (or Play) to listen to what is in view (left / right arrows zoom in time, up / down in pitch, drag the ruler to move). Drag a box around the noise (drag its edges or corners to resize it), set the controls, then press Fix, then Play to listen. Undo if it did not work; or draw a box round the next problem and Fix again (the earlier fixes stay). Write back to file when you are happy.  Double-click the picture to clear the box.", juce::dontSendNotification);
                    self->scanned = true; self->refreshControls();
                });
    }
    ~RepairDialog() override
    {
        ticker.stopTimer(); ticker.owner = nullptr;
        if (ctx.stop) ctx.stop();
        if (! accepted)                                                    // closing without Commit puts everything back as it was
        {
            if (applied && ctx.unapply) ctx.unapply();
            if (prepared && ctx.discard) ctx.discard();
        }
        if (ctx.hold) ctx.hold (false);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        auto rowC = r.removeFromBottom (32);
        r.removeFromBottom (4);
        auto rowT = r.removeFromBottom (30);                                     // the transport: Play / Stop / Loop, which sound, and the progress bar
        r.removeFromBottom (4);
        auto rowB = r.removeFromBottom (28);
        auto rowA = r.removeFromBottom (28);
        r.removeFromBottom (4);
        close.setBounds (rowC.removeFromRight (90)); rowC.removeFromRight (8);
        finishBtn.setBounds (rowC.removeFromRight (170)); rowC.removeFromRight (8);
        revertBtn.setBounds (rowC.removeFromRight (90)); rowC.removeFromRight (8);
        compareBtn.setBounds (rowC.removeFromRight (150)); rowC.removeFromRight (8);
        previewBtn.setBounds (rowC.removeFromRight (160));
        playBtn.setBounds (rowT.removeFromLeft (90)); rowT.removeFromLeft (6);
        stopBtn.setBounds (rowT.removeFromLeft (70)); rowT.removeFromLeft (8);
        loopBtn.setBounds (rowT.removeFromLeft (70)); rowT.removeFromLeft (14);
        hearCap.setBounds (rowT.removeFromLeft (70)); hearBox.setBounds (rowT.removeFromLeft (210)); rowT.removeFromLeft (14);
        bar.setBounds (rowT.reduced (0, 4));
        dirCap.setBounds (rowA.removeFromLeft (64)); dirBox.setBounds (rowA.removeFromLeft (330)); rowA.removeFromLeft (14);
        strengthCap.setBounds (rowA.removeFromLeft (56)); strength.setBounds (rowA);
        // second row: patch: surrounding length and before / after;  declick: the sensitivity
        surroundCap.setBounds (rowB.removeFromLeft (130)); surround.setBounds (rowB.removeFromLeft (330)); rowB.removeFromLeft (14);
        weightCap.setBounds (rowB.removeFromLeft (100)); weight.setBounds (rowB);
        sensCapRow = rowB; sensCap.setBounds (10, rowB.getY(), 130, rowB.getHeight()); sens.setBounds (150, rowB.getY(), 330, rowB.getHeight());
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        { auto ir = r.removeFromBottom (22); scaleBox.setBounds (ir.removeFromRight (300)); ir.removeFromRight (6); detailBox.setBounds (ir.removeFromRight (210)); info.setBounds (ir.reduced (2, 0)); }
        view.setBounds (r);
    }
protected:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)                                      // Space: play what is loaded, or stop
        {
            if (scanned && ! running) { if (isPlayingNow()) { if (ctx.stop) ctx.stop(); } else playPressed(); }
            return true;
        }
        if (scanned && view.zoomKey (k)) return true;                           // arrows: right / left zoom in / out in time, up / down in pitch
        return false;
    }
private:
    bool patchMode() const { return modeBox.getSelectedId() == 1; }
    void updateSurround() { const bool lr = dirBox.getSelectedId() != 2; view.setSurround (patchMode() && scanned, surround.getValue(), dirBox.getSelectedId() - 1, lr ? weight.getValue() / 100.0 : 0.5); }
    bool lookingAcross() const { return dirBox.getSelectedId() != 2; }
    /** The changes that make up the repaired sound: everything accepted so far, plus the change being previewed. */
    std::vector<RepairOp> currentOps() const
    {
        auto ops = committed;
        if (previewValid) ops.push_back (RepairOp { pvSpec, pvT0, pvT1 });
        return ops;
    }
    bool haveRepair() const { return previewValid || ! committed.empty(); }
    void invalidate()
    {
        if (running) { changedWhileRunning = true; return; }
        dropPendingPrepared();
        if (! previewValid) return;
        previewValid = false; pvTracks = nullptr;
        status.setText ("The settings changed: press Fix.", juce::dontSendNotification);
        refreshControls();
    }
    /** Back to the original sound, and the repaired files thrown away (they no longer match the settings). */
    void dropPrepared()
    {
        if (applied) { if (ctx.stop) ctx.stop(); if (ctx.unapply) ctx.unapply(); applied = false; }
        if (prepared) { if (ctx.discard) ctx.discard(); prepared = false; }
        preparedOps = 0;
    }
    /** Files that include a change which is not accepted (yet) are dropped; files that match the accepted changes are kept. */
    void dropPendingPrepared() { if (prepared && preparedOps != committed.size()) dropPrepared(); }
    bool isPlayingNow() const { return ctx.position && ctx.position() >= 0.0; }
    void tick()
    {
        const double t = ctx.position ? ctx.position() : -1.0;
        view.setPlayhead (t);
        playBtn.setButtonText (t >= 0.0 ? "Restart" : "Play");
    }
    struct Ticker : juce::Timer { RepairDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    /** What is played: the part of the picture that is in view (zoom in to hear less, out to hear more), not just the box. */
    void playRange()
    {
        if (ctx.play) ctx.play (view.visT0(), juce::jmin (seconds(), view.visT1()), loopBtn.getToggleState());
    }
    void playOriginal()
    {
        if (applied) { if (ctx.stop) ctx.stop(); if (ctx.unapply) ctx.unapply(); applied = false; }
        playRange();
        status.setText ("Playing the ORIGINAL sound, the part you can see. The playhead is on the picture.", juce::dontSendNotification);
        refreshControls();
    }
    void playRepaired()
    {
        if (! applied)
        {
            if (! ctx.apply || ! ctx.apply()) { status.setText ("The audio changed while the repair was being made, so it cannot be played. Press Fix again.", juce::dontSendNotification); prepared = false; preparedOps = 0; refreshControls(); return; }
            applied = true;
        }
        playRange();
        status.setText ("Playing the REPAIRED sound (through the repair). Write back to file keeps it, Undo takes the last fix off, or draw another box and Fix again.", juce::dontSendNotification);
        refreshControls();
    }
    void startRepairedPlayback()
    {
        const auto ops = currentOps();
        if (running || ops.empty()) return;
        if (prepared && preparedOps == ops.size()) { playRepaired(); return; }
        dropPrepared();
        auto prep = ctx.prepare;
        status.setText ("Making the repaired sound ready to listen to...", juce::dontSendNotification);
        previewBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
        juce::Component::SafePointer<RepairDialog> self (this);
        const auto n = ops.size();
        runJob ([prep, ops] (AudioJob& j) { return prep (ops, j); },
                [self, n] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err == "Cancelled." ? juce::String ("Cancelled.") : err, juce::dontSendNotification); self->refreshControls(); return; }
                    self->prepared = true; self->preparedOps = n;
                    if (self->changedWhileRunning) { self->changedWhileRunning = false; self->invalidate(); return; }
                    self->playRepaired();
                });
    }
    void playPressed()
    {
        if (! scanned || running) return;
        if (ctx.stop) ctx.stop();
        if (hearBox.getSelectedId() == 2)
        {
            if (haveRepair()) { startRepairedPlayback(); return; }
            status.setText ("There is no repair to listen to yet: press Fix first. Playing the original.", juce::dontSendNotification);
        }
        playOriginal();
    }
    void refreshControls()
    {
        const bool p = patchMode();
        updateSurround();
        for (auto* c : std::initializer_list<juce::Component*> { &dirBox, &dirCap, &strength, &strengthCap, &surround, &surroundCap }) c->setVisible (p);
        weight.setVisible (p && lookingAcross()); weightCap.setVisible (p && lookingAcross());
        sens.setVisible (! p); sensCap.setVisible (! p);
        if (view.hasSelection())
            info.setText ("Selected: " + juce::String (view.selT0(), 3) + " - " + juce::String (view.selT1(), 3) + " s  ("
                          + juce::String ((view.selT1() - view.selT0()) * 1000.0, 0) + " ms),  " + juce::String ((int) view.selF0()) + " - " + juce::String ((int) view.selF1()) + " Hz"
                          + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        else info.setText ("Nothing selected" + juce::String (p ? "" : " (the whole scanned area will be searched for clicks)")
                           + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        const bool ready = scanned && ! running;
        playBtn.setEnabled (ready); stopBtn.setEnabled (ready); loopBtn.setEnabled (ready); hearBox.setEnabled (ready);
        previewBtn.setEnabled (ready && (p ? view.hasSelection() : true));
        finishBtn.setEnabled (ready && haveRepair());
        revertBtn.setEnabled (ready && ! history.empty());
        compareBtn.setEnabled (ready && ! history.empty());
        compareBtn.setButtonText (showingAfter ? "Show before" : "Show fixed");
    }
    /** The settings as they are now (and the part of the area they apply to). */
    FixSpec buildSpec (juce::int64& t0, juce::int64& t1) const
    {
        FixSpec spec; t0 = 0; t1 = ctx.length;
        if (view.hasSelection()) { t0 = (juce::int64) std::llround (view.selT0() * ctx.rate); t1 = (juce::int64) std::llround (view.selT1() * ctx.rate); }
        if (patchMode())
        {
            spec.kind = FixSpec::Kind::Patch; spec.f0 = view.selF0(); spec.f1 = view.selF1();
            spec.repair.strength = strength.getValue() / 100.0;
            spec.repair.direction = (RepairDirection) (dirBox.getSelectedId() - 1);
            spec.repair.contextPct = surround.getValue();
            spec.repair.weighting = weight.getValue() / 100.0;
        }
        else { spec.kind = FixSpec::Kind::Declick; spec.sensitivity = sens.getValue(); }
        return spec;
    }
    double seconds() const { return (double) ctx.length / ctx.rate; }
    void preview()
    {
        if (! scanned || running || origTracks == nullptr) return;
        dropPendingPrepared();
        juce::int64 t0 = 0, t1 = 0; const auto spec = buildSpec (t0, t1);
        pvSpec = spec; pvT0 = t0; pvT1 = t1;
        auto src = origTracks; const double rate = ctx.rate;
        struct Out { MergedSpectrogram spec; std::shared_ptr<std::vector<std::vector<float>>> tracks; };
        auto out = std::make_shared<Out>();
        status.setText ("Fixing...", juce::dontSendNotification);
        previewBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false);
        juce::Component::SafePointer<RepairDialog> self (this);
        const int detailN = detailSize();
        runJob ([src, out, spec, t0, t1, rate, detailN] (AudioJob& j) -> juce::String
                {
                    auto tracks = std::make_shared<std::vector<std::vector<float>>> (*src);       // from the last accepted state
                    for (size_t i = 0; i < tracks->size(); ++i)
                    {
                        if (j.cancelled()) return "Cancelled.";
                        j.setProgress ((float) i / (float) tracks->size());
                        std::vector<std::vector<float>> ch; ch.push_back (std::move ((*tracks)[i]));
                        audioops::applyFix (ch, rate, (long) t0, (long) t1, spec, (long) (0.02 * rate));
                        (*tracks)[i] = std::move (ch[0]);
                    }
                    out->spec = SpectralRepair::spectrogram (*tracks, rate, 1600, detailN);
                    out->tracks = tracks;
                    return {};
                },
                [self, out] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    // the fix is kept straight away (it stays temporary until "Write back to file"); Undo takes it off again
                    self->history.push_back ({ self->origTracks, self->origSpec });
                    self->committed.push_back (RepairOp { self->pvSpec, self->pvT0, self->pvT1 });
                    self->origTracks = out->tracks;
                    self->origSpec = std::make_shared<MergedSpectrogram> (out->spec);
                    self->view.setData (std::move (out->spec), self->seconds(), true);
                    self->previewValid = false; self->showingAfter = true;
                    self->dropPendingPrepared();
                    self->changedWhileRunning = false;
                    self->hearBox.setSelectedId (2, juce::dontSendNotification);
                    self->status.setText ("Fix " + juce::String ((int) self->committed.size()) + " done (not written yet). Press Play to listen to it. If it worked, draw a box round the next problem and press Fix again, or press Write back to file. If not, press Undo and try again.", juce::dontSendNotification);
                    self->refreshControls();
                });
    }
    int detailSize() const { return juce::jlimit (0, 2, detailBox.getSelectedId() - 1); }
    /** Redraws the picture of the sound as it is now with another low-end detail (a longer or shorter analysis window). */
    void rescan()
    {
        if (! scanned || running || origTracks == nullptr) return;
        auto src = origTracks; const double rate = ctx.rate; const int n = detailSize();
        auto out = std::make_shared<MergedSpectrogram>();
        status.setText ("Redrawing with another low-end detail...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        runJob ([src, out, rate, n] (AudioJob&) -> juce::String { *out = SpectralRepair::spectrogram (*src, rate, 1600, n); return {}; },
                [self, out] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty() && out->frames > 0)
                    {
                        self->origSpec = std::make_shared<MergedSpectrogram> (*out);
                        self->view.setData (std::move (*out), self->seconds(), true);
                        self->showingAfter = true;
                        self->status.setText ("Redrawn. Blended: fine bass below, sharp clicks above. Choose \"off\" for the sharpest timing everywhere.", juce::dontSendNotification);
                    }
                    self->refreshControls();
                });
    }
    void toggleCompare()
    {
        if (history.empty() || origSpec == nullptr || history.back().spec == nullptr) return;
        showingAfter = ! showingAfter;
        view.setData (showingAfter ? *origSpec : *history.back().spec, seconds(), true);
        status.setText (showingAfter ? "Showing the fixed sound." : "Showing the sound before the last fix.", juce::dontSendNotification);
        refreshControls();
    }
    /** Undo: takes the last fix off again (as many times as there are fixes). */
    void revert()
    {
        if (history.empty() || running) return;
        if (ctx.stop) ctx.stop();
        origTracks = history.back().tracks; origSpec = history.back().spec; history.pop_back();
        if (! committed.empty()) committed.pop_back();
        dropPendingPrepared();
        previewValid = false; showingAfter = true;
        if (committed.empty()) hearBox.setSelectedId (1, juce::dontSendNotification);
        view.setData (*origSpec, seconds(), true);
        status.setText (committed.empty() ? "Undone: back to the original. Nothing is changed." : "Last fix undone (" + juce::String ((int) committed.size()) + " fix(es) left).", juce::dontSendNotification);
        refreshControls();
    }
    /** Finish: everything between the marks is rendered again in one go, with every accepted change, and put back. */
    void finish()
    {
        if (running) return;
        const auto ops = currentOps();
        if (ops.empty()) { closeDialogOf (this); return; }
        if (prepared && preparedOps == ops.size()) { finishCommit(); return; }
        dropPrepared();
        auto prep = ctx.prepare;
        previewBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
        status.setText ("Writing: repairing every track from the first mark to the last and making the new files...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        const auto n = ops.size();
        runJob ([prep, ops] (AudioJob& j) { return prep (ops, j); },
                [self, n] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty()) { self->prepared = true; self->preparedOps = n; self->finishCommit(); }
                    else { self->status.setText (err, juce::dontSendNotification); self->refreshControls(); }
                });
    }
    void finishCommit()
    {
        if (ctx.stop) ctx.stop();
        if (! applied)
        {
            if (! ctx.apply || ! ctx.apply()) { status.setText ("The audio changed while the repair was being made, so nothing was changed. Please try again.", juce::dontSendNotification); prepared = false; preparedOps = 0; refreshControls(); return; }
            applied = true;
        }
        if (ctx.accept) ctx.accept();
        accepted = true;
        closeDialogOf (this);
    }
    RepairContext ctx;
    bool declickOnly = false;                                 // true: the De-Click window (finds and mends clicks); false: Spectral Repair (rebuilds a box of the picture)
    InfoNote intro; juce::Label info, sensCap, strengthCap, dirCap, surroundCap, weightCap;
    SpectrogramView view; juce::ComboBox modeBox, dirBox, scaleBox, detailBox; juce::Slider sens, strength, surround, weight;
    juce::TextButton playBtn { "Play" }, stopBtn { "Stop" };
    juce::ToggleButton loopBtn { "Loop" };
    juce::Label hearCap; juce::ComboBox hearBox;
    bool prepared = false, applied = false, accepted = false, changedWhileRunning = false;
    size_t preparedOps = 0;                                        // how many changes the prepared files contain
    std::vector<RepairOp> committed;                               // the changes accepted so far (in order)
    std::shared_ptr<std::vector<std::vector<float>>> pvTracks;     // the audio with the previewed change in it
    juce::TextButton previewBtn { "Fix" }, compareBtn { "Show before" }, revertBtn { "Undo" }, finishBtn { "Write back to file" }, close { "Cancel" };
    struct Step { std::shared_ptr<std::vector<std::vector<float>>> tracks; std::shared_ptr<MergedSpectrogram> spec; };
    std::vector<Step> history;                                     // the state before each fix, for Undo
    bool scanned = false, previewValid = false, showingAfter = false;
    juce::Rectangle<int> sensCapRow;
    std::shared_ptr<std::vector<std::vector<float>>> origTracks;
    std::shared_ptr<MergedSpectrogram> origSpec, repSpec;
    FixSpec pvSpec; juce::int64 pvT0 = 0, pvT1 = 0;
};

// ======================================================================================================== the import dialog
class ImportDialog : public JobDialogBase
{
public:
    ImportDialog (AppContext& a, const juce::Uuid& wid, const juce::Uuid& eid, ImportPlan p) : app (a), windowId (wid), editId (eid), plan (std::move (p))
    {
        setSize (640, 480);
        for (auto* x : std::initializer_list<juce::Component*> { &summary, &doIt, &cancel }) addAndMakeVisible (x);
        summary.setMultiLine (true); summary.setReadOnly (true); summary.setCaretVisible (false); summary.setScrollbarsShown (true);
        summary.setColour (juce::TextEditor::backgroundColourId, theme::field); summary.setFont (juce::FontOptions (13.0f));
        juce::String t;
        t << juce::String ((int) plan.takes.size()) << " take(s), " << juce::String ((int) plan.tracks.size()) << " track(s).\n\n";
        for (size_t i = 0; i < plan.takes.size(); ++i)
        {
            const auto& tk = plan.takes[i];
            t << "Take " << juce::String ((int) i + 1) << (tk.number > 0 ? "  (number " + juce::String (tk.number) + " in the file names)" : juce::String())
              << ":  " << juce::String ((int) tk.parts.size()) << " track(s),  " << formatTime ((double) tk.length / juce::jmax (1.0, tk.sampleRate)).substring (3, 11)
              << ",  " << juce::String (tk.sampleRate, 0) << " Hz\n";
        }
        t << "\nTracks (matched to the tracks you already have by name; new ones are added):\n";
        for (auto& tr : plan.tracks) t << "   " << tr.name << (tr.channels == 2 ? "  (stereo)" : "") << "\n";
        if (plan.notes.size() > 0) { t << "\nNotes:\n"; for (auto& n : plan.notes) t << "   " << n << "\n"; }
        if (! editId.isNull()) t << "\nEach take is placed in the edit, one after the other, with every track on its own row of the edit window (ready for mixing).\n";
        t << "\nThe audio is copied into this project's folder as 24-bit WAV files; your original files are not touched.";
        summary.setText (t, false);
        doIt.setButtonText (editId.isNull() ? "Import " + juce::String ((int) plan.takes.size()) + " take(s)" : "Bring into the edit");
        doIt.setColour (juce::TextButton::buttonColourId, theme::accent);
        doIt.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (30);
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); doIt.setBounds (bottom.removeFromRight (180));
        r.removeFromBottom (6); bar.setBounds (r.removeFromBottom (22)); status.setBounds (r.removeFromBottom (22));
        summary.setBounds (r);
    }
private:
    void start()
    {
        auto* w = app.project.findTakeWindow (windowId);
        auto* ed = app.project.findEdit (editId);
        if (editId.isNull() ? w == nullptr : ed == nullptr) return;
        if (ed != nullptr && ! ed->isEmpty() && ! plan.takes.empty() && std::abs (ed->sampleRate - plan.takes.front().sampleRate) > 0.5)
        {
            status.setText ("This audio is " + juce::String (plan.takes.front().sampleRate, 0) + " Hz but the edit is " + juce::String (ed->sampleRate, 0) + " Hz.", juce::dontSendNotification);
            return;
        }
        doIt.setEnabled (false); status.setText ("Copying the audio...", juce::dontSendNotification);
        std::vector<int> numbers; for (size_t i = 0; i < plan.takes.size(); ++i) numbers.push_back ((w != nullptr ? w->nextNumber : 1) + (int) i);
        const auto folder = w != nullptr ? app.project.takeFolder (*w) : app.project.audioFolder().getChildFile ("Imported into " + sanitiseForFile (ed->name));
        const auto label = w != nullptr ? w->labelFor (TakeGroup()) : juce::String ("Imported");
        auto result = std::make_shared<std::vector<std::vector<juce::File>>>();
        auto planCopy = std::make_shared<ImportPlan> (plan);
        auto* self = this; auto& appRef = app; const auto wid = windowId; const auto eid = editId;
        juce::Component::SafePointer<ImportDialog> safe (this);
        runJob ([planCopy, folder, label, numbers, result] (AudioJob& j) { return copyImportFiles (formats(), *planCopy, folder, label, numbers, *result, &j); },
                [safe, planCopy, result, &appRef, wid, eid, self] (const juce::String& err)
                {
                    juce::ignoreUnused (self);
                    if (safe == nullptr) return;
                    if (err.isNotEmpty()) { safe->status.setText (err, juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                    if (eid.isNull()) finishImport (appRef, wid, *planCopy, *result); else finishImportToEdit (appRef, eid, *planCopy, *result);
                    closeDialogOf (safe.getComponent());
                });
    }

    /** Finds (by name and channel count) or creates the project track of each import track. */
    static std::map<juce::String, juce::Uuid> mapTracks (AppContext& app, const ImportPlan& plan, TakeWindowDef* w)
    {
        auto& p = app.project;
        std::map<juce::String, juce::Uuid> trackFor;
        std::set<int> usedInputs; for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) usedInputs.insert (t.inputOf (c));
        for (auto& tr : plan.tracks)
        {
            const TrackDef* found = nullptr;
            for (auto& t : p.tracks) if (t.name.equalsIgnoreCase (tr.name) && t.channelCount() == tr.channels) { found = &t; break; }
            if (found == nullptr)
            {
                int first = -1;
                for (int i = 0; i + tr.channels <= (int) p.inputs.size() && first < 0; ++i)
                {
                    bool freeSlot = true; for (int c = 0; c < tr.channels; ++c) freeSlot = freeSlot && ! usedInputs.count (i + c);
                    if (freeSlot) first = i;
                }
                auto& nt = p.addTrack (tr.name, tr.channels == 2 ? TrackFormat::Stereo : TrackFormat::Mono, first);
                if (first < 0) nt.inputs.fill (-1); else for (int c = 0; c < tr.channels; ++c) usedInputs.insert (first + c);
                found = &nt;
            }
            trackFor[tr.key] = found->id; if (w != nullptr) w->unhide (found->id);
        }
        return trackFor;
    }

    /** Edit window import: one piece of the edit per imported take (placed one after the other), each file on its own track. */
    static void finishImportToEdit (AppContext& app, const juce::Uuid& eid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files)
    {
        auto* e = app.project.findEdit (eid);
        if (e == nullptr) return;
        auto trackFor = mapTracks (app, plan, nullptr);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            EditRegion r;
            r.id = juce::Uuid();
            r.takeName = files[ti].empty() ? juce::String ("Imported") : files[ti].front().getFileNameWithoutExtension();
            r.sampleRate = tk.sampleRate; r.sourceLength = tk.length; r.srcIn = 0; r.srcOut = tk.length;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                RegionFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                if (auto* tr = app.project.findTrack (f.trackId)) f.trackName = tr->name;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                r.files.push_back (f);
            }
            if (r.files.empty() || r.length() <= 0) continue;
            if (! e->trackIds.empty())
                for (auto& f : r.files) if (std::find (e->trackIds.begin(), e->trackIds.end(), f.trackId) == e->trackIds.end()) e->trackIds.push_back (f.trackId);
            e->insertIndex = -1;
            e->insertRegion (r);
        }
        app.project.structureChanged();
        app.project.changed();
    }

    static void finishImport (AppContext& app, const juce::Uuid& wid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files)
    {
        auto* w = app.project.findTakeWindow (wid);
        if (w == nullptr) return;
        auto& p = app.project;
        auto trackFor = mapTracks (app, plan, w);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            TakeGroup g; g.id = juce::Uuid(); g.number = w->nextNumber++;
            g.startSeconds = w->groups.empty() ? 0.0 : std::ceil (w->endSeconds() + 2.0);
            g.lengthSamples = tk.length; g.sampleRate = tk.sampleRate; g.recordedAt = tk.modified;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                TakeFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                if (auto* tr = p.findTrack (f.trackId)) f.trackName = tr->name;
                g.files.push_back (f);
            }
            w->groups.push_back (std::move (g));
        }
        p.structureChanged();
    }

    AppContext& app; juce::Uuid windowId, editId; ImportPlan plan;
    juce::TextEditor summary; juce::TextButton doIt { "Import" }, cancel { "Cancel" };
};

static void openImportDialog (AppContext& app, const juce::Uuid& wid, const juce::Uuid& eid, const juce::Array<juce::File>& chosen, juce::Component* parent)
{
    juce::Array<juce::File> files;
    for (auto& f : chosen)
    {
        if (f.isDirectory()) { for (auto& c : f.findChildFiles (juce::File::findFiles, false)) if (isAudioFile (c)) files.add (c); }
        else if (isAudioFile (f)) files.add (f);
    }
    if (files.isEmpty()) { say ("Import takes", "No audio files were found. Fermata imports WAV (including polyphonic / multichannel WAV), AIFF and FLAC files."); return; }
    juce::StringArray problems;
    auto sources = inspectFiles (formats(), files, problems);
    if (sources.empty()) { say ("Import takes", "None of those files could be read.\n\n" + problems.joinIntoString ("\n")); return; }
    auto plan = buildImportPlan (std::move (sources));
    for (auto& pr : problems) plan.notes.add (pr);
    launchDialog (std::make_unique<ImportDialog> (app, wid, eid, std::move (plan)), eid.isNull() ? "Import takes" : "Import into the edit", parent);
}

void importTakes (AppContext& app, const juce::Uuid& wid, const juce::Array<juce::File>& files, juce::Component* parent, const juce::Uuid& intoEdit)
{
    if (! files.isEmpty()) { openImportDialog (app, wid, intoEdit, files, parent); return; }
    auto chooser = std::make_shared<juce::FileChooser> ("Choose the audio files of the session (select them all)", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.flac");
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems | juce::FileBrowserComponent::canSelectFiles,
                          [chooser, &app, wid, intoEdit, safeParent] (const juce::FileChooser& fc)
                          { if (fc.getResults().size() > 0) openImportDialog (app, wid, intoEdit, fc.getResults(), safeParent.getComponent()); });
}

void showImportMenu (AppContext& app, const juce::Uuid& wid, juce::Component* source, const juce::Uuid& intoEdit)
{
    juce::PopupMenu m;
    m.addItem (1, "Choose audio files...");
    m.addItem (2, "Choose a folder (every audio file in it)...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (source), [&app, wid, intoEdit, source = juce::Component::SafePointer<juce::Component> (source)] (int r)
    {
        if (r == 1) importTakes (app, wid, {}, source.getComponent(), intoEdit);
        else if (r == 2)
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Choose the folder with the session's audio files");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [chooser, &app, wid, intoEdit, source] (const juce::FileChooser& fc)
                                  { if (fc.getResult() != juce::File()) openImportDialog (app, wid, intoEdit, { fc.getResult() }, source.getComponent()); });
        }
    });
}

// ======================================================================================================== applying a fix to the model
static juce::File takeFixDest (const juce::File& orig, const FixSpec& spec)
{
    auto stem = orig.getFileNameWithoutExtension();
    const int p = stem.lastIndexOf (" (");
    if (p > 0 && stem.endsWith (")") && (stem.substring (p).containsAnyOf ("+-") || stem.contains ("repair") || stem.contains ("declick") || stem.contains ("pitch curve"))) stem = stem.substring (0, p);
    return audioops::uniqueFile (orig.getParentDirectory().getChildFile (stem + " (" + spec.shortName() + ").wav"));
}

static void setUndoForTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, std::vector<TakeFile> oldFiles, const juce::String& label)
{
    app.fixUndoLabel = label;
    app.fixUndo = [&app, wid, gid, oldFiles]
    {
        if (auto* w = app.project.findTakeWindow (wid)) if (auto* g = w->findGroup (gid)) { g->files = oldFiles; app.project.changed(); }
    };
}

static void setUndoForEdit (AppContext& app, const juce::Uuid& eid, std::vector<EditRegion> oldRegions, const juce::String& label)
{
    app.fixUndoLabel = label;
    app.fixUndo = [&app, eid, oldRegions]
    {
        if (auto* e = app.project.findEdit (eid)) { e->regions = oldRegions; app.project.changed(); }
    };
}

/** Everything one region of an edit needs for a fix, gathered on the message thread. */
struct RegionJob
{
    juce::Uuid regionId; juce::int64 a = 0, b = 0;           // the piece [a,b) of the take that becomes its own region
    juce::int64 outFrom = 0, outTo = 0, fixFrom = 0, fixTo = 0;
    std::vector<juce::File> src, dest; std::vector<juce::int64> fileStart;
    std::vector<size_t> idx;                                  // which files of the region these are (all of them, or just the tracks chosen with Alt + drag)
    std::vector<audioops::FixOp> ops;                         // the changes made to this piece, in take samples (one, or several accepted in the repair window)
    std::vector<audioops::PieceResult> results;
    double curveZero = 0.0;                                   // (pitch curve) samples from the first sample fixed to curve time 0
};

static juce::File editFixDest (const juce::File& folder, const EditRegion& r, const RegionFile& f, const FixSpec& spec)
{
    return audioops::uniqueFile (folder.getChildFile (sanitiseForFile (r.takeName.upToFirstOccurrenceOf (" (", false, false) + " - " + f.trackName) + " (" + spec.shortName() + ").wav"));
}

/** Prepares the file lists of one piece [a,b) of a region (message thread). */
static bool prepRegionJob (const juce::File& folder, const EditRegion& r, RegionJob& j, juce::int64 a, juce::int64 b, juce::int64 outFrom, juce::int64 outTo,
                           juce::int64 fixFrom, juce::int64 fixTo, const FixSpec& spec, const std::vector<juce::Uuid>& onlyTracks = {})
{
    j.regionId = r.id; j.a = a; j.b = b; j.outFrom = outFrom; j.outTo = outTo; j.fixFrom = fixFrom; j.fixTo = fixTo;
    j.ops = { audioops::FixOp { spec, fixFrom, fixTo } };
    for (size_t fi = 0; fi < r.files.size(); ++fi)
    {
        auto& f = r.files[fi];
        if (! onlyTracks.empty() && std::find (onlyTracks.begin(), onlyTracks.end(), f.trackId) == onlyTracks.end()) continue;
        j.idx.push_back (fi);
        j.src.push_back (f.file); j.fileStart.push_back (f.fileStart); j.dest.push_back (editFixDest (folder, r, f, spec));
        // two files of one region must not get the same new name
        for (size_t k = 0; k + 1 < j.dest.size(); ++k) if (j.dest[k] == j.dest.back()) j.dest.back() = audioops::uniqueFile (j.dest.back().getSiblingFile (j.dest.back().getFileNameWithoutExtension() + " b.wav"));
    }
    return ! j.src.empty();
}

/** Job thread: makes the new short files of one region job. */
static juce::String runRegionJob (RegionJob& j, const FixSpec& spec, AudioJob& job, float p0, float p1)
{
    juce::ignoreUnused (spec);                                // (what to do is in j.ops, made by prepRegionJob)
    j.results.clear();
    for (size_t k = 0; k < j.src.size(); ++k)
    {
        if (job.cancelled()) { for (auto& r : j.results) r.file.deleteFile(); j.results.clear(); return "Cancelled."; }
        job.setProgress (p0 + (p1 - p0) * (float) k / (float) j.src.size());
        juce::String err;
        auto res = audioops::fixPiece (formats(), j.src[k], j.fileStart[k], j.outFrom, j.outTo, j.ops, j.dest[k], err);
        if (! res.ok) { for (auto& r : j.results) r.file.deleteFile(); j.results.clear(); return err; }
        j.results.push_back (res);
    }
    return {};
}

/** Message thread: cuts the region at a and b and swaps the middle piece onto the new files. */
static bool applyRegionJob (EditDef& e, const RegionJob& j, const juce::String& suffix)
{
    int i = e.indexOf (j.regionId);
    if (i < 0) return false;
    {
        const auto& r = e.regions[(size_t) i];
        if (j.a < r.srcIn || j.b > r.srcOut || j.b <= j.a || j.results.size() != j.idx.size()) return false;
        for (auto& res : j.results) if (! res.ok || res.from > j.a || res.to < j.b) return false;        // the new files must cover the piece
    }
    const bool cutAfter = j.b < e.regions[(size_t) i].srcOut, cutBefore = j.a > e.regions[(size_t) i].srcIn;
    if (cutAfter)  e.splitRegion (i, j.b - e.regions[(size_t) i].srcIn);
    if (cutBefore) i = e.splitRegion (i, j.a - e.regions[(size_t) i].srcIn);
    auto& mid = e.regions[(size_t) i];
    // The new piece and the old audio around it are almost the same sound, so the crossfades where they meet are LINEAR:
    // the two gains then always add up to exactly 1 and there is no bump (an equal-power fade would add about 3 dB in the middle).
    mid.curve = FadeCurve::Linear;
    if (cutBefore && i > 0) e.regions[(size_t) i - 1].curve = FadeCurve::Linear;
    if (cutAfter && i + 1 < (int) e.regions.size()) e.regions[(size_t) i + 1].curve = FadeCurve::Linear;
    // the joins made here are only there to put the fixed audio in: they are not edit points (no number, skipped by next / previous fade in the Trim window)
    if (cutBefore) { mid.fixIn = true; if (i > 0) e.regions[(size_t) i - 1].fixOut = true; }
    if (cutAfter)  { mid.fixOut = true; if (i + 1 < (int) e.regions.size()) e.regions[(size_t) i + 1].fixIn = true; }
    for (size_t k = 0; k < j.idx.size() && j.idx[k] < mid.files.size(); ++k) { mid.files[j.idx[k]].file = j.results[k].file; mid.files[j.idx[k]].fileStart = j.results[k].from; }
    mid.takeName = mid.takeName.upToFirstOccurrenceOf (" (", false, false) + " (" + suffix + ")";
    return true;
}

// ======================================================================================================== take window: pitch / repair
static TakeWindowDef* takeWin (AppContext& app, const juce::Uuid& wid) { return app.project.findTakeWindow (wid); }

/** The take and the marked part (seconds in the take) from the take window's Edit IN / OUT marks; null + message if none. */
static TakeGroup* markedTake (AppContext& app, TakeWindowDef& w, double& in, double& out, const juce::String& title)
{
    auto* g = w.findGroup (w.editTake);
    if (g == nullptr || w.editIn < 0.0 || w.editOut <= w.editIn)
    {
        say (title, "First mark the part with Edit IN [1] and Edit OUT [2] (in one take). To be safe, mark a little more than you need.");
        return nullptr;
    }
    juce::ignoreUnused (app);
    in = w.editIn; out = juce::jmin (w.editOut, g->lengthSeconds());
    return g;
}

/** Indices of the files of the take that are marked (all of them, or the tracks chosen with Alt + drag). */
static std::vector<size_t> takeSel (const TakeWindowDef& w, const TakeGroup& g)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < g.files.size(); ++i) if (w.editUses (g.files[i].trackId)) v.push_back (i);
    return v;
}

void pitchTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, "Pitch correction"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch correction", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    PitchContext c; c.allowPad = false;
    c.what = "Shifts the pitch of the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + ") on every track of the take. "
             "New audio files are made and put in the same place; the originals are not touched (Undo fix brings them back).";
    auto jobFiles = std::make_shared<audioops::TakeFixResult>();
    auto spec = std::make_shared<FixSpec>();
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch correction", "None of the tracks you marked with Alt + drag are in this take."); return; }
    std::vector<juce::File> srcAll; for (auto i : sel) srcAll.push_back (g->files[i].file);
    c.work = [srcAll, in, out, rate, jobFiles, spec] (double cents, double, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents;
        std::vector<juce::File> src = srcAll, dst;
        for (auto& f : src) dst.push_back (takeFixDest (f, *spec));
        *jobFiles = audioops::fixTakeFiles (formats(), src, dst, (juce::int64) std::llround (in * rate), (juce::int64) std::llround (out * rate), *spec, &j);
        return jobFiles->error;
    };
    c.finish = [&app, wid, gid, jobFiles, spec, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || jobFiles->newFiles.size() != sel.size()) return;
        auto old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = jobFiles->newFiles[k];
        setUndoForTake (app, wid, gid, old, "pitch correction");
        app.project.changed();
    };
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

void repairTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent, bool declick)
{
    const juce::String nm = declick ? "De-Click" : "Spectral Repair";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, nm); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (nm, "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say (nm, "None of the tracks you marked with Alt + drag are in this take."); return; }
    std::vector<juce::File> src; for (auto i : sel) src.push_back (g->files[i].file);
    RepairContext c; c.rate = rate; c.length = to - from;
    c.what = declick ? "All " + juce::String ((int) src.size()) + " tracks of " + w->displayName (*g) + " are shown as one picture. Clicks are found and mended on every track (drag a box to limit the search to a stretch of time). New files are made; the originals stay."
                     : "All " + juce::String ((int) src.size()) + " tracks of " + w->displayName (*g) + " are shown as one picture. Draw a box round the noise (time and pitch), then press Preview: "
                       "the box is rebuilt from the clean sound next to it, on every track. New files are made; the originals stay.";
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, src.size()));
            auto r = audioops::openReader (formats(), src[i]); if (r == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*r, from, to - from, ch);
            std::vector<float> mono (ch.empty() ? 0 : ch[0].size(), 0.0f);
            for (auto& x : ch) for (size_t n = 0; n < mono.size(); ++n) mono[n] += x[n] / (float) ch.size();
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; bool applied = false; };
    auto st = std::make_shared<State>();
    c.prepare = [src, from, st] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        std::vector<audioops::FixOp> fo;
        for (auto& o : ops) fo.push_back (audioops::FixOp { o.spec, from + o.t0, from + o.t1 });
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, ops.back().spec));
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, fo, &j);          // one new file per track with every change in it
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;
    };
    c.discard = deleteFiles;
    c.accept = [&app, wid, gid, st, declick] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, (declick ? "de-click" : "spectral repair")); app.project.changed(); };
    c.play = [&app, wid, gid, in, nm] (double a, double b, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + a, in + b, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (std::make_unique<RepairDialog> (std::move (c), declick), nm, parent, true);
}

void pitchCurveTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, "Pitch curve"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch curve", "None of the tracks you marked with Alt + drag are in this take."); return; }
    std::vector<juce::File> src; for (auto i : sel) src.push_back (g->files[i].file);
    CurveContext c; c.rate = rate; c.length = to - from;
    c.what = "Draw how the pitch should change along the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + "), on every track. "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Audition to hear it, then Accept or Revert. "
             "The original files are never touched.";
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; bool applied = false; };
    auto st = std::make_shared<State>();
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (to - from), 0.0f);
        for (auto& f : src)
        {
            if (j.cancelled()) break;
            auto rd = audioops::openReader (formats(), f); if (rd == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*rd, from, to - from, ch);
            for (auto& x : ch) for (size_t n = 0; n < x.size() && n < mix.size(); ++n) mix[n] += x[n];
        }
        std::vector<std::vector<float>> o; o.push_back (std::move (mix)); return o;
    };
    c.prepare = [src, from, to, st] (const FixSpec& spec, double, AudioJob& j) -> juce::String
    {
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, spec));
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, from, to, spec, &j);
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.revert = [&app, wid, gid, st, deleteFiles]
    {
        if (st->applied)
        {
            auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
            st->applied = false;
        }
        deleteFiles();
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;                                  // (the corrected files are kept, ready to be put back)
    };
    c.accept = [&app, wid, gid, st] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, "pitch curve"); app.project.changed(); };
    c.play = [&app, wid, gid, in, out] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + fromSec, juce::jmin (out, in + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (std::make_unique<CurveDialog> (std::move (c)), "Pitch curve", parent, true);
}

// ======================================================================================================== edit window: pitch / repair
static EditDef* editOf (AppContext& app, const juce::Uuid& id) { return app.project.findEdit (id); }

void pitchEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch correction", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch correction", "There is no audio between the marks."); return; }
    PitchContext c; c.allowPad = true;
    c.what = "Shifts the pitch of the audio between marks 1 and 2 (" + juce::String (e->fixOut - e->fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)) on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The audio is replaced in the same place by new files; the originals are not touched (Undo fix brings them back). "
             "'Process extra' also corrects that much more audio each side, so you can move the edit points outward later.";
    auto jobs = std::make_shared<std::vector<RegionJob>>(); auto spec = std::make_shared<FixSpec>();
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.work = [fixFolder, regionsCopy, tIn, tOut, rate, jobs, spec, only] (double cents, double padSec, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents; jobs->clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, a - pad, b + pad, a - pad, b + pad, *spec, only);
            jobs->push_back (std::move (rj));
        }
        for (size_t i = 0; i < jobs->size(); ++i)
        {
            const auto err = runRegionJob ((*jobs)[i], *spec, j, (float) i / (float) jobs->size(), (float) (i + 1) / (float) jobs->size());
            if (err.isNotEmpty()) { for (auto& done : *jobs) for (auto& r : done.results) r.file.deleteFile(); jobs->clear(); return err; }
        }
        return {};
    };
    c.finish = [&app, eid, jobs, spec]
    {
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || jobs->empty()) return;
        const auto old = e2->regions; bool allOk = true;
        for (size_t i = jobs->size(); i-- > 0;) allOk = applyRegionJob (*e2, (*jobs)[i], spec->shortName()) && allOk;
        if (! allOk) { e2->regions = old; say ("Pitch correction", "The edit changed while the audio was being processed, so nothing was changed. Please try again."); return; }
        setUndoForEdit (app, eid, old, "pitch correction");
        app.project.changed();
    };
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

void repairEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent, bool declick)
{
    const juce::String nm = declick ? "De-Click" : "Spectral Repair";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (nm, "First mark the area with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    // every piece the marks touch: the marks may run across edit points (joins), and then the whole marked area is loaded, piece after piece
    juce::int64 lastEnd = 0; int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) { ++pieces; lastEnd = juce::jmax (lastEnd, r.endSample()); }
    if (pieces == 0) { say (nm, "There is no audio between the marks."); return; }
    const auto from = tIn, to = juce::jmin (tOut, lastEnd);
    if (to - from < (juce::int64) (0.05 * rate)) { say (nm, "The marked area is too short (at least about 0.05 s)."); return; }
    const auto only = e->fixTracks;
    // the tracks (one picture row each) and, for each, where its audio comes from along the marked area: whoever owns each moment (the piece that is on top there)
    struct Part { juce::File file; juce::int64 fileStart = 0, srcA = 0, n = 0, dst = 0; };
    std::vector<juce::Uuid> trackIds; std::vector<std::vector<Part>> parts;
    {
        for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to)
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                if (std::find (trackIds.begin(), trackIds.end(), f.trackId) == trackIds.end()) { trackIds.push_back (f.trackId); parts.emplace_back(); }
            }
        std::vector<juce::int64> cuts { from, to };
        for (auto& r : e->regions) { if (r.startSample > from && r.startSample < to) cuts.push_back (r.startSample); if (r.endSample() > from && r.endSample() < to) cuts.push_back (r.endSample()); }
        std::sort (cuts.begin(), cuts.end()); cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
        {
            const auto t0 = cuts[k], t1 = cuts[k + 1];
            const int ri = e->regionAt (t0 + (t1 - t0) / 2);
            if (ri < 0) continue;                                                       // a gap: silence
            const auto& r = e->regions[(size_t) ri];
            for (auto& f : r.files)
            {
                const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                if (ti == trackIds.end()) continue;
                parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (t0 - r.startSample), t1 - t0, t0 - from });
            }
        }
    }
    if (trackIds.empty()) { say (nm, "None of the tracks you marked with Alt + drag are in the marked area."); return; }
    RepairContext c; c.rate = rate; c.length = to - from;
    c.what = declick ? juce::String (only.empty() ? "All " : "The ") + juce::String ((int) trackIds.size()) + " tracks between marks 1 and 2 are shown as one picture, "
                       + (pieces > 1 ? "across the edit points (" + juce::String (pieces) + " pieces). " : juce::String ("")) + "Clicks are found and mended on every track (drag a box to limit the search to that stretch of time, drag its edges to resize it). Set the sensitivity, then press Fix and Play to listen; Undo if it did not work, and Write back to file when you are happy. "
                       "Each piece is repaired in its own place: the edit points and their fades stay exactly as they are, and the original files are not touched (Undo fix brings them back)."
                     : juce::String (only.empty() ? "All " : "The ") + juce::String ((int) trackIds.size()) + " tracks between marks 1 and 2 are shown as one picture, "
                       + (pieces > 1 ? "across the edit points (" + juce::String (pieces) + " pieces). " : juce::String ("")) + "Draw a box round the noise (time and pitch), then press Fix: "
                       "the box is rebuilt from the clean sound next to it, on every track. Press Play to listen, Undo if it did not work, and Write back to file when you are happy. Each piece is repaired in its own place: the edit points and their fades stay exactly as they are, and the original files are not touched (Undo fix brings them back).";
    const auto total = to - from;
    c.load = [parts, total] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, parts.size()));
            std::vector<float> mono ((size_t) total, 0.0f);
            for (auto& pt : parts[i])
            {
                auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                juce::int64 rel = pt.srcA - pt.fileStart, n = pt.n, dst = pt.dst;
                if (rel < 0) { n += rel; dst -= rel; rel = 0; }
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, n, ch);
                for (auto& x : ch) for (size_t k = 0; k < x.size() && dst + (juce::int64) k < total; ++k) mono[(size_t) dst + k] += x[k] / (float) ch.size();
            }
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { std::vector<RegionJob> jobs; std::vector<EditRegion> old; juce::String suffix; bool applied = false; };
    auto st = std::make_shared<State>();
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    auto deleteFiles = [st] { for (auto& rj : st->jobs) for (auto& r : rj.results) r.file.deleteFile(); st->jobs.clear(); };
    c.prepare = [fixFolder, regionsCopy, from, to, rate, st, only, deleteFiles] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        deleteFiles();
        st->suffix = ops.back().spec.shortName();
        const auto h = (juce::int64) (1.0 * rate);        // each piece is made again from its own sound with 1 s of untouched sound either side (the audio next to the marks, for the fades)
        const auto shortest = (juce::int64) (0.03 * rate);
        // each piece gets the changes that fall inside it, in ITS OWN samples: the same box or click search, applied to its own audio
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= from || r.startSample >= to) continue;
            const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
            std::vector<audioops::FixOp> mine;
            for (auto& o : ops)
            {
                const auto i0 = juce::jmax (r0, from + o.t0), i1 = juce::jmin (r1, from + o.t1);
                if (i1 - i0 < 2) continue;
                if (o.t1 - o.t0 > shortest && i1 - i0 <= shortest) continue;           // a sliver of a bigger search is not taken for 'the click itself'
                mine.push_back (audioops::FixOp { o.spec, r.srcIn + (i0 - r.startSample), r.srcIn + (i1 - r.startSample) });
            }
            if (mine.empty()) continue;
            RegionJob rj; const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, a - h, b + h, a, b, ops.back().spec, only);
            rj.ops = mine;
            st->jobs.push_back (std::move (rj));
        }
        if (st->jobs.empty()) return "Nothing to do in this area.";
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            const auto err = runRegionJob (st->jobs[i], ops.back().spec, j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { deleteFiles(); return err; }
        }
        return {};
    };
    c.apply = [&app, eid, st, deleteFiles]
    {
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) return false;
        st->old = e2->regions; bool ok = true;
        for (size_t i = st->jobs.size(); i-- > 0;) ok = applyRegionJob (*e2, st->jobs[i], st->suffix) && ok;
        if (! ok) { e2->regions = st->old; deleteFiles(); return false; }
        st->applied = true; app.project.changed();
        return true;
    };
    c.unapply = [&app, eid, st]
    {
        if (! st->applied) return;
        if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
        st->applied = false;
    };
    c.discard = deleteFiles;
    c.accept = [&app, eid, st, declick] { for (auto& rj : st->jobs) rj.results.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, (declick ? "de-click" : "spectral repair")); app.project.changed(); };
    const double areaStart = (double) from / rate;
    c.play = [&app, eid, areaStart, nm] (double x, double y, bool loop)
    {
        const auto err = app.playEdit (eid, areaStart + x, areaStart + y, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, areaStart]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - areaStart);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (std::make_unique<RepairDialog> (std::move (c), declick), nm, parent, true);
}

// ---- the pitch curve (draw the pitch against time, audition it, accept or revert)
struct EditCurveState { std::vector<RegionJob> jobs; std::vector<EditRegion> old; bool applied = false; };

void pitchCurveEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch curve", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. The pitch line is drawn between the two marks."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch curve", "There is no audio between the marks."); return; }
    const double fixIn = e->fixIn, fixOut = e->fixOut;
    CurveContext c; c.allowPad = true; c.rate = rate; c.length = tOut - tIn;
    c.what = "Draw how the pitch should change along the audio between marks 1 and 2 (" + juce::String (fixOut - fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)), on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Audition to hear it, then Accept or Revert. "
             "The original files are never touched.";
    auto st = std::make_shared<EditCurveState>();
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.load = [regionsCopy, tIn, tOut, only] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (tOut - tIn), 0.0f);
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut || r.files.empty()) continue;
            if (j.cancelled()) break;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                auto rd = audioops::openReader (formats(), f.file); if (rd == nullptr) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*rd, a - f.fileStart, b - a, ch);
                for (auto& x : ch) for (size_t n = 0; n < x.size() && (size_t) (from - tIn) + n < mix.size(); ++n) mix[(size_t) (from - tIn) + n] += x[n];
            }
        }
        std::vector<std::vector<float>> out; out.push_back (std::move (mix)); return out;
    };
    c.prepare = [fixFolder, regionsCopy, tIn, tOut, rate, st, only] (const FixSpec& spec, double padSec, AudioJob& j) -> juce::String
    {
        st->jobs.clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            FixSpec sj = spec; sj.curveZero = (double) pad - (double) (from - tIn);          // curve time 0 is mark 1, wherever this piece starts
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, a - pad, b + pad, a - pad, b + pad, sj, only);
            rj.curveZero = sj.curveZero;
            st->jobs.push_back (std::move (rj));
        }
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            FixSpec sj = spec; sj.curveZero = st->jobs[i].curveZero;
            const auto err = runRegionJob (st->jobs[i], sj, j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { for (auto& done : st->jobs) for (auto& r : done.results) r.file.deleteFile(); st->jobs.clear(); return err; }
        }
        return {};
    };
    auto deleteFiles = [st] { for (auto& j : st->jobs) for (auto& r : j.results) r.file.deleteFile(); st->jobs.clear(); };
    c.apply = [&app, eid, st, deleteFiles]
    {
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) { deleteFiles(); return false; }
        st->old = e2->regions; bool allOk = true;
        for (size_t i = st->jobs.size(); i-- > 0;) allOk = applyRegionJob (*e2, st->jobs[i], "pitch curve") && allOk;
        if (! allOk) { e2->regions = st->old; deleteFiles(); return false; }
        st->applied = true; app.project.changed();
        return true;
    };
    c.revert = [&app, eid, st, deleteFiles]
    {
        if (st->applied) { if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); } st->applied = false; }
        deleteFiles();
    };
    c.unapply = [&app, eid, st]
    {
        if (! st->applied) return;
        if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
        st->applied = false;                                  // (the corrected files are kept, ready to be put back)
    };
    c.accept = [&app, eid, st] { st->jobs.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, "pitch curve"); app.project.changed(); };
    c.play = [&app, eid, fixIn, fixOut] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playEdit (eid, fixIn + fromSec, juce::jmin (fixOut, fixIn + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, fixIn]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - fixIn);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (std::make_unique<CurveDialog> (std::move (c)), "Pitch curve", parent, true);
}

void undoLastFix (AppContext& app)
{
    if (! app.fixUndo) { say ("Undo fix", "There is no pitch correction or repair to undo."); return; }
    auto f = std::move (app.fixUndo); app.fixUndo = nullptr;
    f();
}

// ======================================================================================================== copy the whole project / save as
/** Shows the progress of copying a project (every file is copied, then read back and compared). */
class CopyProjectDialog : public JobDialogBase
{
public:
    CopyProjectDialog (AppContext& a, juce::File dest, juce::String name, bool audio, bool switchWhenDone)
        : app (a), destFolder (std::move (dest)), newName (std::move (name)), copyAudio (audio), saveAs (switchWhenDone)
    {
        setSize (560, 190);
        title.setText (juce::String (saveAs ? "Save as: " : "Copy of the whole project: ") + destFolder.getFullPathName(), juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, theme::text); title.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (title);
        addAndMakeVisible (cancel); addChildComponent (reveal);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        reveal.onClick = [this] { report.projectFile.getParentDirectory().revealToUser(); };
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText ("Starting...", juce::dontSendNotification);
        app.saveNow();
        auto root = app.project.toVar();                       // a snapshot: what is copied is the project as it is now
        const auto src = app.project.projectFolder();
        juce::Component::SafePointer<CopyProjectDialog> self (this);
        auto text = std::make_shared<std::pair<juce::CriticalSection, juce::String>>();
        shared = text;
        auto rep = std::make_shared<projectcopy::Report>();
        runJob ([root, src, dest = destFolder, nm = newName, audio = copyAudio, text, rep] (AudioJob& j) -> juce::String
                {
                    const auto err = projectcopy::run (root, src, dest, nm, audio, [&] (float p, const juce::String& t)
                                                       { j.setProgress (p); { const juce::ScopedLock sl (text->first); text->second = t; } return ! j.cancelled(); }, *rep);
                    return err;
                },
                [self, rep] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->report = *rep;
                    self->finished (err);
                });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        title.setBounds (r.removeFromTop (44));
        status.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
        bar.setBounds (r.removeFromTop (22)); r.removeFromTop (12);
        auto row = r.removeFromBottom (30);
        cancel.setBounds (row.removeFromRight (110)); row.removeFromRight (8); reveal.setBounds (row.removeFromRight (150));
    }
private:
    void timerCallback() override
    {
        JobDialogBase::timerCallback();
        if (auto s = shared.lock()) { const juce::ScopedLock sl (s->first); if (running) status.setText (s->second, juce::dontSendNotification); }
    }
    void finished (const juce::String& err)
    {
        cancel.setButtonText ("Close");
        if (err.isNotEmpty())
        {
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. The original project was not touched; the unfinished copy can be deleted.") : err, juce::dontSendNotification);
            status.setBounds (status.getBounds().withHeight (60));
            return;
        }
        juce::String t = "Done: " + juce::String (report.files) + " files, " + juce::File::descriptionOfSizeInBytes (report.bytes) + ", each read back and checked.";
        if (report.missing > 0) t << "  " << juce::String (report.missing) << " file(s) the project refers to could not be found and were not copied.";
        status.setText (t, juce::dontSendNotification);
        status.setBounds (status.getBounds().withHeight (60));
        reveal.setVisible (true);
        if (saveAs) { const auto f = report.projectFile; juce::MessageManager::callAsync ([this, f] { app.openProject (f); }); }
    }
    AppContext& app; juce::File destFolder; juce::String newName; bool copyAudio, saveAs;
    projectcopy::Report report;
    std::weak_ptr<std::pair<juce::CriticalSection, juce::String>> shared;
    std::shared_ptr<std::pair<juce::CriticalSection, juce::String>> keep;
    juce::Label title;
    juce::TextButton cancel { "Cancel" }, reveal { "Show the folder" };
};

static std::unique_ptr<juce::FileChooser> copyChooser;

void copyProject (AppContext& app, juce::Component* parent, bool saveAs)
{
    if (app.engine.isRecording()) { say ("Recording", "Stop recording first."); return; }
    auto start = [&]
    {
        juce::File last (app.props.getUserSettings()->getValue ("copyFolder"));
        return last.isDirectory() ? last : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    };
    auto choose = [&app, parent, saveAs, start] (bool copyAudio)
    {
        copyChooser = std::make_unique<juce::FileChooser> (saveAs ? "Save as: choose where, and name the project (a folder with this name is created)"
                                                                  : "Copy the whole project: choose where (an external drive, say), and name the copy (a folder with this name is created)",
                                                           start().getChildFile (app.project.name + (saveAs ? " 2" : " copy") + ".fermata"), "*.fermata");
        copyChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [&app, parent, saveAs, copyAudio] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            const auto name = f.getFileNameWithoutExtension();
            auto folder = f.getParentDirectory().getChildFile (name);
            app.props.getUserSettings()->setValue ("copyFolder", f.getParentDirectory().getFullPathName()); app.props.saveIfNeeded();
            launchDialog (std::make_unique<CopyProjectDialog> (app, folder, name, copyAudio, saveAs), saveAs ? "Save project as" : "Copy the whole project", parent);
        });
    };
    if (! saveAs) { choose (true); return; }
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Save as")
                                      .withMessage ("Save this project under a new name and in a new folder.\n\n"
                                                    "Copy everything: all the recordings and bounces are copied too, so the new folder is complete on its own.\n"
                                                    "Project file only: just the project (the edits, mixes and so on) is saved; the audio stays where it is.")
                                      .withButton ("Copy everything").withButton ("Project file only").withButton ("Cancel"),
                                  [choose] (int r) { if (r == 1) choose (true); else if (r == 2) choose (false); });
}

// ======================================================================================================== Export for Processing
/** A small window with a progress bar for a background job; it closes itself when the job has finished fine (after 'onSuccess'), and shows the problem otherwise. */
class SimpleJobDialog : public JobDialogBase
{
public:
    SimpleJobDialog (const juce::String& text, AudioJob::Work work, std::function<void()> onSuccess, std::function<void()> onFail)
    {
        setSize (480, 120);
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText (text, juce::dontSendNotification);
        addAndMakeVisible (cancel);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        runJob (std::move (work), [this, onSuccess, onFail] (const juce::String& err)
        {
            if (err.isEmpty())
            {
                if (onSuccess) onSuccess();
                juce::Component::SafePointer<SimpleJobDialog> sp (this);
                juce::MessageManager::callAsync ([sp] { if (sp != nullptr) closeDialogOf (sp.getComponent()); });
                return;
            }
            if (onFail) onFail();
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. Nothing was changed.") : err, juce::dontSendNotification);
            cancel.setButtonText ("Close");
        });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        status.setBounds (r.removeFromTop (24)); r.removeFromTop (6);
        bar.setBounds (r.removeFromTop (20)); r.removeFromTop (10);
        cancel.setBounds (r.removeFromBottom (28).removeFromRight (110));
    }
private:
    juce::TextButton cancel { "Cancel" };
};

/** Asks for the name of a processing job; 'done' gets the (non-empty) name. */
static void askProcessingName (const juce::String& message, const juce::String& suggestion, std::function<void (const juce::String&)> done)
{
    auto* aw = new juce::AlertWindow ("Export for Processing", message, juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("n", suggestion);
    aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, done] (int res)
    {
        const auto n = aw->getTextEditorContents ("n").trim();
        delete aw;
        if (res == 1 && n.isNotEmpty() && done) done (n);
    }));
}

/** A new folder for the job inside Processing Media, named after the job. */
static juce::File processingJobFolder (AppContext& app, const juce::String& name)
{
    const auto base = app.project.processingFolder();
    base.createDirectory();
    auto f = base.getChildFile (sanitiseForFile (name));
    for (int i = 2; f.exists() && i < 1000; ++i) f = base.getChildFile (sanitiseForFile (name) + " (" + juce::String (i) + ")");
    return f;
}

struct ExportJob { std::vector<juce::File> src, dest; std::vector<juce::int64> fileStart; juce::int64 a = 0, b = 0; juce::File folder; };

static AudioJob::Work exportWork (std::shared_ptr<ExportJob> j)
{
    return [j] (AudioJob& job) -> juce::String
    {
        j->folder.createDirectory();
        for (size_t i = 0; i < j->src.size(); ++i)
        {
            if (job.cancelled()) { j->folder.deleteRecursively(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, j->src.size()));
            juce::String err;
            if (! audioops::exportRange (formats(), j->src[i], j->fileStart[i], j->a, j->b, j->dest[i], err)) { j->folder.deleteRecursively(); return err; }
        }
        return {};
    };
}

static std::shared_ptr<ExportJob> makeExportJob (AppContext& app, const juce::String& name, const std::vector<juce::File>& src, const std::vector<juce::String>& trackNames,
                                                 const std::vector<juce::int64>& fileStart, juce::int64 a, juce::int64 b)
{
    auto j = std::make_shared<ExportJob>();
    j->a = a; j->b = b; j->src = src; j->fileStart = fileStart;
    j->folder = processingJobFolder (app, name);
    for (size_t i = 0; i < src.size(); ++i)
    {
        auto d = audioops::uniqueFile (j->folder.getChildFile (sanitiseForFile (name + " - " + trackNames[i]) + ".wav"));
        for (auto& o : j->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        j->dest.push_back (d);
    }
    return j;
}

static juce::String exportedMessage (const juce::String& name, const juce::File& folder)
{
    return "'" + name + "' is in the folder\n" + folder.getFullPathName() + "\n\nOpen the files in your other software (e.g. iZotope RX), correct them and save OVER the same files, "
           "keeping the same length, sample rate and channels. Then right-click the 'Waiting for corrected audio' block and choose 'Re-link corrected audio'. "
           "Fermata keeps none of these files open, so the other program can overwrite them freely.";
}

void exportForProcessingEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    const juce::String title = "Export for Processing";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (title, "First mark the part to send out with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    const EditRegion* region = nullptr;
    for (auto& r : e->regions) if (tIn >= r.startSample && tIn < r.endSample()) region = &r;
    if (region == nullptr) { say (title, "Mark 1 is not on any piece of audio."); return; }
    if (region->waiting.active()) { say (title, "This piece is already waiting for corrected audio. Re-link it first (right-click the 'Waiting for corrected audio' block)."); return; }
    const auto from = tIn, to = juce::jmin (tOut, region->endSample());
    if (to - from < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short (it must be inside one piece, at least about 0.01 s)."); return; }
    const auto a = region->srcIn + (from - region->startSample), b = a + (to - from);
    const auto rid = region->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart; std::vector<juce::Uuid> trk;
    for (auto& f : region->files) { if (! e->fixUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (f.fileStart); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Alt + drag are in this piece."); return; }
    const bool subset = ! e->fixTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, eid, rid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, eid, rid, a, b, j, name, trk, subset]
        {
            auto* e2 = app.project.findEdit (eid); int i = e2 ? e2->indexOf (rid) : -1;
            bool ok = i >= 0 && ! e2->regions[(size_t) i].waiting.active() && a >= e2->regions[(size_t) i].srcIn && b <= e2->regions[(size_t) i].srcOut;
            if (! ok) { j->folder.deleteRecursively(); say ("Export for Processing", "The edit changed while the audio was being exported, so nothing was changed. Please try again."); return; }
            const auto old = e2->regions;
            const bool cutAfter = b < e2->regions[(size_t) i].srcOut, cutBefore = a > e2->regions[(size_t) i].srcIn;
            if (cutAfter)  e2->splitRegion (i, b - e2->regions[(size_t) i].srcIn);
            if (cutBefore) i = e2->splitRegion (i, a - e2->regions[(size_t) i].srcIn);
            auto& mid = e2->regions[(size_t) i];
            mid.waiting.name = name; mid.waiting.from = a; mid.waiting.to = b; mid.waiting.files = j->dest;
            mid.waiting.tracks = subset ? trk : std::vector<juce::Uuid>();
            mid.curve = FadeCurve::Linear;
            if (cutBefore && i > 0) e2->regions[(size_t) i - 1].curve = FadeCurve::Linear;
            if (cutAfter && i + 1 < (int) e2->regions.size()) e2->regions[(size_t) i + 1].curve = FadeCurve::Linear;
            if (cutBefore) { mid.fixIn = true; if (i > 0) e2->regions[(size_t) i - 1].fixOut = true; }                 // not edit points
            if (cutAfter)  { mid.fixOut = true; if (i + 1 < (int) e2->regions.size()) e2->regions[(size_t) i + 1].fixIn = true; }
            setUndoForEdit (app, eid, old, "export for processing");
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

void exportForProcessingTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    const juce::String title = "Export for Processing";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, title); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (title, "Stop recording first."); return; }
    const double rate = g->sampleRate > 0 ? g->sampleRate : 48000.0;
    const auto a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (in * rate)), b = juce::jmin (g->lengthSamples, (juce::int64) std::llround (out * rate));
    if (b - a < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short."); return; }
    for (auto& wp : g->waiting) if (wp.active() && a < wp.to && b > wp.from) { say (title, "Part of this is already waiting for corrected audio ('" + wp.name + "'). Re-link it first (right-click the block)."); return; }
    const auto gid = g->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart;
    std::vector<juce::Uuid> trk;
    for (auto& f : g->files) { if (! w->editUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (0); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Alt + drag are in this take."); return; }
    const bool subset = ! w->editTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, wid, gid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, wid, gid, a, b, j, name, trk, subset]
        {
            auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 == nullptr) { j->folder.deleteRecursively(); return; }
            const auto old = g2->waiting;
            WaitingPiece wp; wp.name = name; wp.from = a; wp.to = b; wp.files = j->dest; if (subset) wp.tracks = trk;
            g2->waiting.push_back (wp);
            app.fixUndoLabel = "export for processing";
            app.fixUndo = [&app, wid, gid, old]
            {
                if (auto* w3 = app.project.findTakeWindow (wid)) if (auto* g3 = w3->findGroup (gid)) { g3->waiting = old; app.project.changed(); }
            };
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

/** "" if the corrected files can be used, else the sentence(s) that say what is wrong. */
static juce::String checkWaitingFiles (const WaitingPiece& wp, const std::vector<int>& channels, double rate)
{
    juce::String problems;
    for (size_t i = 0; i < wp.files.size(); ++i)
    {
        const auto p = audioops::checkReplacement (formats(), wp.files[i], wp.to - wp.from, i < channels.size() ? channels[i] : 1, rate);
        if (p.isNotEmpty()) problems << p << "\n";
    }
    return problems;
}

void relinkEdit (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid, juce::Component*)
{
    const juce::String title = "Re-link corrected audio";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const int i = e->indexOf (rid); if (i < 0) return;
    const auto& r = e->regions[(size_t) i];
    if (! r.waiting.active()) return;
    // which file of the piece each exported file belongs to
    std::vector<size_t> where;
    for (size_t k = 0; k < r.waiting.files.size(); ++k)
    {
        size_t fi = r.files.size();
        if (r.waiting.tracks.empty()) fi = k;
        else for (size_t q = 0; q < r.files.size(); ++q) if (r.files[q].trackId == r.waiting.tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = r.srcIn == r.waiting.from && r.srcOut == r.waiting.to && (! r.waiting.tracks.empty() || r.waiting.files.size() == r.files.size());
    for (auto fi : where) if (fi >= r.files.size()) fits = false;
    if (! fits) { say (title, "This piece was changed after it was sent out, so the corrected files no longer fit it. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (r.files[fi].numChannels);
    const auto problems = checkWaitingFiles (r.waiting, ch, r.sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    const auto old = e->regions;
    auto& m = e->regions[(size_t) i];
    for (size_t k = 0; k < where.size(); ++k) { m.files[where[k]].file = m.waiting.files[k]; m.files[where[k]].fileStart = m.srcIn; }
    m.takeName = m.takeName.upToFirstOccurrenceOf (" (", false, false) + " (" + m.waiting.name + ")";
    m.waiting = WaitingPiece();
    setUndoForEdit (app, eid, old, "re-link corrected audio");
    app.project.changed();
}

void relinkTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, juce::int64 waitingFrom, juce::Component* parent)
{
    const juce::String title = "Re-link corrected audio";
    auto* w = takeWin (app, wid); auto* g = w ? w->findGroup (gid) : nullptr; if (g == nullptr) return;
    const WaitingPiece* wp = nullptr; for (auto& x : g->waiting) if (x.active() && x.from == waitingFrom) wp = &x;
    if (wp == nullptr) return;
    std::vector<size_t> where;                                   // which file of the take each exported file belongs to
    for (size_t k = 0; k < wp->files.size(); ++k)
    {
        size_t fi = g->files.size();
        if (wp->tracks.empty()) fi = k;
        else for (size_t q = 0; q < g->files.size(); ++q) if (g->files[q].trackId == wp->tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = wp->tracks.empty() ? wp->files.size() == g->files.size() : true;
    for (auto fi : where) if (fi >= g->files.size()) fits = false;
    if (! fits) { say (title, "The take has changed since this was sent out. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (g->files[fi].numChannels);
    const auto problems = checkWaitingFiles (*wp, ch, g->sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    struct Splice { std::vector<juce::File> src, repl, dest; std::vector<size_t> where; juce::int64 from, to; juce::String name; };
    auto sp = std::make_shared<Splice>();
    sp->from = wp->from; sp->to = wp->to; sp->name = wp->name; sp->repl = wp->files; sp->where = where;
    for (auto fi : where)
    {
        auto& f = g->files[fi];
        sp->src.push_back (f.file);
        auto d = audioops::uniqueFile (f.file.getSiblingFile (f.file.getFileNameWithoutExtension() + " (" + sanitiseForFile (wp->name) + ").wav"));
        for (auto& o : sp->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        sp->dest.push_back (d);
    }
    auto work = [sp] (AudioJob& job) -> juce::String
    {
        for (size_t i = 0; i < sp->src.size(); ++i)
        {
            if (job.cancelled()) { for (auto& d : sp->dest) d.deleteFile(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, sp->src.size()));
            juce::String err;
            if (! audioops::spliceFile (formats(), sp->src[i], sp->repl[i], sp->from, sp->to, sp->dest[i], 0.005, err)) { for (auto& d : sp->dest) d.deleteFile(); return err; }
        }
        return {};
    };
    auto onSuccess = [&app, wid, gid, sp]
    {
        auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        bool okIdx = g2 != nullptr; if (okIdx) for (auto fi : sp->where) if (fi >= g2->files.size()) okIdx = false;
        if (! okIdx) { for (auto& d : sp->dest) d.deleteFile(); return; }
        const auto old = g2->files;
        for (size_t k = 0; k < sp->where.size(); ++k) g2->files[sp->where[k]].file = sp->dest[k];
        g2->waiting.erase (std::remove_if (g2->waiting.begin(), g2->waiting.end(), [sp] (const WaitingPiece& x) { return x.from == sp->from; }), g2->waiting.end());
        setUndoForTake (app, wid, gid, old, "re-link corrected audio");
        app.project.changed();
    };
    launchDialog (std::make_unique<SimpleJobDialog> ("Putting the corrected audio into the take...", work, onSuccess, nullptr), title, parent);
}
}} // namespace td::fixtools

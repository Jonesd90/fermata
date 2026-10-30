#pragma once
// The pitch, pitch-curve and repair windows of the fix tools (3.18.6: from FixTools.cpp). Only FixToolsEdits.cpp includes this.
#include "FixToolsCommon.h"

namespace td { namespace fixtools
{
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
    std::function<void (std::function<bool()>)> pairWritten;                                // one stream window of an OUT / IN pair: Accept only makes this stream ready (the pair puts both into the edit together)
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
        // the zoom keeps the playhead where it is on the screen: the one running across the picture, or else the one you placed by clicking the ruler;
        // with neither in view it zooms around the middle
        const double at = (playT >= vt0 && playT <= vt1) ? playT : (cueT >= vt0 && cueT <= vt1) ? cueT : 0.5 * (vt0 + vt1);
        const double frac = (vt1 - vt0) > 0.0 ? (at - vt0) / (vt1 - vt0) : 0.5;
        setWindow (at - frac * span, span);
    }
    void zoomCents (double factor) { kc = juce::jlimit (5.0, (double) PitchCurve::kRange, kc * factor); repaint(); }
    /** Left / right = zoom out / in in time (right in), up / down = zoom out / in vertically (down in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); return true; }
        if (k == juce::KeyPress::downKey)  { zoomCents (0.7); return true; }
        if (k == juce::KeyPress::upKey)    { zoomCents (1.0 / 0.7); return true; }
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
    /** The playhead you place by clicking the time ruler: where Play starts, and the point the zoom keys keep still. */
    double playFrom() const { return cueT >= vt0 && cueT < vt1 ? cueT : vt0; }
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
        if (playT < 0.0 && cueT >= vt0 && cueT <= vt1)         // the playhead you placed on the ruler
        {
            g.setColour (theme::playhead); g.drawVerticalLine (xOf (cueT), (float) pl.getY(), (float) pl.getBottom());
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
        if (e.y < plot().getY() - 2 && e.x >= plot().getX()) { panning = true; panT0 = vt0; return; }          // the ruler: click to put the playhead there, drag to move the view
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
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (panning && e.getDistanceFromDragStart() < 4) { cueT = toT (e.x); repaint(); }                  // a click on the ruler (not a drag) places the playhead
        dragging = false; panning = false;
    }
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
    double cueT = -1.0, duration = 1.0, vt0 = 0.0, vt1 = 1.0, kc = PitchCurve::kRange, panT0 = 0.0; std::vector<float> env; int selected = -1; bool dragging = false, panning = false; double playT = -1.0;
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
        playBtn.setTooltip ("Plays the part you can see (left / right arrows zoom in and out in time, down / up zoom in / out in cents; click the ruler to place the playhead, drag it to move; R = Preview) with the sound as it is now: the original until you Preview, then the corrected sound if 'Hear corrected' is lit. With Loop lit it goes round and round. Key: Space");
        hearToggle.setTooltip ("Lit: you hear the CORRECTED sound. Off: you hear the ORIGINAL. Click it while playing to switch at once and hear the difference, without touching the line. (Available after Preview.)");
        hearToggle.onClick = [this] { hearChanged(); };
        loopBtn.setTooltip ("Repeat the marked part over and over until you press Stop");
        loopBtn.onClick = [this] { const bool on = loopBtn.getToggleState(); if (! playing()) return; if (on) { if (ctx.play) ctx.play (view.visT0(), view.visT1(), true); } else if (ctx.setLooping) ctx.setLooping (false); };
        playBtn.onClick = [this] { if (ctx.stop) ctx.stop(); if (ctx.play) ctx.play (view.playFrom(), view.visT1(), loopBtn.getToggleState()); };
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
        if (ctx.pairWritten)
        {
            acceptBtn.setButtonText ("Accept stream"); closeBtn.setButtonText ("Cancel both");
            acceptBtn.setTooltip ("Gets this stream ready and closes this window (with no Preview first, it leaves this stream as it is). When BOTH streams have been accepted the corrected audio goes into the edit, with the edit point and its fades exactly where they are. Cancel (or the X) closes both windows and changes nothing.");
        }
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
        acceptBtn.setEnabled (ready && ((tentative && ! stale) || (ctx.pairWritten != nullptr && ! tentative)));
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
                        if (self->ctx.play) self->ctx.play (self->view.playFrom(), self->view.visT1(), self->loopBtn.getToggleState());
                        self->status.setText ("Playing the corrected sound.  'Hear corrected' switches to the original and back; Accept keeps it, Revert goes back, or change the line and Preview again.", juce::dontSendNotification);
                    }
                    else self->status.setText ("The audio changed while it was being calculated, so nothing was changed. Please try again.", juce::dontSendNotification);
                    self->refresh();
                });
    }
    void accept()
    {
        if (running) return;
        if (ctx.pairWritten && ! tentative)                                  // an untouched stream: it is left as it is
        {
            if (ctx.stop) ctx.stop();
            accepted = true; ctx.pairWritten ([] { return true; });
            return;
        }
        if (! tentative || stale) return;
        if (ctx.stop) ctx.stop();
        if (! corrected)                                                // the original was being heard: put the corrected sound back before keeping it
        {
            if (! (ctx.apply && ctx.apply())) { status.setText ("The audio changed, so the corrected sound is no longer available. Press Preview again.", juce::dontSendNotification); tentative = false; refresh(); return; }
            corrected = true;
        }
        if (ctx.pairWritten)
        {
            accepted = true; ctx.pairWritten ([] { return true; });
            return;
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
            if (spaceIsForRecording()) return true;
            if (loaded && ! running) { if (playing()) { if (ctx.stop) ctx.stop(); } else if (ctx.play) ctx.play (view.playFrom(), view.visT1(), loopBtn.getToggleState()); }
            return true;
        }
        if ((k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R') && ! k.getModifiers().isAnyModifierKeyDown())      // R: audition the line as it is
        {
            if (auditionBtn.isEnabled()) auditionBtn.triggerClick();
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

// ======================================================================================================== the repair dialog (RepairOp and RepairContext are in FixToolsCommon.h)

/** How the spectrogram is drawn (the same ideas as the Spectrogram Settings of iZotope RX). Remembered between sessions. */
struct SpectroSettings
{
    bool   sharp = true;            // the picture is made for what is in view, with reassignment (sharp lines); off = the quick, softer picture
    bool   reassign = true;
    int    palette = 0;             // 0 cyan to orange, 1 standard (heat), 2 grey, 3 ice, 4 rainbow
    double lo = -134.3, hi = -5.3;  // the levels (dB) at the two ends of the colours
    double detail = 0.5;            // 0 = finer pitch detail, 1 = finer time detail
    double reduceAbove = 240.0;     // seconds in view above which fewer analysis points are used
    void load (juce::PropertiesFile* f)
    {
        if (f == nullptr) return;
        sharp = f->getBoolValue ("specSharp", true); reassign = f->getBoolValue ("specReassign", true); palette = juce::jlimit (0, 4, f->getIntValue ("specPalette", 0));
        lo = f->getDoubleValue ("specLo", -134.3); hi = f->getDoubleValue ("specHi", -5.3); detail = juce::jlimit (0.0, 1.0, f->getDoubleValue ("specDetail", 0.5)); reduceAbove = f->getDoubleValue ("specReduce", 240.0);
        if (hi - lo < 6.0) { lo = -134.3; hi = -5.3; }
    }
    void save (juce::PropertiesFile* f) const
    {
        if (f == nullptr) return;
        f->setValue ("specSharp", sharp); f->setValue ("specReassign", reassign); f->setValue ("specPalette", palette); f->setValue ("specLo", lo); f->setValue ("specHi", hi);
        f->setValue ("specDetail", detail); f->setValue ("specReduce", reduceAbove);
    }
};
inline juce::PropertiesFile* gSpecPrefs = nullptr;                      // the program's settings file (set when a repair window is opened)

struct PictureRequest { juce::int64 id = 0; double t0 = 0.0, t1 = 1.0; int w = 0, h = 0; PictureAxis axis; PictureOptions opt; };

class SpectrogramView : public juce::Component
{
public:
    std::function<void()> onSelection;
    std::function<void (const PictureRequest&)> onNeedDetail;     // asks for the sharp picture of what is in view (made on another thread, then given back with setDetail)
    std::function<void (const juce::String&)> onNote;         // a line for the status line (colour roll, scale, view)

    void setData (MergedSpectrogram s, double seconds, bool keepSelection = false)
    {
        const bool same = spec.frames > 0 && std::abs (seconds - duration) < 1.0e-9;
        spec = std::move (s); duration = juce::jmax (0.001, seconds); dT0 = 0.0; dT1 = duration;
        if (! keepSelection) hasSel = false;
        if (! same) resetView();
        detailStale = true; scheduleDetail();
        dirty = true; repaint();
    }
    /** A quick picture that covers only part of the time (Noise mode: a slice of the whole edit's pyramid for what is in view): its first frame
        stands for 'dataT0' seconds and its last for 'dataT1'; 'seconds' is the whole length. The view, the selection and the sharp picture stay. */
    void setDataRange (MergedSpectrogram s, double dataT0, double dataT1, double seconds)
    {
        const bool fresh = spec.frames == 0;
        spec = std::move (s); duration = juce::jmax (0.001, seconds); dT0 = dataT0; dT1 = juce::jmax (dataT0 + 1.0e-6, dataT1);
        if (fresh) { resetView(); scheduleDetail(); }
        dirty = true; repaint();
    }
    std::function<void()> onView;                                  // the part of the time in view changed (zoom, pan, reset)
    /** The sound changed under the picture: the sharp picture is made again. */
    void invalidateDetail() { detailStale = true; dirty = true; repaint(); scheduleDetail(); }
    /** The sharp picture asked for with onNeedDetail (ignored if the view has moved on and asked again since). */
    void setDetail (DetailPicture p)
    {
        if (p.id != detailSeq || ! p.valid()) return;
        detail = std::move (p); detailStale = false; detailOpt = lastOpt; dirty = true; repaint();
    }
    const SpectroSettings& settings() const { return ss; }
    void setSettings (const SpectroSettings& n) { ss = n; buildLut(); detailStale = ss.sharp ? true : detailStale; dirty = true; repaint(); scheduleDetail(); }
    void resized() override { scheduleDetail(); }
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
    /** The playhead you place by clicking the time ruler: where Play starts, and the point the zoom keys keep still. */
    double playFrom() const { return cueT >= vt0 && cueT < vt1 ? cueT : vt0; }
    /** The transport: put the placed playhead at t (seconds; < 0 takes it away), and the width of the page shown. */
    void placeCue (double t) { cueT = t < 0.0 ? -1.0 : juce::jlimit (0.0, duration, t); repaint(); }
    /** The playhead placed by clicking the ruler (seconds from the start of the area), or -1 when none is placed. */
    double cueTime() const { return cueT; }
    double pageSpan() const { return vt1 - vt0; }
    /** Where Play starts: the placed playhead if it is on this page (the very end counts), else the left edge of the page. */
    double startPoint() const { return cueT >= vt0 && cueT <= vt1 ? cueT : vt0; }
    /** Makes sure time t is on the page: if it is not, the page is turned so that t is a tenth of the way in (the zoom stays). */
    void showTime (double t) { if (t >= vt0 && t < vt1) return; setWindow (t - 0.1 * (vt1 - vt0), vt1 - vt0); }
    /** Turns to the page that starts at t0 (clamped so the last page ends with the audio; the zoom stays). */
    void showFrom (double t0) { setWindow (t0, vt1 - vt0); }

    // ---- the view: which part of the time is shown (this is what Play plays), which pitches, and how the pitches are spread up the picture
    double visT0() const { return vt0; }
    double visT1() const { return vt1; }
    void resetView() { vt0 = 0.0; vt1 = duration; fLo = kMinHz; fHi = maxHz(); dirty = true; repaint(); scheduleDetail(); if (onView) onView(); }
    /** factor < 1 zooms in, > 1 zooms out, around the middle of what is shown. */
    void zoomTime (double factor)
    {
        const double span = juce::jlimit (juce::jmin (0.05, duration), duration, (vt1 - vt0) * factor);
        // the zoom keeps the playhead where it is on the screen: the one running across the picture, or else the one you placed by clicking the ruler;
        // with neither in view it zooms around the middle
        const double at = (playT >= vt0 && playT <= vt1) ? playT : (cueT >= vt0 && cueT <= vt1) ? cueT : 0.5 * (vt0 + vt1);
        const double frac = (vt1 - vt0) > 0.0 ? (at - vt0) / (vt1 - vt0) : 0.5;
        setWindow (at - frac * span, span);
    }
    void zoomFreq (double factor)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        const double span = juce::jlimit (std::log (1.6), hi - lo, std::log (fHi / fLo) * factor);
        setFreqWindow (0.5 * (std::log (fLo) + std::log (fHi)) - 0.5 * span, span);
    }
    /** Dragging the pitch scale at the left moves what is shown up and down (the picture follows the mouse) and never changes how much is shown:
        the shift is made in the units the scale is drawn in (Mel, straight Hz, or octaves), from the window as it was when the drag began. 'down' is the drag
        distance as a fraction of the picture's height (down = positive = higher pitches come into view). */
    void dragPitchWindow (double down)
    {
        auto toU = [this] (double f) { return mel ? PictureAxis::melOf (f) : warp < 0.5 ? f : std::log (f); };
        auto fromU = [this] (double u) { return mel ? PictureAxis::hzOfMel (u) : warp < 0.5 ? u : std::exp (u); };
        const double u0 = toU (panF), u1 = toU (panF1), span = u1 - u0;
        const double minU = toU (kMinHz), maxU = toU (maxHz());
        double lo = u0 + down * span;
        lo = juce::jlimit (minU, juce::jmax (minU, maxU - span), lo);
        fLo = juce::jmax ((double) kMinHz, fromU (lo)); fHi = juce::jmin ((double) maxHz(), fromU (lo + span));
        if (fHi <= fLo * 1.01) fHi = fLo * 1.01;
        dirty = true; repaint(); scheduleDetail();
    }
    /** 0 = pitch spread evenly (linear), 1 = every octave the same height (musical); in between is a blend. */
    void setScale (double w) { warp = juce::jlimit (0.0, 1.0, w); mel = false; dirty = true; repaint(); scheduleDetail(); }
    /** The Mel scale (the pitch scale of iZotope RX). */
    void setMel() { mel = true; dirty = true; repaint(); scheduleDetail(); }
    /** Left / right = zoom out / in in time (right in), up / down = zoom out / in vertically (down in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); note(); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); note(); return true; }
        if (k == juce::KeyPress::downKey)  { zoomFreq (0.7); return true; }
        if (k == juce::KeyPress::upKey)    { zoomFreq (1.0 / 0.7); return true; }
        return false;
    }
    /** The round colour control (bottom left): each step is another colour map and another range of levels, so quiet noises show up. */
    void rollColour (double amount) { setRoll (rollPos + amount); }
    double rollValue() const { return rollPos; }
    void setRoll (double p)
    {
        rollPos = std::fmod (std::fmod (p, (double) kPresets) + (double) kPresets, (double) kPresets);
        preset = (int) std::floor (rollPos + 0.5) % kPresets;
        buildLut(); dirty = true; repaint();
        if (onNote) onNote ("Colour: " + juce::String (presetAt (preset).name) + ".  Roll the round control smoothly (drag up / down, or the mouse wheel) to blend between the looks and show quieter sounds; click to jump to the next one; double-click for the standard colours.");
    }

    SpectrogramView() { ss.load (gSpecPrefs); debounce.owner = this; buildLut(); setMouseCursor (juce::MouseCursor::CrosshairCursor); }

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
            juce::Graphics::ScopedSaveState saved (g);
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
            if (playT < 0.0 && cueT >= vt0 && cueT <= vt1 && duration > 0.0)          // the playhead you placed on the ruler
            {
                const int x = tToX (cueT, plot);
                g.setColour (theme::playhead); g.fillRect (x - 1, plot.getY(), 2, plot.getHeight());
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
        if (e.x < pl.getX() && e.y >= pl.getY() && e.y < pl.getBottom()) { mode = Mode::pitchPan; panF = fLo; panF1 = fHi; return; }
        if (const int ed = edgesAt (e.getPosition()))                    // on an edge or corner of the box: drag it to resize the box
        {
            a = { selT0(), selF0() }; b = { selT1(), selF1() };        // a = lower-left corner, b = upper-right
            const auto r = selRect();                                  // where the pointer is on the edge, so the edge does not jump to the pointer
            grabDx = (ed & 3) ? e.x - ((ed & 1) ? r.getX() : r.getRight()) : 0;
            grabDy = (ed & 12) ? e.y - ((ed & 4) ? r.getY() : r.getBottom()) : 0;
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
            // Each side belongs to one axis: a left / right edge changes the width and nothing else, a top / bottom edge changes the height and nothing else;
            // only a corner changes both. The edge being dragged follows the pointer (where it was grabbed), the opposite edge stays where it is, and the
            // box is kept at least a few pixels across so it cannot collapse to a line while it is being resized.
            const auto pl2 = plotArea();
            const auto q = toTF ({ e.x - grabDx, e.y - grabDy });
            if (edgeMask & 3)                                              // width: a = left, b = right (they may swap places if the edge is dragged across)
            {
                const double minT = juce::jmax (0.002, 3.0 * (vt1 - vt0) / (double) juce::jmax (1, pl2.getWidth()));
                double& mv = (edgeMask & 1) ? a.x : b.x;
                const double fixedT = (edgeMask & 1) ? b.x : a.x;
                double v = q.x;
                if (std::abs (v - fixedT) < minT)
                {
                    const double side = v >= fixedT ? 1.0 : -1.0;
                    v = fixedT + side * minT;
                    if (v < 0.0 || v > duration) v = fixedT - side * minT;
                }
                if (v >= 0.0 && v <= duration) mv = v;
            }
            if (edgeMask & 12)                                             // height: b.y = top (highest pitch), a.y = bottom
            {
                double& mv = (edgeMask & 4) ? b.y : a.y;
                const double fixedF = (edgeMask & 4) ? a.y : b.y;
                const int yFixed = hzToY (fixedF, pl2);
                const int yTop = pl2.getY(), yBottom = pl2.getBottom() - 1;
                int yy = juce::jlimit (yTop, yBottom, e.y - grabDy);
                if (std::abs (yy - yFixed) < 3)
                {
                    const int side = yy >= yFixed ? 1 : -1;
                    yy = yFixed + side * 3;
                    if (yy < yTop || yy > yBottom) yy = yFixed - side * 3;
                }
                if (yy >= yTop && yy <= yBottom) mv = toTF ({ pl2.getX(), yy }).y;
            }
            repaint();
        }
        else if (mode == Mode::pan) setWindow (panT0 - (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0), vt1 - vt0);
        else if (mode == Mode::pitchPan) dragPitchWindow ((double) e.getDistanceFromDragStartY() / (double) juce::jmax (1, pl.getHeight()));
        else if (mode == Mode::knob)
        {
            setRoll (rollStart - (double) e.getDistanceFromDragStartY() / 45.0);       // smooth: about 45 px per colour look
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto m = mode; mode = Mode::none;
        if (m == Mode::knob && e.getDistanceFromDragStart() < 4) { setRoll (std::floor (rollPos + 0.5) + (e.mods.isRightButtonDown() || e.mods.isShiftDown() ? -1 : 1)); return; }
        if (m == Mode::pan && e.getDistanceFromDragStart() < 4)                  // a click (not a drag) on the time ruler puts the playhead there
        {
            const auto pl = plotArea();
            cueT = juce::jlimit (0.0, duration, vt0 + (double) (e.x - pl.getX()) / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0));
            repaint(); return;
        }
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
    int grabDx = 0, grabDy = 0;                                 // while resizing: how far the pointer was from the edge it took hold of (pixels)
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
        static const Stop cyanOrange[] = { { 0.0f, 0.0f, 0.0f, 0.04f }, { 0.18f, 0.02f, 0.10f, 0.32f }, { 0.36f, 0.0f, 0.38f, 0.62f }, { 0.52f, 0.0f, 0.80f, 0.85f }, { 0.64f, 0.55f, 0.92f, 0.70f }, { 0.78f, 1.0f, 0.82f, 0.25f }, { 0.90f, 1.0f, 0.52f, 0.05f }, { 1.0f, 1.0f, 0.95f, 0.85f } };
        static const Stop rain[] = { { 0.0f, 0, 0, 0.15f }, { 0.2f, 0.15f, 0, 0.7f }, { 0.4f, 0, 0.6f, 1.0f }, { 0.6f, 0.1f, 0.9f, 0.3f }, { 0.8f, 1.0f, 0.9f, 0.0f }, { 1.0f, 1.0f, 0.15f, 0.1f } };
        return pal == 0 ? ramp (heat, 5, t) : pal == 1 ? ramp (grey, 2, t) : pal == 2 ? ramp (ice, 4, t) : pal == 4 ? ramp (cyanOrange, 8, t) : ramp (rain, 6, t);
    }
    void buildLut()
    {
        const Preset *pa, *pb; float f; blended (pa, pb, f);
        for (int i = 0; i < 256; ++i)
        {
            const float u = (float) i / 255.0f;
            const auto ca = paletteAt (pa->palette, std::pow (u, pa->gamma)), cb = paletteAt (pb->palette, std::pow (u, pb->gamma));
            lut[(size_t) i] = ca.interpolatedWith (cb, f);
            static const int paletteIds[] = { 4, 0, 1, 2, 3 };                          // the sharp picture uses the palette chosen in the spectrogram settings, with the roll's gamma
            lutD[(size_t) i] = paletteAt (paletteIds[juce::jlimit (0, 4, ss.palette)], std::pow (u, pa->gamma + f * (pb->gamma - pa->gamma)));
        }
    }
    /** The two ends of the colours (dB) for the sharp picture: the settings, shifted by the colour roll's look. */
    void detailRange (float& lo, float& hi) const
    {
        const Preset *pa, *pb; float f; const Preset P = blended (pa, pb, f);
        lo = (float) ss.lo + (P.lo + 100.0f); hi = (float) ss.hi + (P.hi + 15.0f);
        if (hi - lo < 6.0f) hi = lo + 6.0f;
    }
    PictureAxis axisNow() const { PictureAxis ax; ax.fLo = fLo; ax.fHi = fHi; ax.warp = warp; ax.mel = mel; return ax; }
    void scheduleDetail() { if (ss.sharp && spec.frames > 0) debounce.startTimer (260); }
    void requestDetailNow()
    {
        if (! ss.sharp || spec.frames <= 0 || ! onNeedDetail) return;
        const auto pl = plotArea();
        if (pl.getWidth() < 50 || pl.getHeight() < 50) return;
        PictureRequest rq; rq.t0 = vt0; rq.t1 = vt1; rq.w = pl.getWidth(); rq.h = pl.getHeight(); rq.axis = axisNow();
        rq.opt.reassign = ss.reassign; rq.opt.detail = ss.detail; rq.opt.reduceAboveSec = ss.reduceAbove;
        const bool optSame = detailOpt.reassign == rq.opt.reassign && std::abs (detailOpt.detail - rq.opt.detail) < 1.0e-9 && std::abs (detailOpt.reduceAboveSec - rq.opt.reduceAboveSec) < 1.0e-9;
        if (detail.valid() && ! detailStale && optSame && detail.w == rq.w && detail.h == rq.h && std::abs (detail.t0 - rq.t0) < 1.0e-9 && std::abs (detail.t1 - rq.t1) < 1.0e-9 && detail.axis.same (rq.axis)) return;
        rq.id = ++detailSeq; lastOpt = rq.opt;
        onNeedDetail (rq);
    }
    struct Debounce : juce::Timer { SpectrogramView* owner = nullptr; void timerCallback() override { stopTimer(); if (owner != nullptr) owner->requestDetailNow(); } } debounce;
    double maxHz() const { return juce::jmin (22000.0, spec.sampleRate * 0.5); }
    juce::Rectangle<int> plotArea() const { return getLocalBounds().withTrimmedLeft (46).withTrimmedTop (22).withTrimmedBottom (30); }
    juce::Rectangle<int> rulerArea() const { auto pl = plotArea(); return { pl.getX(), 0, pl.getWidth(), pl.getY() }; }
    juce::Rectangle<int> knobArea() const { return { 8, getHeight() - 28, 24, 24 }; }
    void note() { if (onNote) onNote ("Showing " + juce::String (vt0, 2) + " - " + juce::String (vt1, 2) + " s.  Right / left arrows zoom in / out in time, down / up in pitch; click the ruler to place the playhead, drag it to move; double-click the ruler to see it all."); }
    void setWindow (double t0, double span)
    {
        span = juce::jlimit (juce::jmin (0.05, duration), duration, span);
        t0 = juce::jlimit (0.0, juce::jmax (0.0, duration - span), t0);
        vt0 = t0; vt1 = t0 + span; dirty = true; repaint(); scheduleDetail(); if (onView) onView();
    }
    void setFreqWindow (double logLo, double logSpan)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        logSpan = juce::jlimit (std::log (1.6), hi - lo, logSpan);
        logLo = juce::jlimit (lo, hi - logSpan, logLo);
        fLo = std::exp (logLo); fHi = std::exp (logLo + logSpan); dirty = true; repaint(); scheduleDetail();
    }
    // pitch <-> height: the Mel scale, or a blend of the straight line (even Hz) and the logarithm (even octaves)
    double posOf (double hz) const { return axisNow().posOf (hz); }
    double hzOf (double pos) const { return axisNow().hzOf (pos); }
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
        const auto r = selRect();                                             // (the rules - sides belong to one axis, only a real corner takes both - are in core/BoxEdges.h, with a test)
        return boxedit::edgesAt ({ r.getX(), r.getY(), r.getWidth(), r.getHeight() }, p.x, p.y);
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
            const double fp = juce::jlimit (0.0, (double) spec.frames - 1.0001, (t - dT0) / (dT1 - dT0) * (double) (spec.frames - 1));
            const int f0 = (int) fp; const float ff = (float) (fp - f0); const int f1 = juce::jmin (spec.frames - 1, f0 + 1);
            for (int y = 0; y < H; ++y)
            {
                const float d0 = spec.level (f0, (double) rf[(size_t) y]), d1 = spec.level (f1, (double) rf[(size_t) y]);
                const float d = d0 * (1.0f - ff) + d1 * ff;
                const int idx = juce::jlimit (0, 255, (int) ((d - lo) * inv));
                bd.setPixelColour (x, y, lut[(size_t) idx]);
            }
        }
        // the sharp picture, where it covers the view (it replaces the quick one; the quick one shows while the sharp one is being made)
        if (ss.sharp && detail.valid() && ! detailStale && std::abs (detail.t0 - vt0) < 1.0e-6 && std::abs (detail.t1 - vt1) < 1.0e-6 && detail.axis.same (axisNow()))
        {
            float dlo, dhi; detailRange (dlo, dhi); const float dinv = 255.0f / (dhi - dlo);
            const double dSpan = juce::jmax (1.0e-9, detail.t1 - detail.t0);
            std::vector<int> drow ((size_t) H, -1);
            for (int y = 0; y < H; ++y)
            {
                const double hz = rf[(size_t) y];
                if (hz >= detail.axis.fLo && hz <= detail.axis.fHi) drow[(size_t) y] = juce::jlimit (0, detail.h - 1, (int) std::lround ((1.0 - detail.axis.posOf (hz)) * (double) (detail.h - 1)));
            }
            for (int x = 0; x < W; ++x)
            {
                const double t = vt0 + ((double) x + 0.5) / (double) W * (vt1 - vt0);
                const int col = (int) std::floor ((t - detail.t0) / dSpan * (double) detail.w);
                if (col < 0 || col >= detail.w) continue;
                for (int y = 0; y < H; ++y)
                {
                    const int row = drow[(size_t) y];
                    if (row < 0) continue;
                    const float db = detail.db[(size_t) row * (size_t) detail.w + (size_t) col];
                    bd.setPixelColour (x, y, lutD[(size_t) juce::jlimit (0, 255, (int) ((db - dlo) * dinv))]);
                }
            }
        }
    }
    MergedSpectrogram spec; double duration = 1.0, dT0 = 0.0, dT1 = 1.0; juce::Image image; bool dirty = true;
    double vt0 = 0.0, vt1 = 1.0, fLo = kMinHz, fHi = 20000.0, warp = 1.0;
    int preset = 0; double rollPos = 0.0, rollStart = 0.0; Mode mode = Mode::none; double panT0 = 0.0, panF = kMinHz, panF1 = 20000.0;
    std::array<juce::Colour, 256> lut, lutD;
    SpectroSettings ss; bool mel = true; DetailPicture detail; bool detailStale = true; juce::int64 detailSeq = 0; PictureOptions detailOpt, lastOpt;
    bool hasSel = false; juce::Point<double> a, b; double playT = -1.0, cueT = -1.0;
    bool showSurround = false; double surroundPct = 100.0; int surroundDir = 0; double surroundAfter = 0.5;
};

class RepairDialog : public JobDialogBase
{
public:
    explicit RepairDialog (RepairContext c) : ctx (std::move (c))
    {
        setSize (1100, 760);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::text);
        dirBox.addItem ("Direction: Horizontal", 1); dirBox.addItem ("Direction: Vertical", 2); dirBox.addItem ("Direction: 2D (both)", 3);
        dirBox.setSelectedId (1, juce::dontSendNotification); dirBox.onChange = [this] { refreshControls(); invalidate(); };
        dirBox.setTooltip ("Where the repair finds its material. Left and right suits steady sounds (hum, a held note). Up and down suits short bursts (a click, a cough) with music above and below. Both blends the two.");
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (sensCap, "Click sensitivity"); cap (strengthCap, "Strength"); cap (surroundCap, "Surrounding length"); cap (weightCap, "Before <-> After");
        cap (amountCap, "Amount"); cap (residualCap, "Residual"); cap (roomCap, "");
        auto slider = [this] (juce::Slider& sl, double lo, double hi, double step, double v, const juce::String& suffix)
        {
            sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (lo, hi, step); sl.setValue (v, juce::dontSendNotification); sl.setScrollWheelEnabled (false);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 22); sl.setTextValueSuffix (suffix);
            sl.onValueChange = [this] { updateSurround(); invalidate(); };
            addAndMakeVisible (sl);
        };
        slider (sens, 1, 10, 1, 6, ""); slider (strength, 0, 100, 1, 80, " %"); slider (surround, 10, 400, 5, 100, " %"); slider (weight, 0, 100, 1, 50, " %");
        slider (amount, -6.0, 12.0, 0.5, 0.0, " dB"); slider (residual, -30.0, -3.0, 1.0, -12.0, " dB");
        amount.setDoubleClickReturnValue (true, 0.0); residual.setDoubleClickReturnValue (true, -12.0);
        amount.setTooltip ("Denoise: how much louder than measured the room tone is taken to be. 0 = as learned; +3 dB takes a little more out, -3 dB a little less. Double-click for 0.");
        residual.setTooltip ("Denoise: how much of the room tone is left in the box (the gain never goes below this). -12 dB leaves a quiet, natural floor; -24 dB is a lot. Double-click for -12.");
        roomBtn.onClick = [this] { roomFromBox(); };
        denoiseBtn.onClick = [this] { preview (2); };
        roomBtn.setTooltip ("Draw a box over a part of THIS picture that holds nothing but the room (its pitch range does not matter: the whole spectrum is learned) and press this: that box is the room tone for Denoise here, instead of the project's. Key: L");
        denoiseBtn.setTooltip ("Denoise: takes the background noise out of the box you drew - its time span and its pitch range, on every track - with the room tone (the project's, from the edit window's Room tone button, or a box of this picture marked with L). "
                               "Above and below the box's pitch range nothing is touched; the denoising fades in and out at the box's edges. Same rules as ReTouch: nothing is written yet, Play to listen, Undo if it did not work. Key: N");
        denoiseBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        addAndMakeVisible (roomBtn); addAndMakeVisible (denoiseBtn);
        strength.setTooltip ("How much of the repair replaces the original sound inside the box. 100 % is a full repair; less keeps some of the original.");
        surround.setTooltip ("How much of the sound around the box is used, as a percentage of the box: its length in time (left and right) and its height in pitch (up and down).");
        weight.setTooltip ("0 uses only the sound before the box, 100 only the sound after it, 50 uses both equally (a cross-fade). Applies to left and right.\n"
                           "Attenuate a stray consonant: leave it at 50 and Fermata finds, for every track, the side away from the main consonant; move it to choose a side yourself.");
        methodBox.addItem ("Method: Attenuate", 1); methodBox.addItem ("Method: Replace", 2);
        methodBox.addItem ("Method: Stray consonant", 3);
        methodBox.setSelectedId (1, juce::dontSendNotification);
        methodBox.setTooltip ("Attenuate keeps the music that is under the noise and only turns down what sticks out above the surrounding sound: the best for a click, a pencil drop or a cough over a held note. Replace throws the box away and builds new sound from the surroundings: for gaps and loud, wide damage.\n\n"
                               "Attenuate a stray consonant: for tidying a doubled or messy consonant to one. Draw the box round the EARLY or LATE copy only (from where the stray one starts up to where the main one starts, "
                               "or from where the main one ends to where the stray one stops), from about 2 kHz (1.5 kHz for sh) to the top. Each track is matched to the side of the box AWAY from the main consonant "
                               "(the vowel before an early copy, the sound after a late one), so the main consonant next to the box does not count as 'normal' sound and the stray copy comes right down; "
                               "vowels that carry on through the box keep their level. The sound just before the box dies away naturally into it and the box rises to meet the sound after it, so there are no hard edges: "
                               "cover all of the stray copy and a little past it on the clean side. Use 100 % strength.");
        for (int b : { 128, 256, 512, 1024, 2048, 4096 }) bandsBox.addItem ("Bands: " + juce::String (b), b);
        bandsBox.setSelectedId (1024, juce::dontSendNotification);
        bandsBox.setTooltip ("How finely the sound is split into pitches for Attenuate. More bands follow steady notes more exactly; fewer bands are sharper in time (clicks). 1024 suits most things.");
        linkBtn.setToggleState (true, juce::dontSendNotification);
        linkBtn.setTooltip ("A stereo file is turned down by the SAME amount on both sides (the amount found for the louder side), with each side's own phase kept. The time difference between the microphones and the level difference between them, which make the stereo image, are then not changed. Untick to treat every channel on its own.");
        linkBtn.onClick = [this] { invalidate(); };
        multiBtn.setToggleState (true, juce::dontSendNotification);
        multiBtn.setTooltip ("Attenuate works with a long window for the bass and a short one for the top and joins them smoothly, so both low notes and clicks are matched well.");
        specBtn.setTooltip ("How the picture is drawn: sharpness, colours, and the levels shown.");
        specBtn.onClick = [this] { openSpectroSettings(); };
        methodBox.onChange = [this] { refreshControls(); invalidate(); };
        bandsBox.onChange = [this] { invalidate(); };
        multiBtn.onClick = [this] { invalidate(); };
        view.onNeedDetail = [this] (const PictureRequest& rq) { startDetail (rq); };
        for (auto* x : std::initializer_list<juce::Component*> { &methodBox, &bandsBox, &multiBtn, &linkBtn, &specBtn, &dirBox, &previewBtn, &declickBtn, &compareBtn, &revertBtn, &finishBtn, &close, &playBtn, &stopBtn, &loopBtn, &hearCap, &hearBox, &scaleBox, &detailBox }) addAndMakeVisible (x);
        scaleBox.addItem ("Pitch scale: Mel (as iZotope RX)", 6); scaleBox.addItem ("Pitch scale: linear (even Hz)", 1); scaleBox.addItem ("Pitch scale: mostly linear", 2); scaleBox.addItem ("Pitch scale: halfway", 3);
        scaleBox.addItem ("Pitch scale: mostly musical", 4); scaleBox.addItem ("Pitch scale: musical (every octave the same height)", 5);
        scaleBox.setSelectedId (6, juce::dontSendNotification); view.setMel();
        scaleBox.setTooltip ("How the pitches are spread up the picture. Linear: every 1000 Hz takes the same height (the high end gets room, the low end is squashed). Musical: every octave takes the same height (like a piano). The steps in between blend the two.");
        detailBox.addItem ("Bass detail: blended (sharp top, fine bass)", 1); detailBox.addItem ("Bass detail: off (sharpest in time)", 2); detailBox.addItem ("Bass detail: extra fine", 3);
        detailBox.setSelectedId (1, juce::dontSendNotification);
        detailBox.setTooltip ("The picture is worked out twice: with a short window (sharp in time, good for clicks) and with a long one (sharp in pitch, good for bass notes). Blended: the long one is used at the bottom, the short one at the top, and they fade into each other in between (250 Hz to 1.5 kHz). Off: the short one only. Extra fine: an even longer window for the bass (below about 800 Hz).");
        detailBox.onChange = [this] { rescan(); };
        scaleBox.onChange = [this] { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; const int id = scaleBox.getSelectedId(); if (id == 6) view.setMel(); else view.setScale (w[juce::jlimit (0, 4, id - 1)]); };
        view.onNote = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };
        hearCap.setText ("Listen to", juce::dontSendNotification); hearCap.setColour (juce::Label::textColourId, theme::text);
        hearBox.addItem ("Original", 1); hearBox.addItem ("Fixed (after Fix)", 2); hearBox.setSelectedId (1, juce::dontSendNotification);
        hearBox.setTooltip ("Which sound Play plays: the audio as it is, or the audio with the repair applied (available after you press ReTouch or De-click). Press Play while a loop runs to hear the other one.");
        playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        playBtn.setTooltip ("Plays the part of the picture you can see, from the red playhead if you have clicked the time ruler to place one (zoom in with the left / right arrows to play a shorter piece, zoom out to hear more), through the mixers, with a playhead on the picture. No repair is needed: use it to listen for what has to be mended. Key: Space");
        loopBtn.setTooltip ("Repeat what is playing until you press Stop");
        stopBtn.setTooltip ("Stops playing");
        playBtn.onClick = [this] { playPressed(); };
        stopBtn.onClick = [this] { if (ctx.stop) ctx.stop(); };
        loopBtn.onClick = [this] { if (! isPlayingNow()) return; if (loopBtn.getToggleState()) playPressed(); else if (ctx.setLooping) ctx.setLooping (false); };
        hearBox.onChange = [this] { if (isPlayingNow()) playPressed(); };
        ticker.owner = this; ticker.startTimerHz (30);
        if (ctx.hold) ctx.hold (true);
        previewBtn.onClick = [this] { preview (0); };
        gainBtn.onClick = [this] { openGain(); };
        gainBtn.setTooltip ("Gain: makes the sound inside the box louder or quieter (from silence up to +18 dB), and nothing else: for a resonance, a ring or a note in one spot that should stand out less, or more. "
                            "Draw the box round it and press G. The gain fades in and out smoothly at the box's edges.");
        addAndMakeVisible (gainBtn); gainBtn.setWantsKeyboardFocus (false);
        declickBtn.onClick = [this] { preview (1); };
        compareBtn.onClick = [this] { toggleCompare(); };
        revertBtn.onClick = [this] { revert(); };
        finishBtn.onClick = [this] { finish(); };
        close.onClick = [this] { if (running) job.requestCancel(); else if (mayClose()) closeDialogOf (this); };
        previewBtn.setColour (juce::TextButton::buttonColourId, theme::accent); declickBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        finishBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        declickBtn.setTooltip ("Finds and mends the clicks in the time span of the box (or everywhere if there is no box; the pitch range of the box is ignored) and shows the result. Set how many it finds with Click sensitivity. A digital dropout (a block of samples that went missing: the sound jumps in one sample on every track at once) is mended by putting the missing samples back, on every track in the same way. Same rules as ReTouch: nothing is written yet, Play to listen, Undo if it did not work.");
        previewBtn.setTooltip ("ReTouch: rebuilds or turns down the box you drew from its surroundings and shows the result. (A small box round a click that reaches down into the bass: if the click is a digital dropout, the samples that went missing are put back first, on every track in the same way, and then the box is left alone; Click sensitivity decides how readily such a dropout is found.) It does not play, and nothing is written to the files yet: press Play to listen, and Undo if it did not work. You can then draw a box round another problem and press ReTouch or De-click again: the earlier fixes stay.");
        finishBtn.setTooltip ("Writes all the fixes into the files (everything between the marks is rendered again in one go). The Undo command of the window brings the original back afterwards.");
        close.setTooltip ("Closes the window and puts everything back as it was (nothing is kept).");
        revertBtn.setTooltip ("Takes the last fix off again. Press it again to take off the one before.");
        compareBtn.setTooltip ("Switches the picture between the original and the repaired sound.");
        view.onSelection = [this] { refreshControls(); invalidate(); };
        setupTabs();
        setupConsonants();
        loadControls();
        refreshControls();
        close.setButtonText ("Cancel");
        if (ctx.pairWritten)
        {
            finishBtn.setButtonText ("Write back this stream"); close.setButtonText ("Cancel both");
            finishBtn.setTooltip ("Gets this stream ready and closes this window (press it on an untouched stream to leave that one as it is). When BOTH streams have been written back the corrected audio goes into the edit, with the edit point and its fades exactly where they are. Cancel (or the X) closes both windows and changes nothing.");
        }
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
                    self->status.setText ("Press Space (or Play) to listen to what is in view (left / right arrows zoom in time, down / up zoom in / out in pitch, click the ruler to place the playhead, drag it to move; R = ReTouch, D = De-click). Drag a box around the noise (drag its edges or corners to resize it), set the controls, then press ReTouch (or De-click), then Play to listen. Undo if it did not work; or draw a box round the next problem and ReTouch / De-click again (the earlier fixes stay). Write back to file when you are happy.  Double-click the picture to clear the box.", juce::dontSendNotification);
                    self->scanned = true; self->refreshControls();
                });
    }
    juce::String prefKey (const char* k) const { return juce::String ("repair.") + k; }
    /** What the controls were set to last time (the picture settings, the repair settings): put back when the window opens, saved when it closes. */
    void loadControls()
    {
        auto& p = uiPrefs();
        auto num = [&] (const char* k, juce::Slider& s) { const auto key = prefKey (k); if (p.containsKey (key)) s.setValue (juce::jlimit (s.getMinimum(), s.getMaximum(), p.getDoubleValue (key)), juce::dontSendNotification); };
        auto id = [&] (const char* k, juce::ComboBox& b) { const auto key = prefKey (k); if (p.containsKey (key) && b.indexOfItemId (p.getIntValue (key)) >= 0) b.setSelectedId (p.getIntValue (key), juce::dontSendNotification); };
        num ("sens", sens); num ("strength", strength); num ("surround", surround); num ("weight", weight); num ("amount", amount); num ("residual", residual);
        id ("method", methodBox); id ("bands", bandsBox); id ("dir", dirBox); id ("scale", scaleBox); id ("detail", detailBox);
        if (p.containsKey (prefKey ("multi"))) multiBtn.setToggleState (p.getBoolValue (prefKey ("multi")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("link"))) linkBtn.setToggleState (p.getBoolValue (prefKey ("link")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("roll"))) view.setRoll (p.getDoubleValue (prefKey ("roll")));
        num ("consLevel", level);
        tab = juce::jlimit (0, (int) kTabCount - 1, p.getIntValue (prefKey ("tab"), kTabTouch)); lightTab();
        const int sc = scaleBox.getSelectedId();
        if (sc == 6) view.setMel(); else { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; view.setScale (w[juce::jlimit (0, 4, sc - 1)]); }
    }
    void saveControls()
    {
        auto& p = uiPrefs();
        p.setValue (prefKey ("sens"), sens.getValue()); p.setValue (prefKey ("strength"), strength.getValue()); p.setValue (prefKey ("surround"), surround.getValue()); p.setValue (prefKey ("weight"), weight.getValue());
        p.setValue (prefKey ("amount"), amount.getValue()); p.setValue (prefKey ("residual"), residual.getValue());
        p.setValue (prefKey ("method"), methodBox.getSelectedId()); p.setValue (prefKey ("bands"), bandsBox.getSelectedId()); p.setValue (prefKey ("dir"), dirBox.getSelectedId());
        p.setValue (prefKey ("scale"), scaleBox.getSelectedId()); p.setValue (prefKey ("detail"), detailBox.getSelectedId());
        p.setValue (prefKey ("multi"), multiBtn.getToggleState()); p.setValue (prefKey ("link"), linkBtn.getToggleState()); p.setValue (prefKey ("roll"), view.rollValue()); p.setValue (prefKey ("tab"), tab);
        p.setValue (prefKey ("consLevel"), level.getValue());
    }
    /** Closing (Cancel, the X or Escape) with fixes made but not written back asks first (they used to go without a word - Escape is easily pressed).
        False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (committed.empty() || accepted) return true;
        const int n = (int) committed.size();
        juce::Component::SafePointer<RepairDialog> self (this);
        confirmAsync ("Spectral Repair", juce::String (n) + (n == 1 ? " fix has" : " fixes have") + " not been written back to the file yet. Close the window and throw "
                      + (n == 1 ? "it" : "them") + " away? (Write back to file keeps " + (n == 1 ? "it" : "them") + ".)",
                      n == 1 ? "Throw it away" : "Throw them away", [self] { if (self != nullptr) closeDialogOf (self.getComponent()); });
        return false;
    }
    ~RepairDialog() override
    {
        saveControls();
        ticker.stopTimer(); ticker.owner = nullptr;
        if (detailCancel != nullptr) detailCancel->store (true);
        if (ctx.stop) ctx.stop();
        if (! accepted)                                                    // closing without Commit puts everything back as it was
        {
            if (applied && ctx.unapply) ctx.unapply();
            if (prepared && ctx.discard) ctx.discard();
        }
        if (ctx.hold) ctx.hold (false);
    }
    // ---- the tools at the bottom: one tab for each tool (only one set of controls shows at a time, so the picture keeps the room), then the transport and the fixes ----
    enum { kTabTouch, kTabGain, kTabClick, kTabDenoise, kTabConsonant, kTabCount };
    static constexpr int kTabRows = (kTabCount + 1) / 2;                          // the tabs stand in two columns
    static constexpr int kRowH = 24, kRowGap = 4, kPanelPad = 6, kTabW = 104, kTabGap = 12, kCapGap = 12, kTransportH = 26, kActionW = 116;      // (kActionW: the button that does the work, the same size and in the same place on every tab)
    static constexpr int sliderW = 140;                                           // a slider with its number: short on purpose
    static constexpr int kChoicesW = 184 + 8 + 180 + 8 + 96 + 8 + 132 + 8 + 112;                       // the five choices of ReTouch in a row
    static constexpr int kAmountsW = (58 + sliderW) + kCapGap + (116 + sliderW) + kCapGap + (100 + sliderW) + 16 + kActionW;   // its three sliders and the button in a row
    static constexpr int kListenW = 70 + 6 + 60 + 6 + 64 + 12 + 62 + 170, kBarMinW = 90, kFixesW = 120 + 8 + 70 + 8 + 176 + 8 + 96;
    int tab = kTabTouch;
    juce::TextButton tabBtn[kTabCount];
    juce::Label gainHint;
    juce::Rectangle<int> panelR, contentR;                                       // the tool panel and the part of it that holds the controls (set by layoutBottom)
    /** ReTouch has two rows (the choices; the sliders and the button) and, where the window is narrow, one more for each that does not fit in a row. */
    static int reTouchRows (int w) { return (w >= kChoicesW ? 1 : 2) + (w >= kAmountsW ? 1 : 2); }
    static int transportRows (int w) { return w >= kListenW + 8 + kBarMinW + 16 + kFixesW ? 1 : 2; }
    static juce::Rectangle<int> rowOf (juce::Rectangle<int> inner, int row) { return { inner.getX(), inner.getY() + row * (kRowH + kRowGap), inner.getWidth(), kRowH }; }
    /** Puts a control at the left of the row (nothing if it is hidden: the ones after it move up to fill the space). */
    static void putLeft (juce::Rectangle<int>& row, juce::Component& c, int w, int gap = 8)
    {
        if (! c.isVisible()) return;
        c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
    }
    /** A caption and the slider after it. */
    static void putSlider (juce::Rectangle<int>& row, juce::Label& cap, juce::Slider& sl, int capW)
    {
        if (! sl.isVisible()) { cap.setBounds ({}); return; }
        cap.setBounds (row.removeFromLeft (capW)); sl.setBounds (row.removeFromLeft (sliderW)); row.removeFromLeft (kCapGap);
    }
    std::vector<juce::Component*> tabControls (int t)
    {
        switch (t)
        {
            case kTabTouch:   return { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &strengthCap, &strength, &surroundCap, &surround, &weightCap, &weight, &previewBtn };
            case kTabGain:    return { &gainBtn, &gainHint };
            case kTabClick:   return { &sensCap, &sens, &declickBtn };
            case kTabConsonant: return { &copyBtn, &copyMoveBtn, &consHint, &consCap, &consBox, &renameBtn, &forgetBtn, &levelCap, &level,
                                         &nudgeCap, &nudgeL5, &nudgeL1, &nudgeRead, &nudgeR1, &nudgeR5, &pasteBtn };
            default:          return { &amountCap, &amount, &residualCap, &residual, &roomBtn, &roomCap, &denoiseBtn };
        }
    }
    /** Lights the tab that is showing and no other. (By hand, not with a radio group: JUCE's radio group runs the click action of the key it turns off, and that put ReTouch back on.) */
    void lightTab() { for (int i = 0; i < kTabCount; ++i) tabBtn[i].setToggleState (i == tab, juce::dontSendNotification); }
    void selectTab (int t)
    {
        tab = juce::jlimit (0, (int) kTabCount - 1, t);
        lightTab();
        refreshControls();                                                       // (which also lays the controls out again)
    }
    void setupTabs()
    {
        const char* names[] = { "ReTouch [R]", "Gain [G]", "De-click [D]", "Denoise [N]", "Consonants [C]" };
        const char* tips[] = { "ReTouch: rebuilds or turns down the box you drew from its surroundings (key R).", "Gain: makes the sound in the box louder or quieter (key G).",
                               "De-click: finds and mends clicks (key D).", "Denoise: takes the room noise out of the box (keys L and N).",
                               "Consonants: copy a consonant (every track, every channel, as each heard it), move an early or late one, or paste a kept one where one is missing (key C; P pastes; comma and full stop nudge)." };
        for (int i = 0; i < kTabCount; ++i)
        {
            auto& t = tabBtn[i]; t.setButtonText (names[i]); t.setTooltip (tips[i]); t.setClickingTogglesState (false);
            t.setColour (juce::TextButton::buttonOnColourId, theme::accent);
            t.onClick = [this, i] { selectTab (i); };
            addAndMakeVisible (t);
        }
        lightTab();
        gainHint.setText ("Louder or quieter inside the box, from silence up to +18 dB, and nothing else. Draw a box, then press Gain [G].", juce::dontSendNotification);
        gainHint.setColour (juce::Label::textColourId, theme::dimText); gainHint.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (gainHint);
    }
    /** The controls of the tab that is showing, in the panel's content area. */
    void placeTab (juce::Rectangle<int> in, int rows)
    {
        auto lastRow = rowOf (in, rows - 1);                                     // the button that does the work is in the same place whichever tab shows: the bottom right corner of the panel
        const auto action = lastRow.removeFromRight (kActionW);
        switch (tab)
        {
            case kTabTouch:
            {   // first row what to do and how, then how much, and the button that does it
                int row = 0;
                auto r = rowOf (in, row++);
                putLeft (r, methodBox, 184); putLeft (r, dirBox, 180); putLeft (r, bandsBox, 96);
                if (in.getWidth() < kChoicesW) r = rowOf (in, row++);                                        // narrow: the two ticks on a row of their own
                putLeft (r, multiBtn, 132); putLeft (r, linkBtn, 112);
                r = rowOf (in, row++);
                if (in.getWidth() >= kAmountsW)
                {
                    previewBtn.setBounds (action); r.removeFromRight (kActionW + 16);
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116); putSlider (r, weightCap, weight, 100);
                }
                else                                                                                           // narrow: Before <-> After and the button go on a second row
                {
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116);
                    r = rowOf (in, row++);
                    putSlider (r, weightCap, weight, 100);
                    previewBtn.setBounds (action);
                }
                break;
            }
            case kTabGain:
            {
                gainBtn.setBounds (action); gainHint.setBounds (rowOf (in, 0));
                break;
            }
            case kTabClick:
            {
                auto r = rowOf (in, 0);
                sensCap.setBounds (r.removeFromLeft (112)); r.removeFromLeft (8); sens.setBounds (r.removeFromLeft (sliderW));
                declickBtn.setBounds (action);
                break;
            }
            case kTabConsonant:
            {   // copy; the kept ones; then the level, the nudge and Paste (in the action corner)
                auto r0 = rowOf (in, 0);
                putLeft (r0, copyBtn, 150); putLeft (r0, copyMoveBtn, 150, 12); consHint.setBounds (r0);
                auto r1 = rowOf (in, 1);
                consCap.setBounds (r1.removeFromLeft (44)); r1.removeFromLeft (4);
                forgetBtn.setBounds (r1.removeFromRight (70)); r1.removeFromRight (6); renameBtn.setBounds (r1.removeFromRight (84)); r1.removeFromRight (8);
                consBox.setBounds (r1.withWidth (juce::jmin (r1.getWidth(), 520)));
                pasteBtn.setBounds (action);
                levelCap.setBounds (lastRow.removeFromLeft (44)); level.setBounds (lastRow.removeFromLeft (sliderW)); lastRow.removeFromLeft (kCapGap + 4);
                nudgeCap.setBounds (lastRow.removeFromLeft (48));
                nudgeL5.setBounds (lastRow.removeFromLeft (34)); lastRow.removeFromLeft (3); nudgeL1.setBounds (lastRow.removeFromLeft (28)); lastRow.removeFromLeft (3);
                nudgeRead.setBounds (lastRow.removeFromLeft (70)); lastRow.removeFromLeft (3);
                nudgeR1.setBounds (lastRow.removeFromLeft (28)); lastRow.removeFromLeft (3); nudgeR5.setBounds (lastRow.removeFromLeft (34));
                break;
            }
            default:
            {
                auto r1 = rowOf (in, 0), r2 = rowOf (in, 1);
                amountCap.setBounds (r1.removeFromLeft (62)); amount.setBounds (r1.removeFromLeft (sliderW)); r1.removeFromLeft (kCapGap);
                residualCap.setBounds (r1.removeFromLeft (66)); residual.setBounds (r1.removeFromLeft (sliderW));
                denoiseBtn.setBounds (action); r2.removeFromRight (kActionW + 8);
                roomBtn.setBounds (r2.removeFromLeft (190)); r2.removeFromLeft (8); roomCap.setBounds (r2);
                break;
            }
        }
    }
    /** The tool panel (the tabs in two columns on the left, the controls of the chosen one beside them) and under it the transport and the fixes.
        Returns the height they need at this width (place = false only measures). */
    int layoutBottom (juce::Rectangle<int> area, bool place)
    {
        const int tabsW = 2 * kTabW + kRowGap;
        const int contentW = juce::jmax (100, area.getWidth() - 2 * kPanelPad - tabsW - kTabGap);
        const int rows = juce::jmax ((int) kTabRows, reTouchRows (contentW));     // (the same height whichever tab shows, so the picture does not jump; the tabs need their rows)
        const int panelH = 2 * kPanelPad + rows * kRowH + (rows - 1) * kRowGap;
        const int trRows = transportRows (area.getWidth());
        const int total = panelH + trRows * (kRowGap + kTransportH);
        if (! place) return total;
        panelR = { area.getX(), area.getY(), area.getWidth(), panelH };
        auto inner = panelR.reduced (kPanelPad);
        auto tabsArea = inner.removeFromLeft (tabsW); inner.removeFromLeft (kTabGap);
        for (int i = 0; i < kTabCount; ++i) tabBtn[i].setBounds (tabsArea.getX() + (i % 2) * (kTabW + kRowGap), tabsArea.getY() + (i / 2) * (kRowH + kRowGap), kTabW, kRowH);
        contentR = inner;
        placeTab (contentR, rows);
        auto placeFixes = [this] (juce::Rectangle<int> r)
        {
            close.setBounds (r.removeFromRight (96)); r.removeFromRight (8);
            finishBtn.setBounds (r.removeFromRight (176)); r.removeFromRight (8);
            revertBtn.setBounds (r.removeFromRight (70)); r.removeFromRight (8);
            compareBtn.setBounds (r.removeFromRight (120));
        };
        auto row = juce::Rectangle<int> (area.getX(), panelR.getBottom() + kRowGap, area.getWidth(), kTransportH);
        if (trRows == 1) placeFixes (row.removeFromRight (kFixesW));
        playBtn.setBounds (row.removeFromLeft (70)); row.removeFromLeft (6);
        stopBtn.setBounds (row.removeFromLeft (60)); row.removeFromLeft (6);
        loopBtn.setBounds (row.removeFromLeft (64)); row.removeFromLeft (12);
        hearCap.setBounds (row.removeFromLeft (62)); hearBox.setBounds (row.removeFromLeft (170)); row.removeFromLeft (8);
        bar.setBounds (row.withTrimmedRight (trRows == 1 ? 16 : 0).reduced (0, 3));                      // (the progress bar of a scan or a fix, shown only while it runs)
        if (trRows == 2) placeFixes ({ area.getX(), row.getBottom() + kRowGap, area.getWidth(), kTransportH });
        return total;
    }
    void paint (juce::Graphics& g) override
    {
        if (panelR.isEmpty()) return;
        const auto b = panelR.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.16f)); g.fillRoundedRectangle (b, 5.0f);
        const auto line = theme::manuscript() ? theme::giltDim() : theme::border.brighter (0.25f);
        g.setColour (line); g.drawRoundedRectangle (b, 5.0f, 1.0f);
        const float dx = (float) contentR.getX() - (float) kTabGap * 0.5f;
        g.setColour (line.withAlpha (0.7f)); g.drawLine (dx, b.getY() + 5.0f, dx, b.getBottom() - 5.0f, 1.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        const int bottomH = layoutBottom (r, false);                             // how high the tools need to be at this width
        auto bottom = r.removeFromBottom (bottomH);
        layoutBottom (bottom, true);
        r.removeFromBottom (4);
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        { auto ir = r.removeFromBottom (22); specBtn.setBounds (ir.removeFromRight (150)); ir.removeFromRight (6); scaleBox.setBounds (ir.removeFromRight (300)); ir.removeFromRight (6); detailBox.setBounds (ir.removeFromRight (210)); info.setBounds (ir.reduced (2, 0)); }
        view.setBounds (r);
        repaint (bottom.expanded (2));
    }
protected:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)                                      // Space: play what is loaded, or stop
        {
            if (spaceIsForRecording()) return true;
            if (scanned && ! running) { if (isPlayingNow()) { if (ctx.stop) ctx.stop(); } else playPressed(); }
            return true;
        }
        if ((k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R') && ! k.getModifiers().isAnyModifierKeyDown())      // R: do the fix with the settings as they are
        {
            selectTab (kTabTouch);
            if (previewBtn.isEnabled()) preview (0);
            return true;
        }
        if ((k.getTextCharacter() == 'n' || k.getTextCharacter() == 'N') && ! k.getModifiers().isAnyModifierKeyDown())      // N: denoise the box
        {
            selectTab (kTabDenoise);
            if (denoiseBtn.isEnabled()) preview (2);
            return true;
        }
        if ((k.getTextCharacter() == 'l' || k.getTextCharacter() == 'L') && ! k.getModifiers().isAnyModifierKeyDown())      // L: this box is the room tone
        {
            selectTab (kTabDenoise);
            if (roomBtn.isEnabled()) roomFromBox();
            return true;
        }
        if ((k.getTextCharacter() == 'g' || k.getTextCharacter() == 'G') && ! k.getModifiers().isAnyModifierKeyDown())      // G: change the gain of the box
        {
            selectTab (kTabGain);
            if (gainBtn.isEnabled()) openGain();
            return true;
        }
        if ((k.getTextCharacter() == 'd' || k.getTextCharacter() == 'D') && ! k.getModifiers().isAnyModifierKeyDown())
        {
            selectTab (kTabClick);
            if (declickBtn.isEnabled()) preview (1);
            return true;
        }
        if ((k.getTextCharacter() == 'c' || k.getTextCharacter() == 'C') && ! k.getModifiers().isAnyModifierKeyDown())      // C: the Consonants tab
        {
            selectTab (kTabConsonant);
            return true;
        }
        if ((k.getTextCharacter() == 'p' || k.getTextCharacter() == 'P') && ! k.getModifiers().isAnyModifierKeyDown())      // P: paste the chosen consonant at the playhead
        {
            selectTab (kTabConsonant);
            if (pasteBtn.isEnabled()) pasteConsonant();
            return true;
        }
        {   // , and . nudge the consonant just pasted 1 ms earlier / later; < and > (Shift with them) 5 ms
            const auto ch = k.getTextCharacter();
            const int ms = ch == ',' ? -1 : ch == '.' ? 1 : ch == '<' ? -5 : ch == '>' ? 5 : 0;
            if (ms != 0 && ! k.getModifiers().isCommandDown() && ! k.getModifiers().isAltDown())
            {
                if (scanned && (running ? pasteRunning : lastIsPaste())) { selectTab (kTabConsonant); nudge (ms); }
                return true;
            }
        }
        if (scanned && view.zoomKey (k)) return true;                           // arrows: right / left zoom in / out in time, up / down in pitch
        return false;
    }
private:
    /** The room tone Denoise uses: a box of this picture marked with L (its time span; the whole spectrum of it), else the project's. */
    bool haveRoomTone() const { if (rtBoxT1 > rtBoxT0) return true; for (auto& p : ctx.roomMono) if (p.valid()) return true; return false; }
    /** The same in a few words, for the little caption in the Denoise box (the full sentence is its tooltip). */
    juce::String roomShortText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (ctx.roomSource.isNotEmpty()) return "Room tone: the project's";
        return "Room tone: none yet";
    }
    juce::String roomText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: the box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s of this picture";
        if (ctx.roomSource.isNotEmpty()) return "Room tone: the project's (" + ctx.roomSource + ")";
        return "Room tone: none yet - draw a box over room only and press L, or use the edit window's Room tone button";
    }
    void roomFromBox()
    {
        if (! scanned || running || ! view.hasSelection()) return;
        rtBoxT0 = view.selT0(); rtBoxT1 = view.selT1();
        if (rtBoxT1 - rtBoxT0 < 0.5) { status.setText ("The room-tone box is too short: make it at least half a second long (a second or more is better).", juce::dontSendNotification); rtBoxT0 = rtBoxT1 = -1.0; refreshControls(); return; }     // (what learnDenoise needs: a shorter box was taken here, then refused at Write back)
        status.setText (roomText() + ". Now draw a box round the noise to take out and press Denoise (N).", juce::dontSendNotification);
        refreshControls();
    }      // one window for both: ReTouch and De-click share the picture, the box and the list of fixes
    void updateSurround() { const bool lr = dirBox.getSelectedId() != 2; view.setSurround (scanned && ! consonantMethod(), surround.getValue(), dirBox.getSelectedId() - 1, lr ? weight.getValue() / 100.0 : 0.5); }
    bool consonantMethod() const { return methodBox.getSelectedId() == 3; }
    bool lookingAcross() const { return dirBox.getSelectedId() != 2 || consonantMethod(); }
    /** The changes that make up the repaired sound: everything accepted so far, plus the change being previewed. */
    std::vector<RepairOp> currentOps() const
    {
        return committed;
    }
    bool haveRepair() const { return ! committed.empty(); }
    void invalidate()
    {
        if (running) { changedWhileRunning = true; return; }
        dropPendingPrepared();
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
        if (ctx.play) ctx.play (view.playFrom(), juce::jmin (seconds(), view.visT1()), loopBtn.getToggleState());
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
        status.setText ("Playing the REPAIRED sound (through the repair). Write back to file keeps it, Undo takes the last fix off, or draw another box and ReTouch / De-click again.", juce::dontSendNotification);
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
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
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
            status.setText ("There is no repair to listen to yet: press ReTouch or De-click first. Playing the original.", juce::dontSendNotification);
        }
        playOriginal();
    }
    void refreshControls()
    {
        for (auto* c : tabControls (tab)) c->setVisible (true);                    // (the rules below hide what the chosen method does not use)
        updateSurround();
        const bool cons = consonantMethod();
        const bool att = methodBox.getSelectedId() == 1 || cons;
        for (auto* c : std::initializer_list<juce::Component*> { &strength, &strengthCap, &methodBox }) c->setVisible (true);
        for (auto* c : std::initializer_list<juce::Component*> { &dirBox, &surround, &surroundCap }) c->setVisible (! cons);      // a stray consonant: always the sound before / after, its own search length
        weightCap.setText (cons ? "Side (50 = auto)" : "Before <-> After", juce::dontSendNotification);
        bandsBox.setVisible (att); multiBtn.setVisible (att); linkBtn.setVisible (att);
        weight.setVisible (lookingAcross()); weightCap.setVisible (lookingAcross());
        sens.setVisible (true); sensCap.setVisible (true);
        for (int t = 0; t < kTabCount; ++t) if (t != tab) for (auto* c : tabControls (t)) c->setVisible (false);      // only the tab that is showing
        if (view.hasSelection())
            info.setText ("Selected: " + juce::String (view.selT0(), 3) + " - " + juce::String (view.selT1(), 3) + " s  ("
                          + juce::String ((view.selT1() - view.selT0()) * 1000.0, 0) + " ms),  " + juce::String ((int) view.selF0()) + " - " + juce::String ((int) view.selF1()) + " Hz"
                          + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        else info.setText (juce::String ("Nothing selected")
                           + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        const bool ready = scanned && ! running;
        playBtn.setEnabled (ready); stopBtn.setEnabled (ready); loopBtn.setEnabled (ready); hearBox.setEnabled (ready);
        previewBtn.setEnabled (ready && view.hasSelection()); declickBtn.setEnabled (ready); gainBtn.setEnabled (ready && view.hasSelection());
        denoiseBtn.setEnabled (ready && view.hasSelection() && haveRoomTone()); roomBtn.setEnabled (ready && view.hasSelection());
        roomCap.setText (roomShortText(), juce::dontSendNotification); roomCap.setTooltip (roomText());
        finishBtn.setEnabled (ready && (haveRepair() || ctx.pairWritten != nullptr));
        revertBtn.setEnabled (ready && ! history.empty());
        compareBtn.setEnabled (ready && ! history.empty());
        compareBtn.setButtonText (showingAfter ? "Show before" : "Show fixed");
        refreshConsonantList();
        const bool canCopy = ready && view.hasSelection() && ctx.readChannels != nullptr && ctx.keepConsonant != nullptr;
        copyBtn.setEnabled (canCopy); copyMoveBtn.setEnabled (canCopy);
        const bool haveSel = selectedConsonant() != nullptr;
        consBox.setEnabled (! consList.empty()); renameBtn.setEnabled (haveSel); forgetBtn.setEnabled (haveSel);
        pasteBtn.setEnabled (ready && haveSel);
        const bool adj = scanned && (running ? pasteRunning : lastIsPaste());
        for (auto* b : { &nudgeL5, &nudgeL1, &nudgeR1, &nudgeR5 }) b->setEnabled (adj);
        nudgeRead.setText (nudgeText(), juce::dontSendNotification);
        resized();                                   // (what is shown decides how the boxes are packed)
    }
    /** The settings as they are now (and the part of the area they apply to). what: 0 = ReTouch (or Gain), 1 = De-click, 2 = Denoise. */
    FixSpec buildSpec (juce::int64& t0, juce::int64& t1, int what) const
    {
        FixSpec spec; t0 = 0; t1 = ctx.length;
        if (view.hasSelection()) { t0 = (juce::int64) std::llround (view.selT0() * ctx.rate); t1 = (juce::int64) std::llround (view.selT1() * ctx.rate); }
        if (what == 2)
        {
            spec.kind = FixSpec::Kind::Denoise; spec.f0 = view.selF0(); spec.f1 = view.selF1();
            spec.dn.amountDb = amount.getValue(); spec.dn.residualDb = residual.getValue();
            spec.dnBoxT0 = rtBoxT1 > rtBoxT0 ? rtBoxT0 : -1.0; spec.dnBoxT1 = rtBoxT1 > rtBoxT0 ? rtBoxT1 : -1.0;
        }
        else if (what == 0)
        {
            spec.kind = FixSpec::Kind::Patch; spec.f0 = view.selF0(); spec.f1 = view.selF1(); spec.sensitivity = sens.getValue();         // (the click sensitivity also decides how readily lost samples (a digital dropout) in a small box are found)
            spec.repair.mode = methodBox.getSelectedId() == 2 ? RepairMode::Replace : RepairMode::Attenuate;
            spec.repair.consonant = consonantMethod();
            spec.repair.bands = bandsBox.getSelectedId(); spec.repair.multiRes = multiBtn.getToggleState(); spec.repair.linkChannels = linkBtn.getToggleState();
            spec.repair.strength = strength.getValue() / 100.0;
            spec.repair.direction = spec.repair.consonant ? RepairDirection::LeftRight : (RepairDirection) (dirBox.getSelectedId() - 1);
            spec.repair.contextPct = surround.getValue();
            spec.repair.weighting = weight.getValue() / 100.0;
            if (gainNext) { spec.repair.mode = RepairMode::Gain; spec.repair.gainDb = gainNextDb; spec.repair.strength = 1.0; spec.repair.consonant = false; }
        }
        else { spec.kind = FixSpec::Kind::Declick; spec.sensitivity = sens.getValue(); }
        return spec;
    }
    double seconds() const { return (double) ctx.length / ctx.rate; }
    /** The Gain pop-up: a slider from silence to +18 dB for the box; Apply makes it a fix like ReTouch (Undo takes it off). */
    void openGain()
    {
        if (! scanned || running || ! view.hasSelection()) return;
        struct GainPop : public juce::Component
        {
            juce::Slider sl; juce::TextButton apply { "Apply" }, cancel { "Cancel" }; juce::Label cap;
            std::function<void (double)> onApply;
            GainPop (double start)
            {
                cap.setText ("Gain of the box", juce::dontSendNotification); cap.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (cap);
                sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (-60.0, 18.0, 0.1); sl.setValue (start, juce::dontSendNotification);
                sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22); sl.setDoubleClickReturnValue (true, 0.0);
                sl.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : (v > 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };
                sl.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.retainCharacters ("-+0123456789.").getDoubleValue(); };
                sl.setSkewFactorFromMidPoint (-6.0);
                addAndMakeVisible (sl); addAndMakeVisible (apply); addAndMakeVisible (cancel);
                apply.setColour (juce::TextButton::buttonColourId, theme::accent);
                apply.onClick = [this] { const double v = sl.getValue(); auto cb = onApply; if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); if (cb) cb (v); };
                cancel.onClick = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
                setSize (380, 92);
            }
            void resized() override
            {
                auto r = getLocalBounds().reduced (8);
                cap.setBounds (r.removeFromTop (20)); sl.setBounds (r.removeFromTop (28)); r.removeFromTop (6);
                cancel.setBounds (r.removeFromRight (80)); r.removeFromRight (6); apply.setBounds (r.removeFromRight (80));
            }
        };
        auto pop = std::make_unique<GainPop> (uiPrefs().getDoubleValue (prefKey ("gainDb"), -6.0));
        juce::Component::SafePointer<RepairDialog> self (this);
        pop->onApply = [self] (double db)
        {
            if (self == nullptr) return;
            uiPrefs().setValue (self->prefKey ("gainDb"), db);
            if (std::abs (db) < 0.05) { self->status.setText ("Gain 0 dB: nothing to change.", juce::dontSendNotification); return; }
            self->gainNext = true; self->gainNextDb = db <= -59.95 ? -200.0 : db;
            self->preview (0);
            self->gainNext = false;
        };
        juce::CallOutBox::launchAsynchronously (std::move (pop), gainBtn.getScreenBounds(), nullptr);
    }
    void preview (int what)
    {
        if (! scanned || running || origTracks == nullptr) return;
        if (what == 2 && ! haveRoomTone()) { status.setText ("No room tone: draw a box over a part that is only room and press L, or use the edit window's Room tone button first.", juce::dontSendNotification); return; }
        juce::int64 t0 = 0, t1 = 0; const auto spec = buildSpec (t0, t1, what);
        startFix (spec, t0, t1);
    }
    /** Makes one change on a job thread - on the picture's sound as it is now (every fix so far), track by track - and keeps it as the next fix (Undo takes it
        off). 'placed': a pasted consonant, where it was first put (a nudge keeps it, so the window can say how far it has been moved). */
    void startFix (const FixSpec& spec, juce::int64 t0, juce::int64 t1, juce::int64 placed = -1)
    {
        if (! scanned || running || origTracks == nullptr) return;
        dropPendingPrepared();
        pvSpec = spec; pvT0 = t0; pvT1 = t1; pvPlaced = placed;
        pasteRunning = spec.kind == FixSpec::Kind::Paste;
        auto src = origTracks; const double rate = ctx.rate;
        const auto trackIds = ctx.trackIds;
        struct Out { MergedSpectrogram spec; std::shared_ptr<std::vector<std::vector<float>>> tracks; FixSpec fix; SpectralRepair::DeclickReport rep { 0, 0, 0, 0 }; };
        auto out = std::make_shared<Out>();
        status.setText (spec.kind == FixSpec::Kind::Denoise ? "Denoising the box..." : spec.kind == FixSpec::Kind::Paste ? "Pasting the consonant..." : "Fixing...", juce::dontSendNotification);
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); denoiseBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false);
        juce::Component::SafePointer<RepairDialog> self (this);
        const int detailN = detailSize();
        auto roomMono = std::make_shared<std::vector<denoise::Profile>> (ctx.roomMono);
        auto firstTracks = history.empty() ? origTracks : history.front().tracks;   // the sound as it was loaded, before any of these fixes
        runJob ([src, firstTracks, out, spec, t0, t1, rate, detailN, roomMono, trackIds] (AudioJob& j) -> juce::String
                {
                    auto tracks = std::make_shared<std::vector<std::vector<float>>> (*src);       // from the last accepted state
                    // a denoise: each picture track's room tone - the box of this picture, learned from the track as it was loaded (as the files are, when they are
                    // written: once a fix had denoised the room tone itself, the picture learned a profile far too low from it), or the project's for that track
                    std::vector<std::shared_ptr<const std::vector<denoise::Profile>>> dnProf;
                    if (spec.kind == FixSpec::Kind::Denoise)
                        for (size_t i = 0; i < tracks->size(); ++i)
                        {
                            denoise::Profile pr;
                            if (spec.dnBoxT1 > spec.dnBoxT0)
                            {
                                const auto& x = firstTracks != nullptr && i < firstTracks->size() ? (*firstTracks)[i] : (*tracks)[i];
                                const auto a = juce::jlimit ((juce::int64) 0, (juce::int64) x.size(), (juce::int64) std::llround (spec.dnBoxT0 * rate)), b = juce::jlimit ((juce::int64) 0, (juce::int64) x.size(), (juce::int64) std::llround (spec.dnBoxT1 * rate));
                                if (b > a) pr = denoise::learnProfile ({ std::vector<float> (x.begin() + a, x.begin() + b) }, rate, spec.dn);
                            }
                            else if (i < roomMono->size()) pr = (*roomMono)[i];
                            if (! pr.valid()) return "There is no room tone for track " + juce::String ((int) i + 1) + " (learn it with the edit window's Room tone button, or mark a box of this picture with L).";
                            dnProf.push_back (std::make_shared<const std::vector<denoise::Profile>> (std::vector<denoise::Profile> { pr }));
                        }
                    // lost samples (a digital jump where a block of samples went missing) are one event on every track: they are found on all the tracks together, and the
                    // list goes with the fix, so the files are written with exactly the same mend
                    auto fix = spec;
                    if (audioops::findsLost (spec, rate, t0, t1) && ! tracks->empty())
                    {
                        const long L = (long) (*tracks)[0].size();
                        fix.lost = audioops::findLost (*tracks, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), spec.sensitivity);
                        for (auto& l : fix.lost) l.at += juce::jlimit ((juce::int64) 0, (juce::int64) L, t0) - t0;          // (from t0 itself)
                        fix.lostKnown = true;
                    }
                    // a stray consonant: which side of the box the main consonant is on is ONE answer for all the tracks (found on all of them together); it goes with
                    // the fix so the files are written the same way
                    if (fix.kind == FixSpec::Kind::Patch && fix.repair.mode == RepairMode::Attenuate && fix.repair.consonant && std::abs (fix.repair.weighting - 0.5) < 0.04 && ! tracks->empty())
                    {
                        const long L = (long) (*tracks)[0].size();
                        std::vector<const std::vector<float>*> tp; for (auto& t : *tracks) tp.push_back (&t);
                        const auto d = SpectralRepair::consonantSides (tp, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), fix.f0, fix.f1);
                        fix.repair.consonantBefore = d.first; fix.repair.consonantEdge = d.second;
                    }
                    out->fix = fix;
                    for (size_t i = 0; i < tracks->size(); ++i)
                    {
                        if (j.cancelled()) return "Cancelled.";
                        j.setProgress ((float) i / (float) tracks->size());
                        std::vector<std::vector<float>> ch; ch.push_back (std::move ((*tracks)[i]));
                        auto fi = fix; if (i < dnProf.size()) fi.dnProfiles = dnProf[i];
                        if (fi.kind == FixSpec::Kind::Paste) audioops::resolvePaste (fi, i < trackIds.size() ? trackIds[i] : juce::Uuid::null());     // (the picture's row: the track's channels, averaged)
                        audioops::applyFix (ch, rate, (long) t0, (long) t1, fi, (long) (0.02 * rate));
                        (*tracks)[i] = std::move (ch[0]);
                        if (fix.kind == FixSpec::Kind::Declick) { const auto r = SpectralRepair::lastDeclickReport(); out->rep.mended += r.mended; out->rep.lines += r.lines; out->rep.linesMended += r.linesMended; out->rep.linesLeft += r.linesLeft; }
                    }
                    out->fix.dnProfiles = nullptr;                                                // (the profiles are not part of the fix that is kept)
                    out->fix.pasteChannels = nullptr;                                             // (nor is the one track's part of a pasted consonant: each file gets its own)
                    out->spec = SpectralRepair::spectrogram (*tracks, rate, 1600, detailN);
                    out->tracks = tracks;
                    return {};
                },
                [self, out] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->nextNote.clear(); self->pendingNudge = 0; self->pendingLevel = false; self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    // the fix is kept straight away (it stays temporary until "Write back to file"); Undo takes it off again
                    self->history.push_back ({ self->origTracks, self->origSpec });
                    self->pvSpec = out->fix;                                             // (with the lost samples found on all the tracks)
                    self->committed.push_back (RepairOp { self->pvSpec, self->pvT0, self->pvT1, self->pvPlaced });
                    self->origTracks = out->tracks;
                    self->origSpec = std::make_shared<MergedSpectrogram> (out->spec);
                    self->view.setData (std::move (out->spec), self->seconds(), true);
                    self->showingAfter = true;
                    self->dropPendingPrepared();
                    self->changedWhileRunning = false;
                    self->hearBox.setSelectedId (2, juce::dontSendNotification);
                    juce::String lostNote = self->nextNote; self->nextNote.clear();
                    for (auto& l : out->fix.lost)
                        lostNote << "Digital dropout at " << juce::String ((double) (self->pvT0 + l.at) / self->ctx.rate, 3) << " s: " << l.count << " samples were missing on every track; put back. ";
                    if (out->fix.kind == FixSpec::Kind::Patch && out->fix.repair.mode == RepairMode::Gain)
                        lostNote << "Gain " << (out->fix.repair.gainDb <= -120.0 ? juce::String ("-inf") : (out->fix.repair.gainDb > 0 ? "+" : "") + juce::String (out->fix.repair.gainDb, 1)) << " dB on the box. ";
                    if (out->fix.kind == FixSpec::Kind::Denoise)
                        lostNote << "Denoised the box (" << (int) out->fix.f0 << " - " << (int) out->fix.f1 << " Hz, residual " << juce::String (out->fix.dn.residualDb, 0) << " dB). ";
                    if (out->fix.kind == FixSpec::Kind::Declick)
                    {
                        const auto& r = out->rep;
                        lostNote << "De-click, all tracks: " << r.linesMended << " vertical line(s) seen in the picture and mended whole, " << r.mended << " click(s) found in the waveform and mended"
                                 << (r.linesLeft > 0 ? ", " + juce::String (r.linesLeft) + " line(s) STILL THERE (try a higher sensitivity, or ReTouch a box round each)" : juce::String (", none left in the picture")) << ". ";
                    }
                    if (out->fix.kind == FixSpec::Kind::Paste && out->fix.paste != nullptr)
                    {
                        const auto missing = consonants::tracksMissing (*out->fix.paste, self->ctx.trackIds);
                        int here = 0; for (auto& id : self->ctx.trackIds) if (out->fix.paste->forTrack (id) != nullptr) ++here;
                        lostNote << "Consonant pasted at " << juce::String ((double) (self->pvT0 + out->fix.paste->lead) / self->ctx.rate, 3) << " s on " << here << (here == 1 ? " track" : " tracks")
                                 << (std::abs (out->fix.pasteGainDb) >= 0.05 ? ", " + juce::String (out->fix.pasteGainDb, 1) + " dB" : juce::String())
                                 << (self->pvPlaced >= 0 && self->pvT0 != self->pvPlaced ? ", nudged " + self->nudgeText() : juce::String()) << ". ";
                        int none = 0; for (auto& id : self->ctx.trackIds) if (out->fix.paste->forTrack (id) == nullptr) ++none;
                        if (none > 0) lostNote << none << (none == 1 ? " track of this window was" : " tracks of this window were") << " not in it and get nothing. ";
                        if (! missing.isEmpty()) lostNote << "(It also has " << missing.joinIntoString (", ") << ", not in this window.) ";
                        lostNote << "Press Play to listen; nudge it with comma / full stop (1 ms) or Shift with them (5 ms), or change Level; Undo takes it off. ";
                        self->status.setText (lostNote, juce::dontSendNotification);
                        self->refreshControls();
                        self->followPending();
                        return;
                    }
                    if (out->fix.kind == FixSpec::Kind::Patch && out->fix.repair.consonant)
                    {
                        const auto& r = out->fix.repair;
                        if (r.consonantBefore < 0.0) lostNote << "Stray consonant: matched to the side you chose with Before / After. ";
                        else if (r.consonantEdge > 0) lostNote << "Stray consonant: the main one is just AFTER the box, so every track was matched to the sound before it. ";
                        else if (r.consonantEdge < 0) lostNote << "Stray consonant: the main one is just BEFORE the box, so every track was matched to the sound after it. ";
                        else lostNote << "Stray consonant: no main consonant right next to the box, so both sides were used (" << juce::roundToInt (r.consonantBefore * 100.0) << " % before). ";
                    }
                    self->status.setText (lostNote + "Fix " + juce::String ((int) self->committed.size()) + " done (not written yet). Press Play to listen to it. If it worked, draw a box round the next problem and press ReTouch or De-click again, or press Write back to file. If not, press Undo and try again.", juce::dontSendNotification);
                    self->refreshControls();
                });
        for (auto* b : { &nudgeL5, &nudgeL1, &nudgeR1, &nudgeR5 }) b->setEnabled (pasteRunning);     // (a nudge pressed now follows when this one is made)
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
    /** The audio the picture shows now (the fixed sound, or the sound before the last fix). */
    std::shared_ptr<std::vector<std::vector<float>>> shownTracks() const
    {
        if (! showingAfter && ! history.empty() && history.back().tracks != nullptr) return history.back().tracks;
        return origTracks;
    }
    /** Makes the sharp picture of what is in view on its own thread (the previous request, if still running, is dropped). */
    void startDetail (const PictureRequest& rq)
    {
        auto tr = shownTracks(); if (tr == nullptr || tr->empty()) return;
        if (detailCancel != nullptr) detailCancel->store (true);
        auto cancel = std::make_shared<std::atomic<bool>> (false); detailCancel = cancel;
        const double rate = ctx.rate; juce::Component::SafePointer<RepairDialog> self (this);
        std::thread ([tr, rq, rate, cancel, self]
        {
            try                                                                  // (nothing may escape a thread: out of memory leaves the quick picture, nothing worse)
            {
                auto pic = picture::render (*tr, rate, rq.t0, rq.t1, rq.w, rq.h, rq.axis, rq.opt, [cancel] { return cancel->load(); });
                pic.id = rq.id;
                if (cancel->load()) return;
                juce::MessageManager::callAsync ([self, p = std::make_shared<DetailPicture> (std::move (pic))] { if (self != nullptr) self->view.setDetail (std::move (*p)); });
            }
            catch (...) {}
        }).detach();
    }
    void openSpectroSettings();
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
        showingAfter = true;
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
        if (ops.empty())
        {
            if (ctx.pairWritten) { if (ctx.stop) ctx.stop(); accepted = true; ctx.pairWritten ([] { return true; }); return; }          // an untouched stream is left as it is; this window goes away
            closeDialogOf (this); return;
        }
        if (prepared && preparedOps == ops.size()) { finishCommit(); return; }
        dropPrepared();
        auto prep = ctx.prepare;
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
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
        if (ctx.pairWritten)
        {
            accepted = true;                                         // (the corrected files wait in the pair; this window goes away and the other stream is next)
            ctx.pairWritten ([] { return true; });
            return;
        }
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
    InfoNote intro; juce::Label info, sensCap, strengthCap, surroundCap, weightCap, amountCap, residualCap, roomCap;
    SpectrogramView view; juce::ComboBox dirBox, scaleBox, detailBox, methodBox, bandsBox; juce::ToggleButton multiBtn { "Multi-resolution" }, linkBtn { "Link stereo" }; juce::TextButton specBtn { "Spectrogram settings..." };
    std::shared_ptr<std::atomic<bool>> detailCancel; juce::Slider sens, strength, surround, weight, amount, residual;
    juce::TextButton roomBtn { "This box is the room tone [L]" }, denoiseBtn { "Denoise [N]" }; double rtBoxT0 = -1.0, rtBoxT1 = -1.0;
    juce::TextButton playBtn { "Play" }, stopBtn { "Stop" };
    juce::ToggleButton loopBtn { "Loop" };
    juce::Label hearCap; juce::ComboBox hearBox;
    bool prepared = false, applied = false, accepted = false, changedWhileRunning = false;
    size_t preparedOps = 0;                                        // how many changes the prepared files contain
    std::vector<RepairOp> committed;                               // the changes accepted so far (in order)
    juce::TextButton previewBtn { "ReTouch [R]" }, declickBtn { "De-click [D]" }, compareBtn { "Show before" }, revertBtn { "Undo" }, finishBtn { "Write back to file" }, close { "Cancel" };
    juce::TextButton gainBtn { "Gain [G]" };
    bool gainNext = false; double gainNextDb = 0.0;                // the next fix is a gain change of the box (set by the Gain pop-up)
    struct Step { std::shared_ptr<std::vector<std::vector<float>>> tracks; std::shared_ptr<MergedSpectrogram> spec; };
    std::vector<Step> history;                                     // the state before each fix, for Undo
    bool scanned = false, showingAfter = false;
    std::shared_ptr<std::vector<std::vector<float>>> origTracks;
    std::shared_ptr<MergedSpectrogram> origSpec;
    FixSpec pvSpec; juce::int64 pvT0 = 0, pvT1 = 0, pvPlaced = -1;

    // ---- consonants (3.6.0): Copy / Copy and remove keep what the stray-consonant ReTouch takes out of the box (every track, every channel); Paste adds a kept one
    //      at the playhead; the last paste can be nudged and its level changed (it is taken off and put back) ----
    juce::TextButton copyBtn { "Copy consonant" }, copyMoveBtn { "Copy and remove" }, renameBtn { "Rename..." }, forgetBtn { "Delete" }, pasteBtn { "Paste here [P]" };
    juce::TextButton nudgeL5 { "<<" }, nudgeL1 { "<" }, nudgeR1 { ">" }, nudgeR5 { ">>" };
    juce::Label consHint, consCap, levelCap, nudgeCap, nudgeRead;
    juce::ComboBox consBox;
    juce::Slider level;
    std::vector<ConsonantDef> consList;                            // the project's kept consonants, as the list shows them
    juce::String consListKey;                                      // (what the list was made from: it is made again only when that changes)
    juce::Uuid consSel;                                            // the one chosen in the list
    std::map<juce::String, std::shared_ptr<const ConsonantAudio>> consCache;    // the audio of the ones pasted while this window is open
    juce::String lastConsName { "t" };
    juce::String nextNote;                                         // said first when the fix under way is done (Copy and remove: what to do next)
    bool pasteRunning = false;                                     // the fix under way is a paste (or a nudge of one)
    juce::int64 pendingNudge = 0; bool pendingLevel = false;       // nudges / a level change pressed while it is being made

    void setupConsonants()
    {
        auto label = [this] (juce::Label& l, const juce::String& t, bool dim = false)
        {
            l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, dim ? theme::dimText : theme::text); l.setMinimumHorizontalScale (0.8f); addChildComponent (l);
        };
        label (consHint, "Draw the box round the consonant from about 2 kHz up, then Copy.", true);
        label (consCap, "Kept:"); label (levelCap, "Level"); label (nudgeCap, "Nudge");
        nudgeRead.setText ({}, juce::dontSendNotification); nudgeRead.setJustificationType (juce::Justification::centred); nudgeRead.setColour (juce::Label::textColourId, theme::text); addChildComponent (nudgeRead);
        nudgeRead.setTooltip ("How far the consonant just pasted has been nudged from where it was first put.");
        copyBtn.setTooltip ("Keeps the consonant in the box with the project: what the stray-consonant ReTouch would take out of it (the consonant itself, above the vowel and the room), "
                            "on EVERY track and every channel, each as that microphone heard it - nothing is mixed down. The audio here is not changed. "
                            "It can be pasted into any ReTouch window of this project, today or weeks later (until you delete it).");
        copyMoveBtn.setTooltip ("For an early or late consonant: keeps it (as Copy consonant does) and takes it out here, as the stray-consonant ReTouch does at full strength. "
                                "Then click the time ruler where it should sound and press Paste here [P]: each track's own part of it goes back on that track.");
        renameBtn.setTooltip ("Gives the chosen consonant another name (t, s, sh, soft d ...).");
        forgetBtn.setTooltip ("Takes the chosen consonant off this project's list and deletes its audio. Fixes already made with it stay as they are.");
        pasteBtn.setTooltip ("Adds the chosen consonant on top of the sound, the start of the box it was copied from at the playhead you placed by clicking the time ruler. "
                             "Each track gets its own part of it (a track it was not copied from gets nothing). Same rules as ReTouch: Play to listen, Undo takes it off, Write back keeps it. Key: P");
        consBox.setTooltip ("The consonants kept with this project (Copy consonant adds one; Copy and remove adds one too). Paste uses the one chosen here.");
        consBox.onChange = [this]
        {
            const int i = consBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow (i, (int) consList.size())) consSel = consList[(size_t) i].id;
            refreshControls();
        };
        level.setSliderStyle (juce::Slider::LinearHorizontal); level.setRange (-24.0, 12.0, 0.5); level.setValue (0.0, juce::dontSendNotification); level.setScrollWheelEnabled (false);
        level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22); level.setTextValueSuffix (" dB"); level.setDoubleClickReturnValue (true, 0.0);
        level.setTooltip ("How loud the consonant goes in, against how it was copied (0 dB = as it was). Changing it changes the consonant just pasted, too. Double-click for 0 dB.");
        level.onValueChange = [this] { if (! level.isMouseButtonDown()) levelChanged(); };
        level.onDragEnd = [this] { levelChanged(); };
        addChildComponent (level);
        auto nb = [this] (juce::TextButton& b, int ms, const juce::String& tip)
        {
            b.setTooltip (tip); b.onClick = [this, ms] { nudge (ms); }; b.setWantsKeyboardFocus (false); addChildComponent (b);
        };
        nb (nudgeL5, -5, "The consonant just pasted 5 ms earlier (Shift + comma)."); nb (nudgeL1, -1, "The consonant just pasted 1 ms earlier (comma).");
        nb (nudgeR1, 1, "The consonant just pasted 1 ms later (full stop)."); nb (nudgeR5, 5, "The consonant just pasted 5 ms later (Shift + full stop).");
        copyBtn.onClick = [this] { copyConsonant (false); };
        copyMoveBtn.onClick = [this] { copyConsonant (true); };
        renameBtn.onClick = [this] { renamePressed(); };
        forgetBtn.onClick = [this] { forgetPressed(); };
        pasteBtn.onClick = [this] { pasteConsonant(); };
        pasteBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        for (auto* c : std::initializer_list<juce::Component*> { &copyBtn, &copyMoveBtn, &renameBtn, &forgetBtn, &pasteBtn, &consBox }) { addChildComponent (c); c->setWantsKeyboardFocus (false); }
    }
    const ConsonantDef* selectedConsonant() const { for (auto& c : consList) if (c.id == consSel) return &c; return nullptr; }
    bool lastIsPaste() const { return ! committed.empty() && committed.back().spec.kind == FixSpec::Kind::Paste; }
    juce::String nudgeText() const
    {
        if (! lastIsPaste() || committed.back().placed < 0) return {};
        const double ms = 1000.0 * (double) (committed.back().t0 - committed.back().placed) / ctx.rate;
        return (ms > 0.04 ? "+" : "") + juce::String (ms, std::abs (ms - std::round (ms)) < 0.05 ? 0 : 1) + " ms";
    }
    /** The list, from the project (made again only when the project's list has changed). */
    void refreshConsonantList()
    {
        if (! ctx.consonantList) return;
        auto list = ctx.consonantList();
        juce::String key; for (auto& c : list) key << c.id.toString() << c.name << c.from << ";";
        if (key == consListKey) return;
        consListKey = key; consList = std::move (list);
        consBox.clear (juce::dontSendNotification);
        for (int i = 0; i < (int) consList.size(); ++i) consBox.addItem (consList[(size_t) i].listText(), i + 1);
        int at = -1; for (int i = 0; i < (int) consList.size(); ++i) if (consList[(size_t) i].id == consSel) at = i;
        if (at < 0 && ! consList.empty()) { at = (int) consList.size() - 1; consSel = consList.back().id; }      // (none chosen yet: the newest)
        if (at >= 0) consBox.setSelectedItemIndex (at, juce::dontSendNotification);
        consBox.setTextWhenNothingSelected (consList.empty() ? "None kept yet: draw a box round a consonant and press Copy" : "Choose one");
    }
    /** Copy (and remove): on a job thread, every track's channels round the box are read from the files, this window's fixes so far put on them, and the
        stray-consonant ReTouch's difference kept (consonants::extract). Then it is named (Copy) or named for where it came from (Copy and remove, which then
        takes it out here like the ReTouch). */
    void copyConsonant (bool remove)
    {
        if (! scanned || running || ! view.hasSelection() || ! ctx.readChannels || ! ctx.keepConsonant) return;
        juce::int64 t0 = 0, t1 = 0;
        auto spec = buildSpec (t0, t1, 0);
        spec = consonants::copySpec (spec);
        if (t1 - t0 < (juce::int64) (0.005 * ctx.rate)) { status.setText ("The box is too short to hold a consonant.", juce::dontSendNotification); return; }
        const auto pad = (juce::int64) std::llround ((1.0 + 2.0 * (double) (t1 - t0) / ctx.rate) * ctx.rate);       // (the ReTouch measures the sound round the box)
        const auto a = juce::jmax ((juce::int64) 0, t0 - pad), b = juce::jmin (ctx.length, t1 + pad);
        auto read = ctx.readChannels; const auto ids = ctx.trackIds; const auto names = ctx.trackNames; const double rate = ctx.rate;
        const auto ops = committed; auto picture = origTracks;
        auto out = std::make_shared<ConsonantAudio>();
        status.setText (remove ? "Copying the consonant (every track) and taking it out here..." : "Copying the consonant (every track)...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        runJob ([read, ids, names, rate, ops, picture, out, spec, t0, t1, a, b] (AudioJob& j) -> juce::String
                {
                    auto tracks = read (a, b, j);
                    if (j.cancelled()) return "Cancelled.";
                    if (tracks.size() != ids.size()) return "The tracks could not all be read.";
                    // this window's fixes so far, where they reach this stretch (a denoise is left out: it needs each file's room tone, and only touches the background)
                    for (auto& o : ops)
                    {
                        if (o.spec.kind == FixSpec::Kind::Denoise || o.t1 <= a || o.t0 >= b) continue;
                        for (size_t i = 0; i < tracks.size(); ++i)
                        {
                            auto s = o.spec; if (s.kind == FixSpec::Kind::Paste) audioops::resolvePaste (s, ids[i]);
                            audioops::applyFix (tracks[i], rate, (long) (o.t0 - a), (long) (o.t1 - a), s, (long) (0.02 * rate));
                        }
                    }
                    // which side of the box is the background: one answer for all the tracks, found on the picture as the ReTouch finds it
                    auto s = spec;
                    if (std::abs (s.repair.weighting - 0.5) < 0.04 && picture != nullptr && ! picture->empty())
                    {
                        const long L = (long) (*picture)[0].size();
                        std::vector<const std::vector<float>*> tp; for (auto& t : *picture) tp.push_back (&t);
                        const auto d = SpectralRepair::consonantSides (tp, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), s.f0, s.f1);
                        s.repair.consonantBefore = d.first; s.repair.consonantEdge = d.second;
                    }
                    *out = consonants::extract (ids, names, tracks, rate, (long) (t0 - a), (long) (t1 - a), s);
                    if (out->tracks.empty()) return "Nothing to copy: the ReTouch found nothing in the box that stands out from the sound beside it. Draw the box round the consonant only, from about 2 kHz up.";
                    return {};
                },
                [self, out, remove, spec, t0, t1] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    const auto from = self->ctx.where + ", " + timefmt::minSecText (self->ctx.areaStart + (double) t0 / self->ctx.rate, 2);
                    if (remove)
                    {
                        juce::Uuid id; const auto e = self->ctx.keepConsonant (*out, "moved", from, id);
                        if (e.isNotEmpty()) { self->status.setText (e, juce::dontSendNotification); self->refreshControls(); return; }
                        self->consSel = id; self->consCache[id.toString()] = out;
                        self->refreshControls();
                        self->nextNote = "Copied (kept as \"moved\") and taken out here. Now click the time ruler where it should start and press Paste here [P]. ";
                        self->startFix (spec, t0, t1);                         // (taken out here as the stray-consonant ReTouch does, at full strength)
                        return;
                    }
                    auto keep = [self, out, from] (const juce::String& name)
                    {
                        if (self == nullptr) return;
                        juce::Uuid id; const auto e = self->ctx.keepConsonant (*out, name, from, id);
                        if (e.isNotEmpty()) { self->status.setText (e, juce::dontSendNotification); self->refreshControls(); return; }
                        self->lastConsName = name; self->consSel = id; self->consCache[id.toString()] = out;
                        self->status.setText ("Kept \"" + name + "\" (" + juce::String ((int) out->tracks.size()) + (out->tracks.size() == 1 ? " track, " : " tracks, ")
                                              + juce::String (juce::roundToInt (1000.0 * (double) out->length() / out->sampleRate)) + " ms) with the project: paste it here, or in any ReTouch window of this project, whenever you need it.",
                                              juce::dontSendNotification);
                        self->refreshControls();
                    };
                    askName ("Keep this consonant", "A name for it (t, s, sh, soft d ...). It is kept with the project, every track and channel as recorded.", self->lastConsName, keep);
                    self->refreshControls();
                });
    }
    static void askName (const juce::String& title, const juce::String& message, const juce::String& suggestion, std::function<void (const juce::String&)> done)
    {
        auto* aw = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("n", suggestion);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, done] (int res)
        {
            const auto n = aw->getTextEditorContents ("n").trim();
            delete aw;
            if (res == 1 && n.isNotEmpty() && done) done (n);
        }));
    }
    void renamePressed()
    {
        const auto* c = selectedConsonant(); if (c == nullptr || ! ctx.renameConsonant) return;
        const auto id = c->id;
        juce::Component::SafePointer<RepairDialog> self (this);
        askName ("Rename consonant", "A new name for \"" + c->name + "\" (" + c->from + ").", c->name, [self, id] (const juce::String& n)
        {
            if (self == nullptr) return;
            self->ctx.renameConsonant (id, n); self->refreshControls();
        });
    }
    void forgetPressed()
    {
        const auto* c = selectedConsonant(); if (c == nullptr || ! ctx.forgetConsonant) return;
        const auto id = c->id;
        juce::Component::SafePointer<RepairDialog> self (this);
        confirmAsync ("Delete consonant", "Take \"" + c->name + "\" (" + c->from + ") off this project's list and delete its audio? Fixes already made with it stay as they are.", "Delete",
                      [self, id] { if (self == nullptr) return; self->ctx.forgetConsonant (id); self->consCache.erase (id.toString()); self->consSel = juce::Uuid::null(); self->refreshControls(); });
    }
    /** Paste: the chosen consonant, its box's start at the placed playhead, at the level set; a fix like the others. */
    void pasteConsonant()
    {
        if (! scanned || running) return;
        const auto* c = selectedConsonant();
        if (c == nullptr) { status.setText ("Choose a kept consonant in the list first (or copy one: draw a box round it and press Copy consonant).", juce::dontSendNotification); return; }
        const double cue = view.cueTime();
        if (cue < 0.0) { status.setText ("Click the time ruler where the consonant should start (a red playhead appears), then press Paste here [P].", juce::dontSendNotification); return; }
        auto audio = consonantAudio (*c);
        if (audio == nullptr) return;
        if (std::abs (audio->sampleRate - ctx.rate) > 0.5) { status.setText ("\"" + c->name + "\" was copied at " + juce::String (audio->sampleRate / 1000.0, 1) + " kHz and this audio is at " + juce::String (ctx.rate / 1000.0, 1) + " kHz: it cannot be pasted here.", juce::dontSendNotification); return; }
        int here = 0; for (auto& id : ctx.trackIds) if (audio->forTrack (id) != nullptr) ++here;
        if (here == 0)
        {
            status.setText ("None of the tracks \"" + c->name + "\" was copied from (" + juce::StringArray (audio->trackNames.data(), (int) audio->trackNames.size()).joinIntoString (", ")
                            + ") are in this window, so there is nothing to paste on.", juce::dontSendNotification);
            return;
        }
        const auto t0 = (juce::int64) std::llround (cue * ctx.rate) - audio->lead, t1 = t0 + audio->length();
        const auto edge = (juce::int64) std::llround (0.04 * ctx.rate);                   // (clear of the joins at the ends of the part, where the new audio is crossfaded in)
        if (t0 < edge || t1 > ctx.length - edge) { status.setText ("The consonant would run past the part loaded in this window. Place the playhead further in, or mark a wider part (keys 1 and 2) and open the window again.", juce::dontSendNotification); return; }
        FixSpec spec; spec.kind = FixSpec::Kind::Paste; spec.paste = audio; spec.pasteGainDb = level.getValue();
        startFix (spec, t0, t1, t0);
    }
    std::shared_ptr<const ConsonantAudio> consonantAudio (const ConsonantDef& c)
    {
        auto it = consCache.find (c.id.toString());
        if (it != consCache.end()) return it->second;
        if (! ctx.loadConsonant) return nullptr;
        juce::String err;
        auto a = ctx.loadConsonant (c.id, err);
        if (a == nullptr) { status.setText (err.isNotEmpty() ? err : "The consonant's audio could not be read.", juce::dontSendNotification); return nullptr; }
        consCache[c.id.toString()] = a;
        return a;
    }
    /** The consonant just pasted, again: moved by 'ms' (and at the level set now). It is taken off and put back, so Undo still takes it off in one go. */
    void nudge (int ms)
    {
        const auto d = (juce::int64) std::llround ((double) ms * 0.001 * ctx.rate);
        if (running && pasteRunning) { pendingNudge += d; return; }            // (pressed again while the last nudge is still being made: it follows when that is done)
        adjustPaste (d);
    }
    void levelChanged()
    {
        if (running && pasteRunning) { pendingLevel = true; return; }
        if (lastIsPaste() && std::abs (committed.back().spec.pasteGainDb - level.getValue()) >= 0.01) adjustPaste (0);
    }
    /** A paste (or a nudge of one) has been made: nudges and level changes that came in meanwhile follow now, in one go. */
    void followPending()
    {
        const auto d = pendingNudge; const bool lv = pendingLevel;
        pendingNudge = 0; pendingLevel = false;
        if (d != 0 || (lv && lastIsPaste() && std::abs (committed.back().spec.pasteGainDb - level.getValue()) >= 0.01)) adjustPaste (d);
    }
    void adjustPaste (juce::int64 delta)
    {
        if (! scanned || running || ! lastIsPaste()) return;
        auto op = committed.back();
        op.t0 += delta; op.t1 += delta; op.spec.pasteGainDb = level.getValue();
        const auto edge = (juce::int64) std::llround (0.04 * ctx.rate);
        if (op.t0 < edge || op.t1 > ctx.length - edge) { status.setText ("It cannot go further: it would run past the part loaded in this window.", juce::dontSendNotification); return; }
        revert();
        startFix (op.spec, op.t0, op.t1, op.placed);
    }
};

/** The spectrogram settings (the ones iZotope RX has, as far as they apply here). Changes show at once and are remembered. */
class SpectroSettingsDialog : public juce::Component
{
public:
    SpectroSettingsDialog (const SpectroSettings& s, std::function<void (const SpectroSettings&)> apply) : cur (s), onApply (std::move (apply))
    {
        setSize (440, 330);
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (cPal, "Colour map"); cap (cLo, "Lowest level (dB)"); cap (cHi, "Highest level (dB)"); cap (cDet, "Detail"); cap (cRed, "Fewer points above (s)");
        sharpBtn.setButtonText ("Sharp picture for what is in view (high-quality rendering)"); sharpBtn.setToggleState (cur.sharp, juce::dontSendNotification);
        reBtn.setButtonText ("Enable reassignment (crisp lines)"); reBtn.setToggleState (cur.reassign, juce::dontSendNotification);
        pal.addItem ("Cyan to orange", 1); pal.addItem ("Heat (standard)", 2); pal.addItem ("Grey", 3); pal.addItem ("Ice", 4); pal.addItem ("Rainbow", 5);
        pal.setSelectedId (cur.palette + 1, juce::dontSendNotification);
        auto sl = [this] (juce::Slider& x, double a, double b, double st, double v) { x.setRange (a, b, st); x.setValue (v, juce::dontSendNotification); x.setSliderStyle (juce::Slider::LinearHorizontal); x.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22); addAndMakeVisible (x); x.onValueChange = [this] { push(); }; };
        sl (lo, -160.0, -40.0, 0.1, cur.lo); sl (hi, -60.0, 0.0, 0.1, cur.hi); sl (det, 0.0, 1.0, 0.05, cur.detail); sl (red, 20.0, 1200.0, 10.0, cur.reduceAbove);
        det.setTooltip ("Left: finer pitch (long analysis window). Right: finer time (short window).");
        reset.setButtonText ("Back to the RX-style defaults");
        reset.onClick = [this] { SpectroSettings d; cur = d; pal.setSelectedId (1, juce::dontSendNotification); lo.setValue (d.lo, juce::dontSendNotification); hi.setValue (d.hi, juce::dontSendNotification); det.setValue (d.detail, juce::dontSendNotification); red.setValue (d.reduceAbove, juce::dontSendNotification); sharpBtn.setToggleState (true, juce::dontSendNotification); reBtn.setToggleState (true, juce::dontSendNotification); onApply (cur); };
        for (auto* c : std::initializer_list<juce::Component*> { &sharpBtn, &reBtn, &pal, &reset }) addAndMakeVisible (c);
        sharpBtn.onClick = [this] { push(); }; reBtn.onClick = [this] { push(); }; pal.onChange = [this] { push(); };
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto row = [&r] { auto x = r.removeFromTop (30); r.removeFromTop (6); return x; };
        sharpBtn.setBounds (row()); reBtn.setBounds (row());
        auto a = row(); cPal.setBounds (a.removeFromLeft (150)); pal.setBounds (a);
        a = row(); cLo.setBounds (a.removeFromLeft (150)); lo.setBounds (a);
        a = row(); cHi.setBounds (a.removeFromLeft (150)); hi.setBounds (a);
        a = row(); cDet.setBounds (a.removeFromLeft (150)); det.setBounds (a);
        a = row(); cRed.setBounds (a.removeFromLeft (150)); red.setBounds (a);
        reset.setBounds (r.removeFromTop (30).removeFromLeft (230));
    }
private:
    void push()
    {
        cur.sharp = sharpBtn.getToggleState(); cur.reassign = reBtn.getToggleState(); cur.palette = juce::jmax (0, pal.getSelectedId() - 1);
        cur.lo = lo.getValue(); cur.hi = hi.getValue(); if (cur.hi - cur.lo < 6.0) cur.hi = cur.lo + 6.0;
        cur.detail = det.getValue(); cur.reduceAbove = red.getValue();
        onApply (cur);
    }
    SpectroSettings cur; std::function<void (const SpectroSettings&)> onApply;
    juce::Label cPal, cLo, cHi, cDet, cRed; juce::ComboBox pal; juce::Slider lo, hi, det, red;
    juce::ToggleButton sharpBtn, reBtn; juce::TextButton reset;
};

inline void RepairDialog::openSpectroSettings()
{
    juce::Component::SafePointer<RepairDialog> self (this);
    launchDialog (std::make_unique<SpectroSettingsDialog> (view.settings(), [self] (const SpectroSettings& n)
    {
        if (self == nullptr) return;
        self->view.setSettings (n); n.save (gSpecPrefs);
        if (gSpecPrefs != nullptr) gSpecPrefs->saveIfNeeded();
    }), "Spectrogram settings", this);
}

}} // namespace td::fixtools

#include "RetimerWindow.h"
#include "FixTools.h"
#include "Uikit.h"
#include "../core/Retime.h"
#include "../core/AudioOps.h"
#include "../core/TimeFormat.h"
#include "WaveStore.h"
#include <set>
#include <thread>
#include "../core/AudioFormats.h"

namespace td
{
namespace
{
void say (const juce::String& text) { fixtools::say ("ReTimEr", text); }     // (the fix tools' message box, with this window's title)

juce::String clockOf (double seconds)
{
    timefmt::Format f; f.mode = timefmt::Mode::minSec;
    return timefmt::format (f, seconds).text;
}

constexpr juce::uint32 kReTimEr = 0xffe5484d;          // the red of the ReTimEr's spring (its button)

/** Deletes files that were only being tried. On Windows a file that is still open cannot be deleted - the edit window may still be reading a retimed
    file for its waveform a moment after it was listened to - so what could not go now is tried again every half second for half a minute. */
void deleteTried (std::vector<juce::File> files, int triesLeft = 60)
{
    std::vector<juce::File> left;
    for (auto& f : files) if (f.exists() && ! f.deleteFile()) left.push_back (f);
    if (left.empty()) return;
    if (triesLeft <= 0) { for (auto& f : left) juce::Logger::writeToLog ("ReTimEr: could not delete " + f.getFullPathName() + " (still in use)"); return; }
    juce::Timer::callAfterDelay (500, [left, triesLeft] { deleteTried (left, triesLeft - 1); });
}
std::vector<juce::File> filesOf (const retime::Plan& plan)
{
    std::vector<juce::File> v;
    for (auto& s : plan.streams) for (auto& t : s.tracks) v.push_back (t.dest);
    return v;
}

/** The speed of the music along the part (100 % = as recorded), with the part shaded: what the settings do, at a glance. */
class SpeedCurve : public juce::Component
{
public:
    retime::TimeMap map;
    bool valid = false, whole = false;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::field); g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::border); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        const double L = map.t1 - map.t0;
        if (L <= 0.0) return;
        auto plot = r.reduced (52.0f, 12.0f).withTrimmedBottom (14.0f);
        const double pad = L * 0.12, x0 = map.t0 - pad, x1 = map.t1 + pad;
        const double held = valid ? 100.0 / map.S : 100.0;
        double lo = juce::jmin (held, 100.0), hi = juce::jmax (held, 100.0);
        const double span = juce::jmax (4.0, hi - lo); lo -= span * 0.45; hi += span * 0.45;
        auto X = [&] (double t) { return plot.getX() + (float) ((t - x0) / (x1 - x0)) * plot.getWidth(); };
        auto Y = [&] (double s) { return plot.getBottom() - (float) ((s - lo) / (hi - lo)) * plot.getHeight(); };
        g.setColour (theme::accent.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (X (map.t0), plot.getY() - 4.0f, X (map.t1), plot.getBottom() + 4.0f));
        // 100 %: as recorded
        {
            juce::Path base; base.startNewSubPath (plot.getX(), Y (100.0)); base.lineTo (plot.getRight(), Y (100.0));
            const float dash[] { 4.0f, 4.0f };
            juce::Path dashed; juce::PathStrokeType (1.0f).createDashedStroke (dashed, base, dash, 2);
            g.setColour (theme::dimText.withAlpha (0.7f)); g.fillPath (dashed);
            g.setFont (juce::FontOptions (11.5f)); g.drawText ("100 %", juce::Rectangle<float> (r.getX() + 4.0f, Y (100.0) - 8.0f, 44.0f, 16.0f), juce::Justification::centredRight);
        }
        // the curve
        juce::Path p;
        for (int i = 0; i <= 480; ++i)
        {
            const double t = x0 + (x1 - x0) * i / 480.0;
            const double s = valid ? 100.0 / map.rate (t) : 100.0;
            if (i == 0) p.startNewSubPath (X (t), Y (s)); else p.lineTo (X (t), Y (s));
        }
        g.setColour (valid ? juce::Colour (kReTimEr) : theme::dimText); g.strokePath (p, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (valid && std::abs (held - 100.0) > 0.005)
        {
            g.setColour (theme::text); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            g.drawText (juce::String (held, 2) + " %", juce::Rectangle<float> (X (map.t0 + map.rIn), Y (held) + (held < 100.0 ? 3.0f : -19.0f),
                                                                             juce::jmax (60.0f, X (map.t1 - map.rOut) - X (map.t0 + map.rIn)), 16.0f), juce::Justification::centred);
        }
        // what the ends are
        g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.0f));
        const juce::String a = whole ? "piece start" : "mark 1", b = whole ? "piece end" : "mark 2";
        g.drawText (a, juce::Rectangle<float> (X (map.t0) - 50.0f, plot.getBottom() + 2.0f, 100.0f, 14.0f), juce::Justification::centred);
        g.drawText (b, juce::Rectangle<float> (X (map.t1) - 50.0f, plot.getBottom() + 2.0f, 100.0f, 14.0f), juce::Justification::centred);
        g.setFont (juce::FontOptions (11.0f, juce::Font::italic));
        g.drawText ("speed", juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 3.0f, 60.0f, 14.0f), juce::Justification::centredLeft);
    }
};

class RetimerPanel : public juce::Component, private juce::Timer
{
public:
    RetimerPanel (AppContext& a, const juce::Uuid& eid, const retime::Request& q, juce::int64 from, juce::int64 to, const juce::String& what, const juce::String& note)
        : app (a), editId (eid), req (q), t0 (from), t1 (to), baseNote (note), hold (a), bar (progressValue)
    {
        auto* e = edit();
        sr = e != nullptr && e->sampleRate > 0 ? e->sampleRate : 48000.0;
        if (e != nullptr) { base.regions = e->regions; base.overdubs = e->overdubs; base.lanes = e->lanes; base.fixIn = e->fixIn; base.fixOut = e->fixOut; base.insertIndex = e->insertIndex; }
        for (auto& t : app.project.tracks) if (t.isLtc) ltcTracks.insert (t.id);
        app.undoHold += 1;                                                       // (trying it out makes no undo steps: Write back makes one)
        getProperties().set (ownsRKeyProperty(), true);
        setWantsKeyboardFocus (true);

        auto label = [this] (juce::Label& l, const juce::String& t, float size = 14.0f, bool bold = false)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
            l.setColour (juce::Label::textColourId, theme::text); l.setJustificationType (juce::Justification::centredLeft); addAndMakeVisible (l);
        };
        label (whatLabel, what, 14.0f); whatLabel.setJustificationType (juce::Justification::topLeft);
        label (noteLabel, note, 13.0f); noteLabel.setJustificationType (juce::Justification::topLeft); noteLabel.setColour (juce::Label::textColourId, theme::warn);
        label (selCap, "Selected length"); label (selValue, clockOf ((double) (t1 - t0) / sr), 22.0f, true);
        label (newCap, "New length");
        label (effectLabel, {}, 13.5f);
        label (rampInCap, "Ramp in"); label (rampOutCap, "Ramp out"); label (rampHint, "seconds - 0 = at once", 12.5f); rampHint.setColour (juce::Label::textColourId, theme::dimText);
        label (secIn, "s"); label (secOut, "s"); label (pctSign, "%");
        label (prerollCap, "Play from"); label (prerollAfter, "before", 13.0f);
        label (statusLabel, "Choose the new length, then Make retimed audio (or just Play retimed).", 13.0f); statusLabel.setJustificationType (juce::Justification::topLeft);
        for (auto* l : { &whatLabel, &noteLabel, &statusLabel, &effectLabel }) l->setMinimumHorizontalScale (1.0f);     // (long sentences wrap: never squashed)
        for (auto* l : { &rampInCap, &rampOutCap, &rampHint, &secIn, &secOut }) l->setVisible (! req.wholePiece);

        for (auto* t : { &asLength, &asPercent })
        {
            t->setRadioGroupId (4711); t->setColour (juce::ToggleButton::textColourId, theme::text); t->setWantsKeyboardFocus (false);
            t->onClick = [this] { refresh(); }; addAndMakeVisible (t);
        }
        const bool pct = app.props.getUserSettings() == nullptr || app.props.getUserSettings()->getBoolValue ("retimer.asPercent", true);
        (pct ? asPercent : asLength).setToggleState (true, juce::dontSendNotification);
        auto field = [this] (juce::TextEditor& t, const juce::String& text, const juce::String& tip)
        {
            t.setText (text, false); t.setJustification (juce::Justification::centred); t.setFont (juce::FontOptions (15.0f));
            t.setColour (juce::TextEditor::backgroundColourId, theme::field); t.setColour (juce::TextEditor::textColourId, theme::text);
            t.setColour (juce::TextEditor::outlineColourId, theme::border); t.setTooltip (tip);
            t.onTextChange = [this, &t]
            {
                if (&t == &lengthEdit) asLength.setToggleState (true, juce::dontSendNotification);          // (typing in a box makes it the one that counts)
                if (&t == &percentEdit) asPercent.setToggleState (true, juce::dontSendNotification);
                refresh();
            };
            t.onReturnKey = [this] { tidyFields(); grabKeyboardFocus(); };
            t.onFocusLost = [this] { tidyFields(); };
            addAndMakeVisible (t);
        };
        field (lengthEdit, clockOf ((double) (t1 - t0) / sr), "The new length of the part (minutes:seconds.milliseconds, or seconds). Longer is slower.");
        field (percentEdit, "100.00", "The new length as a percentage of the selected length: 105 is 5 % longer (slower), 95 is 5 % shorter (faster).");
        auto* us = app.props.getUserSettings();
        field (rampInEdit, us != nullptr ? us->getValue ("retimer.rampIn", "0") : "0", "How long the speed takes to glide from normal to the new speed after mark 1 (seconds of the original). 0: the new speed starts at once - good at a bar line or the start of a phrase.");
        field (rampOutEdit, us != nullptr ? us->getValue ("retimer.rampOut", "0") : "0", "How long the speed takes to glide back to normal before mark 2 (seconds of the original). 0: back to normal at once.");
        for (auto* t : { &rampInEdit, &rampOutEdit }) t->setVisible (! req.wholePiece);
        curve.whole = req.wholePiece; addAndMakeVisible (curve);

        for (int s : { 1, 2, 3, 5, 10 }) preroll.addItem (juce::String (s) + " s", s);
        preroll.setSelectedId (us != nullptr ? juce::jlimit (1, 10, us->getIntValue ("retimer.preroll", 3)) : 3, juce::dontSendNotification);
        if (preroll.getSelectedId() == 0) preroll.setSelectedId (3, juce::dontSendNotification);
        preroll.setTooltip ("How much of the music before the part plays first, so you hear it arrive");
        preroll.setWantsKeyboardFocus (false);
        addAndMakeVisible (preroll);

        makeButton.setTooltip ("Makes the retimed audio of every track (new files: the originals are never touched). Takes a little while - the bar shows how far it is.");
        playOrig.setTooltip ("Plays the edit as it is now, from a little before the part to a little after it [O]");
        playNew.setTooltip ("Plays the edit with the retimed audio in it (made first if it is not made yet) [Space]");
        stopButton.setTooltip ("Stops [Space]");
        writeButton.setTooltip ("Puts the retimed audio into the edit for good; everything after it moves along. Undo fix (or Ctrl+Z) takes it off again.");
        cancelButton.setTooltip ("Closes the window and leaves the edit as it was");
        makeButton.onClick = [this] { if (running) job->requestCancel(); else make (Then::nothing); };
        playOrig.onClick = [this] { playOriginal(); };
        playNew.onClick = [this] { playRetimed(); };
        stopButton.onClick = [this] { stop(); };
        writeButton.onClick = [this] { writeBack(); };
        cancelButton.onClick = [this] { closeMe(); };
        writeButton.setColour (juce::TextButton::buttonColourId, theme::accent);
        for (auto* b : { &makeButton, &playOrig, &playNew, &stopButton, &writeButton, &cancelButton }) { b->setWantsKeyboardFocus (false); addAndMakeVisible (b); }
        addChildComponent (bar);
        setSize (680, req.wholePiece ? 540 : 590);
        refresh();
        startTimerHz (10);
    }

    ~RetimerPanel() override
    {
        stopTimer();
        if (! accepted) { app.stopPlayback(); takeOut(); discardMade(); }
        hold = false;
        app.undoHold -= 1;
        if (job != nullptr) job->release();                                     // (asks it to stop; it tidies itself away - and the files of a run that is cut short)
        auto* us = app.props.getUserSettings();
        if (us != nullptr)
        {
            us->setValue ("retimer.asPercent", asPercent.getToggleState());
            us->setValue ("retimer.preroll", preroll.getSelectedId());
            if (! req.wholePiece) { us->setValue ("retimer.rampIn", rampInEdit.getText().trim()); us->setValue ("retimer.rampOut", rampOutEdit.getText().trim()); }
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::window);
        auto head = getLocalBounds().reduced (16, 10).removeFromTop (40);
        auto icon = head.removeFromLeft (52).toFloat();
        g.setColour (theme::button); g.fillRoundedRectangle (icon.reduced (2.0f, 2.0f), 5.0f);
        g.setColour (theme::border); g.drawRoundedRectangle (icon.reduced (2.5f), 5.0f, 1.0f);
        drawIcon (g, Icon::ReTimEr, icon.reduced (6.0f, 4.0f), theme::inkOn (theme::button));
        head.removeFromLeft (10);
        g.setColour (theme::text); g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        g.drawText ("ReTimEr", head, juce::Justification::centredLeft);
        g.setColour (theme::border); g.fillRect (16, sepY, getWidth() - 32, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 10);
        r.removeFromTop (46);
        whatLabel.setBounds (r.removeFromTop (38));
        noteLabel.setBounds (r.removeFromTop (noteLabel.getText().isEmpty() ? 4 : 52));
        sepY = r.getY() + 3; r.removeFromTop (10);
        { auto row = r.removeFromTop (30); selCap.setBounds (row.removeFromLeft (130)); selValue.setBounds (row); }
        r.removeFromTop (6);
        {
            auto row = r.removeFromTop (30);
            newCap.setBounds (row.removeFromLeft (130));
            asLength.setBounds (row.removeFromLeft (112)); lengthEdit.setBounds (row.removeFromLeft (120).reduced (0, 2)); row.removeFromLeft (26);
            asPercent.setBounds (row.removeFromLeft (138)); percentEdit.setBounds (row.removeFromLeft (84).reduced (0, 2)); pctSign.setBounds (row.removeFromLeft (24));
        }
        r.removeFromTop (2);
        effectLabel.setBounds (r.removeFromTop (24).withTrimmedLeft (130));
        if (! req.wholePiece)
        {
            r.removeFromTop (6);
            auto row = r.removeFromTop (28);
            rampInCap.setBounds (row.removeFromLeft (130)); rampInEdit.setBounds (row.removeFromLeft (70).reduced (0, 1)); secIn.setBounds (row.removeFromLeft (22));
            row.removeFromLeft (24);
            rampOutCap.setBounds (row.removeFromLeft (70)); rampOutEdit.setBounds (row.removeFromLeft (70).reduced (0, 1)); secOut.setBounds (row.removeFromLeft (22));
            row.removeFromLeft (12); rampHint.setBounds (row);
        }
        r.removeFromTop (10);
        auto bottom = r.removeFromBottom (32);
        cancelButton.setBounds (bottom.removeFromRight (110)); bottom.removeFromRight (8); writeButton.setBounds (bottom.removeFromRight (170));
        r.removeFromBottom (8);
        statusLabel.setBounds (r.removeFromBottom (38));
        {
            auto row = r.removeFromBottom (30);
            playOrig.setBounds (row.removeFromLeft (124)); row.removeFromLeft (6); playNew.setBounds (row.removeFromLeft (124)); row.removeFromLeft (6);
            stopButton.setBounds (row.removeFromLeft (80)); row.removeFromLeft (18);
            prerollCap.setBounds (row.removeFromLeft (70)); preroll.setBounds (row.removeFromLeft (70).reduced (0, 2)); prerollAfter.setBounds (row.removeFromLeft (60));
        }
        r.removeFromBottom (8);
        {
            auto row = r.removeFromBottom (30);
            makeButton.setBounds (row.removeFromLeft (180)); row.removeFromLeft (10); bar.setBounds (row.reduced (0, 4));
        }
        r.removeFromBottom (10);
        curve.setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey) { if (spaceIsForRecording()) return true; if (app.isPlaying()) stop(); else playRetimed(); return true; }
        if (k.getTextCharacter() == 'o' || k.getTextCharacter() == 'O') { playOriginal(); return true; }
        return false;
    }

private:
    enum class Then { nothing, play, write };
    struct Base
    {
        std::vector<EditRegion> regions, overdubs; std::vector<AutoLane> lanes; double fixIn = -1.0, fixOut = -1.0; int insertIndex = -1;
        void restore (EditDef& e) const { e.regions = regions; e.overdubs = overdubs; e.lanes = lanes; e.fixIn = fixIn; e.fixOut = fixOut; e.insertIndex = insertIndex; }
    };
    struct Made { retime::Plan plan; juce::String key; };

    EditDef* edit() const { return app.project.findEdit (editId); }
    static juce::String keyOf (const retime::Request& q) { return juce::String (q.S, 9) + "|" + juce::String (q.rampIn, 6) + "|" + juce::String (q.rampOut, 6); }
    bool fresh() const
    {
        retime::Request q; juce::String why, note;
        return made != nullptr && settings (q, why, note) && keyOf (q) == made->key;
    }

    /** What the window's fields ask for, as a request; false + why when they cannot be used. */
    bool settings (retime::Request& q, juce::String& why, juce::String& note) const
    {
        const double L = (double) (t1 - t0);
        double target = L;
        if (asPercent.getToggleState())
        {
            const auto t = percentEdit.getText().trim().trimCharactersAtEnd ("% ").replaceCharacter (',', '.');
            if (t.isEmpty() || ! t.containsOnly ("0123456789.")) { why = "Type the new length as a percentage of the selected length: 105 is 5 % longer (slower)."; return false; }
            target = L * t.getDoubleValue() / 100.0;
        }
        else
        {
            timefmt::Format f; f.mode = timefmt::Mode::minSec;
            const auto s = timefmt::parse (f, lengthEdit.getText());
            if (! s.has_value() || *s <= 0.0) { why = "Type the new length as minutes:seconds.milliseconds (or seconds)."; return false; }
            target = *s * sr;
        }
        if (target < L * 0.5 || target > L * 2.0) { why = "The new length has to be between half and twice the selected length."; return false; }
        double rIn = 0.0, rOut = 0.0;
        if (! req.wholePiece)
        {
            auto secs = [] (const juce::TextEditor& t, double& out)
            {
                const auto s = t.getText().trim().replaceCharacter (',', '.');
                if (s.isEmpty()) { out = 0.0; return true; }
                if (! s.containsOnly ("0123456789.")) return false;
                out = s.getDoubleValue(); return true;
            };
            if (! secs (rampInEdit, rIn) || ! secs (rampOutEdit, rOut)) { why = "Type the ramps in seconds (0 = at once)."; return false; }
            if ((rIn + rOut) * sr > L) { const double k = L / ((rIn + rOut) * sr); rIn *= k; rOut *= k; note = "The ramps were longer than the part, so they are shortened to fit (" + juce::String (rIn, 2) + " s and " + juce::String (rOut, 2) + " s)."; }
        }
        const double S = req.wholePiece ? target / L : retime::TimeMap::solve (L, target, rIn * sr, rOut * sr);
        if (! (S >= retime::kStretchMin && S <= retime::kStretchMax)) { why = "With ramps that long, the speed in the middle would have to change by more than half: shorten the ramps or make the change smaller."; return false; }
        if (std::abs (target - L) < 0.001 * sr) { why = "Set the new length, as a length or as a percentage."; return false; }
        q = req; q.S = S; q.rampIn = rIn; q.rampOut = rOut;
        q.label = juce::String (100.0 * target / L, 1) + "%";
        return true;
    }

    void refresh()
    {
        if (refreshing) return;
        const juce::ScopedValueSetter<bool> busy (refreshing, true);
        const double L = (double) (t1 - t0);
        lengthEdit.setColour (juce::TextEditor::textColourId, asPercent.getToggleState() ? theme::dimText : theme::text);
        percentEdit.setColour (juce::TextEditor::textColourId, asPercent.getToggleState() ? theme::text : theme::dimText);
        lengthEdit.applyColourToAllText (lengthEdit.findColour (juce::TextEditor::textColourId));
        percentEdit.applyColourToAllText (percentEdit.findColour (juce::TextEditor::textColourId));
        retime::Request q; juce::String why, note;
        const bool ok = settings (q, why, note);
        if (ok)
        {
            const double target = req.wholePiece ? L * q.S : L + (q.S - 1.0) * (L - 0.5 * (q.rampIn + q.rampOut) * sr);
            if (asPercent.getToggleState()) lengthEdit.setText (clockOf (target / sr), false);
            else percentEdit.setText (juce::String (100.0 * target / L, 2), false);
            const double pct = 100.0 * target / L;
            juce::String t;
            t << juce::String (std::abs (pct - 100.0), 2) << " % " << (pct > 100.0 ? "longer, slower" : "shorter, faster") << ":  speed " << juce::String (100.0 * L / target, 2) << " %";
            if (! req.wholePiece && (q.rampIn > 0.0 || q.rampOut > 0.0)) t << "  (" << juce::String (100.0 / q.S, 2) << " % between the ramps)";
            effectLabel.setText (t, juce::dontSendNotification);
            effectLabel.setColour (juce::Label::textColourId, theme::text);
        }
        else
        {
            effectLabel.setText (why, juce::dontSendNotification);
            effectLabel.setColour (juce::Label::textColourId, theme::warn);
        }
        curve.valid = ok;
        curve.map = { (double) t0, (double) t1, ok ? q.S : 1.0, ok ? q.rampIn * sr : 0.0, ok ? q.rampOut * sr : 0.0, false };
        curve.repaint();
        const juce::String notes = (baseNote + (note.isNotEmpty() ? " " + note : juce::String()) + (madeNote.isNotEmpty() ? " " + madeNote : juce::String())).trim();
        if (noteLabel.getText() != notes) { noteLabel.setText (notes, juce::dontSendNotification); resized(); repaint(); }
        makeButton.setButtonText (running ? "Stop making" : "Make retimed audio");
        makeButton.setEnabled (running || ok);
        playNew.setEnabled (ok);
        writeButton.setEnabled (ok && ! running);
        if (! running && made != nullptr && ! fresh()) setStatus ("The settings have changed: Make (or Play retimed) makes the retimed audio again.", true);
    }

    /** The length field shows a tidy time, the percentage two decimals, the ramps a plain number - once typing is finished. */
    void tidyFields()
    {
        retime::Request q; juce::String why, note;
        if (! settings (q, why, note)) return;
        const double L = (double) (t1 - t0), target = req.wholePiece ? L * q.S : L + (q.S - 1.0) * (L - 0.5 * (q.rampIn + q.rampOut) * sr);
        const juce::ScopedValueSetter<bool> busy (refreshing, true);
        lengthEdit.setText (clockOf (target / sr), false);
        percentEdit.setText (juce::String (100.0 * target / L, 2), false);
    }

    void setStatus (const juce::String& t, bool warn)
    {
        statusLabel.setText (t, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, warn ? theme::warn : theme::text);
    }

    // ---- making the retimed audio
    void make (Then after)
    {
        retime::Request q; juce::String why, note;
        if (! settings (q, why, note)) { setStatus (why, true); return; }
        if (running) { then = after; return; }
        app.stopPlayback(); takeOut(); discardMade();
        then = after;
        auto* e = edit();
        if (e == nullptr) { setStatus ("The edit is gone.", true); return; }
        auto snapshot = std::make_shared<EditDef> (*e);
        base.restore (*snapshot);
        const auto folder = app.project.audioFolder().getChildFile ("Fixes");
        auto result = std::make_shared<Made>(); result->key = keyOf (q);
        auto ltc = ltcTracks;
        if (job == nullptr) job = new AudioJob();
        running = true; progressValue = 0.0; bar.setVisible (true);
        setStatus ("Making the retimed audio of every track...", false);
        juce::Component::SafePointer<RetimerPanel> self (this);
        job->start ([snapshot, q, folder, ltc, result] (AudioJob& j) -> juce::String
        {
            juce::AudioFormatManager fm; td::registerAudioFormats (fm);
            std::map<juce::String, juce::int64> lengths;
            auto fileLen = [&] (const juce::File& f) -> juce::int64
            {
                const auto key = f.getFullPathName();
                if (auto it = lengths.find (key); it != lengths.end()) return it->second;
                auto r = audioops::openReader (fm, f);
                const juce::int64 n = r != nullptr ? r->lengthInSamples : -1;
                lengths[key] = n;
                return n;
            };
            auto isLtc = [&ltc] (const juce::Uuid& t) { return ltc.count (t) > 0; };
            auto quiet = [&] (const EditRegion& r, juce::int64 a, juce::int64 b) { return retime::quietestMoment (fm, r, a, b, isLtc); };
            result->plan = retime::makePlan (*snapshot, q, folder, fileLen, isLtc, quiet);
            if (! result->plan.ok()) return result->plan.error;
            folder.createDirectory();
            const int threads = juce::jlimit (1, 4, (int) std::thread::hardware_concurrency() - 1);
            return retime::renderStreams (result->plan, [&j] { return j.cancelled(); }, [&j] (float f) { j.setProgress (f); }, threads);
        },
        [self, result] (const juce::String& err)
        {
            if (self == nullptr) { deleteTried (filesOf (result->plan)); return; }       // (the window was closed meanwhile: nobody wants these files)
            self->jobDone (result, err);
        });
    }

    void jobDone (std::shared_ptr<Made> result, const juce::String& err)
    {
        running = false; bar.setVisible (false);
        if (err.isNotEmpty())
        {
            deleteTried (filesOf (result->plan));                              // (a stop that came after the last file was finished still leaves nothing behind)
            then = Then::nothing;
            setStatus (err == "Cancelled." ? "Stopped: nothing was made." : err, err != "Cancelled.");
            refresh();
            return;
        }
        made = result;
        madeNote = result->plan.note;
        refresh();
        if (! fresh())
        {
            then = Then::nothing;
            setStatus ("The settings were changed while it was being made: press Make retimed audio again.", true);
            return;
        }
        const auto pieces = result->plan.streams.size();
        setStatus ("Ready (" + juce::String ((int) pieces) + (pieces == 1 ? " piece" : " pieces") + " made again): Play retimed to listen, Play original to compare, then Write back to clip.", false);
        const auto after = then; then = Then::nothing;
        if (after == Then::play) playRetimed();
        else if (after == Then::write) writeBack();
    }

    void discardMade()
    {
        if (made == nullptr) return;
        auto files = filesOf (made->plan);
        const auto waves = app.project.projectFolder().getChildFile ("Waveforms");      // (their saved waveforms are named after the file's size and date: while it is still there)
        for (auto& f : files) if (f.existsAsFile()) for (auto* ext : { ".fwave2", ".fwcol" }) wavestore::fileFor (waves, f, ext).deleteFile();
        deleteTried (std::move (files));
        made.reset(); madeNote.clear();
    }

    // ---- the retimed audio in the edit (while it is listened to, and for good at Write back)
    void putIn()
    {
        auto* e = edit();
        if (e == nullptr || made == nullptr) return;
        base.restore (*e);
        retime::applyPlan (*e, made->plan);
        hold = true;
        app.project.changed();
    }
    void takeOut()
    {
        if (! hold) return;
        if (auto* e = edit()) { base.restore (*e); app.project.changed(); }
        hold = false;
    }

    double prerollSeconds() const { return (double) juce::jmax (1, preroll.getSelectedId()); }
    void playOriginal()
    {
        app.stopPlayback(); takeOut();
        const auto err = app.playEdit (editId, juce::jmax (0.0, (double) t0 / sr - prerollSeconds()), (double) t1 / sr + 2.0);
        if (err.isNotEmpty()) setStatus (err, true); else playingWhat = 1;
    }
    void playRetimed()
    {
        if (! fresh()) { make (Then::play); return; }
        app.stopPlayback();
        putIn();
        const auto err = app.playEdit (editId, juce::jmax (0.0, (double) t0 / sr - prerollSeconds()), (double) made->plan.newSampleAt (t1) / sr + 2.0);
        if (err.isNotEmpty()) { setStatus (err, true); takeOut(); } else playingWhat = 2;
    }
    void stop() { app.stopPlayback(); takeOut(); playingWhat = 0; }

    void writeBack()
    {
        if (! fresh()) { make (Then::write); return; }
        app.stopPlayback();
        if (! hold) putIn();
        accepted = true;
        hold = false;                                                     // (it is the edit now: the automatic save may write it)
        app.armFixUndo ("ReTimEr", [a = &app, id = editId, old = base]
        {
            if (auto* e = a->project.findEdit (id)) { old.restore (*e); a->project.changed(); }
        }, true, editId);
        app.project.changed();
        app.setNotice ("ReTimEr: " + (made != nullptr ? made->plan.retimed.size() == 1 ? juce::String ("1 piece") : juce::String ((int) made->plan.retimed.size()) + " pieces" : juce::String()) + " retimed. Undo fix takes it off again.");
        closeMe();
    }

    void closeMe()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    }

    void timerCallback() override
    {
        if (running) progressValue = (double) job->progress();
        if (hold && ! app.isPlaying()) takeOut();                       // (it finished playing by itself)
        const bool playing = app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId;
        playOrig.setToggleState (playing && playingWhat == 1, juce::dontSendNotification);
        playNew.setToggleState (playing && playingWhat == 2, juce::dontSendNotification);
    }

    AppContext& app;
    juce::Uuid editId;
    retime::Request req;
    juce::int64 t0, t1;
    double sr = 48000.0;
    juce::String baseNote, madeNote;
    Base base;
    std::shared_ptr<Made> made;
    fixtools::PreviewApplied hold;                                       // while the retimed audio is in the edit only to be listened to, the automatic save waits (as every fix window: 3.1.7)
    bool accepted = false, running = false, refreshing = false;
    int playingWhat = 0, sepY = 0;
    Then then = Then::nothing;
    AudioJob* job = nullptr;                                                  // (handed back with release(): it may outlive the window)
    double progressValue = 0.0;
    std::set<juce::Uuid> ltcTracks;

    juce::Label whatLabel, noteLabel, selCap, selValue, newCap, effectLabel, rampInCap, rampOutCap, rampHint, secIn, secOut, pctSign, prerollCap, prerollAfter, statusLabel;
    juce::ToggleButton asLength { "as a length" }, asPercent { "as a percentage" };
    juce::TextEditor lengthEdit, percentEdit, rampInEdit, rampOutEdit;
    SpeedCurve curve;
    juce::ComboBox preroll;
    juce::TextButton makeButton { "Make retimed audio" }, playOrig { "Play original" }, playNew { "Play retimed" }, stopButton { "Stop" },
                     writeButton { "Write back to clip" }, cancelButton { "Cancel" };
    juce::ProgressBar bar;
};
} // namespace

void openRetimer (AppContext& app, const juce::Uuid& editId, const juce::Uuid& selectedPiece, juce::Component* parent)
{
    auto* e = app.project.findEdit (editId);
    if (e == nullptr) return;
    if (app.engine.isRecording()) { say ("Stop recording first."); return; }
    if (e->regions.empty()) { say ("The edit has no pieces to retime."); return; }
    const double sr = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    retime::Request q;
    juce::int64 t0 = 0, t1 = 0;
    juce::String what, note;
    auto fmt = [sr] (juce::int64 s) { return clockOf ((double) s / sr); };
    if (e->fixIn >= 0.0 && e->fixOut > e->fixIn)
    {
        q.from = (juce::int64) std::llround (e->fixIn * sr); q.to = (juce::int64) std::llround (e->fixOut * sr);
        const auto s = retime::snapSpan (*e, q.from, q.to, &note);
        t0 = s.first; t1 = juce::jmin (s.second, e->lengthSamples());
        what = "Marks 1 - 2: " + fmt (t0) + " to " + fmt (t1) + " - the music between the marks is retimed on every track, and everything after mark 2 moves along.";
        if (! e->fixTracks.empty()) note = (note + " All the tracks are retimed together (the marks' choice of tracks does not apply here).").trim();
    }
    else if (! selectedPiece.isNull() && e->indexOf (selectedPiece) >= 0)
    {
        q.wholePiece = true; q.piece = selectedPiece;
        const auto run = retime::wholeRun (*e, selectedPiece);
        t0 = e->regions[(size_t) run.first].startSample; t1 = e->regions[(size_t) run.second].endSample();
        const auto name = e->regions[(size_t) run.first].takeName.upToFirstOccurrenceOf (" (", false, false);
        what = "Whole piece: \"" + name + "\", " + fmt (t0) + " to " + fmt (t1) + ", evenly - with 20 s of its take either side, so its edit points can still be "
               "trimmed. The pieces after it move along.";
    }
    else
    {
        say ((! selectedPiece.isNull() && e->isOverdub (selectedPiece))
                 ? "An overdub is not retimed on its own: mark the part with keys 1 and 2 (the overdub and everything under it are retimed together)."
                 : "Click a piece to retime it whole (evenly, with 20 s of its take either side), or mark the part to retime with keys 1 and 2 (with ramps in and out).");
        return;
    }
    if (t1 - t0 < (juce::int64) (0.25 * sr)) { say ("The part is too short to retime (at least a quarter of a second)."); return; }
    bool hasLtc = false;
    for (auto& r : e->regions)
        if (r.endSample() > t0 && r.startSample < t1)
            for (auto& f : r.files) if (auto* t = app.project.findTrack (f.trackId)) hasLtc = hasLtc || t->isLtc;
    if (hasLtc) note = (note + " The LTC (timecode) track is not stretched: the retimed part has no timecode.").trim();

    fixtools::launchDialog (std::make_unique<RetimerPanel> (app, editId, q, t0, t1, what, note), "ReTimEr", parent);     // (the same dialog as the other process windows: 3.1.7)
}
} // namespace td

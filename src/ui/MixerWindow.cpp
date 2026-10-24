#include "MixerWindow.h"
#include "ColourPicker.h"
#include "PluginManager.h"
#include "ValueEntry.h"
#include "LowCutPanel.h"

namespace td
{
namespace
{
constexpr int kStripW = 90;                                        // the usual strip width (level 3 of the six below)
/** The width of the mixer strips: six steps, chosen with - / + in the METERS strip (shared by all the mixers, remembered on this computer). */
constexpr int kStripWidths[] = { 66, 74, 82, 90, 104, 118 };
int& stripWidthLevel() { static int level = 3; return level; }
int& stripWidthGen()   { static int gen = 0; return gen; }        // goes up when the width changes, so every open mixer lays itself out again
int stripWidth() { return kStripWidths[juce::jlimit (0, 5, stripWidthLevel())]; }
constexpr int kBandH = 16, kPlateH = 26, kInputRowH = 19, kSlotH = 16, kSendCellH = 50, kPanRowH = 30, kMSH = 24, kFaderH = 280, kFaderMin = 110, kOutRowH = 20, kSecH = 11, kToTrackH = 20, kLatH = 13;
constexpr int kLcRowH = 18;      // the LC key's row (audio tracks and Int Buses), between the inputs and the inserts

/** A small recessed read-out field (peak value, fader value, pan position). */
void styleField (juce::Label& l, float size = 10.0f, bool bold = true)
{
    const auto& t = mixTheme();
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::backgroundColourId, t.field);
    l.setColour (juce::Label::outlineColourId, t.line);
    l.setColour (juce::Label::textColourId, t.text);
    l.setBorderSize (juce::BorderSize<int> (0, 1, 0, 1));
    l.setMinimumHorizontalScale (0.8f);
    l.setInterceptsMouseClicks (false, false);
}

/** A tiny caption (no background). */
void styleCaption (juce::Label& l, const juce::String& text, juce::Justification j = juce::Justification::centredLeft)
{
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (8.5f, juce::Font::bold));
    l.setColour (juce::Label::textColourId, mixTheme().dim);
    l.setJustificationType (j);
    l.setMinimumHorizontalScale (0.8f);
    l.setInterceptsMouseClicks (false, false);
}

/** A small arrow button beside the fader reading: the down arrow on its left and the up arrow on its right move the fader 1 dB in the direction they
    point. Held down, it repeats. */
class NudgeButton : public juce::Button
{
public:
    explicit NudgeButton (bool pointsUp) : juce::Button (pointsUp ? "Fader up 1 dB" : "Fader down 1 dB"), up (pointsUp)
    {
        setWantsKeyboardFocus (false);
        setTriggeredOnMouseDown (true);                    // steps at once on the press (and then repeats while held), so nothing extra happens on release
        setRepeatSpeed (450, 90);
        setTooltip (pointsUp ? "Fader up 1 dB (hold to keep going)" : "Fader down 1 dB (hold to keep going)");
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = mixTheme();
        const auto r = getLocalBounds().toFloat();
        g.setColour (down ? t.line : over ? t.soft : t.field);  g.fillRoundedRectangle (r, 2.0f);
        g.setColour (t.line);                                   g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        const auto c = r.getCentre();
        const float w = juce::jmin (r.getWidth() * 0.55f, 7.0f), h = w * 0.75f;
        juce::Path p;
        if (up) p.addTriangle (c.x - w * 0.5f, c.y + h * 0.5f, c.x + w * 0.5f, c.y + h * 0.5f, c.x, c.y - h * 0.5f);
        else    p.addTriangle (c.x - w * 0.5f, c.y - h * 0.5f, c.x + w * 0.5f, c.y - h * 0.5f, c.x, c.y + h * 0.5f);
        g.setColour (t.text);  g.fillPath (p);
    }
private:
    bool up;
};

/** Vertical fader with a finely divided dB scale on its left, a stereo peak meter on its right (on the SAME scale, so the
    markings line up), the held peak above and the fader value below (with a small down arrow on its left and up arrow on its right: 1 dB a press). */
class FaderBlock : public juce::Component
{
public:
    explicit FaderBlock (std::function<void (float)> onChange, float initialDb) : changed (std::move (onChange))
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (-80.0, 12.0, 0.1);
        slider.setSkewFactorFromMidPoint (-20.0);
        slider.setSliderSnapsToMousePosition (false);          // grab the cap; clicking the slot does not make it jump
        slider.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::ctrlModifier);       // double-click (or Ctrl + click) the cap = back to 0 dB; double-click the number under it to type a level
        slider.getProperties().set ("fader", true);
        slider.setValue (initialDb <= -99.0f ? -80.0 : (double) initialDb, juce::dontSendNotification);
        slider.onValueChange = [this] { const float db = (float) slider.getValue(); updateLabel(); changed (db <= -79.5f ? -100.0f : db); };
        meterL.useFaderScale(); meterR.useFaderScale();
        addAndMakeVisible (slider); addAndMakeVisible (meterL); addAndMakeVisible (meterR);
        addAndMakeVisible (label); addAndMakeVisible (peakLabel);
        addAndMakeVisible (downBtn); addAndMakeVisible (upBtn);
        downBtn.onClick = [this] { nudge (-1.0); };
        upBtn.onClick   = [this] { nudge (+1.0); };
        styleField (label, 10.5f);
        styleField (peakLabel, 9.5f);
        peakLabel.setTooltip ("Highest peak so far. Held for 10 seconds, or until 'Reset peaks' if it went above -2 dBFS.");
        label.setTooltip ("Fader level in dB. Double-click here to type a level. (Double-click the fader cap to go back to 0 dB.)");
        labelClick = std::make_unique<valueentry::ReadoutClick> (label, true, [this] { askValue(); });
        updateLabel();
    }
    /** The cap of the fader takes the channel's colour. */
    void setAccent (juce::Colour c)
    {
        if (c == accent) return;
        accent = c;
        slider.getProperties().set ("cap", (juce::int64) c.getARGB());
        slider.repaint();
    }
    void setDb (float db)
    {
        const double v = db <= -99.0f ? -80.0 : (double) db;
        if (std::abs (slider.getValue() - v) > 0.05) { slider.setValue (v, juce::dontSendNotification); updateLabel(); }
    }
    bool isBeingDragged() const { return slider.isMouseButtonDown(); }
    void noModeSwap() { slider.setVelocityModeParameters (1.0, 1, 0.0, false); }
    void pushPeaks (float l, float r, float rmsL, float rmsR)
    {
        meterL.setLevel (l, rmsL); meterR.setLevel (r, rmsR);
        const float pk = juce::jmax (meterL.getHoldDb(), meterR.getHoldDb());
        const bool latched = meterL.isLatched() || meterR.isLatched();
        peakLabel.setText (pk <= -99.0f ? juce::String() : juce::String (pk, 1), juce::dontSendNotification);
        peakLabel.setColour (juce::Label::textColourId, latched ? juce::Colour (0xffe03a2c) : mixTheme().text);
    }
    void resetPeaks() { meterL.resetPeak(); meterR.resetPeak(); peakLabel.setText ({}, juce::dontSendNotification); }
    /** The VCAs that control this strip: what they add (dB, all of them together), whether one of them is muted, and the colour to draw it in.
        active false: no VCA controls it. A line beside the slot then runs from the fader cap to where the level really is, with a faint cap there. */
    void setVcaGhost (bool active, float addDb, bool muted, juce::Colour c)
    {
        if (active == ghostOn && std::abs (addDb - ghostDb) < 0.05f && muted == ghostMuted && c == ghostColour) return;
        ghostOn = active; ghostDb = addDb; ghostMuted = muted; ghostColour = c;
        repaint();
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (! ghostOn || (! ghostMuted && std::abs (ghostDb) < 0.05f)) return;
        const float top = (float) travelTop, bottom = (float) travelBottom, span = bottom - top;
        auto yOf = [&] (float db) { return bottom - span * LevelMeter::faderFrac (juce::jlimit (-80.0f, 12.0f, db)); };
        const float faderDb = (float) slider.getValue();
        const float realDb = ghostMuted || faderDb <= -79.5f ? -80.0f : faderDb + ghostDb;
        const float y0 = yOf (faderDb), y1 = yOf (realDb);
        const float x = (float) sliderRect.getRight() + (float) ghostX;                 // in the gap between the slot and the meters
        g.setColour (ghostColour.withAlpha (0.95f));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, juce::jmin (y0, y1), 2.0f, juce::jmax (1.0f, std::abs (y1 - y0))));
        g.fillRect (juce::Rectangle<float> (x - 3.0f, y1 - 1.0f, 5.0f, 2.0f));          // where it ends: the real level
        const float slotX = (float) sliderRect.getCentreX();                           // a faint ghost of the cap at the real level
        g.setColour (ghostColour.withAlpha (0.55f));
        g.fillRoundedRectangle (juce::Rectangle<float> (slotX - (float) sliderRect.getWidth() * 0.32f, y1 - 2.0f, (float) sliderRect.getWidth() * 0.64f, 4.0f), 1.5f);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = mixTheme();
        const float top = (float) travelTop, bottom = (float) travelBottom, span = bottom - top;
        auto yOf = [&] (float db) { return bottom - span * LevelMeter::faderFrac (db); };
        g.setFont (juce::FontOptions (scaleRect.getWidth() < 19 ? 7.0f : 8.5f));          // (smaller numbers in a narrow strip, so "+12" still fits)
        for (int db : LevelMeter::faderScaleDbs())                                       // numbered marks
        {
            const int y = (int) std::round (yOf ((float) db));
            g.setColour (t.dim);  g.drawText (db > 0 ? "+" + juce::String (db) : juce::String (db), scaleRect.getX(), y - 5, scaleRect.getWidth() - 5, 10, juce::Justification::centredRight, false);
            g.setColour (t.line); g.fillRect (scaleRect.getRight() - 4, y, 5, 1);
        }
        g.setColour (t.dim);  g.drawText ("inf", scaleRect.getX(), travelBottom - 5, scaleRect.getWidth() - 5, 10, juce::Justification::centredRight, false);
        g.setColour (t.line); g.fillRect (scaleRect.getRight() - 4, travelBottom, 5, 1);
        g.setColour (t.soft);                                                             // the fine divisions in between
        for (int db = 12; db >= -12; db -= 3) if (db % 6 != 0) g.fillRect (scaleRect.getRight() - 2, (int) std::round (yOf ((float) db)), 3, 1);
        for (int db : { 9, 3, -3, -9, -15, -25, -35, -50, -70 }) g.fillRect (scaleRect.getRight() - 2, (int) std::round (yOf ((float) db)), 3, 1);
        for (int db = 11; db >= -11; --db) if (db % 3 != 0) g.fillRect (scaleRect.getRight() - 1, (int) std::round (yOf ((float) db)), 2, 1);
        // the unity mark runs across the slot
        g.setColour (t.line); g.fillRect (sliderRect.getX() - 1, (int) std::round (yOf (0.0f)), 3, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        {   // the reading with its two arrow buttons (the same height as the reading), the whole row centred
            auto row = r.removeFromBottom (15);
            const int bw = row.getWidth() < 72 ? 12 : 14, gp = 2;
            row = row.withSizeKeepingCentre (juce::jmin (row.getWidth(), 62 + 2 * (bw + gp)), 15);
            downBtn.setBounds (row.removeFromLeft (bw));   row.removeFromLeft (gp);
            upBtn.setBounds (row.removeFromRight (bw));    row.removeFromRight (gp);
            label.setBounds (row);
        }
        peakLabel.setBounds (r.removeFromTop (14).withSizeKeepingCentre (62, 14));
        r.removeFromTop (3); r.removeFromBottom (3);
        int scaleW = 20, sliderW = 28, between = 5;
        const int meterW = 8, gap = 1;
        {   // a narrow strip: the gap, the scale and the slot give a little each, so everything still fits
            int over = scaleW + sliderW + between + meterW * 2 + gap - r.getWidth();
            auto give = [&over] (int& v, int least) { const int d = juce::jlimit (0, juce::jmax (0, v - least), over); v -= d; over -= d; };
            give (between, 2); give (sliderW, 22); give (scaleW, 16);
        }
        const int total = scaleW + sliderW + between + meterW * 2 + gap;
        const int x0 = r.getX() + juce::jmax (0, (r.getWidth() - total) / 2);
        scaleRect  = { x0, r.getY(), scaleW, r.getHeight() };
        sliderRect = { x0 + scaleW, r.getY(), sliderW, r.getHeight() };
        slider.setBounds (sliderRect);
        travelTop = r.getY() + MixerLookAndFeel::kCapHalf; travelBottom = r.getBottom() - MixerLookAndFeel::kCapHalf;
        const int mx = x0 + scaleW + sliderW + between;
        // the meters' bars run exactly over the fader's travel; the well's edge and the clip light sit above it, the edge below
        const int above = 1 + LevelMeter::kClipH + 1;
        meterL.setBounds (mx, travelTop - above, meterW, travelBottom - travelTop + above + 1);
        meterR.setBounds (mx + meterW + gap, travelTop - above, meterW, travelBottom - travelTop + above + 1);
        ghostX = juce::jmax (1, between / 2);
        syncDrag();
    }
    void lookAndFeelChanged() override { syncDrag(); }
    void parentHierarchyChanged() override { syncDrag(); }
private:
    /** The mouse has to move the cap exactly as far as the cap travels: a slider that does not jump to the mouse otherwise counts a fixed 250 pixels for the whole
        way, so on a tall fader the cap fell behind the mouse (and on a short one it ran ahead). */
    void syncDrag()
    {
        const int travel = slider.getLookAndFeel().getSliderLayout (slider).sliderBounds.getHeight();
        slider.setMouseDragSensitivity (juce::jmax (40, travel));
    }
    /** The arrow buttons: the fader moves by 'deltaDb' from where it is, through the same path as the mouse (so linked strips, the project and the
        mixer's sound all follow). All the way down is -inf (off) and stays there; one step up from it is -79 dB. */
    void nudge (double deltaDb)
    {
        const double v = slider.getValue();
        if (deltaDb < 0.0 && v <= -79.95) return;
        slider.setValue (juce::jlimit (-80.0, 12.0, v + deltaDb), juce::sendNotificationSync);
    }
    void askValue()
    {
        const double v = slider.getValue();
        juce::Component::SafePointer<FaderBlock> safe (this);
        valueentry::ask (slider, "Fader level in dB (or -inf)", v <= -79.5 ? juce::String ("-inf") : juce::String (v, 1), [safe] (const juce::String& t)
        {
            double db = 0.0;
            if (safe != nullptr && valueentry::parseDb (t, -80.0, -80.0, 12.0, db)) safe->slider.setValue (db, juce::sendNotificationSync);
        });
    }
    void updateLabel()
    {
        const double v = slider.getValue();
        label.setText (v <= -79.5 ? juce::String ("-inf") : juce::String (v, 1), juce::dontSendNotification);
    }
    valueentry::TypedSlider slider; LevelMeter meterL, meterR; juce::Label label, peakLabel;
    NudgeButton downBtn { false }, upBtn { true };
    std::unique_ptr<valueentry::ReadoutClick> labelClick;
    std::function<void (float)> changed;
    juce::Colour accent { 0 };
    juce::Rectangle<int> scaleRect, sliderRect; int travelTop = 0, travelBottom = 0;
    bool ghostOn = false, ghostMuted = false; float ghostDb = 0.0f; juce::Colour ghostColour; int ghostX = 2;
};

/** What the VCAs in 'mask' do to a strip of mixer m, for the fader's ghost line: their gains added up (dB), whether one is muted, and the colour of the first. */
static void vcaGhostFor (AppContext& app, const MixerState& m, juce::uint32 mask, float& addDb, bool& muted, juce::Colour& colour)
{
    addDb = 0.0f; muted = false; colour = chan::automaticFor (NodeKind::Vca);
    bool first = true;
    for (int i = 0; i < kMaxVcas; ++i)
        if (mask & (1u << i))
        {
            if (m.vcas[i].mute.get()) muted = true;
            addDb += juce::jmax (-100.0f, m.vcas[i].gainDb.get());
            if (first) { if (const auto* vd = app.project.vcaByNumber (i + 1); vd != nullptr && vd->colour != 0) colour = juce::Colour (vd->colour); first = false; }
        }
}

/** A mixer strip that can be selected (click its name plate; Shift + click another for every strip in between, Ctrl + click one more or one less)
    and, while Shift + Alt are held, moved together with the other selected strips. */
class GangMember
{
public:
    virtual ~GangMember() = default;
    virtual juce::Uuid gangId() const = 0;
    virtual StripState& gangState() = 0;
    virtual int gangChannels() const = 0;
    virtual void gangRefresh() = 0;                   // show what the model now says (pans, M / S, sends, inserts)
    virtual juce::Component& gangComponent() = 0;
};

/** The selected strips of one mixer window, and whether they are linked right now: only while Shift + Alt are held, and only for a strip that is
    one of two or more selected. Faders, sends and pans then move by the same amount on every selected strip (each keeps its own offset); M, S, a
    send's MUTE / PRE / Follow mute are set the same; a plug-in inserted, removed or bypassed is on every one of them. Let go of the keys and each
    strip is on its own again (the selection stays). */
struct StripGang
{
    std::vector<juce::Uuid> selected;
    juce::Uuid anchor;
    std::vector<GangMember*> members;                 // the audio-track strips, left to right
    bool shownLinked = false;

    bool isSelected (const juce::Uuid& id) const { return std::find (selected.begin(), selected.end(), id) != selected.end(); }
    static bool keysHeld()
    {
        const auto m = juce::ModifierKeys::getCurrentModifiersRealtime();
        return m.isShiftDown() && m.isAltDown() && ! m.isCtrlDown() && ! m.isCommandDown();
    }
    bool linked (const juce::Uuid& id) const { return selected.size() > 1 && isSelected (id) && keysHeld(); }
    /** fn on every OTHER selected strip, when 'id' is linked to them right now (nothing otherwise). */
    void forOthers (const juce::Uuid& id, const std::function<void (GangMember&)>& fn) const
    {
        if (! linked (id)) return;
        for (auto* m : std::vector<GangMember*> (members)) if (m->gangId() != id && isSelected (m->gangId())) fn (*m);
    }
    /** A click on a strip's name plate: on its own = select only this one (or nothing, if it was the only one); Shift = every strip from the last one
        clicked to this one; Ctrl = this one in or out of the selection. */
    void click (const juce::Uuid& id, const juce::ModifierKeys& mods)
    {
        const bool ctrl = mods.isCtrlDown() || mods.isCommandDown();
        int a = -1, b = -1;
        for (int i = 0; i < (int) members.size(); ++i) { if (members[(size_t) i]->gangId() == anchor) a = i; if (members[(size_t) i]->gangId() == id) b = i; }
        if (mods.isShiftDown() && a >= 0 && b >= 0)
        {
            if (! ctrl) selected.clear();
            for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i) if (! isSelected (members[(size_t) i]->gangId())) selected.push_back (members[(size_t) i]->gangId());
        }
        else if (ctrl)
        {
            if (isSelected (id)) selected.erase (std::remove (selected.begin(), selected.end(), id), selected.end());
            else selected.push_back (id);
            anchor = id;
        }
        else
        {
            if (! mods.isShiftDown() && selected.size() == 1 && selected.front() == id) selected.clear();
            else selected.assign (1, id);
            anchor = id;
        }
        repaintAll();
    }
    void selectAll() { selected.clear(); for (auto* m : members) selected.push_back (m->gangId()); repaintAll(); }
    void clear() { selected.clear(); repaintAll(); }
    void repaintAll() { for (auto* m : members) m->gangComponent().repaint(); }
    /** From the meter timer: the strips' outlines change when Shift + Alt go down or up. */
    void tick()
    {
        const bool now = selected.size() > 1 && keysHeld();
        if (now != shownLinked) { shownLinked = now; repaintAll(); }
    }
};

/** A send of 'src' moved from oldDb to newDb (-100 = off) on a linked strip: the same send of 'other' moves by as many dB (a send that is off counts
    as the lowest level, so it comes in from there). A send to an audio track is only moved on a strip that already sends to that track. */
static void nudgeSend (AppContext& app, MixerState& m, const juce::Uuid& other, const juce::Uuid& dest, float oldDb, float newDb, bool destIsTrack)
{
    if (other == dest) return;
    auto* l = app.project.sendsOf (m, other);
    auto* s = l != nullptr ? l->find (dest) : nullptr;
    if (destIsTrack && (s == nullptr || ! s->active())) return;
    auto fl = [] (float v) { return v <= kSendOffDb + 0.05f ? kSendOffDb : v; };
    const float cur = s != nullptr && s->active() ? s->gainDb.get() : kSendOffDb;
    const float nv = juce::jlimit (kSendOffDb, 6.0f, fl (cur) + (fl (newDb) - fl (oldDb)));
    app.setSendLevel (m, other, dest, nv <= kSendOffDb + 0.05f ? -100.0f : nv);
}

/** Linked strips must not change how a slider drags: JUCE switches a slider to its velocity mode while Ctrl / Alt is held. */
static void noModeSwap (juce::Slider& s) { s.setVelocityModeParameters (1.0, 1, 0.0, false); }

class SlotButtons;

/** One insert slot as drawn: the name (click it to open the plug-in straight away) and a little dot on the right (click it for the menu).
    Drag the name onto another slot, in this mixer or another one, to MOVE the plug-in with all its settings (two plug-ins swap places);
    hold Alt while dropping to put an exact COPY there instead, with all its current settings (Shift does the same). */
class SlotCell : public juce::Component, public juce::SettableTooltipClient
{
public:
    SlotCell (SlotButtons& o, int i) : owner (o), index (i) { setRepaintsOnMouseActivity (true); }
    void setLook (const juce::String& n, juce::Colour c, bool loadedNow) { name = n; fill = c; loaded = loadedNow; repaint(); }
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override { dragging = false; downOnDot = dotArea().contains (e.getPosition()); }
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    juce::Rectangle<int> dotArea() const { return getLocalBounds().removeFromRight (16); }
    /** Alt (or Shift) held while dragging: the plug-in is copied, not moved. */
    static bool isCopyDrag (const juce::ModifierKeys& m) { return m.isAltDown() || m.isShiftDown(); }
    SlotButtons& owner; int index;
    bool dropHighlight = false, dropCopy = false;
private:
    juce::String name; juce::Colour fill; bool loaded = false, dragging = false, downOnDot = false;
    SlotCell* hovered = nullptr;
};

/** A column of VST3 insert slots (four on a channel, two on a send). */
class SlotButtons : public juce::Component
{
public:
    SlotButtons (AppContext& a, InsertSlot* s, int n = kNumSlots) : app (a), slots (s), count (n)
    {
        for (int i = 0; i < count; ++i)
        {
            auto* b = cells.add (new SlotCell (*this, i));
            b->setTooltip ("Insert slot. Click the name to open the plug-in (an empty slot opens the list), click the dot for the menu (load, bypass, remove). "
                           "Drag the name onto another slot, in any mixer, to move the plug-in with its settings; hold Alt to drop an exact copy instead (all its settings).");
            addAndMakeVisible (b);
        }
        refresh();
    }
    void refresh()
    {
        const auto& t = mixTheme();
        for (int i = 0; i < count; ++i)
        {
            auto name = slots[i].getName();
            const bool loaded = name.isNotEmpty();
            const bool byp = slots[i].bypass.get();
            cells[i]->setLook (name, loaded ? (byp ? t.insertByp : t.insertOn) : t.field, loaded);
        }
    }
    void resized() override
    {
        auto r = getLocalBounds();
        const int h = r.getHeight() / count;
        for (auto* b : cells) b->setBounds (r.removeFromTop (h).reduced (1, 1));
    }
    bool slotLoaded (int i) const { return slots[i].isLoaded(); }
    /** A channel strip linked to others right now (Shift + Alt held, see StripGang): their insert slots, so a plug-in inserted, removed or bypassed
        here is on them too; and a function that shows the result on their strips. Empty: on its own. */
    std::function<std::vector<InsertSlot*>()> gangSlots;
    std::function<void()> gangRefresh;
    /** The slot of another strip a linked plug-in goes into: the same one if it is empty, else the first empty one (nothing is replaced); -1: full. */
    static int slotFor (InsertSlot* arr, int want)
    {
        if (! arr[want].isLoaded()) return want;
        for (int k = 0; k < kNumSlots; ++k) if (! arr[k].isLoaded()) return k;
        return -1;
    }
    /** Puts a fresh instance (made by 'make') into each linked strip; says so if some had no free slot. */
    void insertOnLinked (const std::vector<InsertSlot*>& others, int want, const std::function<std::unique_ptr<InsertProcessor>()>& make, bool bypassed)
    {
        if (others.empty()) return;
        // 'make' loads a plug-in, and one that shows a window while it loads (a licence, a trial notice) runs a modal loop in which this row of slots can be
        // rebuilt and deleted (the mixer's refresh sees the new plug-in): from here on nothing of 'this' is used, only these copies
        auto& a = app; const auto refreshGang = gangRefresh;
        int full = 0;
        const double sr = a.engine.getSampleRate(); const int blk = a.engine.getMaxBlock();
        for (auto* arr : others)
        {
            const int k = slotFor (arr, want);
            if (k < 0) { ++full; continue; }
            auto p = make();
            if (p == nullptr) { showError ("Plug-in", "Could not load the plug-in on every selected strip."); break; }
            if (arr[k].set (std::move (p), sr, blk, 2)) arr[k].bypass.set (bypassed);
        }
        a.project.markDirty(); a.engine.checkLatency();
        if (refreshGang) refreshGang();
        a.project.structure.sendChangeMessage();                                     // (a strip may need another slot row)
        if (full > 0) showError ("Plug-in", juce::String (full) + " of the selected strips had no free insert slot: the plug-in was not added there.");
    }
    /** The name was clicked: open the plug-in, or (empty slot) offer the list. */
    void openOrMenu (int i)
    {
        if (slots[i].isLoaded()) app.plugins.showEditor (slots[i].getProcessorUnsafe());
        else clicked (i);
    }
    void menu (int i) { clicked (i); }

    /** Move (or, with copy, duplicate) the plug-in in this column's slot 'from' to slot 'to' of 'target'. */
    void dropTo (int from, SlotButtons& target, int to, bool copy)
    {
        if (&target == this && from == to) return;
        auto& src = slots[from]; auto& dst = target.slots[to];
        auto* sp = src.getProcessorUnsafe();
        if (sp == nullptr) return;
        const double sr = app.engine.getSampleRate(); const int blk = app.engine.getMaxBlock();
        if (copy)
        {
            const auto desc = sp->getDescription(); const auto state = sp->getStateBase64();
            auto factory = app.plugins.makeFactory ([sr] { return sr; }, [blk] { return blk; });
            auto p = factory ? factory (desc, state) : nullptr;
            if (p == nullptr) { showError ("Plug-in", "Could not make a copy of " + sp->getName()); return; }
            app.plugins.closeEditorsFor (dst.getProcessorUnsafe());
            if (! dst.set (std::move (p), sr, blk, 2)) { showError ("Plug-in", stuckText (dst)); return; }
            dst.bypass.set (src.bypass.get());
        }
        else
        {
            app.plugins.closeEditorsFor (sp); app.plugins.closeEditorsFor (dst.getProcessorUnsafe());
            const bool bs = src.bypass.get(), bd = dst.bypass.get();
            auto pa = src.take();                                              // (null and still loaded: the slot could not be locked - a plug-in stuck in its process call)
            if (pa == nullptr && src.isLoaded()) { showError ("Plug-in", stuckText (src)); return; }
            auto pb = dst.take();                                              // the one that was there (if any) swaps places with it
            if (pb == nullptr && dst.isLoaded()) { src.set (std::move (pa), sr, blk, 2); src.bypass.set (bs); showError ("Plug-in", stuckText (dst)); return; }
            dst.set (std::move (pa), sr, blk, 2); dst.bypass.set (bs);
            if (pb != nullptr) { src.set (std::move (pb), sr, blk, 2); src.bypass.set (bd); }
            else src.bypass.set (false);
        }
        app.project.markDirty(); app.engine.checkLatency();
        refresh(); target.refresh();
        if (copy && target.gangSlots)                                          // dropped (as a copy) on a strip linked to others: a copy on each of them too
        {
            const auto desc = sp->getDescription(); const auto state = sp->getStateBase64(); const bool byp = src.bypass.get();
            auto others = target.gangSlots();
            others.erase (std::remove (others.begin(), others.end(), slots), others.end());                  // (not back onto the strip it came from)
            auto factory = app.plugins.makeFactory ([sr] { return sr; }, [blk] { return blk; });
            target.insertOnLinked (others, to, [factory, desc, state] { return factory ? factory (desc, state) : nullptr; }, byp);
        }
    }
    /** The message when a slot could not be locked within InsertSlot::kLockWaitMs (see InsertSlot). */
    static juce::String stuckText (const InsertSlot& s)
    {
        return (s.getName().isNotEmpty() ? s.getName() : juce::String ("The plug-in")) + " did not let go of its slot for " + juce::String (InsertSlot::kLockWaitMs / 1000)
               + " seconds (it may be stuck inside its process call). Nothing was changed; the log has the details.";
    }
    /** The slot cell under a screen position (any mixer window), or null. */
    static SlotCell* cellAt (juce::Point<int> screenPos)
    {
        for (auto* c = juce::Desktop::getInstance().findComponentAt (screenPos); c != nullptr; c = c->getParentComponent())
            if (auto* sc = dynamic_cast<SlotCell*> (c)) return sc;
        return nullptr;
    }
    AppContext& app;
private:
    void clicked (int i)
    {
        auto descs = std::make_shared<std::vector<juce::PluginDescription>>();
        juce::PopupMenu m;
        auto& slot = slots[i];
        const bool loaded = slot.isLoaded();
        if (loaded)
        {
            m.addItem (1, "Open editor");
            m.addItem (2, "Bypass", true, slot.bypass.get());
            m.addItem (3, "Remove");
            m.addSeparator();
            m.addSubMenu ("Replace with...", app.plugins.buildPluginMenu (1000, *descs));
        }
        else
        {
            if (app.plugins.getKnownPlugins().getNumTypes() == 0) m.addItem (-1, "No plug-ins known yet", false);
            else m.addSubMenu ("Insert VST3 plug-in", app.plugins.buildPluginMenu (1000, *descs));
        }
        m.addSeparator();
        m.addItem (9, app.plugins.isScanning() ? "Scanning..." : "Scan for VST3 plug-ins", ! app.plugins.isScanning());
        m.addItem (10, "Plug-ins: ignore one, try one again...");

        auto safe = juce::Component::SafePointer<SlotButtons> (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (cells[i]), [safe, i, descs] (int r)
        {
            if (safe == nullptr || r == 0) return;
            safe->handle (i, r, *descs);
        });
    }
    void handle (int i, int r, const std::vector<juce::PluginDescription>& descs)
    {
        auto& slot = slots[i];
        const auto linkedSlots = gangSlots ? gangSlots() : std::vector<InsertSlot*>();      // (Shift + Alt held as the menu item was chosen)
        const auto name = slot.getName();
        if (r == 1) { app.plugins.showEditor (slot.getProcessorUnsafe()); return; }
        if (r == 2)
        {
            const bool b = ! slot.bypass.get();
            slot.bypass.set (b);
            for (auto* arr : linkedSlots) if (arr[i].isLoaded() && arr[i].getName() == name) arr[i].bypass.set (b);     // the same plug-in in the same slot
            app.project.markDirty(); app.engine.checkLatency(); refresh(); if (! linkedSlots.empty() && gangRefresh) gangRefresh(); return;
        }
        if (r == 3)
        {
            app.plugins.closeEditorsFor (slot.getProcessorUnsafe());
            if (! slot.clear (app.engine.getSampleRate(), app.engine.getMaxBlock(), 2)) showError ("Plug-in", stuckText (slot));
            for (auto* arr : linkedSlots)
                if (arr[i].isLoaded() && arr[i].getName() == name)
                {
                    app.plugins.closeEditorsFor (arr[i].getProcessorUnsafe());
                    if (! arr[i].clear (app.engine.getSampleRate(), app.engine.getMaxBlock(), 2)) showError ("Plug-in", stuckText (arr[i]));
                }
            app.project.markDirty(); app.engine.checkLatency(); refresh(); if (! linkedSlots.empty() && gangRefresh) gangRefresh(); return;
        }
        if (r == 9)
        {
            auto safe = juce::Component::SafePointer<SlotButtons> (this);
            app.plugins.startScan ([safe] { if (safe != nullptr) safe->refresh(); });
            return;
        }
        if (r == 10) { showPluginManager (app); return; }
        if (r >= 1000 && (size_t) (r - 1000) < descs.size())
        {
            // A plug-in that shows a window while it loads runs a modal loop, in which this row of slots can be rebuilt and deleted: after a load only what
            // outlives the row is used ('a', the strip's own slot, the copies), and 'this' only after checking 'safe'.
            auto& a = app;
            juce::Component::SafePointer<SlotButtons> safe (this);
            const auto desc = descs[(size_t) (r - 1000)];
            juce::String err;
            auto p = a.plugins.create (desc, a.engine.getSampleRate(), a.engine.getMaxBlock(), err);
            if (p == nullptr) { showError ("Plug-in", "Could not load " + desc.name + ":\n" + err); return; }
            a.plugins.closeEditorsFor (slot.getProcessorUnsafe());
            if (! slot.set (std::move (p), a.engine.getSampleRate(), a.engine.getMaxBlock(), 2)) { showError ("Plug-in", stuckText (slot)); if (safe != nullptr) refresh(); return; }
            a.project.markDirty(); a.engine.checkLatency(); if (safe != nullptr) refresh();
            if (! linkedSlots.empty() && safe != nullptr)                              // linked strips: the same plug-in on each (its own instance)
            {
                const double sr = a.engine.getSampleRate(); const int blk = a.engine.getMaxBlock();
                insertOnLinked (linkedSlots, i, [&a, desc, sr, blk] { juce::String e2; return a.plugins.create (desc, sr, blk, e2); }, false);
            }
            a.plugins.showEditor (slot.getProcessorUnsafe());
        }
    }
    InsertSlot* slots;
    int count;
    juce::OwnedArray<SlotCell> cells;
};

void SlotCell::paint (juce::Graphics& g)
{
    const auto& t = mixTheme();
    auto r = getLocalBounds().toFloat();
    g.setColour (isMouseOver() && ! dragging ? fill.brighter (0.12f) : fill); g.fillRoundedRectangle (r, 3.0f);
    g.setColour (dropHighlight ? juce::Colour (dropCopy ? 0xff3fbf6a : 0xfff2c200) : t.line); g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, dropHighlight ? 2.0f : 1.0f);
    auto text = getLocalBounds().withTrimmedRight (16).reduced (4, 0);
    g.setColour (t.text); g.setFont (juce::FontOptions (11.0f));
    g.drawText (name, text, juce::Justification::centredLeft, true);
    const auto dot = dotArea().toFloat();
    const bool overDot = isMouseOver() && dotArea().contains (getMouseXYRelative());
    g.setColour (t.text.withAlpha (overDot ? 1.0f : loaded ? 0.6f : 0.35f));
    g.fillEllipse (dot.getCentreX() - 3.0f, dot.getCentreY() - 3.0f, 6.0f, 6.0f);
}

void SlotCell::mouseDrag (const juce::MouseEvent& e)
{
    if (downOnDot || ! loaded) return;
    if (! dragging && e.getDistanceFromDragStart() < 6) return;
    dragging = true;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    auto* now = SlotButtons::cellAt (e.getScreenPosition());
    if (now == this) now = nullptr;
    if (hovered != now) { if (hovered != nullptr) hovered->dropHighlight = false, hovered->repaint(); hovered = now; }
    if (hovered != nullptr) { hovered->dropHighlight = true; hovered->dropCopy = isCopyDrag (e.mods); hovered->repaint(); }
}

void SlotCell::mouseUp (const juce::MouseEvent& e)
{
    const bool wasDragging = dragging;
    dragging = false; setMouseCursor (juce::MouseCursor::NormalCursor);
    if (hovered != nullptr) { hovered->dropHighlight = false; hovered->repaint(); }
    auto* target = wasDragging ? SlotButtons::cellAt (e.getScreenPosition()) : nullptr;
    hovered = nullptr;
    if (wasDragging)
    {
        if (target != nullptr && target != this) owner.dropTo (index, target->owner, target->index, isCopyDrag (e.mods));
        return;
    }
    if (e.mods.isPopupMenu() || (downOnDot && dotArea().contains (e.getPosition()))) { owner.menu (index); return; }
    if (getLocalBounds().contains (e.getPosition())) owner.openOrMenu (index);
}

/** Whether the channel a send comes from has its MUTE on (an audio track or an Int Bus of this mixer). */
bool channelMuteOn (AppContext& app, MixerState& m, const juce::Uuid& src)
{
    switch (app.project.kindOf (src))
    {
        case NodeKind::Track:  return m.stripFor (src)->mute.get();
        case NodeKind::IntBus: return m.busFor (src)->mute.get();
        default: break;
    }
    return false;
}

/** A send's own MUTE (in its panel): lit red while the send is muted; a red outline while the channel's MUTE silences it (Follow mute ticked and the
    channel muted), so it is plain why the send is silent although its own button is off. */
class SendMuteButton : public juce::Button
{
public:
    SendMuteButton() : juce::Button ("MUTE") { setClickingTogglesState (true); }
    void setFollowMuted (bool b) { if (b != followMuted) { followMuted = b; repaint(); } }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const juce::Colour red (0xffe0312b);
        const bool on = getToggleState();
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        auto base = on ? red : theme::button;
        if (down) base = base.darker (0.2f); else if (over) base = base.brighter (0.12f);
        g.setColour (base); g.fillRoundedRectangle (b, 4.0f);
        if (followMuted && ! on) { g.setColour (red); g.drawRoundedRectangle (b.reduced (1.0f), 4.0f, 2.0f); }
        else { g.setColour (on ? red.darker (0.5f) : theme::border); g.drawRoundedRectangle (b, 4.0f, 1.0f); }
        g.setColour (on ? juce::Colours::white : followMuted ? red.brighter (0.25f) : theme::inkOn (theme::button));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("MUTE", getLocalBounds(), juce::Justification::centred, false);
    }
private:
    bool followMuted = false;
};

juce::TextButton* makeToggle (juce::Component& parent, juce::OwnedArray<juce::TextButton>& list, const juce::String& text, juce::Colour on)
{
    auto* b = list.add (new juce::TextButton (text));
    b->setClickingTogglesState (true);
    b->setColour (juce::TextButton::buttonOnColourId, on);
    parent.addAndMakeVisible (b);
    return b;
}
} // namespace


/** What opens when you click (without dragging) a send dial: the level on a small fader, pre / post fader, the send's own MUTE, and up to two inserts on the send. */
class SendPanel : public juce::Component, private juce::Timer
{
public:
    using ForOthers = std::function<void (const std::function<void (GangMember&)>&)>;
    SendPanel (AppContext& a, MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, std::function<void()> changed, ForOthers others = {}, bool destTrack = false)
        : app (a), mixer (m), srcId (src), destId (dest), onChanged (std::move (changed)), send (*app.project.sendsOf (m, src)->get (dest)),
          slots (a, send.slots, kSendSlots), forOthers (std::move (others)), toTrack (destTrack)
    {
        noModeSwap (fader);
        title.setText (app.project.nodeName (src) + "  >  " + app.project.nodeName (dest), juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (title);
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
        fader.setRange (kSendOffDb, 6.0, 0.1);
        fader.setSkewFactorFromMidPoint (-18.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setScrollWheelEnabled (false);                       // (a wheel never moves a fader in the mixer)
        fader.textFromValueFunction = [] (double v) { return v <= kSendOffDb + 0.05 ? juce::String ("off") : juce::String (v, 1) + " dB"; };
        fader.setValue (send.active() ? (double) send.gainDb.get() : (double) kSendOffDb, juce::dontSendNotification);
        fader.onValueChange = [this]
        {
            const float db = fader.getValue() <= kSendOffDb + 0.05 ? -100.0f : (float) fader.getValue();
            const float old = send.active() ? send.gainDb.get() : -100.0f;
            if (app.setSendLevel (mixer, srcId, destId, db) == SendResult::Refused) fader.setValue (kSendOffDb, juce::dontSendNotification);
            else linked ([&] (GangMember& o) { nudgeSend (app, mixer, o.gangId(), destId, old, db, toTrack); });
            if (onChanged) onChanged();
        };
        addAndMakeVisible (fader);
        for (auto* b : { &preButton, &postButton })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (4711);
            b->setColour (juce::TextButton::buttonOnColourId, theme::accent);
            addAndMakeVisible (b);
        }
        preButton.setTooltip ("Take the send BEFORE the channel fader: moving the fader does not change it");
        postButton.setTooltip ("Take the send AFTER the channel fader: it follows the fader");
        preButton.setToggleState (send.pre.get(), juce::dontSendNotification);
        postButton.setToggleState (! send.pre.get(), juce::dontSendNotification);
        preButton.onClick  = [this] { send.pre.set (true);  linkedSend ([] (SendState& o) { o.pre.set (true); });  app.project.markDirty(); if (onChanged) onChanged(); };
        postButton.onClick = [this] { send.pre.set (false); linkedSend ([] (SendState& o) { o.pre.set (false); }); app.project.markDirty(); if (onChanged) onChanged(); };
        followMute.setButtonText ("Follow mute");
        followMute.setToggleState (send.followMute.get(), juce::dontSendNotification);
        followMute.setTooltip ("Ticked: muting the channel mutes this send too, whether it is taken before or after the fader. "
                               "Not ticked: the send carries on when the channel is muted (a solo elsewhere still silences it).");
        followMute.onClick = [this]
        {
            const bool on = followMute.getToggleState();
            send.followMute.set (on); linkedSend ([on] (SendState& o) { o.followMute.set (on); });
            app.project.markDirty(); refreshMute(); if (onChanged) onChanged();
        };
        addAndMakeVisible (followMute);
        muteButton.setTooltip ("Mutes this send only (its level is kept). Lit red: muted. A red outline: the channel's own MUTE is silencing it (Follow mute is ticked).");
        muteButton.setToggleState (send.mute.get(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            const bool on = muteButton.getToggleState();
            send.mute.set (on); linkedSend ([on] (SendState& o) { o.mute.set (on); });
            app.project.markDirty(); if (onChanged) onChanged();
        };
        addAndMakeVisible (muteButton);
        insertsCaption.setText ("Inserts on this send only", juce::dontSendNotification);
        insertsCaption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        insertsCaption.setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (insertsCaption); addAndMakeVisible (slots);
        refreshMute();
        startTimerHz (8);                                           // (the channel's MUTE may change while this is open)
        setSize (250, 254);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::row); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        fader.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (8);
        auto pp = r.removeFromTop (28);
        preButton.setBounds (pp.removeFromLeft (pp.getWidth() / 2).reduced (1));
        postButton.setBounds (pp.reduced (1));
        r.removeFromTop (4);
        muteButton.setBounds (r.removeFromTop (28));
        r.removeFromTop (2);
        followMute.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        insertsCaption.setBounds (r.removeFromTop (16));
        slots.setBounds (r.removeFromTop (kSendSlots * 28));
    }
private:
    /** fn on every strip linked to this one right now (Shift + Alt held), and their strips shown again afterwards. */
    void linked (const std::function<void (GangMember&)>& fn)
    {
        if (! forOthers) return;
        forOthers ([&] (GangMember& o) { fn (o); o.gangRefresh(); });
    }
    /** fn on the same send (to the same place) of every linked strip; a send to an audio track only where that strip already has one. */
    void linkedSend (const std::function<void (SendState&)>& fn)
    {
        linked ([&] (GangMember& o)
        {
            auto* l = app.project.sendsOf (mixer, o.gangId());
            if (l == nullptr || o.gangId() == destId) return;
            auto* s = toTrack ? l->find (destId) : l->get (destId);
            if (s != nullptr && (! toTrack || s->active())) fn (*s);
        });
    }
    void refreshMute()
    {
        if (muteButton.getToggleState() != send.mute.get()) muteButton.setToggleState (send.mute.get(), juce::dontSendNotification);
        muteButton.setFollowMuted (send.followMute.get() && channelMuteOn (app, mixer, srcId));
    }
    void timerCallback() override
    {
        bool alive = false;                                         // (the mixer deleted while this is open: close instead of reading it)
        for (auto& m : app.project.mixers) if (m.get() == &mixer) alive = true;
        if (! alive) { stopTimer(); if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); return; }
        refreshMute();
    }
    AppContext& app; MixerState& mixer; juce::Uuid srcId, destId; std::function<void()> onChanged; SendState& send;
    juce::Label title, insertsCaption; juce::Slider fader; juce::TextButton preButton { "PRE" }, postButton { "POST" };
    juce::ToggleButton followMute;
    SendMuteButton muteButton;
    SlotButtons slots;
    ForOthers forOthers; bool toTrack = false;
};

/** A rotary dial that sends a channel to another channel: drag up / down to change the level, click once for the details.
    The arc of the dial takes the colour of the channel it sends to. */
class SendDial : public juce::Component, private juce::Timer
{
public:
    SendDial (AppContext& a, MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, bool toTrack)
        : app (a), mixer (m), srcId (src), destId (dest), destName (app.project.nodeName (dest)), destIsTrack (toTrack)
    {
        noModeSwap (knob);
        styleCaption (label, {}, juce::Justification::centred);
        label.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        addAndMakeVisible (label);
        styleField (value, 9.0f, false);
        addAndMakeVisible (value);
        knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange (kSendOffDb, 6.0, 0.1);
        knob.onValueChange = [this]
        {
            const float db = knob.getValue() <= kSendOffDb + 0.05 ? -100.0f : (float) knob.getValue();
            showValue();
            auto* l = app.project.sendsOf (mixer, srcId);
            auto* s0 = l != nullptr ? l->find (destId) : nullptr;
            const float old = s0 != nullptr && s0->active() ? s0->gainDb.get() : -100.0f;
            if (app.setSendLevel (mixer, srcId, destId, db) == SendResult::Refused) refuse();
            else if (forOthers) forOthers ([&] (GangMember& o) { nudgeSend (app, mixer, o.gangId(), destId, old, db, destIsTrack); o.gangRefresh(); });   // linked strips (Shift + Alt)
        };
        knob.onClickWithoutDrag = [this] { openPanel(); };
        juce::Component::SafePointer<SendDial> safe (this);
        knob.onDoubleClick = [safe]
        {
            if (safe == nullptr) return;
            if (juce::ModifierKeys::currentModifiers.isCtrlDown()) safe->askValue();       // Ctrl + double-click types a level
            else safe->knob.setValue (0.0, juce::sendNotificationSync);                    // double-click: straight to 0 dB
        };
        addAndMakeVisible (knob);
        refreshFromModel();
    }
    void refreshFromModel()
    {
        auto* l = app.project.sendsOf (mixer, srcId);
        auto* s = l != nullptr ? l->find (destId) : nullptr;
        const double v = s != nullptr && s->active() ? (double) s->gainDb.get() : (double) kSendOffDb;
        if (std::abs (knob.getValue() - v) > 0.05) knob.setValue (v, juce::dontSendNotification);
        showValue();
        const auto arc = (juce::int64) chan::of (app.project, destId).getARGB();
        if ((juce::int64) knob.getProperties().getWithDefault ("arc", (juce::int64) 0) != arc) { knob.getProperties().set ("arc", arc); knob.repaint(); }
        // taken before the fader: a yellow square round the dial (the name stays the name). Its own MUTE: the ring goes grey and the read-out says MUTE;
        // silenced by the channel's MUTE (Follow mute ticked): a red outline round the read-out
        const bool pre = s != nullptr && s->pre.get(), own = s != nullptr && s->mute.get();
        const bool byChannel = s != nullptr && ! own && s->followMute.get() && channelMuteOn (app, mixer, srcId);
        if ((bool) knob.getProperties().getWithDefault ("pre", false) != pre || (bool) knob.getProperties().getWithDefault ("muted", false) != own)
        {
            knob.getProperties().set ("pre", pre); knob.getProperties().set ("muted", own); knob.repaint();
        }
        if (own != ownMute || byChannel != channelMute) { ownMute = own; channelMute = byChannel; showValue(); }
        if (! refused)
        {
            label.setText (destName + (s != nullptr && ! s->followMute.get() ? " *" : ""), juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, s != nullptr && s->active() ? mixTheme().text : mixTheme().dim);
        }
        knob.setTooltip ("Send to " + destName + ". Drag up / down to change, double-click for 0 dB (Ctrl + double-click to type a level), click once for pre / post fader, MUTE, inserts and a fader. "
                         + (pre ? "(Taken before the fader: the yellow square.)" : "(Taken after the fader.)")
                         + (own ? " MUTED (its own MUTE, in the panel)." : byChannel ? " Silenced by the channel's MUTE (red outline)." : "")
                         + (s != nullptr && ! s->followMute.get() ? " * = does not follow mute: it carries on when the channel is muted." : ""));
    }
    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (11));
        value.setBounds (r.removeFromBottom (12).withSizeKeepingCentre (38, 12));
        knob.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), r.getHeight()));
    }
private:
    /** One click (no drag) opens the send's panel, but only once it is clear that no second click is coming: a double-click types a level instead. */
    struct Knob : public valueentry::TypedSlider, private juce::Timer
    {
        std::function<void()> onClickWithoutDrag;
        void mouseDown (const juce::MouseEvent& e) override { if (e.getNumberOfClicks() > 1) { stopTimer(); secondClick = true; } valueentry::TypedSlider::mouseDown (e); }
        void mouseDoubleClick (const juce::MouseEvent& e) override { stopTimer(); secondClick = true; valueentry::TypedSlider::mouseDoubleClick (e); }
        void mouseUp (const juce::MouseEvent& e) override
        {
            const bool dragged = e.mouseWasDraggedSinceMouseDown();
            juce::Slider::mouseUp (e);
            if (secondClick) { secondClick = false; return; }
            if (! dragged && ! e.mods.isPopupMenu() && onClickWithoutDrag) startTimer (juce::MouseEvent::getDoubleClickTimeout() + 60);
        }
        void timerCallback() override { stopTimer(); if (onClickWithoutDrag) onClickWithoutDrag(); }
        bool secondClick = false;
    };
    void askValue()
    {
        const double v = knob.getValue();
        juce::Component::SafePointer<SendDial> safe (this);
        valueentry::ask (knob, "Send level in dB (or off)", v <= kSendOffDb + 0.05 ? juce::String ("off") : juce::String (v, 1), [safe] (const juce::String& t)
        {
            double db = 0.0;
            if (safe != nullptr && valueentry::parseDb (t, (double) kSendOffDb, (double) kSendOffDb, 6.0, db)) safe->knob.setValue (db, juce::sendNotificationSync);
        });
    }
    void showValue()
    {
        const double v = knob.getValue();
        const juce::Colour red (0xffe0312b);
        value.setText (ownMute ? juce::String ("MUTE") : v <= kSendOffDb + 0.05 ? juce::String ("off") : juce::String (v, 1), juce::dontSendNotification);
        value.setColour (juce::Label::textColourId, ownMute ? red.brighter (0.2f) : v <= kSendOffDb + 0.05 ? mixTheme().dim : mixTheme().text);
        value.setColour (juce::Label::outlineColourId, channelMute ? red : mixTheme().line);
    }
    void refuse()
    {
        knob.setValue (kSendOffDb, juce::dontSendNotification);
        refused = true;
        label.setText ("no loops!", juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffe03a2c));
        startTimer (1500);
    }
    void timerCallback() override { stopTimer(); refused = false; refreshFromModel(); }
    void openPanel()
    {
        auto* l = app.project.sendsOf (mixer, srcId);
        if (l == nullptr) return;
        auto safe = juce::Component::SafePointer<SendDial> (this);
        auto panel = std::make_unique<SendPanel> (app, mixer, srcId, destId, [safe] { if (safe != nullptr) safe->refreshFromModel(); }, forOthers, destIsTrack);
        juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
    }
    AppContext& app; MixerState& mixer; juce::Uuid srcId, destId; juce::String destName; bool destIsTrack = false;
    bool refused = false, ownMute = false, channelMute = false;
    juce::Label label, value; Knob knob;
public:
    /** Set by a channel strip: runs a function on every strip linked to it right now (Shift + Alt held, see StripGang). */
    SendPanel::ForOthers forOthers;
};

// ============================================================================ strips
/** The frame every strip shares: a coloured bar on top (what kind of channel), a coloured name plate at the bottom, and between them
    recessed wells and thin section headers - all one pixel fine. Click either coloured bar to choose the channel's colour. */
class StripBase : public juce::Component, public juce::SettableTooltipClient
{
public:
    StripBase (AppContext* a, const juce::Uuid& id) : appPtr (a), nodeId (id)
    {
        if (a != nullptr) setTooltip ("Click the coloured bar to change this channel's colour (it is used in the take and edit windows too)");
    }
    virtual void refreshFromModel() {}
    virtual void tick() {}
    virtual void resetPeaks() {}

    juce::Colour colour() const { return appPtr != nullptr ? chan::of (appPtr->project, nodeId) : juce::Colour (0xff8a909a); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = mixTheme();
        auto b = getLocalBounds();
        const auto c0 = colour();
        const float rad = 6.0f;                                                                                   // the strips have rounded corners
        g.setColour (t.strip.interpolatedWith (c0, t.ground ? 0.22f : 0.16f)); g.fillRoundedRectangle (b.toFloat(), rad);   // the strip itself is tinted with the channel colour
        for (auto& w : wells) { g.setColour (t.well.interpolatedWith (c0, t.ground ? 0.13f : 0.10f)); g.fillRect (w); g.setColour (t.soft.interpolatedWith (c0, 0.3f)); g.drawRect (w, 1); }
        g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
        for (auto& s : sections)
        {
            const int tw = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (8.5f, juce::Font::bold), s.first);
            g.setColour (t.dim); g.drawText (s.first, s.second.getX(), s.second.getY(), tw + 2, s.second.getHeight(), juce::Justification::centredLeft, false);
            g.setColour (t.soft.interpolatedWith (c0, 0.3f)); g.fillRect (s.second.getX() + tw + 6, s.second.getCentreY(), juce::jmax (0, s.second.getWidth() - tw - 6), 1);
        }
        g.setColour (t.line); g.drawRoundedRectangle (b.toFloat().reduced (0.5f), rad, 1.0f);

        const auto c = c0;
        auto band = bandRect();
        {   // the coloured bar at the top follows the rounded corners
            juce::Path bp; bp.addRoundedRectangle ((float) band.getX(), (float) band.getY(), (float) band.getWidth(), (float) band.getHeight(), rad - 1.0f, rad - 1.0f,
                                                    band.getY() <= 1, band.getY() <= 1, false, false);
            g.setColour (c); g.fillPath (bp);
        }
        g.setColour (c.brighter (0.45f));        g.fillRect (band.getX() + 3, band.getY(), band.getWidth() - 6, 1);
        g.setColour (c.darker (0.45f));          g.fillRect (band.getX(), band.getBottom() - 1, band.getWidth(), 1);
        g.setColour (chan::textOn (c));          g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (bandText(), band.reduced (4, 0), juce::Justification::centredLeft, true);
        if (const auto vm = vcaMask(); vm != 0 && appPtr != nullptr)            // the VCAs that control this strip: a glowing number each, at the right of the coloured bar
        {
            const int d = 12; int x = band.getRight() - 3 - d;
            for (int n = kMaxVcas; n-- > 0 && x > band.getX() + 28;)
            {
                if (! (vm & (1u << n))) continue;
                const auto* vd = appPtr->project.vcaByNumber (n + 1);
                const juce::Colour vc = vd != nullptr && vd->colour != 0 ? juce::Colour (vd->colour) : chan::automaticFor (NodeKind::Vca);
                const auto cc = juce::Rectangle<float> ((float) x, (float) band.getCentreY() - d * 0.5f, (float) d, (float) d);
                for (int k = 3; k >= 1; --k) { g.setColour (vc.withAlpha (0.16f)); g.fillEllipse (cc.expanded ((float) k * 1.3f)); }       // the glow
                g.setColour (vc.brighter (0.2f)); g.fillEllipse (cc);
                g.setColour (juce::Colours::white.withAlpha (0.9f)); g.drawEllipse (cc, 0.8f);
                g.setColour (chan::textOn (vc)); g.setFont (juce::FontOptions (n + 1 > 9 ? 7.5f : 9.0f, juce::Font::bold));
                g.drawText (juce::String (n + 1), cc.toNearestInt(), juce::Justification::centred, false);
                x -= d + 4;
            }
        }

        if (const int lat = shownLatency(); lat >= 0)
        {
            const bool zero = lat == 0;
            const auto al = alignText();
            if (al.isNotEmpty())                                                 // a spot mic lined up with the main pair: its delay, then any plug-in delay
            {
                auto lr = latencyRect();
                g.setColour (theme::accent); g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                g.drawText (zero ? al : al + "  " + juce::String (lat) + " smp", lr, juce::Justification::centred, false);
            }
            else
            {
                g.setColour (zero ? t.dim : t.text);
                g.setFont (juce::FontOptions (9.5f, zero ? juce::Font::plain : juce::Font::bold));
                g.drawText (juce::String (lat) + (lat == 1 ? " sample" : " samples"), latencyRect(), juce::Justification::centred, false);
            }
        }
        if (appPtr != nullptr)
        {
            auto plate = plateRect();
            {   // the name plate at the bottom follows the rounded corners too (it used to be square and covered them)
                juce::Path pp; pp.addRoundedRectangle ((float) plate.getX(), (float) plate.getY(), (float) plate.getWidth(), (float) plate.getHeight(), rad - 1.0f, rad - 1.0f,
                                                        false, false, plate.getBottom() >= getHeight() - 1, plate.getBottom() >= getHeight() - 1);
                g.setColour (c); g.fillPath (pp);
            }
            g.setColour (c.brighter (0.45f));    g.fillRect (plate.getX(), plate.getY(), plate.getWidth(), 1);
            g.setColour (c.darker (0.45f));      g.fillRect (plate.getX() + (int) rad, plate.getBottom() - 1, plate.getWidth() - 2 * (int) rad, 1);
            {   // the strip's outline again over the plate, so its rounded bottom edge stays crisp
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (plate.expanded (1, 1));
                g.setColour (t.line); g.drawRoundedRectangle (b.toFloat().reduced (0.5f), rad, 1.0f);
            }
            g.setColour (chan::textOn (c));      g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawFittedText (plateText(), plate.reduced (3, 2), juce::Justification::centred, 2, 0.85f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (appPtr == nullptr || nodeId.isNull()) return;
        if (plateRect().contains (e.getPosition()) && plateClicked (e)) return;          // an audio track's name plate: selects it (see StripGang)
        const auto area = bandRect().contains (e.getPosition()) ? bandRect() : plateRect().contains (e.getPosition()) ? plateRect() : juce::Rectangle<int>();
        if (area.isEmpty()) return;
        showColourPickerAt (*appPtr, nodeId, area.translated (getScreenX(), getScreenY()));
    }
    /** Selected strips have an outline: thin while they are only selected, bold and orange while Shift + Alt link them. */
    void paintOverChildren (juce::Graphics& g) override
    {
        const int look = gangLook();
        if (look == 0) return;
        const auto c = look == 2 ? juce::Colour (0xffff9f1c) : juce::Colour (0xff29b6ff);
        g.setColour (c); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (look == 2 ? 1.5f : 1.0f), 6.0f, look == 2 ? 3.0f : 2.0f);
        if (look == 2) { g.setColour (c.withAlpha (0.10f)); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f); }
    }

protected:
    /** A click on the name plate: true if the strip dealt with it (else it is the colour picker, as on the coloured bar). */
    virtual bool plateClicked (const juce::MouseEvent&) { return false; }
    /** 0: not selected; 1: selected; 2: selected and linked right now (Shift + Alt held). */
    virtual int gangLook() const { return 0; }
    virtual juce::String bandText() const { return {}; }
    virtual juce::String plateText() const { return {}; }
    juce::Rectangle<int> bandRect() const  { return { 1, 1, getWidth() - 2, kBandH }; }
    juce::Rectangle<int> plateRect() const { return { 1, getHeight() - 1 - kPlateH, getWidth() - 2, kPlateH }; }

    /** The area between the two coloured bars, with a little air on each side. */
    juce::Rectangle<int> body() const { return getLocalBounds().withTrimmedTop (1 + kBandH + 3).withTrimmedBottom (1 + kPlateH + 3 + (shownLatency() >= 0 ? kLatH : 0)).reduced (4, 0); }
    /** Delay compensation: the samples this channel's plug-ins hold the sound back by (-1: this kind of strip shows nothing). */
    virtual int shownLatency() const { return -1; }
    /** A spot mic's alignment delay now in force ("+3.42 ms"), or nothing. */
    virtual juce::String alignText() const { return {}; }
    /** The VCAs that control this strip (bit n = VCA n+1): they are drawn as glowing numbers on the coloured bar. */
    virtual juce::uint32 vcaMask() const { return 0; }
    void setLatencyTip (int own, const MixerState& m)
    {
        if (appPtr == nullptr) return;
        const double sr = appPtr->engine.getSampleRate() > 0 ? appPtr->engine.getSampleRate() : 48000.0;
        setTooltip ("Click the coloured bar to change this channel's colour (it is used in the take and edit windows too)\n\n"
                    "Plug-in delay of this channel: " + juce::String (own) + " samples (" + juce::String (own * 1000.0 / sr, 2) + " ms). Fermata holds the shorter paths back by the same "
                    "amount (delay compensation), so everything stays in time. Longest path in this mixer: " + juce::String (appPtr->engine.mixerLatency (&m)) + " samples.");
    }
    juce::Rectangle<int> latencyRect() const { return { 1, getHeight() - 1 - kPlateH - kLatH, getWidth() - 2, kLatH }; }

    AppContext* appPtr; juce::Uuid nodeId;
    std::vector<juce::Rectangle<int>> wells;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> sections;
};

/** The output of a strip, after its fader: an on / off button and the place it goes to ('Main' = every Ext Bus ticked as Main out, or any Int / Ext Bus).
    On an Ext Bus strip the same spot holds the tick box that makes it one of the mixer's main outputs. */
class StripOutputRow : public juce::Component
{
public:
    StripOutputRow (AppContext& a, MixerState& m, const juce::Uuid& channel, bool isTrack, bool isExt, Flag* onFlag, juce::Uuid* destPtr)
        : app (a), mixer (m), id (channel), ext (isExt), on (onFlag), dest (destPtr)
    {
        if (ext)
        {
            mainBox.setButtonText ("Main out");
            mainBox.setTooltip ("Ticked: this Ext Bus is a main output of this mixer - every strip whose output is set to 'Main' goes here, as well as whatever is "
                                "sent to it. Several Ext Buses can be ticked: each gets the main mix (plus its own extra sends, e.g. a performer's main mix with more of "
                                "themselves). None ticked: 'Main' goes nowhere.");
            mainBox.onClick = [this]
            {
                if (updating) return;
                mixer.busFor (id)->isMain.set (mainBox.getToggleState());
                app.engine.rebuildPlan(); app.project.markDirty(); app.project.changed();
            };
            addAndMakeVisible (mainBox);
        }
        else
        {
            button.setButtonText ("OUT"); button.setClickingTogglesState (true);
            button.setTooltip ("Send the output of this strip (after the fader) to the place chosen on the right");
            button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2b9e6a));
            button.onClick = [this] { if (! updating) { on->set (button.getToggleState()); app.engine.rebuildPlan(); app.project.markDirty(); } };
            box.setTooltip ("Where the output of this strip (after the fader) goes: 'Main' (every Ext Bus ticked as Main out), or any Int / Ext Bus of this mixer");
            box.onChange = [this] { if (! updating) choose(); };
            addAndMakeVisible (button); addAndMakeVisible (box);
        }
        fill();
    }
    void fill()
    {
        if (ext) { refresh(); return; }                                   // (an Ext Bus: only its Main out tick to show - it used to be left unticked whatever the mixer did)
        juce::String sig;                                                  // rebuild the list only when the buses changed, so an open drop-down is not torn down 3 times a second
        for (auto& b : app.project.buses) sig << b.id.toString() << b.name << (b.external ? "E" : "I") << "|";
        sig << id.toString();
        if (sig == lastSig && box.getNumItems() > 0) { refresh(); return; }
        lastSig = sig;
        updating = true;
        box.clear (juce::dontSendNotification);
        ids.clear();
        box.addItem ("Main", 1);
        int next = 2;
        for (auto& b : app.project.buses) if (! b.external && b.id != id) { ids.push_back (b.id); box.addItem (b.name + "  (Int)", next++); }     // the name first: a narrow box still shows it
        for (auto& b : app.project.buses) if (b.external)                 { ids.push_back (b.id); box.addItem (b.name + "  (Ext)", next++); }
        updating = false;
        refresh();
    }
    void refresh()
    {
        updating = true;
        if (ext)
        {
            mainBox.setToggleState (mixer.busFor (id)->isMain.get(), juce::dontSendNotification);
        }
        else
        {
            if (button.getToggleState() != on->get()) button.setToggleState (on->get(), juce::dontSendNotification);
            int want = 1;
            for (size_t i = 0; i < ids.size(); ++i) if (! dest->isNull() && ids[i] == *dest) want = (int) i + 2;
            if (box.getSelectedId() != want) box.setSelectedId (want, juce::dontSendNotification);
            const auto mains = mainNames();
            box.setTooltip ("Where the output of this strip goes (after the fader). 'Main' = " + (mains.isNotEmpty() ? mains : juce::String ("nowhere: no Ext Bus is ticked as Main out")));
        }
        updating = false;
    }
    void resized() override
    {
        auto r = getLocalBounds();
        if (ext) { mainBox.setBounds (r); return; }
        button.setBounds (r.removeFromLeft (r.getWidth() < 70 ? 26 : 34).reduced (0, 1)); r.removeFromLeft (r.getWidth() < 40 ? 2 : 3);
        box.setBounds (r.reduced (0, 1));
    }
private:
    /** The Ext Buses ticked as Main out in this mixer, by name ("Main L-R, Cans 1"). */
    juce::String mainNames() const
    {
        juce::StringArray n;
        for (auto& b : app.project.buses) if (b.external && mixer.busFor (b.id)->isMain.get()) n.add (b.name);
        return n.joinIntoString (", ");
    }
    void choose()
    {
        const int sel = box.getSelectedId();
        *dest = sel <= 1 || (size_t) sel - 2 >= ids.size() ? juce::Uuid::null() : ids[(size_t) sel - 2];
        app.engine.rebuildPlan(); app.project.markDirty();
    }
    AppContext& app; MixerState& mixer; juce::Uuid id; bool ext; Flag* on; juce::Uuid* dest;
    bool updating = false; juce::String lastSig;
    juce::TextButton button; juce::ComboBox box; juce::ToggleButton mainBox; std::vector<juce::Uuid> ids;
};

class ChannelStrip : public StripBase, public GangMember
{
public:
    ChannelStrip (AppContext& a, MixerState& m, const TrackDef& t, int slotCount, StripGang* g = nullptr)
        : StripBase (&a, t.id), app (a), mixer (m), trackId (t.id), strip (*m.stripFor (t.id)), numCh (t.channelCount()),
          slots (a, strip.slots, slotCount),
          fader ([this] (float db) { const float old = strip.gainDb.get(); strip.gainDb.set (db); app.project.markDirty(); linkFader (old, db); }, strip.gainDb.get()),
          outRow (a, m, t.id, true, false, &strip.outOn, &strip.outDest), gang (g)
    {
        nSlots = slotCount;
        if (gang != nullptr)
        {
            gang->members.push_back (this);
            juce::Component::SafePointer<ChannelStrip> safe (this);
            slots.gangSlots = [safe]
            {
                std::vector<InsertSlot*> v;
                if (safe != nullptr && safe->gang != nullptr) safe->gang->forOthers (safe->trackId, [&] (GangMember& o) { v.push_back (o.gangState().slots); });
                return v;
            };
            slots.gangRefresh = [safe] { if (safe != nullptr && safe->gang != nullptr) for (auto* o : safe->gang->members) if (safe->gang->isSelected (o->gangId())) o->gangRefresh(); };
        }

        addAndMakeVisible (outRow);
        // the ASIO input(s) of this track: the same setting as in the main window and the Project Designer
        const int nBoxes = numCh == 2 ? 2 : 1;
        for (int c = 0; c < nBoxes; ++c)
        {
            auto* cb = inBoxes.add (new juce::ComboBox());
            cb->setTooltip (numCh == 2 ? (c == 0 ? "ASIO input of the left channel (the right one follows it, change it below if needed)" : "ASIO input of the right channel")
                                       : "ASIO input of this track");
            cb->setTextWhenNothingSelected ("(no input)");
            cb->onChange = [this, c, cb]
            {
                if (updating) return;
                auto* tr = app.project.findTrack (trackId);
                if (tr == nullptr) return;
                if (app.engine.isRecording()) { showError ("Recording", "Inputs cannot be changed while recording."); refreshInputs(); return; }
                const int in = cb->getSelectedId() - 2;                       // id 1 = none
                if (c == 0) { if (in >= 0) app.project.assignInputsFrom (*tr, in); else tr->inputs[0] = -1; }
                else tr->inputs[1] = in;
                app.engine.rebuildPlan();
                app.project.changed();
                refreshInputs();
            };
            addAndMakeVisible (cb);
        }
        fillInputs();
        addAndMakeVisible (lcKey);
        addAndMakeVisible (slots);
        buildSends();

        // one pan slider per channel, as wide as the strip, with its exact position written beside it (L100 .. C .. R100).
        // A stereo track has a left AND a right one, so the left channel can sit on the right (and vice versa).
        const int nPan = numCh == 2 ? 2 : 1;
        for (int c = 0; c < nPan; ++c)
        {
            auto* l = panLabels.add (new juce::Label());
            styleCaption (*l, numCh == 2 ? (c == 0 ? "PAN L" : "PAN R") : "PAN");
            addAndMakeVisible (l);
            auto* v = panValues.add (new juce::Label());
            styleField (*v, 9.5f);
            addAndMakeVisible (v);
            auto* s = static_cast<valueentry::TypedSlider*> (pans.add (new valueentry::TypedSlider()));
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->setRange (-1.0, 1.0, 0.01);
            s->getProperties().set ("pan", true);
            const double def = numCh == 2 ? (c == 0 ? -1.0 : 1.0) : 0.0;
            s->setDoubleClickReturnValue (true, def, juce::ModifierKeys::ctrlModifier);          // double-click (or Ctrl + click) = home: C (a stereo track's two pans: hard left / hard right)
            {
                juce::Component::SafePointer<juce::Slider> safeS (s);
                panClicks.add (new valueentry::ReadoutClick (*v, false, [safeS]
                {
                    if (safeS == nullptr) return;
                    valueentry::ask (*safeS, "Pan (L30, C, R45 ...)", panText ((float) safeS->getValue()), [safeS] (const juce::String& t)
                    {
                        double pv = 0.0;
                        if (safeS != nullptr && valueentry::parsePan (t, pv)) safeS->setValue (pv, juce::sendNotificationSync);
                    });
                }));
                v->setTooltip ("Click to type a position (L30, C, R45 ...)");
            }
            Param& p = numCh == 2 ? (c == 0 ? strip.panL : strip.panR) : strip.pan;
            s->setValue (p.get(), juce::dontSendNotification);
            v->setText (panText (p.get()), juce::dontSendNotification);
            s->setTooltip (numCh == 2 ? juce::String (c == 0 ? "Where the LEFT channel of this track is placed (left .. right). " : "Where the RIGHT channel of this track is placed (left .. right). ")
                                            + "Double-click goes back to hard " + (c == 0 ? "left" : "right") + "; click the read-out beside it to type a position."
                                      : juce::String ("Pan (left .. right). Double-click centres it; click the read-out beside it to type a position."));
            s->onValueChange = [this, s, v, &p, c]
            {
                const float old = p.get();
                p.set ((float) s->getValue()); v->setText (panText ((float) s->getValue()), juce::dontSendNotification); app.project.markDirty();
                linkPan (c, (float) s->getValue() - old);
            };
            noModeSwap (*s);
            addAndMakeVisible (s);
        }

        mute = makeToggle (*this, toggles, "M", juce::Colour (0xffe0312b));
        solo = makeToggle (*this, toggles, "S", juce::Colour (0xfff2c200));
        solo->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        mute->setTooltip ("Mute (this mixer only)"); solo->setTooltip ("Solo (this mixer only)");
        mute->onClick = [this] { const bool on = mute->getToggleState(); strip.mute.set (on); linkAll ([on] (StripState& o) { o.mute.set (on); }); app.project.markDirty(); };
        solo->onClick = [this] { const bool on = solo->getToggleState(); strip.solo.set (on); linkAll ([on] (StripState& o) { o.solo.set (on); }); app.project.markDirty(); };
        mute->setToggleState (strip.mute.get(), juce::dontSendNotification);
        solo->setToggleState (strip.solo.get(), juce::dontSendNotification);
        addAndMakeVisible (fader);
        fader.noModeSwap();
        refreshFromModel();
    }
    ~ChannelStrip() override
    {
        if (gang != nullptr) gang->members.erase (std::remove (gang->members.begin(), gang->members.end(), static_cast<GangMember*> (this)), gang->members.end());
    }

    // ---- GangMember
    juce::Uuid gangId() const override { return trackId; }
    StripState& gangState() override { return strip; }
    int gangChannels() const override { return numCh; }
    juce::Component& gangComponent() override { return *this; }
    void gangRefresh() override
    {
        mute->setToggleState (strip.mute.get(), juce::dontSendNotification);
        solo->setToggleState (strip.solo.get(), juce::dontSendNotification);
        for (int c = 0; c < pans.size(); ++c)
        {
            Param& p = numCh == 2 ? (c == 0 ? strip.panL : strip.panR) : strip.pan;
            if (! pans[c]->isMouseButtonDown()) pans[c]->setValue (p.get(), juce::dontSendNotification);
            panValues[c]->setText (panText (p.get()), juce::dontSendNotification);
        }
        for (auto* d : dials) d->refreshFromModel();
        slots.refresh();
    }

    void fillInputs()
    {
        updating = true;
        for (auto* cb : inBoxes)
        {
            cb->clear (juce::dontSendNotification);
            cb->addItem ("(none)", 1);
            for (int i = 0; i < (int) app.project.inputs.size(); ++i) cb->addItem (app.project.inputLabel (i), i + 2);
        }
        updating = false;
        refreshInputs();
    }
    void refreshInputs()
    {
        auto* tr = app.project.findTrack (trackId);
        if (tr == nullptr) return;
        updating = true;
        for (int c = 0; c < inBoxes.size(); ++c)
        {
            const int want = tr->inputOf (c) >= 0 ? tr->inputOf (c) + 2 : 1;
            if (inBoxes[c]->getSelectedId() != want) inBoxes[c]->setSelectedId (want, juce::dontSendNotification);
            inBoxes[c]->setTooltip (inBoxes[c]->getText());
        }
        updating = false;
    }

    int shownLatency() const override { int t = strip.lowCut.latencySamples(); for (auto& sl : strip.slots) t += sl.latencySamples(); return t; }
    juce::String alignText() const override
    {
        const auto* tr = app.project.findTrack (strip.trackId);
        const double sr = app.engine.getSampleRate() > 0.0 ? app.engine.getSampleRate() : 48000.0;
        const double smp = tr != nullptr ? app.project.alignSamplesFor (*tr, sr) : 0.0;
        return smp > 0.0 ? juce::String::charToString (0x0394) + " " + juce::String (smp * 1000.0 / sr, 2) + " ms" : juce::String();
    }
    juce::uint32 vcaMask() const override { return strip.vcaMask.load(); }
    void refreshFromModel() override
    {
        setLatencyTip (shownLatency(), mixer);
        if (gang != nullptr)
            setTooltip (getTooltip() + "\n\nClick the name plate to select this strip (Shift + click another: every strip in between; Ctrl + click: one more or one less). "
                        "Hold Shift + Alt and whatever you do to one selected strip happens to all of them: faders, sends and pans move by the same amount; "
                        "M, S, a send's MUTE / PRE; inserting, removing or bypassing a plug-in (hold the keys as you choose it in the menu, or drop a copy with them held). "
                        "Right-click the name plate to hide strips from this mixer (+ Strip shows them again).");
        repaint();
        slots.refresh();
        lcKey.refresh();
        refreshInputs();
        outRow.fill();
        fader.setAccent (colour());
        mute->setToggleState (strip.mute.get(), juce::dontSendNotification);
        solo->setToggleState (strip.solo.get(), juce::dontSendNotification);
        for (int c = 0; c < pans.size(); ++c)
        {
            Param& p = numCh == 2 ? (c == 0 ? strip.panL : strip.panR) : strip.pan;
            if (! pans[c]->isMouseButtonDown() && std::abs (pans[c]->getValue() - p.get()) > 0.0001) pans[c]->setValue (p.get(), juce::dontSendNotification);
            panValues[c]->setText (panText (p.get()), juce::dontSendNotification);
            const auto arc = (juce::int64) colour().getARGB();
            if ((juce::int64) pans[c]->getProperties().getWithDefault ("arc", (juce::int64) 0) != arc) { pans[c]->getProperties().set ("arc", arc); pans[c]->repaint(); }
        }
        for (auto* d : dials) d->refreshFromModel();
        repaint();
    }

    /** "L35", "C" or "R100": the exact pan position. */
    static juce::String panText (float pan)
    {
        const int n = (int) std::lround (juce::jlimit (-1.0f, 1.0f, pan) * 100.0f);
        return n == 0 ? juce::String ("C") : n < 0 ? "L" + juce::String (-n) : "R" + juce::String (n);
    }

    /** One dial for every Int Bus and Ext Bus, plus one for every track this channel already sends to. */
    void buildSends()
    {
        auto addDial = [this] (const juce::Uuid& dest, bool toTrack)
        {
            auto* d = dials.add (new SendDial (app, mixer, trackId, dest, toTrack));
            juce::Component::SafePointer<ChannelStrip> safe (this);
            d->forOthers = [safe] (const std::function<void (GangMember&)>& fn) { if (safe != nullptr && safe->gang != nullptr) safe->gang->forOthers (safe->trackId, fn); };
            addAndMakeVisible (d);
        };
        for (auto& b : app.project.buses) if (! b.external) addDial (b.id, false);
        for (auto& b : app.project.buses) if (b.external)   addDial (b.id, false);
        for (auto& t : app.project.tracks)
            if (t.id != trackId)
                if (auto* s = strip.sends.find (t.id)) if (s->active()) addDial (t.id, true);
        toTrackButton.setButtonText ("+ send to track");
        toTrackButton.setTooltip ("Send this channel to another audio track of this mixer");
        toTrackButton.onClick = [this] { chooseTrack(); };
        addAndMakeVisible (toTrackButton);
    }
    void chooseTrack()
    {
        juce::PopupMenu m;
        std::vector<juce::Uuid> ids;
        for (auto& t : app.project.tracks)
        {
            if (t.id == trackId) continue;
            const bool ok = app.project.sendAllowed (mixer, trackId, t.id);
            auto* s = strip.sends.find (t.id);
            ids.push_back (t.id);
            m.addItem ((int) ids.size(), t.name + (s != nullptr && s->active() ? "  (already sent)" : ok ? "" : "  (would loop)"), ok && ! (s != nullptr && s->active()));
        }
        if (ids.empty()) m.addItem (-1, "No other audio tracks", false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toTrackButton), [this, ids] (int r)
        {
            if (r < 1 || (size_t) r > ids.size()) return;
            if (app.setSendLevel (mixer, trackId, ids[(size_t) r - 1], 0.0f) != SendResult::Refused) app.project.structureChanged();   // the strip grows a dial
        });
    }
    int dialCount() const { return dials.size(); }

    void tick() override
    {
        if (! fader.isBeingDragged()) fader.setDb (strip.gainDb.get());          // the fader follows the level, so automation (and the Stream Deck) moves it on screen
        followModel();                                                           // (and M / S and the pans follow the control surface)
        fader.pushPeaks (strip.meterL.take(), strip.meterR.take(), strip.rmsL.get(), strip.rmsR.get());
        {   // the VCAs that control this strip: a line beside the fader to the level it really has
            const auto vm = strip.vcaMask.load (std::memory_order_relaxed);
            float add = 0.0f; bool muted = false; juce::Colour vc;
            if (vm != 0) vcaGhostFor (app, mixer, vm, add, muted, vc);
            fader.setVcaGhost (vm != 0, add, muted, vc);
        }
    }
    void resetPeaks() override { fader.resetPeaks(); }
    /** The M / S keys and the pans show what the model says (the control surface changes it without a mouse): cheap, so it runs on every meter tick. */
    void followModel()
    {
        if (mute->getToggleState() != strip.mute.get()) mute->setToggleState (strip.mute.get(), juce::dontSendNotification);
        if (solo->getToggleState() != strip.solo.get()) solo->setToggleState (strip.solo.get(), juce::dontSendNotification);
        for (int c = 0; c < pans.size(); ++c)
        {
            const Param& p = numCh == 2 ? (c == 0 ? strip.panL : strip.panR) : strip.pan;
            if (! pans[c]->isMouseButtonDown() && std::abs (pans[c]->getValue() - p.get()) > 0.0001)
            {
                pans[c]->setValue (p.get(), juce::dontSendNotification);
                panValues[c]->setText (panText (p.get()), juce::dontSendNotification);
            }
        }
    }

    void resized() override
    {
        wells.clear(); sections.clear();
        auto r = body();
        outRow.setBounds (r.removeFromBottom (kOutRowH));
        r.removeFromBottom (3);
        auto faderArea = r.removeFromBottom (juce::jmax (kFaderMin, kFaderH + getHeight() - heightFor (2 * juce::jmax (1, sendRows), nSlots)));   // a taller / shorter window stretches the fader
        wells.push_back (faderArea.expanded (2, 0));
        r.removeFromBottom (3);
        auto ms = r.removeFromBottom (kMSH);
        r.removeFromBottom (4);
        auto panArea = r.removeFromBottom (2 * kPanRowH);

        for (int c = 0; c < 2; ++c)
        {
            auto row = r.removeFromTop (kInputRowH);
            if (c < inBoxes.size()) inBoxes[c]->setBounds (row.reduced (0, 1));
        }
        r.removeFromTop (3);
        lcKey.setBounds (r.removeFromTop (kLcRowH));
        r.removeFromTop (2);
        sections.push_back ({ "INSERTS", r.removeFromTop (kSecH) });
        auto slotArea = r.removeFromTop (nSlots * kSlotH + 2);
        wells.push_back (slotArea.expanded (2, 0));
        slots.setBounds (slotArea.reduced (0, 1));
        r.removeFromTop (4);
        sections.push_back ({ "SENDS", r.removeFromTop (kSecH) });
        const int rows = juce::jmax (1, sendRows);
        auto sendArea = r.removeFromTop (rows * kSendCellH);
        wells.push_back (sendArea.expanded (2, 0));
        const int cw = sendArea.getWidth() / 2;
        for (int k = 0; k < dials.size(); ++k)
            dials[k]->setBounds (juce::Rectangle<int> (sendArea.getX() + (k % 2) * cw, sendArea.getY() + (k / 2) * kSendCellH, cw, kSendCellH));
        toTrackButton.setBounds (r.removeFromTop (kToTrackH).reduced (0, 2));

        for (int c = 0; c < 2; ++c)
        {
            auto row = panArea.removeFromTop (kPanRowH);
            if (c < pans.size())
            {
                auto head = row.removeFromTop (14);
                panLabels[c]->setBounds (head.removeFromLeft (head.getWidth() / 2));
                panValues[c]->setBounds (head.removeFromRight (36).reduced (0, 1));
                pans[c]->setBounds (row.reduced (0, 0));                       // the whole width of the strip
            }
        }
        mute->setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1, 0));
        solo->setBounds (ms.reduced (1, 0));
        fader.setBounds (faderArea);
    }
    void setSendRows (int rows) { sendRows = rows; }

    /** The height of a strip with 'maxDials' sends on its busiest channel. */
    static int heightFor (int maxDials, int slotCount)
    {
        const int rows = juce::jmax (1, (maxDials + 1) / 2);
        return 1 + kBandH + 3 + 2 * kInputRowH + 3 + kLcRowH + 2 + kSecH + slotCount * kSlotH + 2 + 4 + kSecH + rows * kSendCellH + kToTrackH
               + 2 * kPanRowH + 4 + kMSH + 3 + kFaderH + 3 + kOutRowH + 3 + kLatH + kPlateH + 1 + 4;
    }

protected:
    juce::String bandText() const override { return numCh == 1 ? "AUDIO  MONO" : numCh == 2 ? "AUDIO  STEREO" : "AUDIO  " + juce::String (numCh) + " CH"; }
    juce::String plateText() const override { auto* t = app.project.findTrack (trackId); return t != nullptr ? t->name : juce::String(); }
    int gangLook() const override { return gang == nullptr || ! gang->isSelected (trackId) ? 0 : gang->linked (trackId) ? 2 : 1; }
    bool plateClicked (const juce::MouseEvent& e) override
    {
        if (gang == nullptr) return false;
        if (e.mods.isPopupMenu()) { plateMenu(); return true; }
        gang->click (trackId, e.mods);
        return true;
    }

private:
    /** The fader moved from oldDb to newDb: every linked strip's fader moves by as many dB (each keeps its own level; -inf counts as the bottom of the fader). */
    void linkFader (float oldDb, float newDb)
    {
        if (gang == nullptr) return;
        auto fl = [] (float v) { return v <= -79.5f ? -80.0f : v; };
        const float d = fl (newDb) - fl (oldDb);
        if (std::abs (d) < 1.0e-4f) return;
        gang->forOthers (trackId, [&] (GangMember& o)
        {
            auto& st = o.gangState();
            const float nv = juce::jlimit (-80.0f, 12.0f, fl (st.gainDb.get()) + d);
            st.gainDb.set (nv <= -79.5f ? -100.0f : nv);
        });                                                       // (their faders follow on the next meter tick)
    }
    /** Pan 'side' (0 / 1 of a stereo strip, 0 of a mono one) moved by d: the linked strips' pans move by as much - a stereo strip's same side, or both
        sides when the strip moved is mono; a mono strip's one pan. */
    void linkPan (int side, float d)
    {
        if (gang == nullptr || std::abs (d) < 1.0e-6f) return;
        gang->forOthers (trackId, [&] (GangMember& o)
        {
            auto& st = o.gangState();
            auto move = [d] (Param& p) { p.set (juce::jlimit (-1.0f, 1.0f, p.get() + d)); };
            if (o.gangChannels() == 2) { if (numCh == 2) move (side == 0 ? st.panL : st.panR); else { move (st.panL); move (st.panR); } }
            else move (st.pan);
            o.gangRefresh();
        });
    }
    void linkAll (const std::function<void (StripState&)>& fn)
    {
        if (gang == nullptr) return;
        gang->forOthers (trackId, [&] (GangMember& o) { fn (o.gangState()); o.gangRefresh(); });
    }
    /** Right-click on the name plate: colour, select, and hide strips from this mixer. */
    void plateMenu()
    {
        juce::PopupMenu m;
        const bool sel = gang->isSelected (trackId);
        const int n = sel ? (int) gang->selected.size() : 1;
        m.addItem (1, "Colour...");
        m.addSeparator();
        m.addItem (2, n > 1 ? "Hide the " + juce::String (n) + " selected strips from this mixer" : "Hide this strip from this mixer");
        m.addSeparator();
        m.addItem (3, "Select every audio strip");
        m.addItem (4, "Select none", ! gang->selected.empty());
        juce::Component::SafePointer<ChannelStrip> safe (this);
        const auto where = plateRect().translated (getScreenX(), getScreenY());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (where), [safe, where] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            if (r == 1) { showColourPickerAt (self.app, self.trackId, where); return; }
            if (r == 3) { self.gang->selectAll(); return; }
            if (r == 4) { self.gang->clear(); return; }
            if (r == 2)
            {
                std::vector<juce::Uuid> ids;
                if (self.gang->isSelected (self.trackId)) ids = self.gang->selected; else ids.push_back (self.trackId);
                hideStrips (self.app, self.mixer, ids);            // (the window is rebuilt: 'self' is gone after this)
            }
        });
    }
public:
    /** Takes audio tracks out of mixer m: no strips in its window, nothing of them in its mix (a solo on them is let go of first). The + Strip button
        shows them again. */
    static void hideStrips (AppContext& app, MixerState& m, const std::vector<juce::Uuid>& ids)
    {
        for (auto& id : ids)
        {
            if (! m.isStripHidden (id)) m.hiddenStrips.push_back (id);
            m.stripFor (id)->solo.set (false);
        }
        app.engine.rebuildPlan();
        app.project.markDirty();
        app.project.structureChanged();
    }
private:
    AppContext& app; MixerState& mixer; juce::Uuid trackId; StripState& strip; int numCh;
    bool updating = false;
    int sendRows = 1, nSlots = kNumSlots;
    LowCutKey lcKey { app, strip.lowCut, [this] { auto* tr = app.project.findTrack (trackId); return tr != nullptr ? tr->name : juce::String(); } };
    juce::OwnedArray<juce::ComboBox> inBoxes;
    juce::OwnedArray<SendDial> dials;
    juce::TextButton toTrackButton;
    juce::OwnedArray<juce::Slider> pans; juce::OwnedArray<juce::Label> panLabels, panValues;
    juce::OwnedArray<valueentry::ReadoutClick> panClicks;            // (after panValues: they go first)
    juce::OwnedArray<juce::TextButton> toggles;
    juce::TextButton *mute = nullptr, *solo = nullptr;
    SlotButtons slots; FaderBlock fader; StripOutputRow outRow;
    StripGang* gang = nullptr;
};

/** An Int Bus (fed by dials on audio tracks and other buses; goes on through its own dials)
    or an Ext Bus (fed the same way; its only way out is a pair of driver outputs, or nowhere). */
class BusStrip : public StripBase
{
public:
    BusStrip (AppContext& a, MixerState& m, const BusDef& def, int slotCount)
        : StripBase (&a, def.id), app (a), mixer (m), busId (def.id), st (*m.busFor (def.id)), external (def.external), slots (a, st.slots, slotCount),
          fader ([this] (float db) { st.gainDb.set (db); app.project.markDirty(); }, st.gainDb.get()),
          outRow (a, m, def.id, false, def.external, &st.outOn, &st.outDest)
    {
        nSlots = slotCount;
        addAndMakeVisible (slots); addAndMakeVisible (fader); addAndMakeVisible (outRow);
        mute = makeToggle (*this, toggles, "M", juce::Colour (0xffe0312b));
        mute->setToggleState (st.mute.get(), juce::dontSendNotification);
        mute->setTooltip ("Mute (this mixer only)");
        mute->onClick = [this] { st.mute.set (mute->getToggleState()); app.project.markDirty(); };
        if (! external)
        {
            lcKey = std::make_unique<LowCutKey> (a, st.lowCut, [this] { auto* b = app.project.findBus (busId); return b != nullptr ? b->name : juce::String(); });
            addAndMakeVisible (*lcKey);
            solo = makeToggle (*this, toggles, "S", juce::Colour (0xfff2c200));
            solo->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            solo->setToggleState (st.solo.get(), juce::dontSendNotification);
            solo->setTooltip ("Solo this bus (this mixer only): everything that feeds it stays open, the rest is silenced");
            solo->onClick = [this] { st.solo.set (solo->getToggleState()); app.project.markDirty(); };
        }
        if (! external)                                                            // an Int Bus has a pan (balance of its stereo pair) like a track
        {
            styleCaption (panLabel, "PAN"); addAndMakeVisible (panLabel);
            styleField (panValue, 9.5f); addAndMakeVisible (panValue);
            panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            panSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            panSlider.setRange (-1.0, 1.0, 0.01);
            panSlider.getProperties().set ("pan", true);
            panSlider.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::ctrlModifier);       // double-click (or Ctrl + click) = C
            panClick = std::make_unique<valueentry::ReadoutClick> (panValue, false, [this]
            {
                valueentry::ask (panSlider, "Balance (L30, C, R45 ...)", ChannelStrip::panText ((float) panSlider.getValue()), [this] (const juce::String& t)
                { double pv = 0.0; if (valueentry::parsePan (t, pv)) panSlider.setValue (pv, juce::sendNotificationSync); });
            });
            panValue.setTooltip ("Click to type a position (L30, C, R45 ...)");
            panSlider.setValue (st.pan.get(), juce::dontSendNotification);
            panValue.setText (ChannelStrip::panText (st.pan.get()), juce::dontSendNotification);
            panSlider.setTooltip ("Balance of this bus's stereo pair: moving it turns the opposite side down. Double-click centres it; click the read-out beside it to type a position. Automatable.");
            panSlider.onValueChange = [this] { st.pan.set ((float) panSlider.getValue()); panValue.setText (ChannelStrip::panText ((float) panSlider.getValue()), juce::dontSendNotification); app.project.markDirty(); };
            addAndMakeVisible (panSlider);
        }
        if (external)
        {
            outBox.addItem ("(nowhere)", 1);
            const int n = (int) app.project.outputs.size();
            for (int i = 0; i + 1 < n; i += 2) outBox.addItem (juce::String (i + 1) + "-" + juce::String (i + 2), i + 2);
            outBox.setTooltip ("An Ext Bus must go to driver outputs (a stereo pair) or nowhere");
            outBox.onChange = [this] { if (! updating) { st.outFirst.store (outBox.getSelectedId() <= 1 ? -1 : outBox.getSelectedId() - 2); app.project.markDirty(); app.project.changed(); } };
            addAndMakeVisible (outBox);
            note.setText ("An Ext Bus is how sound leaves the mixer: pick the pair of driver outputs above (or nowhere). Tick Main out below to make this the bus that strips send to by default.", juce::dontSendNotification);
            note.setFont (juce::FontOptions (9.5f));
            note.setColour (juce::Label::textColourId, mixTheme().dim);
            note.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (note);
        }
        else
        {
            for (auto& b : app.project.buses) if (! b.external && b.id != busId) { auto* d = dials.add (new SendDial (app, mixer, busId, b.id, false)); addAndMakeVisible (d); }
            for (auto& b : app.project.buses) if (b.external)                    { auto* d = dials.add (new SendDial (app, mixer, busId, b.id, false)); addAndMakeVisible (d); }
            for (auto& t : app.project.tracks)
                if (auto* s = st.sends.find (t.id)) if (s->active()) { auto* d = dials.add (new SendDial (app, mixer, busId, t.id, true)); addAndMakeVisible (d); }
            toTrackButton.setButtonText ("+ send to track");
            toTrackButton.setTooltip ("Send this bus to an audio track of this mixer");
            toTrackButton.onClick = [this] { chooseTrack(); };
            addAndMakeVisible (toTrackButton);
        }
        refreshFromModel();
    }
    int dialCount() const { return dials.size(); }
    void setSendRows (int rows) { sendRows = rows; }
    int shownLatency() const override { int t = st.lowCut.latencySamples(); for (auto& sl : st.slots) t += sl.latencySamples(); return t; }
    juce::uint32 vcaMask() const override { return st.vcaMask.load(); }
    void refreshFromModel() override
    {
        setLatencyTip (shownLatency(), mixer);
        repaint();
        slots.refresh();
        if (lcKey != nullptr) lcKey->refresh();
        outRow.fill();
        fader.setAccent (colour());
        mute->setToggleState (st.mute.get(), juce::dontSendNotification);
        if (solo != nullptr) solo->setToggleState (st.solo.get(), juce::dontSendNotification);
        if (external)
        {
            updating = true;
            const int f = st.outFirst.load();
            outBox.setSelectedId (f < 0 ? 1 : f + 2, juce::dontSendNotification);
            updating = false;
        }
        for (auto* d : dials) d->refreshFromModel();
        repaint();
    }
    void tick() override
    {
        if (! fader.isBeingDragged()) fader.setDb (st.gainDb.get());
        if (mute->getToggleState() != st.mute.get()) mute->setToggleState (st.mute.get(), juce::dontSendNotification);          // (the control surface)
        if (solo != nullptr && solo->getToggleState() != st.solo.get()) solo->setToggleState (st.solo.get(), juce::dontSendNotification);
        if (! external && ! panSlider.isMouseButtonDown() && std::abs (panSlider.getValue() - st.pan.get()) > 0.0001)
        {
            panSlider.setValue (st.pan.get(), juce::dontSendNotification);
            panValue.setText (ChannelStrip::panText (st.pan.get()), juce::dontSendNotification);
        }
        if (! external)
        {   // an Int Bus controlled by VCAs: the same ghost line
            const auto vm = st.vcaMask.load (std::memory_order_relaxed);
            float add = 0.0f; bool muted = false; juce::Colour vc;
            if (vm != 0) vcaGhostFor (app, mixer, vm, add, muted, vc);
            fader.setVcaGhost (vm != 0, add, muted, vc);
        }
        fader.pushPeaks (st.meterL.take(), st.meterR.take(), st.rmsL.get(), st.rmsR.get());
    }
    void resetPeaks() override { fader.resetPeaks(); }
    void resized() override
    {
        wells.clear(); sections.clear();
        auto r = body();
        outRow.setBounds (r.removeFromBottom (kOutRowH));
        r.removeFromBottom (3);
        auto faderArea = r.removeFromBottom (juce::jmax (kFaderMin, kFaderH + getHeight() - ChannelStrip::heightFor (2 * juce::jmax (1, sendRows), nSlots)));   // same vertical positions as on the audio track strips
        wells.push_back (faderArea.expanded (2, 0));
        r.removeFromBottom (3);
        auto ms = r.removeFromBottom (kMSH);
        r.removeFromBottom (4);
        auto panArea = r.removeFromBottom (2 * kPanRowH);
        if (! external)
        {
            auto row = panArea.removeFromTop (kPanRowH);
            auto head = row.removeFromTop (14);
            panLabel.setBounds (head.removeFromLeft (head.getWidth() / 2));
            panValue.setBounds (head.removeFromRight (36).reduced (0, 1));
            panSlider.setBounds (row);
        }
        mute->setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1, 0));
        if (solo != nullptr) solo->setBounds (ms.reduced (1, 0));
        fader.setBounds (faderArea);

        auto top = r.removeFromTop (2 * kInputRowH);                         // where audio track strips have their input selectors
        if (external) { sections.push_back ({ "DRIVER OUTPUTS", top.removeFromTop (kSecH) }); outBox.setBounds (top.removeFromTop (kInputRowH).reduced (0, 1)); }
        r.removeFromTop (3);
        auto lcRow = r.removeFromTop (kLcRowH);
        if (lcKey != nullptr) lcKey->setBounds (lcRow);
        r.removeFromTop (2);
        sections.push_back ({ "INSERTS", r.removeFromTop (kSecH) });
        auto slotArea = r.removeFromTop (nSlots * kSlotH + 2);
        wells.push_back (slotArea.expanded (2, 0));
        slots.setBounds (slotArea.reduced (0, 1));
        r.removeFromTop (4);
        if (external) { note.placeRightOf (r.removeFromTop (22)); return; }
        sections.push_back ({ "SENDS", r.removeFromTop (kSecH) });
        auto sendArea = r.removeFromTop (juce::jmax (1, sendRows) * kSendCellH);
        wells.push_back (sendArea.expanded (2, 0));
        const int cw = sendArea.getWidth() / 2;
        for (int k = 0; k < dials.size(); ++k)
            dials[k]->setBounds (juce::Rectangle<int> (sendArea.getX() + (k % 2) * cw, sendArea.getY() + (k / 2) * kSendCellH, cw, kSendCellH));
        toTrackButton.setBounds (r.removeFromTop (kToTrackH).reduced (0, 2));
    }
protected:
    juce::String bandText() const override { return external ? "EXT BUS" : "INT BUS"; }
    juce::String plateText() const override { auto* b = app.project.findBus (busId); return b != nullptr ? b->name : juce::String(); }
private:
    void chooseTrack()
    {
        juce::PopupMenu m;
        std::vector<juce::Uuid> ids;
        for (auto& t : app.project.tracks)
        {
            const bool ok = app.project.sendAllowed (mixer, busId, t.id);
            auto* s = st.sends.find (t.id);
            ids.push_back (t.id);
            m.addItem ((int) ids.size(), t.name + (s != nullptr && s->active() ? "  (already sent)" : ok ? "" : "  (would loop)"), ok && ! (s != nullptr && s->active()));
        }
        if (ids.empty()) m.addItem (-1, "No audio tracks", false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toTrackButton), [this, ids] (int r)
        {
            if (r < 1 || (size_t) r > ids.size()) return;
            if (app.setSendLevel (mixer, busId, ids[(size_t) r - 1], 0.0f) != SendResult::Refused) app.project.structureChanged();
        });
    }
    AppContext& app; MixerState& mixer; juce::Uuid busId; BusState& st; bool external; SlotButtons slots; FaderBlock fader; StripOutputRow outRow;
    bool updating = false; int sendRows = 1, nSlots = kNumSlots;
    std::unique_ptr<LowCutKey> lcKey;                                   // Int Buses only
    InfoNote note; juce::ComboBox outBox;
    juce::OwnedArray<SendDial> dials; juce::TextButton toTrackButton;
    juce::OwnedArray<juce::TextButton> toggles; juce::TextButton *mute = nullptr, *solo = nullptr;
    juce::Label panLabel, panValue; valueentry::TypedSlider panSlider;
    std::unique_ptr<valueentry::ReadoutClick> panClick;
};

/** Which audio tracks and Int Buses a VCA controls in one mixer: a tick box for each. Opens from the strip's Assign button; every tick takes effect at once. */
class VcaAssignPanel : public juce::Component
{
public:
    VcaAssignPanel (AppContext& a, MixerState& m, const VcaDef& d) : app (a), mixer (m), vcaId (d.id)
    {
        title.setText ("VCA " + d.label() + " controls:", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold)); title.setColour (juce::Label::textColourId, mixTheme().text);
        addAndMakeVisible (title);
        allBtn.setButtonText ("All tracks"); noneBtn.setButtonText ("None");
        allBtn.onClick  = [this] { setAll (true); };
        noneBtn.onClick = [this] { setAll (false); };
        addAndMakeVisible (allBtn); addAndMakeVisible (noneBtn);
        auto add = [this, &d] (const juce::Uuid& id, const juce::String& name)
        {
            auto* b = boxes.add (new juce::ToggleButton (name));
            b->setToggleState (app.project.isVcaMember (mixer, d, id), juce::dontSendNotification);
            b->setColour (juce::ToggleButton::textColourId, mixTheme().text);
            b->onClick = [this, id, b]
            {
                if (auto* v = app.project.findVca (vcaId)) { app.project.setVcaMember (mixer, *v, id, b->getToggleState()); app.project.changed(); }
            };
            ids.add (id);
            content.addAndMakeVisible (b);
        };
        for (auto& t : app.project.tracks) add (t.id, t.name);
        for (auto& b : app.project.buses) if (! b.external) add (b.id, b.name + "   (Int Bus)");
        empty.setText ("There are no audio tracks or Int Buses.", juce::dontSendNotification);
        empty.setColour (juce::Label::textColourId, mixTheme().dim);
        addChildComponent (empty); empty.setVisible (boxes.isEmpty());
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        setSize (260, juce::jlimit (150, 460, 70 + boxes.size() * 24));
    }
    void paint (juce::Graphics& g) override { g.fillAll (mixTheme().strip); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (22));
        auto row = r.removeFromTop (24);
        allBtn.setBounds (row.removeFromLeft (90)); row.removeFromLeft (6); noneBtn.setBounds (row.removeFromLeft (70));
        r.removeFromTop (4);
        viewport.setBounds (r);
        const int w = viewport.getWidth() - (viewport.isVerticalScrollBarShown() ? viewport.getScrollBarThickness() : 0);
        content.setSize (w, juce::jmax (24, boxes.size() * 24));
        for (int i = 0; i < boxes.size(); ++i) boxes[i]->setBounds (0, i * 24, w, 24);
        empty.setBounds (r.withHeight (22));
    }
private:
    void setAll (bool on)
    {
        auto* v = app.project.findVca (vcaId);
        if (v == nullptr) return;
        for (int i = 0; i < boxes.size(); ++i)
        {
            if (app.project.kindOf (ids[i]) != NodeKind::Track && on) continue;                  // "All tracks" ticks the audio tracks only
            boxes[i]->setToggleState (on, juce::dontSendNotification);
            app.project.setVcaMember (mixer, *v, ids[i], on);
        }
        app.project.changed();
    }
    AppContext& app; MixerState& mixer; juce::Uuid vcaId;
    juce::Label title, empty; juce::TextButton allBtn, noneBtn;
    juce::Viewport viewport; juce::Component content;
    juce::OwnedArray<juce::ToggleButton> boxes; juce::Array<juce::Uuid> ids;
};

/** A VCA strip: just a fader (and M / S). It carries no audio: it moves the faders of the strips it controls by the same amount, without touching their own faders. */
class VcaStrip : public StripBase
{
public:
    VcaStrip (AppContext& a, MixerState& m, const VcaDef& def)
        : StripBase (&a, def.id), app (a), mixer (m), vcaId (def.id), number (def.number), st (m.vcas[juce::jlimit (0, kMaxVcas - 1, def.number - 1)]),
          fader ([this] (float db) { st.gainDb.set (db); app.project.markDirty(); }, st.gainDb.get())
    {
        addAndMakeVisible (fader);
        mute = makeToggle (*this, toggles, "M", juce::Colour (0xffe0312b));
        mute->setTooltip ("Mute everything this VCA controls (this mixer only)");
        mute->onClick = [this] { st.mute.set (mute->getToggleState()); app.project.markDirty(); };
        solo = makeToggle (*this, toggles, "S", juce::Colour (0xfff2c200));
        solo->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        solo->setTooltip ("Solo everything this VCA controls: those strips stay open, the rest is silenced (this mixer only)");
        solo->onClick = [this] { st.solo.set (solo->getToggleState()); app.project.markDirty(); };
        assignBtn.setButtonText ("Assign...");
        assignBtn.setTooltip ("Choose the audio tracks and Int Buses this VCA controls. Each one gets a glowing number on its strip.");
        assignBtn.onClick = [this] { openAssign(); };
        addAndMakeVisible (assignBtn);
        countLabel.setJustificationType (juce::Justification::centred); styleField (countLabel, 9.5f);
        addAndMakeVisible (countLabel);
        nameEd.setTextToShowWhenEmpty ("name (optional)", mixTheme().dim);
        nameEd.setFont (juce::FontOptions (11.0f));
        nameEd.setTooltip ("Type a name: the strip shows  " + juce::String (number) + " - name");
        nameEd.onReturnKey = [this] { commitName(); nameEd.giveAwayKeyboardFocus(); };      // Enter: done, and the box lets go of the keyboard (Space / R work again)
        nameEd.onEscapeKey = [this] { if (auto* v = app.project.findVca (vcaId)) nameEd.setText (v->name, juce::dontSendNotification); nameEd.giveAwayKeyboardFocus(); };
        nameEd.onFocusLost = [this] { commitName(); };
        addAndMakeVisible (nameEd);
        removeBtn.setButtonText ("Remove strip");
        removeBtn.setTooltip ("Takes this VCA's strip out of this mixer (the VCA itself stays in the project, and its settings here are kept); add it again with + Strip");
        removeBtn.onClick = [this]
        {
            const auto id = vcaId; auto* mx = &mixer; auto* ap = &app;
            juce::MessageManager::callAsync ([ap, mx, id] { if (auto* v = ap->project.findVca (id)) ap->project.showVca (*mx, *v, false); });
        };
        addAndMakeVisible (removeBtn);
        refreshFromModel();
    }
    void setSendRows (int rows) { sendRows = rows; }
    void setSlotCount (int n) { nSlots = n; }
    void refreshFromModel() override
    {
        mute->setToggleState (st.mute.get(), juce::dontSendNotification);
        solo->setToggleState (st.solo.get(), juce::dontSendNotification);
        fader.setAccent (colour());
        int n = 0;
        for (auto& s : mixer.stripPool) if (s->vcaMask.load() & (1u << (number - 1))) ++n;
        for (auto& b : mixer.busPool)   if (b->vcaMask.load() & (1u << (number - 1))) ++n;
        countLabel.setText (n == 0 ? juce::String ("controls nothing") : n == 1 ? juce::String ("controls 1 strip") : "controls " + juce::String (n) + " strips", juce::dontSendNotification);
        if (auto* v = app.project.findVca (vcaId); v != nullptr && ! nameEd.hasKeyboardFocus (false)) nameEd.setText (v->name, juce::dontSendNotification);
        repaint();
    }
    void tick() override
    {
        if (! fader.isBeingDragged()) fader.setDb (st.gainDb.get());
        if (mute->getToggleState() != st.mute.get()) mute->setToggleState (st.mute.get(), juce::dontSendNotification);          // (the control surface)
        if (solo->getToggleState() != st.solo.get()) solo->setToggleState (st.solo.get(), juce::dontSendNotification);
    }
    void resized() override
    {
        wells.clear(); sections.clear();
        auto r = body().withTrimmedBottom (kLatH);       // (the other strips keep a line for the plug-in delay above the name plate: the same room here, so
        removeBtn.setBounds (r.removeFromBottom (kOutRowH));   //  M / S, the fader, its meter and level line up with theirs)
        r.removeFromBottom (3);
        auto faderArea = r.removeFromBottom (juce::jmax (kFaderMin, kFaderH + getHeight() - ChannelStrip::heightFor (2 * juce::jmax (1, sendRows), nSlots)));
        wells.push_back (faderArea.expanded (2, 0));
        r.removeFromBottom (3);
        auto ms = r.removeFromBottom (kMSH);
        mute->setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1, 0));
        solo->setBounds (ms.reduced (1, 0));
        fader.setBounds (faderArea);
        r.removeFromBottom (4);
        sections.push_back ({ "CONTROLS", r.removeFromTop (kSecH) });
        assignBtn.setBounds (r.removeFromTop (26).reduced (0, 1));
        countLabel.setBounds (r.removeFromTop (16));
        r.removeFromTop (6);
        sections.push_back ({ "NAME", r.removeFromTop (kSecH) });
        nameEd.setBounds (r.removeFromTop (22).reduced (0, 1));
    }
protected:
    juce::String bandText() const override { return "VCA " + juce::String (number); }
    juce::String plateText() const override { auto* v = app.project.findVca (vcaId); return v != nullptr ? v->label() : juce::String(); }
private:
    void commitName()
    {
        auto* v = app.project.findVca (vcaId);
        if (v == nullptr) return;
        const auto t = nameEd.getText().trim();
        if (t == v->name) return;
        v->name = t; app.project.markDirty(); app.project.changed();         // (only a refresh: this editor must not be rebuilt while it is being used)
    }
    void openAssign()
    {
        auto* v = app.project.findVca (vcaId);
        if (v == nullptr) return;
        auto panel = std::make_unique<VcaAssignPanel> (app, mixer, *v);
        juce::CallOutBox::launchAsynchronously (std::move (panel), assignBtn.getScreenBounds(), nullptr);
    }
    AppContext& app; MixerState& mixer; juce::Uuid vcaId; int number; VcaState& st; FaderBlock fader;
    juce::OwnedArray<juce::TextButton> toggles; juce::TextButton *mute = nullptr, *solo = nullptr;
    juce::TextButton assignBtn, removeBtn; juce::Label countLabel; juce::TextEditor nameEd;
    int sendRows = 1, nSlots = kNumSlots;
};

/** The last strip: what the level meters show and how they move (shared by all mixers and remembered), plus the phase scope. */
class MeterSettingsStrip : public StripBase
{
public:
    MeterSettingsStrip (AppContext& a, juce::Uuid mixerId, std::function<void()> resetFn)
        : StripBase (nullptr, juce::Uuid::null()), app (a), mixer (mixerId), onReset (std::move (resetFn))
    {
        resetButton.setButtonText ("Reset peaks");
        resetButton.setTooltip ("Clears all the peak-hold markers and numbers in this mixer. Peaks above -2 dBFS stay marked until you press this.");
        resetButton.onClick = [this] { if (onReset) onReset(); };
        addAndMakeVisible (resetButton);

        {
            int sel = 1;
            if (auto* mx = mixerState()) sel = mx->ditherBits.load() == 0 ? 0 : mx->ditherBits.load() == 16 ? 2 : 1;
            setup (ditherBox, ditherCap, "OUTPUT DITHER", { "Off", "24 bit", "16 bit" }, sel,
                   "Dither on this mixer's outputs to the interface, added just before the audio goes to the driver (TPDF, 1 LSB). "
                   "24 bit: for a 24-bit converter (the normal choice). 16 bit: when this mixer feeds a 16-bit device or recorder. "
                   "Off: none. Files are dithered separately whenever they are written (bounce, repairs, DDP).");
            ditherBox.onChange = [this]
            {
                if (auto* mx = mixerState())
                {
                    const int id = ditherBox.getSelectedId();
                    mx->ditherBits.store (id == 1 ? 0 : id == 3 ? 16 : 24);
                    app.project.markDirty();
                }
            };
        }
        const auto& ms = meterSettings();
        setup (modeBox, modeCap, "METER SHOWS", { "Peak", "RMS", "Peak + RMS" }, ms.mode,
               "Peak: the highest sample. RMS: the average energy (about 300 ms), closer to how loud it sounds. Peak + RMS: a bright RMS bar inside a dim peak bar.");
        setup (riseBox, riseCap, "RISE", { "Instant", "Fast", "Medium", "Slow" }, ms.rise, "How quickly the bar climbs when the level goes up: at once, or over 10, 50 or 300 ms. The lighter cap on the bar is always the sample peak, at once.");
        setup (fallBox, fallCap, "FALL", { "Fast", "Medium", "Slow", "Very slow" }, ms.fall, "How quickly the bar drops when the level goes down: 90, 25, 12 or 6 dB a second. The cap drops at 60 dB a second.");
        setup (holdBox, holdCap, "PEAK HOLD", { "Off", "1 second", "3 seconds", "10 seconds", "30 seconds", "Until reset" }, ms.hold,
               "How long the white peak marker stays before it drops. Peaks above -2 dBFS always stay until 'Reset peaks'.");
        auto change = [this]
        {
            auto& m = meterSettings();
            m.mode = modeBox.getSelectedId() - 1; m.rise = riseBox.getSelectedId() - 1; m.fall = fallBox.getSelectedId() - 1; m.hold = holdBox.getSelectedId() - 1;
            if (auto* st = app.props.getUserSettings())
            {
                st->setValue ("meterMode", m.mode); st->setValue ("meterRise", m.rise); st->setValue ("meterFall", m.fall); st->setValue ("meterHold", m.hold);
                app.props.saveIfNeeded();
            }
        };
        for (auto* b : { &modeBox, &riseBox, &fallBox, &holdBox }) b->onChange = change;

        scopeButton.setButtonText ("Phase scope...");
        scopeButton.setTooltip ("Open a stereo phase scope (goniometer) for any audio track, Int Bus or Ext Bus of this mixer");
        scopeButton.onClick = [this] { if (app.showScope) app.showScope (mixer); };
        addAndMakeVisible (scopeButton);
        monoButton.setButtonText ("Mono");
        monoButton.setClickingTogglesState (true);
        monoButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe8a317));
        monoButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        monoButton.setTooltip ("Mono check: this mixer's outputs to the interface carry the stereo pair added together (L+R)/2 on both sides, "
                               "so you can hear phase and mono-compatibility problems. Lit = mono. It only changes what you hear, never files or bounces, and is off again when the project opens.");
        if (auto* mx = mixerState()) monoButton.setToggleState (mx->monoCheck.load(), juce::dontSendNotification);
        monoButton.onClick = [this] { if (auto* mx = mixerState()) mx->monoCheck.store (monoButton.getToggleState()); };
        addAndMakeVisible (monoButton);
        addStripButton.setButtonText ("+ Strip...");
        addStripButton.setTooltip ("Add a strip to the project: an Int Bus, an Ext Bus or a VCA (a fader that controls the level of other strips). Int and Ext Buses are added to the whole project (every mixer); a VCA is added to this mixer. Audio strips hidden from this mixer are shown again from here too.");
        addStripButton.onClick = [this] { chooseNewStrip(); };
        addAndMakeVisible (addStripButton);
        styleCaption (widthCap, "STRIP WIDTH"); addAndMakeVisible (widthCap);
        narrowerButton.setButtonText ("-"); widerButton.setButtonText ("+");
        narrowerButton.setTooltip ("Make every strip narrower (fits more strips in the window)");
        widerButton.setTooltip ("Make every strip wider");
        narrowerButton.onClick = [this] { stepWidth (-1); };
        widerButton.onClick = [this] { stepWidth (+1); };
        addAndMakeVisible (narrowerButton); addAndMakeVisible (widerButton);
        showWidth();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 0);
        r.removeFromTop (kBandH + 6);
        resetButton.setBounds (r.removeFromTop (26).reduced (0, 1));
        r.removeFromTop (8);
        auto place = [&] (juce::Label& cap, juce::ComboBox& box)
        {
            cap.setBounds (r.removeFromTop (12));
            box.setBounds (r.removeFromTop (22).reduced (0, 1));
            r.removeFromTop (6);
        };
        place (ditherCap, ditherBox);
        place (modeCap, modeBox); place (riseCap, riseBox); place (fallCap, fallBox); place (holdCap, holdBox);
        r.removeFromTop (4);
        scopeButton.setBounds (r.removeFromTop (26).reduced (0, 1));
        monoButton.setBounds (r.removeFromTop (26).reduced (0, 1));
        r.removeFromTop (8);
        addStripButton.setBounds (r.removeFromTop (26).reduced (0, 1));
        r.removeFromTop (8);
        widthCap.setBounds (r.removeFromTop (12));
        auto row = r.removeFromTop (26).reduced (0, 1);
        narrowerButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (0, 0).withTrimmedRight (2));
        widerButton.setBounds (row.withTrimmedLeft (2));
    }
protected:
    juce::String bandText() const override { return "METERS"; }
private:
    void showWidth()
    {
        widthCap.setText ("STRIP WIDTH " + juce::String (stripWidthLevel() + 1) + " / 6", juce::dontSendNotification);
        narrowerButton.setEnabled (stripWidthLevel() > 0); widerButton.setEnabled (stripWidthLevel() < 5);
    }
    void stepWidth (int d)
    {
        const int lv = juce::jlimit (0, 5, stripWidthLevel() + d);
        if (lv == stripWidthLevel()) return;
        stripWidthLevel() = lv; ++stripWidthGen();
        if (auto* st = app.props.getUserSettings()) { st->setValue ("mixerStripWidth", lv); app.props.saveIfNeeded(); }
        showWidth();
    }
    void chooseNewStrip()
    {
        auto* mx = mixerState();
        if (mx == nullptr) return;
        juce::PopupMenu m, vca;
        m.addItem (1, "Int Bus");
        m.addItem (2, "Ext Bus");
        vca.addItem (10, "New VCA");
        std::vector<juce::Uuid> hidden;
        for (auto& v : app.project.vcas)
            if (! (mx->vcaShown.load() & (1u << (v.number - 1)))) { hidden.push_back (v.id); vca.addItem (100 + (int) hidden.size() - 1, "VCA " + v.label() + "  (already in the project)"); }
        m.addSubMenu ("VCA", vca);
        juce::PopupMenu shows;                                     // audio tracks hidden from this mixer (right-click a name plate: Hide)
        std::vector<juce::Uuid> hiddenTracks;
        for (auto& t : app.project.tracks) if (mx->isStripHidden (t.id)) { hiddenTracks.push_back (t.id); shows.addItem (500 + (int) hiddenTracks.size() - 1, t.name); }
        if (! hiddenTracks.empty()) { shows.addSeparator(); shows.addItem (499, "All of them (" + juce::String ((int) hiddenTracks.size()) + ")"); }
        m.addSeparator();
        m.addSubMenu ("Show a hidden audio strip again", shows, ! hiddenTracks.empty());
        const auto mid = mixer;
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addStripButton), [this, mid, hidden, hiddenTracks] (int r)
        {
            if (r <= 0) return;
            MixerState* mx2 = nullptr;
            for (auto& mm : app.project.mixers) if (mm->id == mid) mx2 = mm.get();
            if (mx2 == nullptr) return;
            if (r == 499 || (r >= 500 && (size_t) (r - 500) < hiddenTracks.size()))
            {
                auto& h = mx2->hiddenStrips;
                if (r == 499) h.clear();
                else h.erase (std::remove (h.begin(), h.end(), hiddenTracks[(size_t) (r - 500)]), h.end());
                app.engine.rebuildPlan(); app.project.markDirty(); app.project.structureChanged();
                return;
            }
            if (app.engine.isRecording()) { showError ("Recording", "Strips cannot be added while recording."); return; }
            if (r == 1 || r == 2)
            {
                const bool ext = r == 2;
                const juce::String kind = ext ? "Ext bus" : "Int bus";
                app.project.addBus (kind + " " + juce::String ((int) app.project.busIndices (ext).size() + 1), ext);
            }
            else if (r == 10)
            {
                if (auto* v = app.project.addVca ({}, false)) app.project.showVca (*mx2, *v, true);
                else showError ("VCAs", "There can be at most " + juce::String (kMaxVcas) + " VCAs.");
            }
            else if (r >= 100 && (size_t) (r - 100) < hidden.size())
                if (auto* v = app.project.findVca (hidden[(size_t) (r - 100)])) app.project.showVca (*mx2, *v, true);
        });
    }
    void setup (juce::ComboBox& box, juce::Label& cap, const juce::String& title, const juce::StringArray& items, int sel, const juce::String& tip)
    {
        styleCaption (cap, title);
        addAndMakeVisible (cap);
        for (int i = 0; i < items.size(); ++i) box.addItem (items[i], i + 1);
        box.setSelectedId (juce::jlimit (0, items.size() - 1, sel) + 1, juce::dontSendNotification);
        box.setTooltip (tip);
        addAndMakeVisible (box);
    }
    MixerState* mixerState() const
    {
        for (auto& m : app.project.mixers) if (m->id == mixer) return m.get();
        return nullptr;
    }
    AppContext& app; juce::Uuid mixer; std::function<void()> onReset;
    juce::TextButton resetButton, scopeButton, monoButton, addStripButton, narrowerButton, widerButton;
    juce::Label widthCap;
    juce::ComboBox ditherBox, modeBox, riseBox, fallBox, holdBox;
    juce::Label ditherCap, modeCap, riseCap, fallCap, holdCap;
};

class MixerStripsHolder : public juce::Component
{
public:
    static constexpr int kEdge = 6;                       // the dark border above and below the strips: as wide as the one on the left
    void paint (juce::Graphics& g) override { g.fillAll (mixTheme().bg); }
    void clear() { strips.clear(); widths.clear(); removeAllChildren(); }      // (the widths too: they used to be kept, so a rebuilt strip took the OLD strip's width)
    void add (StripBase* s, int width = -1) { strips.add (s); widths.add (width < 0 ? stripWidth() : width); addAndMakeVisible (s); }
    void layout (int height)
    {
        int x = 6;
        for (int i = 0; i < strips.size(); ++i) { strips[i]->setBounds (x, kEdge, widths[i], height); x += widths[i] + 3; }
        naturalW = x + 3; naturalH = height + 2 * kEdge;
        setSize (juce::jmax (naturalW, minW), juce::jmax (naturalH, minH));
    }
    /** Keeps the dark ground behind the strips filling the whole window. */
    void setMinimumSize (int w, int h) { minW = w; minH = h; setSize (juce::jmax (naturalW, minW), juce::jmax (naturalH, minH)); }
    int getNaturalWidth() const noexcept  { return naturalW; }
    int getNumStrips() const noexcept { return strips.size(); }
    int getNaturalHeight() const noexcept { return naturalH; }
    void refreshFromModel() { for (auto* s : strips) s->refreshFromModel(); }
    void tick() { gang.tick(); for (auto* s : strips) s->tick(); }
    void resetPeaks() { for (auto* s : strips) s->resetPeaks(); }
    /** After a rebuild: a selected strip that is no longer there (hidden, its track deleted) is no longer selected. */
    void pruneGang()
    {
        auto& sel = gang.selected;
        sel.erase (std::remove_if (sel.begin(), sel.end(), [this] (const juce::Uuid& id)
        {
            for (auto* mm : gang.members) if (mm->gangId() == id) return false;
            return true;
        }), sel.end());
    }
    StripGang gang;                                        // (declared before the strips: it outlives them)
private:
    juce::OwnedArray<StripBase> strips;
    juce::Array<int> widths;
    int naturalW = 0, naturalH = 0, minW = 0, minH = 0;
};

// ============================================================================ window
MixerComponent::MixerComponent (AppContext& a, const juce::Uuid& id) : app (a), mixerId (id), holder (new MixerStripsHolder())
{
    setMixDark (theme::isDark());                           // the mixers follow the program-wide dark / light look
    shownDark = mixTheme().dark; shownGen = theme::generation();
    mixLook.apply();
    viewport.setLookAndFeel (&mixLook);

    addAndMakeVisible (statusLabel);
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, theme::warn);
    if (auto* st = app.props.getUserSettings())              // the meter settings are shared by all mixers
    {
        auto& m = meterSettings();
        m.mode = st->getIntValue ("meterMode", m.mode); m.rise = st->getIntValue ("meterRise", m.rise);
        m.fall = st->getIntValue ("meterFall", m.fall); m.hold = st->getIntValue ("meterHold", m.hold);
        stripWidthLevel() = juce::jlimit (0, 5, st->getIntValue ("mixerStripWidth", stripWidthLevel()));
    }
    shownWidthGen = stripWidthGen();

    themeButton.setTooltip ("Switch the whole program between the light look and the dark look (the same colours, a shade darker)");
    themeButton.onClick = [] { if (themeSwitchHook()) themeSwitchHook() (! theme::isDark()); };
    addAndMakeVisible (themeButton);

    auditionButton.setClickingTogglesState (false);
    auditionButton.onClick = [this] { if (auto* m = mixer()) { app.setAudition (m->id); if (app.isAuditioning (m->id) && app.showMixer) app.showMixer (m->id); } };
    addChildComponent (auditionButton);
    altButton.setTooltip ("Make this the Alt Mixer: the one the second Mixer key of the Stream Deck opens and closes. Lit = it is. (With none chosen, the first cue mixer is used.)");
    altButton.onClick = [this] { if (auto* m = mixer()) { app.project.altMixer = app.project.altMixer == m->id ? juce::Uuid::null() : m->id; app.project.markDirty(); app.project.sendChangeMessage(); } };
    addChildComponent (altButton);
    copyButton.setTooltip ("Copy this mixer: tick faders, sends, pan and inserts, then copy it (to Paste in another mixer) or send it straight to any mixer");
    pasteButton.setButtonText ("Paste");
    pasteButton.onClick = [this] { pasteHere(); };
    addChildComponent (pasteButton);
    copyButton.onClick = [this] { showCopyMenu(); };
    addAndMakeVisible (copyButton);
    deleteButton.setTooltip ("Delete this cue mixer (the processing mixer cannot be deleted)");
    deleteButton.onClick = [this]
    {
        auto* m = mixer();
        if (m == nullptr || app.isEngineerMixer (m->id)) return;
        const auto id = m->id;
        confirmAsync ("Delete cue mixer", "Delete the cue mixer '" + m->name + "'?\n\nIts levels, sends and plug-ins are lost. Nothing else is touched.", "Delete",
                      [this, id] { app.deleteMixer (id); });
    };
    addChildComponent (deleteButton);

    followBox.setTooltip ("Ticked: this cue mixer plays exactly what the processing mixer sends to its main output (plug-ins, automation and all), on every one of "
                          "this mixer's Ext Bus outputs. This mixer's own mix is kept as it is, untouched, and comes back the moment you untick it.");
    followBox.setColour (juce::ToggleButton::textColourId, theme::text);
    followBox.onClick = [this]
    {
        if (auto* m = mixer()) { m->followProcessing.set (followBox.getToggleState()); app.project.markDirty(); }
        updateFollow();
    };
    addChildComponent (followBox);

    nameCaption.setText ("Mixer name:", juce::dontSendNotification);
    addAndMakeVisible (nameCaption);
    nameEditor.setTooltip ("Type a new name and press Enter to rename this mixer");
    if (auto* m = mixer()) nameEditor.setText (m->name, juce::dontSendNotification);
    auto commitName = [this]
    {
        auto* m = mixer();
        const auto t = nameEditor.getText().trim();
        if (m == nullptr || t.isEmpty() || t == m->name) { if (m != nullptr) nameEditor.setText (m->name, juce::dontSendNotification); return; }
        if (! m->editId.isNull())                  // an Edit's mixer carries the name of its Edit: renaming one renames the other
        { if (auto* e = app.project.findEdit (m->editId)) e->name = t; }
        m->name = t;
        app.project.syncEditMixers();
        app.project.structureChanged();            // titles and menus pick the new name up
    };
    nameEditor.onReturnKey = [this, commitName] { commitName(); nameEditor.giveAwayKeyboardFocus(); };     // Enter: done, and the box lets go of the keyboard
    nameEditor.onFocusLost = commitName;
    addAndMakeVisible (nameEditor);

    viewport.setViewedComponent (holder.get(), false);
    viewport.setScrollBarsShown (true, true);
    viewport.setScrollBarThickness (12);
    addAndMakeVisible (viewport);
    rebuild();
    updateAuditionButton();
    app.project.structure.addChangeListener (this);
    app.project.addChangeListener (this);
    startTimerHz (30);
    setSize (naturalContentW(), naturalContentH());
}

MixerComponent::~MixerComponent()
{
    app.project.removeChangeListener (this);
    app.project.structure.removeChangeListener (this);
    viewport.setLookAndFeel (nullptr);
}

int MixerComponent::topBarMinWidth() const
{
    auto* m = mixer();
    const bool other = m != nullptr && ! app.isEngineerMixer (m->id);
    return 84 + 180 + 76 + 140 + (other ? 600 : 0);                // name, the two icons, a little room for messages (a cue mixer: audition, delete, follow)
}
int MixerComponent::slotsNeeded()
{
    auto* m = mixer();
    int highest = -1;                                              // the highest insert slot in use anywhere in this mixer
    if (m != nullptr)
    {
        for (auto& s : m->stripPool) for (int i = 0; i < kNumSlots; ++i) if (s->slots[i].mightBeLoaded()) highest = juce::jmax (highest, i);
        for (auto& b : m->busPool)   for (int i = 0; i < kNumSlots; ++i) if (b->slots[i].mightBeLoaded()) highest = juce::jmax (highest, i);
    }
    slotsBase = juce::jlimit (1, kNumSlots, highest + 2);          // always one free slot beyond the last used
    return juce::jlimit (1, kNumSlots, slotsBase + extraSlots);    // (a big window shows more empty slots)
}
int MixerComponent::naturalContentW() const { return juce::jmax (topBarMinWidth(), holder->getNaturalWidth()); }
/** The height the strips like to have (full-size fader). */
int MixerComponent::naturalContentH() const { return 30 + ChannelStrip::heightFor (maxDialsCached, slotCount) + 2 * MixerStripsHolder::kEdge; }
/** The least height before a scroll bar is needed (the faders shrink to their smallest). */
int MixerComponent::minContentH() const { return 30 + ChannelStrip::heightFor (maxDialsCached, slotCount) - (kFaderH - kFaderMin) + 2 * MixerStripsHolder::kEdge; }

/** Gives the strips the height of the window: more height = more empty insert slots and a taller fader; less = a shorter fader (down to a minimum). */
void MixerComponent::applyHeight()
{
    // the room under the strips: less the horizontal scroll bar when the strips are wider than the window (the bar sits over the bottom of the view, so the
    // strips used to run on under it, hiding their rounded bottom corners and name plates)
    const bool needH = holder->getNaturalWidth() > viewport.getWidth();
    const int availH = viewport.getHeight() - (needH ? viewport.getScrollBarThickness() : 0) - 2 * MixerStripsHolder::kEdge;
    int h = ChannelStrip::heightFor (maxDialsCached, slotCount);
    if (availH > 0)
    {
        const int surplus = availH - ChannelStrip::heightFor (maxDialsCached, slotsBase);
        const int want = juce::jlimit (0, kNumSlots - slotsBase, surplus > 0 ? (surplus * 2 / 5) / kSlotH : 0);
        if (want != extraSlots && ! inApply)
        {
            extraSlots = want; inApply = true; rebuild(); inApply = false; return;
        }
        h = juce::jmax (ChannelStrip::heightFor (maxDialsCached, slotCount) - (kFaderH - kFaderMin), availH);
    }
    holder->layout (h);
}

/** Windows applies the biggest-size limit to the whole window, frame and title bar included, while the sizes here are for the inside: with the limit set exactly to
    the strips' size, the window was cut short by the width of the frame (about 14 x 37 points) and scroll bars appeared. The limit is therefore a little looser; the
    window is still made exactly the strips' size by the code (and snaps back to it when it grows too big). */
static constexpr int kLimitSlack = 120;

/** The biggest inside size the window can have where it is: no wider than its display (its frame included), and no taller than the room from its top
    down to the bottom of the display, so its bottom edge - where the horizontal scroll bar is - is always on the screen. */
static juce::Point<int> roomOnScreen (const juce::DocumentWindow& win)
{
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win.getBounds())->userArea;
    int frameLR = 16, frameBottom = 8;
    if (auto* peer = win.getPeer()) if (peer->getFrameSizeIfPresent()) { const auto f = *peer->getFrameSizeIfPresent(); frameLR = f.getLeftAndRight(); frameBottom = f.getBottom(); }
    return { juce::jmax (300, area.getWidth() - frameLR), juce::jmax (260, area.getBottom() - frameBottom - juce::jmax (win.getY(), area.getY())) };
}

/** The limits for resizing the window by hand: never narrower than the top bar; as wide as the strips need, even past the edge of the display (only
    a drag takes it there: Fermata itself never opens or fits a mixer wider than its display); taller is free (a tall window shows more empty insert
    slots). 'fullW' / 'fullH' are the size that shows everything within the display, 'stripsW' the strips' whole width (frame included). */
static void setMixerLimits (juce::DocumentWindow& win, int minW, int fullW, int fullH, int stripsW)
{
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win.getBounds())->userArea;
    if (auto* c = win.getConstrainer())
    {
        c->setMinimumSize (juce::jmin (minW, fullW), juce::jmin (260, fullH));
        c->setMaximumSize (juce::jmax (stripsW + kLimitSlack, area.getWidth() + 40), juce::jmax (fullH + kLimitSlack, area.getHeight() + 40));
    }
}

/** A window that showed every strip keeps showing every strip when they change (another insert slot, a strip more or less, the strip width) - but
    never wider than the display, nor so tall that its bottom edge is off the screen: then the strips scroll. A smaller window keeps its size. */
void MixerComponent::updateWindowLimits()
{
    auto* win = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    if (win == nullptr || win->getContentComponent() != this || getWidth() <= 0) return;
    // The frame around the strips, from the window itself (not from sizes that may not be settled yet: while the window is first being fitted to this
    // content its own size is still a placeholder, which used to make the limits - and so the window - tiny).
    const auto frame = win->getContentComponentBorder();
    const int borderW = frame.getLeftAndRight(), borderH = frame.getTopAndBottom();
    // The biggest the window may be: what the strips need (so there is no scroll bar at all at that size), within the room on the screen. (It used to
    // be the strips' whole width even when that was wider than the display: the window grew off the edge, the scroll bar went, and a change of strip
    // width left the view at the first strip with the METERS strip somewhere off the screen.)
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
    const auto room = roomOnScreen (*win);
    const int maxW = juce::jmin (naturalContentW() + borderW, room.x);
    const int maxH = juce::jmin (naturalContentH() + borderH, room.y);
    setMixerLimits (*win, topBarMinWidth() + borderW, maxW, maxH, naturalContentW() + borderW);
    const bool firstTime = lastMaxW == 0 || lastMaxH == 0;          // a window that has never been fitted opens showing everything
    const bool wasFullW = firstTime || win->getWidth() == lastMaxW, wasFullH = firstTime || win->getHeight() == lastMaxH;
    lastMaxW = maxW; lastMaxH = maxH;
    const int w = wasFullW ? maxW : win->getWidth();                // (a window dragged wider than the display by hand keeps that width)
    const int h = wasFullH ? maxH : win->getHeight();
    if ((w != win->getWidth() || h != win->getHeight()) && ! win->isFullScreen())
    {
        win->setSize (w, h);
        if (win->getY() < area.getY()) win->setTopLeftPosition (win->getX(), area.getY());
    }
}

void MixerComponent::fitWindowToStrips (juce::Rectangle<int> remembered)
{
    auto* win = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    if (win == nullptr || win->getContentComponent() != this || win->isFullScreen()) return;
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
    if (remembered.isEmpty() && win->getY() < area.getY() + 44) win->setTopLeftPosition (win->getX(), area.getY() + 44);   // a new window opens below the menu bar
    const auto frame = win->getContentComponentBorder();
    const auto room = roomOnScreen (*win);                          // (from where its top now is)
    const int fullW = juce::jmin (naturalContentW() + frame.getLeftAndRight(), room.x);
    const int fullH = juce::jmin (naturalContentH() + frame.getTopAndBottom(), room.y);
    int w = fullW, h = fullH;
    if (! remembered.isEmpty())                                     // the size it had when it was last closed, within the same limits
    {
        w = juce::jlimit (juce::jmin (topBarMinWidth() + frame.getLeftAndRight(), fullW), fullW, remembered.getWidth());
        h = juce::jlimit (juce::jmin (260, room.y), room.y, remembered.getHeight());
    }
    setMixerLimits (*win, topBarMinWidth() + frame.getLeftAndRight(), fullW, fullH, naturalContentW() + frame.getLeftAndRight());
    lastMaxW = fullW; lastMaxH = fullH;
    win->setSize (w, h);
    if (win->getRight() > area.getRight()) win->setTopLeftPosition (juce::jmax (area.getX(), area.getRight() - win->getWidth()), win->getY());   // all of it on its display
}

void MixerComponent::parentHierarchyChanged() { updateWindowLimits(); }

MixerState* MixerComponent::mixer() const
{
    for (auto& m : app.project.mixers) if (m->id == mixerId) return m.get();
    return nullptr;
}

void MixerComponent::selectStrip (const juce::Uuid& trackId)
{
    for (auto* mm : holder->gang.members) if (mm->gangId() == trackId) { holder->gang.click (trackId, juce::ModifierKeys()); return; }
}
bool MixerComponent::isStripSelected (const juce::Uuid& trackId) const { return holder->gang.isSelected (trackId); }

void MixerComponent::rebuild()
{
    holder->clear();
    auto* m = mixer();
    if (m == nullptr) return;
    if (! nameEditor.hasKeyboardFocus (true)) nameEditor.setText (m->name, juce::dontSendNotification);
    app.project.syncMixers();
    slotCount = slotsNeeded();
    int maxDials = 0;
    juce::Array<ChannelStrip*> chans; juce::Array<BusStrip*> busStrips;
    for (auto& t : app.project.tracks)
    {
        if (m->isStripHidden (t.id)) continue;                    // taken out of this mixer (+ Strip shows it again)
        auto* cs = new ChannelStrip (app, *m, t, slotCount, &holder->gang);
        chans.add (cs); maxDials = juce::jmax (maxDials, cs->dialCount());
        holder->add (cs);
    }
    holder->pruneGang();
    for (int pass = 0; pass < 2; ++pass)                          // Int Buses first, then Ext Buses
        for (auto& b : app.project.buses)
            if (b.external == (pass == 1))
            {
                auto* bs = new BusStrip (app, *m, b, slotCount);
                busStrips.add (bs); maxDials = juce::jmax (maxDials, bs->dialCount());
                holder->add (bs);
            }
    juce::Array<VcaStrip*> vcaStrips;
    for (auto& v : app.project.vcas)                              // VCA strips come after the buses
        if (m->vcaShown.load() & (1u << (v.number - 1)))
        {
            auto* vs = new VcaStrip (app, *m, v);
            vcaStrips.add (vs); holder->add (vs);
        }
    const int rows = juce::jmax (1, (maxDials + 1) / 2);
    for (auto* c : chans) c->setSendRows (rows);
    for (auto* b : busStrips) b->setSendRows (rows);
    for (auto* v : vcaStrips) { v->setSendRows (rows); v->setSlotCount (slotCount); }
    holder->add (new MeterSettingsStrip (app, mixerId, [this] { holder->resetPeaks(); }), 112);
    {   // no fader, pan or send dial in a mixer turns with the mouse wheel: it passes the wheel on to the view, which moves the strips left / right
        std::function<void (juce::Component&)> noWheel = [&noWheel] (juce::Component& c)
        {
            if (auto* s = dynamic_cast<juce::Slider*> (&c)) s->setScrollWheelEnabled (false);
            for (auto* ch : c.getChildren()) noWheel (*ch);
        };
        noWheel (*holder);
    }
    maxDialsCached = maxDials;
    applyHeight();
    updateBars();
    updateWindowLimits();
}

/** Decides which scroll bars are really needed and gives the holder exactly the visible size as its minimum (never more, or a bar would appear
    just because the other bar took some room). */
void MixerComponent::updateBars()
{
    const int bar = viewport.getScrollBarThickness();
    const int w = viewport.getWidth(), h = viewport.getHeight();
    const int nw = holder->getNaturalWidth(), nh = minContentH() - 30;
    bool needV = h < nh, needH = (w - (needV ? bar : 0)) < nw;
    needV = (h - (needH ? bar : 0)) < nh;
    needH = (w - (needV ? bar : 0)) < nw;
    viewport.setScrollBarsShown (needV, needH);
    holder->setMinimumSize (w - (needV ? bar : 0), h - (needH ? bar : 0));
}

void MixerComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &app.project)                                   // a colour, a level or a name changed somewhere: refresh what is drawn
    {
        holder->refreshFromModel();
        holder->repaint();
        return;
    }
    rebuild(); updateAuditionButton(); resized();
}

void MixerComponent::updateAuditionButton()
{
    auto* m = mixer();
    const bool other = m != nullptr && ! app.isEngineerMixer (m->id) && m->editId.isNull();      // (an Edit's mixer is not a cue mixer: no audition, no delete)
    const bool on = m != nullptr && app.isAuditioning (m->id);
    auditionButton.setVisible (other);
    deleteButton.setVisible (other);
    if (followBox.isVisible() != other) { followBox.setVisible (other); resized(); }
    if (m != nullptr && (followBox.getToggleState() != m->followProcessing.get() || (followCover != nullptr) != (other && m->followProcessing.get()))) updateFollow();
    altButton.setVisible (other);
    altButton.setGlow (other && m != nullptr && app.project.altMixer == m->id);
    auditionButton.setButtonText ((on ? "AUDITIONING: " : "") + (m != nullptr ? m->name : juce::String()) + " Audition");
    auditionButton.setToggleState (on, juce::dontSendNotification);
    auditionButton.setColour (juce::TextButton::buttonColourId, theme::button);
    auditionButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9822b));
    auditionButton.setTooltip ("Listen to this mixer on your own outputs, and edit it, until you press this again");
    {
        const bool showPaste = app.canPasteMix();
        pasteButton.setTooltip (showPaste ? "Paste \"" + app.mixClipLabel() + "\" here (the parts you ticked when you copied it)" : juce::String());
        if (pasteButton.isVisible() != showPaste) { pasteButton.setVisible (showPaste); resized(); }
    }
    themeButton.setDarkLook (mixTheme().dark);
    themeButton.setTooltip (mixTheme().dark ? "Dark look is on (the desk a shade darker). Click for the light look." : "Light look is on. Click for the dark look (the same colours, a shade darker).");
}

/** While a cue mixer follows the processing mixer: a cover over its strips, which rest (kept as they were) until the box is unticked. */
namespace
{
class FollowCover : public juce::Component
{
public:
    FollowCover() { setInterceptsMouseClicks (true, true); }      // (the strips under it can't be touched while they rest)
    void paint (juce::Graphics& g) override
    {
        g.fillAll (mixTheme().bg.withAlpha (0.82f));
        auto r = getLocalBounds().reduced (24).withSizeKeepingCentre (juce::jmin (620, getWidth() - 48), 120);
        g.setColour (theme::accent.withAlpha (0.18f)); g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (theme::accent); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.5f);
        g.setColour (mixTheme().text);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText ("Following the processing mixer", r.removeFromTop (48).withTrimmedTop (14), juce::Justification::centred, false);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Every Ext Bus output of this mixer plays the processing mixer's main mix.\nThis mixer's own mix is kept as it was: untick \"Follow Processing Mixer\" to use it again.",
                          r.reduced (16, 4), juce::Justification::centredTop, 3);
    }
};
}

void MixerComponent::updateFollow()
{
    auto* m = mixer();
    const bool other = m != nullptr && ! app.isEngineerMixer (m->id) && m->editId.isNull();
    const bool on = other && m->followProcessing.get();
    followBox.setToggleState (on, juce::dontSendNotification);
    if (on && followCover == nullptr) { followCover = std::make_unique<FollowCover>(); addAndMakeVisible (*followCover); followCover->setBounds (viewport.getBounds()); }
    if (! on && followCover != nullptr) followCover.reset();      // (the engine reads the flag at every block: nothing else to do)
}

namespace
{
/** The panel that opens from a mixer's copy button: tick what to take, then copy it (to paste into another mixer) or send it straight to a mixer. */
struct CopyMixPanel : public juce::Component
{
    // choice: -1 = copy only (paste it elsewhere), -2 = all other mixers, -3 = undo, 0.. = the mixer at that place in the list
    std::function<void (const Project::MixParts&, int)> onChoose;
    CopyMixPanel (const juce::String& from, const Project::MixParts& start, const std::vector<juce::String>& others, bool canUndo)
    {
        title.setText ("Copy mixer \"" + from + "\"", juce::dontSendNotification);
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold)); title.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (title);
        const char* names[4] = { "Faders (levels, mutes, solos, output buttons)", "Sends", "Pan", "Inserts (plug-ins with their settings)" };
        const bool on[4] = { start.faders, start.sends, start.pan, start.inserts };
        for (int k = 0; k < 4; ++k)
        {
            ticks[k].setButtonText (names[k]); ticks[k].setToggleState (on[k], juce::dontSendNotification);
            ticks[k].setColour (juce::ToggleButton::textColourId, juce::Colours::white);
            addAndMakeVisible (ticks[k]);
        }
        copyBtn.setButtonText ("Copy (then press Paste in another mixer)");
        copyBtn.onClick = [this] { choose (-1); };
        addAndMakeVisible (copyBtn);
        sendLbl.setText ("Or send it straight to:", juce::dontSendNotification);
        sendLbl.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        addAndMakeVisible (sendLbl);
        for (size_t k = 0; k < others.size(); ++k)
        {
            auto b = std::make_unique<juce::TextButton> (others[k]);
            b->onClick = [this, k] { choose ((int) k); };
            addAndMakeVisible (*b); sendBtns.push_back (std::move (b));
        }
        if (others.size() > 1)
        {
            allBtn.setButtonText ("All other mixers"); allBtn.onClick = [this] { choose (-2); };
            addAndMakeVisible (allBtn);
        }
        undoBtn.setButtonText ("Undo last copy / paste"); undoBtn.setEnabled (canUndo);
        undoBtn.onClick = [this] { choose (-3); };
        addAndMakeVisible (undoBtn);
        setSize (330, 12 + 24 + 4 * 24 + 8 + 30 + 8 + 20 + (int) sendBtns.size() * 28 + (others.size() > 1 ? 28 : 0) + 8 + 28 + 12);
    }
    Project::MixParts parts() const { return { ticks[0].getToggleState(), ticks[1].getToggleState(), ticks[2].getToggleState(), ticks[3].getToggleState() }; }
    void choose (int c)
    {
        if (c != -3 && ! (ticks[0].getToggleState() || ticks[1].getToggleState() || ticks[2].getToggleState() || ticks[3].getToggleState())) return;   // nothing ticked
        auto cb = onChoose; const auto p = parts();
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
        if (cb) cb (p, c);
    }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff2a2d33)); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 12);
        title.setBounds (r.removeFromTop (24));
        for (auto& t : ticks) t.setBounds (r.removeFromTop (24));
        r.removeFromTop (8);
        copyBtn.setBounds (r.removeFromTop (30)); r.removeFromTop (8);
        sendLbl.setBounds (r.removeFromTop (20));
        for (auto& b : sendBtns) { b->setBounds (r.removeFromTop (26)); r.removeFromTop (2); }
        if (allBtn.getButtonText().isNotEmpty()) { allBtn.setBounds (r.removeFromTop (26)); r.removeFromTop (2); }
        r.removeFromTop (6);
        undoBtn.setBounds (r.removeFromTop (26));
    }
    juce::Label title, sendLbl;
    juce::ToggleButton ticks[4];
    juce::TextButton copyBtn, allBtn, undoBtn;
    std::vector<std::unique_ptr<juce::TextButton>> sendBtns;
};
}

void MixerComponent::showCopyMenu()
{
    auto* me = mixer();
    if (me == nullptr || app.project.mixers.empty()) return;
    const auto myId = me->id;
    std::vector<juce::Uuid> ids; std::vector<juce::String> names;
    for (auto& m : app.project.mixers) if (m->id != myId) { ids.push_back (m->id); names.push_back (m->name); }
    auto panel = std::make_unique<CopyMixPanel> (me->name, app.mixClipParts, names, app.canUndoCopyMix());
    juce::Component::SafePointer<MixerComponent> self (this);
    panel->onChoose = [self, myId, ids] (const Project::MixParts& p, int c)
    {
        if (self == nullptr) return;
        auto& app = self->app;
        app.mixClipParts = p;
        juce::String msg;
        if (c == -3) msg = app.undoCopyMix();
        else if (c == -1)
        {
            msg = app.copyMixToClipboard (myId, p);
        }
        else if (c == -2) msg = app.copyMix (myId, {}, p);
        else if (juce::isPositiveAndBelow (c, (int) ids.size())) msg = app.copyMix (myId, ids[(size_t) c], p);
        else return;
        self->statusLabel.setText (msg, juce::dontSendNotification);
        self->statusFlash = 120;
        self->updateAuditionButton();
    };
    juce::CallOutBox::launchAsynchronously (std::move (panel), copyButton.getScreenBounds(), nullptr);
}

void MixerComponent::pasteHere()
{
    auto* me = mixer();
    if (me == nullptr) return;
    statusLabel.setText (app.pasteMix (me->id), juce::dontSendNotification);
    statusFlash = 120;
    updateAuditionButton();
}

void MixerComponent::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30);
    themeButton.setBounds (top.removeFromRight (34).withSizeKeepingCentre (28, 24));
    top.removeFromRight (2 + 32);                                    // room for the pin button, which sits just left of the sun / moon in the window's own title strip
    copyButton.setBounds (top.removeFromRight (34).withSizeKeepingCentre (28, 24));
    if (pasteButton.isVisible()) { pasteButton.setBounds (top.removeFromRight (64).reduced (2, 3)); }
    if (altButton.isVisible()) { altButton.setBounds (top.removeFromRight (34).withSizeKeepingCentre (30, 24)); top.removeFromRight (2); }
    if (deleteButton.isVisible()) deleteButton.setBounds (top.removeFromRight (150).reduced (3));
    if (followBox.isVisible()) followBox.setBounds (top.removeFromRight (190).reduced (2, 3));
    if (auditionButton.isVisible()) auditionButton.setBounds (top.removeFromRight (260).reduced (3));
    nameCaption.setBounds (top.removeFromLeft (84).reduced (4, 0));
    nameEditor.setBounds (top.removeFromLeft (180).reduced (0, 3));
    statusLabel.setBounds (top.reduced (8, 0));
    // Big enough for every strip: no scroll bars at all (never one that is only there because the other one takes some room). Smaller: bars.
    viewport.setBounds (r);
    if (followCover != nullptr) followCover->setBounds (r);
    if (! inApply && holder->getNumStrips() > 0) applyHeight();
    updateBars();
    if (! inLimits) { inLimits = true; updateWindowLimits(); inLimits = false; }
}

void MixerComponent::timerCallback()
{
    if (shownDark != mixTheme().dark || shownGen != theme::generation() || shownWidthGen != stripWidthGen())   // the look, the desk colour or the strip width changed (here or in another window)
    {
        const bool widthChanged = shownWidthGen != stripWidthGen();
        shownDark = mixTheme().dark; shownGen = theme::generation(); shownWidthGen = stripWidthGen();
        // a strip width change keeps the meter strip (the last one, with the - / + buttons) where it is: the window keeps its right edge, and the view
        // keeps the same distance from the right end of the strips, so - / + can be pressed again and again without the strip moving away
        auto* win = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
        const int oldRight = win != nullptr ? win->getRight() : 0;
        const int rightGap = juce::jmax (0, holder->getWidth() - (viewport.getViewPositionX() + viewport.getViewWidth()));
        mixLook.apply();
        rebuild(); updateAuditionButton(); resized();       // (a window that showed every strip takes their new width; a smaller one keeps its size)
        if (widthChanged)
        {
            if (win != nullptr && win->getContentComponent() == this && ! win->isFullScreen())
            {
                const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
                win->setTopLeftPosition (juce::jmax (area.getX(), oldRight - win->getWidth()), win->getY());     // growing / shrinking to the left
            }
            viewport.setViewPosition (juce::jmax (0, holder->getWidth() - viewport.getViewWidth() - rightGap), viewport.getViewPositionY());
        }
        viewport.sendLookAndFeelChange();
        repaint();
    }
    holder->tick();
    const auto scan = app.plugins.scanStatus();
    if (statusFlash > 0) --statusFlash;
    else statusLabel.setText (app.plugins.isScanning() ? scan : juce::String(), juce::dontSendNotification);
    updateAuditionButton();
    if (auditionButton.isVisible() && auditionButton.getWidth() == 0) resized();
    if (++refreshCounter % 10 == 0)                         // 3 Hz: plug-in names, solo / mute changes
    {
        app.engine.checkLatency();                          // a plug-in's delay changed (loaded, bypassed ...): line the paths up again
        if (slotsNeeded() != slotCount) { rebuild(); resized(); }        // an insert was used (or removed): every strip gets the new number of slots
        else holder->refreshFromModel();
    }
}
} // namespace td

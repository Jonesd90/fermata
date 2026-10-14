#include "MixerWindow.h"
#include "ColourPicker.h"
#include "ValueEntry.h"

namespace td
{
namespace
{
constexpr int kStripW = 90;
constexpr int kBandH = 16, kPlateH = 26, kInputRowH = 19, kSlotH = 16, kSendCellH = 50, kPanRowH = 30, kMSH = 24, kFaderH = 280, kFaderMin = 110, kOutRowH = 20, kSecH = 11, kToTrackH = 20, kLatH = 13;

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

/** Vertical fader with a finely divided dB scale on its left, a stereo peak meter on its right (on the SAME scale, so the
    markings line up), the held peak above and the fader value below. */
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
        slider.setDoubleClickReturnValue (true, 0.0, juce::ModifierKeys::ctrlModifier);       // Ctrl + click = back to 0 dB; a double-click opens the box to type a level
        slider.onDoubleClick = [this] { askValue(); };
        slider.getProperties().set ("fader", true);
        slider.setValue (initialDb <= -99.0f ? -80.0 : (double) initialDb, juce::dontSendNotification);
        slider.onValueChange = [this] { const float db = (float) slider.getValue(); updateLabel(); changed (db <= -79.5f ? -100.0f : db); };
        meterL.useFaderScale(); meterR.useFaderScale();
        addAndMakeVisible (slider); addAndMakeVisible (meterL); addAndMakeVisible (meterR);
        addAndMakeVisible (label); addAndMakeVisible (peakLabel);
        styleField (label, 10.5f);
        styleField (peakLabel, 9.5f);
        peakLabel.setTooltip ("Highest peak so far. Held for 10 seconds, or until 'Reset peaks' if it went above -2 dBFS.");
        label.setTooltip ("Fader level in dB. Double-click the fader to type a level (Ctrl + click goes back to 0).");
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
    void pushPeaks (float l, float r, float rmsL, float rmsR)
    {
        meterL.setLevel (l, rmsL); meterR.setLevel (r, rmsR);
        const float pk = juce::jmax (meterL.getHoldDb(), meterR.getHoldDb());
        const bool latched = meterL.isLatched() || meterR.isLatched();
        peakLabel.setText (pk <= -99.0f ? juce::String() : juce::String (pk, 1), juce::dontSendNotification);
        peakLabel.setColour (juce::Label::textColourId, latched ? juce::Colour (0xffe03a2c) : mixTheme().text);
    }
    void resetPeaks() { meterL.resetPeak(); meterR.resetPeak(); peakLabel.setText ({}, juce::dontSendNotification); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = mixTheme();
        const float top = (float) travelTop, bottom = (float) travelBottom, span = bottom - top;
        auto yOf = [&] (float db) { return bottom - span * LevelMeter::faderFrac (db); };
        g.setFont (juce::FontOptions (8.5f));
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
        label.setBounds (r.removeFromBottom (15).withSizeKeepingCentre (62, 15));
        peakLabel.setBounds (r.removeFromTop (14).withSizeKeepingCentre (62, 14));
        r.removeFromTop (3); r.removeFromBottom (3);
        const int scaleW = 20, sliderW = 28, meterW = 8, gap = 1, between = 5;
        const int total = scaleW + sliderW + between + meterW * 2 + gap;
        const int x0 = r.getX() + juce::jmax (0, (r.getWidth() - total) / 2);
        scaleRect  = { x0, r.getY(), scaleW, r.getHeight() };
        sliderRect = { x0 + scaleW, r.getY(), sliderW, r.getHeight() };
        slider.setBounds (sliderRect);
        travelTop = r.getY() + MixerLookAndFeel::kCapHalf; travelBottom = r.getBottom() - MixerLookAndFeel::kCapHalf;
        const int mx = x0 + scaleW + sliderW + between;
        meterL.setBounds (mx, travelTop, meterW, travelBottom - travelTop);
        meterR.setBounds (mx + meterW + gap, travelTop, meterW, travelBottom - travelTop);
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
    std::function<void (float)> changed;
    juce::Colour accent { 0 };
    juce::Rectangle<int> scaleRect, sliderRect; int travelTop = 0, travelBottom = 0;
};

class SlotButtons;

/** One insert slot as drawn: the name (click it to open the plug-in straight away) and a little dot on the right (click it for the menu).
    Drag the name onto another slot, in this mixer or another one, to MOVE the plug-in with all its settings (two plug-ins swap places);
    hold Shift while dropping to put a COPY there instead, with all its current settings. */
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
                           "Drag the name onto another slot, in any mixer, to move the plug-in with its settings; hold Shift to drop a copy instead.");
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
            dst.set (std::move (p), sr, blk, 2);
            dst.bypass.set (src.bypass.get());
        }
        else
        {
            app.plugins.closeEditorsFor (sp); app.plugins.closeEditorsFor (dst.getProcessorUnsafe());
            const bool bs = src.bypass.get(), bd = dst.bypass.get();
            auto pa = src.take(); auto pb = dst.take();                        // the one that was there (if any) swaps places with it
            dst.set (std::move (pa), sr, blk, 2); dst.bypass.set (bs);
            if (pb != nullptr) { src.set (std::move (pb), sr, blk, 2); src.bypass.set (bd); }
            else src.bypass.set (false);
        }
        app.project.markDirty(); app.engine.checkLatency();
        refresh(); target.refresh();
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
        if (r == 1) { app.plugins.showEditor (slot.getProcessorUnsafe()); return; }
        if (r == 2) { slot.bypass.set (! slot.bypass.get()); app.project.markDirty(); app.engine.checkLatency(); refresh(); return; }
        if (r == 3)
        {
            app.plugins.closeEditorsFor (slot.getProcessorUnsafe());
            slot.clear (app.engine.getSampleRate(), app.engine.getMaxBlock(), 2);
            app.project.markDirty(); app.engine.checkLatency(); refresh(); return;
        }
        if (r == 9)
        {
            auto safe = juce::Component::SafePointer<SlotButtons> (this);
            app.plugins.startScan ([safe] { if (safe != nullptr) safe->refresh(); });
            return;
        }
        if (r >= 1000 && (size_t) (r - 1000) < descs.size())
        {
            juce::String err;
            auto p = app.plugins.create (descs[(size_t) (r - 1000)], app.engine.getSampleRate(), app.engine.getMaxBlock(), err);
            if (p == nullptr) { showError ("Plug-in", "Could not load " + descs[(size_t) (r - 1000)].name + ":\n" + err); return; }
            app.plugins.closeEditorsFor (slot.getProcessorUnsafe());
            slot.set (std::move (p), app.engine.getSampleRate(), app.engine.getMaxBlock(), 2);
            app.project.markDirty(); app.engine.checkLatency(); refresh();
            app.plugins.showEditor (slot.getProcessorUnsafe());
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
    if (hovered != nullptr) { hovered->dropHighlight = true; hovered->dropCopy = e.mods.isShiftDown(); hovered->repaint(); }
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
        if (target != nullptr && target != this) owner.dropTo (index, target->owner, target->index, e.mods.isShiftDown());
        return;
    }
    if (e.mods.isPopupMenu() || (downOnDot && dotArea().contains (e.getPosition()))) { owner.menu (index); return; }
    if (getLocalBounds().contains (e.getPosition())) owner.openOrMenu (index);
}

juce::TextButton* makeToggle (juce::Component& parent, juce::OwnedArray<juce::TextButton>& list, const juce::String& text, juce::Colour on)
{
    auto* b = list.add (new juce::TextButton (text));
    b->setClickingTogglesState (true);
    b->setColour (juce::TextButton::buttonOnColourId, on);
    parent.addAndMakeVisible (b);
    return b;
}
} // namespace


/** What opens when you click (without dragging) a send dial: the level on a small fader, pre / post fader, and up to two inserts on the send. */
class SendPanel : public juce::Component
{
public:
    SendPanel (AppContext& a, MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, std::function<void()> changed)
        : app (a), mixer (m), srcId (src), destId (dest), onChanged (std::move (changed)), send (*app.project.sendsOf (m, src)->get (dest)),
          slots (a, send.slots, kSendSlots)
    {
        title.setText (app.project.nodeName (src) + "  >  " + app.project.nodeName (dest), juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (title);
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
        fader.setRange (kSendOffDb, 6.0, 0.1);
        fader.setSkewFactorFromMidPoint (-18.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.textFromValueFunction = [] (double v) { return v <= kSendOffDb + 0.05 ? juce::String ("off") : juce::String (v, 1) + " dB"; };
        fader.setValue (send.active() ? (double) send.gainDb.get() : (double) kSendOffDb, juce::dontSendNotification);
        fader.onValueChange = [this]
        {
            const float db = fader.getValue() <= kSendOffDb + 0.05 ? -100.0f : (float) fader.getValue();
            if (app.setSendLevel (mixer, srcId, destId, db) == SendResult::Refused) fader.setValue (kSendOffDb, juce::dontSendNotification);
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
        preButton.onClick  = [this] { send.pre.set (true);  app.project.markDirty(); if (onChanged) onChanged(); };
        postButton.onClick = [this] { send.pre.set (false); app.project.markDirty(); if (onChanged) onChanged(); };
        insertsCaption.setText ("Inserts on this send only", juce::dontSendNotification);
        insertsCaption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        insertsCaption.setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (insertsCaption); addAndMakeVisible (slots);
        setSize (250, 196);
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
        r.removeFromTop (8);
        insertsCaption.setBounds (r.removeFromTop (16));
        slots.setBounds (r.removeFromTop (kSendSlots * 28));
    }
private:
    AppContext& app; MixerState& mixer; juce::Uuid srcId, destId; std::function<void()> onChanged; SendState& send;
    juce::Label title, insertsCaption; juce::Slider fader; juce::TextButton preButton { "PRE" }, postButton { "POST" };
    SlotButtons slots;
};

/** A rotary dial that sends a channel to another channel: drag up / down to change the level, click once for the details.
    The arc of the dial takes the colour of the channel it sends to. */
class SendDial : public juce::Component, private juce::Timer
{
public:
    SendDial (AppContext& a, MixerState& m, const juce::Uuid& src, const juce::Uuid& dest, bool toTrack)
        : app (a), mixer (m), srcId (src), destId (dest), destName (app.project.nodeName (dest))
    {
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
            if (app.setSendLevel (mixer, srcId, destId, db) == SendResult::Refused) refuse();
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
        if (! refused)
        {
            label.setText (destName + (s != nullptr && s->pre.get() ? " (pre)" : ""), juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, s != nullptr && s->active() ? mixTheme().text : mixTheme().dim);
        }
        knob.setTooltip ("Send to " + destName + ". Drag up / down to change, double-click for 0 dB (Ctrl + double-click to type a level), click once for pre / post fader, inserts and a fader. "
                         + (s != nullptr && s->pre.get() ? "(Taken before the fader.)" : "(Taken after the fader.)"));
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
        value.setText (v <= kSendOffDb + 0.05 ? juce::String ("off") : juce::String (v, 1), juce::dontSendNotification);
        value.setColour (juce::Label::textColourId, v <= kSendOffDb + 0.05 ? mixTheme().dim : mixTheme().text);
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
        auto panel = std::make_unique<SendPanel> (app, mixer, srcId, destId, [safe] { if (safe != nullptr) safe->refreshFromModel(); });
        juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
    }
    AppContext& app; MixerState& mixer; juce::Uuid srcId, destId; juce::String destName;
    bool refused = false;
    juce::Label label, value; Knob knob;
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
        g.setColour (t.strip.interpolatedWith (c0, t.dark ? 0.22f : 0.16f)); g.fillRect (b);                      // the strip itself is tinted with the channel colour
        for (auto& w : wells) { g.setColour (t.well.interpolatedWith (c0, t.dark ? 0.13f : 0.10f)); g.fillRect (w); g.setColour (t.soft.interpolatedWith (c0, 0.3f)); g.drawRect (w, 1); }
        g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
        for (auto& s : sections)
        {
            const int tw = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (8.5f, juce::Font::bold), s.first);
            g.setColour (t.dim); g.drawText (s.first, s.second.getX(), s.second.getY(), tw + 2, s.second.getHeight(), juce::Justification::centredLeft, false);
            g.setColour (t.soft.interpolatedWith (c0, 0.3f)); g.fillRect (s.second.getX() + tw + 6, s.second.getCentreY(), juce::jmax (0, s.second.getWidth() - tw - 6), 1);
        }
        g.setColour (t.line); g.drawRect (b, 1);

        const auto c = c0;
        auto band = bandRect();
        g.setColour (c);                         g.fillRect (band);
        g.setColour (c.brighter (0.45f));        g.fillRect (band.getX(), band.getY(), band.getWidth(), 1);
        g.setColour (c.darker (0.45f));          g.fillRect (band.getX(), band.getBottom() - 1, band.getWidth(), 1);
        g.setColour (chan::textOn (c));          g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (bandText(), band.reduced (4, 0), juce::Justification::centredLeft, true);

        if (const int lat = shownLatency(); lat >= 0)
        {
            const bool zero = lat == 0;
            g.setColour (zero ? t.dim : t.text);
            g.setFont (juce::FontOptions (9.5f, zero ? juce::Font::plain : juce::Font::bold));
            g.drawText (juce::String (lat) + (lat == 1 ? " sample" : " samples"), latencyRect(), juce::Justification::centred, false);
        }
        if (appPtr != nullptr)
        {
            auto plate = plateRect();
            g.setColour (c);                     g.fillRect (plate);
            g.setColour (c.brighter (0.45f));    g.fillRect (plate.getX(), plate.getY(), plate.getWidth(), 1);
            g.setColour (c.darker (0.45f));      g.fillRect (plate.getX(), plate.getBottom() - 1, plate.getWidth(), 1);
            g.setColour (chan::textOn (c));      g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawFittedText (plateText(), plate.reduced (3, 2), juce::Justification::centred, 2, 0.85f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (appPtr == nullptr || nodeId.isNull()) return;
        const auto area = bandRect().contains (e.getPosition()) ? bandRect() : plateRect().contains (e.getPosition()) ? plateRect() : juce::Rectangle<int>();
        if (area.isEmpty()) return;
        showColourPickerAt (*appPtr, nodeId, area.translated (getScreenX(), getScreenY()));
    }

protected:
    virtual juce::String bandText() const { return {}; }
    virtual juce::String plateText() const { return {}; }
    juce::Rectangle<int> bandRect() const  { return { 1, 1, getWidth() - 2, kBandH }; }
    juce::Rectangle<int> plateRect() const { return { 1, getHeight() - 1 - kPlateH, getWidth() - 2, kPlateH }; }

    /** The area between the two coloured bars, with a little air on each side. */
    juce::Rectangle<int> body() const { return getLocalBounds().withTrimmedTop (1 + kBandH + 3).withTrimmedBottom (1 + kPlateH + 3 + (shownLatency() >= 0 ? kLatH : 0)).reduced (4, 0); }
    /** Delay compensation: the samples this channel's plug-ins hold the sound back by (-1: this kind of strip shows nothing). */
    virtual int shownLatency() const { return -1; }
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

/** The output of a strip, after its fader: an on / off button and the place it goes to (the mixer's main Ext Bus, or any Int / Ext Bus).
    On an Ext Bus strip the same spot holds the tick box that makes it the mixer's main output. */
class StripOutputRow : public juce::Component
{
public:
    StripOutputRow (AppContext& a, MixerState& m, const juce::Uuid& channel, bool isTrack, bool isExt, Flag* onFlag, juce::Uuid* destPtr)
        : app (a), mixer (m), id (channel), ext (isExt), on (onFlag), dest (destPtr)
    {
        if (ext)
        {
            mainBox.setButtonText ("Main out");
            mainBox.setTooltip ("Tick to make this Ext Bus the main output of this mixer. Strips whose output is set to 'Main' go here.");
            mainBox.onClick = [this]
            {
                if (updating) return;
                mixer.mainBus = id;
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
            box.setTooltip ("Where the output of this strip (after the fader) goes: the main Ext Bus, or any Int / Ext Bus of this mixer");
            box.onChange = [this] { if (! updating) choose(); };
            addAndMakeVisible (button); addAndMakeVisible (box);
        }
        fill();
    }
    void fill()
    {
        if (ext) return;
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
        for (auto& b : app.project.buses) if (! b.external && b.id != id) { ids.push_back (b.id); box.addItem ("Int  " + b.name, next++); }
        for (auto& b : app.project.buses) if (b.external)                 { ids.push_back (b.id); box.addItem ("Ext  " + b.name, next++); }
        updating = false;
        refresh();
    }
    void refresh()
    {
        updating = true;
        if (ext)
        {
            mainBox.setToggleState (effectiveMain() == id, juce::dontSendNotification);
        }
        else
        {
            if (button.getToggleState() != on->get()) button.setToggleState (on->get(), juce::dontSendNotification);
            int want = 1;
            for (size_t i = 0; i < ids.size(); ++i) if (! dest->isNull() && ids[i] == *dest) want = (int) i + 2;
            if (box.getSelectedId() != want) box.setSelectedId (want, juce::dontSendNotification);
            const auto mainId = effectiveMain();
            const auto* mb = app.project.findBus (mainId);
            box.setTooltip ("Where the output of this strip goes (after the fader). 'Main' = " + (mb != nullptr ? mb->name : juce::String ("no Ext Bus yet")));
        }
        updating = false;
    }
    void resized() override
    {
        auto r = getLocalBounds();
        if (ext) { mainBox.setBounds (r); return; }
        button.setBounds (r.removeFromLeft (34).reduced (0, 1)); r.removeFromLeft (3);
        box.setBounds (r.reduced (0, 1));
    }
private:
    juce::Uuid effectiveMain() const
    {
        const auto* mb = app.project.findBus (mixer.mainBus);
        if (mb != nullptr && mb->external) return mixer.mainBus;
        for (auto& b : app.project.buses) if (b.external) return b.id;
        return juce::Uuid::null();
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

class ChannelStrip : public StripBase
{
public:
    ChannelStrip (AppContext& a, MixerState& m, const TrackDef& t, int slotCount)
        : StripBase (&a, t.id), app (a), mixer (m), trackId (t.id), strip (*m.stripFor (t.id)), numCh (t.channelCount()),
          slots (a, strip.slots, slotCount),
          fader ([this] (float db) { strip.gainDb.set (db); app.project.markDirty(); }, strip.gainDb.get()),
          outRow (a, m, t.id, true, false, &strip.outOn, &strip.outDest)
    {
        nSlots = slotCount;
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
            s->setDoubleClickReturnValue (true, def, juce::ModifierKeys::ctrlModifier);
            {
                juce::Component::SafePointer<juce::Slider> safeS (s);
                s->onDoubleClick = [safeS]
                {
                    if (safeS == nullptr) return;
                    valueentry::ask (*safeS, "Pan (L30, C, R45 ...)", panText ((float) safeS->getValue()), [safeS] (const juce::String& t)
                    {
                        double pv = 0.0;
                        if (safeS != nullptr && valueentry::parsePan (t, pv)) safeS->setValue (pv, juce::sendNotificationSync);
                    });
                };
            }
            Param& p = numCh == 2 ? (c == 0 ? strip.panL : strip.panR) : strip.pan;
            s->setValue (p.get(), juce::dontSendNotification);
            v->setText (panText (p.get()), juce::dontSendNotification);
            s->setTooltip (numCh == 2 ? juce::String (c == 0 ? "Where the LEFT channel of this track is placed (left .. right). " : "Where the RIGHT channel of this track is placed (left .. right). ")
                                            + "Double-click to type a position (Ctrl + click resets)."
                                      : juce::String ("Pan (left .. right). Double-click to type a position (Ctrl + click centres)."));
            s->onValueChange = [this, s, v, &p] { p.set ((float) s->getValue()); v->setText (panText ((float) s->getValue()), juce::dontSendNotification); app.project.markDirty(); };
            addAndMakeVisible (s);
        }

        mute = makeToggle (*this, toggles, "M", juce::Colour (0xffe0312b));
        solo = makeToggle (*this, toggles, "S", juce::Colour (0xfff2c200));
        solo->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        mute->setTooltip ("Mute (this mixer only)"); solo->setTooltip ("Solo (this mixer only)");
        mute->onClick = [this] { strip.mute.set (mute->getToggleState()); app.project.markDirty(); };
        solo->onClick = [this] { strip.solo.set (solo->getToggleState()); app.project.markDirty(); };
        mute->setToggleState (strip.mute.get(), juce::dontSendNotification);
        solo->setToggleState (strip.solo.get(), juce::dontSendNotification);
        addAndMakeVisible (fader);
        refreshFromModel();
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

    int shownLatency() const override { int t = 0; for (auto& sl : strip.slots) t += sl.latencySamples(); return t; }
    void refreshFromModel() override
    {
        setLatencyTip (shownLatency(), mixer);
        repaint();
        slots.refresh();
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
        fader.pushPeaks (strip.meterL.take(), strip.meterR.take(), strip.rmsL.get(), strip.rmsR.get());
    }
    void resetPeaks() override { fader.resetPeaks(); }

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
        return 1 + kBandH + 3 + 2 * kInputRowH + 3 + kSecH + slotCount * kSlotH + 2 + 4 + kSecH + rows * kSendCellH + kToTrackH
               + 2 * kPanRowH + 4 + kMSH + 3 + kFaderH + 3 + kOutRowH + 3 + kLatH + kPlateH + 1 + 4;
    }

protected:
    juce::String bandText() const override { return numCh == 1 ? "AUDIO  MONO" : numCh == 2 ? "AUDIO  STEREO" : "AUDIO  " + juce::String (numCh) + " CH"; }
    juce::String plateText() const override { auto* t = app.project.findTrack (trackId); return t != nullptr ? t->name : juce::String(); }

private:
    AppContext& app; MixerState& mixer; juce::Uuid trackId; StripState& strip; int numCh;
    bool updating = false;
    int sendRows = 1, nSlots = kNumSlots;
    juce::OwnedArray<juce::ComboBox> inBoxes;
    juce::OwnedArray<SendDial> dials;
    juce::TextButton toTrackButton;
    juce::OwnedArray<juce::Slider> pans; juce::OwnedArray<juce::Label> panLabels, panValues;
    juce::OwnedArray<juce::TextButton> toggles;
    juce::TextButton *mute = nullptr, *solo = nullptr;
    SlotButtons slots; FaderBlock fader; StripOutputRow outRow;
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
            solo = makeToggle (*this, toggles, "S", juce::Colour (0xfff2c200));
            solo->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            solo->setToggleState (st.solo.get(), juce::dontSendNotification);
            solo->setTooltip ("Solo this bus (this mixer only): everything that feeds it stays open, the rest is silenced");
            solo->onClick = [this] { st.solo.set (solo->getToggleState()); app.project.markDirty(); };
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
    int shownLatency() const override { int t = 0; for (auto& sl : st.slots) t += sl.latencySamples(); return t; }
    void refreshFromModel() override
    {
        setLatencyTip (shownLatency(), mixer);
        repaint();
        slots.refresh();
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
        r.removeFromBottom (4 + 2 * kPanRowH);
        mute->setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1, 0));
        if (solo != nullptr) solo->setBounds (ms.reduced (1, 0));
        fader.setBounds (faderArea);

        auto top = r.removeFromTop (2 * kInputRowH);                         // where audio track strips have their input selectors
        if (external) { sections.push_back ({ "DRIVER OUTPUTS", top.removeFromTop (kSecH) }); outBox.setBounds (top.removeFromTop (kInputRowH).reduced (0, 1)); }
        r.removeFromTop (3);
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
    InfoNote note; juce::ComboBox outBox;
    juce::OwnedArray<SendDial> dials; juce::TextButton toTrackButton;
    juce::OwnedArray<juce::TextButton> toggles; juce::TextButton *mute = nullptr, *solo = nullptr;
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
        setup (riseBox, riseCap, "RISE", { "Instant", "Fast", "Medium", "Slow" }, ms.rise, "How quickly the bar climbs when the level goes up");
        setup (fallBox, fallCap, "FALL", { "Fast", "Medium", "Slow", "Very slow" }, ms.fall, "How quickly the bar drops when the level goes down");
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
    }
protected:
    juce::String bandText() const override { return "METERS"; }
private:
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
    juce::TextButton resetButton, scopeButton, monoButton;
    juce::ComboBox ditherBox, modeBox, riseBox, fallBox, holdBox;
    juce::Label ditherCap, modeCap, riseCap, fallCap, holdCap;
};

class MixerStripsHolder : public juce::Component
{
public:
    static constexpr int kEdge = 6;                       // the dark border above and below the strips: as wide as the one on the left
    void paint (juce::Graphics& g) override { g.fillAll (mixTheme().bg); }
    void clear() { strips.clear(); removeAllChildren(); }
    void add (StripBase* s, int width = kStripW) { strips.add (s); widths.add (width); addAndMakeVisible (s); }
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
    void tick() { for (auto* s : strips) s->tick(); }
    void resetPeaks() { for (auto* s : strips) s->resetPeaks(); }
private:
    juce::OwnedArray<StripBase> strips;
    juce::Array<int> widths;
    int naturalW = 0, naturalH = 0, minW = 0, minH = 0;
};

// ============================================================================ window
MixerComponent::MixerComponent (AppContext& a, const juce::Uuid& id) : app (a), mixerId (id), holder (new MixerStripsHolder())
{
    setMixDark (theme::isDark());                           // the mixers follow the program-wide dark / light look
    shownDark = mixTheme().dark;
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
    }

    themeButton.setTooltip ("Switch the whole program between the light look and the dark console look");
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
    nameEditor.onReturnKey = commitName;
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
    return 84 + 180 + 76 + 140 + (other ? 410 : 0);                // name, the two icons, a little room for messages
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
    const int availH = viewport.getHeight() - 2 * MixerStripsHolder::kEdge;
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

/** The window never grows bigger than the strips need: it stops there (and snaps back if it was bigger). Smaller is fine: scroll bars appear. */
void MixerComponent::updateWindowLimits()
{
    auto* win = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    if (win == nullptr || win->getContentComponent() != this || getWidth() <= 0) return;
    // The frame around the strips, from the window itself (not from sizes that may not be settled yet: while the window is first being fitted to this
    // content its own size is still a placeholder, which used to make the limits - and so the window - tiny).
    const auto frame = win->getContentComponentBorder();
    const int borderW = frame.getLeftAndRight(), borderH = frame.getTopAndBottom();
    // The biggest the window may be is exactly what the strips need, so there is no scroll bar at all at that size. (Only when the strips cannot fit
    // on the screen do they need bars; a bar then takes room from the other direction, so the window may be that much bigger.)
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
    const int maxW = naturalContentW() + borderW + 0;
    const int maxH = juce::jmin (naturalContentH() + borderH, area.getHeight());          // never taller than the screen when opened
    if (auto* c = win->getConstrainer())
    {
        c->setMinimumSize (juce::jmin (topBarMinWidth() + borderW, maxW), juce::jmin (260, maxH));
        c->setMaximumSize (juce::jmax (maxW + kLimitSlack, area.getWidth() + 40), juce::jmax (maxH + kLimitSlack, area.getHeight() + 40));   // free to grow: a big window shows more
    }
    const bool firstTime = lastMaxW == 0 || lastMaxH == 0;          // a window that has never been fitted opens showing everything
    const bool wasFullW = firstTime || win->getWidth() == lastMaxW, wasFullH = firstTime || win->getHeight() == lastMaxH;
    lastMaxW = maxW; lastMaxH = maxH;
    // a window that showed everything keeps showing everything when the strips change (e.g. another insert slot appears).
    // It may be taller than the screen: the whole strip is always reachable (the title bar stays on the screen, the rest hangs off the bottom).
    const int w = wasFullW ? maxW : win->getWidth();
    const int h = wasFullH ? maxH : win->getHeight();
    if ((w != win->getWidth() || h != win->getHeight()) && ! win->isFullScreen())
    {
        win->setSize (w, h);
        if (win->getY() < area.getY()) win->setTopLeftPosition (win->getX(), area.getY());
    }
}

void MixerComponent::fitWindowToStrips()
{
    auto* win = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    if (win == nullptr || win->getContentComponent() != this || win->isFullScreen()) return;
    const auto frame = win->getContentComponentBorder();
    const auto scr = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
    const int w = naturalContentW() + frame.getLeftAndRight(), h = juce::jmin (naturalContentH() + frame.getTopAndBottom(), scr.getHeight());
    if (auto* c = win->getConstrainer()) { c->setMinimumSize (juce::jmin (topBarMinWidth() + frame.getLeftAndRight(), w), juce::jmin (260, h)); c->setMaximumSize (juce::jmax (w + kLimitSlack, scr.getWidth() + 40), juce::jmax (h + kLimitSlack, scr.getHeight() + 40)); }
    lastMaxW = w; lastMaxH = h;
    win->setSize (w, h);
    const auto area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (win->getBounds())->userArea;
    if (win->getY() < area.getY() + 44) win->setTopLeftPosition (win->getX(), area.getY() + 44);
}

void MixerComponent::parentHierarchyChanged() { updateWindowLimits(); }

MixerState* MixerComponent::mixer() const
{
    for (auto& m : app.project.mixers) if (m->id == mixerId) return m.get();
    return nullptr;
}

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
        auto* cs = new ChannelStrip (app, *m, t, slotCount);
        chans.add (cs); maxDials = juce::jmax (maxDials, cs->dialCount());
        holder->add (cs);
    }
    for (int pass = 0; pass < 2; ++pass)                          // Int Buses first, then Ext Buses
        for (auto& b : app.project.buses)
            if (b.external == (pass == 1))
            {
                auto* bs = new BusStrip (app, *m, b, slotCount);
                busStrips.add (bs); maxDials = juce::jmax (maxDials, bs->dialCount());
                holder->add (bs);
            }
    const int rows = juce::jmax (1, (maxDials + 1) / 2);
    for (auto* c : chans) c->setSendRows (rows);
    for (auto* b : busStrips) b->setSendRows (rows);
    holder->add (new MeterSettingsStrip (app, mixerId, [this] { holder->resetPeaks(); }), 112);
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
    themeButton.setTooltip (mixTheme().dark ? "Dark look is on. Click for the light look." : "Light look is on. Click for the dark look.");
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
    if (auditionButton.isVisible()) auditionButton.setBounds (top.removeFromRight (260).reduced (3));
    nameCaption.setBounds (top.removeFromLeft (84).reduced (4, 0));
    nameEditor.setBounds (top.removeFromLeft (180).reduced (0, 3));
    statusLabel.setBounds (top.reduced (8, 0));
    // Big enough for every strip: no scroll bars at all (never one that is only there because the other one takes some room). Smaller: bars.
    viewport.setBounds (r);
    if (! inApply && holder->getNumStrips() > 0) applyHeight();
    updateBars();
    if (! inLimits) { inLimits = true; updateWindowLimits(); inLimits = false; }
}

void MixerComponent::timerCallback()
{
    if (shownDark != mixTheme().dark)                             // the look was switched (here or in another mixer window)
    {
        shownDark = mixTheme().dark;
        mixLook.apply();
        rebuild(); updateAuditionButton(); resized();
        viewport.sendLookAndFeelChange();
        repaint();
    }
    holder->tick();
    const auto scan = app.plugins.scanStatus();
    if (statusFlash > 0) --statusFlash;
    else statusLabel.setText (app.plugins.isScanning() ? scan : juce::String(), juce::dontSendNotification);
    updateAuditionButton();
    if (auditionButton.isVisible() && auditionButton.getWidth() == 0) resized();
    static int counter = 0;
    if (++counter % 10 == 0)                                // 3 Hz: plug-in names, solo / mute changes
    {
        app.engine.checkLatency();                          // a plug-in's delay changed (loaded, bypassed ...): line the paths up again
        if (slotsNeeded() != slotCount) { rebuild(); resized(); }        // an insert was used (or removed): every strip gets the new number of slots
        else holder->refreshFromModel();
    }
}
} // namespace td

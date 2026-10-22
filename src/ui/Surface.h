#pragma once
#include "AppContext.h"
#include "../core/Mcu.h"
#include <deque>
#include <map>

namespace td
{
/** What the surface's own buttons can be made to do: the eight keys along the bottom of an Avid S1 (whatever EuControl sends for them: they are
    learned), the navigation arrows, Flip, the Inserts / Pan keys and the transport. The strips' own Mute / Solo / Select / Rec / dial-push keys
    are chosen separately (StripButton / StripAction). */
namespace surface
{
enum Action : int
{
    ZoomOut = 0,       // the take / edit window used last: zoom out (time) - like the Left arrow
    ZoomIn,            // zoom in - the Right arrow
    TracksSmaller,     // the tracks smaller (zoom out vertically) - the Up arrow
    TracksBigger,      // the tracks bigger - the Down arrow
    WaveSmaller,       // what the , key does: the waveforms smaller
    WaveBigger,        // the . key: the waveforms bigger
    MeterBridge,       // opens the meter bridge, or closes it
    MixerToggle,       // opens the mixer, or closes it (what the M key does)
    DialGain,          // the dials turn the strips' preamp gain (the Inserts key); again: back to pan
    DialPan,           // the dials turn the pans
    FlipFaders,        // Flip: the faders and the dials swap jobs
    BankLeft, BankRight, ChannelLeft, ChannelRight,
    PlayPause, StopPlay, RecordTake,
    NumActions
};
inline juce::String actionName (int a)
{
    static const char* names[] = { "Zoom out (time)", "Zoom in (time)", "Tracks smaller", "Tracks bigger", "Waveforms smaller  ( , key)", "Waveforms bigger  ( . key)",
                                   "Meter bridge open / close", "Mixer open / close", "Dials = preamp gain (Inserts)", "Dials = pan", "Flip (faders <-> dials)",
                                   "Bank left (8 strips)", "Bank right", "One strip left", "One strip right", "Play / pause", "Stop", "Record (start / stop a take)" };
    return juce::isPositiveAndBelow (a, NumActions) ? names[a] : "";
}
/** The buttons a surface has on every strip, and what each one does. */
enum StripButton : int { SbRec = 0, SbSolo, SbMute, SbSelect, SbPush, NumStripButtons };
enum StripAction : int { SaNone = 0, SaMute, SaSolo, SaSelect, SaLowCut, SaDialHome, NumStripActions };
inline juce::String stripButtonName (int b) { static const char* n[] = { "Rec", "Solo", "Mute", "Select", "Dial push" }; return juce::isPositiveAndBelow (b, NumStripButtons) ? n[b] : ""; }
inline juce::String stripActionName (int a) { static const char* n[] = { "Nothing", "Mute", "Solo", "Select the strip", "LC (low cut) on / off", "Dial home (pan centre / 20 dB gain)" }; return juce::isPositiveAndBelow (a, NumStripActions) ? n[a] : ""; }
}

/** The control surface: an Avid S1 (through EuControl's Mackie Control emulation) or any Mackie Control surface, on a pair of MIDI ports.
    It controls ONE mixer at a time: the one whose window is in front or was in front last ("follow"), or one chosen in the main window's
    CONTROLLER box ("pinned"). Its eight strips are a window of eight of that mixer's channels (audio tracks, Int Buses, Ext Buses, then the
    VCAs shown there), moved with the Bank and Channel keys: faders (motorised: they follow the screen, and the screen follows them), the dials
    (pan, or the preamp gain of the track's input after the Inserts key; Flip swaps them with the faders), Mute / Solo / Select / Rec (Rec is the
    LC key unless chosen otherwise) and the two lines of writing (names above, levels below).
    Everything it does goes through the same model as the mouse does (the strips' Params), so the open mixer windows simply follow.
    The settings (ports, what the keys do) are kept in Fermata's own settings file: they belong to this computer, not to a project. */
class SurfaceController : public juce::ChangeBroadcaster, private juce::MidiInputCallback, private juce::AsyncUpdater, private juce::Timer
{
public:
    explicit SurfaceController (AppContext&);
    ~SurfaceController() override;

    // ---- which mixer
    enum class Target { Off, Follow, Pinned };
    void setTarget (Target t, const juce::Uuid& pin = juce::Uuid::null());
    Target target() const noexcept { return mode; }
    juce::Uuid pinned() const noexcept { return pin; }
    /** The mixer being controlled right now (null: off, or no project). */
    juce::Uuid controlledMixer() const;

    // ---- the MIDI ports
    struct Ports { juce::String inId, inName, outId, outName; };
    void setPorts (const juce::String& inIdentifier, const juce::String& outIdentifier);
    Ports ports() const { return want; }
    bool inputOpen() const noexcept  { return in != nullptr; }
    bool outputOpen() const noexcept { return out != nullptr; }
    juce::String openInId() const  { return in != nullptr ? in->getDeviceInfo().identifier : juce::String(); }
    juce::String openOutId() const { return out != nullptr ? out->getDeviceInfo().identifier : juce::String(); }
    /** One line for the lamp / the panel: "S1: connected", "No MIDI ports chosen", "Waiting for 'EuMidi' ..." */
    juce::String statusText() const;
    bool connected() const noexcept { return in != nullptr && out != nullptr; }

    // ---- what the keys do
    int actionOf (int note) const;                                    // -1: nothing
    juce::String bindingsText (int action) const;                     // "F1, Cursor left"
    void startLearn (int action);                                     // the next key pressed on the surface does this action
    void cancelLearn() { learnAction = -1; sendChangeMessage(); }
    int learning() const noexcept { return learnAction; }
    void unbind (int action);
    void resetMap();
    int stripAssign (int button) const { return juce::isPositiveAndBelow (button, surface::NumStripButtons) ? stripMap[button] : surface::SaNone; }
    void setStripAssign (int button, int action);
    bool metersOn() const noexcept { return meters; }
    void setMetersOn (bool on);

    // ---- the MIDI monitor (the panel shows it)
    struct MonitorLine { juce::String time, dir, hex, text; };
    const std::deque<MonitorLine>& monitor() const noexcept { return lines; }
    void clearMonitor() { lines.clear(); sendChangeMessage(); }
    bool monitorPaused = false;
    bool monitorShowsOutput = false;                                  // (what Fermata sends is a lot: off unless asked for)

    /** The surface shows everything again (after the panel changed something). */
    void refreshAll() { force = true; }

    static juce::String noteName (int note) { return mcu::buttonName (note); }

private:
    // the channels of the controlled mixer, in the order the surface shows them
    struct Chan
    {
        enum class Kind { Track, IntBus, ExtBus, Vca } kind = Kind::Track;
        juce::Uuid id; juce::String name; int numCh = 1;
        StripState* s = nullptr; BusState* b = nullptr; VcaState* v = nullptr;
        bool hasPan() const noexcept   { return kind == Kind::Track || kind == Kind::IntBus; }
        bool hasSolo() const noexcept  { return kind != Kind::ExtBus; }
        bool hasLc() const noexcept    { return kind == Kind::Track || kind == Kind::IntBus; }
        Param& level() const noexcept  { return s != nullptr ? s->gainDb : b != nullptr ? b->gainDb : v->gainDb; }
        Flag& mute() const noexcept    { return s != nullptr ? s->mute : b != nullptr ? b->mute : v->mute; }
        Flag& solo() const noexcept    { return s != nullptr ? s->solo : b != nullptr ? b->solo : v->solo; }
    };
    std::vector<Chan> channels (MixerState&) const;
    const Chan* chanAt (int strip) const { return strip >= 0 && strip + bank < (int) chans.size() ? &chans[(size_t) strip + (size_t) bank] : nullptr; }

    // MIDI in (its own thread) -> the message thread
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void handle (const mcu::Event&, const juce::MidiMessage&);
    void doAction (int action);
    void doStripButton (int strip, int button);
    void moveFader (int strip, int value14);
    void turnDial (int strip, int delta);
    void send (const juce::MidiMessage&);
    void log (bool outgoing, const juce::MidiMessage&);
    void openPorts();
    void closePorts();
    void blank();                                                     // faders down, lamps off, writing cleared
    void show (bool everything);                                      // the feedback: what changed since last time (or everything)
    void showValue (int strip, const juce::String& text);             // on the bottom line for a moment
    void setPreampGain (const Chan&, float deltaDb, bool home);
    float preampGainOf (const Chan&, bool& has, bool shown) const;   // the gain of the track's (first) input: raw 0 .. 66, or as the read-out shows it
    float panMid (const Chan&) const;
    void setPanMid (const Chan&, float mid);
    void load(); void save();
    void defaults();

    AppContext& app;
    Target mode = Target::Follow; juce::Uuid pin;
    Ports want;
    std::unique_ptr<juce::MidiInput> in; std::unique_ptr<juce::MidiOutput> out;
    juce::CriticalSection lock; std::vector<juce::MidiMessage> pending;
    std::map<int, int> map;                                           // note number -> Action
    int stripMap[surface::NumStripButtons] = { surface::SaLowCut, surface::SaSolo, surface::SaMute, surface::SaSelect, surface::SaDialHome };
    bool meters = true;
    int learnAction = -1;
    std::deque<MonitorLine> lines;

    std::vector<Chan> chans; juce::Uuid shownMixer;
    std::map<juce::Uuid, int> banks; int bank = 0;
    bool dialGain = false, flipped = false;
    bool touched[mcu::kStrips] = {};
    bool force = true, active = false;
    int ticks = 0;
    struct Sent { int fader = -1, ring = -1, meter = -1, lamp[4] = { -1, -1, -1, -1 }; juce::String top, bottom; } sent[mcu::kStrips];
    int sentFlip = -1, sentPan = -1, sentPlug = -1, sentDigits = -1;
    juce::String valueText[mcu::kStrips]; juce::uint32 valueUntil[mcu::kStrips] = {};
    juce::String banner; juce::uint32 bannerUntil = 0; bool bannerShown = false;
    juce::uint32 lastOpenTry = 0;
};
} // namespace td

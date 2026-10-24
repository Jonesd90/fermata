#pragma once
#include <juce_core/juce_core.h>

namespace td::remote
{
/** The control port (for a Stream Deck or any other controller). Fermata listens on 127.0.0.1 only. Every message is one line of JSON.

    Controller -> Fermata (the keys of the Stream Deck Neo):
        {"cmd":"record"}  {"cmd":"playpause"}  {"cmd":"talk","down":true|false}  {"cmd":"playback"}  {"cmd":"mixer","index":0}  {"cmd":"prerec"}  {"cmd":"hello"}
    Controller -> Fermata (the Stream Deck + XL: 36 keys and 6 dials; see RemoteDesk.h for what each does):
        {"cmd":"desk","on":true}                               start (or stop) the "desk" and "meters" messages below
        {"cmd":"bridge"} {"cmd":"pitchpipe"} {"cmd":"click"}   open / close that window
        {"cmd":"takewindow"}                                   minimise / bring back the take window used last
        {"cmd":"minimiseall"}                                  minimise every window but the main one (again: bring them back)
        {"cmd":"armtoggle"}  {"cmd":"wavecolour"}  {"cmd":"loop"}  {"cmd":"openwin","what":"editwin|takedisplay|organiser|media|mastering"}  {"cmd":"wact","what":"mark1|mark2|align|search|trim|markers|feedback|slipl|slipr"}
        {"cmd":"monitor","input":3}                            this input's tracks step E -> S -> S-L -> T
        {"cmd":"talkin","input":3}                             this input in / out of the talkback (control-room) mic
        {"cmd":"preamp","input":3,"what":"mic|v48|pol|lc|pad"} a preamp key of that input
        {"cmd":"gain","input":3,"delta":0.5}                   the preamp gain of that input, up or down by delta dB
        {"cmd":"mixout","index":0,"delta":0.5}                 the main output fader of mixer 0 (processing) .. 5 (cue mixers 1 - 5), delta dB
        {"cmd":"mixmute","index":0}                            that main output on / off
        {"cmd":"stage","what":"playback|mic1|mic2|out","delta":0.5}   the stage speaker panel's levels; {"cmd":"stagereset","what":"..."} puts one back to 0 dB
    Fermata -> controller:
        {"type":"state", ...}   sent when something changes (and at least once a second for the clocks); "deskProtocol":1 = knows the Stream Deck + XL commands
        {"type":"desk", ...}    (after "desk") the inputs, the mixers' main outputs, the stage levels, the windows: sent when something changes
        {"type":"nodesk"}       (after "desk") the project was closed: there is no desk until one is open again
        {"type":"meters", ...}  (after "desk") 20 times a second: the peak of every input and of each mixer's main output, in whole dB */
constexpr int kDefaultPort = 7788;
constexpr int kMixerSlots = 6;           // the dials: the processing mixer and cue mixers 1 - 5

/** What a controller needs to draw its keys. */
struct State
{
    bool recording = false;
    int  takeNumber = 1;               // recording: the number of this take; otherwise the number of the next take
    bool playing = false;
    bool talkbackReady = false;        // a CR mic and a TB output are set up
    bool playbackReady = false;        // a TB output is set up (enough for the Playback speaker key: it does not need the CR mic)
    bool crOpen = false;               // the CR mic is open (latched or held)
    bool pbOn = false;                 // playback is going to the TB speaker
    bool preRec = false;
    bool mixerOpen[2] = { false, false };
    bool hasMixer2 = false;            // there is an Alt Mixer
    juce::String mixer2Name;           // its name, or "Alt-Mixer" when there is none
    bool hasSession = false;           // a session countdown is set
    juce::int64 sessionLeft = 0;       // seconds (negative = over time)
    juce::String timeOfDay;            // HH:MM:SS
    juce::String sessionText;          // 01:02:03 or -00:04:53, empty when no countdown

    juce::String toJson() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", "state");
        o->setProperty ("recording", recording); o->setProperty ("takeNumber", takeNumber); o->setProperty ("playing", playing);
        o->setProperty ("talkbackReady", talkbackReady); o->setProperty ("playbackReady", playbackReady); o->setProperty ("crOpen", crOpen); o->setProperty ("pbOn", pbOn); o->setProperty ("preRec", preRec);
        o->setProperty ("mixer1Open", mixerOpen[0]); o->setProperty ("mixer2Open", mixerOpen[1]); o->setProperty ("hasMixer2", hasMixer2); o->setProperty ("mixer2Name", mixer2Name);
        o->setProperty ("hasSession", hasSession); o->setProperty ("sessionLeft", sessionLeft);
        o->setProperty ("timeOfDay", timeOfDay); o->setProperty ("sessionText", sessionText);
        o->setProperty ("deskProtocol", 1);                                  // this Fermata knows the Stream Deck + XL's "desk" command (a controller shows "open a project" when no desk message comes)
        // the colours Fermata itself uses, so the keys match the status squares
        auto* c = new juce::DynamicObject();
        c->setProperty ("record", "#e53935"); c->setProperty ("ready", "#2e9e4f"); c->setProperty ("cr", "#a83cff"); c->setProperty ("pb", "#ffd21f");
        o->setProperty ("colours", juce::var (c));
        return juce::JSON::toString (juce::var (o), true);
    }
};

/** The window keys of the Stream Deck: the buttons of the take / edit window used last (and the slip setting), by name. */
inline const char* const kWindowActs[] = { "mark1", "mark2", "align", "search", "trim", "markers", "feedback", "slipl", "slipr" };
inline bool isWindowAct (const juce::String& w) { for (auto* k : kWindowActs) if (w == k) return true; return false; }

/** The windows the Stream Deck opens and closes by name (state in desk.win, 0 not open, 1 shown, 3 minimised). */
inline bool isOpenWin (const juce::String& w) { for (auto* k : { "editwin", "takedisplay", "organiser", "media", "mastering" }) if (w == k) return true; return false; }

struct Command
{
    juce::String name;                 // "record", "playpause", ... (see the list in parseCommand); empty = not understood
    bool down = false;                 // talk
    int  index = 0;                    // mixer, mixout, mixmute
    // the Stream Deck + XL
    bool on = true;                    // desk: start / stop the desk and meters messages
    int  input = -1;                   // a driver input, 0-based (-1 = none given)
    juce::String what;                 // preamp: mic / v48 / pol / lc / pad;  stage: playback / mic1 / mic2 / out
    double delta = 0.0;                // gain, mixout, stage: dB
};

inline Command parseCommand (const juce::String& line)
{
    Command c;
    const auto v = juce::JSON::parse (line);
    if (! v.isObject()) return c;
    const auto name = v["cmd"].toString().toLowerCase();
    static const char* known[] = { "record", "playpause", "talk", "playback", "mixer", "prerec", "hello",
                                   "desk", "bridge", "pitchpipe", "click", "takewindow", "minimiseall", "armtoggle", "wavecolour", "loop", "wact", "openwin",
                                   "monitor", "talkin", "preamp", "gain", "mixout", "mixmute", "stage", "stagereset" };
    for (auto* k : known) if (name == k) c.name = name;
    c.down = (bool) v["down"];
    c.index = juce::jmax (0, (int) v["index"]);
    c.on = v.hasProperty ("on") ? (bool) v["on"] : true;
    c.input = v.hasProperty ("input") ? (int) v["input"] : -1;
    c.what = v["what"].toString().toLowerCase();
    c.delta = juce::jlimit (-100.0, 100.0, (double) v["delta"]);              // (a stray huge number never throws a fader to its end)
    return c;
}
} // namespace td::remote

#pragma once
#include <juce_core/juce_core.h>

namespace td::remote
{
/** The control port (for a Stream Deck or any other controller). Fermata listens on 127.0.0.1 only. Every message is one line of JSON.

    Controller -> Fermata:   {"cmd":"record"}  {"cmd":"playpause"}  {"cmd":"talk","down":true|false}  {"cmd":"playback"}
                             {"cmd":"mixer","index":0}  {"cmd":"prerec"}  {"cmd":"hello"}
    Fermata -> controller:   {"type":"state", ...}  sent when something changes (and at least once a second for the clocks). */
constexpr int kDefaultPort = 7788;

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
        // the colours Fermata itself uses, so the keys match the status squares
        auto* c = new juce::DynamicObject();
        c->setProperty ("record", "#e53935"); c->setProperty ("ready", "#2e9e4f"); c->setProperty ("cr", "#a83cff"); c->setProperty ("pb", "#ffd21f");
        o->setProperty ("colours", juce::var (c));
        return juce::JSON::toString (juce::var (o), true);
    }
};

struct Command
{
    juce::String name;                 // "record", "playpause", "talk", "playback", "mixer", "prerec", "hello"; empty = not understood
    bool down = false;                 // talk
    int  index = 0;                    // mixer
};

inline Command parseCommand (const juce::String& line)
{
    Command c;
    const auto v = juce::JSON::parse (line);
    if (! v.isObject()) return c;
    const auto name = v["cmd"].toString().toLowerCase();
    static const char* known[] = { "record", "playpause", "talk", "playback", "mixer", "prerec", "hello" };
    for (auto* k : known) if (name == k) c.name = name;
    c.down = (bool) v["down"];
    c.index = juce::jmax (0, (int) v["index"]);
    return c;
}
} // namespace td::remote

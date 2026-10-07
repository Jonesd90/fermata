#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <cmath>
#include <limits>
#include <array>

namespace td
{
constexpr int kSendSlots        = 2;    // VST3 inserts that can sit between a channel and one of its sends
constexpr float kSendOffDb      = -60.0f;   // a send at or below this level is off
constexpr int kNumSlots         = 4;    // VST3 insert slots per strip / fx channel
/** Every piece of audio starts and ends with a short fade (seconds) so nothing clicks; the files are never touched. Editable per piece. */
constexpr double kDefaultEdgeFade = 0.025;           // the fade at the start / end of an edit, and of a take, until the user changes it (25 ms)
constexpr int kMaxTrackChannels = 16;   // widest track (surround)
constexpr int kMaxInputs        = 256;  // physical input channels tracked for metering

/** A float that can be read/written from the audio thread without locking. */
struct Param
{
    Param (float x = 0.0f) : v (x) {}
    float get() const noexcept        { return v.load (std::memory_order_relaxed); }
    void  set (float x) noexcept      { v.store (x, std::memory_order_relaxed); }
    float take() noexcept             { return v.exchange (0.0f, std::memory_order_relaxed); }   // read-and-reset (meters)
    std::atomic<float> v;
};

struct Flag
{
    Flag (bool x = false) : v (x) {}
    bool get() const noexcept         { return v.load (std::memory_order_relaxed); }
    void set (bool x) noexcept        { v.store (x, std::memory_order_relaxed); }
    std::atomic<bool> v;
};

/** How a track's live input is monitored through the mixers.
    Edit        'E'    : live input is audible only while the track is armed.
    Session     'S'    : always audible, armed or not, except while a take plays back (then only the recording is heard).
    SessionLive 'S-L'  : always audible, also while a take plays back.
    Talkback    'T'    : always audible (between takes, during playback) but completely off while recording. */
enum class Monitor { Edit = 0, Session = 1, SessionLive = 2, Talkback = 3 };

inline const char* monitorLabel (Monitor m) noexcept
{
    switch (m) { case Monitor::Edit: return "E"; case Monitor::Session: return "S"; case Monitor::SessionLive: return "S-L"; default: return "T"; }
}

/** State of a track that the audio thread reads (shared with the engine's plan so it can never dangle). */
struct TrackLive
{
    Flag armed;
    std::atomic<int> monitor { (int) Monitor::Session };   // new tracks start as 'S': always audible, so you hear the room before arming
    Monitor getMonitor() const noexcept { return (Monitor) monitor.load (std::memory_order_relaxed); }
};

inline float dbToGain (float db) noexcept { return db <= -100.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }

inline juce::String pad3 (int n) { return juce::String (n).paddedLeft ('0', 3); }

/** Make a string safe to use in a Windows file name. */
inline juce::String sanitiseForFile (const juce::String& s)
{
    auto r = s.removeCharacters ("\\/:*?\"<>|\r\n\t").trim();
    return r.isEmpty() ? juce::String ("untitled") : r;
}
} // namespace td

#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <atomic>
#include <memory>

namespace td
{
/** LTC (linear timecode, SMPTE 12M) recorded as audio on a track. The decoder reads the biphase-mark code from the samples, finds the sync words
    and returns the timecode of every frame together with the sample position where that frame starts. */
class LtcDecoder
{
public:
    struct Frame { double pos = 0.0; int h = 0, m = 0, s = 0, f = 0; bool drop = false; };

    explicit LtcDecoder (double sampleRate);
    /** Feed the samples of ONE channel, in order (any block sizes). */
    void feed (const float* samples, int count);
    const std::vector<Frame>& frames() const noexcept { return found; }
    /** For live use on the audio thread: do not keep every frame (that would allocate); only count them and remember the last one. */
    void setStoreFrames (bool on) noexcept { store = on; }
    long long frameTotal() const noexcept { return total; }
    const Frame& lastFrame() const noexcept { return last; }
    double bitLength() const noexcept { return tBit; }

private:
    void edge (double pos);
    void bit (int value, double startPos);
    void tryFrame();

    double rate;
    double tBit;                                   // the length of one bit in samples (follows the signal)
    double minBit, maxBit;
    // signal -> edges
    float prev = 0.0f; double env = 0.0; int polarity = 0; double pendingZc = -1.0, zcPos = 0.0; double index = 0.0;
    // edges -> bits
    double e1 = -1.0, e2 = -1.0; bool shortPending = false; double shortStart = 0.0;
    // bits -> frames
    int ring[80]; double ringPos[80]; int ringCount = 0, sinceSync = 0;
    std::vector<Frame> found;
    bool store = true; long long total = 0; Frame last;
};

/** Watches one live input channel for LTC (audio thread: no allocation). The message thread asks for the status. */
class LtcLive
{
public:
    struct Status { bool present = false; double fps = 25.0; bool drop = false; double tcSeconds = 0.0; };
    void prepare (double sampleRate);                       // not while audio is running
    void feed (const float* samples, int count) noexcept;   // audio thread
    void feedNothing (int count) noexcept;                  // audio thread: no channel chosen
    Status status() const noexcept;                         // any thread
private:
    std::unique_ptr<LtcDecoder> dec;
    double rate = 48000.0;
    long long seen = 0; double prevPos = -1.0; int prevN = -1; double lenEma = 0.0;
    int maxF = 0, maxFOld = 0; long long sinceWindow = 0;
    bool dropSeen = false; long long dropLast = 0;
    long long sinceFrame = 1 << 30; long long idx = 0;
    std::atomic<bool> present { false }, dropA { false }; std::atomic<double> fpsA { 25.0 }, tcA { 0.0 };
};

/** Makes LTC (audio thread): call setFormat / start before, then fill() for every block. */
class LtcGenerator
{
public:
    /** formatCode: 0 = 24, 1 = 25, 2 = 29.97 drop-frame, 3 = 29.97 non-drop, 4 = 30, 5 = 23.976 */
    static void formatInfo (int formatCode, double& fps, int& nominal, bool& drop);
    static juce::String formatName (int formatCode);
    void prepare (double sampleRate);
    /** Starts (or restarts) at the given time of day in seconds. */
    void start (int formatCode, double startSeconds) noexcept;
    void stop() noexcept { running = false; }
    bool isRunning() const noexcept { return running; }
    void fill (float* out, int count) noexcept;
private:
    void buildFrame() noexcept;
    double rate = 48000.0, fps = 25.0, bitLen = 24.0, phase = 0.0, smooth = 0.0;
    int nominal = 25; bool drop = false, running = false, halfDone = false;
    long long frameNo = 0, dayFrames = 0; int bitIdx = 0; int bits[80] = {}; float level = 1.0f;
};

/** What was found on a track. */
struct LtcResult
{
    bool   found = false;
    double startSeconds = 0.0;      // the timecode (as time since midnight, in seconds) that belongs to the FIRST SAMPLE of the file
    double fps = 25.0;              // 23.976, 24, 25, 29.97 or 30
    bool   drop = false;            // 29.97 drop-frame
    int    frameCount = 0;          // how many frames were decoded
    int    jumps = 0;               // places where the timecode does not follow the audio (a gap, a re-jam ...)
    double coveredSeconds = 0.0;    // from the first to the last frame found
    juce::String firstText;         // timecode of the first sample as text (hh:mm:ss:ff, ';' before the frames when drop-frame)
};

/** Works out frame rate, start time and continuity from the decoded frames. */
LtcResult analyseLtc (const std::vector<LtcDecoder::Frame>&, double sampleRate);

/** Reads channel 'channel' of an audio file (the whole file, or up to maxSeconds of it) and decodes it. */
LtcResult scanFileForLtc (juce::AudioFormatManager&, const juce::File&, int channel = 0, double maxSeconds = 0.0, const std::atomic<bool>* cancel = nullptr);

/** The name of a frame rate for the screen: "25", "29.97 DF", "23.976" ... */
juce::String fpsName (double fps, bool drop);

/** "01:02:03:04" (";" before the frames for drop-frame) for a time in seconds since midnight. */
juce::String timecodeText (double seconds, double fps, bool drop);
} // namespace td

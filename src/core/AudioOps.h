#pragma once
#include "Project.h"
#include "PitchShift.h"
#include "SpectralRepair.h"
#include "Denoise.h"
#include <limits>

namespace td
{
/** What a fix does to the audio. */
struct FixSpec
{
    enum class Kind { Pitch, Patch, Declick, Reharmonise } kind = Kind::Pitch;
    double cents = 0.0;                     // Pitch: +100 = one semitone up
    bool useCurve = false;                  // Pitch: follow 'curve' instead of the fixed 'cents'
    PitchCurve curve;                       // Pitch curve: cents against time (seconds); time 0 is 'curveZero' samples after the first sample of the fixed part
    double curveZero = 0.0;                 // (samples) see above: the extra audio processed before the marked part
    double f0 = 0.0, f1 = 24000.0;          // Patch: the band that is replaced (Hz)
    RepairSide side = RepairSide::Both;     // (old) Patch: which clean audio is used
    RepairParams repair;                    // Patch: strength, direction, surrounding length, before / after weighting
    double sensitivity = 6.0;               // Declick: 1 .. 10
    /** Lost samples (a digital jump where a block of samples went missing): where they are and how many, found on ALL the tracks together by the repair window (one event,
        the same on every track, so every file must be mended in exactly the same way). 'at' counts from the start of the fixed part (s0). Without them (lostKnown false)
        each call looks for itself, on the channels it is given. */
    struct Lost { juce::int64 at = 0; int count = 0; };
    std::vector<Lost> lost;
    bool lostKnown = false;
    juce::int64 lostLo = -(juce::int64 (1) << 40), lostHi = juce::int64 (1) << 40;     // how far the time squeeze that puts them back may reach (samples from s0): the piece that is put in
    juce::String shortName() const
    {
        if (kind == Kind::Pitch && useCurve) return "pitch curve";
        if (kind == Kind::Pitch) return "pitch " + juce::String (cents >= 0 ? "+" : "") + juce::String (cents, 0) + "c";
        if (kind == Kind::Reharmonise) return "reharmonised";
        return kind == Kind::Patch ? "repair" : "declick";
    }
};

/** A job that runs on its own thread; the result is picked up on the message thread. */
class AudioJob : private juce::Thread
{
public:
    using Work = std::function<juce::String (AudioJob&)>;            // returns an error text ("" = fine)
    AudioJob() : juce::Thread ("Audio job") {}
    ~AudioJob() override { cancel = true; stopThread (20000); }
    /** onDone runs on the message thread with the error text. */
    void start (Work w, std::function<void (const juce::String&)> onDone)
    {
        stopThread (5000);                                   // a job that has just finished may still be winding its thread down
        work = std::move (w); done = std::move (onDone); cancel = false; progressValue = 0.0f; finished = false;
        startThread();
    }
    void requestCancel() { cancel = true; }
    bool cancelled() const noexcept { return cancel.load(); }
    void setProgress (float p) noexcept { progressValue = p; }
    float progress() const noexcept { return progressValue.load(); }
    bool isFinished() const noexcept { return finished.load(); }
    bool isRunning() const { return isThreadRunning(); }
private:
    void run() override
    {
        juce::String err;
        try { err = work (*this); } catch (...) { err = "Something went wrong while processing."; }
        if (cancel.load() && err.isEmpty()) err = "Cancelled.";
        finished = true;
        auto cb = done;
        juce::MessageManager::callAsync ([cb, err] { if (cb) cb (err); });
    }
    Work work; std::function<void (const juce::String&)> done;
    std::atomic<bool> cancel { false }, finished { false }; std::atomic<float> progressValue { 0.0f };
};

namespace audioops
{
/** Opens a reader for any file Fermata can play (wav, aiff, flac ...). */
std::unique_ptr<juce::AudioFormatReader> openReader (juce::AudioFormatManager&, const juce::File&);
/** Reads [start, start+n) of the file as one vector per channel; anything outside the file is silence. */
bool readRange (juce::AudioFormatReader&, juce::int64 start, juce::int64 n, std::vector<std::vector<float>>& out);
/** A file name next to 'wanted' that does not exist yet ("name.wav", "name (2).wav" ...). */
juce::File uniqueFile (const juce::File& wanted);
/** A WAV writer (24-bit, or 32-bit float when 'floatData'); null on failure. */
std::unique_ptr<juce::AudioFormatWriter> makeWavWriter (const juce::File&, double sampleRate, int numChannels, bool floatData = false);
/** Writes a 24-bit WAV (or 32-bit float when 'floatData'). */
bool writeWav (const juce::File&, const std::vector<std::vector<float>>&, double sampleRate, bool floatData = false);

/** Writes the samples [a, b) of 'source' (the file's sample 0 is the take's sample 'fileStart') to a new WAV file with the same channels and sample rate (24-bit, no dither, nothing held open afterwards). */
bool exportRange (juce::AudioFormatManager&, const juce::File& source, juce::int64 fileStart, juce::int64 a, juce::int64 b, const juce::File& dest, juce::String& error);
/** Checks that 'file' (a corrected file that came back) can replace 'frames' samples with 'channels' channels at 'rate'. Returns "" if so, else a sentence saying what is wrong. */
juce::String checkReplacement (juce::AudioFormatManager&, const juce::File& file, juce::int64 frames, int channels, double rate);
/** A new file as long as 'source' in which [s0, s1) comes from 'replacement' (whose sample 0 is the sample s0), with a short linear crossfade inside the range at both ends. */
bool spliceFile (juce::AudioFormatManager&, const juce::File& source, const juce::File& replacement, juce::int64 s0, juce::int64 s1, const juce::File& dest, double crossfadeSeconds, juce::String& error);

/** Applies a fix to the samples [s0, s1) of every channel (the rest of the buffer is context and is not changed, apart from the cross-fade at the edges and, for a de-click or a repair box of
    250 ms or less that reaches below 1 kHz: lost samples found inside [s0, s1) are put back, and the time squeeze that keeps the length reaches up to lostReach() each side). */
void applyFix (std::vector<std::vector<float>>& channels, double sampleRate, long s0, long s1, const FixSpec&, long crossfade);

/** A ReTouch box round 'the click itself' (250 ms or less) that reaches down below 1 kHz, with a strength above 0: lost samples inside it are put back before the box is treated. */
bool boxRoundClick (const FixSpec&, double sampleRate, long s0, long s1);
/** Whether this fix looks for lost samples (a de-click, or a box round a click), and how far from [s0, s1) it can then change the audio (the time squeeze). */
bool findsLost (const FixSpec&, double sampleRate, juce::int64 s0, juce::int64 s1);
juce::int64 lostReach (double sampleRate);
/** The lost samples in [s0, s1) of all these tracks together (for FixSpec::lost: 'at' from s0). */
std::vector<FixSpec::Lost> findLost (const std::vector<std::vector<float>>& tracks, double sampleRate, long s0, long s1, double sensitivity);

/** One change: a fix applied to the samples [s0, s1) (in samples of the take, or of the loaded area where a caller says so). */
struct FixOp { FixSpec spec; juce::int64 s0 = 0, s1 = 0; };

/** One file of a take, made again with a fix applied to the part [s0, s1) (in samples of the take). The new file is the same length as the old one.
    Returns the new file (empty File on failure). With a list of changes they are all made, one after another, in a single new file. */
juce::File fixWholeFile (juce::AudioFormatManager&, const juce::File& source, juce::int64 s0, juce::int64 s1, const FixSpec&, const juce::File& dest, juce::String& error);
juce::File fixWholeFile (juce::AudioFormatManager&, const juce::File& source, const std::vector<FixOp>& ops, const juce::File& dest, juce::String& error);

/** What a fix produces for one region file: a short new file that holds the take's samples [from, to). */
struct PieceResult { juce::File file; juce::int64 from = 0, to = 0; bool ok = false; };
/** 'range' is in the TAKE's samples; the region file starts at 'fileStart'. 'outFrom' / 'outTo' is what is written (the fix region plus extra, clamped to the file).
    The fix is applied to [fixFrom, fixTo), or each change of the list to its own range. */
PieceResult fixPiece (juce::AudioFormatManager&, const juce::File& source, juce::int64 fileStart,
                      juce::int64 outFrom, juce::int64 outTo, juce::int64 fixFrom, juce::int64 fixTo,
                      const FixSpec&, const juce::File& dest, juce::String& error);
PieceResult fixPiece (juce::AudioFormatManager&, const juce::File& source, juce::int64 fileStart,
                      juce::int64 outFrom, juce::int64 outTo, const std::vector<FixOp>& ops, const juce::File& dest, juce::String& error);

/** The background denoiser (Denoise.h) on whole files. A profile per channel of the file, learned from the room tone, and (optionally) the room's
    decay per channel, measured over the file once so that a preview and the final run agree. */
struct DenoiseProfiles
{
    std::vector<denoise::Profile> perChannel; std::vector<denoise::Decay> decay;     // decay: empty, or one per channel
    double sampleRate = 0.0; int channels = 0;
    bool valid() const { return channels > 0 && (int) perChannel.size() == channels; }
};
/** Learns a profile per channel of 'file' from its samples [s0, s1): the part that holds nothing but the room (the first 60 s of it, if longer).
    With 'decaySeconds' > 0 the room's decay is measured as well, over up to that much of the file after the room tone. Empty profiles + 'error' on failure. */
DenoiseProfiles learnDenoise (juce::AudioFormatManager&, const juce::File&, juce::int64 s0, juce::int64 s1, const denoise::Params&, juce::String& error, double decaySeconds = 0.0);
/** One run of the denoiser over the files of a take (all of them together: with Params::groupMode > 0 each track's gains use what the other tracks hear). */
struct DenoiseRun
{
    juce::int64 a = 0, b = std::numeric_limits<juce::int64>::max();   // the stretch to denoise (samples); the rest is copied
    bool preview = false;                                             // a preview: the part before the stretch is written as silence and the file ends with the stretch
    bool removed = false;                                             // also write what was taken out (input - output) to the 'removed' files
    int threads = 1;                                                  // tracks (channels) done at once
};
/** Writes dests[i]: sources[i] with the stretch denoised (24-bit, same rate and channels) and, when run.removed, removed[i] with what was taken out.
    All sources must have the same sample rate; each has its own DenoiseProfiles. 'progress' (optional) is told how far it has got, 0..1, and stops the
    work by returning false; false (and no files) when cancelled or failed. */
bool denoiseFiles (juce::AudioFormatManager&, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests, const std::vector<juce::File>& removed,
                   const std::vector<DenoiseProfiles>&, const denoise::Params&, const DenoiseRun&, const std::function<bool (float)>& progress, juce::String& error);
/** One file on its own (no other tracks to listen to): [a, b) denoised, the rest copied as it is. */
bool denoiseFile (juce::AudioFormatManager&, const juce::File& source, const juce::File& dest, const juce::File& removed, juce::int64 a, juce::int64 b,
                  const DenoiseProfiles&, const denoise::Params&, const std::function<bool (float)>& progress, juce::String& error);

/** The take-window version of a fix. Runs on a job thread; makes new files and returns them (the model is changed by the caller). */
struct TakeFixResult { std::vector<juce::File> newFiles; juce::String error; };
TakeFixResult fixTakeFiles (juce::AudioFormatManager&, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            juce::int64 s0, juce::int64 s1, const FixSpec&, AudioJob*);
TakeFixResult fixTakeFiles (juce::AudioFormatManager&, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            const std::vector<FixOp>& ops, AudioJob*);
} // namespace audioops
} // namespace td

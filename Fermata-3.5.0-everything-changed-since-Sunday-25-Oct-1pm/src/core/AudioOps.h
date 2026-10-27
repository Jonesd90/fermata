#pragma once
#include "Project.h"
#include "PitchShift.h"
#include "SpectralRepair.h"
#include "Denoise.h"

namespace td
{
/** What a fix does to the audio. */
struct FixSpec
{
    enum class Kind { Pitch, Patch, Declick, Reharmonise, Denoise } kind = Kind::Pitch;
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
    /** Denoise: the background denoiser (Denoise.h) on the box - its time span is [s0, s1), its pitch range f0 .. f1 (the profile's weight is set to it, feathered a
        quarter of an octave inside; outside the box nothing is touched), with the room-tone profile of the channels being processed. The profiles are not part of
        the settings: the caller sets 'dnProfiles' (one per channel of the audio it hands over, or one for all) before each call - the project's room tone, or one
        learned from a box of the loaded area ('dnBoxT0 / T1', seconds from the start of the area, both < 0 = the project's). */
    denoise::Params dn;
    double dnBoxT0 = -1.0, dnBoxT1 = -1.0;
    std::shared_ptr<const std::vector<denoise::Profile>> dnProfiles;
    juce::String shortName() const
    {
        if (kind == Kind::Pitch && useCurve) return "pitch curve";
        if (kind == Kind::Pitch) return "pitch " + juce::String (cents >= 0 ? "+" : "") + juce::String (cents, 0) + "c";
        if (kind == Kind::Reharmonise) return "reharmonised";
        if (kind == Kind::Denoise) return "denoise";
        return kind == Kind::Patch ? "repair" : "declick";
    }
};

/** A job that runs on its own thread; the result is picked up on the message thread.
    Its owner (a window) makes it with 'new' and hands it back with release() when it goes: the work is asked to stop and the job deletes itself once
    its thread has finished. So a window that is closed during a long job never waits for it: the work may not look at the request for a long time
    (a de-click or denoise of a long file), and waiting froze the program for up to 20 s, after which the thread was killed - which can hang it for good. */
class AudioJob : private juce::Thread
{
public:
    using Work = std::function<juce::String (AudioJob&)>;            // returns an error text ("" = fine)
    AudioJob() : juce::Thread ("Audio job") {}
    ~AudioJob() override { cancel = true; stopThread (20000); }      // (through release(): only once its thread has finished)
    /** The owner is going: the work is asked to stop, and from here on the job looks after itself - it deletes itself (on the message thread) once its
        thread has finished, at once if it is not running. The onDone of the last start() is still called: it must check that its window is still there.
        Message thread. */
    void release()
    {
        cancel = true;
        const juce::ScopedLock sl (endLock);
        released = true;
        if (! isThreadRunning() || finished.load()) deleteSoon();
    }
    /** onDone runs on the message thread with the error text. A job still running from the last start() is asked to stop and waited for first,
        and its own onDone is then dropped, so the window only hears about the job it asked for last. (3.1.2. Before, the old thread was killed
        after 5 s - and that always happened, because a job looks at cancelled(), never at the thread's own stop request. A thread killed in the
        middle of writing a file or asking for memory can hang the whole program for good. Reachable in the ReHarmoniSer: Isolate twice quickly.) */
    void start (Work w, std::function<void (const juce::String&)> onDone)
    {
        if (isThreadRunning())
        {
            cancel = true;                                   // (a job that has just finished is only winding its thread down: that is a moment)
            const auto t0 = juce::Time::getMillisecondCounter();
            if (! waitForThreadToExit (30000))
            {
                juce::Logger::writeToLog ("Audio job: the previous job has not stopped after 30 s; the new one was not started");
                if (onDone) onDone ("The previous job is still running. Please wait for it to finish, then try again.");
                return;
            }
            if (juce::Time::getMillisecondCounter() - t0 > 2000)
                juce::Logger::writeToLog ("Audio job: waited " + juce::String ((juce::Time::getMillisecondCounter() - t0) / 1000) + " s for the previous job to stop");
        }
        ++*generation;                                       // (the replaced job's onDone, if it is still on its way, is dropped: see run)
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
        auto cb = done; auto gen = generation; const int g = gen->load();
        juce::MessageManager::callAsync ([cb, err, gen, g] { if (cb && gen->load() == g) cb (err); });     // (not when a newer start() has replaced this job)
        const juce::ScopedLock sl (endLock);
        finished = true;
        if (released) deleteSoon();                                  // (its owner has gone: nothing else will)
    }
    /** After run() has returned (or when it never ran): the destructor's stopThread then only waits for the thread itself to end. */
    void deleteSoon() { juce::MessageManager::callAsync ([this] { delete this; }); }
    Work work; std::function<void (const juce::String&)> done;
    std::atomic<bool> cancel { false }, finished { false }; std::atomic<float> progressValue { 0.0f };
    std::shared_ptr<std::atomic<int>> generation = std::make_shared<std::atomic<int>> (0);     // which start() the running job belongs to (shared: the posted onDone may outlive the job)
    juce::CriticalSection endLock; bool released = false;            // (release() and the end of run(): whichever comes second deletes the job)
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
/** After a writer has been closed: checks that the file holds exactly 'frames' sample frames. A full drive, or one that has gone away, can leave a
    file short while every write said it went well (the last buffer only reaches the disk when the writer closes, where nothing can report it),
    so every file Fermata makes is looked at once more before it is called done (3.1.3). A WAV is checked header against bytes (wavrepair::inspect:
    a header that says more than the file holds is the mark of a file cut short); any other format by its reader's length, and for PCM by its size.
    "" when all is well, else a sentence saying what is wrong. 'fm' may be null (a manager is made for the call). */
juce::String checkWrittenLength (juce::AudioFormatManager* fm, const juce::File&, juce::int64 frames);

/** A file for an AudioFormatWriter to write, kept here so that it can be checked once the writer has been closed: whether every byte reached the
    disk. The writer's own stream can only report a failure while it is being written to; what its destructor flushes - the last buffer, the final
    header - reports nothing, and a compressed file (FLAC) cut short that way keeps a header that says it is whole (3.1.3). */
class CheckedAudioFile
{
public:
    explicit CheckedAudioFile (const juce::File& f);
    ~CheckedAudioFile();
    bool failedToOpen() const { return out == nullptr || out->failedToOpen(); }
    /** The stream to give the writer: it only forwards to the file kept here, which stays open when the writer lets go of it. */
    std::unique_ptr<juce::OutputStream> stream();
    /** After the writer has been closed: "" when every byte reached the disk and the file is as long as everything written to it, else what is wrong.
        The file is closed here (so it can be renamed over or deleted afterwards, which Windows refuses while it is open). */
    juce::String finish();
private:
    struct Forwarder;
    juce::File file; std::unique_ptr<juce::FileOutputStream> out; juce::int64 maxPos = 0;
};

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

/** The room-tone profiles of a file for the background denoiser (Denoise.h): one per channel, learned from the room tone. */
struct DenoiseProfiles
{
    std::vector<denoise::Profile> perChannel;
    double sampleRate = 0.0; int channels = 0;
    bool valid() const { return channels > 0 && (int) perChannel.size() == channels; }
};
/** Learns a profile per channel of 'file' from its samples [s0, s1): the part that holds nothing but the room (the first 60 s of it, if longer).
    Empty profiles + 'error' on failure. */
DenoiseProfiles learnDenoise (juce::AudioFormatManager&, const juce::File&, juce::int64 s0, juce::int64 s1, const denoise::Params&, juce::String& error);

/** The take-window version of a fix. Runs on a job thread; makes new files and returns them (the model is changed by the caller). */
struct TakeFixResult { std::vector<juce::File> newFiles; juce::String error; };
TakeFixResult fixTakeFiles (juce::AudioFormatManager&, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            juce::int64 s0, juce::int64 s1, const FixSpec&, AudioJob*);
/** 'profilesFor' (optional): a denoise among the ops gets the room-tone profiles (one per channel) of each source file from it. */
TakeFixResult fixTakeFiles (juce::AudioFormatManager&, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            const std::vector<FixOp>& ops, AudioJob*, const std::function<std::shared_ptr<const std::vector<denoise::Profile>> (size_t, const FixSpec&)>& profilesFor = {});
} // namespace audioops
} // namespace td

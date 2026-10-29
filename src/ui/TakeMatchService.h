#pragma once
#include "../core/Project.h"
#include "../core/CrashReport.h"
#include "../core/AudioEngine.h"
#include "../core/TakeMatch.h"
#include "../core/AudioFormats.h"
#include <map>
#include <deque>
#include <memory>

namespace td
{
/** Keeps a fingerprint (core/TakeMatch.h) for every take and lines takes up on request: the plumbing behind "the same place in another
    take" (3.4.0). A thread of its own makes the fingerprints, one take at a time, at low priority, and never while a take is being
    recorded (the recording's disk writes come first: work under way is cancelled and done later). Each fingerprint is saved in the
    project folder's Analysis folder, so a project opened again does not analyse its takes afresh - unless a take's files have changed.
    Takes that have no fingerprint yet are found by a sweep every few seconds (a take just recorded, imported, or whose files came back).
    Everything here is called on the message thread except the thread's own run(). */
class TakeMatchService : private juce::Thread, private juce::Timer
{
public:
    TakeMatchService (Project& p, AudioEngine& e) : juce::Thread ("Take matching"), project (p), engine (e)
    {
        registerAudioFormats (formats);
        startThread (juce::Thread::Priority::low);
        startTimer (3000);
    }
    ~TakeMatchService() override
    {
        stop();
        if (isThreadRunning()) stopThread (2000);        // (still busy as the object goes: it cannot be left running on a deleted object)
    }

    /** Stops the background analysis (3.17.0: called early in closing, timed in the closing log). Waits up to 3 s for the thread to finish the block
        it is reading; if it has not, it is left to end with the program (the closing safety net in Main ends the program if it lingers) - it is
        never killed half-way through by this. Safe to call again. */
    void stop()
    {
        stopTimer();
        cancel = true; signalThreadShouldExit(); wake.signal();
        if (isThreadRunning() && ! waitForThreadToExit (3000))
        {
            if (! stopLogged) juce::Logger::writeToLog ("Take matching: still busy 3 s after being told to stop - left to end with the program");
            stopLogged = true;
            return;
        }
        stopThread (100);                                // (finished: this only tidies up)
    }

    /** The project has been replaced (opened, new, closed): everything known belongs to the old one. */
    void projectReplaced()
    {
        cancel = true;
        const juce::ScopedLock sl (lock);
        queue.clear(); known.clear(); alignments.clear();
        wake.signal();
    }
    /** A take has just finished recording: its fingerprint is wanted soon (the sweep would find it anyway). */
    void takeFinished (const juce::Uuid& takeId) { enqueue (takeId, true); }
    /** A take has just started recording: work under way stops at once (the recording's disk writes come first); the take is done later. */
    void recordingStarted() { cancel = true; }

    enum class State { Unknown, Queued, Analysing, Ready, NoAudio, Failed };
    struct Status { State state = State::Unknown; float progress = 0.0f; juce::String why; };
    Status statusOf (const juce::Uuid& takeId) const
    {
        const juce::ScopedLock sl (lock);
        auto it = known.find (takeId);
        if (it == known.end()) return {};
        return { it->second.state, it->second.progress, it->second.why };
    }
    /** The take's fingerprint, or null when it is not ready (then it is queued, so it will be). A take whose files have changed since
        is analysed again. */
    std::shared_ptr<const takematch::Fingerprint> fingerprintFor (const juce::Uuid& takeId)
    {
        {
            const juce::ScopedLock sl (lock);
            auto it = known.find (takeId);
            if (it != known.end() && it->second.state == State::Ready && it->second.fp != nullptr)
            {
                if (it->second.fp->sourceKey == takematch::sourceKeyFor (filesOf (takeId))) return it->second.fp;
                it->second = Entry();                       // (the files are not the ones the fingerprint came from)
            }
        }
        enqueue (takeId, false);
        return nullptr;
    }
    /** The alignment of take A with take B, made now (a few tens of milliseconds for ten-minute takes) and kept. Null when either
        fingerprint is not ready. */
    std::shared_ptr<const takematch::Alignment> alignmentFor (const juce::Uuid& a, const juce::Uuid& b)
    {
        auto fa = fingerprintFor (a), fb = fingerprintFor (b);
        if (fa == nullptr || fb == nullptr) return nullptr;
        const auto key = std::make_pair (a, b);
        {
            const juce::ScopedLock sl (lock);
            auto it = alignments.find (key);
            if (it != alignments.end() && it->second.fa == fa && it->second.fb == fb) return it->second.al;
        }
        auto al = std::make_shared<takematch::Alignment> (takematch::align (*fa, *fb));
        const juce::ScopedLock sl (lock);
        if (alignments.size() > 400) alignments.clear();        // (a session's worth; they are quick to make again)
        alignments[key] = { fa, fb, al };
        return al;
    }
    /** How many takes are waiting for or having their fingerprint made (for a status line). */
    int pending() const
    {
        const juce::ScopedLock sl (lock);
        int n = 0;
        for (auto& kv : known) if (kv.second.state == State::Queued || kv.second.state == State::Analysing) ++n;
        return n;
    }

private:
    struct Entry { State state = State::Unknown; float progress = 0.0f; juce::String why; std::shared_ptr<const takematch::Fingerprint> fp; };
    struct Job { juce::Uuid takeId = juce::Uuid::null(); std::vector<juce::File> files; juce::File cacheFile; };
    struct Cached { std::shared_ptr<const takematch::Fingerprint> fa, fb; std::shared_ptr<const takematch::Alignment> al; };

    /** The files a take's fingerprint is made from: its MAIN microphones (3.7.0, from real takes: the main pair hears the whole choir; a spot hears
        part of it, and the first four tracks of a take used to be whatever came first - often spots). The tracks given the role Main in the Align
        window; failing that, the tracks whose names say main pair (MAIN, AB, A-B, ORTF, DECCA, TREE, XY, MS, NOS, OMNI, PAIR); failing that, the first
        four tracks. Never the LTC track. */
    std::vector<juce::File> filesOf (const juce::Uuid& takeId) const
    {
        std::vector<juce::File> all, byRole, byName;
        for (auto& w : project.takeWindows)
            if (auto* g = w->findGroup (takeId))
            {
                for (auto& f : g->files)
                {
                    if (f.file == juce::File()) continue;
                    const auto* t = trackOf (f.trackId);
                    if (t != nullptr && t->isLtc) continue;
                    all.push_back (f.file);
                    if (t != nullptr && t->alignRole == 1) byRole.push_back (f.file);
                    if (soundsLikeMain (t != nullptr ? t->name : f.trackName)) byName.push_back (f.file);
                }
                break;
            }
        if (! byRole.empty()) return byRole;
        if (! byName.empty()) return byName;
        if (all.size() > 4) all.resize (4);
        return all;
    }
    static bool soundsLikeMain (const juce::String& name)
    {
        auto words = juce::StringArray::fromTokens (name.toUpperCase().replaceCharacters ("-_/.()", "      "), " ", "");
        words.removeEmptyStrings();
        const auto joined = name.toUpperCase().removeCharacters (" -_/.");
        for (auto& w : words)
            for (auto* key : { "MAIN", "MAINS", "AB", "ORTF", "DECCA", "TREE", "XY", "MS", "NOS", "OMNI", "OMNIS", "PAIR" })
                if (w == key) return true;
        return joined.contains ("MAINPRINT") || joined.contains ("DECCATREE");
    }
    const TrackDef* trackOf (const juce::Uuid& id) const { for (auto& t : project.tracks) if (t.id == id) return &t; return nullptr; }
    juce::File cacheFileFor (const juce::Uuid& takeId) const
    {
        if (project.projectFile == juce::File()) return {};
        return project.projectFolder().getChildFile ("Analysis").getChildFile (takeId.toString() + ".fpr");
    }
    void enqueue (const juce::Uuid& takeId, bool front)
    {
        const auto files = filesOf (takeId);
        const juce::ScopedLock sl (lock);
        auto& e = known[takeId];
        if (e.state == State::Queued || e.state == State::Analysing || e.state == State::Ready) return;
        if (files.empty()) { e.state = State::NoAudio; e.why = "this take has no audio files to analyse"; return; }
        if (e.state == State::Failed && e.why.isNotEmpty()) return;      // (tried and failed: not tried again until the files change or the project is opened again)
        e.state = State::Queued; e.progress = 0.0f; e.why.clear();
        Job job { takeId, files, cacheFileFor (takeId) };
        if (front) queue.push_front (std::move (job)); else queue.push_back (std::move (job));
        wake.signal();
    }
    /** The sweep: every take the service does not know yet is queued (the ones just recorded first, as the queue is a deque: pushed to the back, so in the project's order). */
    void timerCallback() override
    {
        if (engine.isRecording()) { cancel = true; return; }
        std::vector<juce::Uuid> unknown;
        {
            const juce::ScopedLock sl (lock);
            for (auto& w : project.takeWindows)
                for (auto& g : w->groups)
                    if (known.find (g.id) == known.end()) unknown.push_back (g.id);
        }
        for (auto& id : unknown) enqueue (id, false);
    }
    void run() override
    {
        td::crashreport::nameThisThread (getThreadName());                // (3.17.0: named for the log)
        while (! threadShouldExit())
        {
            // 3.17.2: whether there is a job is told by the queue itself. (It used to be told by the job's take id being null - but a juce::Uuid made
            // without a value is a NEW RANDOM id, never null: with nothing queued, the thread analysed a made-up take with no files, filed it under that
            // id, and went round again at once, for ever - a whole core busy, millions of entries piling up (22 GB), and minutes to free them on closing.)
            Job job;
            bool have = false;
            {
                const juce::ScopedLock sl (lock);
                if (! queue.empty()) { job = std::move (queue.front()); queue.pop_front(); have = true; }
            }
            if (! have) { wake.wait (1000); continue; }
            if (engine.isRecording())                    // not while a take records: back on the queue, tried again in a moment
            {
                { const juce::ScopedLock sl (lock); queue.push_front (std::move (job)); }
                wait (1500);
                continue;
            }
            cancel = false;
            takematch::Fingerprint fp;
            bool ok = false;
            const auto key = takematch::sourceKeyFor (job.files);
            if (job.cacheFile != juce::File() && takematch::load (fp, job.cacheFile) && fp.sourceKey == key) ok = true;      // saved last time, and the files are the same
            else
            {
                { const juce::ScopedLock sl (lock); known[job.takeId].state = State::Analysing; }
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                fp = takematch::analyseFiles (formats, job.files, &cancel, [this, id = job.takeId] (float p) { const juce::ScopedLock sl (lock); known[id].progress = p; });
                ok = ! fp.empty() && ! cancel;
                if (ok)
                {
                    if (job.cacheFile != juce::File() && ! takematch::save (fp, job.cacheFile)) juce::Logger::writeToLog ("Take matching: the fingerprint could not be saved in " + job.cacheFile.getParentDirectory().getFullPathName());
                    juce::Logger::writeToLog ("Take matching: " + juce::String ((int) job.files.size()) + (job.files.size() == 1 ? " file" : " files") + " of a take analysed in "
                                              + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms (" + juce::String (fp.lengthSeconds(), 1) + " s)");
                }
            }
            const juce::ScopedLock sl (lock);
            if (cancel) { known.erase (job.takeId); continue; }               // (cancelled: a recording started, or the project went; the sweep queues it again when the time is right)
            auto& e = known[job.takeId];
            if (ok) { e.state = State::Ready; e.progress = 1.0f; e.why.clear(); e.fp = std::make_shared<const takematch::Fingerprint> (std::move (fp)); }
            else { e.state = State::Failed; e.why = "the take's audio could not be read"; e.fp = nullptr; }
        }
    }

    Project& project;
    AudioEngine& engine;
    juce::AudioFormatManager formats;
    mutable juce::CriticalSection lock;
    std::map<juce::Uuid, Entry> known;
    std::deque<Job> queue;
    std::map<std::pair<juce::Uuid, juce::Uuid>, Cached> alignments;
    std::atomic<bool> cancel { false };
    juce::WaitableEvent wake;
    bool stopLogged = false;
};
} // namespace td

#pragma once
#include "../core/Project.h"
#include "../core/CrashReport.h"
#include "../core/AudioEngine.h"
#include "../core/TakeMatch.h"
#include "../core/AudioFormats.h"
#include <map>
#include <set>
#include <deque>
#include <memory>

namespace td
{
/** Keeps a fingerprint (core/TakeMatch.h) for every take and lines takes up on request: the plumbing behind "the same place in another
    take" (3.4.0). A thread of its own makes the fingerprints, one take at a time, at low priority, and never while a take is being
    recorded (the recording's disk writes come first: work under way is cancelled and done later). Each fingerprint is saved in the
    project folder's Analysis folder, so a project opened again does not analyse its takes afresh - unless a take's files have changed.
    Takes that have no fingerprint yet are found by a sweep every few seconds (a take just recorded, imported, or whose files came back).
    3.18.0: nothing is analysed in secret any more. Only the take windows the person has switched take matching on for are analysed (off until then,
    saved with each window), never while Rec mode is on (recording / tracking: Fermata keeps the computer for the recording), and never while the
    person has paused it; progress() says how far it has got and roughly how long is left (the MATCHING box in the main window and its panel).
    A fingerprint made (or saved) before is used at once when a window is switched on again.
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
        const juce::ScopedLock sl (lock);
        requeueOnCancel = false; cancel = true;
        queue.clear(); known.clear(); alignments.clear();
        audioPerSecond = 0.0;
        wake.signal();
    }

    // ---- 3.18.0: the person decides when the analysis runs ----
    /** Is take matching switched on for the take window this take is in? */
    bool isOnFor (const juce::Uuid& takeId) const
    {
        for (auto& w : project.takeWindows) if (w->findGroup (takeId) != nullptr) return w->takeMatching;
        return false;
    }
    /** A take window was switched on or off. The queue keeps only the takes of windows switched on; a take of a window switched off that is being
        analysed stops at once (what was finished stays known: switched on again, it is used at once); then the sweep, at once. */
    void windowsChanged()
    {
        {
            const juce::ScopedLock sl (lock);
            std::deque<Job> keep;
            for (auto& j : queue)
            {
                if (isOnFor (j.takeId)) { keep.push_back (std::move (j)); continue; }
                if (auto it = known.find (j.takeId); it != known.end() && it->second.state == State::Queued) known.erase (it);
            }
            queue.swap (keep);
            if (! current.isNull() && ! isOnFor (current)) { requeueOnCancel = false; cancel = true; }
        }
        timerCallback();
    }
    /** Pause / resume (the MATCHING panel). Not saved: a restart resumes. Pausing stops the take under way at once; it starts again first on resume. */
    void setPaused (bool p) { paused = p; holdChanged(); }
    bool isPaused() const { return paused.load(); }
    /** Rec mode: held while it is on (AppContext::setRecMode). */
    void setRecHeld (bool h) { recHeld = h; holdChanged(); }
    bool isHeld() const { return paused.load() || recHeld.load(); }

    /** How far the analysis has got, for the take windows switched on (or for one window, switched on or not). Message thread. */
    struct Progress
    {
        int windowsOn = 0;              // take windows switched on (in scope)
        int total = 0;                  // their takes
        int ready = 0;                  // fingerprinted (or loaded from the Analysis folder)
        int cannot = 0;                 // no audio files, or the audio could not be read
        juce::Uuid nowTake = juce::Uuid::null();   // the take being analysed now (null: none)
        float nowProgress = 0.0f;
        double secondsLeft = -1.0;      // roughly how long the rest takes (-1: not known yet - no take has been analysed since the start)
        int left() const { return juce::jmax (0, total - ready - cannot); }
    };
    Progress progress (const TakeWindowDef* onlyWindow = nullptr) const
    {
        Progress pr;
        double audioLeft = 0.0;
        const juce::ScopedLock sl (lock);
        for (auto& w : project.takeWindows)
        {
            if (onlyWindow != nullptr ? w.get() != onlyWindow : ! w->takeMatching) continue;
            if (w->takeMatching) ++pr.windowsOn;
            for (auto& g : w->groups)
            {
                ++pr.total;
                auto it = known.find (g.id);
                const State st = it != known.end() ? it->second.state : State::Unknown;
                if (st == State::Ready) { ++pr.ready; continue; }
                if (st == State::NoAudio || st == State::Failed) { ++pr.cannot; continue; }
                if (st == State::Analysing) { pr.nowTake = g.id; pr.nowProgress = it->second.progress; audioLeft += g.lengthSeconds() * (1.0 - (double) it->second.progress); }
                else if (st != State::Queued || ! it->second.hasCache) audioLeft += g.lengthSeconds();       // (one with a saved fingerprint is a moment)
            }
        }
        if (audioPerSecond > 0.0) pr.secondsLeft = audioLeft / audioPerSecond;
        return pr;
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
    /** The take's fingerprint, or null when it is not ready (then it is queued, so it will be - if take matching is switched on for its window).
        A take whose files have changed since is analysed again. */
    std::shared_ptr<const takematch::Fingerprint> fingerprintFor (const juce::Uuid& takeId)
    {
        {
            const juce::ScopedLock sl (lock);
            auto it = known.find (takeId);
            if (it != known.end() && it->second.state == State::Ready)
            {
                if (it->second.fp == nullptr && it->second.hasCache)           // 3.18.3: let go of to save memory (see trimResident): read back from the Analysis folder
                {
                    takematch::Fingerprint back;
                    if (takematch::load (back, cacheFileFor (takeId))) it->second.fp = std::make_shared<const takematch::Fingerprint> (std::move (back));
                }
                if (it->second.fp != nullptr && it->second.fp->sourceKey == takematch::sourceKeyFor (filesOf (takeId)))
                {
                    it->second.used = ++useCounter;
                    trimResident();
                    return it->second.fp;
                }
                it->second = Entry();                       // (the files are not the ones the fingerprint came from, or the saved one has gone)
            }
        }
        enqueue (takeId, false);
        return nullptr;
    }
    /** How many fingerprints are in memory right now (the rest of the Ready takes are read back from the Analysis folder when asked for). For the tests. */
    int residentFingerprints() const
    {
        const juce::ScopedLock sl (lock);
        int n = 0; for (auto& kv : known) if (kv.second.fp != nullptr) ++n;
        return n;
    }
    /** At most this many fingerprints stay in memory (3.18.3): a ten-minute take's is about 3 MB, and a project of 367 takes with take matching on
        everywhere held half a gigabyte. The ones used longest ago are let go and read back from the Analysis folder when asked for (a few ms). */
    static constexpr int kResident = 48;
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
            if (it != alignments.end() && it->second.ka == fa->sourceKey && it->second.kb == fb->sourceKey) return it->second.al;      // (3.18.3: by the files' keys, not the pointers - a kept pointer pinned the fingerprint in memory)
        }
        auto al = std::make_shared<takematch::Alignment> (takematch::align (*fa, *fb));
        const juce::ScopedLock sl (lock);
        if (alignments.size() > 400) alignments.clear();        // (a session's worth; they are quick to make again)
        alignments[key] = { fa->sourceKey, fb->sourceKey, al };
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
    struct Entry { State state = State::Unknown; float progress = 0.0f; juce::String why; std::shared_ptr<const takematch::Fingerprint> fp; bool hasCache = false; juce::uint64 used = 0; };
    /** Under 'lock': lets go of the fingerprints used longest ago beyond kResident - only those saved in the Analysis folder, so they can come back. */
    void trimResident()
    {
        for (;;)
        {
            int resident = 0; Entry* oldest = nullptr;
            for (auto& kv : known)
                if (kv.second.fp != nullptr) { ++resident; if (kv.second.hasCache && (oldest == nullptr || kv.second.used < oldest->used)) oldest = &kv.second; }
            if (resident <= kResident || oldest == nullptr) return;
            oldest->fp = nullptr;
        }
    }
    struct Job { juce::Uuid takeId = juce::Uuid::null(); std::vector<juce::File> files; juce::File cacheFile; };
    struct Cached { juce::String ka, kb; std::shared_ptr<const takematch::Alignment> al; };     // (the source keys of the two fingerprints it was made from)

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
        if (! isOnFor (takeId)) return;                                  // (3.18.0: only the windows switched on)
        const auto files = filesOf (takeId);
        const auto cacheFile = cacheFileFor (takeId);
        const bool hasCache = cacheFile.existsAsFile();
        const juce::ScopedLock sl (lock);
        auto& e = known[takeId];
        if (e.state == State::Queued || e.state == State::Analysing || e.state == State::Ready) return;
        if (files.empty()) { e.state = State::NoAudio; e.why = "this take has no audio files to analyse"; return; }
        if (e.state == State::Failed && e.why.isNotEmpty()) return;      // (tried and failed: not tried again until the files change or the project is opened again)
        e.state = State::Queued; e.progress = 0.0f; e.why.clear(); e.hasCache = hasCache;
        Job job { takeId, files, cacheFile };
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
                if (w->takeMatching)                                       // (3.18.0: only the windows switched on)
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
            if (isHeld()) { wake.wait (500); continue; }                 // (3.18.0: paused, or Rec mode: nothing is touched until it is let go)
            Job job;
            bool have = false;
            {
                const juce::ScopedLock sl (lock);
                if (! queue.empty()) { job = std::move (queue.front()); queue.pop_front(); have = true; current = job.takeId; }
            }
            if (! have) { wake.wait (1000); continue; }
            cancel = false;
            if (engine.isRecording() || isHeld())        // not while a take records, nor once held: back on the queue, tried again later
            {                                            // (checked after cancel was cleared: a hold that came in between still stops the work)
                { const juce::ScopedLock sl (lock); queue.push_front (std::move (job)); current = juce::Uuid::null(); requeueOnCancel = false; }
                wait (1500);
                continue;
            }
            takematch::Fingerprint fp;
            bool ok = false, saved = false;
            const auto key = takematch::sourceKeyFor (job.files);
            if (job.cacheFile != juce::File() && takematch::load (fp, job.cacheFile) && fp.sourceKey == key) ok = saved = true;      // saved last time, and the files are the same
            else
            {
                { const juce::ScopedLock sl (lock); known[job.takeId].state = State::Analysing; }
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                fp = takematch::analyseFiles (formats, job.files, &cancel, [this, id = job.takeId] (float p) { const juce::ScopedLock sl (lock); known[id].progress = p; });
                ok = ! fp.empty() && ! cancel;
                if (ok)
                {
                    const double took = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;       // (3.18.0: how quickly takes go, for the time left)
                    if (took > 0.001 && fp.lengthSeconds() > 0.0)
                    {
                        const double r = fp.lengthSeconds() / took;
                        const juce::ScopedLock sl (lock);
                        audioPerSecond = audioPerSecond > 0.0 ? 0.7 * audioPerSecond + 0.3 * r : r;
                    }
                    saved = job.cacheFile != juce::File() && takematch::save (fp, job.cacheFile);
                    if (job.cacheFile != juce::File() && ! saved) juce::Logger::writeToLog ("Take matching: the fingerprint could not be saved in " + job.cacheFile.getParentDirectory().getFullPathName());
                    juce::Logger::writeToLog ("Take matching: " + juce::String ((int) job.files.size()) + (job.files.size() == 1 ? " file" : " files") + " of a take analysed in "
                                              + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms (" + juce::String (fp.lengthSeconds(), 1) + " s)");
                }
            }
            const juce::ScopedLock sl (lock);
            current = juce::Uuid::null();
            if (cancel)
            {
                if (requeueOnCancel)                     // (3.18.0: held - paused or Rec mode: it goes back to the front, first when the hold is let go)
                {
                    requeueOnCancel = false;
                    auto& e = known[job.takeId]; e.state = State::Queued; e.progress = 0.0f;
                    queue.push_front (std::move (job));
                }
                else known.erase (job.takeId);           // (cancelled: a recording started, the project went or its window was switched off; the sweep queues it again when the time is right)
                continue;
            }
            auto& e = known[job.takeId];
            if (ok) { e.state = State::Ready; e.progress = 1.0f; e.why.clear(); e.fp = std::make_shared<const takematch::Fingerprint> (std::move (fp)); e.hasCache = saved; e.used = ++useCounter; trimResident(); }
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
    // 3.18.0
    std::atomic<bool> paused { false }, recHeld { false };
    bool requeueOnCancel = false;                    // (under 'lock') a hold cancelled the take under way: it goes back on the queue, not away
    juce::Uuid current = juce::Uuid::null();         // (under 'lock') the take the thread is working on, null when none
    double audioPerSecond = 0.0;                     // (under 'lock') seconds of takes analysed per second, lately: 0 until the first is done
    juce::uint64 useCounter = 0;                     // (under 'lock') for the "used longest ago" order of trimResident
    /** Paused or Rec mode just changed. Held: the take under way stops now and goes back on the queue. Let go: the thread wakes at once. */
    void holdChanged()
    {
        if (isHeld())
        {
            const juce::ScopedLock sl (lock);
            if (! current.isNull()) { requeueOnCancel = true; cancel = true; }
        }
        wake.signal();
    }
};
} // namespace td

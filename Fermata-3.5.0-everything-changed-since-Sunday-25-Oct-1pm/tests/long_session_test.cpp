// A long recording session, simulated faster than real time (3.1.3, the code sweep's area B): hours of takes with everything that runs alongside
// them in the program - the meters and readouts polled as the windows poll them, the undo tick, the automatic save, the second-drive copy, a
// playback of the last take between takes - looking for memory that grows, lists that never shrink and timing that drifts.
//
// Time is the pretend audio thread's block count (48 kHz, 512 samples a block). The audio thread runs flat out between takes and is held to
// about 20 x real time while a take records, so the disk writer can keep up (overruns would be the sandbox's disk, not a finding). Finished takes'
// files are deleted as the session goes on (hours of 24-bit audio would not fit the sandbox); only the last take's files stay, for the playback.
//
// Two halves: first the session as it really runs (every take stays in the project: the memory may grow with the project), then the same cycle
// in a steady state (every finished take is dropped from the window once the next one exists, so the project stays the same size): any growth
// there is a leak, or a list that nobody empties.
//
// Usage: long_session_test [simulated hours] (default 2)
#include "../src/core/AudioEngine.h"
#include "../src/core/SaveWriter.h"
#include "../src/core/WavRepair.h"
#include <cstdio>
#include <thread>
#include <random>
#include <malloc.h>

using namespace td;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

/** Bytes of heap in use by the program (glibc), in MB. */
static double heapMb()
{
    const auto mi = mallinfo2();
    return (double) (mi.uordblks + mi.hblkhd) / (1024.0 * 1024.0);
}

/** The resident set, in MB (what the operating system has given the process), from /proc. */
static double rssMb()
{
    FILE* f = std::fopen ("/proc/self/statm", "r");
    if (f == nullptr) return 0.0;
    long size = 0, resident = 0;
    const int got = std::fscanf (f, "%ld %ld", &size, &resident);
    std::fclose (f);
    return got == 2 ? (double) resident * 4096.0 / (1024.0 * 1024.0) : 0.0;
}

int main (int argc, char** argv)
{
    const double hours = argc > 1 ? atof (argv[1]) : 2.0;
    juce::ScopedJuceInitialiser_GUI init;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata_long");
    tmp.deleteRecursively(); tmp.createDirectory();
    const auto backupFolder = tmp.getChildFile ("second drive"); backupFolder.createDirectory();

    constexpr int nIn = 8, nOut = 4, block = 512;
    constexpr double rate = 48000.0;
    Project p;
    p.projectFile = tmp.getChildFile ("Session").getChildFile ("Session.fermata");
    p.projectFile.getParentDirectory().createDirectory();
    p.setInputCount (nIn);
    p.setOutputCount (nOut);
    p.tracks.clear();
    const auto violinId = p.addTrack ("violin", TrackFormat::Mono, 0).id;
    p.addTrack ("viola", TrackFormat::Mono, 1);
    p.addTrack ("cello", TrackFormat::Mono, 2);
    const auto mainId = p.addTrack ("Main", TrackFormat::Stereo, 3).id;
    p.retireAllMixers();
    const auto outBus = p.addBus ("Out", true).id;
    auto& mixA = p.addMixer ("A");
    auto& tw = *p.takeWindows.front();
    tw.name = "Concert";
    for (auto& t : p.tracks) { t.setArmed (true); p.setSendLevel (mixA, t.id, outBus, 0.0f); }
    mixA.busFor (outBus)->outFirst.store (0); mixA.busFor (outBus)->isMain.set (true);
    p.undoReset();

    AudioEngine eng (p);
    eng.setMixerThreads (1);
    eng.setBackupFolder (backupFolder);
    eng.prepare (rate, block);
    eng.rebuildPlan();
    eng.setRunningForTests (true);
    BackgroundSaver saver;                                          // (the program's: saves are written on its thread)

    // ---- the pretend audio thread: time itself
    std::atomic<bool> stop { false }, throttle { false };
    std::atomic<juce::int64> blocks { 0 };
    std::thread audio ([&]
    {
        std::vector<std::vector<float>> in (nIn, std::vector<float> (block)), out (nOut, std::vector<float> (block));
        std::vector<const float*> inP; std::vector<float*> outP;
        for (auto& v : in) inP.push_back (v.data());
        for (auto& v : out) outP.push_back (v.data());
        juce::int64 pos = 0;
        while (! stop.load (std::memory_order_relaxed))
        {
            for (int c = 0; c < nIn; ++c)
                for (int i = 0; i < block; ++i)
                    in[(size_t) c][(size_t) i] = 0.3f * (float) std::sin (2.0 * 3.14159265358979 * (110.0 + 55.0 * c) * (double) (pos + i) / rate);
            eng.process (inP.data(), nIn, outP.data(), nOut, block);
            pos += block;
            const auto b = blocks.fetch_add (1) + 1;
            if (throttle.load (std::memory_order_relaxed)) std::this_thread::sleep_for (std::chrono::microseconds (500));     // ~20 x real time while recording
            else if ((b & 63) == 0) std::this_thread::yield();
        }
    });
    auto simSeconds = [&] { return (double) blocks.load() * block / rate; };

    // ---- what the program does alongside: the windows' polling, the undo tick, the automatic save, the second drive's notices
    double lastSaveAt = 0.0, lastUndoTick = 0.0, lastPoll = 0.0; int saves = 0, saveFailures = 0, pollsDone = 0, undoTicks = 0;
    juce::StringArray secondDriveNotices;
    auto drainSaves = [&] { saver.drain(); };
    auto housekeeping = [&]
    {
        const double t = simSeconds();
        if (t - lastPoll >= 0.05)                                   // the take window's 20 Hz timer: meters, the clock, the live overview
        {
            lastPoll = t; ++pollsDone;
            for (int i = 0; i < nIn; ++i) eng.takeInputPeak (i);
            const juce::int16* d; int bins, bs; eng.getLivePeaks (violinId, d, bins, bs); eng.getLivePeaks (mainId, d, bins, bs);
            (void) eng.recordedSeconds(); (void) eng.playbackSecondsSmooth(); (void) eng.overruns(); (void) eng.samplesLost(); (void) eng.recordingProblem();
            for (auto& f : eng.reapFinishedTakes()) (void) f;      // (AppContext's 100 ms watcher)
            if (((int) (t * 20)) % 20 == 0) { (void) eng.takeLoadStats(); for (auto& n : eng.takeBackupProblems()) secondDriveNotices.add (n); }   // (MainWindow, once a second)
        }
        if (t - lastUndoTick >= 0.5) { lastUndoTick = t; p.undoTick (eng.isRecording()); ++undoTicks; }
        drainSaves();
        // the automatic save: dirty, not recording, the saver idle, 30 s since the last one (AppContext::autosaveIfIdle)
        if (p.dirty.load() && ! eng.isRecording() && ! saver.isBusy() && t - lastSaveAt >= 30.0)
        {
            lastSaveAt = t;
            if (auto job = p.prepareSave (true))
            {
                saver.write (std::move (job), [&] (BackgroundSaver::Result& r) { ++saves; if (! r.ok) { ++saveFailures; p.saveFailed (*r.job); } });
            }
        }
    };

    struct TakeRecord { juce::Uuid id; juce::int64 blocksAtStart, blocksAtStop; };
    std::vector<TakeRecord> takes;
    std::mt19937 rng (11);
    auto between = [&] (double a, double b) { return a + (b - a) * (double) (rng() % 10000) / 10000.0; };
    std::vector<double> heapSamples, rssSamples, heapSteady;
    int overruns = 0; bool anyFailed = false;
    const double total = hours * 3600.0, steadyFrom = total * 0.4;
    std::vector<juce::File> filesToDelete;
    juce::Uuid lastTake;

    // ---- the session
    double t = 0.0;
    int cycle = 0;
    while (t < total)
    {
        ++cycle;
        // idle between takes: 15 - 50 s (talk, notes, a playback of the last take now and then)
        const double idleEnd = t + between (15.0, 50.0);
        bool playedBack = false;
        while (simSeconds() < idleEnd)
        {
            housekeeping();
            if (! playedBack && ! lastTake.isNull() && (rng() % 100) < 40 && simSeconds() > t + 3.0)
            {
                if (auto* g = tw.findGroup (lastTake); g != nullptr && g->lengthSamples > 0 && ! eng.isFinishing (lastTake))
                {
                    std::vector<int> counts; for (auto& tr : p.tracks) counts.push_back (tr.channelCount());
                    auto sess = std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *g), 0, juce::jmin (g->lengthSamples, (juce::int64) (12.0 * rate)), rate, eng.getMaxBlock());
                    if (eng.startPlayback (std::move (sess), false).isEmpty())
                    {
                        const double until = simSeconds() + between (4.0, 10.0);
                        while (simSeconds() < until) housekeeping();
                        eng.stopPlayback (true);                            // (as a press of Stop: freed by the Retirer)
                    }
                }
                playedBack = true;
            }
            std::this_thread::sleep_for (std::chrono::microseconds (100));
        }
        // a take of 40 - 200 s, with a marker or two along the way
        TakeRecord tr;
        throttle = true;
        const auto err = eng.startRecording (tw);
        if (err.isNotEmpty()) { std::printf ("  record failed: %s\n", err.toRawUTF8()); break; }
        tr.blocksAtStart = blocks.load();                           // (the recording takes in every block from here to the Stop call, give or take the two those fall in)
        {   // the save at Record (toggleRecord does one at once)
            if (auto job = p.prepareSave (true)) saver.write (std::move (job), [&] (BackgroundSaver::Result& r) { ++saves; if (! r.ok) { ++saveFailures; p.saveFailed (*r.job); } });
        }
        const double takeEnd = simSeconds() + between (40.0, 200.0);
        double nextMarker = simSeconds() + between (10.0, 60.0);
        while (simSeconds() < takeEnd)
        {
            housekeeping();
            if (simSeconds() >= nextMarker) { addMarker (tw.markers, tw.endSeconds()); p.changed(); nextMarker += between (20.0, 80.0); }
            std::this_thread::sleep_for (std::chrono::microseconds (100));
        }
        tr.blocksAtStop = blocks.load();
        tr.id = eng.stopRecording();
        throttle = false;
        lastTake = tr.id;
        takes.push_back (tr);
        if (auto job = p.prepareSave (true)) saver.write (std::move (job), [&] (BackgroundSaver::Result& r) { ++saves; if (! r.ok) { ++saveFailures; p.saveFailed (*r.job); } });
        // the take before this one is finished by now: its files go (the sandbox has no room for hours of audio); in the steady state the take itself goes too
        if (takes.size() >= 2)
        {
            const auto& prev = takes[takes.size() - 2];
            for (int k = 0; k < 2000 && eng.isFinishing (prev.id); ++k) { housekeeping(); std::this_thread::sleep_for (std::chrono::milliseconds (1)); }
            if (auto* g = tw.findGroup (prev.id))
            {
                overruns += g->overruns; anyFailed = anyFailed || g->writeFailed;
                // timing: the take's length is what the audio thread delivered between Record and Stop (to within the blocks those two fell in),
                // and every one of its files holds exactly that length
                const auto expect = (prev.blocksAtStop - prev.blocksAtStart) * block;
                if (std::abs (g->lengthSamples - expect) > 2 * block) { ++failures; std::printf ("  FAIL: take %d is %lld samples, the audio thread delivered %lld\n", g->number, (long long) g->lengthSamples, (long long) expect); }
                for (auto& f : g->files)
                {
                    const auto r = wavrepair::inspect (f.file, false);
                    if (! r.ok || r.frames != g->lengthSamples) { ++failures; std::printf ("  FAIL: %s holds %lld frames, the take says %lld\n", f.file.getFileName().toRawUTF8(), (long long) r.frames, (long long) g->lengthSamples); }
                    filesToDelete.push_back (f.file);
                }
                while (eng.backupCopiesGoing() > 0 && simSeconds() < takeEnd + 400.0) { housekeeping(); std::this_thread::sleep_for (std::chrono::milliseconds (1)); }
                for (auto& f : filesToDelete) { f.deleteFile(); backupFolder.getChildFile ("Session").getChildFile (f.getRelativePathFrom (p.projectFolder())).deleteFile(); }
                filesToDelete.clear();
                if (simSeconds() > steadyFrom)                        // steady state: the window keeps only its newest take
                {
                    for (size_t i = 0; i < tw.groups.size(); ++i) if (tw.groups[i].id == prev.id) { tw.groups.erase (tw.groups.begin() + (long) i); break; }
                    tw.markers.clear();
                    p.changed();
                }
            }
        }
        for (int k = 0; k < 50; ++k) housekeeping();
        malloc_trim (0);
        const double h = heapMb(), r = rssMb();
        heapSamples.push_back (h); rssSamples.push_back (r);
        if (simSeconds() > steadyFrom) heapSteady.push_back (h);
        t = simSeconds();
        if (cycle % 10 == 0) std::printf ("  %5.1f min: %d takes, heap %.1f MB, rss %.1f MB, undo depth %d, saves %d\n", t / 60.0, (int) takes.size(), h, r, p.undoDepth(), saves);
    }

    // ---- the end of the session
    if (eng.isRecording()) eng.stopRecording();
    eng.waitForFinishedTakes (30000);
    for (int k = 0; k < 400 && eng.sessionsWaitingToBeFreed() > 0; ++k) std::this_thread::sleep_for (std::chrono::milliseconds (5));
    for (int k = 0; k < 2000 && eng.backupCopiesGoing() > 0; ++k) std::this_thread::sleep_for (std::chrono::milliseconds (5));
    saver.waitUntilIdle (20000); drainSaves();
    const bool allFreed = eng.sessionsWaitingToBeFreed() == 0, neverStalled = eng.audioThreadNeverStalled();
    for (auto& f : eng.reapFinishedTakes()) (void) f;
    const double simHours = simSeconds() / 3600.0;
    stop = true;
    audio.join();
    eng.setRunningForTests (false);

    // ---- what grew
    const int n = (int) heapSteady.size();
    double steadyGrowth = 0.0;
    if (n >= 8)
    {
        // the steady state's first quarter against its last quarter (medians, so one odd sample does not decide it)
        auto median = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v[v.size() / 2]; };
        const std::vector<double> head (heapSteady.begin(), heapSteady.begin() + n / 4), tail (heapSteady.end() - n / 4, heapSteady.end());
        steadyGrowth = median (tail) - median (head);
    }
    std::printf ("long session: %.2f simulated hours, %d takes, %d automatic saves (%d failed), %d undo ticks, %d polls, %d overruns%s, second-drive notices %d\n",
                 simHours, (int) takes.size(), saves, saveFailures, undoTicks, pollsDone, overruns, anyFailed ? ", A FILE FAILED" : "", secondDriveNotices.size());
    for (auto& s : secondDriveNotices) std::printf ("  second drive: %s\n", s.toRawUTF8());
    std::printf ("  heap in use: start %.1f MB, end of the growing half %.1f MB, end %.1f MB; rss %.1f -> %.1f MB; steady state grew %.2f MB (first to last quarter)\n",
                 heapSamples.empty() ? 0.0 : heapSamples.front(), heapSteady.empty() || heapSamples.size() <= heapSteady.size() ? 0.0 : heapSamples[heapSamples.size() - heapSteady.size() - 1],
                 heapSamples.empty() ? 0.0 : heapSamples.back(), rssSamples.empty() ? 0.0 : rssSamples.front(), rssSamples.empty() ? 0.0 : rssSamples.back(), steadyGrowth);
    std::printf ("  lists: finishing %s, plays waiting to be freed %d, second-drive copies going %d, undo depth %d / redo %d, window files %d, markers %d\n",
                 eng.anyFinishing() ? "YES" : "none", eng.sessionsWaitingToBeFreed(), eng.backupCopiesGoing(), p.undoDepth(), p.redoDepth(), (int) p.windowFiles.size(), (int) tw.markers.size());
    CHECK (takes.size() >= 4);
    CHECK (saves > 0 && saveFailures == 0);
    CHECK (! anyFailed);
    CHECK (allFreed && neverStalled);
    CHECK (! eng.anyFinishing() && eng.backupCopiesGoing() == 0);
    CHECK (p.undoDepth() <= Project::kUndoLevels && p.windowFiles.size() <= 2);          // (this window and the processing mixer: nothing piles up in the files list)
    CHECK (n < 8 || steadyGrowth < 4.0);                            // a leak of even 16 bytes a block would be 5 MB an hour; the steady state must not grow
    std::printf (failures == 0 ? "LONG SESSION TEST PASSED\n" : "LONG SESSION TEST FAILED (%d)\n", failures);
    tmp.deleteRecursively();
    return failures == 0 ? 0 : 1;
}

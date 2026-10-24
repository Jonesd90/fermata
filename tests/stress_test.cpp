// Concurrency stress test for the Fermata engine: a pretend audio thread runs process() flat out while the "message thread" does everything
// a user does during a session (rebuilds the plan, records, plays, loads and removes plug-ins, turns sends on and off, switches session mode,
// auditions, moves faders). Run plain it checks nothing crashes and no audio is lost; built with ThreadSanitizer / AddressSanitizer
// (tools/sandbox-check.sh does both) any report is a finding. Usage: stress_test [seconds]
#include "../src/core/AudioEngine.h"
#include <cstdio>
#include <thread>
#include <random>

using namespace td;

struct HalfGain : InsertProcessor
{
    void prepare (double, int, int) override {}
    void process (juce::AudioBuffer<float>& b) override { b.applyGain (0.5f); }
    juce::String getName() const override { return "HalfGain"; }
    int numParams() const override { return 1; }
    bool paramAutomatable (int) const override { return true; }
    void setParamNorm (int, float v) override { p = v; }
    float p = 0.0f;
};

struct LatencyInsert : InsertProcessor
{
    explicit LatencyInsert (int n) : lat (n) {}
    void prepare (double, int, int) override { line.assign ((size_t) (2 * (lat + 1)), 0.0f); pos = 0; }
    void process (juce::AudioBuffer<float>& b) override
    {
        for (int c = 0; c < b.getNumChannels() && c < 2; ++c)
        {
            auto* x = b.getWritePointer (c); auto* d = line.data() + (size_t) c * (size_t) (lat + 1);
            int q = pos;
            for (int i = 0; i < b.getNumSamples(); ++i) { const float o = lat == 0 ? x[i] : d[q]; if (lat > 0) { d[q] = x[i]; if (++q == lat) q = 0; } x[i] = o; }
        }
        pos = lat > 0 ? (pos + b.getNumSamples()) % lat : 0;
    }
    int getLatencySamples() const override { return lat; }
    juce::String getName() const override { return "LatencyInsert"; }
    int lat, pos = 0; std::vector<float> line;
};

int main (int argc, char** argv)
{
    const double seconds = argc > 1 ? atof (argv[1]) : 4.0;
    juce::ScopedJuceInitialiser_GUI init;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata_stress");
    tmp.deleteRecursively(); tmp.createDirectory();

    Project p;
    p.projectFile = tmp.getChildFile ("Stress.fermata");
    p.setInputCount (8);
    p.setOutputCount (6);
    p.tracks.clear();
    auto violinId = p.addTrack ("violin", TrackFormat::Mono, 0).id;
    auto violaId  = p.addTrack ("viola", TrackFormat::Mono, 1).id;
    auto mainId   = p.addTrack ("Main", TrackFormat::Stereo, 2).id;
    p.retireAllMixers();
    const auto revId = p.addBus ("Reverb").id;
    const auto outBus = p.addBus ("Out", true).id;
    const auto cueBus = p.addBus ("Cue", true).id;
    auto& mixA = p.addMixer ("A");
    auto& mixB = p.addMixer ("B");
    auto& tw = *p.takeWindows.front();
    tw.name = "Stress";
    for (auto& t : p.tracks) t.setArmed (true);

    AudioEngine eng (p);
    eng.setMixerThreads (2);
    eng.prepare (48000.0, 512);
    for (auto& t : p.tracks) { p.setSendLevel (mixA, t.id, outBus, 0.0f); p.setSendLevel (mixB, t.id, outBus, 0.0f); }
    p.setSendLevel (mixA, revId, outBus, 0.0f); p.setSendLevel (mixB, revId, outBus, 0.0f);
    mixA.busFor (outBus)->outFirst.store (0); mixB.busFor (outBus)->outFirst.store (2); mixB.busFor (cueBus)->outFirst.store (4);
    mixA.busFor (outBus)->isMain.set (true); mixB.busFor (outBus)->isMain.set (true);
    eng.rebuildPlan();
    eng.setRunningForTests (true);

    // ---- the pretend audio thread
    std::atomic<bool> stop { false };
    std::atomic<long> blocks { 0 };
    std::thread audio ([&]
    {
        constexpr int nIn = 8, nOut = 6, block = 512;
        std::vector<std::vector<float>> in (nIn, std::vector<float> (block)), out (nOut, std::vector<float> (block));
        std::vector<const float*> inP; std::vector<float*> outP;
        for (auto& v : in) inP.push_back (v.data());
        for (auto& v : out) outP.push_back (v.data());
        juce::int64 pos = 0;
        while (! stop.load())
        {
            for (int c = 0; c < nIn; ++c)
                for (int i = 0; i < block; ++i)
                    in[(size_t) c][(size_t) i] = 0.5f * (float) std::sin (2.0 * 3.14159265358979 * (200.0 + 100.0 * c) * (double) (pos + i) / 48000.0);
            eng.process (inP.data(), nIn, outP.data(), nOut, block);
            pos += block;
            blocks.fetch_add (1);
            std::this_thread::sleep_for (std::chrono::microseconds (200));     // far faster than real time, but it lets the other thread in
        }
    });

    // ---- the pretend message thread
    std::mt19937 rng (7);
    auto chance = [&] (int percent) { return (int) (rng() % 100) < percent; };
    const auto t0 = juce::Time::getMillisecondCounter();
    int recordings = 0, plays = 0, rebuilds = 0, slotChanges = 0, sends = 0;
    juce::Uuid lastTake;
    while (juce::Time::getMillisecondCounter() - t0 < (juce::uint32) (seconds * 1000.0))
    {
        const int action = (int) (rng() % 12);
        switch (action)
        {
            case 0: eng.rebuildPlan(); eng.servicePreRoll(); ++rebuilds; break;
            case 1:
                if (! eng.isRecording() && ! eng.isPlaying()) { if (eng.startRecording (tw).isEmpty()) ++recordings; }
                else if (eng.isRecording()) { lastTake = eng.stopRecording(); eng.reapFinishedTakes(); }
                break;
            case 2:
                if (! eng.isRecording() && ! eng.isPlaying() && ! lastTake.isNull())
                {
                    if (auto* g = tw.findGroup (lastTake); g != nullptr && g->lengthSamples > 0)
                    {
                        std::vector<int> counts; for (auto& t : p.tracks) counts.push_back (t.channelCount());
                        auto sess = std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *g), 0, g->lengthSamples, 48000.0, eng.getMaxBlock());
                        sess->setLooping (chance (50));
                        if (eng.startPlayback (std::move (sess), chance (50)).isEmpty()) ++plays;     // (half of them do not wait for the first moments: 2.102)
                    }
                }
                else if (eng.isPlaying()) eng.stopPlayback (chance (50));       // (half of them as a press of Stop: freed by a thread of its own, 2.102)
                break;
            case 3:
            {   // plug-ins come and go
                auto& m = chance (50) ? mixA : mixB;
                InsertSlot* slot = chance (50) ? &m.stripFor (violinId)->slots[rng() % kNumSlots] : &m.busFor (chance (50) ? outBus : revId)->slots[rng() % kNumSlots];
                if (chance (30)) slot->clear (48000.0, 512, 2);
                else if (chance (50)) slot->set (std::make_unique<HalfGain>(), 48000.0, 512, 2);
                else slot->set (std::make_unique<LatencyInsert> ((int) (rng() % 300)), 48000.0, 512, 2);
                ++slotChanges;
                eng.checkLatency();
                break;
            }
            case 4:
            {   // sends on and off (routing changes)
                auto& m = chance (50) ? mixA : mixB;
                const auto src = chance (50) ? violinId : violaId;
                if (p.setSendLevel (m, src, revId, chance (50) ? -12.0f : -100.0f) == SendResult::RoutingChanged) eng.rebuildPlan();
                ++sends;
                break;
            }
            case 5: eng.setSessionMode (chance (50)); eng.servicePreRoll(); break;
            case 6: (void) eng.mixerLatency (&mixA); (void) eng.offlineLatency(); (void) eng.recordedSeconds(); (void) eng.playbackSecondsSmooth(); break;
            case 7: eng.setAuditionMixer (chance (50) ? &mixB : nullptr); break;
            case 8: eng.setScopeSource (chance (50) ? (const void*) mixA.stripFor (mainId) : nullptr); { float l[4096], r[4096]; eng.readScope (l, r, 4096); } break;
            case 9: eng.setLtcOutput (5, chance (70), 1, 3600.0, chance (20)); eng.setLtcInput (chance (50) ? 7 : -1); break;
            case 10:
            {   // faders, mutes, solos, follow
                auto& m = chance (50) ? mixA : mixB;
                m.stripFor (violinId)->gainDb.set ((float) (rng() % 30) - 20.0f);
                m.stripFor (violaId)->mute.set (chance (30)); m.stripFor (mainId)->solo.set (chance (20));
                mixB.followProcessing.set (chance (30));
                for (int i = 0; i < 8; ++i) eng.takeInputPeak (i);
                const juce::int16* d; int bins, bs; eng.getLivePeaks (violinId, d, bins, bs);
                break;
            }
            case 11: eng.setTalkbackRouting (std::vector<int> { 6 }, std::vector<int> { 4 }); eng.setCrMic (chance (50)); eng.setTalkbackPlayback (chance (50)); break;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (rng() % 3000));
    }
    if (eng.isRecording()) eng.stopRecording();
    eng.waitForFinishedTakes (20000); eng.reapFinishedTakes();
    eng.stopPlayback();
    for (int k = 0; k < 400 && eng.sessionsWaitingToBeFreed() > 0; ++k) std::this_thread::sleep_for (std::chrono::milliseconds (5));     // (the audio thread still runs: they are freed)
    const bool allFreed = eng.sessionsWaitingToBeFreed() == 0, neverStalled = eng.audioThreadNeverStalled();
    stop = true;
    audio.join();
    eng.setRunningForTests (false);
    int overruns = 0; bool failed = false;
    for (auto& g : tw.groups) { overruns += g.overruns; failed = failed || g.writeFailed; }
    std::printf ("stress: %ld blocks, %d recordings, %d plays, %d rebuilds, %d slot changes, %d send changes, %d overruns%s\n",
                 blocks.load(), recordings, plays, rebuilds, slotChanges, sends, overruns, failed ? ", A FILE FAILED" : "");
    if (! allFreed || ! neverStalled) std::printf ("stress: stopped plays still waiting to be freed: %s, audio thread stalled: %s\n", allFreed ? "no" : "YES", neverStalled ? "no" : "YES");
    const bool ok = blocks.load() > 0 && recordings > 0 && plays > 0 && ! failed && allFreed && neverStalled;
    std::printf (ok ? "STRESS TEST PASSED\n" : "STRESS TEST FAILED\n");
    tmp.deleteRecursively();
    return ok ? 0 : 1;
}

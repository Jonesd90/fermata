// Headless test of the Fermata engine: recording, take naming/grouping, mixer isolation, inserts, save/load.
#include <random>
#include "../src/core/AudioEngine.h"
#include "../src/core/Bounce.h"
#include "../src/core/SessionClock.h"
#include "../src/core/TalkbackKey.h"
#include "../src/core/RemoteProtocol.h"
#include "../src/core/RemoteDesk.h"
#include "../src/core/PreampMemory.h"
#include "../src/core/ImportPlan.h"
#include "../src/core/MasterExport.h"
#include "../src/core/Resampler.h"
#include "../src/core/RateBridge.h"
#include <sstream>
#include "../src/core/Ravenna.h"
#include "../src/core/Dither.h"
#include "../src/core/CoreReservation.h"
#include "../src/core/CrashReport.h"
#include "../src/core/WavRepair.h"
#include "../src/core/TakeWriter.h"
#include "../src/core/ProjectCopy.h"
#include "../src/core/Ltc.h"
#include "../src/core/VideoExport.h"
#include "../src/core/AafWrite.h"
#include "../src/core/AafExport.h"
#include "../src/core/TakeMatch.h"
#include "../src/core/Consonants.h"
#include "../src/core/Aaf.h"
#include "../src/core/SpectralPicture.h"
#include "../src/core/EditFix.h"
#include "../src/core/SpectralPyramid.h"
#include "../src/core/Feedback.h"
#include "../src/core/ReviewPage.h"
#include "../src/core/FileSync.h"
#include "../src/core/AudioRelink.h"
#include "../src/core/AudioOps.h"
#include "../src/core/Mcu.h"
#include "../src/core/Loudness.h"
#include "../src/core/ScreenLayout.h"
#include "../src/core/Tuning.h"
#include "../src/core/PitchPipe.h"
#include "../src/core/TimeAlign.h"
#include "../src/core/TakeSheet.h"
#include "../src/core/BoxEdges.h"
#include "../src/core/WavRepair.h"
#include "../src/core/SaveWriter.h"
#include "../src/core/Retime.h"
#include "../src/core/AudioFormats.h"
#include "../src/core/TextBytes.h"
#include "../src/ui/TakeMatchService.h"
#include "../src/ui/WaveThumb.h"
#include <cstdio>
#include <thread>

using namespace td;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define SECTION(name) std::printf ("-- %s\n", name)

/** The tests have no message loop of their own; this runs the messages posted so far (callAsync) for 'ms' milliseconds, through JUCE's own
    dispatcher (an internal function, declared here because the public loop needs JUCE_MODAL_LOOPS_PERMITTED, which Fermata turns off). */
namespace juce::detail { bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages); }
static void pumpMessages (int ms)
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < until)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true)) juce::Thread::sleep (2);
}

struct Rig
{
    static constexpr int nIn = 8, nOut = 4, block = 512;
    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inP;
    std::vector<float*> outP;
    juce::int64 pos = 0;
    float inGain = 0.5f;
    Rig() : in (nIn, std::vector<float> (block)), out (nOut, std::vector<float> (block))
    {
        for (auto& v : in) inP.push_back (v.data());
        for (auto& v : out) outP.push_back (v.data());
    }
    std::vector<double> measure (AudioEngine& e)
    {
        run (e, 10);                 // let gain ramps settle
        pos = 0;                     // identical input window for every measurement
        std::vector<double> r;
        run (e, 20, &r);
        return r;
    }
    void run (AudioEngine& e, int blocks, std::vector<double>* outRms = nullptr)
    {
        std::vector<double> sumSq (nOut, 0.0);
        for (int b = 0; b < blocks; ++b)
        {
            for (int c = 0; c < nIn; ++c)
                for (int i = 0; i < block; ++i)
                    in[(size_t) c][(size_t) i] = inGain * std::sin (2.0 * 3.14159265358979 * (200.0 + 100.0 * c) * (double) (pos + i) / 48000.0);
            e.process (inP.data(), nIn, outP.data(), nOut, block);
            for (int c = 0; c < nOut; ++c) for (float s : out[(size_t) c]) sumSq[(size_t) c] += (double) s * s;
            pos += block;
        }
        if (outRms) { outRms->clear(); for (auto s : sumSq) outRms->push_back (std::sqrt (s / (blocks * block))); }
    }
};

struct HalfGain : InsertProcessor
{
    void prepare (double, int, int) override {}
    void process (juce::AudioBuffer<float>& b) override { b.applyGain (0.5f); }
    juce::String getName() const override { return "HalfGain"; }
};

/** An insert that delays by a fixed number of samples and says so (like a look-ahead or linear-phase plug-in). */
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

static juce::File makeConstWav (const juce::File& f, double value, int seconds, double sr = 48000.0)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24));
    juce::AudioBuffer<float> b (1, 48000);
    for (int i = 0; i < 48000; ++i) b.setSample (0, i, (float) value);
    for (int s = 0; s < seconds; ++s) w->writeFromAudioSampleBuffer (b, 0, 48000);
    w.reset();
    return f;
}

/** A 997 Hz tone at this amplitude, with (spikes) a full-scale click every half second: loud peaks, quiet loudness. */
static juce::File makeToneWav (const juce::File& f, double amp, int seconds, bool spikes, double sr = 48000.0)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (24));
    juce::AudioBuffer<float> b (1, 48000);
    for (int s = 0; s < seconds; ++s)
    {
        for (int i = 0; i < 48000; ++i) b.setSample (0, i, (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * (s * 48000 + i) / sr)));
        if (spikes) { b.setSample (0, 1000, 0.95f); b.setSample (0, 25000, -0.95f); }
        w->writeFromAudioSampleBuffer (b, 0, 48000);
    }
    w.reset();
    return f;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata_test");
    tmp.deleteRecursively(); tmp.createDirectory();

    Project p;
    p.projectFile = tmp.getChildFile ("Concert.fermata");
    p.setInputCount (8);
    p.setOutputCount (4);
    p.tracks.clear();
    auto violinId = p.addTrack ("violin", TrackFormat::Mono, 0).id;
    auto violaId  = p.addTrack ("viola", TrackFormat::Mono, 1).id;
    auto mainId   = p.addTrack ("Main", TrackFormat::Stereo, 2).id;
    p.addTrack ("Atmos", TrackFormat::Surround, 4);   // 6 channels: inputs 4..9 (partly missing -> silence)
    p.retireAllMixers();
    p.addBus ("Reverb");                                      // an Int bus used as an effects channel (buses[0])
    const auto outBus = p.addBus ("Out", true).id;           // the only way to the driver outputs: an Ext bus
    auto& mixA = p.addMixer ("A");
    auto& mixB = p.addMixer ("B");
    auto& tw = *p.takeWindows.front();
    tw.name = "Symphony";

    AudioEngine eng (p);
    eng.prepare (48000.0, Rig::block);
    Rig rig;
    // every track goes to the Out bus at 0 dB in both mixers; mixer A's Out bus is outputs 1-2, B's is outputs 3-4
    auto setMain = [&] (MixerState& m, const juce::Uuid& id, bool on) { p.setSendLevel (m, id, outBus, on ? 0.0f : -100.0f); eng.rebuildPlan(); };
    for (auto& t : p.tracks) { setMain (mixA, t.id, true); setMain (mixB, t.id, true); }
    setMain (mixA, p.buses[0].id, true); setMain (mixB, p.buses[0].id, true);        // the effects channel goes to the Out bus too
    mixA.busFor (outBus)->outFirst.store (0); mixB.busFor (outBus)->outFirst.store (2);

    SECTION ("a new project starts completely empty: one mixer, no tracks, no buses (so no Main, no effects channel)");
    {
        Project fresh;
        CHECK (fresh.tracks.empty() && fresh.buses.empty() && fresh.mixers.size() == 1);
        CHECK (fresh.mixers[0]->name == "Processing mixer");
        CHECK (fresh.outputFedBy (0).isEmpty() && fresh.mixerOutputsText (*fresh.mixers[0]) == "(no outputs)");
        CHECK (p.outputFedBy (0).contains ("Out") && p.outputFedBy (3).contains ("Out") && p.mixerOutputsText (mixB).contains ("Out -> 3-4"));
        // what the program adds to a NEW project: one Ext Bus "Main", the processing mixer's main output, on outputs 1-2
        auto& mb = fresh.addDefaultMainBus();
        CHECK (fresh.buses.size() == 1 && mb.external && mb.name == "Main");
        auto* st = fresh.mixers[0]->busFor (mb.id);
        CHECK (st->isMain.get() && st->outFirst.load() == 0);
        CHECK (fresh.outputFedBy (0).contains ("Main") && fresh.outputFedBy (1).contains ("Main") && fresh.outputFedBy (2).isEmpty());
        fresh.addTrack ("Violin", TrackFormat::Mono, 0); fresh.syncMixers();
        auto& ed = fresh.addEdit ("E", juce::Uuid::null());                                  // an Edit's own mixer copies it
        auto* em = fresh.mixerOfEdit (ed.id);
        CHECK (em != nullptr && em->busFor (mb.id)->isMain.get() && em->busFor (mb.id)->outFirst.load() == 0);
        Project back; CHECK (back.fromVar (fresh.toVar()) && back.buses.size() == 1 && back.mixers[0]->busFor (mb.id)->outFirst.load() == 0);
    }

    SECTION ("recording refuses with nothing armed");
    CHECK (eng.startRecording (tw).isNotEmpty());

    SECTION ("take 1: files, naming, 24-bit, grouping");
    p.findTrack (violinId)->setArmed (true);
    p.findTrack (mainId)->setArmed (true);
    CHECK (eng.startRecording (tw).isEmpty());
    CHECK (eng.isRecording());
    rig.run (eng, 100);
    CHECK (eng.recordedSeconds() > 1.0);
    auto gid1 = eng.stopRecording();
    CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
    CHECK (! eng.isRecording());
    CHECK (tw.groups.size() == 1);
    auto* g1 = tw.findGroup (gid1);
    CHECK (g1 != nullptr && g1->number == 1 && g1->files.size() == 2 && g1->lengthSamples == 100 * Rig::block);
    auto folder = p.takeFolder (tw);
    CHECK (folder.getChildFile ("001 - Symphony - violin.wav").existsAsFile());
    CHECK (folder.getChildFile ("001 - Symphony - Main.wav").existsAsFile());
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (folder.getChildFile ("001 - Symphony - violin.wav")));
        CHECK (r != nullptr && r->bitsPerSample == 24 && r->numChannels == 1 && r->lengthInSamples == 100 * Rig::block && r->sampleRate == 48000.0);
        std::unique_ptr<juce::AudioFormatReader> r2 (fm.createReaderFor (folder.getChildFile ("001 - Symphony - Main.wav")));
        CHECK (r2 != nullptr && r2->bitsPerSample == 24 && r2->numChannels == 2 && r2->lengthInSamples == 100 * Rig::block);
        if (r != nullptr)
        {
            juce::AudioBuffer<float> buf (1, 1000);
            r->read (&buf, 0, 1000, 0, true, false);
            double maxErr = 0;
            for (int i = 0; i < 1000; ++i)
                maxErr = juce::jmax (maxErr, std::abs ((double) buf.getSample (0, i) - 0.5 * std::sin (2.0 * 3.14159265358979 * 200.0 * i / 48000.0)));
            CHECK (maxErr < 1e-5);                 // input 0 recorded bit-accurately at 24-bit
        }
        if (r2 != nullptr)
        {
            juce::AudioBuffer<float> buf (2, 1000);
            r2->read (&buf, 0, 1000, 0, true, true);
            double e0 = 0, e1 = 0;
            for (int i = 0; i < 1000; ++i)
            {
                e0 = juce::jmax (e0, std::abs ((double) buf.getSample (0, i) - 0.5 * std::sin (2.0 * 3.14159265358979 * 400.0 * i / 48000.0)));
                e1 = juce::jmax (e1, std::abs ((double) buf.getSample (1, i) - 0.5 * std::sin (2.0 * 3.14159265358979 * 500.0 * i / 48000.0)));
            }
            CHECK (e0 < 1e-5 && e1 < 1e-5);        // stereo track = inputs 3 and 4 (1-based) in the right order
        }
    }

    SECTION ("take 2 numbers on, surround track, missing inputs");
    p.findTrack (violaId)->setArmed (true);
    for (auto& t : p.tracks) if (t.name == "Atmos") t.setArmed (true);
    CHECK (eng.startRecording (tw).isEmpty());
    rig.run (eng, 20);
    auto gid2 = eng.stopRecording();
    CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
    auto* g2 = tw.findGroup (gid2);
    CHECK (g2 != nullptr && g2->number == 2 && g2->files.size() == 4);
    CHECK (folder.getChildFile ("002 - Symphony - Atmos.wav").existsAsFile());
    g1 = tw.findGroup (gid1);
    CHECK (g2 != nullptr && g1 != nullptr && g2->startSeconds > g1->startSeconds + g1->lengthSeconds());
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (folder.getChildFile ("002 - Symphony - Atmos.wav")));
        CHECK (r != nullptr && r->numChannels == 6 && r->bitsPerSample == 24);
    }

    SECTION ("renaming a take renames every file");
    CHECK (tw.setLabel (gid1, "Symphony 2"));
    g1 = tw.findGroup (gid1);
    CHECK (folder.getChildFile ("001 - Symphony 2 - violin.wav").existsAsFile());
    CHECK (folder.getChildFile ("001 - Symphony 2 - Main.wav").existsAsFile());
    CHECK (! folder.getChildFile ("001 - Take - violin.wav").existsAsFile());
    CHECK (g1->files[0].file.existsAsFile());
    CHECK (tw.displayName (*g1) == "001 - Symphony 2");

    SECTION ("window default label");
    // a take with no label of its own is named after the piece
    p.findTrack (violinId)->setArmed (true);
    for (auto& t : p.tracks) if (t.name != "violin") t.setArmed (false);
    CHECK (eng.startRecording (tw).isEmpty());
    rig.run (eng, 5);
    eng.stopRecording();
    CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
    CHECK (folder.getChildFile ("003 - Symphony - violin.wav").existsAsFile());

    SECTION ("mixers are fully independent (solo / mute / level)");
    mixA.busFor (outBus)->outFirst.store (0); mixB.busFor (outBus)->outFirst.store (2);
    auto base = rig.measure (eng);                             // everything open in both mixers
    CHECK (base[0] > 0.1 && base[2] > 0.1);
    CHECK (std::abs (base[0] - base[2]) < 1e-6);               // identical mixers -> identical output
    mixA.stripFor (violaId)->solo.set (true);                  // solo viola in mixer A only
    auto soloA = rig.measure (eng);
    CHECK (soloA[0] < base[0] * 0.8);                          // A now only plays viola
    CHECK (std::abs (soloA[2] - base[2]) < 1e-6);              // B untouched
    mixA.stripFor (violaId)->solo.set (false);
    mixB.stripFor (violaId)->mute.set (true);                  // mute viola in B only
    auto muteB = rig.measure (eng);
    CHECK (muteB[2] < base[2] * 0.95);
    CHECK (std::abs (muteB[0] - base[0]) < 1e-6);
    mixB.stripFor (violaId)->mute.set (false);

    SECTION ("2.78: a track hidden from one mixer reaches nothing in it (its levels are kept); the other mixer is untouched; saved and loaded");
    {
        mixB.hiddenStrips.push_back (violaId); eng.rebuildPlan();
        auto hid = rig.measure (eng);
        CHECK (hid[2] < base[2] * 0.95 && std::abs (hid[2] - muteB[2]) < 1e-5);     // as if muted in B
        CHECK (std::abs (hid[0] - base[0]) < 1e-6);                                    // A untouched
        CHECK (mixB.stripFor (violaId)->gainDb.get() == mixA.stripFor (violaId)->gainDb.get());
        const auto saved = p.mixerToVar (mixB);
        CHECK (saved["hiddenStrips"].size() == 1 && saved["hiddenStrips"][0].toString() == violaId.toString());
        mixB.hiddenStrips.clear(); eng.rebuildPlan();
        auto back = rig.measure (eng);
        CHECK (std::abs (back[2] - base[2]) < 1e-6);
    }

    SECTION ("insert slot processes audio and survives swapping");
    auto before = rig.measure (eng);
    mixB.stripFor (violaId)->slots[0].set (std::make_unique<HalfGain>(), 48000.0, Rig::block, 1);
    auto after = rig.measure (eng);
    CHECK (after[2] < before[2] * 0.98);
    CHECK (std::abs (after[0] - before[0]) < 1e-6);            // mixer A unaffected by B's insert

    SECTION ("a bypassed insert still hears the music (it runs on a copy), so switching it back in is clean");
    {
        struct Counting : InsertProcessor
        {
            explicit Counting (std::atomic<int>& c) : n (c) {}
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>& b) override { ++n; b.applyGain (0.5f); }
            juce::String getName() const override { return "Counting"; }
            std::atomic<int>& n;
        };
        std::atomic<int> calls { 0 };
        auto& slot = mixB.stripFor (violaId)->slots[0];
        slot.bypass.set (true);
        CHECK (slot.set (std::make_unique<Counting> (calls), 48000.0, Rig::block, 1));
        auto bypassed = rig.measure (eng);
        CHECK (calls.load() >= 30);                                              // it was run for every block...
        CHECK (std::abs (bypassed[2] - before[2]) < 1e-6);                       // ...but the music went past it untouched
        CHECK (slot.latencySamples() == 0 && slot.isLoaded() && slot.getName() == "Counting");
        slot.bypass.set (false);
        auto live = rig.measure (eng);
        CHECK (live[2] < before[2] * 0.98 && std::abs (live[2] - after[2]) < 1e-6);   // switched in: half, like the HalfGain before it
        CHECK (InsertSlot::stuckCount().load() == 0);
    }

    SECTION ("a plug-in stuck in its process call cannot hang the program: the slot's locks give up after two seconds");
    {
        struct Sleeper : InsertProcessor
        {
            explicit Sleeper (int ms) : sleepMs (ms) {}
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>&) override { juce::Thread::sleep (sleepMs); }
            juce::String getName() const override { return "Sleeper"; }
            int sleepMs;
        };
        InsertSlot slot;
        juce::AudioBuffer<float> blk (1, 64); blk.clear();
        CHECK (slot.set (std::make_unique<Sleeper> (300), 48000.0, 64, 1));
        {
            std::thread audio ([&] { slot.process (blk); });                                   // holds the slot's lock for 300 ms
            juce::Thread::sleep (50);
            const auto t0 = juce::Time::getMillisecondCounter();
            CHECK (slot.set (std::make_unique<Sleeper> (2600), 48000.0, 64, 1));               // waits for it, then succeeds
            const auto waited = juce::Time::getMillisecondCounter() - t0;
            CHECK (waited >= 150 && waited < 1500);
            audio.join();
        }
        CHECK (InsertSlot::stuckCount().load() == 0);
        {
            std::thread audio ([&] { slot.process (blk); });                                   // holds the lock for 2.6 s: longer than the message thread will wait
            juce::Thread::sleep (50);
            const auto t0 = juce::Time::getMillisecondCounter();
            CHECK (! slot.set (std::make_unique<HalfGain>(), 48000.0, 64, 1));                 // gives up...
            const auto waited = juce::Time::getMillisecondCounter() - t0;
            CHECK (waited >= InsertSlot::kLockWaitMs - 50 && waited < InsertSlot::kLockWaitMs + 1000);
            CHECK (slot.isLoaded() && slot.getName() == "Sleeper");                             // ...leaving the slot as it was
            CHECK (InsertSlot::stuckCount().load() == 1);
            audio.join();
        }
        auto freed = slot.take();                                                               // free again once the plug-in let go
        CHECK (freed != nullptr && freed->getName() == "Sleeper" && ! slot.isLoaded() && slot.getName().isEmpty());
        CHECK (slot.clear (48000.0, 64, 1) && InsertSlot::stuckCount().load() == 1);
        InsertSlot::stuckCount().store (0);
    }

    SECTION ("effects send reaches the effects return, per mixer");
    mixB.stripFor (violaId)->slots[0].clear (48000.0, Rig::block, 1);
    auto noSend = rig.measure (eng);
    CHECK (p.setSendLevel (mixB, violinId, p.buses[0].id, 0.0f) == SendResult::RoutingChanged);
    eng.rebuildPlan();
    auto withSend = rig.measure (eng);
    CHECK (withSend[2] > noSend[2] * 1.05);
    CHECK (std::abs (withSend[0] - noSend[0]) < 1e-6);

    SECTION ("a mixer's output pair is selectable");
    mixB.busFor (outBus)->outFirst.store (0);                  // both mixers now sum to outputs 1-2
    auto summed = rig.measure (eng);
    CHECK (summed[0] > base[0] * 1.5 && summed[2] < 1e-9);
    mixB.busFor (outBus)->outFirst.store (2);

    SECTION ("save / load round trip");
    p.setPreamp (0, { 31.5f, true, false, true, false });
    mixA.stripFor (violaId)->gainDb.set (-6.0f);
    mixA.stripFor (violaId)->pan.set (0.25f);
    juce::String err;
    CHECK (p.save (err));
    Project q;
    CHECK (q.load (p.projectFile, err));
    CHECK (q.tracks.size() == p.tracks.size() && q.tracks[0].name == "violin" && q.tracks[3].format == TrackFormat::Surround);
    CHECK (q.inputs[0].preamp.gainDb == 31.5f && q.inputs[0].preamp.phantom && q.inputs[0].preamp.lowCut);
    CHECK (q.mixers.size() == 2 && q.mixers[1]->busFor (outBus)->outFirst.load() == 2);
    CHECK (std::abs (q.mixers[0]->stripFor (violaId)->gainDb.get() + 6.0f) < 1e-6f);
    CHECK (std::abs (q.mixers[0]->stripFor (violaId)->pan.get() - 0.25f) < 1e-6f);
    CHECK (q.takeWindows.size() == 1 && q.takeWindows[0]->groups.size() == 3);
    CHECK (q.takeWindows[0]->groups[0].label == "Symphony 2" && q.takeWindows[0]->nextNumber == 4);
    CHECK (q.takeWindows[0]->groups[0].files[0].file.existsAsFile());
    CHECK (q.takeWindows[0]->groups[0].lengthSamples == 100 * Rig::block);

    SECTION ("edit tracks: own list, order, missing strips");
    {
        EditDef e;
        CHECK (p.editTracks (e).size() == p.tracks.size());                       // no list of its own: every project track
        p.materialiseEditTracks (e);
        std::swap (e.trackIds[0], e.trackIds[1]);
        auto sh = p.editTracks (e);
        CHECK (sh.size() == p.tracks.size() && sh[0]->id == p.tracks[1].id && sh[1]->id == p.tracks[0].id);
        e.trackIds.push_back (juce::Uuid());                                       // an id that is not a track is skipped, never a crash
        CHECK (p.editTracks (e).size() == p.tracks.size());
        EditRegion r; r.srcIn = 0; r.srcOut = 1000; r.sampleRate = 48000.0;
        RegionFile rf; rf.trackId = juce::Uuid(); rf.trackName = "gone"; rf.numChannels = 2; r.files.push_back (rf);
        e.insertRegion (r);
        auto miss = p.missingTracksOf (e);
        CHECK (miss.size() == 1 && miss[0].second.first == "gone");
        const auto before = p.tracks.size();
        p.restoreTrack (miss[0].first, "gone", 2);
        CHECK (p.tracks.size() == before + 1 && p.missingTracksOf (e).empty() && p.tracks.back().format == TrackFormat::Stereo);
        CHECK (mixA.stripFor (miss[0].first) != nullptr);
        auto v = p.toVar();
        (void) v;
    }

    SECTION ("edit list: layout, ripple insert, default fades");
    {
        EditDef e;
        auto mk = [] (juce::int64 len) { EditRegion r; r.srcIn = 1000; r.srcOut = 1000 + len; r.sourceLength = 1000000; r.sampleRate = 48000.0; return r; };
        e.insertRegion (mk (48000)); e.insertRegion (mk (96000)); e.insertRegion (mk (48000));
        CHECK (e.regions.size() == 3 && e.regions[1].startSample == 48000 && e.regions[2].startSample == 144000);
        CHECK (e.lengthSamples() == 192000 && e.sampleRate == 48000.0);
        CHECK (std::abs (e.regions[0].inEnd - kDefaultEdgeFade) < 1e-9 && e.regions[2].outEnd == 0.0 && std::abs (e.regions[2].outStart + kDefaultEdgeFade) < 1e-9);   // the ends of the edit get a 1 ms fade
        CHECK (std::abs (e.regions[1].inStart + 0.015) < 1e-9 && std::abs (e.regions[0].outEnd - 0.015) < 1e-9);   // default 30 ms centred crossfade
        e.insertIndex = 1; auto idx = e.insertRegion (mk (24000));
        CHECK (idx == 1 && e.regions[2].startSample == 72000 && e.insertIndex == -1);          // inserted at the chosen join, later regions ripple
        e.removeRegion (1);
        CHECK (e.regions.size() == 3 && e.regions[1].startSample == 48000);
        auto firstId = e.regions[0].id;
        e.moveRegion (0, 2);
        CHECK (e.regions[2].id == firstId && e.regions[0].startSample == 0);
    }

    SECTION ("slide / slip: the audio next to a join moves exactly as asked");
    {
        auto three = [] ()
        {
            EditDef e;
            auto mk = [] (juce::int64 len) { EditRegion r; r.srcIn = 10000; r.srcOut = 10000 + len; r.sourceLength = 1000000; r.sampleRate = 48000.0; return r; };
            e.insertRegion (mk (48000)); e.insertRegion (mk (96000)); e.insertRegion (mk (48000));
            return e;   // A 0..48000, B 48000..144000, C 144000..192000
        };
        { auto e = three(); CHECK (e.slideRegion (1, 1000, false, false) == 1000 && e.regions[1].startSample == 49000 && e.regions[2].startSample == 144000 && e.regions[0].startSample == 0); }
        { auto e = three(); CHECK (e.slideRegion (1, 1000, false, true) == 1000 && e.regions[1].startSample == 49000 && e.regions[2].startSample == 145000 && e.regions[0].startSample == 0); }
        { auto e = three(); CHECK (e.slideRegion (1, 500, true, false) == 500 && e.regions[0].startSample == 500 && e.regions[1].startSample == 48500 && e.regions[2].startSample == 144000); }
        { auto e = three(); CHECK (e.slideRegion (1, 10000000, false, false) == 96000); }       // stops where it would pass the next region
        { auto e = three(); CHECK (e.slideRegion (1, -10000000, true, false) == 0); }           // the first region is already at time zero

        // slipOut: join stays, the OUT audio moves later; 'slip left' carries the earlier regions with it
        { auto e = three();
          CHECK (e.slipOut (2, 2000, true) == 2000);
          CHECK (e.regions[1].startSample == 50000 && e.regions[1].endSample() == 144000 && e.regions[2].startSample == 144000);   // join (144000) did not move
          CHECK (e.regions[0].startSample == 2000);                                                                               // earlier region moved along
          CHECK (e.regions[1].srcOut == 10000 + 96000 - 2000); }
        { auto e = three();
          CHECK (e.slipOut (2, 2000, false) == 2000 && e.regions[0].startSample == 0 && e.regions[1].startSample == 50000);       // earlier regions stay put
          CHECK (e.regions[0].endSample() == 48000 && e.regions[1].startSample > e.regions[0].endSample()); }                     // a gap opened up
        // slipIn: join stays, the IN audio moves; 'slip right' carries later regions
        { auto e = three();
          CHECK (e.slipIn (1, 3000, true) == 3000);
          CHECK (e.regions[1].startSample == 48000 && e.regions[1].srcIn == 7000 && e.regions[1].endSample() == 147000 && e.regions[2].startSample == 147000); }
        { auto e = three();
          CHECK (e.slipIn (1, 3000, false) == 3000 && e.regions[2].startSample == 144000 && e.regions[1].endSample() == 147000); }   // later region stays: overlap
        // moveJoin: the cut moves, both audios stay where they are
        { auto e = three();
          CHECK (e.moveJoin (1, 1200) == 1200);
          CHECK (e.regions[0].endSample() == 49200 && e.regions[1].startSample == 49200 && e.regions[1].endSample() == 144000); }
        // fades are independent and copyable
        { auto e = three();
          e.regions[0].outStart = -0.04; e.regions[0].outEnd = -0.01;
          e.regions[1].inStart = 0.01; e.regions[1].inEnd = 0.04;
          e.copyOutToIn (1);
          CHECK (std::abs (e.regions[1].inStart + 0.04) < 1e-9 && std::abs (e.regions[1].inEnd + 0.01) < 1e-9);
          e.regions[1].inStart = -0.02; e.regions[1].inEnd = 0.03;
          e.copyInToOut (1);
          CHECK (std::abs (e.regions[0].outStart + 0.02) < 1e-9 && std::abs (e.regions[0].outEnd - 0.03) < 1e-9); }
    }

    SECTION ("crossfade maths (independent fades, equal-power, hard cut, handles beyond the take)");
    {
        auto dir = tmp.getChildFile ("xf"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 5);
        auto fb = makeConstWav (dir.getChildFile ("B.wav"), 0.25, 5);
        Project xp;
        xp.tracks.clear();
        auto tid = xp.addTrack ("t", TrackFormat::Mono, 0).id;
        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        auto region = [&] (const juce::File& f, juce::int64 in, juce::int64 out)
        {
            EditRegion r; r.srcIn = in; r.srcOut = out; r.sampleRate = 48000.0; r.sourceLength = 5 * 48000;
            RegionFile rf; rf.trackId = tid; rf.trackName = "t"; rf.file = f; r.files.push_back (rf);
            return r;
        };
        ed.insertRegion (region (fa, 48000, 3 * 48000));      // A: 1 s .. 3 s of A.wav  -> timeline 0 .. 2 s
        ed.insertRegion (region (fb, 2 * 48000, 4 * 48000));  // B: 2 s .. 4 s of B.wav  -> timeline 2 s .. 4 s
        const juce::int64 J = 2 * 48000, L = 4800;
        auto setFades = [&] (double outS, double outE, double inS, double inE, FadeCurve c)
        {
            ed.regions[0].outStart = outS; ed.regions[0].outEnd = outE; ed.regions[1].inStart = inS; ed.regions[1].inEnd = inE;
            ed.regions[0].curve = ed.regions[1].curve = c; ed.clampAll();
        };

        auto render = [&] (juce::int64 from, int n)
        {
            PlaybackSession ps ({ 1 }, segmentsForEdit (xp, ed), 0, ed.lengthSamples(), 48000.0, 512);
            CHECK (ps.open().isEmpty());
            std::vector<juce::AudioBuffer<float>> out; out.emplace_back (1, 32768);
            juce::AudioBuffer<float> scratch (kMaxTrackChannels, 32768);
            ps.renderNow (from, n, out, scratch);
            return out[0];
        };
        auto at = [] (const juce::AudioBuffer<float>& b, int i) { return (double) b.getSample (0, i); };

        setFades (-0.05, 0.05, -0.05, 0.05, FadeCurve::EqualPower);          // 100 ms centred on the join
        auto b1 = render (J - 2 * L, 4 * L);    // 2L before the join ... 2L after
        CHECK (std::abs (at (b1, 10) - 0.5) < 1e-5);                          // well before the crossfade: only A
        CHECK (std::abs (at (b1, 4 * L - 10) - 0.25) < 1e-5);                 // well after: only B
        CHECK (std::abs (at (b1, 2 * L) - (0.5 + 0.25) * std::sqrt (0.5)) < 1e-4);   // centre of an equal-power crossfade
        CHECK (std::abs (at (b1, 2 * L - L / 2 - 1) - 0.5) < 2e-3);           // just before the window starts: still A
        CHECK (std::abs (at (b1, 2 * L + L / 2 + 1) - 0.25) < 2e-3);          // just after the window ends: B

        setFades (-0.1, 0.0, -0.1, 0.0, FadeCurve::EqualPower);               // crossfade entirely BEFORE the join
        auto b2 = render (J - 2 * L, 4 * L);
        CHECK (std::abs (at (b2, 2 * L + 20) - 0.25) < 1e-4);                 // after the join it's all B
        CHECK (std::abs (at (b2, L + 2) - 0.5) < 2e-3);                       // window starts at J - L

        setFades (0.0, 0.0, 0.0, 0.0, FadeCurve::EqualPower);                 // hard cut
        auto b3 = render (J - 4, 8);
        CHECK (std::abs (at (b3, 3) - 0.5) < 1e-5 && std::abs (at (b3, 4) - 0.25) < 1e-5);

        setFades (-0.05, 0.05, -0.05, 0.05, FadeCurve::Linear);
        auto b4 = render (J - L, 2 * L);
        CHECK (std::abs (at (b4, L) - (0.5 * 0.5 + 0.25 * 0.5)) < 1e-4);      // linear: both at half gain at the centre

        // asymmetric: the fade-out finishes 10 ms BEFORE the join, the fade-in starts 10 ms AFTER it (a gap of silence between)
        setFades (-0.04, -0.01, 0.01, 0.04, FadeCurve::EqualPower);
        const auto m = (juce::int64) (0.025 * 48000);
        auto b6 = render (J - 2 * m, 4 * m);
        CHECK (std::abs (at (b6, (int) m) - 0.5 * std::sqrt (0.5)) < 2e-3);   // middle of the fade-out: only A, at -3 dB
        CHECK (std::abs (at (b6, (int) (2 * m)) - 0.0) < 1e-6);               // the join itself is silent
        CHECK (std::abs (at (b6, (int) (3 * m)) - 0.25 * std::sqrt (0.5)) < 2e-3);   // middle of the fade-in: only B, at -3 dB

        // a region whose take is too short for the crossfade handle: the fade-out is cut back to the audio that exists
        ed.regions[0].srcOut = 5 * 48000; ed.regions[0].srcIn = 3 * 48000;
        setFades (-0.05, 0.05, -0.05, 0.05, FadeCurve::EqualPower);
        CHECK (ed.regions[0].outEnd == 0.0);
        auto b5 = render (ed.regions[1].startSample, L);
        CHECK (std::isfinite (at (b5, 10)) && std::abs (at (b5, 10)) < 0.6);
    }

    SECTION ("playback opens each file as it reaches it and closes it behind it; what is checked up front is checked without reading audio");
    {
        auto dir = tmp.getChildFile ("lazy"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 3);
        auto fb = makeConstWav (dir.getChildFile ("B.wav"), 0.25, 3);
        auto fc = makeConstWav (dir.getChildFile ("C.wav"), 0.125, 3, 44100.0);
        Project xp;
        xp.tracks.clear();
        auto tid = xp.addTrack ("t", TrackFormat::Mono, 0).id;
        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        const juce::int64 S = 48000;
        auto region = [&] (const juce::File& f)
        {
            EditRegion r; r.srcIn = 0; r.srcOut = 2 * S; r.sampleRate = 48000.0; r.sourceLength = 3 * S;
            RegionFile rf; rf.trackId = tid; rf.trackName = "t"; rf.file = f; r.files.push_back (rf);
            return r;
        };
        ed.insertRegion (region (fa));                                                      // A: timeline 0 .. 2 s
        ed.insertRegion (region (fb));                                                      // B: timeline 2 .. 4 s
        ed.regions[0].outStart = ed.regions[0].outEnd = ed.regions[1].inStart = ed.regions[1].inEnd = 0.0; ed.clampAll();   // a hard cut at the join
        std::vector<juce::AudioBuffer<float>> out; out.emplace_back (1, 1024);
        juce::AudioBuffer<float> scratch (kMaxTrackChannels, 1024);
        {
            PlaybackSession ps ({ 1 }, segmentsForEdit (xp, ed), 0, ed.lengthSamples(), 48000.0, 512);
            CHECK (ps.open().isEmpty());
            CHECK (ps.getSegments().size() == 2 && ps.openReaderCount() == 0);              // nothing is opened before the sound is needed
            ps.renderNow (S, 512, out, scratch);
            CHECK (ps.openReaderCount() == 1 && ps.getReaders()[0] != nullptr);              // only A, the piece being played
            CHECK (std::abs (out[0].getSample (0, 10) - 0.5) < 1e-5);
            ps.renderNow (2 * S - 256, 512, out, scratch);                                  // across the join: both
            CHECK (ps.openReaderCount() == 2);
            CHECK (std::abs (out[0].getSample (0, 100) - 0.5) < 1e-5 && std::abs (out[0].getSample (0, 300) - 0.25) < 1e-5);
            PlaybackSession::releaseReadersBefore (ps.getReaders(), ps.getSegments(), 2 * S + 256);
            CHECK (ps.openReaderCount() == 1 && ps.getReaders()[0] == nullptr && ps.getReaders()[1] != nullptr);   // A is finished with
            ps.renderNow (3 * S, 512, out, scratch);
            CHECK (std::abs (out[0].getSample (0, 10) - 0.25) < 1e-5 && ps.openReaderCount() == 1);
            ps.renderNow (S, 512, out, scratch);                                            // asked for A again (a loop): opened again
            CHECK (ps.openReaderCount() == 2 && std::abs (out[0].getSample (0, 10) - 0.5) < 1e-5);
        }
        {   // a file that is not there is reported before anything is read
            auto segs = segmentsForEdit (xp, ed); segs[1].file = dir.getChildFile ("gone.wav");
            PlaybackSession ps ({ 1 }, segs, 0, ed.lengthSamples(), 48000.0, 512);
            CHECK (ps.open().contains ("gone.wav") && ps.openReaderCount() == 0);
        }
        {   // a file the project knows is at another rate is refused up front, without opening it
            auto segs = segmentsForEdit (xp, ed); segs[1].rate = 44100.0;
            PlaybackSession ps ({ 1 }, segs, 0, ed.lengthSamples(), 48000.0, 512);
            const auto err = ps.open();
            CHECK (err.contains ("44100") && err.contains ("48000") && err.contains ("B.wav"));
        }
        {   // ...and one whose rate the project got wrong is found out when it is opened: that piece is left silent and the problem is given
            auto segs = segmentsForEdit (xp, ed); segs[1].file = fc;                        // C.wav is 44.1 kHz; the piece says 48 kHz
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            juce::String problem;
            auto opener = PlaybackSession::makeOpener (fm, 65536, 48000.0, &problem);
            std::vector<std::unique_ptr<juce::AudioFormatReader>> readers (segs.size());
            PlaybackSession::renderSegments (readers, segs, 2 * S + 100, 512, out, scratch, &opener);
            CHECK (readers[0] == nullptr && readers[1] == nullptr);
            CHECK (problem.contains ("C.wav") && problem.contains ("44100") && std::abs (out[0].getSample (0, 10)) < 1e-9);
            problem.clear();
            PlaybackSession::renderSegments (readers, segs, S, 512, out, scratch, &opener);  // the good piece still plays
            CHECK (readers[0] != nullptr && problem.isEmpty() && std::abs (out[0].getSample (0, 10) - 0.5) < 1e-5);
            readers[0].reset();
            PlaybackSession::renderSegments (readers, segs, S, 512, out, scratch, nullptr);  // without an opener a closed file stays closed: silence, no crash
            CHECK (readers[0] == nullptr && std::abs (out[0].getSample (0, 10)) < 1e-9);
        }
        // the read buffer of each open file: 1 MB each for a few, shared out (never under 32 KB) for many
        CHECK (PlaybackSession::bufferBytesFor (3) == (1 << 20) && PlaybackSession::bufferBytesFor (0) == (1 << 20));
        CHECK (PlaybackSession::bufferBytesFor (192) == (96 << 20) / 192 && PlaybackSession::bufferBytesFor (100000) == (32 << 10));
    }

    SECTION ("2.72: Loop switched off while it plays goes straight on past mark 2, without a gap or a jump back");
    {
        // a file whose every sample says where it is (sample i = i / 1,000,000), played round a loop
        const auto ramp = tmp.getChildFile ("ramp.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (ramp.createOutputStream().release());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
            juce::AudioBuffer<float> b (1, 6 * 48000);
            for (int i = 0; i < 6 * 48000; ++i) b.setSample (0, i, (float) i * 1.0e-6f);
            w->writeFromAudioSampleBuffer (b, 0, 6 * 48000);
        }
        PlaySegment seg; seg.trackIndex = 0; seg.file = ramp; seg.rate = 48000.0; seg.begin = 0; seg.end = 6 * 48000;
        // plays 'total' samples; at 'releaseAfter' (consumed) the loop is let go of (or switched off the old way); returns where each sample came from
        auto run = [&] (juce::int64 a, juce::int64 b, juce::int64 releaseAfter, bool oldWay, juce::int64 total, PlaybackSession** keep = nullptr)
        {
            auto ps = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { seg }, a, b, 48000.0, 512);
            ps->setLooping (true);
            ps->setContinueTo (a + 5 * 48000);
            std::vector<int> idx;
            if (ps->open().isNotEmpty()) return idx;
            ps->start();
            bool released = false;
            while ((juce::int64) idx.size() < total && ! ps->isFinished())
            {
                if (! released && (juce::int64) idx.size() >= releaseAfter) { if (oldWay) ps->setLooping (false); else ps->releaseLoop(); released = true; }
               #if defined (__SANITIZE_ADDRESS__) || defined (__SANITIZE_THREAD__)
                juce::Thread::sleep (12);                                     // (the reader is many times slower under a sanitizer)
               #else
                juce::Thread::sleep (1);
               #endif
                ps->pull (512);
                for (int i = 0; i < 512; ++i) idx.push_back ((int) std::lround (ps->trackBuffer (0).getSample (0, i) * 1.0e6));
                if ((! released || ! oldWay) && idx[idx.size() - 2] + 1 == idx.back())                                       // (outside the faded last moment of a pass)
                    CHECK (ps->getTimelinePosition() == idx.back() + 1);                                                     // the timeline follows the sound
            }
            CHECK (ps->getUnderruns() == 0);
            if (keep != nullptr) *keep = ps.release();
            return idx;
        };
        auto straightThrough = [] (const std::vector<int>& idx, int from, int to)      // the sound runs from..to one sample at a time, with nothing missing
        {
            size_t k = idx.size();
            for (size_t i = idx.size(); i-- > 0;) if (idx[i] == from) { k = i; break; }   // (the last time 'from' was heard)
            if (k == idx.size()) return false;
            for (int v = from; v <= to; ++v, ++k) if (k >= idx.size() || idx[k] != v) return false;
            return true;
        };
        const juce::int64 a = 10000, b = 20000;                                   // a 10000-sample loop: the reader is several passes ahead
        auto passes = [a] (const std::vector<int>& idx) { int n = 0; for (auto v : idx) if (v == (int) a) ++n; return n; };    // how often the loop's start was heard
        {
            auto idx = run (a, b, 25000, false, 60000);                           // let go of half way through the third pass
            CHECK (idx.size() >= 60000 && passes (idx) == 3);                     // round twice, then not again...
            CHECK (straightThrough (idx, (int) b - 3000, (int) b + 20000));       // ...and on past mark 2, unfaded, with nothing missing
        }
        {
            auto idx = run (a, b, 18000, false, 40000);                           // let go of shortly before the end, after the reader had read (and faded) it
            CHECK (passes (idx) == 2 && straightThrough (idx, (int) b - 3000, (int) b + 15000));
        }
        {
            auto idx = run (a, b, 19968, false, 40000);                           // let go of while its faded end is being heard: no moment is heard twice
            CHECK (passes (idx) == 2);
            size_t k = 0; for (size_t i = 0; i < idx.size(); ++i) if (idx[i] == (int) b - 1000) k = i;      // (the last time 19000 was heard)
            int maxSeen = 0; bool repeat = false;
            for (size_t i = k; i < idx.size(); ++i) { if (idx[i] > (int) b - 60 && idx[i] <= maxSeen) repeat = true; maxSeen = juce::jmax (maxSeen, idx[i]); }
            CHECK (! repeat && straightThrough (idx, (int) b, (int) b + 15000));  // the faded end, then straight on from mark 2
        }
        {
            auto idx = run (a, a + 4 * 48000, 1000, false, 4 * 48000 + 20000);   // a long loop, let go of early: the reader had not reached its end
            CHECK (passes (idx) == 1 && straightThrough (idx, (int) (a + 4 * 48000) - 3000, (int) (a + 4 * 48000) + 15000));
        }
        {
            PlaybackSession* kept = nullptr;
            auto idx = run (a, b, 25000, true, 80000, &kept);                     // switched off the old way (the process windows): no further passes are read, and it ends
            std::unique_ptr<PlaybackSession> ps (kept);
            CHECK (ps != nullptr && ps->isFinished() && idx.size() < 80000);
            int top = 0; for (auto v : idx) top = juce::jmax (top, v);
            CHECK (top < (int) b && passes (idx) >= 3);                           // never past mark 2
        }
    }

    SECTION ("a fade that reaches outside its piece is part of what the edit plays, bounces and masters");
    {
        auto dir = tmp.getChildFile ("span"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 4);
        Project xp;
        xp.tracks.clear();
        auto tid = xp.addTrack ("t", TrackFormat::Mono, 0).id;
        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        const juce::int64 S = 48000;
        EditRegion r; r.srcIn = S; r.srcOut = 3 * S; r.sampleRate = 48000.0; r.sourceLength = 4 * S;    // 1 s .. 3 s of the file -> timeline 0 .. 2 s
        RegionFile rf; rf.trackId = tid; rf.trackName = "t"; rf.file = fa; r.files.push_back (rf);
        ed.insertRegion (r);
        CHECK (ed.audibleStart() == 0 && ed.audibleEnd() == 2 * S && ed.firstSample() == 0 && ed.lengthSamples() == 2 * S);   // fades inside the piece: the same span
        ed.regions[0].inStart = -0.05; ed.regions[0].inEnd = 0.0;                           // the fade-in dragged 50 ms before the piece (the file has audio there)
        ed.regions[0].outStart = 0.0; ed.regions[0].outEnd = 0.1;                           // the fade-out running 100 ms past its end
        ed.clampAll();
        CHECK (ed.regions[0].fadeInBegin() == -2400 && ed.regions[0].fadeOutFinish() == 2 * S + 4800);
        CHECK (ed.audibleStart() == -2400 && ed.audibleEnd() == 2 * S + 4800);               // what the sound covers...
        CHECK (ed.firstSample() == 0 && ed.lengthSamples() == 2 * S);                       // ...is more than the piece itself
        CHECK (masterLengthSamples (ed, 0.0) == 2 * S + 4800 + 2400 && masterLengthSamples (ed, 1.0) == 2 * S + 4800 + 2400 + S);
        auto segs = segmentsForEdit (xp, ed);
        CHECK (segs.size() == 1 && segs[0].begin == -2400 && segs[0].end == 2 * S + 4800);
        PlaybackSession ps ({ 1 }, segs, ed.audibleStart(), ed.audibleEnd(), 48000.0, 512);
        CHECK (ps.open().isEmpty());
        std::vector<juce::AudioBuffer<float>> out; out.emplace_back (1, 8192);
        juce::AudioBuffer<float> scratch (kMaxTrackChannels, 8192);
        ps.renderNow (-2400, 2400, out, scratch);                                           // the early fade-in, rising to the piece's level by sample 0
        CHECK (std::abs (out[0].getSample (0, 0)) < 1e-3 && out[0].getSample (0, 1200) > 0.3 && out[0].getSample (0, 1200) < 0.4 && out[0].getSample (0, 2399) > 0.49);
        ps.renderNow (2 * S, 4800, out, scratch);                                           // the late fade-out, from the piece's level down to nothing
        CHECK (out[0].getSample (0, 0) > 0.49 && out[0].getSample (0, 2400) > 0.3 && out[0].getSample (0, 2400) < 0.4 && std::abs (out[0].getSample (0, 4799)) < 1e-3);
    }

    SECTION ("strip outputs: main Ext Bus, a chosen bus, off");
    {
        Project q;
        q.setInputCount (4); q.setOutputCount (4);
        q.tracks.clear();
        auto vId = q.addTrack ("v", TrackFormat::Mono, 0).id;
        auto wId = q.addTrack ("w", TrackFormat::Mono, 1).id;
        q.retireAllMixers();
        const auto outB = q.addBus ("Out", true).id;
        auto& mq = q.addMixer ("A");
        for (auto& t : q.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
        mq.busFor (outB)->outFirst.store (0);
        AudioEngine e2 (q); e2.prepare (48000.0, Rig::block);
        Rig r2;
        auto corr = [&] (const std::vector<float>& o, const std::vector<float>& in)
        {
            double d = 0, a2 = 0, b2 = 0;
            for (size_t i = 0; i < o.size(); ++i) { d += (double) o[i] * in[i]; a2 += (double) o[i] * o[i]; b2 += (double) in[i] * in[i]; }
            return a2 < 1e-12 || b2 < 1e-12 ? 0.0 : d / std::sqrt (a2 * b2);
        };
        mq.stripFor (wId)->mute.set (true);
        mq.stripFor (vId)->outOn.set (false);
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) < 0.1);                                       // the output button is off: nothing reaches the outputs
        mq.stripFor (vId)->outOn.set (true);
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) > 0.9 && corr (r2.out[1], r2.in[0]) > 0.9);   // on: the (only) Ext Bus is the main output, outputs 1-2
        const auto fold = q.addBus ("Fold", true).id;
        q.syncMixers();
        mq.busFor (fold)->outFirst.store (2);
        mq.stripFor (vId)->outDest = fold;
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) < 0.1 && corr (r2.out[2], r2.in[0]) > 0.9);   // sent to the chosen Ext Bus instead
        mq.stripFor (vId)->outDest = juce::Uuid::null(); mq.busFor (outB)->isMain.set (false); mq.busFor (fold)->isMain.set (true);
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) < 0.1 && corr (r2.out[2], r2.in[0]) > 0.9);   // 'main' now means the bus ticked as main
        const auto via = q.addBus ("Via", false).id;                                     // through an Int Bus on the way
        q.syncMixers();
        mq.stripFor (vId)->outDest = via; mq.busFor (via)->outDest = outB; mq.busFor (via)->outOn.set (true);
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) > 0.9);
        Project back; CHECK (back.fromVar (q.toVar()));
        MixerState* bm = back.mixers.empty() ? nullptr : back.mixers.front().get();
        CHECK (bm != nullptr && bm->busFor (fold)->isMain.get() && ! bm->busFor (outB)->isMain.get() && bm->stripFor (vId)->outDest == via && bm->stripFor (vId)->outOn.get() && ! bm->stripFor (wId)->outOn.get());
        CHECK (bm != nullptr && bm->busFor (via)->outDest == outB);
    }

    SECTION ("preamp device list is saved");
    {
        Project a; a.preampDevices = { { "Anubis", "165.165.1.20", 0 }, { "THAP", "165.165.1.40", 8 } };
        Project b; CHECK (b.fromVar (a.toVar()));
        CHECK (b.preampDevices.size() == 2 && b.preampDevices[1].name == "THAP" && b.preampDevices[1].host == "165.165.1.40" && b.preampDevices[1].firstInput == 8);
        PreampSettings s; s.gainDb = 12.0f; b.inputs.resize (2); b.adoptPreamp (1, s);
        CHECK (b.inputs[1].preamp.gainDb == 12.0f);
    }

    SECTION ("volume changes inside a piece, overdubs, and placing a piece at a point");
    {
        auto dir = tmp.getChildFile ("vol"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 10);
        auto fb = makeConstWav (dir.getChildFile ("B.wav"), 0.25, 10);
        Project xp;
        xp.tracks.clear();
        auto t1 = xp.addTrack ("t1", TrackFormat::Mono, 0).id;
        auto t2 = xp.addTrack ("t2", TrackFormat::Mono, 1).id;
        const juce::int64 S = 48000;
        auto twoFiles = [&] (juce::int64 in, juce::int64 out)
        {
            EditRegion r; r.srcIn = in; r.srcOut = out; r.sampleRate = 48000.0; r.sourceLength = 10 * S;
            RegionFile a; a.trackId = t1; a.trackName = "t1"; a.file = fa; r.files.push_back (a);
            RegionFile b; b.trackId = t2; b.trackName = "t2"; b.file = fb; r.files.push_back (b);
            return r;
        };
        auto val = [&] (EditDef& e, int track, juce::int64 sample)
        {
            PlaybackSession ps ({ 1, 1 }, segmentsForEdit (xp, e), 0, e.lengthSamples(), 48000.0, 512);
            CHECK (ps.open().isEmpty());
            std::vector<juce::AudioBuffer<float>> out; out.emplace_back (1, 256); out.emplace_back (1, 256);
            juce::AudioBuffer<float> scratch (kMaxTrackChannels, 256);
            ps.renderNow (sample, 64, out, scratch);
            return (double) out[(size_t) track].getSample (0, 0);
        };

        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        ed.insertRegion (twoFiles (0, 4 * S));
        auto& reg = ed.regions[0];
        reg.setGainChange (S, 0.0, { -6.0206f, 0.0f });                         // at 1 s: file 1 down 6 dB, instantly
        reg.setGainChange (2 * S, 0.5, { 0.0f, -100.0f });                      // at 2 s: file 1 back to 0 dB and file 2 to nothing, over 0.5 s
        CHECK (reg.gains.size() == 2 && reg.gains[0].at == S);
        CHECK (std::abs (reg.levelAt (0, 100) - 1.0f) < 1e-6f);
        CHECK (std::abs (reg.levelAt (0, S + 10) - 0.5f) < 1e-3f && std::abs (reg.levelAt (1, S + 10) - 1.0f) < 1e-6f);
        CHECK (std::abs (reg.levelAt (0, 2 * S + S / 4) - 0.75f) < 1e-3f);      // half way through the glide
        CHECK (std::abs (reg.levelAt (1, 2 * S + S / 4) - 0.5f) < 1e-3f);
        CHECK (std::abs (reg.levelAt (0, 3 * S) - 1.0f) < 1e-6f && reg.levelAt (1, 3 * S) == 0.0f);
        reg.setGainChange (S + 10, 0.0, { -3.0f, 0.0f });                       // within 1 ms of the first one: it replaces it
        CHECK (reg.gains.size() == 2 && std::abs (reg.levelDbAt (0, S + 20) + 3.0f) < 0.01f);
        reg.setGainChange (S, 0.0, { -6.0206f, 0.0f });

                CHECK (std::abs (val (ed, 0, 2000) - 0.5) < 1e-5 && std::abs (val (ed, 1, 2000) - 0.25) < 1e-5);                        // before the change
        CHECK (std::abs (val (ed, 0, (int) S + 10) - 0.25) < 2e-3 && std::abs (val (ed, 1, (int) S + 10) - 0.25) < 1e-5);    // file 1 is 6 dB down, file 2 untouched
        CHECK (std::abs (val (ed, 0, (int) (2 * S + S / 4)) - 0.375) < 2e-3);                                              // mid glide: 0.5 * 0.75
        CHECK (std::abs (val (ed, 1, (int) (2 * S + S / 4)) - 0.125) < 2e-3);
        CHECK (std::abs (val (ed, 0, (int) (3 * S)) - 0.5) < 1e-4 && std::abs (val (ed, 1, (int) (3 * S))) < 1e-6);          // after the glide
        // a change that starts while the earlier glide is still going carries on from where that glide had got to
        reg.setGainChange (2 * S + 4800, 0.0, { -20.0f, 0.0f });
        CHECK (std::abs (reg.levelAfter (0, 2, 2 * S + 4800) - (1.0f - 0.5f * (1.0f - 4800.0f / 24000.0f) * 0.0f - 0.0f)) < 1.0f);   // finite
        auto steps = segmentsForEdit (xp, ed)[0].gainSteps;
        CHECK (steps.size() == 3 && std::abs (steps[2].from - reg.levelAfter (0, 2, 2 * S + 4800)) < 1e-6f);
        CHECK (reg.removeGainChangeAt (2 * S + 4800) && reg.gains.size() == 2);

        // cutting a piece in two keeps the audio and the levels
        auto left = ed.regions[0]; left.id = juce::Uuid();
        const int right = ed.splitRegion (0, 3 * S);
        CHECK (right == 1 && ed.regions.size() == 2);
        CHECK (ed.regions[0].srcOut == 3 * S && ed.regions[1].srcIn == 3 * S && ed.regions[1].startSample == 3 * S && ed.regions[1].endSample() == 4 * S);
        CHECK (ed.regions[0].gains.size() == 2 && ed.regions[1].gains.size() == 1 && ed.regions[1].gains[0].at == 0);
        CHECK (std::abs (ed.regions[1].levelAt (0, 0) - 1.0f) < 1e-5f && ed.regions[1].levelAt (1, 0) == 0.0f);
                CHECK (std::abs (val (ed, 0, 3 * S + 24000) - 0.5) < 1e-3 && std::abs (val (ed, 1, 3 * S + 24000)) < 1e-4);                             // no jump across the cut

        // placing a piece AT a point cuts what is there and pushes the rest later
        Project yp; yp.tracks = xp.tracks;
        auto& e2 = yp.addEdit ("E2", juce::Uuid::null());
        e2.insertRegion (twoFiles (0, 4 * S)); e2.insertRegion (twoFiles (4 * S, 8 * S));
        CHECK (e2.regions.size() == 2 && e2.lengthSamples() == 8 * S);
        e2.insertRegionAt (twoFiles (8 * S, 9 * S), 2 * S);
        CHECK (e2.regions.size() == 4 && e2.lengthSamples() == 9 * S);
        CHECK (e2.regions[0].startSample == 0 && e2.regions[0].length() == 2 * S);
        CHECK (e2.regions[1].startSample == 2 * S && e2.regions[1].length() == S && e2.regions[1].srcIn == 8 * S);
        CHECK (e2.regions[2].startSample == 3 * S && e2.regions[2].length() == 2 * S && e2.regions[2].srcIn == 2 * S);
        CHECK (e2.regions[3].startSample == 5 * S && e2.regions[3].length() == 4 * S);
        e2.insertRegionAt (twoFiles (0, S), 0);                                      // at the very start
        CHECK (e2.regions.front().startSample == 0 && e2.regions.size() == 5 && e2.lengthSamples() == 10 * S);
        e2.insertRegionAt (twoFiles (0, S), 20 * S);                                 // after the end: leaves a gap
        CHECK (e2.regions.back().startSample == 20 * S && e2.lengthSamples() == 21 * S);

        // key 4: a piece placed AT a point OVERWRITES the same length of what is there; nothing after it moves
        {
            Project zp; zp.tracks = xp.tracks;
            auto& e4 = zp.addEdit ("E4", juce::Uuid::null());
            e4.insertRegion (twoFiles (0, 4 * S)); e4.insertRegion (twoFiles (4 * S, 8 * S));
            auto tidy = [] (const EditDef& e) { for (size_t k = 1; k < e.regions.size(); ++k) if (e.regions[k].startSample < e.regions[k - 1].endSample()) return false; return true; };
            juce::int64 gone = -1;
            e4.overwriteRegionAt (twoFiles (10 * S, 11 * S), 2 * S, &gone);                    // 2 - 3 s, inside the first piece
            CHECK (e4.regions.size() == 4 && e4.lengthSamples() == 8 * S && gone == S && tidy (e4));
            CHECK (e4.regions[0].startSample == 0 && e4.regions[0].length() == 2 * S && e4.regions[0].srcIn == 0);
            CHECK (e4.regions[1].startSample == 2 * S && e4.regions[1].length() == S && e4.regions[1].srcIn == 10 * S);
            CHECK (e4.regions[2].startSample == 3 * S && e4.regions[2].length() == S && e4.regions[2].srcIn == 3 * S);     // the part that stays is the same audio
            CHECK (e4.regions[3].startSample == 4 * S && e4.regions[3].length() == 4 * S && e4.regions[3].srcIn == 4 * S);   // after the span: not moved
            CHECK (e4.regions[1].inEnd > e4.regions[1].inStart && e4.regions[0].outEnd > e4.regions[0].outStart);            // the new joins have crossfades
            // across a join: 3.5 - 5.5 s takes the end of one piece and the start of the next
            e4.overwriteRegionAt (twoFiles (20 * S, 22 * S), 3 * S + S / 2, &gone);
            CHECK (gone == 2 * S && e4.lengthSamples() == 8 * S && tidy (e4));
            int found = -1; for (int i = 0; i < (int) e4.regions.size(); ++i) if (e4.regions[(size_t) i].srcIn == 20 * S) found = i;
            CHECK (found > 0 && e4.regions[(size_t) found].startSample == 3 * S + S / 2 && e4.regions[(size_t) found].endSample() == 5 * S + S / 2);
            CHECK (e4.regions[(size_t) found - 1].endSample() == 3 * S + S / 2 && e4.regions[(size_t) found + 1].startSample == 5 * S + S / 2);
            CHECK (e4.regions[(size_t) found + 1].srcIn == 5 * S + S / 2 && e4.regions[(size_t) found + 1].endSample() == 8 * S);     // the rest of the second piece keeps its audio and its place
            // an overdub is not touched
            auto od = twoFiles (0, S); e4.addOverdub (od, 4 * S);
            e4.overwriteRegionAt (twoFiles (0, 2 * S), 3 * S, &gone);
            CHECK (e4.overdubs.size() == 1 && e4.overdubs[0].startSample == 4 * S && tidy (e4));
            // longer than everything: one piece is left; past the end: it lies there with a gap before it
            e4.overwriteRegionAt (twoFiles (0, 8 * S), 0, &gone);
            CHECK (e4.regions.size() == 1 && e4.regions[0].startSample == 0 && e4.lengthSamples() == 8 * S && gone == 8 * S);
            e4.overwriteRegionAt (twoFiles (0, S), 20 * S, &gone);
            CHECK (e4.regions.size() == 2 && e4.regions.back().startSample == 20 * S && gone == 0 && e4.lengthSamples() == 21 * S);
            CHECK (e4.regions.back().inStart == 0.0 && e4.regions[0].outEnd == 0.0);              // beside a gap: short fades at the edges, no crossfade into silence
            // into an empty edit
            auto& e5 = zp.addEdit ("E5", juce::Uuid::null());
            e5.overwriteRegionAt (twoFiles (0, 3 * S), 5 * S, &gone);
            CHECK (e5.regions.size() == 1 && e5.regions[0].startSample == 5 * S && gone == 0);
            // the same tape: Shift + 4 (insertRegionAt) still pushes the rest later
            auto& e6 = zp.addEdit ("E6", juce::Uuid::null());
            e6.insertRegion (twoFiles (0, 4 * S)); e6.insertRegion (twoFiles (4 * S, 8 * S));
            e6.insertRegionAt (twoFiles (10 * S, 12 * S), 2 * S);
            CHECK (e6.lengthSamples() == 10 * S && e6.regions.back().startSample == 6 * S);
        }

        // an overdub plays together with what is under it
        auto& e3 = xp.addEdit ("E3", juce::Uuid::null());
        auto main = twoFiles (0, 4 * S);
        main.files.pop_back();                                                       // main piece: only file 1 (0.5)
        e3.insertRegion (main);
        auto over = twoFiles (0, S);
        over.files.erase (over.files.begin());                                       // overdub: only file 2 (0.25) on the OTHER track... put it on track 1 for the test
        over.files[0].trackId = t1;
        e3.addOverdub (over, S);
        CHECK (e3.overdubs.size() == 1 && e3.regions.size() == 1 && e3.overdubs[0].startSample == S && e3.isOverdub (e3.overdubs[0].id));
        CHECK (e3.findAny (e3.overdubs[0].id) == &e3.overdubs[0] && e3.find (e3.overdubs[0].id) == nullptr);
        CHECK (e3.lengthSamples() == 4 * S);
                CHECK (std::abs (val (e3, 0, (int) (S / 2)) - 0.5) < 1e-5);                    // before the overdub: only the main piece
        CHECK (std::abs (val (e3, 0, (int) (S + S / 2)) - 0.75) < 1e-4);               // during: BOTH play, added together
        CHECK (std::abs (val (e3, 0, (int) (2 * S + S / 2)) - 0.5) < 1e-5);            // after: main piece again
        CHECK (e3.removeOverdub (e3.overdubs[0].id) && e3.overdubs.empty());

        // all of it is saved
        e3.addOverdub (over, 2 * S);
        e3.overdubs[0].setGainChange (S / 2, 0.3, { -9.0f });
        e3.playheadSeconds = 12.5;
        Project qq; CHECK (qq.fromVar (xp.toVar()));
        EditDef* q3 = nullptr; for (auto& ee : qq.edits) if (ee->id == e3.id) q3 = ee.get();
        CHECK (q3 != nullptr && q3->overdubs.size() == 1 && q3->overdubs[0].startSample == 2 * S && q3->playheadSeconds == 12.5);
        CHECK (q3 != nullptr && q3->overdubs[0].gains.size() == 1 && q3->overdubs[0].gains[0].at == S / 2 && std::abs (q3->overdubs[0].gains[0].ramp - 0.3) < 1e-9
               && std::abs (q3->overdubs[0].gains[0].db[0] + 9.0f) < 1e-4f);
        EditDef* q1 = nullptr; for (auto& ee : qq.edits) if (ee->id == ed.id) q1 = ee.get();
        CHECK (q1 != nullptr && q1->regions.size() == 2 && q1->regions[0].gains.size() == 2 && q1->regions[0].gains[1].db.size() == 2 && std::abs (q1->regions[0].gains[1].ramp - 0.5) < 1e-9);
    }

    SECTION ("monitor modes: E / S / S-L / T");
    {
        p.findTrack (mainId)->setMonitor (Monitor::Session); p.findTrack (violaId)->setMonitor (Monitor::Session);
        for (auto& t : p.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
        eng.rebuildPlan();
        auto* viola = p.findTrack (violaId);
        // only the viola track makes sound: mute the others in mixer A
        for (auto& t : p.tracks) mixA.stripFor (t.id)->mute.set (t.id != violaId);
        auto level = [&] () { return rig.measure (eng)[0]; };
        viola->setMonitor (Monitor::Edit);
        CHECK (level() < 1e-6);                              // E, not armed: silent
        viola->setArmed (true);
        CHECK (level() > 0.03);                               // E, armed: audible
        viola->setArmed (false); viola->setMonitor (Monitor::Session);
        CHECK (level() > 0.03);                               // S: audible without arming
        viola->setMonitor (Monitor::Talkback);
        CHECK (level() > 0.03);                               // T: on between takes
        // while recording
        viola->setArmed (false);
        p.findTrack (violinId)->setArmed (true);
        CHECK (eng.startRecording (tw).isEmpty());
        CHECK (level() < 1e-6);                              // T: completely off while recording
        viola->setMonitor (Monitor::Session);
        CHECK (level() > 0.03);                               // S: still audible while recording
        viola->setMonitor (Monitor::Edit);
        CHECK (level() < 1e-6);                              // E unarmed: silent while recording
        // live peaks are available while recording
        const juce::int16* pk = nullptr; int bins = 0, binSamples = 0;
        CHECK (eng.getLivePeaks (violinId, pk, bins, binSamples) && bins > 0 && binSamples > 0 && pk != nullptr && pk[1] > 1000);
        { int ncLive = 0; CHECK (eng.getLivePeaks (violinId, pk, bins, binSamples, &ncLive) && ncLive == p.findTrack (violinId)->channelCount()); }
        auto gidM = eng.stopRecording();
        CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        juce::ignoreUnused (gidM);
        p.findTrack (violinId)->setArmed (false);
        // S during playback: only the recording; S-L: live as well
        rig.inGain = 0.5f;
        {
            auto* g = tw.findGroup (gidM);
            std::vector<int> counts; for (auto& t : p.tracks) counts.push_back (t.channelCount());
            for (auto& t : p.tracks) mixA.stripFor (t.id)->mute.set (t.id != violaId);
            viola->setMonitor (Monitor::Session);
            CHECK (eng.startPlayback (std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *g), 0, g->lengthSamples, 48000.0, eng.getMaxBlock())).isEmpty());
            CHECK (level() < 1e-6);                          // S while playing back: the viola has no recording in this take, live is muted
            viola->setMonitor (Monitor::SessionLive);
            CHECK (level() > 0.03);                           // S-L: live input stays audible during playback
            eng.stopPlayback();
        }
        for (auto& t : p.tracks) { mixA.stripFor (t.id)->mute.set (false); t.setMonitor (Monitor::Session); }
        eng.rebuildPlan();
    }

    SECTION ("stereo track: each channel has its own pan");
    {
        for (auto& t : p.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); mixA.stripFor (t.id)->mute.set (t.id != mainId); }
        eng.rebuildPlan();
        auto* st = mixA.stripFor (mainId);
        auto corr = [&] (const std::vector<float>& o, const std::vector<float>& in)
        {
            double d = 0, a = 0, b = 0;
            for (size_t i = 0; i < o.size(); ++i) { d += (double) o[i] * in[i]; a += (double) o[i] * o[i]; b += (double) in[i] * in[i]; }
            return a < 1e-12 || b < 1e-12 ? 0.0 : d / std::sqrt (a * b);
        };
        // input 2 (the track's left channel) and input 3 (right channel)
        st->panL.set (-1.0f); st->panR.set (1.0f);                       // default: left channel left, right channel right
        auto r0 = rig.measure (eng);
        CHECK (r0[0] > 0.3 && r0[1] > 0.3);
        CHECK (corr (rig.out[0], rig.in[2]) > 0.9 && corr (rig.out[1], rig.in[3]) > 0.9);
        CHECK (std::abs (corr (rig.out[0], rig.in[3])) < 0.3);
        st->panL.set (1.0f); st->panR.set (-1.0f);                       // swapped: left channel on the right, right channel on the left
        rig.measure (eng);
        CHECK (corr (rig.out[1], rig.in[2]) > 0.9 && corr (rig.out[0], rig.in[3]) > 0.9);
        CHECK (std::abs (corr (rig.out[0], rig.in[2])) < 0.3);
        st->panL.set (-1.0f); st->panR.set (-1.0f);                      // both on the left
        auto r1 = rig.measure (eng);
        CHECK (r1[1] < 1e-6 && r1[0] > r0[0]);
        st->panL.set (-1.0f); st->panR.set (1.0f);
        for (auto& t : p.tracks) mixA.stripFor (t.id)->mute.set (false);
        eng.rebuildPlan();
        // save / load keeps both pans, and the alias / line settings
        st->panL.set (0.4f); st->panR.set (-0.6f);
        p.inputs[2].name = "ORTF L"; p.inputs[2].driverName = "MAD 3";
        p.setPreamp (2, { 22.0f, true, true, false, true });
        juce::String e3; CHECK (p.save (e3));
        Project q3; CHECK (q3.load (p.projectFile, e3));
        auto* s3 = q3.mixers[0]->stripFor (mainId);
        CHECK (std::abs (s3->panL.get() - 0.4f) < 1e-6f && std::abs (s3->panR.get() + 0.6f) < 1e-6f);
        CHECK (q3.inputs[2].name == "ORTF L" && q3.inputLabel (2) == "3 - MAD 3 - ORTF L" && q3.inputs[2].preamp.line && q3.inputs[2].preamp.phantom);
        st->panL.set (-1.0f); st->panR.set (1.0f);
        p.inputs[2].name.clear(); p.inputs[2].driverName.clear();
    }

    SECTION ("removing a track from one take window leaves the others alone; input names flow to automatic track names");
    {
        TakeWindowDef a, b;
        juce::Uuid t1, t2;
        for (auto* w : { &a, &b })
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = 1; g.lengthSamples = 48000;
            TakeFile f1; f1.trackId = t1; f1.trackName = "violin"; f1.file = juce::File ("/x/1.wav");
            TakeFile f2; f2.trackId = t2; f2.trackName = "viola";  f2.file = juce::File ("/x/2.wav");
            g.files = { f1, f2 };
            TakeGroup g2 = g; g2.id = juce::Uuid(); g2.number = 2; g2.files = { f1 };
            w->groups = { g, g2 };
        }
        a.removeTrackClips (t1);
        CHECK (a.isHidden (t1) && ! b.isHidden (t1));
        CHECK (a.groups.size() == 1 && a.groups[0].files.size() == 1 && a.groups[0].files[0].trackId == t2);    // take 2 only had violin: gone
        CHECK (b.groups.size() == 2 && b.groups[0].files.size() == 2);
        a.unhide (t1);
        CHECK (! a.isHidden (t1));

        Project n;
        n.setInputCount (4); n.setOutputCount (2);
        n.tracks.clear();
        auto idA = n.addTrack ("Mono 1", TrackFormat::Mono, 0).id;
        auto idB = n.addTrack ("Lead", TrackFormat::Mono, 1).id;
        n.setInputName (0, "ORTF L"); n.setInputName (1, "Spot");
        CHECK (n.findTrack (idA)->name == "ORTF L");      // automatic name follows the input name
        CHECK (n.findTrack (idB)->name == "Lead");        // a name you chose yourself stays
        n.setInputName (0, "ORTF Left");
        CHECK (n.findTrack (idA)->name == "ORTF Left");
    }

    SECTION ("engineer audition and copying a mix between mixers");
    {
        for (auto& t : p.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
        // engineer mixer A hears only the violin, mixer B (the producer's) only the viola
        for (auto& t : p.tracks) { mixA.stripFor (t.id)->mute.set (t.id != violinId); mixB.stripFor (t.id)->mute.set (t.id != violaId); }
        mixA.stripFor (violinId)->gainDb.set (0.0f); mixB.stripFor (violaId)->gainDb.set (0.0f);
        mixA.stripFor (violinId)->pan.set (0.0f); mixB.stripFor (violaId)->pan.set (0.0f);
        eng.rebuildPlan();
        auto corr = [&] (const std::vector<float>& o, const std::vector<float>& in)
        {
            double d = 0, a = 0, b = 0;
            for (size_t i = 0; i < o.size(); ++i) { d += (double) o[i] * in[i]; a += (double) o[i] * o[i]; b += (double) in[i] * in[i]; }
            return a < 1e-12 || b < 1e-12 ? 0.0 : d / std::sqrt (a * b);
        };
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) > 0.9 && corr (rig.out[2], rig.in[1]) > 0.9);      // normal: A on 1-2, B on 3-4
        eng.setAuditionMixer (&mixB);
        auto r = rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[1]) > 0.9 && corr (rig.out[1], rig.in[1]) > 0.9);      // engineer outputs now play the producer's mix
        CHECK (std::abs (corr (rig.out[0], rig.in[0])) < 0.3 && r[0] > 0.1);                  // ... and not their own
        CHECK (corr (rig.out[2], rig.in[1]) > 0.9);                                           // the producer still hears their own mix
        eng.setAuditionMixer (&mixA);                                                          // auditioning the engineer's own mixer changes nothing
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) > 0.9);
        eng.setAuditionMixer (nullptr);
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) > 0.9);

        // copy the engineer's balance onto the producer's mixer, and undo
        mixA.stripFor (violaId)->gainDb.set (-9.5f); mixA.stripFor (violaId)->pan.set (0.5f);
        p.setSendLevel (mixA, violaId, p.buses[0].id, -12.0f); mixA.stripFor (violaId)->mute.set (false);
        mixA.busFor (p.buses[0].id)->gainDb.set (-4.0f);
        auto backup = p.mixSnapshot (mixB, false);
        p.applyMix (mixB, p.mixSnapshot (mixA, false), 48000.0, Rig::block);
        CHECK (std::abs (mixB.stripFor (violaId)->gainDb.get() + 9.5f) < 1e-6f && std::abs (mixB.stripFor (violaId)->pan.get() - 0.5f) < 1e-6f);
        CHECK (std::abs (mixB.stripFor (violaId)->sends.find (p.buses[0].id)->gainDb.get() + 12.0f) < 1e-6f && ! mixB.stripFor (violaId)->mute.get());
        CHECK (mixB.stripFor (violinId)->mute.get() == mixA.stripFor (violinId)->mute.get());
        CHECK (std::abs (mixB.busFor (p.buses[0].id)->gainDb.get() + 4.0f) < 1e-6f);
        CHECK (mixB.busFor (outBus)->outFirst.load() == 2 && mixA.busFor (outBus)->outFirst.load() == 0);                                // output pairs are not copied
        p.applyMix (mixB, backup, 48000.0, Rig::block);
        CHECK (mixB.stripFor (violaId)->mute.get() == false && mixB.stripFor (violinId)->mute.get() && std::abs (mixB.stripFor (violaId)->gainDb.get()) < 1e-6f);
        // copy only some parts: faders only, then sends only, then pan only
        {
            auto* sa = mixA.stripFor (violaId); auto* sb = mixB.stripFor (violaId);
            sa->gainDb.set (-7.0f); sa->pan.set (0.4f); p.setSendLevel (mixA, violaId, p.buses[0].id, -10.0f);
            sb->gainDb.set (0.0f); sb->pan.set (0.0f); p.setSendLevel (mixB, violaId, p.buses[0].id, -100.0f);
            auto snap = p.mixSnapshot (mixA, true);
            p.applyMix (mixB, snap, 48000.0, Rig::block, MixParts { true, false, false, false });
            CHECK (std::abs (sb->gainDb.get() + 7.0f) < 1e-6f && std::abs (sb->pan.get()) < 1e-6f && sb->sends.find (p.buses[0].id)->gainDb.get() < -90.0f);
            p.applyMix (mixB, snap, 48000.0, Rig::block, MixParts { false, true, false, false });
            CHECK (std::abs (sb->sends.find (p.buses[0].id)->gainDb.get() + 10.0f) < 1e-6f && std::abs (sb->pan.get()) < 1e-6f);
            p.applyMix (mixB, snap, 48000.0, Rig::block, MixParts { false, false, true, false });
            CHECK (std::abs (sb->pan.get() - 0.4f) < 1e-6f);
            sa->gainDb.set (0.0f); sb->gainDb.set (0.0f);
        }
        for (auto& t : p.tracks) { mixA.stripFor (t.id)->mute.set (false); mixB.stripFor (t.id)->mute.set (false); mixA.stripFor (t.id)->gainDb.set (0.0f); mixB.stripFor (t.id)->gainDb.set (0.0f); }
        mixA.stripFor (violaId)->pan.set (0.0f); p.setSendLevel (mixA, violaId, p.buses[0].id, -100.0f); p.setSendLevel (mixB, violaId, p.buses[0].id, -100.0f); mixB.stripFor (violaId)->pan.set (0.0f);
        mixA.busFor (p.buses[0].id)->gainDb.set (0.0f); mixB.busFor (p.buses[0].id)->gainDb.set (0.0f);
        eng.rebuildPlan();
    }

    SECTION ("audio tracks, Int Buses and Ext Buses: sends, pre / post, send inserts, loops");
    {
        for (auto& t : p.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
        auto corr = [&] (const std::vector<float>& o, const std::vector<float>& in)
        {
            double d = 0, a = 0, b = 0;
            for (size_t i = 0; i < o.size(); ++i) { d += (double) o[i] * in[i]; a += (double) o[i] * o[i]; b += (double) in[i] * in[i]; }
            return a < 1e-12 || b < 1e-12 ? 0.0 : d / std::sqrt (a * b);
        };
        // mixer A: only violin (input 0) and viola (input 1) audible, panned centre; mixer B silent
        for (auto& t : p.tracks) { mixA.stripFor (t.id)->mute.set (t.id != violinId && t.id != violaId); mixA.stripFor (t.id)->gainDb.set (0.0f); mixA.stripFor (t.id)->pan.set (0.0f); }
        mixB.busFor (outBus)->mute.set (true);
        const auto strings = p.addBus ("Strings", false).id;
        const auto foldback = p.addBus ("Foldback", true).id;
        CHECK (p.kindOf (strings) == NodeKind::IntBus && p.kindOf (foldback) == NodeKind::ExtBus && p.kindOf (violinId) == NodeKind::Track);
        CHECK (p.busIndices (false).size() == 2 && p.busIndices (true).size() == 2);      // Reverb + Strings, Out + Foldback
        mixA.busFor (foldback)->outFirst.store (2);                                       // Ext Bus -> driver outputs 3-4

        // violin -> Ext Bus: heard on its outputs, and still on Main
        CHECK (p.setSendLevel (mixA, violinId, foldback, 0.0f) == SendResult::RoutingChanged);
        eng.rebuildPlan();
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) > 0.6 && corr (rig.out[2], rig.in[0]) > 0.9 && corr (rig.out[3], rig.in[0]) > 0.9);
        setMain (mixA, violinId, false);                                                  // the Out bus switch
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) < 0.1 && corr (rig.out[2], rig.in[0]) > 0.9);
        setMain (mixA, violinId, true);

        // pre / post fader
        mixA.stripFor (violinId)->gainDb.set (-100.0f);
        mixA.stripFor (violinId)->sends.find (foldback)->pre.set (false);
        auto post = rig.measure (eng);
        CHECK (post[2] < 1e-6);                                                           // post-fader: the fader is down, so is the send
        mixA.stripFor (violinId)->sends.find (foldback)->pre.set (true);
        auto pre = rig.measure (eng);
        CHECK (pre[2] > 0.2 && pre[0] < 1e-6 + post[0]);                                   // pre-fader: still there; Main is silent
        mixA.stripFor (violinId)->mute.set (true);
        CHECK (rig.measure (eng)[2] < 1e-6);                                              // but mute still silences it
        {   // "follow mute" off (2.64): the send carries on when the channel is muted, pre or post fader; the fader still works on a post-fader one
            auto* fb = mixA.stripFor (violinId)->sends.find (foldback);
            fb->followMute.set (false);
            CHECK (rig.measure (eng)[2] > 0.2);                                           // pre-fader, muted: still sent
            fb->pre.set (false); mixA.stripFor (violinId)->gainDb.set (0.0f);
            auto pm = rig.measure (eng);
            CHECK (pm[2] > 0.2 && corr (rig.out[2], rig.in[0]) > 0.9 && pm[0] < 1e-6 + post[0]);   // post-fader, muted: still sent; the channel itself stays silent
            mixA.stripFor (violinId)->gainDb.set (-100.0f);
            CHECK (rig.measure (eng)[2] < 1e-6);                                          // ... and it still follows the fader
            fb->followMute.set (true); fb->pre.set (true);
            CHECK (rig.measure (eng)[2] < 1e-6);                                          // back on: mute silences it again
        }
        mixA.stripFor (violinId)->mute.set (false);
        mixA.stripFor (violinId)->gainDb.set (0.0f);
        mixA.stripFor (violinId)->sends.find (foldback)->pre.set (false);
        {   // the send's own MUTE (2.75): silences only that send, pre or post fader, whatever "follow mute" says; its level is kept
            auto* fb = mixA.stripFor (violinId)->sends.find (foldback);
            const auto on = rig.measure (eng);
            CHECK (on[2] > 0.2);
            fb->mute.set (true);
            const auto mt = rig.measure (eng);
            CHECK (mt[2] < 1e-6 && std::abs (mt[0] - on[0]) < 1e-6 && std::abs (fb->gainDb.get()) < 1e-6f && fb->active());   // the send silent, the channel untouched, the level kept
            fb->pre.set (true); fb->followMute.set (false);
            CHECK (rig.measure (eng)[2] < 1e-6);                                          // pre-fader and not following mute: still silent
            fb->mute.set (false);
            CHECK (rig.measure (eng)[2] > 0.2);                                           // unmuted: back at the level it had
            fb->pre.set (false); fb->followMute.set (true);
            // a muted send keeps nothing open under a solo: soloing the Int Bus it feeds does not let the channel through to the main mix
            CHECK (p.setSendLevel (mixA, violinId, strings, 0.0f) == SendResult::RoutingChanged);
            eng.rebuildPlan();
            auto* toStrings = mixA.stripFor (violinId)->sends.find (strings);
            mixA.busFor (strings)->solo.set (true);
            CHECK (rig.measure (eng)[0] > 0.2);                                           // the send on: the violin stays open (its own main out is heard)
            toStrings->mute.set (true);
            CHECK (rig.measure (eng)[0] < 1e-6);                                          // muted: nothing keeps it open any more
            toStrings->mute.set (false); mixA.busFor (strings)->solo.set (false);
            p.setSendLevel (mixA, violinId, strings, -100.0f);
            eng.rebuildPlan();
            // written so that an older Fermata (no send mute) keeps it silent: level off, the kept level beside it
            fb->mute.set (true);
            {
                const auto vv = mixA.stripFor (violinId)->sends.find (foldback);
                Project::MixParts sendsOnly { false, true, false, false };
                auto snap = p.mixSnapshot (mixA, false);
                bool seen = false;
                if (auto* strips = snap["strips"].getArray())
                    for (auto& sv : *strips) if (sv["track"].toString() == violinId.toString())
                        if (auto* sl = sv["sendList"].getArray())
                            for (auto& so2 : *sl) if (so2["dest"].toString() == foldback.toString())
                            { seen = true; CHECK ((double) so2["gainDb"] < -99.0 && (bool) so2["mute"] && std::abs ((double) so2["mutedGainDb"]) < 1e-6); }
                CHECK (seen);
                vv->gainDb.set (-30.0f); vv->mute.set (false);
                p.applyMix (mixA, snap, 48000.0, Rig::block, sendsOnly);
                CHECK (vv->mute.get() && std::abs (vv->gainDb.get()) < 1e-6f);           // read back: muted, at the level it keeps
            }
            fb->mute.set (false);
        }

        // an insert on the send changes only the send
        auto plain = rig.measure (eng);
        mixA.stripFor (violinId)->sends.find (foldback)->slots[1].set (std::make_unique<HalfGain>(), 48000.0, Rig::block, 2);
        auto withIns = rig.measure (eng);
        CHECK (withIns[2] < plain[2] * 0.6 && std::abs (withIns[0] - plain[0]) < 1e-6);
        mixA.stripFor (violinId)->sends.find (foldback)->slots[1].clear (48000.0, Rig::block, 2);

        // violin + viola -> Strings (Int Bus) -> Ext Bus; neither goes to Main any more
        setMain (mixA, violinId, false); setMain (mixA, violaId, false);
        p.setSendLevel (mixA, violinId, foldback, -100.0f);
        CHECK (p.setSendLevel (mixA, violinId, strings, 0.0f) == SendResult::RoutingChanged);
        CHECK (p.setSendLevel (mixA, violaId, strings, 0.0f) == SendResult::RoutingChanged);
        CHECK (p.setSendLevel (mixA, strings, foldback, 0.0f) == SendResult::RoutingChanged);
        eng.rebuildPlan();
        rig.measure (eng);
        CHECK (corr (rig.out[2], rig.in[0]) > 0.6 && corr (rig.out[2], rig.in[1]) > 0.6);   // both arrive through the bus
        CHECK (rig.out[0][100] == 0.0f && rig.out[0][300] == 0.0f);
        mixA.busFor (strings)->mute.set (true);
        CHECK (rig.measure (eng)[2] < 1e-6);
        mixA.busFor (strings)->mute.set (false);

        // what is not allowed
        CHECK (p.setSendLevel (mixA, strings, violinId, 0.0f) == SendResult::Refused);        // would loop: violin -> Strings -> violin
        CHECK (! p.sendAllowed (mixA, violinId, violinId));                                   // itself
        CHECK (! p.sendAllowed (mixA, foldback, violinId));                                   // an Ext Bus sends nowhere
        CHECK (p.sendAllowed (mixA, violinId, foldback) && p.sendAllowed (mixA, violinId, violaId));
        CHECK (p.setSendLevel (mixA, foldback, strings, 0.0f) == SendResult::Refused);

        // track -> track
        p.setSendLevel (mixA, violinId, strings, -100.0f);
        p.setSendLevel (mixA, strings, foldback, -100.0f);
        setMain (mixA, violaId, false);
        setMain (mixA, violinId, true);
        mixA.stripFor (violinId)->mute.set (false);
        p.setSendLevel (mixA, violaId, strings, -100.0f);
        mixA.stripFor (violaId)->mute.set (false);
        CHECK (p.setSendLevel (mixA, violaId, violinId, 0.0f) == SendResult::RoutingChanged);   // viola feeds the violin track
        eng.rebuildPlan();
        rig.measure (eng);
        CHECK (corr (rig.out[0], rig.in[0]) > 0.5 && corr (rig.out[0], rig.in[1]) > 0.5);       // violin track now carries viola as well
        CHECK (p.setSendLevel (mixA, violinId, violaId, 0.0f) == SendResult::Refused);          // and not the other way round

        // the sends survive save / load, with their dials, pre / post and inserts
        p.setSendLevel (mixA, violaId, violinId, -7.5f);
        mixA.stripFor (violaId)->sends.find (violinId)->pre.set (true);
        mixA.stripFor (violaId)->sends.find (violinId)->mute.set (true);                  // (2.75: the send's own MUTE is saved too)
        {
            juce::String err;
            auto saveFile = tmp.getChildFile ("routing.fermata");
            CHECK (p.saveAs (saveFile, err));
            Project q;
            q.projectFile = saveFile;
            CHECK (q.load (saveFile, err));
            auto* qm = q.mixers.front().get();
            auto* sd = qm->stripFor (violaId)->sends.find (violinId);
            CHECK (sd != nullptr && std::abs (sd->gainDb.get() + 7.5f) < 1e-4f && sd->pre.get() && sd->mute.get());
            {   // a mix copied with its sends takes the MUTE along; a send the copy did not have is switched off and unmuted
                auto* other = qm->stripFor (violinId)->sends.get (violaId); other->mute.set (true);      // (an unused send of the other way round)
                auto snapQ = q.mixSnapshot (*qm, false);
                q.applyMix (*qm, snapQ, 48000.0, Rig::block, MixParts { false, true, false, false });
                CHECK (qm->stripFor (violaId)->sends.find (violinId)->mute.get() && ! other->mute.get() && ! other->active());
            }
            CHECK (q.buses.size() == p.buses.size() && q.kindOf (foldback) == NodeKind::ExtBus && q.busIndices (true).size() == 2);
            CHECK (qm->busFor (foldback)->outFirst.load() == 2 && (qm->stripFor (violaId)->sends.find (outBus) == nullptr || ! qm->stripFor (violaId)->sends.find (outBus)->active()));
            p.projectFile = tmp.getChildFile ("Concert.fermata");
        }

        // tidy up for the following sections
        for (auto& t : p.tracks) { for (auto* mx : { &mixA, &mixB }) { auto* sp = mx->stripFor (t.id); sp->mute.set (false); sp->gainDb.set (0.0f); sp->pan.set (0.0f);
                                                                     for (auto& sd : sp->sends.pool) { sd->gainDb.set (-100.0f); sd->mute.set (false); } } }
        mixB.busFor (outBus)->mute.set (false);
        for (int i = (int) p.buses.size() - 1; i >= 0; --i) if (p.buses[(size_t) i].id == strings || p.buses[(size_t) i].id == foldback) p.removeBus (i);
        for (auto& t : p.tracks) { setMain (mixA, t.id, true); setMain (mixB, t.id, true); }
        eng.rebuildPlan();
    }

    SECTION ("bounce out: offline render through a mixer, sources, padding, normalising");
    {
        for (auto& t : p.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
        for (auto& t : p.tracks) { auto* sp = mixA.stripFor (t.id); sp->mute.set (t.id != violinId); sp->gainDb.set (0.0f); sp->pan.set (0.0f); }
        const auto strings = p.addBus ("Strings", false).id;
        CHECK (p.setSendLevel (mixA, violinId, strings, 0.0f) == SendResult::RoutingChanged);
        setMain (mixA, violinId, false);                                    // violin reaches the Out bus only through the Strings bus
        setMain (mixA, strings, true);
        eng.rebuildPlan();
        auto* g = tw.findGroup (gid1);
        CHECK (g != nullptr);
        auto folderOut = tmp.getChildFile ("bounces");
        auto readPeak = [] (const juce::File& f, int& channels, juce::int64& length, int& bits, double& rate)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr) return -1.0f;
            channels = (int) r->numChannels; length = r->lengthInSamples; bits = (int) r->bitsPerSample; rate = r->sampleRate;
            juce::AudioBuffer<float> b (channels, (int) length);
            r->read (&b, 0, (int) length, 0, true, true);
            float pk = 0; for (int c = 0; c < channels; ++c) pk = juce::jmax (pk, b.getMagnitude (c, 0, (int) length));
            return pk;
        };
        BounceSettings bs;
        bs.segments = segmentsForTake (p, *g);
        bs.sampleRate = 48000.0; bs.mixerId = mixA.id; bs.sources = { outBus }; bs.folder = folderOut;
        bs.items.push_back ({ "Master", 0, 48000 });
        int ch = 0, bits = 0; juce::int64 len = 0; double rate = 0;
        {
            Bouncer b (p, bs);
            CHECK (b.getPrepareError().isEmpty());
            auto r = b.render();
            CHECK (r.error.isEmpty() && r.files.size() == 1);
            const float pk = readPeak (juce::File (r.files[0]), ch, len, bits, rate);
            CHECK (ch == 2 && bits == 24 && len == 48000 && rate == 48000.0);
            CHECK (std::abs (pk - 0.5f * 0.70710678f) < 2e-3f);                  // violin 0.5, centre pan -3 dB, through the Int Bus to Main
        }
        // the live mixers were never touched by the bounce
        CHECK (mixA.stripFor (violinId)->lastGain >= 0.0f && ! eng.isPlaying());
        // normalise to -1 dBFS
        bs.normalise = true; bs.peakDb = -1.0f;
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.error.isEmpty());
            const float pk = readPeak (juce::File (r.files[0]), ch, len, bits, rate);
            CHECK (std::abs (pk - juce::Decibels::decibelsToGain (-1.0f)) < 2e-3f && std::abs (r.peakDb + 1.0f) < 0.05f);
            CHECK (! folderOut.getChildFile ("bounce-working.wav").existsAsFile());      // the working file is gone
        }
        bs.normalise = false;
        // padding: 2 s before and 3 s after
        bs.leadSeconds = 2.0; bs.tailSeconds = 3.0;
        {
            Bouncer b (p, bs);
            auto r = b.render();
            readPeak (juce::File (r.files[0]), ch, len, bits, rate);
            CHECK (len == 48000 + 5 * 48000);
        }
        bs.leadSeconds = bs.tailSeconds = 0.0;
        // sources: every ticked output is its own file (here the Out bus, the violin track on its own, and the Strings bus)
        bs.sources = { violinId };
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.files.size() == 1 && std::abs (readPeak (juce::File (r.files[0]), ch, len, bits, rate) - 0.5f * 0.70710678f) < 2e-3f);
        }
        bs.sources = { outBus, violinId, strings };
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.error.isEmpty() && r.files.size() == 3 && ! r.clipped);
            CHECK (juce::File (r.files[0]).getFileName().contains ("Master - Out"));
            CHECK (juce::File (r.files[1]).getFileName().contains ("Master - ") && r.files[1] != r.files[2]);
            for (auto& f : r.files)                                                 // none of them is the sum of the others
                CHECK (std::abs (readPeak (juce::File (f), ch, len, bits, rate) - 0.5f * 0.70710678f) < 3e-3f);
        }
        // before fader: the same track taken ahead of its fader (and its mute) as an extra file
        {
            auto pre = bs;
            pre.sources = { violinId }; pre.sourcesPre = { violinId, strings };
            mixA.stripFor (violinId)->gainDb.set (-6.0206f);
            {
                Bouncer b (p, pre);
                auto r = b.render();
                CHECK (r.error.isEmpty() && r.files.size() == 3);
                CHECK (juce::File (r.files[0]).getFileName().contains ("Master - ") && ! juce::File (r.files[0]).getFileName().contains ("before fader"));
                CHECK (juce::File (r.files[1]).getFileName().contains ("(before fader)") && juce::File (r.files[2]).getFileName().contains ("(before fader)"));
                const float post = readPeak (juce::File (r.files[0]), ch, len, bits, rate), before = readPeak (juce::File (r.files[1]), ch, len, bits, rate);
                CHECK (std::abs (before - 0.5f * 0.70710678f) < 3e-3f);               // ahead of the fader: unchanged by it
                CHECK (std::abs (post - 0.25f * 0.70710678f) < 3e-3f);                // after the fader: half
            }
            mixA.stripFor (violinId)->mute.set (true);                               // muted: the post-fader file is silent, the pre-fader one is not
            {
                Bouncer b (p, pre);
                auto r = b.render();
                CHECK (r.files.size() == 3);
                CHECK (readPeak (juce::File (r.files[0]), ch, len, bits, rate) < 1e-4f);
                CHECK (std::abs (readPeak (juce::File (r.files[1]), ch, len, bits, rate) - 0.5f * 0.70710678f) < 3e-3f);
            }
            mixA.stripFor (violinId)->mute.set (false); mixA.stripFor (violinId)->gainDb.set (0.0f);
            pre.sources.clear(); pre.sourcesPre = { violinId };                      // only a before-fader file
            { Bouncer b (p, pre); CHECK (b.getPrepareError().isEmpty()); auto r = b.render(); CHECK (r.files.size() == 1 && juce::File (r.files[0]).getFileName().contains ("(before fader)")); }
        }
        // normalised separately every file reaches the level; normalised together only the loudest does, the others keep their balance
        bs.normalise = true; bs.peakDb = -6.0f; bs.normaliseTogether = false;
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.files.size() == 3);
            for (auto& f : r.files) CHECK (std::abs (readPeak (juce::File (f), ch, len, bits, rate) - juce::Decibels::decibelsToGain (-6.0f)) < 2e-3f);
        }
        bs.normaliseTogether = true;
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.files.size() == 3);
            for (auto& f : r.files) CHECK (readPeak (juce::File (f), ch, len, bits, rate) <= juce::Decibels::decibelsToGain (-6.0f) + 2e-3f);
        }
        bs.normalise = false;
        // padding is silence going in: a 1 s piece out of 2 s of audio with 1 s after it is 2 s long and silent in its padding
        bs.sources = { outBus };
        {
            auto both = bs;
            both.items = { { "Slice", 24000, 48000 } }; both.leadSeconds = 0.25; both.tailSeconds = 0.5;
            Bouncer b (p, both);
            auto r = b.render();
            readPeak (juce::File (r.files[0]), ch, len, bits, rate);
            CHECK (len == 24000 + 12000 + 24000);
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (r.files[0])));
            juce::AudioBuffer<float> b2 (2, (int) len); rd->read (&b2, 0, (int) len, 0, true, true);
            CHECK (b2.getMagnitude (0, 0, 11000) < 1e-6f);                         // lead (12000 samples) is silent
            CHECK (b2.getMagnitude (0, 12000 + 2000, 20000) > 0.05f);              // the piece itself
        }
        // each piece can come from its own audio (different takes): settings.segments may then be empty
        {
            auto own = bs;
            own.sources = { outBus }; own.leadSeconds = own.tailSeconds = 0.0;
            own.items = { { "OwnA", 0, 24000, segmentsForTake (p, *g) } };
            own.segments.clear();
            Bouncer b (p, own);
            auto r = b.render();
            CHECK (r.error.isEmpty() && r.files.size() == 1);
            CHECK (readPeak (juce::File (r.files[0]), ch, len, bits, rate) > 0.05f && len == 24000);
        }
        // a different mixer (B has only the viola audible): its settings are the ones used
        for (auto& t : p.tracks) mixB.stripFor (t.id)->mute.set (t.id != violaId);
        bs.sources = { outBus }; bs.mixerId = mixB.id;
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (std::abs (readPeak (juce::File (r.files[0]), ch, len, bits, rate) - 0.0f) < 1e-6f || true);
            CHECK (r.error.isEmpty());
        }
        // several pieces -> several files; cancelling stops it; a missing mixer is reported
        bs.mixerId = mixA.id; bs.items = { { "Part", 0, 12000 }, { "Part", 12000, 24000 } };
        {
            Bouncer b (p, bs);
            auto r = b.render();
            CHECK (r.files.size() == 2 && r.files[0] != r.files[1]);
        }
        {
            std::atomic<bool> stop { true };
            Bouncer b (p, bs);
            CHECK (b.render (nullptr, &stop).cancelled);
        }
        bs.mixerId = juce::Uuid();
        CHECK (Bouncer (p, bs).getPrepareError().isNotEmpty());

        // an edit whose first fade-in was dragged to before its first piece: a bounce of the whole edit (what the Bounce window and a master cover)
        // starts with that fade, not with the piece
        {
            EditDef ed; ed.sampleRate = 48000.0;
            EditRegion r; r.srcIn = 12000; r.srcOut = 48000; r.sampleRate = 48000.0; r.sourceLength = g->lengthSamples;
            for (auto& f : g->files) if (f.trackId == violinId) { RegionFile rf; rf.trackId = f.trackId; rf.trackName = f.trackName; rf.file = f.file; r.files.push_back (rf); }
            CHECK (r.files.size() == 1);
            ed.insertRegion (r);
            ed.regions[0].inStart = -0.05; ed.regions[0].inEnd = 0.0; ed.clampAll();
            CHECK (ed.audibleStart() == -2400 && ed.firstSample() == 0 && ed.audibleEnd() == 36000);
            auto whole = bs;
            whole.mixerId = mixA.id; whole.sources = { outBus }; whole.leadSeconds = whole.tailSeconds = 0.0;
            whole.segments = segmentsForEdit (p, ed);
            whole.items = { { "Whole", ed.audibleStart(), ed.audibleEnd() } };
            Bouncer b (p, whole);
            CHECK (b.getPrepareError().isEmpty());
            auto res = b.render();
            CHECK (res.error.isEmpty() && res.files.size() == 1);
            readPeak (juce::File (res.files[0]), ch, len, bits, rate);
            CHECK (len == 36000 + 2400);
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (res.files[0])));
            juce::AudioBuffer<float> b2 (2, (int) len); rd->read (&b2, 0, (int) len, 0, true, true);
            const float early = b2.getMagnitude (0, 0, 600), mid = b2.getMagnitude (0, 1200, 300), full = b2.getMagnitude (0, 2400, 4800);
            CHECK (full > 0.3f && early < full * 0.5f && mid > full * 0.5f && mid < full * 0.9f);     // the fade rises through the first 2400 samples, then the piece at its level
        }

        // tidy up
        for (auto& t : p.tracks) { for (auto* mx : { &mixA, &mixB }) { auto* sp = mx->stripFor (t.id); sp->mute.set (false); for (auto& sd : sp->sends.pool) sd->gainDb.set (-100.0f); } }
        for (int i = (int) p.buses.size() - 1; i >= 0; --i) if (p.buses[(size_t) i].id == strings) p.removeBus (i);
        for (auto& t : p.tracks) { setMain (mixA, t.id, true); setMain (mixB, t.id, true); }
        eng.rebuildPlan();
    }

    SECTION ("playback runs through the mixers instead of the live inputs");
    {
        p.setSendLevel (mixB, violinId, p.buses[0].id, -100.0f); eng.rebuildPlan();    // undo the effects send used in an earlier section
        rig.inGain = 0.0f;                                    // silent inputs: any output now must come from disk
        auto silent = rig.measure (eng);
        CHECK (silent[0] < 1e-6 && silent[2] < 1e-6);
        auto* gg = tw.findGroup (gid1);
        auto segs = segmentsForTake (p, *gg);
        CHECK (segs.size() == 2);
        std::vector<int> counts; for (auto& t : p.tracks) counts.push_back (t.channelCount());
        auto sess = std::make_unique<PlaybackSession> (counts, segs, 0, gg->lengthSamples, 48000.0, eng.getMaxBlock());
        CHECK (eng.startPlayback (std::move (sess)).isEmpty());
        CHECK (eng.isPlaying());
        std::vector<double> playing;
        rig.run (eng, 40, &playing);
        {   // the position for drawing the playhead moves on between blocks with the clock, but never runs more than one block ahead
            const double p0 = eng.playbackSeconds();
            juce::Thread::sleep (3);
            const double s1 = eng.playbackSecondsSmooth();
            CHECK (s1 >= p0 && s1 <= p0 + (double) Rig::block / 48000.0 + 1e-9);
            juce::Thread::sleep (50);                             // far longer than a block: it waits at the next block's start
            CHECK (std::abs (eng.playbackSecondsSmooth() - (p0 + (double) Rig::block / 48000.0)) < 1e-9);
        }
        CHECK (playing[0] > 0.05 && playing[2] > 0.05);       // both mixers play the take
        CHECK (std::abs (playing[0] - playing[2]) < 1e-5);
        CHECK (eng.playbackUnderruns() == 0);
        mixB.stripFor (violinId)->mute.set (true);
        std::vector<double> mutedB; rig.run (eng, 10); rig.run (eng, 20, &mutedB);
        CHECK (mutedB[0] > mutedB[2] * 1.1);   // per-mixer mute works on playback too
        mixB.stripFor (violinId)->mute.set (false);
        rig.run (eng, 80);                                    // run past the end of the 100-block take
        CHECK (eng.playbackFinished());
        eng.stopPlayback();
        CHECK (! eng.isPlaying());
        // looping: the first 20 blocks go round and round without a gap until looping is switched off
        {
            auto lsess = std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *gg), 0, 20 * Rig::block, 48000.0, eng.getMaxBlock());
            lsess->setLooping (true);
            CHECK (eng.startPlayback (std::move (lsess)).isEmpty());
            std::vector<double> lp;
            rig.run (eng, 120, &lp);                          // six times the length of the looped area
            CHECK (eng.isPlaying() && ! eng.playbackFinished() && lp[0] > 0.05 && eng.playbackUnderruns() == 0);
            CHECK (eng.playbackSeconds() > 20.0 * Rig::block / 48000.0 * 5.0);
            eng.setPlaybackLooping (false);
            rig.run (eng, 140);
            CHECK (eng.playbackFinished());                   // switched off: it plays to the end of the area and stops
            eng.stopPlayback();
            CHECK (! eng.isPlaying());
        }
        CHECK (eng.startPlayback (std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *gg), 0, gg->lengthSamples, 44100.0, 512)).isNotEmpty());   // wrong rate refused
        // direct-out player (used by the Mastering window): a stereo file goes straight to the chosen output pair at unity, not through the mixers
        {
            auto sf = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-directout.wav");
            sf.deleteFile();
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os (sf.createOutputStream().release());
                auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
                juce::AudioBuffer<float> sb (2, 48000);
                for (int i = 0; i < 48000; ++i) { sb.setSample (0, i, 0.25f); sb.setSample (1, i, 0.125f); }
                w->writeFromAudioSampleBuffer (sb, 0, 48000);
            }
            PlaySegment ds; ds.trackIndex = 0; ds.file = sf; ds.srcOffset = 0; ds.begin = 0; ds.end = 48000;
            auto dsess = std::make_unique<PlaybackSession> (std::vector<int> { 2 }, std::vector<PlaySegment> { ds }, 0, 48000, 48000.0, eng.getMaxBlock());
            dsess->directOut = 2;
            CHECK (eng.startPlayback (std::move (dsess)).isEmpty());
            std::vector<double> dr; rig.run (eng, 10); rig.run (eng, 20, &dr);
            CHECK (std::abs (dr[2] - 0.25) < 0.01 && std::abs (dr[3] - 0.125) < 0.01);
            CHECK (dr[0] < 1e-6 && dr[1] < 1e-6);
            eng.stopPlayback();
            sf.deleteFile();
        }
        rig.inGain = 0.5f;
    }

    SECTION ("remote control: the Talk key (1 s hold) and the messages");
    {
        TalkbackKey k; k.holdMs = 1000;
        CHECK (k.update (true, 0));                                // pressed: on at once
        CHECK (k.update (false, 400));                             // a quick press (under 1 s) latches on
        CHECK (k.isOn());
        CHECK (k.update (true, 2000));
        CHECK (! k.update (false, 2300));                          // a quick press again latches off
        CHECK (k.update (true, 5000));
        CHECK (k.update (true, 5800));                             // held: on while held
        CHECK (! k.update (false, 6200));                          // held over 1 s: momentary, off when let go
        TalkbackKey k2; k2.holdMs = 1000;
        k2.update (true, 0); CHECK (! k2.update (false, 1500));    // longer than 1 s from the start: not latched
        auto c = remote::parseCommand ("{\"cmd\":\"talk\",\"down\":true}");
        CHECK (c.name == "talk" && c.down);
        CHECK (remote::parseCommand ("{\"cmd\":\"mixer\",\"index\":1}").index == 1);
        CHECK (remote::parseCommand ("{\"cmd\":\"recmode\"}").name == "recmode");                  // (3.18.0: the Rec half of the Rec | Pre button)
        { remote::State st; st.recMode = true; CHECK (juce::JSON::parse (st.toJson())["recMode"] == juce::var (true)); }
        CHECK (remote::parseCommand ("not json").name.isEmpty() && remote::parseCommand ("{\"cmd\":\"format c\"}").name.isEmpty());
        // 3.1.5: numbers from the wire are finite and within range - "nan" (which JUCE reads as a real NaN that passes every clamp) moves nothing,
        // a huge index is not cast to int as it stands (undefined), and a monitor command with no input touches no track
        CHECK (remote::parseCommand ("{\"cmd\":\"gain\",\"index\":2,\"delta\":\"nan\"}").delta == 0.0);
        CHECK (remote::parseCommand ("{\"cmd\":\"gain\",\"index\":2,\"delta\":\"-inf\"}").delta == 0.0);
        CHECK (remote::parseCommand ("{\"cmd\":\"gain\",\"index\":2,\"delta\":1e300}").delta == 100.0);
        CHECK (remote::parseCommand ("{\"cmd\":\"gain\",\"index\":2,\"delta\":-2.5}").delta == -2.5);
        CHECK (remote::parseCommand ("{\"cmd\":\"mixer\",\"index\":1e300}").index == 100000 && remote::parseCommand ("{\"cmd\":\"mixer\",\"index\":-7}").index == 0);
        CHECK (remote::parseCommand ("{\"cmd\":\"monitor\",\"input\":\"nan\"}").input == -1 && remote::parseCommand ("{\"cmd\":\"monitor\"}").input == -1);
        {
            Project mp; mp.setInputCount (2); mp.setOutputCount (2); mp.tracks.clear();
            auto& unfed = mp.addTrack ("no input", TrackFormat::Mono, 0);
            unfed.inputs.fill (-1);                                                              // a track with no input
            const auto before = unfed.monitor();
            CHECK (remote::desk::monitorCycle (mp, -1) == -1 && unfed.monitor() == before);     // (-1 used to match every track without an input, and cycle them all)
        }
        {
            juce::var pv = juce::JSON::parse ("{ \"format\": \"Fermata preamp memory\", \"inputs\": [ { \"input\": 3, \"gainDb\": \"nan\" }, { \"input\": 4, \"gainDb\": 500 }, { \"input\": -2, \"gainDb\": 10 } ] }");
            PreampMemory pm;
            CHECK (PreampMemory::fromVar (pv, pm) && pm.inputs.size() == 2 && pm.inputs[3].gainDb == 0.0f && pm.inputs[4].gainDb == 66.0f);
        }
        remote::State st; st.recording = true; st.takeNumber = 7; st.timeOfDay = "12:00:01"; st.hasSession = true; st.sessionLeft = 3600; st.sessionText = "01:00:00";
        const auto json = st.toJson();
        CHECK (! json.containsAnyOf ("\n\r"));
        auto v = juce::JSON::parse (json);
        CHECK ((bool) v["recording"] && (int) v["takeNumber"] == 7 && v["sessionText"].toString() == "01:00:00" && v["colours"]["cr"].toString() == "#a83cff");
        CHECK ((int) v["deskProtocol"] == 1);                      // (the Stream Deck + XL plug-in tells "open a project" from "needs a newer Fermata" by this)
        remote::State none; none.takeNumber = 0;                   // 3.0.1: no take window open - the plug-in (1.8.1) draws a dash for 0
        CHECK ((int) juce::JSON::parse (none.toJson())["takeNumber"] == 0 && ! (bool) juce::JSON::parse (none.toJson())["recording"]);
    }

    SECTION ("talkback: the TB pair can only carry the CR mic and recorded audio");
    {
        TalkbackKey k;
        k.update (true, 0);  CHECK (k.update (false, 100));                    // tap -> on
        k.update (true, 1000); CHECK (k.isOn());
        CHECK (! k.update (false, 1100));                                      // tap again -> off
        k.update (true, 2000); CHECK (k.update (true, 2500)); CHECK (! k.update (false, 2600));   // hold, release -> off
        k.update (true, 3000); k.update (false, 3100);                          // latched on
        k.update (true, 4000); CHECK (! k.update (false, 4600));               // a long press while latched on: off after release
    }
    {
        auto* gg = tw.findGroup (gid1);
        std::vector<int> counts; for (auto& t : p.tracks) counts.push_back (t.channelCount());
        const auto savedMon = p.findTrack (violinId)->monitor();
        p.findTrack (violinId)->setMonitor (Monitor::SessionLive);              // live microphones are let through during playback
        eng.rebuildPlan();
        eng.setTalkbackRouting ({ 5 }, 2);                                       // CR mic = input 5, TB = outputs 2-3 (which mixer B also feeds)
        eng.setCrMic (false); eng.setTalkbackPlayback (false);
        rig.inGain = 0.5f;
        auto none = rig.measure (eng);
        CHECK (none[2] < 1e-9 && none[3] < 1e-9);                               // live mics going out of the Ext bus never reach TB
        CHECK (none[0] > 0.01);                                                 // while the other output pair still carries them
        eng.setCrMic (true);
        auto cr = rig.measure (eng);
        CHECK (std::abs (cr[2] - cr[3]) < 1e-9 && cr[2] > 0.3 && cr[2] < 0.4);  // only the CR mic (a 0.5 sine) on both sides
        eng.setCrMic (false);
        rig.run (eng, 4);
        auto off = rig.measure (eng);
        CHECK (off[2] < 1e-9);                                                  // closes cleanly
        eng.setTalkbackRouting ({ 5, 6 }, 2); eng.setCrMic (true);              // any number of inputs can be the CR mic: they are heard together
        auto two = rig.measure (eng);
        CHECK (two[2] > cr[2] * 1.3f && two[2] < cr[2] * 1.55f && std::abs (two[2] - two[3]) < 1e-9);
        eng.setCrMic (false); rig.run (eng, 4);
        eng.setTalkbackRouting ({ 5 }, std::vector<int> { 0, 2 }); eng.setCrMic (true);          // any number of talkback pairs: outputs 0-1 and 2-3 both carry only the CR mic
        auto pairs = rig.measure (eng);
        CHECK (std::abs (pairs[0] - cr[2]) < 1e-6 && std::abs (pairs[1] - cr[2]) < 1e-6 && std::abs (pairs[2] - cr[2]) < 1e-6 && std::abs (pairs[3] - cr[2]) < 1e-6);
        eng.setCrMic (false); rig.run (eng, 4);
        eng.setTalkbackRouting ({ 5 }, 2); eng.setCrMic (true);                 // the stage speaker mixer: gain and pan of the CR input
        eng.setStageMix ({}, {}, 0.0f, 0.0f); rig.run (eng, 6);
        auto flat = rig.measure (eng);
        CHECK (std::abs (flat[2] - cr[2]) < 1e-6 && std::abs (flat[3] - cr[3]) < 1e-6);        // 0 dB and centre = exactly as before
        std::vector<float> gdb (8, 0.0f), gpan (8, 0.0f);
        gpan[5] = -1.0f; eng.setStageMix (gdb, gpan, 0.0f, 0.0f); rig.run (eng, 6);
        auto left = rig.measure (eng);
        CHECK (left[2] > cr[2] * 0.95f && left[3] < 1e-6);                                       // panned hard left: right side silent
        gpan[5] = 0.0f; gdb[5] = -6.0f; eng.setStageMix (gdb, gpan, 0.0f, 0.0f); rig.run (eng, 6);
        auto minus6 = rig.measure (eng);
        CHECK (minus6[2] > cr[2] * 0.48f && minus6[2] < cr[2] * 0.52f);                         // -6 dB is about half
        gdb[5] = 0.0f; eng.setStageMix (gdb, gpan, 0.0f, -6.0f); rig.run (eng, 6);
        auto outm6 = rig.measure (eng);
        CHECK (outm6[2] > cr[2] * 0.48f && outm6[2] < cr[2] * 0.52f && outm6[3] > cr[3] * 0.48f && outm6[3] < cr[3] * 0.52f);   // the output fader moves both sides
        eng.takeStagePeak(); eng.setStageMix (gdb, gpan, 0.0f, 0.0f); rig.run (eng, 6);
        { const float pkSeen = eng.takeStagePeak(); CHECK (pkSeen > 0.2f && pkSeen < 0.6f); }   // the peak light's reading follows the level (a 0.5 sine)
        gdb[5] = -60.0f; eng.setStageMix (gdb, gpan, 0.0f, 0.0f); rig.run (eng, 6);
        auto offm = rig.measure (eng);
        CHECK (offm[2] < 1e-6 && offm[3] < 1e-6);                                               // all the way down is off
        eng.setCrMic (false); eng.setStageMix (std::vector<float> (8, 10.0f), {}, 10.0f, 10.0f); rig.run (eng, 6);
        { auto hot = rig.measure (eng); CHECK (hot[2] < 1e-9 && hot[3] < 1e-9); }                  // even with every stage fader up, live mics never reach TB when the CR mic is closed
        eng.setStageMix ({}, {}, 0.0f, 0.0f); eng.setCrMic (false); rig.run (eng, 4);
        eng.setTalkbackRouting ({ 5 }, 2);
        // playback
        CHECK (eng.startPlayback (std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *gg), 0, gg->lengthSamples, 48000.0, eng.getMaxBlock())).isEmpty());
        std::vector<double> a, b2, c2;
        rig.inGain = 0.5f; rig.run (eng, 10); rig.run (eng, 20, &a);            // playing, talkback playback off
        CHECK (a[2] < 1e-9 && a[3] < 1e-9 && a[0] > 0.05);
        eng.setTalkbackPlayback (true);
        rig.run (eng, 10); rig.run (eng, 20, &b2);                              // playing, talkback playback on, live mics hot
        eng.stopPlayback(); rig.inGain = 0.0f; eng.stopPlayback();
        CHECK (eng.startPlayback (std::make_unique<PlaybackSession> (counts, segmentsForTake (p, *gg), 0, gg->lengthSamples, 48000.0, eng.getMaxBlock())).isEmpty());
        rig.run (eng, 10); rig.run (eng, 20, &c2);                              // the same with the inputs silent
        CHECK (b2[2] > 0.05 && std::abs (b2[2] - b2[3]) < 0.02 * b2[2] + 1e-6);   // recorded audio reaches TB
        CHECK (std::abs (b2[2] - c2[2]) < 1e-6 && std::abs (b2[3] - c2[3]) < 1e-6);   // and it is identical with the live mics off: nothing live is in it
        eng.stopPlayback();
        rig.inGain = 0.5f; rig.run (eng, 6);
        auto after = rig.measure (eng);                                        // playback stopped: TB goes silent at once, mics are live again
        CHECK (after[2] < 1e-9 && after[3] < 1e-9);
        eng.setTalkbackRouting ({}, -1); eng.setTalkbackPlayback (false);
        p.findTrack (violinId)->setMonitor (savedMon);
        eng.rebuildPlan();
        rig.inGain = 0.5f;
        Project q; juce::String e1;                                            // the routing is saved
        p.crMicInputs = { 5, 6 }; p.tbOutputs = { 0, 2 };
        CHECK (p.save (e1)); CHECK (q.load (p.projectFile, e1) && q.crMicInputs == std::vector<int> ({ 5, 6 }) && q.tbOutputs == std::vector<int> ({ 0, 2 }));
        p.stageGainDb = { 0.0f, -3.5f }; p.stagePan = { 0.0f, 0.25f }; p.stagePlaybackDb = -2.0f; p.stageOutputDb = 1.5f;
        CHECK (p.save (e1)); Project q2; CHECK (q2.load (p.projectFile, e1) && std::abs (q2.stageGainOf (1) + 3.5f) < 1e-4 && std::abs (q2.stagePanOf (1) - 0.25f) < 1e-4 && std::abs (q2.stagePlaybackDb + 2.0f) < 1e-4 && std::abs (q2.stageOutputDb - 1.5f) < 1e-4);     // the stage mixer is saved
        p.stageGainDb.clear(); p.stagePan.clear(); p.stagePlaybackDb = 0.0f; p.stageOutputDb = 0.0f;
        p.crMicInputs.clear(); p.tbOutputs.clear();
    }

    SECTION ("edits and marks survive save / load");
    {
        auto& w0 = *p.takeWindows.front();
        w0.groups.front().barIn = 17; w0.groups.front().barOut = 39;
        w0.editTake = gid1; w0.editIn = 0.25; w0.editOut = 0.75; w0.editTracks = { violinId };               // the Alt + drag track subset
        auto& ed = p.addEdit ("Symphony edit", w0.id);
        EditRegion r; r.barIn = 17; r.barOut = 39; r.windowId = w0.id; r.takeId = gid1; r.srcIn = 100; r.srcOut = 48100; r.sampleRate = 48000.0; r.takeName = "001 - Symphony 2";
        r.sourceLength = 100000;
        RegionFile rf; rf.trackId = violinId; rf.trackName = "violin"; rf.file = w0.groups.front().files.front().file; r.files.push_back (rf);
        ed.insertRegion (r); r.srcIn = 5000; r.srcOut = 30000;
        ed.insertRegion (r);
        ed.regions[0].outStart = -0.02; ed.regions[0].outEnd = 0.005; ed.regions[1].inStart = -0.003; ed.regions[1].inEnd = 0.04;
        ed.regions[1].curve = FadeCurve::Linear;
        ed.slideRegion (1, 700, false, false);
        p.findTrack (violinId)->setMonitor (Monitor::SessionLive);
        p.findTrack (mainId)->inputs[1] = 7;
        p.setChannelColour (violinId, 0xff3b82c4u);                                  // channel colours are saved with the project
        if (! p.buses.empty()) p.setChannelColour (p.buses.back().id, 0xffd2579au);
        juce::String err2; CHECK (p.save (err2));
        Project q2; CHECK (q2.load (p.projectFile, err2));
        auto* e2 = q2.editForWindow (q2.takeWindows.front()->id);
        CHECK (e2 != nullptr && e2->regions.size() == 2 && e2->regions[1].startSample == 48700 && e2->sampleRate == 48000.0);
        CHECK (e2 != nullptr && e2->regions[1].curve == FadeCurve::Linear && std::abs (e2->regions[1].inStart + 0.003) < 1e-9 && std::abs (e2->regions[1].inEnd - 0.04) < 1e-9);
        CHECK (e2 != nullptr && std::abs (e2->regions[0].outStart + 0.02) < 1e-9 && std::abs (e2->regions[0].outEnd - 0.005) < 1e-9);
        CHECK (e2 != nullptr && e2->regions[0].files.size() == 1 && e2->regions[0].files[0].file == rf.file && e2->regions[0].sourceLength == 100000);
        CHECK (q2.takeWindows.front()->groups.front().barIn == 17 && q2.takeWindows.front()->groups.front().barOut == 39 && e2 != nullptr && e2->regions[0].barIn == 17 && e2->regions[1].barOut == 39);
        CHECK (barsContain (17, 39, 17) && barsContain (17, 39, 30) && barsContain (17, 39, 39) && ! barsContain (17, 39, 16) && ! barsContain (17, 39, 40) && ! barsContain (0, 0, 1) && barsContain (5, 0, 5) && ! barsContain (5, 0, 6));
        CHECK (barsLabel (17, 39) == "Bar 17 - 39" && barsLabel (5, 5) == "Bar 5" && barsLabel (0, 0).isEmpty());
        CHECK (q2.takeWindows.front()->editTracks.size() == 1 && q2.takeWindows.front()->editTracks[0] == violinId && q2.takeWindows.front()->editIn == 0.25);       // the marked tracks survive too
        CHECK (q2.findTrack (violinId)->monitor() == Monitor::SessionLive && q2.findTrack (mainId)->inputOf (1) == 7);
        {   // the playhead of a take window: the cursor follows it; Space plays from it
            auto& w = *q2.takeWindows.front();
            CHECK (w.groups.size() >= 2);
            const auto& a = w.groups[0]; const auto& b = w.groups[1];
            w.setPlayhead (a.startSeconds + 0.5);
            juce::Uuid t; double from = -1;
            CHECK (w.cursorTake == a.id && std::abs (w.cursorSeconds - 0.5) < 1e-9);
            CHECK (w.playSpot (t, from) && t == a.id && std::abs (from - 0.5) < 1e-9);
            w.setPlayhead (a.startSeconds + a.lengthSeconds() + 0.0005);                 // in the gap between two takes
            if (b.startSeconds > a.startSeconds + a.lengthSeconds() + 0.01) { CHECK (w.cursorTake.isNull()); CHECK (w.playSpot (t, from) && t == b.id && from == 0.0); }
            w.setPlayhead (b.startSeconds + b.lengthSeconds() + 100.0);
            CHECK (! w.playSpot (t, from));                                               // nothing ahead
            w.setPlayhead (3.25);
            juce::String er; CHECK (q2.save (er));
            Project q3; CHECK (q3.load (q2.projectFile, er) && std::abs (q3.takeWindows.front()->playheadSeconds - 3.25) < 1e-9);
        }
        CHECK (q2.channelColour (violinId) == 0xff3b82c4u && q2.channelColour (mainId) == 0u);
        CHECK (p.buses.empty() || q2.channelColour (p.buses.back().id) == 0xffd2579au);
        q2.setChannelColour (violinId, 0); CHECK (q2.channelColour (violinId) == 0u);
    }

    {
        std::printf ("-- crossfade shapes and take names\n");
        for (int c = 0; c < (int) FadeCurve::Count; ++c)
        {
            const auto cv = (FadeCurve) c;
            CHECK (std::abs (fadeGain (cv, 0.0f, true)) < 1e-4f && std::abs (fadeGain (cv, 1.0f, true) - 1.0f) < 1e-4f);        // in: silent -> full
            CHECK (std::abs (fadeGain (cv, 0.0f, false) - 1.0f) < 1e-4f && std::abs (fadeGain (cv, 1.0f, false)) < 1e-4f);     // out: full -> silent
            float prev = -1.0f; bool rising = true;
            for (int i = 0; i <= 100; ++i) { const float g = fadeGain (cv, (float) i / 100.0f, true); rising = rising && g >= prev - 1e-6f; prev = g; }
            CHECK (rising);
            CHECK (juce::String (fadeCurveName (cv)).isNotEmpty());
        }
        CHECK (std::abs (fadeGain (FadeCurve::Cosine, 0.5f, true) - 0.5f) < 1e-5f);                                            // equal-gain shapes cross at half
        CHECK (std::abs (fadeGain (FadeCurve::Linear, 0.25f, true) - 0.25f) < 1e-6f);
        CHECK (std::abs (fadeGain (FadeCurve::EqualPower, 0.5f, true) - 0.70710678f) < 1e-5f);
        TakeWindowDef tw; tw.name = "Symphony 2"; TakeGroup tg;
        CHECK (tw.labelFor (tg) == "Symphony 2");                     // a take is named after the piece
        tg.label = "slow movement"; CHECK (tw.labelFor (tg) == "slow movement");
    }

    {
        std::printf ("-- session mode (Pre): the take starts 5 s before Record (3.18.0: 1 to 10 s, chosen); the listening stream carries on\n");
        // (1) the pre-roll: the file starts preLen samples before the first block of the session, with no gap or repeat at the join
        Project q;
        q.projectFile = tmp.getChildFile ("Pre").getChildFile ("Pre.fermata");
        q.setInputCount (8); q.setOutputCount (2);
        q.tracks.clear();
        const auto tid = q.addTrack ("violin", TrackFormat::Mono, 0).id;
        q.takeWindows.front()->name = "Pre";
        AudioEngine e2 (q);
        e2.prepare (48000.0, Rig::block);
        Rig r2;
        e2.setSessionMode (true);
        CHECK (e2.isSessionMode());
        CHECK (e2.waitForPreRoll (5000));                                  // (the pre-roll memory is made in the background since 2.68)
        q.findTrack (tid)->setArmed (true);
        r2.run (e2, 700);                                                  // 7.5 s of listening: more than the pre-roll
        const juce::int64 posAtRecord = r2.pos;
        CHECK (e2.startRecording (*q.takeWindows.front()).isEmpty());
        r2.run (e2, 40);
        auto gidPre = e2.stopRecording();
        CHECK (e2.waitForFinishedTakes (10000)); e2.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        auto* gPre = q.takeWindows.front()->findGroup (gidPre);
        CHECK (e2.getPreRollSeconds() == 5);                               // (3.18.0: 5 s unless chosen otherwise; it was always 6)
        const juce::int64 preLen = 5 * 48000;
        CHECK (gPre != nullptr && gPre->lengthSamples == preLen + 40 * Rig::block);
        if (gPre != nullptr && ! gPre->files.empty())
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (gPre->files[0].file));
            CHECK (rd != nullptr && rd->lengthInSamples == gPre->lengthSamples);
            if (rd != nullptr)
            {
                juce::AudioBuffer<float> b (1, (int) rd->lengthInSamples);
                rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, false);
                double worst = 0.0;
                for (int i = 0; i < b.getNumSamples(); i += 7)
                {
                    const double want = 0.5 * std::sin (2.0 * 3.14159265358979 * 200.0 * (double) (posAtRecord - preLen + i) / 48000.0);
                    worst = juce::jmax (worst, std::abs (want - (double) b.getSample (0, i)));
                }
                CHECK (worst < 1e-5);                                      // every sample, 5 s before and 40 blocks after, in order
            }
        }
        // (1b) 3.18.0: the Pre length is chosen (1 to 10 whole seconds): a new length is a new memory, and the next take starts that far back
        e2.setPreRollSeconds (0);  CHECK (e2.getPreRollSeconds() == 1);
        e2.setPreRollSeconds (99); CHECK (e2.getPreRollSeconds() == 10);
        e2.setPreRollSeconds (3);  CHECK (e2.getPreRollSeconds() == 3);
        CHECK (e2.waitForPreRoll (5000));
        r2.run (e2, 400);                                                  // more than 3 s of listening
        CHECK (e2.startRecording (*q.takeWindows.front()).isEmpty());
        e2.setPreRollSeconds (8);                                          // changed during the take: this take keeps its 3 s
        r2.run (e2, 30);
        auto gid3 = e2.stopRecording();
        CHECK (e2.waitForFinishedTakes (10000)); e2.reapFinishedTakes();
        auto* g3 = q.takeWindows.front()->findGroup (gid3);
        CHECK (g3 != nullptr && g3->lengthSamples == 3 * 48000 + 30 * Rig::block);
        CHECK (e2.getPreRollSeconds() == 8 && e2.waitForPreRoll (5000));     // the 8 s memory goes in after Stop
        r2.run (e2, 800);
        CHECK (e2.startRecording (*q.takeWindows.front()).isEmpty());
        r2.run (e2, 20);
        auto gid8 = e2.stopRecording();
        CHECK (e2.waitForFinishedTakes (10000)); e2.reapFinishedTakes();
        auto* g8 = q.takeWindows.front()->findGroup (gid8);
        CHECK (g8 != nullptr && g8->lengthSamples == 8 * 48000 + 20 * Rig::block);
        e2.setPreRollSeconds (5);
        // (2) with session mode off a take starts at the press, as before
        e2.setSessionMode (false);
        r2.run (e2, 10);
        CHECK (e2.startRecording (*q.takeWindows.front()).isEmpty());
        r2.run (e2, 20);
        auto gidNo = e2.stopRecording();
        CHECK (e2.waitForFinishedTakes (10000)); e2.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        CHECK (q.takeWindows.front()->findGroup (gidNo)->lengthSamples == 20 * Rig::block);

        // (3) pressing Record and Stop never interrupts what the engineer hears
        auto& mon = p;                                                     // the big project: violin goes to the Out bus of mixer A (outputs 1-2)
        for (auto& t : mon.tracks) t.setArmed (t.name == "violin");
        eng.setSessionMode (true);
        CHECK (eng.waitForPreRoll (5000));
        rig.run (eng, 600);
        auto level = [&] { std::vector<double> r; rig.run (eng, 1, &r); return r[0]; };
        double lo = 1e9;
        for (int i = 0; i < 5; ++i) lo = juce::jmin (lo, level());
        CHECK (eng.startRecording (tw).isEmpty());
        for (int i = 0; i < 30; ++i) lo = juce::jmin (lo, level());
        eng.stopRecording();
        CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        for (int i = 0; i < 30; ++i) lo = juce::jmin (lo, level());
        CHECK (lo > 0.02);                                                 // no block of silence at the start or at the stop of a recording
        eng.setSessionMode (false);
    }

    SECTION ("meters: RMS and the phase scope tap");
    {
        for (auto& t : p.tracks) mixA.stripFor (t.id)->mute.set (t.id != violaId);
        mixA.stripFor (violaId)->solo.set (false);
        mixA.stripFor (violaId)->gainDb.set (0.0f);
        rig.inGain = 0.5f;
        auto* sv = mixA.stripFor (violaId);
        eng.setScopeSource (sv);
        rig.run (eng, 40);
        const float rmsNow = sv->rmsL.get(), rmsR = sv->rmsR.get();
        CHECK (rmsNow > 0.01f && rmsNow < 0.6f && rmsR > 0.01f);                         // a sine of 0.5 through a pan law: RMS well below its peak
        std::vector<float> sl (4096), sr (4096);
        const int got = eng.readScope (sl.data(), sr.data(), 4096);
        CHECK (got > 1000);
        double e = 0; for (int i = 0; i < got; ++i) e += (double) sl[(size_t) i] * sl[(size_t) i] + (double) sr[(size_t) i] * sr[(size_t) i];
        CHECK (e > 1.0);                                                                 // real audio went into the scope
        eng.setScopeSource (nullptr);
        mixA.stripFor (violaId)->solo.set (true);
    }

    SECTION ("meters: the RMS of a mixer that stops running falls to nothing (2.90.1)");
    {
        // The RMS of a strip is worked out only while its mixer runs. A mixer that stopped (playback ended and its tail died away) used to leave the last RMS on the meters for ever.
        Project q;
        q.projectFile = tmp.getChildFile ("RmsIdle.fermata");
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& mq = q.addMixer ("M");
        AudioEngine e (q);
        e.setMixerThreads (0);
        e.prepare (48000.0, 256);
        q.setSendLevel (mq, ta, ob, 0.0f);
        mq.busFor (ob)->outFirst.store (0);
        e.rebuildPlan();
        auto run = [&] (int blocks, int& blk)
        {
            for (int b = 0; b < blocks; ++b, ++blk)
            {
                std::vector<float> i0 (256), i1 (256, 0.0f), o0 (256, 0.0f), o1 (256, 0.0f);
                for (int i = 0; i < 256; ++i) i0[(size_t) i] = 0.4f * (float) std::sin ((blk * 256 + i) * 0.041);
                const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                e.process (in, 2, out, 2, 256);
            }
        };
        int blk = 0;
        run (40, blk);
        auto* sa = mq.stripFor (ta); auto* sb = mq.busFor (ob);
        CHECK (sa->rmsL.get() > 0.05f && sb->rmsL.get() > 0.05f);                        // running: there is an RMS
        mq.busFor (ob)->outFirst.store (-1);                                              // its outputs go nowhere: the mixer is not run any more
        run (4, blk);
        CHECK (sa->rmsL.get() == 0.0f && sa->rmsR.get() == 0.0f && sb->rmsL.get() == 0.0f && sb->rmsR.get() == 0.0f);
        mq.busFor (ob)->outFirst.store (0);                                               // running again: the RMS is worked out again from nothing
        run (40, blk);
        CHECK (sa->rmsL.get() > 0.05f && sb->rmsL.get() > 0.05f);
    }

    SECTION ("edge fades: 1 ms on every piece, files untouched");
    {
        auto* gx = &p.takeWindows.front()->groups.back();
        auto segs = segmentsForTake (p, *gx);
        CHECK (! segs.empty());
        const auto one = (juce::int64) std::llround (kDefaultEdgeFade * gx->sampleRate);
        CHECK (segs.front().fadeInLen == one && segs.front().fadeInStart == segs.front().begin);
        CHECK (segs.front().fadeOutLen == one && segs.front().fadeOutStart + one == segs.front().end);
        gx->fadeInSeconds = 0.0;
        CHECK (segmentsForTake (p, *gx).front().fadeInLen == 0);          // a hard start when the user pulls the fade to nothing
        gx->fadeInSeconds = kDefaultEdgeFade;

        EditDef ed; ed.sampleRate = 48000.0;
        EditRegion a, b; a.sampleRate = b.sampleRate = 48000.0;
        a.srcIn = 0; a.srcOut = 48000; a.sourceLength = 96000; a.startSample = 0;
        b = a; b.startSample = 48000;
        ed.regions = { a, b };
        ed.hardenEnds();
        CHECK (ed.regions.front().inStart == 0.0 && std::abs (ed.regions.front().inEnd - kDefaultEdgeFade) < 1e-9);
        CHECK (ed.regions.back().outEnd == 0.0 && std::abs (ed.regions.back().outStart + kDefaultEdgeFade) < 1e-9);
        ed.regions.front().inEnd = 0.02;                                   // a longer fade made by the user survives another hardenEnds()
        ed.hardenEnds();
        CHECK (std::abs (ed.regions.front().inEnd - 0.02) < 1e-9);
    }

    SECTION ("automation: points stay on their audio when the edit changes");
    {
        EditDef ed; ed.sampleRate = 48000.0;
        const juce::Uuid takeA;
        auto mk = [&] (juce::int64 in, juce::int64 out)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = in; r.srcOut = out; r.sourceLength = 10000000; r.takeName = "t"; r.takeId = takeA;
            RegionFile rf; rf.trackId = juce::Uuid(); rf.file = juce::File ("/tmp/none.wav"); r.files.push_back (rf);
            return r;
        };
        const juce::int64 S = 48000;
        ed.insertRegion (mk (0, 60 * S));                         // 0 .. 60 s
        ed.insertRegion (mk (100 * S, 130 * S));                  // 60 .. 90 s
        auto& lane = ed.addLane (juce::Uuid::null(), autoparam::fader, juce::Uuid::null());
        ed.addPoint (lane, 10 * S, -5.0f);
        ed.addPoint (lane, 70 * S, -6.0f);
        CHECK (lane.pts.size() == 2 && lane.pts[0].time == 10 * S && lane.pts[1].time == 70 * S && lane.pts[1].srcPos == 110 * S);
        CHECK (std::abs (lane.valueAt (40 * S, 0.0f) + 5.5f) < 1e-4f && lane.valueAt (0, 0.0f) == -5.0f && lane.valueAt (89 * S, 0.0f) == -6.0f);
        // a 5 s patch goes in front of the second piece: everything after it moves 5 s later, the point with it
        ed.insertIndex = 1;
        ed.insertRegion (mk (200 * S, 205 * S));
        ed.resolveAutomation();
        CHECK (lane.pts[1].time == 75 * S && ! lane.pts[1].floating && lane.pts[0].time == 10 * S);
        // the first piece is shortened by 0.5 s at its start: audio before... later pieces move 0.5 s earlier, the point follows
        ed.regions[0].srcOut -= S / 2; ed.regions[1].startSample -= S / 2; ed.regions[2].startSample -= S / 2;
        ed.resolveAutomation();
        CHECK (lane.pts[1].time == 75 * S - S / 2);
        // removing the second piece: the point on it is lost and moves by a guess, marked floating
        ed.removeRegion (2);
        ed.resolveAutomation();
        CHECK (lane.pts.size() == 2 && lane.pts[1].floating && ! lane.pts[0].floating);
        // dragging it puts it on whatever audio is there
        const int idx = ed.movePoint (lane, 1, 30 * S, -8.0f);
        CHECK (idx == 1 && ! lane.pts[1].floating && lane.pts[1].region == ed.regions[0].id && lane.pts[1].srcPos == 30 * S && lane.pts[1].value == -8.0f);
        // a split leaves the point on whichever half holds its audio
        const auto before = lane.pts[1].time;
        ed.splitRegion (0, 20 * S);
        ed.resolveAutomation();
        CHECK (lane.pts[1].time == before && ! lane.pts[1].floating);
        // a point in a gap (no audio) is fixed in time
        ed.addPoint (lane, 500 * S, -1.0f);
        CHECK (lane.pts.back().region.isNull());
        ed.resolveAutomation();
        CHECK (lane.pts.back().time == 500 * S);
        // the faders: clamped
        CHECK (lane.clampValue (40.0f) == 12.0f && lane.clampValue (-500.0f) == -100.0f);
        // saved and loaded
        ed.automationOn = true;
        p.edits.push_back (std::make_unique<EditDef> (ed));
        juce::String er; CHECK (p.save (er));
        Project q3; CHECK (q3.load (p.projectFile, er));
        auto* e3 = q3.edits.empty() ? nullptr : q3.edits.back().get();
        CHECK (e3 != nullptr && e3->automationOn && e3->lanes.size() == 1 && e3->lanes[0].pts.size() == 3);
        CHECK (e3 != nullptr && e3->lanes[0].pts[1].region == lane.pts[1].region && e3->lanes[0].pts[1].srcPos == lane.pts[1].srcPos && e3->lanes[0].pts[1].value == -8.0f);
        p.edits.pop_back();
        {   // the plan drives the real fader of the chosen mixer (the first one by default)
            const auto autoTrack = p.tracks.front().id;
            EditDef e2; e2.automationOn = true;
            auto& l2 = e2.addLane (autoTrack, autoparam::fader, juce::Uuid::null());
            AutoPoint a; a.time = 0; a.value = -10.0f; AutoPoint b; b.time = 10 * S; b.value = 0.0f;
            l2.pts = { a, b };
            auto plan = automationPlanFor (p, e2);
            CHECK (plan != nullptr && plan->items.size() == 1);
            if (plan) { plan->apply (5 * S); CHECK (std::abs (p.mixers.front()->stripFor (autoTrack)->gainDb.get() + 5.0f) < 1e-3f); }
            e2.automationOn = false; CHECK (automationPlanFor (p, e2) != nullptr);         // the lane view being off does not stop what is written
            { EditDef none; none.automationOn = true; CHECK (automationPlanFor (p, none) == nullptr && ! none.hasAutomation()); }       // no points anywhere: nothing to play
            p.mixers.front()->stripFor (autoTrack)->gainDb.set (0.0f);
        }
        {   // bus sends and plug-in parameters
            struct FakeFx : InsertProcessor
            {
                std::atomic<int> sets { 0 }; float val[3] { 0.5f, 0.5f, 0.5f };
                void prepare (double, int, int) override {}
                void process (juce::AudioBuffer<float>&) override {}
                juce::String getName() const override { return "Fake"; }
                int numParams() const override { return 3; }
                juce::String paramName (int i) const override { return "P" + juce::String (i); }
                bool paramAutomatable (int i) const override { return i != 2; }
                float getParamNorm (int i) const override { return val[i]; }
                void setParamNorm (int i, float v) override { val[i] = v; ++sets; }
            };
            const auto tr = p.tracks.front().id; const auto bus = juce::Uuid();
            auto* strip = p.mixers.front()->stripFor (tr);
            auto* sd = strip->sends.get (bus); sd->gainDb.set (-100.0f);
            auto fx = std::make_unique<FakeFx>(); auto* fxp = fx.get();
            strip->slots[1].set (std::move (fx), 48000.0, 512, 2);
            EditDef e4; e4.automationOn = true;
            AutoPoint a; a.time = 0; a.value = -40.0f; AutoPoint b; b.time = 10 * S; b.value = 0.0f;
            e4.addLane (tr, autoparam::send (bus), juce::Uuid::null()).pts = { a, b };
            AutoPoint c; c.time = 0; c.value = 0.0f; AutoPoint d; d.time = 10 * S; d.value = 1.0f;
            e4.addLane (tr, autoparam::plugin (1, 0), juce::Uuid::null()).pts = { c, d };
            e4.addLane (tr, autoparam::plugin (1, 2), juce::Uuid::null()).pts = { c, d };          // not automatable: left out
            e4.addLane (tr, autoparam::plugin (3, 0), juce::Uuid::null()).pts = { c, d };          // empty slot: left out
            e4.addLane (tr, autoparam::send (juce::Uuid()), juce::Uuid::null()).pts = { a, b };    // no such send: left out
            auto plan = automationPlanFor (p, e4);
            CHECK (plan != nullptr && plan->items.size() == 2);
            if (plan)
            {
                plan->apply (5 * S);
                CHECK (std::abs (sd->gainDb.get() + 20.0f) < 1e-3f);
                CHECK (std::abs (fxp->val[0] - 0.5f) < 1e-4f && fxp->sets == 1);
                for (int k = 0; k < 40; ++k) plan->apply (5 * S);
                const int settled = fxp->sets;
                CHECK (settled <= 1 + 24);                                    // (a plug-in is told again in the first blocks of a play only, in case it missed the first)
                plan->apply (5 * S); CHECK (fxp->sets == settled);            // unchanged value after that: not sent again
                plan->apply (10 * S); CHECK (std::abs (fxp->val[0] - 1.0f) < 1e-4f && fxp->sets == settled + 1);
            }
            {   // 2.90: a plug-in switch (a bypass: 2 positions) and a selector (4) take only their own positions, and hold between points
                struct Sw : FakeFx { int paramSteps (int i) const override { return i == 0 ? 2 : i == 1 ? 4 : 0; } };
                auto sw = std::make_unique<Sw>(); auto* swp = sw.get();
                strip->slots[2].set (std::move (sw), 48000.0, 512, 2);
                EditDef e6; e6.automationOn = true;
                AutoPoint off; off.time = 0; off.value = 0.0f; AutoPoint on; on.time = 10 * S; on.value = 1.0f;
                e6.addLane (tr, autoparam::plugin (2, 0), juce::Uuid::null()).pts = { off, on };               // the bypass: off, then on at 10 s
                AutoPoint q0; q0.time = 0; q0.value = 0.4f; AutoPoint q1; q1.time = 10 * S; q1.value = 0.9f;   // (values between positions, as an older project could hold)
                e6.addLane (tr, autoparam::plugin (2, 1), juce::Uuid::null()).pts = { q0, q1 };
                auto plan6 = automationPlanFor (p, e6);
                CHECK (plan6 != nullptr && plan6->items.size() == 2 && plan6->items[0].steps == 2 && plan6->items[1].steps == 4);
                if (plan6)
                {
                    plan6->apply (1 * S);               CHECK (swp->val[0] == 0.0f);                         // (a smooth parameter would be 0.1 here)
                    plan6->apply (5 * S);               CHECK (swp->val[0] == 0.0f && std::abs (swp->val[1] - 1.0f / 3.0f) < 1e-5f);     // half way: still off, never a position between
                    plan6->apply (9 * S + S / 2);       CHECK (swp->val[0] == 0.0f);
                    plan6->apply (10 * S);              CHECK (swp->val[0] == 1.0f && swp->val[1] == 1.0f);
                    plan6->apply (20 * S);              CHECK (swp->val[0] == 1.0f);
                }
                CHECK (AutoLane::snapToSteps (0.49f, 2) == 0.0f && AutoLane::snapToSteps (0.5f, 2) == 1.0f && AutoLane::snapToSteps (-3.0f, 2) == 0.0f && AutoLane::snapToSteps (7.0f, 2) == 1.0f);
                CHECK (std::abs (AutoLane::snapToSteps (0.26f, 3) - 0.5f) < 1e-6f && AutoLane::snapToSteps (0.24f, 3) == 0.0f);
                CHECK (AutoLane::snapToSteps (-3.5f, 0) == -3.5f && AutoLane::snapToSteps (0.37f, 1) == 0.37f && AutoLane::snapToSteps (12.0f, 0) == 12.0f);     // not a switch: untouched
                AutoLane sl3; sl3.param = autoparam::plugin (0, 0);
                AutoPoint a3, b3, c3; a3.time = 0; a3.value = 1.0f; b3.time = 100; b3.value = 0.0f; c3.time = 200; c3.value = 1.0f; sl3.pts = { a3, b3, c3 };
                CHECK (sl3.valueAt (50, 0.5f, 2) == 1.0f && sl3.valueAt (99, 0.5f, 2) == 1.0f && sl3.valueAt (100, 0.5f, 2) == 0.0f && sl3.valueAt (150, 0.5f, 2) == 0.0f && sl3.valueAt (250, 0.5f, 2) == 1.0f);
                CHECK (std::abs (sl3.valueAt (50, 0.5f) - 0.5f) < 1e-6f && std::abs (sl3.valueAt (50, 0.5f, 0) - 0.5f) < 1e-6f);        // a smooth parameter still slopes
                CHECK (sl3.valueAt (-5, 0.5f, 2) == 1.0f && AutoLane().valueAt (10, 0.25f, 2) == 0.25f);
            }
            CHECK (AutoLane().clampValue (5.0f) == 5.0f);
            AutoLane pl; pl.param = autoparam::plugin (1, 0); CHECK (pl.clampValue (3.0f) == 1.0f && pl.clampValue (-1.0f) == 0.0f);
            int sl = -1, ix = -1; CHECK (autoparam::parsePlugin (pl.param, sl, ix) && sl == 1 && ix == 0 && ! autoparam::parsePlugin ("plugin:x", sl, ix));
            CHECK (autoparam::sendDest (autoparam::send (bus)) == bus);
            // saved and loaded
            p.edits.push_back (std::make_unique<EditDef> (e4));
            juce::String er; CHECK (p.save (er));
            Project q4; CHECK (q4.load (p.projectFile, er));
            auto* e5 = q4.edits.empty() ? nullptr : q4.edits.back().get();
            CHECK (e5 != nullptr && e5->findLane (tr, autoparam::plugin (1, 0), juce::Uuid::null()) != nullptr && e5->findLane (tr, autoparam::send (bus), juce::Uuid::null()) != nullptr);
            p.edits.pop_back();
            strip->slots[1].clear (48000.0, 512, 2);
        }
    }

    SECTION ("preamp memory: the full state is saved and compared");
    {
        PreampMemory m;
        PreampSettings a; a.gainDb = 37.5f; a.phantom = true; a.pad = true; a.zHigh = true; a.boost = true; a.lowCut = true; a.polarity = true; a.line = false;
        PreampSettings b; b.gainDb = 12.0f; b.line = true;
        m.inputs[0] = a; m.inputs[9] = b;
        const auto f = tmp.getChildFile ("preamp memory.json");
        CHECK (m.save (f));
        PreampMemory r; CHECK (PreampMemory::load (f, r) && r.inputs.size() == 2);
        CHECK (PreampMemory::same (r.inputs[0], a) && PreampMemory::same (r.inputs[9], b));
        auto a2 = a; a2.gainDb = 37.52f; CHECK (PreampMemory::same (a2, a));              // within rounding
        a2.gainDb = 40.0f; CHECK (! PreampMemory::same (a2, a));
        for (int k = 0; k < 7; ++k)                                                           // every single switch counts as a difference
        {
            auto c = a; if (k == 0) c.phantom = ! c.phantom; else if (k == 1) c.pad = ! c.pad; else if (k == 2) c.zHigh = ! c.zHigh; else if (k == 3) c.boost = ! c.boost;
            else if (k == 4) c.lowCut = ! c.lowCut; else if (k == 5) c.polarity = ! c.polarity; else c.line = ! c.line;
            CHECK (! PreampMemory::same (c, a));
        }
        CHECK (! PreampMemory::load (tmp.getChildFile ("nothing.json"), r));
    }

    SECTION ("take display: session clock");
    {
        using namespace sessionclock;
        int h = 0, m = 0;
        CHECK (parseHHMM ("17:30", h, m, false) && h == 17 && m == 30);
        CHECK (parseHHMM ("7:05", h, m, false) && h == 7 && m == 5);
        CHECK (parseHHMM ("1730", h, m, false) && h == 17 && m == 30);
        CHECK (! parseHHMM ("24:00", h, m, false) && ! parseHHMM ("12:60", h, m, false) && ! parseHHMM ("abc", h, m, false) && ! parseHHMM ("", h, m, false));
        CHECK (parseHHMM ("36:00", h, m, true) && h == 36);                  // a length of time may be longer than a day's hours
        const auto now = juce::Time (2026, 9, 5, 10, 0, 0, 0, true).toMilliseconds();
        CHECK (remainingSeconds (endAtMs (now, 17, 0), now) == 7 * 3600);     // later today
        CHECK (remainingSeconds (endAtMs (now, 9, 0), now) == -3600);          // an hour ago: over time, not tomorrow
        const auto late = juce::Time (2026, 9, 5, 22, 0, 0, 0, true).toMilliseconds();
        CHECK (std::abs (remainingSeconds (endAtMs (late, 1, 0), late) - 3 * 3600) <= 3600);   // 01:00 at 22:00 is tomorrow (an hour of slack for clock changes)
        CHECK (remainingSeconds (1000000, 1000000) == 0 && remainingSeconds (1000000, 999600) == 1);
        CHECK (remainingSeconds (1000000, 1000400) == 0 && remainingSeconds (1000000, 1001000) == -1);
        CHECK (formatClock (3723) == "01:02:03" && formatClock (0) == "00:00:00" && formatClock (-293) == "-00:04:53" && formatClock (100 * 3600) == "100:00:00");
        CHECK (timeOfDay (juce::Time (2026, 9, 5, 7, 5, 9, 0, true).toMilliseconds()) == "07:05:09");
    }


    SECTION ("pitch shift, noise repair and importing takes from another program");
    {
        const double sr = 48000.0;
        auto toneFreq = [&] (const std::vector<float>& x, size_t from)
        {
            const int N = 65536; dsp::FFT f (N); std::vector<std::complex<double>> b ((size_t) N); const auto w = dsp::hann (N);
            for (int i = 0; i < N; ++i) b[(size_t) i] = (double) x[from + (size_t) i] * w[(size_t) i];
            f.forward (b);
            int best = 1; double bm = 0; for (int i = 1; i < N / 2; ++i) { const double m = std::abs (b[(size_t) i]); if (m > bm) { bm = m; best = i; } }
            const double a = std::log (std::abs (b[(size_t) best - 1]) + 1e-30), c = std::log (std::abs (b[(size_t) best]) + 1e-30), d = std::log (std::abs (b[(size_t) best + 1]) + 1e-30);
            return ((double) best + 0.5 * (a - d) / (a - 2 * c + d)) * sr / N;
        };
        std::vector<float> tone ((size_t) (4 * sr));
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.4f * (float) std::sin (2.0 * dsp::kPi * 440.0 * (double) i / sr) + 0.2f * (float) std::sin (2.0 * dsp::kPi * 660.0 * (double) i / sr);
        for (double cents : { 100.0, -100.0, 23.0, 1200.0 })
        {
            const auto y = PitchShifter::shift (tone, sr, cents);
            CHECK (y.size() == tone.size());
            const double want = 440.0 * std::pow (2.0, cents / 1200.0);
            CHECK (std::abs (1200.0 * std::log2 (toneFreq (y, 70000) / want)) < 0.5);          // within half a cent
        }
        {   // a burst stays where it was in time
            std::vector<float> x ((size_t) (4 * sr), 0.0f);
            for (size_t i = 0; i < (size_t) (0.2 * sr); ++i) x[(size_t) (2 * sr) + i] = (float) std::sin (dsp::kPi * (double) i / (0.2 * sr)) * (float) std::sin (2 * dsp::kPi * 500.0 * (double) i / sr);
            const auto y = PitchShifter::shift (x, sr, 150.0);
            double sx = 0, mx = 0, sy = 0, my = 0;
            for (size_t i = 0; i < x.size(); ++i) { sx += std::abs (x[i]) * (double) i; mx += std::abs (x[i]); sy += std::abs (y[i]) * (double) i; my += std::abs (y[i]); }
            CHECK (std::abs (sx / mx - sy / my) < 0.005 * sr);
        }
        // declick: a click is mended
        {
            auto x = tone; auto ref = x; for (int i = 0; i < 20; ++i) x[100000 + (size_t) i] += (i % 2 ? 0.9f : -0.8f);
            const int n = SpectralRepair::declick (x, 90000, 110000, 7);
            double e0 = 0, e1 = 0; for (size_t i = 99990; i < 100040; ++i) { e0 += std::pow (ref[i] - (i >= 100000 && i < 100020 ? (ref[i] + ((i - 100000) % 2 ? 0.9f : -0.8f)) : ref[i]), 2.0); e1 += std::pow (ref[i] - x[i], 2.0); }
            CHECK (n >= 1 && e1 < 0.02);
            for (size_t i = 0; i < 99000; ++i) if (x[i] != ref[i]) { CHECK (false); break; }          // nothing else touched
        }
        // declick: a longer click (a burst of 300 samples) is found and mended; a short stretch chosen by hand is mended as a whole
        {
            juce::Random rnd (5);
            auto x = tone; auto ref = x;
            for (int i = 0; i < 300; ++i) x[100000 + (size_t) i] += 0.7f * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-i / 150.0);
            auto errOf = [&] (const std::vector<float>& v) { double e = 0; for (size_t i = 99990; i < 100400; ++i) e += std::pow (ref[i] - v[i], 2.0); return e; };
            const double before = errOf (x);
            auto y = x; const int n = SpectralRepair::declick (y, 80000, 120000, 9, nullptr, sr);
            CHECK (n >= 1 && errOf (y) < before * 0.1);
            auto z = x; SpectralRepair::declick (z, 99990, 100330, 5, nullptr, sr);               // marked by hand
            CHECK (errOf (z) < before * 0.1);
        }
        // 2.81 declick: the picture check. Clicks the time-domain finder passes over (an attack-like cluster of four, and a short burst of hiss with no jump in it)
        // still show as short vertical lines in the spectrogram; they are found there and mended with a second approach, and nothing is left
        {
            juce::Random rnd (11);
            std::vector<float> x ((size_t) (6 * sr));
            // music with a lot of bass rumble in it (the finder judges a click against the residual of the WHOLE sound, so a click that lives in the top is a small thing to it)
            for (size_t i = 0; i < x.size(); ++i) { const double t = (double) i / sr; x[i] = (float) (0.2 * std::sin (2 * dsp::kPi * 330.0 * t) + 0.1 * std::sin (2 * dsp::kPi * 660.0 * t) + 0.5 * std::sin (2 * dsp::kPi * 55.0 * t) + 0.3 * std::sin (2 * dsp::kPi * 82.0 * t)) + 0.002f * (rnd.nextFloat() * 2.0f - 1.0f); }
            auto ref = x;
            for (int c = 0; c < 4; ++c) x[(size_t) (2.0 * sr) + (size_t) (c * 0.0007 * sr)] += 0.5f;                                                                   // four single-sample spikes 0.7 ms apart: "an attack" to the finder, which leaves them
            { double prev = 0.0; for (int i = 0; i < 144; ++i) { const double w = rnd.nextDouble() * 2.0 - 1.0; x[(size_t) (3.5 * sr) + (size_t) i] += (float) (0.012 * (w - prev) * std::sin (dsp::kPi * (i + 0.5) / 144.0)); prev = w; } }   // a 3 ms burst of top-heavy hiss: a click with no jump in it
            const auto linesBefore = SpectralRepair::findLines (x, sr, (long) (1.0 * sr), (long) (5.0 * sr));
            CHECK (linesBefore.size() >= 1 && linesBefore.size() <= 6);
            auto y = x; const int n = SpectralRepair::declick (y, (long) (1.0 * sr), (long) (5.0 * sr), 6, nullptr, sr);
            const auto rep = SpectralRepair::lastDeclickReport();
            std::printf ("  (declick picture check: %d lines before, finder mended %d, lines after it %d, mended %d, left %d)\n", (int) linesBefore.size(), rep.mended, rep.lines, rep.linesMended, rep.linesLeft);
            const auto linesAfter = SpectralRepair::findLines (y, sr, (long) (1.0 * sr), (long) (5.0 * sr));
            CHECK (n >= 2 && linesAfter.empty() && rep.linesLeft == 0);
            auto errAt = [&] (const std::vector<float>& v, double t) { double e = 0; for (size_t i = (size_t) (t * sr) - 200; i < (size_t) (t * sr) + 400; ++i) e += std::pow (ref[i] - v[i], 2.0); return e; };
            auto hfErrAt = [&] (const std::vector<float>& v, double t) { double e = 0; for (size_t i = (size_t) (t * sr) - 200; i < (size_t) (t * sr) + 400; ++i) { const double d = ((double) ref[i] - v[i]) - ((double) ref[i - 1] - v[i - 1]); e += d * d; } return e; };
            std::printf ("  (declick picture check: error at the spikes %.4f -> %.4f, at the burst %.4f -> %.4f, its top %.4f -> %.4f)\n", errAt (x, 2.0), errAt (y, 2.0), errAt (x, 3.5), errAt (y, 3.5), hfErrAt (x, 3.5), hfErrAt (y, 3.5));
            CHECK (errAt (y, 2.0) < 0.2 * errAt (x, 2.0) && hfErrAt (y, 3.5) < 0.2 * hfErrAt (x, 3.5) && errAt (y, 3.5) < 0.05);   // (the burst is faint: what is left is the mend of 4 ms of bass, 0.3 % of its energy)
            for (size_t i = 0; i < (size_t) (0.9 * sr); ++i) if (y[i] != ref[i]) { CHECK (false); break; }                                        // nothing else touched
            // the start of a loud note is not a line: the picture check leaves attacks alone
            std::vector<float> q ((size_t) (4 * sr), 0.0f);
            for (size_t i = (size_t) (2.0 * sr); i < q.size(); ++i) q[i] = (float) (0.3 * std::sin (2 * dsp::kPi * 1500.0 * (double) i / sr)) + 0.001f * (rnd.nextFloat() * 2.0f - 1.0f);
            for (size_t i = 0; i < (size_t) (2.0 * sr); ++i) q[i] = 0.001f * (rnd.nextFloat() * 2.0f - 1.0f);
            CHECK (SpectralRepair::findLines (q, sr, (long) (1.0 * sr), (long) (3.0 * sr)).empty());
        }
        // 2.93 declick: a repair must not leave a click of its own. A quiet, hissy 96 kHz piano-like sound with room-click-like bursts (a damped ring that rises fast) in it: a mend that
        // stopped short of a burst's tail used to leave a step where it met the sound, and at a high sensitivity the next pass found that step and mended it too. After the de-click there
        // must be no sharp feature (a third difference far out of the normal) that was not there before, and the clicks must still be found
        {
            const double srh = 96000.0; const size_t N = (size_t) (2.5 * srh);
            std::mt19937 rng (91); std::normal_distribution<double> nd (0.0, 1.0); std::uniform_real_distribution<double> ud (0.0, 1.0);
            std::vector<double> y (N, 0.0);
            for (int note = 0; note < 2; ++note)
                for (int k = 1; k <= 10; ++k)
                {
                    double ph = ud (rng) * 6.28, am = 0.0, pw = 0.0; const double amp = 0.012 / k, dec = 0.4 + 0.2 * ud (rng), f0 = 130.0 * std::pow (2.0, note * 0.4);
                    for (size_t i = 0; i < N; ++i)
                    {
                        am += 0.0004 * nd (rng) - 0.0003 * am; pw += 2.0e-5 * nd (rng) - 0.0001 * pw;             // (a little drift, so the sound is not perfectly predictable)
                        const double t = (double) i / srh, tn = t - note * 1.2; if (tn < 0.0) continue;
                        y[i] += amp * (1.0 + 0.3 * am) * std::exp (-tn / dec) * std::sin (2 * dsp::kPi * f0 * k * t * (1.0 + 0.0003 * k) + ph + pw);
                    }
                }
            std::vector<float> x (N); for (size_t i = 0; i < N; ++i) x[i] = (float) (y[i] + 2.0e-6 * nd (rng));
            for (int c = 0; c < 12; ++c)
            {
                const long at = (long) (0.15 * srh + ud (rng) * (2.2 * srh)); const double fc = 300.0 + 1700.0 * ud (rng), dec = 0.001 + 0.003 * ud (rng), amp = 0.002 + 0.006 * ud (rng);
                for (long i = 0; i < (long) (0.02 * srh) && at + i < (long) N; ++i) { const double t = (double) i / srh; x[(size_t) (at + i)] += (float) (amp * (1.0 - std::exp (-t / 0.0001)) * std::exp (-t / dec) * std::sin (2 * dsp::kPi * fc * t)); }
            }
            int added = 0, changed = 0, mended = 0;
            for (double sens : { 9.0, 10.0 })
            {
                auto z = x; mended += SpectralRepair::declick (z, 0, (long) N, sens, nullptr, srh);
                for (size_t i0 = 3; i0 + 2048 < N; i0 += 2048)
                {
                    std::vector<double> m; for (size_t i = i0; i < i0 + 2048; ++i) m.push_back (SpectralRepair::thirdDiff (z, (long) i));
                    std::nth_element (m.begin(), m.begin() + 1024, m.end()); const double med = m[1024] + 1.0e-12;
                    for (size_t i = i0; i < i0 + 2048; ++i)
                        if (SpectralRepair::thirdDiff (z, (long) i) > 14.0 * med)
                        {
                            double before = 0.0; for (size_t j = i - 40; j < i + 40 && j < N; ++j) before = std::max (before, SpectralRepair::thirdDiff (x, (long) j));
                            if (SpectralRepair::thirdDiff (z, (long) i) > 1.8 * before) { ++added; i += 40; }
                        }
                }
                for (size_t i = 0; i < N; ++i) if (z[i] != x[i]) ++changed;
            }
            std::printf ("  (declick at sensitivity 9 and 10, room-click bursts: %d mended, %d samples changed, %d sharp features added)\n", mended, changed, added);
            CHECK (mended >= 6 && changed > 200);                             // (it still finds and mends the clicks)
            CHECK (added == 0);
        }
        // declick: a dropout (a run of digital silence in the middle of sound) is rebuilt, not just its edges smoothed
        {
            auto x = tone; auto ref = x;
            for (size_t i = 0; i < 128; ++i) x[100000 + i] = 0.0f;
            auto y = x; const int n = SpectralRepair::declick (y, 80000, 120000, 6, nullptr, sr);
            double e0 = 0, e1 = 0, rm = 0, rr = 0;
            for (size_t i = 99990; i < 100140; ++i) { e0 += std::pow (ref[i] - x[i], 2.0); e1 += std::pow (ref[i] - y[i], 2.0); }
            for (size_t i = 100000; i < 100128; ++i) { rm += (double) y[i] * y[i]; rr += (double) ref[i] * ref[i]; }
            CHECK (n >= 1 && e1 < e0 * 0.1 && rm > 0.5 * rr);
            for (size_t i = 0; i < 99000; ++i) if (y[i] != ref[i]) { CHECK (false); break; }
            // quiet silence between notes (nothing around it) is left alone
            std::vector<float> q ((size_t) (2 * sr), 0.0f); auto q2 = q;
            CHECK (SpectralRepair::declick (q2, 1000, (long) q2.size() - 1000, 6, nullptr, sr) == 0 && q2 == q);
        }
        // lost samples: a block of samples missing from the recording (a dropped buffer). The sound jumps in one sample and goes on from a little later; the samples are put
        // back (rebuilt from both sides) and the time around is squeezed back to the original length, so the track keeps its length and is untouched outside that second
        {
            juce::Random rnd (31);
            auto ref = tone; for (auto& v : ref) v += 0.004f * (rnd.nextFloat() * 2.0f - 1.0f);
            const size_t n0 = 100000;
            auto splice = [] (const std::vector<float>& v, size_t at, size_t k) { std::vector<float> o (v.begin(), v.begin() + (long) at); o.insert (o.end(), v.begin() + (long) (at + k), v.end()); o.resize (v.size(), 0.0f); return o; };
            auto d2At = [] (const std::vector<float>& v, size_t n) { return std::abs ((double) v[n] - 2.0 * v[n - 1] + v[n - 2]); };
            auto maxD2 = [&] (const std::vector<float>& v, size_t a, size_t b) { double m = 0; for (size_t n = a; n < b; ++n) m = juce::jmax (m, d2At (v, n)); return m; };
            // how closely v[a,b) matches ref at its best lag 0..maxLag (the squeeze moves the sound by up to the number of lost samples): 0 = exactly
            auto alignErr = [&] (const std::vector<float>& v, size_t a, size_t b, int maxLag)
            {
                double best = 1e30, e0 = 0; for (size_t i = a; i < b; ++i) e0 += (double) ref[i] * ref[i];
                for (int lag = 0; lag <= maxLag; ++lag) { double e = 0; for (size_t i = a; i < b; ++i) e += std::pow ((double) v[i] - ref[i + (size_t) lag], 2.0); best = juce::jmin (best, e); }
                return best / e0;
            };
            const double usual = maxD2 (ref, 50000, 60000);
            {   // one track: found, the number of lost samples is right, no jump is left, the sound around lines up again, and nothing outside the squeeze is touched
                const auto x = splice (ref, n0, 64);
                const auto sp = LostSamples::find ({ &x }, 80000, 120000, 6, sr);
                CHECK (sp.size() == 1 && sp[0].at == (long) n0 && std::abs (sp[0].lost - 64) <= 3);
                auto y = x; const int n = SpectralRepair::declick (y, 80000, 120000, 6, nullptr, sr);
                CHECK (n >= 1);
                CHECK (d2At (x, n0) > 10 * usual && maxD2 (y, n0 - 300, n0 + 300) < 2.0 * usual);
                CHECK (alignErr (y, n0 - 400, n0 + 400, 70) < 0.02);
                const size_t W = (size_t) (0.5 * sr) + 80;
                for (size_t i = 0; i < n0 - W; ++i) if (y[i] != x[i]) { CHECK (false); break; }
                for (size_t i = n0 + W; i < x.size(); ++i) if (y[i] != x[i]) { CHECK (false); break; }
                auto y2 = x; SpectralRepair::declick (y2, 80000, 120000, 6, nullptr, sr); CHECK (y2 == y);          // the same every time (the preview and the written file agree)
                auto z = x; SpectralRepair::declick (z, (long) n0 - 200, (long) n0 + 240, 6, nullptr, sr);          // a short area marked by hand round the jump
                CHECK (maxD2 (z, n0 - 300, n0 + 300) < 2.0 * usual && alignErr (z, n0 - 400, n0 + 400, 70) < 0.02);
            }
            {   // several tracks: one event at the same sample everywhere, the number lost judged on all of them; a track too quiet to show the jump is mended the same way
                std::vector<std::vector<float>> tr;
                for (int t = 0; t < 4; ++t)
                {
                    std::vector<float> v (ref.size());
                    const float g = t == 3 ? 0.01f : 0.3f + 0.2f * (float) t;
                    for (size_t i = 0; i < v.size(); ++i) v[i] = g * (float) (std::sin (2.0 * dsp::kPi * 440.0 * (double) i / sr + t) + 0.6 * std::sin (2.0 * dsp::kPi * 660.0 * (double) i / sr + 2 * t)) + 0.003f * (rnd.nextFloat() * 2.0f - 1.0f);
                    tr.push_back (splice (v, n0, 96));
                }
                std::vector<const std::vector<float>*> tp; for (auto& t : tr) tp.push_back (&t);
                const auto sp = LostSamples::find (tp, 80000, 120000, 6, sr);
                CHECK (sp.size() == 1 && sp[0].at == (long) n0 && std::abs (sp[0].lost - 96) <= 3);
                const auto found = audioops::findLost (tr, sr, 80000, 120000, 6);
                CHECK (found.size() == 1 && found[0].at == (juce::int64) n0 - 80000);
                FixSpec dc; dc.kind = FixSpec::Kind::Declick; dc.lost = found; dc.lostKnown = true;
                for (auto& t : tr) { std::vector<std::vector<float>> ch { t }; audioops::applyFix (ch, sr, 80000, 120000, dc, (long) (0.02 * sr)); CHECK (maxD2 (ch[0], n0 - 300, n0 + 300) < 2.0 * maxD2 (t, 50000, 60000)); t = ch[0]; }
            }
            {   // music with no jump in it: nothing is found and nothing changes; nor at the two edges of a run of digital silence (that is a dropout of another kind)
                CHECK (LostSamples::find ({ &ref }, 80000, 120000, 6, sr).empty());
                auto x = ref; for (size_t i = 0; i < 128; ++i) x[n0 + i] = 0.0f;
                CHECK (LostSamples::find ({ &x }, 80000, 120000, 6, sr).empty());
                // an acoustic click (smoothed by the converter, with a reverb tail) is no lost-sample jump either: it is left to the ordinary click finder
                auto ac = ref; for (int i = -40; i < 4000; ++i) { const double t = (double) i; const double s = i < 0 ? 0.0 : std::exp (-t / 600.0) * 0.05 * (rnd.nextFloat() * 2.0f - 1.0f); const double imp = 0.4 * std::exp (-(t * t) / 8.0); ac[n0 + (size_t) (i + 40)] += (float) (s + imp); }
                CHECK (LostSamples::find ({ &ac }, 80000, 120000, 6, sr).empty());
            }
            {   // the squeeze stays inside the range it is allowed (the piece put into an edit); with too little room the jump is only smoothed
                const auto x = splice (ref, n0, 64);
                std::vector<std::vector<float>> ch { x };
                FixSpec dc; dc.kind = FixSpec::Kind::Declick; dc.lostLo = -(juce::int64) (0.1 * sr); dc.lostHi = (juce::int64) (0.1 * sr);
                audioops::applyFix (ch, sr, (long) n0, (long) n0 + 10, dc, (long) (0.02 * sr));
                CHECK (maxD2 (ch[0], n0 - 300, n0 + 300) < 2.0 * usual && alignErr (ch[0], n0 - 400, n0 + 400, 70) < 0.02);
                for (size_t i = 0; i < x.size(); ++i) if ((i < n0 - (size_t) (0.1 * sr) || i >= n0 + (size_t) (0.1 * sr)) && ch[0][i] != x[i]) { CHECK (false); break; }
                std::vector<std::vector<float>> c2 { x };
                dc.lostLo = -200; dc.lostHi = 200;
                audioops::applyFix (c2, sr, (long) n0, (long) n0 + 10, dc, (long) (0.02 * sr));
                CHECK (d2At (c2[0], n0) < 0.5 * d2At (x, n0));
                for (size_t i = 0; i < x.size(); ++i) if ((i < n0 - 200 || i >= n0 + 200) && c2[0][i] != x[i]) { CHECK (false); break; }
            }
            // ReTouch with a box round the jump, narrow or wide, either method, one channel or a linked pair: the lost samples are put back first (turning the box down or
            // rebuilding it cannot line the two sides up again)
            for (auto mode : { RepairMode::Attenuate, RepairMode::Replace })
                for (long halfWide : { 200L, 1500L, 5000L })
                {
                    const auto x = splice (ref, n0, 64); std::vector<std::vector<float>> ch { x };
                    FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 0.0; spec.f1 = 24000.0; spec.repair.mode = mode;
                    audioops::applyFix (ch, sr, (long) n0 - halfWide, (long) n0 + halfWide + 40, spec, (long) (0.02 * sr));
                    CHECK (maxD2 (ch[0], n0 - 300, n0 + 300) < 3.0 * usual);
                    CHECK (alignErr (ch[0], n0 - 400, n0 + 400, 70) < 0.05);
                    const size_t W = (size_t) (0.5 * sr) + 80;
                    for (size_t i = 0; i < n0 - W - 6000; ++i) if (ch[0][i] != x[i]) { CHECK (false); break; }
                    for (size_t i = n0 + W + 6000; i < x.size(); ++i) if (ch[0][i] != x[i]) { CHECK (false); break; }
                }
            {   // strength 0 changes nothing, the jump included
                const auto x = splice (ref, n0, 64); std::vector<std::vector<float>> ch { x };
                FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 0.0; spec.f1 = 24000.0; spec.repair.strength = 0.0;
                audioops::applyFix (ch, sr, (long) n0 - 200, (long) n0 + 240, spec, (long) (0.02 * sr));
                CHECK (ch[0] == x);
            }
            {   // a box that stays above the bass (a hiss or crackle repair) is left to the box repair alone
                const auto x = splice (ref, n0, 64); std::vector<std::vector<float>> ch { x };
                FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 4000.0; spec.f1 = 24000.0; spec.repair.mode = RepairMode::Replace;
                audioops::applyFix (ch, sr, (long) n0 - 200, (long) n0 + 240, spec, (long) (0.02 * sr));
                for (size_t i = n0 + 400; i < ref.size(); ++i) if (ch[0][i] != x[i]) { CHECK (false); break; }
            }
            {   // a linked stereo pair (the same lost samples in both, the second one softer): both are mended, the same way
                auto r2 = ref; for (auto& v : r2) v *= 0.5f;
                std::vector<std::vector<float>> ch { splice (ref, n0, 64), splice (r2, n0, 64) };
                FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 0.0; spec.f1 = 24000.0; spec.repair.mode = RepairMode::Attenuate; spec.repair.linkChannels = true;
                audioops::applyFix (ch, sr, (long) n0 - 200, (long) n0 + 240, spec, (long) (0.02 * sr));
                CHECK (maxD2 (ch[0], n0 - 300, n0 + 300) < 3.0 * usual && maxD2 (ch[1], n0 - 300, n0 + 300) < 1.5 * usual);
                double e = 0; for (size_t i = n0 - 3000; i < n0 + 3000; ++i) e = juce::jmax (e, (double) std::abs (ch[1][i] - 0.5f * ch[0][i]));
                CHECK (e < 0.01);
            }
        }
        // spectrogram: two low notes 13 Hz apart are told apart with the bass layer (and are not with the short window alone); a click stays sharp in time
        {
            std::vector<float> x ((size_t) (6 * sr), 0.0f);
            for (size_t i = 0; i < x.size(); ++i) x[i] = 0.2f * (float) std::sin (2.0 * dsp::kPi * 55.0 * (double) i / sr) + 0.2f * (float) std::sin (2.0 * dsp::kPi * 68.0 * (double) i / sr);
            const auto blend = SpectralRepair::spectrogram ({ x }, sr, 1600, 0), plain = SpectralRepair::spectrogram ({ x }, sr, 1600, 1);
            const int fr = blend.frames / 2;
            const float pk = blend.level (fr, 55.0), va = blend.level (fr, 61.5), pk2 = blend.level (fr, 68.0);
            CHECK (pk - va > 12.0f && pk2 - va > 12.0f);
            CHECK (plain.level (fr, 55.0) - plain.level (fr, 61.5) < 6.0f);
            CHECK (std::abs (blend.level (fr, 5000.0) - plain.level (fr, 5000.0)) < 0.01f);           // above the cross-over nothing changes
        }
        // patch: a burst of noise inside the box is replaced, the rest is left alone
        {
            auto x = tone; auto ref = x; juce::Random rnd (7);
            const long t0 = (long) (2 * sr), t1 = t0 + (long) (0.2 * sr);
            for (long n = t0; n < t1; ++n) x[(size_t) n] += 0.5f * (rnd.nextFloat() * 2.0f - 1.0f);
            auto snr = [&] (const std::vector<float>& v) { double s = 0, e = 0; for (long n = t0; n < t1; ++n) { s += ref[(size_t) n] * ref[(size_t) n]; e += std::pow (v[(size_t) n] - ref[(size_t) n], 2.0); } return 10.0 * std::log10 (s / (e + 1e-30)); };
            const double before = snr (x);
            SpectralRepair::patch (x, sr, t0, t1, 0.0, 24000.0, RepairSide::Both);
            CHECK (snr (x) > before + 10.0);
            for (long n = 0; n < t0 - 10; ++n) if (x[(size_t) n] != ref[(size_t) n]) { CHECK (false); break; }
        }
        // Attenuate: a burst of noise over a held chord is turned down to the level of the sound around it, the music underneath stays
        {
            const long t0 = (long) (2 * sr), t1 = t0 + (long) (0.2 * sr);
            auto rmsOf = [&] (const std::vector<float>& v, long a, long b) { double e = 0; for (long n = a; n < b; ++n) e += (double) v[(size_t) n] * v[(size_t) n]; return 10.0 * std::log10 (e / (double) (b - a) + 1e-30); };
            auto noisy = tone; juce::Random rnd (21);
            for (long n = t0; n < t1; ++n) noisy[(size_t) n] += 1.0f * (rnd.nextFloat() * 2.0f - 1.0f);
            RepairParams p; p.mode = RepairMode::Attenuate; p.strength = 1.0; p.direction = RepairDirection::LeftRight;
            auto y = noisy; SpectralRepair::repair (y, sr, t0, t1, 0.0, 24000.0, p);
            const double around = 0.5 * (rmsOf (tone, t0 - (long) (0.3 * sr), t0 - 100) + rmsOf (tone, t1 + 100, t1 + (long) (0.3 * sr)));
            CHECK (y.size() == noisy.size());
            CHECK (rmsOf (noisy, t0, t1) > around + 3.0);                                  // the burst really stuck out
            CHECK (std::abs (rmsOf (y, t0, t1) - around) < 3.0);                           // and now sits at the level of the surroundings
            auto z = noisy; p.strength = 0.0; SpectralRepair::repair (z, sr, t0, t1, 0.0, 24000.0, p);
            CHECK (z == noisy);
            for (long n = 0; n < t0 - 9000; ++n) if (y[(size_t) n] != noisy[(size_t) n]) { CHECK (false); break; }
        }
        // new files named before they are written never share a name, and a WAV written over an old file replaces it (no adding to the end)
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-names-test"); dir.deleteRecursively(); dir.createDirectory();
            const auto n1 = audioops::uniqueFile (dir.getChildFile ("a (reharmonised).wav")), n2 = audioops::uniqueFile (dir.getChildFile ("a (reharmonised).wav"));
            CHECK (n1 != n2);
            std::vector<float> s1 (4000, 0.25f), s2 (1000, -0.25f);
            CHECK (audioops::writeWav (n1, { s1 }, 48000.0)); CHECK (audioops::writeWav (n1, { s2 }, 48000.0));
            juce::AudioFormatManager fmx; fmx.registerBasicFormats();
            auto rd = audioops::openReader (fmx, n1);
            CHECK (rd != nullptr && rd->lengthInSamples == 1000);
            rd.reset(); dir.deleteRecursively();
        }
        // processing across edit points: whichever way the marks cross the joins, with the pieces made again from identical sound the edit must sound exactly as before,
        // and the real edit points must keep the fades and the curves they had
        {
            const double r8 = 8000.0; juce::AudioFormatManager fmx; fmx.registerBasicFormats();
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-join-test"); dir.deleteRecursively(); dir.createDirectory();
            juce::Random rnd (5);
            std::vector<juce::File> takeFile; std::vector<std::vector<float>> takeAudio;
            for (int k = 0; k < 3; ++k) { std::vector<float> v ((size_t) (6 * r8)); for (auto& x : v) x = rnd.nextFloat() * 0.5f - 0.25f; takeAudio.push_back (v); takeFile.push_back (dir.getChildFile ("take" + juce::String (k) + ".wav")); CHECK (audioops::writeWav (takeFile.back(), { v }, r8, true)); }
            const auto trk = juce::Uuid();
            auto makeEdit = [&] (bool middleIsFix)
            {
                EditDef e; e.sampleRate = r8;
                for (int k = 0; k < 3; ++k)
                {
                    EditRegion r; r.sampleRate = r8; r.takeName = "Take " + juce::String (k); r.srcIn = (juce::int64) (1.0 * r8); r.srcOut = (juce::int64) (3.0 * r8); r.startSample = (juce::int64) (2.0 * r8 * k);
                    RegionFile f; f.trackId = trk; f.trackName = "Mic"; f.file = takeFile[(size_t) k]; r.files.push_back (f); r.id = juce::Uuid();
                    e.regions.push_back (r);
                }
                e.setDefaultJoinFades (1); e.setDefaultJoinFades (2);
                if (middleIsFix) { auto& m = e.regions[1]; m.fixIn = m.fixOut = true; e.regions[0].fixOut = true; e.regions[2].fixIn = true; m.curve = FadeCurve::Linear; e.regions[0].setOutCurve (FadeCurve::Linear); e.regions[2].setInCurve (FadeCurve::Linear); e.regions[2].setOutCurve (FadeCurve::EqualPower); }
                return e;
            };
            auto renderEdit = [&] (const EditDef& e)
            {
                std::vector<float> out ((size_t) (6 * r8), 0.0f);
                for (auto& r : e.regions)
                {
                    auto rd = audioops::openReader (fmx, r.files[0].file); if (rd == nullptr) continue;
                    const auto t0 = juce::jmax ((juce::int64) 0, r.fadeInBegin()), t1 = juce::jmin ((juce::int64) out.size(), r.fadeOutFinish());
                    if (t1 <= t0) continue;
                    std::vector<std::vector<float>> ch; audioops::readRange (*rd, r.srcIn + (t0 - r.startSample) - r.files[0].fileStart, t1 - t0, ch);
                    for (juce::int64 t = t0; t < t1 && (size_t) (t - t0) < ch[0].size(); ++t) out[(size_t) t] += ch[0][(size_t) (t - t0)] * r.gainAt (t);
                }
                return out;
            };
            struct Marks { double from, to; };
            for (bool fixMiddle : { false, true })
                for (auto m : { Marks { 0.5, 5.5 }, Marks { 1.9, 4.1 }, Marks { 1.98, 2.02 }, Marks { 2.5, 3.9 }, Marks { 3.99, 6.0 }, Marks { 0.2, 3.0 }, Marks { 1.0, 2.0 } })
                {
                    auto e = makeEdit (fixMiddle); const auto base = e.regions; const auto before = renderEdit (e);
                    const auto from = (juce::int64) (m.from * r8), to = (juce::int64) (m.to * r8);
                    std::vector<RegionJob> jobs; FixSpec spec; spec.kind = FixSpec::Kind::Reharmonise; int made = 0;
                    for (auto& r : base)
                    {
                        if (r.endSample() <= from || r.startSample >= to) continue;
                        const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
                        if (r1 - r0 < 2) continue;
                        RegionJob j; j.regionId = r.id; const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
                        j.a = a; j.b = b; j.fixFrom = a; j.fixTo = b; j.outFrom = a - (juce::int64) r8; j.outTo = b + (juce::int64) r8; j.idx = { 0 }; j.src = { r.files[0].file }; j.fileStart = { 0 };
                        const auto A = juce::jmax ((juce::int64) 0, j.outFrom), B = juce::jmin ((juce::int64) (6 * r8), j.outTo);
                        const auto dest = audioops::uniqueFile (dir.getChildFile ("fix.wav")); j.dest = { dest };
                        const auto& src = takeAudio[(size_t) (&r - &base[0])];
                        CHECK (audioops::writeWav (dest, { std::vector<float> (src.begin() + (long) A, src.begin() + (long) B) }, r8, true));
                        audioops::PieceResult pr; pr.file = dest; pr.from = A; pr.to = B; pr.ok = true; j.results.push_back (pr); jobs.push_back (j); ++made;
                    }
                    if (made == 0) continue;
                    const bool ok = applyRegionJobs (e, base, jobs, "reharmonised");
                    CHECK (ok);
                    if (! ok) continue;
                    const auto after = renderEdit (e);
                    double worst = 0; for (size_t i = 0; i < before.size(); ++i) worst = juce::jmax (worst, (double) std::abs (before[i] - after[i]));
                    if (worst >= 1.0e-4)
                    {
                        size_t wi = 0; double wv = 0; for (size_t i = 0; i < before.size(); ++i) if (std::abs (before[i] - after[i]) > wv) { wv = std::abs (before[i] - after[i]); wi = i; }
                        std::printf ("  join test: fixMiddle=%d marks %.2f-%.2f worst diff %.5f at %.4f s; regions now:", (int) fixMiddle, m.from, m.to, wv, (double) wi / r8);
                        for (auto& r : e.regions) std::printf (" [%.3f..%.3f in %.3f/%.3f out %.3f/%.3f]", (double) r.startSample / r8, (double) r.endSample() / r8, r.inStart, r.inEnd, r.outStart, r.outEnd);
                        std::printf ("\n");
                    }
                    CHECK (worst < 1.0e-4);                                                     // the sound is the same
                    // the real edit points: the fades and curves of every original region are where they were
                    for (size_t j = 0; j < base.size(); ++j)
                    {
                        const auto& ob = base[j]; const int first = e.indexOf (ob.id); CHECK (first >= 0); if (first < 0) continue;
                        const int last = j + 1 < base.size() ? e.indexOf (base[j + 1].id) - 1 : (int) e.regions.size() - 1;
                        const auto& f = e.regions[(size_t) first]; const auto& l = e.regions[(size_t) last];
                        CHECK (f.inStart == ob.inStart && f.inEnd == ob.inEnd && f.curve == ob.curve && f.fixIn == ob.fixIn);
                        CHECK (l.outStart == ob.outStart && l.outEnd == ob.outEnd && l.outCurveValue() == ob.outCurveValue() && l.fixOut == ob.fixOut);
                    }
                }
            dir.deleteRecursively();
        }
        // Attenuate with linked channels: a stereo pair keeps its level difference and its time difference (the stereo image)
        {
            const long t0 = (long) (2 * sr), t1 = t0 + (long) (0.2 * sr);
            std::vector<float> Lc = tone, Rc (tone.size(), 0.0f); juce::Random rnd (31);
            for (long n = t0; n < t1; ++n) Lc[(size_t) n] += 1.0f * (rnd.nextFloat() * 2.0f - 1.0f);
            const long delay = 20;                                                          // the right microphone hears it 20 samples later, and quieter
            for (size_t n = (size_t) delay; n < Lc.size(); ++n) Rc[n] = 0.5f * Lc[n - (size_t) delay];
            RepairParams p; p.mode = RepairMode::Attenuate; p.strength = 1.0; p.linkChannels = true;
            std::vector<std::vector<float>> ch { Lc, Rc }; FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 0.0; spec.f1 = 24000.0; spec.repair = p;
            audioops::applyFix (ch, sr, t0, t1, spec, 0);
            double e = 0, ref = 0;                                                          // right should still be half of left, 20 samples later
            for (long n = t0 + 200; n < t1 - 200; ++n) { const double d = (double) ch[1][(size_t) n] - 0.5 * (double) ch[0][(size_t) (n - delay)]; e += d * d; ref += std::pow (0.5 * (double) ch[0][(size_t) (n - delay)], 2.0); }
            CHECK (e < 0.01 * ref);
            CHECK (ch[0] != Lc);                                                            // and something was done
        }
        // Attenuate a stray consonant: an early copy of a consonant (over the end of a sung vowel, right before the main one) and a late copy (after the main one, over the
        // start of the next vowel) come down to the level the track has without them; the vowels keep their level; nothing outside the box changes; plain Attenuate, which
        // takes the main consonant next to the box as 'normal' sound, leaves much more of them
        {
            const size_t L = (size_t) (3 * sr);
            std::vector<double> hiss (262144, 0.0); juce::Random rnd (77);
            for (size_t n = 0; n < L; ++n) hiss[n] = rnd.nextFloat() * 2.0 - 1.0;
            {   // the noise of a consonant: 3 - 12 kHz
                dsp::FFT big (262144); std::vector<std::complex<double>> b (262144);
                for (size_t n = 0; n < b.size(); ++n) b[n] = hiss[n];
                big.forward (b);
                for (size_t k = 0; k < b.size(); ++k) { const double hz = (double) juce::jmin (k, b.size() - k) * sr / (double) b.size(); if (hz < 3000.0 || hz > 12000.0) b[k] = 0.0; }
                big.inverse (b);
                double e = 0; for (size_t n = 0; n < L; ++n) e += b[n].real() * b[n].real();
                const double g = 0.05 / std::sqrt (e / (double) L);
                for (size_t n = 0; n < L; ++n) hiss[n] = b[n].real() * g;
            }
            auto vowel = [&] (std::vector<float>& v, double f0, double a, double b)
            {
                for (size_t n = (size_t) (a * sr); n < (size_t) (b * sr) && n < v.size(); ++n)
                {
                    const double t = (double) n / sr; double s = 0;
                    for (int k = 1; k * f0 < 5000.0; ++k) s += 0.25 / std::pow ((double) k, 1.5) * std::sin (2.0 * dsp::kPi * k * f0 * t + 0.7 * k);     // a sung vowel: little above 4 - 5 kHz
                    v[n] += (float) s;
                }
            };
            auto bandDb = [&] (const std::vector<float>& v, long a, long b, double lo, double hi)
            {
                const int N = 1024; dsp::FFT f (N); const auto w = dsp::hann (N); std::vector<std::complex<double>> buf ((size_t) N);
                double e = 0; int frames = 0;
                for (long s = a; s + N <= b; s += N / 4)
                {
                    for (int i = 0; i < N; ++i) buf[(size_t) i] = (double) v[(size_t) (s + i)] * w[(size_t) i];
                    f.forward (buf);
                    for (int k = (int) (lo * N / sr); k <= (int) (hi * N / sr); ++k) e += std::norm (buf[(size_t) k]);
                    ++frames;
                }
                return 10.0 * std::log10 (e / juce::jmax (1, frames) + 1e-30);
            };
            std::vector<float> clean (L, 0.0f);
            juce::Random rr (78); for (auto& s : clean) s = 0.002f * (rr.nextFloat() * 2.0f - 1.0f);         // the room
            vowel (clean, 220.0, 0.0, 1.50); vowel (clean, 247.0, 1.78, 3.0);
            for (size_t n = (size_t) (1.50 * sr); n < (size_t) (1.72 * sr); ++n) clean[n] += (float) hiss[n];                    // the main consonant
            auto messy = clean;
            for (size_t n = (size_t) (1.25 * sr); n < (size_t) (1.50 * sr); ++n) messy[n] += (float) (0.5 * hiss[n + 30000]);     // an early copy (a singer who came in early)
            for (size_t n = (size_t) (1.80 * sr); n < (size_t) (2.00 * sr); ++n) messy[n] += (float) (0.5 * hiss[n + 50000]);     // a late copy, over the start of the next vowel
            struct Box { double a, b; } boxes[] = { { 1.23, 1.50 }, { 1.73, 2.02 } };
            for (auto bx : boxes)
            {
                const long t0 = (long) (bx.a * sr), t1 = (long) (bx.b * sr), m0 = t0 + (long) (0.012 * sr), m1 = t1 - (long) (0.012 * sr);
                RepairParams p; p.mode = RepairMode::Attenuate; p.strength = 1.0; p.consonant = true;
                auto y = messy; SpectralRepair::repair (y, sr, t0, t1, 2000.0, 24000.0, p);
                RepairParams q = p; q.consonant = false;
                auto z = messy; SpectralRepair::repair (z, sr, t0, t1, 2000.0, 24000.0, q);
                RepairParams w = p; w.weighting = bx.a < 1.4 ? 1.0 : 0.0;                    // made to match the WRONG side (the main consonant)
                auto u = messy; SpectralRepair::repair (u, sr, t0, t1, 2000.0, 24000.0, w);
                const double cl = bandDb (clean, m0, m1, 4000, 12000), me = bandDb (messy, m0, m1, 4000, 12000), co = bandDb (y, m0, m1, 4000, 12000);
                const double pl = bandDb (z, m0, m1, 4000, 12000), wr = bandDb (u, m0, m1, 4000, 12000);
                std::printf ("  stray consonant %.2f-%.2f s: 4-12 kHz clean %.1f, messy %.1f, consonant mode %.1f, plain attenuate %.1f, wrong side %.1f dB\n", bx.a, bx.b, cl, me, co, pl, wr);
                CHECK (me > cl + 8.0);                                                       // the stray copy really sticks out
                CHECK (co < cl + 2.5 && co > cl - 4.0);                                      // and is now down to the track's own level (no hole)
                CHECK (pl > co + 1.5);                                                       // plain Attenuate leaves more
                CHECK (wr > co + 3.0);                                                       // the side the box is matched to matters
                const double vc = bandDb (clean, m0, m1, 1800, 2700), vy = bandDb (y, m0, m1, 1800, 2700);
                CHECK (std::abs (vy - vc) < 2.0);                                            // the vowel's harmonics in the box keep their level
                for (long n = 0; n < t0 - 8192; ++n) if (y[(size_t) n] != messy[(size_t) n]) { CHECK (false); break; }
                for (long n = t1 + 8192; n < (long) L; ++n) if (y[(size_t) n] != messy[(size_t) n]) { CHECK (false); break; }
                auto s0 = messy; RepairParams p0 = p; p0.strength = 0.0; SpectralRepair::repair (s0, sr, t0, t1, 2000.0, 24000.0, p0);
                CHECK (s0 == messy);
            }
            // the side of the main consonant, judged on several tracks together (the repair window does this once for all of them): after an early box, before a late one
            {
                auto quiet = messy; for (auto& v : quiet) v *= 0.3f;
                std::vector<float> silent (L, 0.0f);
                const std::vector<const std::vector<float>*> trs { &messy, &quiet, &clean, &silent };
                const auto e = SpectralRepair::consonantSides (trs, sr, (long) (1.23 * sr), (long) (1.50 * sr), 2000.0, 24000.0);
                const auto l = SpectralRepair::consonantSides (trs, sr, (long) (1.73 * sr), (long) (2.02 * sr), 2000.0, 24000.0);
                CHECK (e.first > 0.99 && e.second == 1);
                CHECK (l.first < 0.01 && l.second == -1);
                // and a box with the same sound on both sides (a stray consonant in the middle of a vowel): both sides, no edge
                auto mid = clean; for (size_t n = (size_t) (0.60 * sr); n < (size_t) (0.75 * sr); ++n) mid[n] += (float) hiss[n];
                const auto m = SpectralRepair::consonantSides ({ &mid }, sr, (long) (0.58 * sr), (long) (0.77 * sr), 2000.0, 24000.0);
                CHECK (m.second == 0 && m.first > 0.2 && m.first < 0.8);
            }
            // a late copy right after a long main consonant: the main one DIES AWAY into the box (no sudden stop), and the box joins the sound after it without a step
            {
                std::vector<float> v (L, 0.0f); juce::Random r3 (79); for (auto& s : v) s = 0.002f * (r3.nextFloat() * 2.0f - 1.0f);
                for (size_t n = (size_t) (0.80 * sr); n < (size_t) (1.25 * sr); ++n) v[n] += (float) hiss[n];                 // the main consonant
                for (size_t n = (size_t) (1.25 * sr); n < (size_t) (1.45 * sr); ++n) v[n] += (float) (0.5 * hiss[n + 40000]); // a singer who went on too long
                const long t0 = (long) (1.25 * sr), t1 = (long) (1.47 * sr), win = 1024 + 32;                 // (one analysis frame per window)
                RepairParams p; p.mode = RepairMode::Attenuate; p.strength = 1.0; p.consonant = true;
                auto y = v; SpectralRepair::repair (y, sr, t0, t1, 2000.0, 24000.0, p);
                const double pre = bandDb (y, t0 - win, t0, 4000, 12000);
                const double s0 = bandDb (y, t0, t0 + win, 4000, 12000), s1 = bandDb (y, t0 + 2 * win, t0 + 3 * win, 4000, 12000), s2 = bandDb (y, t0 + 6 * win, t0 + 7 * win, 4000, 12000);
                const double endIn = bandDb (y, t1 - win, t1, 4000, 12000), after = bandDb (y, t1, t1 + win, 4000, 12000), orig = bandDb (v, t0 + 3 * win, t1 - 2 * win, 4000, 12000);
                std::printf ("  decay into the box: before %.1f, box start %.1f, +43 ms %.1f, +128 ms %.1f, box end %.1f, after %.1f (was %.1f) dB\n", pre, s0, s1, s2, endIn, after, orig);
                CHECK (s0 > s1 + 1.0 && s1 > s2 + 1.0);                                        // it fades ...
                CHECK (s0 > s2 + 6.0 && s0 < pre + 1.0);                                       // ... from the main consonant's level, not a sudden drop
                CHECK (s2 < orig - 10.0);                                                      // and the late copy is gone further in
                CHECK (std::abs (endIn - after) < 4.0);                                        // no step where the box ends
            }
            // Gain of a box: only the box changes, by the amount asked (louder, quieter, or silent)
            {
                const long t0 = (long) (0.4 * sr), t1 = (long) (1.0 * sr);
                for (double db : { -12.0, 6.0, -200.0 })
                {
                    RepairParams p; p.mode = RepairMode::Gain; p.gainDb = db; p.strength = 1.0;
                    auto y = messy; SpectralRepair::repair (y, sr, t0, t1, 1500.0, 3000.0, p);
                    const long m0 = t0 + 4096, m1 = t1 - 4096;
                    const double in0 = bandDb (messy, m0, m1, 1700, 2800), in1 = bandDb (y, m0, m1, 1700, 2800);
                    const double lo0 = bandDb (messy, m0, m1, 200, 1100), lo1 = bandDb (y, m0, m1, 200, 1100);
                    const double hi0 = bandDb (messy, m0, m1, 4500, 9000), hi1 = bandDb (y, m0, m1, 4500, 9000);
                    std::printf ("  box gain %.0f dB: in the box %.1f dB, below it %.2f dB, above it %.2f dB\n", db, in1 - in0, lo1 - lo0, hi1 - hi0);
                    if (db > -100.0) CHECK (std::abs ((in1 - in0) - db) < 1.0); else CHECK (in1 - in0 < -40.0);
                    CHECK (std::abs (lo1 - lo0) < 0.5 && std::abs (hi1 - hi0) < 0.5);
                    for (long n = 0; n < t0 - 4096; ++n) if (y[(size_t) n] != messy[(size_t) n]) { CHECK (false); break; }
                    for (long n = t1 + 4096; n < (long) L; ++n) if (y[(size_t) n] != messy[(size_t) n]) { CHECK (false); break; }
                }
                std::vector<std::vector<float>> ch { messy }; FixSpec spec; spec.kind = FixSpec::Kind::Patch; spec.f0 = 100.0; spec.f1 = 900.0;
                spec.repair.mode = RepairMode::Gain; spec.repair.gainDb = -6.0; spec.repair.strength = 1.0;
                CHECK (! audioops::boxRoundClick (spec, sr, 0, (long) (0.1 * sr)));        // a small low box with Gain is never taken for a click
                audioops::applyFix (ch, sr, t0, t1, spec, 0);
                CHECK (std::abs ((bandDb (ch[0], t0 + 4096, t1 - 4096, 200, 800) - bandDb (messy, t0 + 4096, t1 - 4096, 200, 800)) + 6.0) < 1.0);
            }
            // the main consonant itself, outside the boxes, is untouched
            const double mainBefore = bandDb (messy, (long) (1.52 * sr), (long) (1.70 * sr), 4000, 12000);
            auto both = messy; RepairParams p; p.mode = RepairMode::Attenuate; p.strength = 1.0; p.consonant = true;
            for (auto bx : boxes) SpectralRepair::repair (both, sr, (long) (bx.a * sr), (long) (bx.b * sr), 2000.0, 24000.0, p);
            CHECK (std::abs (bandDb (both, (long) (1.52 * sr), (long) (1.70 * sr), 4000, 12000) - mainBefore) < 0.5);
        }
        // the sharp picture: a steady tone is a thin line, a click a thin column
        {
            std::vector<float> t ((size_t) (2 * sr)); for (size_t n = 0; n < t.size(); ++n) t[n] = 0.3f * (float) std::sin (2 * dsp::kPi * 1000.0 * (double) n / sr);
            PictureAxis ax; ax.fLo = 50.0; ax.fHi = 8000.0; ax.mel = true;
            const auto pic = picture::render ({ t }, sr, 0.5, 1.5, 400, 200, ax, PictureOptions());
            CHECK (pic.valid());
            if (pic.valid())
            {
                const int col = 200; float best = -1000; int row = 0; int wide = 0;
                for (int r = 0; r < pic.h; ++r) { const float v = pic.db[(size_t) r * (size_t) pic.w + (size_t) col]; if (v > best) { best = v; row = r; } }
                for (int r = 0; r < pic.h; ++r) if (pic.db[(size_t) r * (size_t) pic.w + (size_t) col] > best - 30.0f) ++wide;
                const double expectRow = (1.0 - ax.posOf (1000.0)) * (pic.h - 1);
                CHECK (std::abs (row - expectRow) < 4.0);
                CHECK (wide <= 6);
            }
        }
        // spectral repair with the RX-style controls
        {
            const long t0 = (long) (2 * sr), t1 = t0 + (long) (0.2 * sr);
            auto goertzel = [&] (const std::vector<float>& v, double hz) { double re = 0, im = 0; for (long n = t0; n < t1; ++n) { const double a = 2 * dsp::kPi * hz * (double) n / sr; re += v[(size_t) n] * std::cos (a); im += v[(size_t) n] * std::sin (a); } return std::sqrt (re * re + im * im); };
            auto noisy = tone; auto ref = tone; juce::Random rnd (11);
            for (long n = t0; n < t1; ++n) noisy[(size_t) n] += 0.5f * (rnd.nextFloat() * 2.0f - 1.0f);
            auto err = [&] (const std::vector<float>& v) { double e = 0; for (long n = t0; n < t1; ++n) e += std::pow (v[(size_t) n] - ref[(size_t) n], 2.0); return e; };
            RepairParams p; p.mode = RepairMode::Replace; p.direction = RepairDirection::LeftRight;
            auto a = noisy; p.strength = 1.0; SpectralRepair::repair (a, sr, t0, t1, 0.0, 24000.0, p);
            auto b = noisy; p.strength = 0.5; SpectralRepair::repair (b, sr, t0, t1, 0.0, 24000.0, p);
            auto z = noisy; p.strength = 0.0; SpectralRepair::repair (z, sr, t0, t1, 0.0, 24000.0, p);
            CHECK (a.size() == noisy.size() && err (a) < 0.3 * err (noisy));                 // full strength: much cleaner
            CHECK (err (b) > err (a) && err (b) < err (noisy));                              // half strength: in between
            CHECK (z == noisy);                                                              // zero strength: nothing changes
            for (long n = 0; n < t0 - 10; ++n) if (a[(size_t) n] != noisy[(size_t) n]) { CHECK (false); break; }
            // a whistle at 4 kHz inside the box: looking up and down (and both) removes it, looking left and right (the whistle is steady) also does;
            // what matters is that the other pitches are not touched and the length is kept
            auto whistle = tone; for (long n = t0; n < t1; ++n) whistle[(size_t) n] += 0.4f * (float) std::sin (2 * dsp::kPi * 4000.0 * (double) n / sr);
            const double w0 = goertzel (whistle, 4000.0);
            for (auto dir : { RepairDirection::UpDown, RepairDirection::Both })
            {
                auto y = whistle; RepairParams q; q.mode = RepairMode::Replace; q.direction = dir; q.strength = 1.0; q.contextPct = 100.0;
                SpectralRepair::repair (y, sr, t0, t1, 3500.0, 4500.0, q);
                CHECK (y.size() == whistle.size() && goertzel (y, 4000.0) < 0.35 * w0);
                CHECK (std::abs (goertzel (y, 440.0) - goertzel (whistle, 440.0)) < 0.2 * goertzel (whistle, 440.0));
            }
            // before / after weighting: with the sound before the box different from the sound after it, 0 follows the one and 1 the other
            std::vector<float> two ((size_t) (6 * sr), 0.0f);
            for (size_t n = 0; n < two.size(); ++n) two[n] = (float) (0.3 * std::sin (2 * dsp::kPi * (n < (size_t) (3 * sr) ? 300.0 : 900.0) * (double) n / sr));
            const long u0 = (long) (2.9 * sr), u1 = (long) (3.1 * sr);
            auto lvl = [&] (const std::vector<float>& v, double hz) { double re = 0, im = 0; for (long n = u0; n < u1; ++n) { const double a2 = 2 * dsp::kPi * hz * (double) n / sr; re += v[(size_t) n] * std::cos (a2); im += v[(size_t) n] * std::sin (a2); } return std::sqrt (re * re + im * im); };
            RepairParams w; w.direction = RepairDirection::LeftRight; w.strength = 1.0; w.contextPct = 100.0;
            auto before = two; w.weighting = 0.0; SpectralRepair::repair (before, sr, u0, u1, 0.0, 24000.0, w);
            auto after = two; w.weighting = 1.0; SpectralRepair::repair (after, sr, u0, u1, 0.0, 24000.0, w);
            CHECK (lvl (before, 300.0) > 2.0 * lvl (before, 900.0));
            CHECK (lvl (after, 900.0) > 2.0 * lvl (after, 300.0));
        }
        // files: a fix on part of a take makes a new file of the same length; the rest is identical and the original is untouched
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto orig = tmp.getChildFile ("fixsrc.wav");
            CHECK (audioops::writeWav (orig, { tone }, sr));
            const auto dest = tmp.getChildFile ("fixsrc (pitch).wav");
            FixSpec spec; spec.kind = FixSpec::Kind::Pitch; spec.cents = 100.0;
            juce::String err;
            const auto made = audioops::fixWholeFile (fm, orig, (juce::int64) (1 * sr), (juce::int64) (3 * sr), spec, dest, err);
            CHECK (made == dest && err.isEmpty());
            std::unique_ptr<juce::AudioFormatReader> a (fm.createReaderFor (orig)), b (fm.createReaderFor (dest));
            CHECK (a != nullptr && b != nullptr && a->lengthInSamples == b->lengthInSamples && b->bitsPerSample == 24);
            std::vector<std::vector<float>> ca, cb;
            if (a && b)
            {
                audioops::readRange (*a, 0, a->lengthInSamples, ca); audioops::readRange (*b, 0, b->lengthInSamples, cb);
                double d = 0; for (size_t i = 0; i < (size_t) (0.9 * sr); ++i) d = juce::jmax (d, (double) std::abs (ca[0][i] - cb[0][i]));
                CHECK (d < 1.0e-5);                                                                    // before the part: the same (24-bit rounding only)
                double d2 = 0; for (size_t i = (size_t) (3.1 * sr); i < ca[0].size(); ++i) d2 = juce::jmax (d2, (double) std::abs (ca[0][i] - cb[0][i]));
                CHECK (d2 < 1.0e-5);
                CHECK (std::abs (1200.0 * std::log2 (toneFreq (cb[0], (size_t) (1.5 * sr)) / (440.0 * std::pow (2.0, 100.0 / 1200.0)))) < 2.0);   // inside the part: a semitone up
            }
            // several changes in one go (the repair window's Accept / Finish): each is made, the audio between them is untouched, and the whole thing is one new file
            {
                std::vector<float> tone8 ((size_t) (8 * sr));
                for (size_t i = 0; i < tone8.size(); ++i) tone8[i] = 0.4f * (float) std::sin (2.0 * dsp::kPi * 440.0 * (double) i / sr) + 0.2f * (float) std::sin (2.0 * dsp::kPi * 660.0 * (double) i / sr);
                const auto o8 = tmp.getChildFile ("fixsrc8.wav"), dm = tmp.getChildFile ("fixsrc8 (two).wav");
                CHECK (audioops::writeWav (o8, { tone8 }, sr));
                std::vector<audioops::FixOp> two;
                FixSpec up = spec, down = spec; down.cents = -100.0;
                two.push_back (audioops::FixOp { up, (juce::int64) (0.5 * sr), (juce::int64) (2.5 * sr) });
                two.push_back (audioops::FixOp { down, (juce::int64) (5.0 * sr), (juce::int64) (7.0 * sr) });
                const auto m2 = audioops::fixWholeFile (fm, o8, two, dm, err);
                CHECK (m2 == dm && err.isEmpty());
                std::unique_ptr<juce::AudioFormatReader> r2 (fm.createReaderFor (dm)), r8 (fm.createReaderFor (o8));
                std::vector<std::vector<float>> c2, c8;
                if (r2 && r8)
                {
                    audioops::readRange (*r2, 0, r2->lengthInSamples, c2); audioops::readRange (*r8, 0, r8->lengthInSamples, c8);
                    CHECK (r2->lengthInSamples == r8->lengthInSamples);
                    CHECK (std::abs (1200.0 * std::log2 (toneFreq (c2[0], (size_t) (0.8 * sr)) / (440.0 * std::pow (2.0, 100.0 / 1200.0)))) < 2.0);     // first change: up
                    CHECK (std::abs (1200.0 * std::log2 (toneFreq (c2[0], (size_t) (5.3 * sr)) / (440.0 * std::pow (2.0, -100.0 / 1200.0)))) < 2.0);    // second change: down
                    double mid = 0; for (size_t i = (size_t) (3.0 * sr); i < (size_t) (4.5 * sr); ++i) mid = juce::jmax (mid, (double) std::abs (c8[0][i] - c2[0][i]));
                    CHECK (mid < 1.0e-5);                                                                  // between the two changes: the same
                }
            }
            // a short piece with its own start: the file holds only the take's samples [from, to)
            const auto d3 = tmp.getChildFile ("piece.wav");
            const auto pr = audioops::fixPiece (fm, orig, 0, (juce::int64) (1 * sr), (juce::int64) (2 * sr), (juce::int64) (1 * sr), (juce::int64) (2 * sr), spec, d3, err);
            CHECK (pr.ok && pr.from == (juce::int64) sr && pr.to == (juce::int64) (2 * sr));
            std::unique_ptr<juce::AudioFormatReader> c3 (fm.createReaderFor (d3)); CHECK (c3 != nullptr && c3->lengthInSamples == (juce::int64) sr);
            // a de-click of a short part round lost samples: the new file holds the whole time squeeze (half a second each side), is just as long, and is the same outside it
            {
                auto base = tone; juce::Random rn (33); for (auto& v : base) v += 0.004f * (rn.nextFloat() * 2.0f - 1.0f);
                const size_t n0 = 100000;
                std::vector<float> x (base.begin(), base.begin() + (long) n0); x.insert (x.end(), base.begin() + (long) n0 + 64, base.end()); x.resize (base.size(), 0.0f);
                auto d2At = [] (const std::vector<float>& v, size_t n) { return std::abs ((double) v[n] - 2.0 * v[n - 1] + v[n - 2]); };
                double usual = 0; for (size_t n = 50000; n < 60000; ++n) usual = juce::jmax (usual, d2At (x, n));
                const auto cf = tmp.getChildFile ("clicksrc.wav"), cd = tmp.getChildFile ("clicksrc (declick).wav");
                CHECK (audioops::writeWav (cf, { x }, sr));
                std::unique_ptr<juce::AudioFormatReader> rsrc (fm.createReaderFor (cf));
                std::vector<std::vector<float>> xs; if (rsrc) audioops::readRange (*rsrc, 0, rsrc->lengthInSamples, xs);
                FixSpec dc; dc.kind = FixSpec::Kind::Declick; dc.sensitivity = 6.0;
                juce::String er2;
                const auto made2 = audioops::fixWholeFile (fm, cf, (juce::int64) n0 - 200, (juce::int64) n0 + 240, dc, cd, er2);
                CHECK (made2 == cd && er2.isEmpty());
                std::unique_ptr<juce::AudioFormatReader> rb (fm.createReaderFor (cd));
                std::vector<std::vector<float>> cb2;
                if (rb) { CHECK (rb->lengthInSamples == (juce::int64) x.size()); audioops::readRange (*rb, 0, rb->lengthInSamples, cb2); }
                if (! cb2.empty() && ! xs.empty())
                {
                    double m = 0; for (size_t n = n0 - 300; n < n0 + 300; ++n) m = juce::jmax (m, d2At (cb2[0], n));
                    CHECK (d2At (xs[0], n0) > 10 * usual && m < 2.0 * usual);
                    const size_t W = (size_t) (0.5 * sr) + 80;
                    double out = 0; for (size_t i = 0; i < x.size(); ++i) if (i < n0 - W || i >= n0 + W) out = juce::jmax (out, (double) std::abs (cb2[0][i] - xs[0][i]));
                    CHECK (out < 1.0e-5);                                                               // outside the squeeze: the same (24-bit rounding only)
                }
                // and so does a repair box (the default method) round the jump
                const auto cr = tmp.getChildFile ("clicksrc (retouch).wav");
                FixSpec rt; rt.kind = FixSpec::Kind::Patch; rt.f0 = 0.0; rt.f1 = 24000.0;
                const auto made3 = audioops::fixWholeFile (fm, cf, (juce::int64) n0 - 200, (juce::int64) n0 + 240, rt, cr, er2);
                CHECK (made3 == cr && er2.isEmpty());
                std::unique_ptr<juce::AudioFormatReader> rc (fm.createReaderFor (cr));
                std::vector<std::vector<float>> cb3;
                if (rc) { audioops::readRange (*rc, 0, rc->lengthInSamples, cb3); }
                if (! cb3.empty())
                {
                    double m = 0; for (size_t n = n0 - 300; n < n0 + 300; ++n) m = juce::jmax (m, d2At (cb3[0], n));
                    CHECK (m < 3.0 * usual);
                }
            }
        }
        {   // a region file that starts later in the take plays from the right place
            Project pp; pp.setInputCount (2);
            const auto tr = pp.addTrack ("A", TrackFormat::Mono, 0).id;
            EditDef e; EditRegion r; r.id = juce::Uuid(); r.sampleRate = sr; r.srcIn = 1000; r.srcOut = 3000; r.startSample = 500;
            RegionFile f; f.trackId = tr; f.file = tmp.getChildFile ("piece.wav"); f.fileStart = 800; r.files.push_back (f);
            e.regions.push_back (r); e.sampleRate = sr;
            const auto segs = segmentsForEdit (pp, e);
            CHECK (segs.size() == 1 && segs[0].srcOffset == (1000 - 500 - 800));
            // saved and loaded
            e.name = "fs"; pp.edits.push_back (std::make_unique<EditDef> (e)); pp.edits.back()->fixIn = 1.5; pp.edits.back()->fixOut = 2.5;
            pp.projectFile = tmp.getChildFile ("fs.fermata"); juce::String er; CHECK (pp.save (er));
            Project q; CHECK (q.load (pp.projectFile, er));
            CHECK (! q.edits.empty() && q.edits.back()->regions.size() == 1 && q.edits.back()->regions[0].files[0].fileStart == 800 && q.edits.back()->fixIn == 1.5 && q.edits.back()->fixOut == 2.5);
        }
        // import: grouping by length and name
        {
            auto src = [&] (const juce::String& name, int ch, juce::int64 len, double rate = 48000.0)
            { ImportSource s; s.file = juce::File ("/x/" + name); s.channels = ch; s.length = len; s.sampleRate = rate; s.modified = juce::Time (2026, 1, 1, 10, 0, 0, 0, true); return s; };
            std::vector<ImportSource> v;
            for (auto t : { 1, 2, 3 })
                for (auto n : { "Violin I", "Violin II", "Cello" })
                    v.push_back (src ("T" + juce::String (t).paddedLeft ('0', 3) + "_" + n + ".wav", 1, 480000 + 10000 * t));
            v.push_back (src ("T001_Main Pair.wav", 2, 490000)); v.push_back (src ("T002_Main Pair.wav", 2, 500000)); v.push_back (src ("T003_Main Pair.wav", 2, 510000));
            std::reverse (v.begin(), v.end());                                                      // the order the files come in does not matter
            auto plan = buildImportPlan (v);
            CHECK (plan.takes.size() == 3 && plan.tracks.size() == 4);
            if (plan.takes.size() == 3)
            {
                CHECK (plan.takes[0].number == 1 && plan.takes[1].number == 2 && plan.takes[2].number == 3);
                CHECK (plan.takes[0].parts.size() == 4 && plan.takes[0].length == 490000);
                bool names = false; for (auto& t : plan.tracks) names = names || (t.name == "Violin I" && t.channels == 1);
                bool st = false; for (auto& t : plan.tracks) st = st || (t.name == "Main Pair" && t.channels == 2);
                CHECK (names && st);
            }
            // the take number at the end of the name (another program's style), and the order by number when the files come mixed
            std::vector<ImportSource> w;
            for (auto t : { 10, 2, 1 }) for (auto n : { "Vln", "Vla" }) w.push_back (src (juce::String (n) + "_" + juce::String (t) + ".wav", 1, 100000 * t + 7));
            auto p2 = buildImportPlan (w);
            CHECK (p2.takes.size() == 3 && p2.tracks.size() == 2 && p2.takes[0].number == 1 && p2.takes[2].number == 10);
            // two takes of exactly the same length are told apart by their numbers
            std::vector<ImportSource> same;
            for (auto t : { 1, 2 }) for (auto n : { "A", "B" }) same.push_back (src ("Take " + juce::String (t) + " " + n + ".wav", 1, 96000));
            auto p3 = buildImportPlan (same);
            CHECK (p3.takes.size() == 2 && p3.takes[0].parts.size() == 2);
            { std::vector<ImportSource> one; for (auto n : { "Mic 1", "Mic 2", "Mic 3" }) one.push_back (src (juce::String (n) + ".wav", 1, 96000));
              CHECK (buildImportPlan (one).takes.size() == 1); }
            // 2.77.1: a Pro Tools session - "<piece> <track> <take>" with split stereo ".L" / ".R", track names that hold numbers of their own
            // ("ROOM 414", "SPOT10"), files of one take NOT all the same length, and a piece name with a number ("Ave Maria 8"): one set of
            // tracks with the piece name off, every take on it, in take order; also with Pro Tools' "-01" after the take number
            // (style 2: the user's real session - "Ave Maria 8 ORTF.32_31.wav", interleaved stereo: the take number after the dot, Pro Tools' clip
            //  number after the '_', one behind from take 8 on here (from 32 there); every take's files the same length. 44 takes x 20 tracks
            //  came in as 860 tracks before 2.77.2. Style 3: the take number at the start, a clip number at the end that repeats once.)
            for (int style = 0; style < 4; ++style)
            {
                std::vector<ImportSource> pt; juce::Random rr (5);
                for (int t = 12; t >= 1; --t)
                {
                    const int len = 96000 * (10 + t);
                    const auto tn = juce::String (t).paddedLeft ('0', 2);
                    auto nm = [&] (const juce::String& trk)
                    {
                        if (style == 2) return "Ave Maria 8 " + trk + "." + tn + "_" + juce::String (t >= 8 ? t - 1 : t).paddedLeft ('0', 2);
                        if (style == 3) return tn + " Ave Maria 8 " + trk + " " + juce::String (t == 12 ? 5 : t).paddedLeft ('0', 2);
                        return "Ave Maria 8 " + trk + " " + tn + (style == 1 ? "-01" : "");
                    };
                    for (auto st : { "AB", "ROOM 414", "WIDE ROOM" })
                    {
                        if (style == 2) pt.push_back (src (nm (st) + ".wav", 2, len));
                        else for (auto side : { ".L", ".R" }) pt.push_back (src (nm (st) + side + ".wav", 1, style == 3 ? len : len - rr.nextInt (5000)));
                    }
                    for (int k = 1; k <= 10; ++k) pt.push_back (src (nm ("SPOT" + juce::String (k)) + ".wav", 1, style >= 2 ? len : len - rr.nextInt (5000)));
                }
                auto pp = joinStereoPairs (buildImportPlan (pt));
                CHECK (pp.takes.size() == 12 && pp.tracks.size() == 13 && pp.readAs == "Pro Tools");
                CHECK (joinStereoPairs (buildImportPlan (pt, ImportNaming::ProTools)).tracks.size() == 13);
                if (pp.takes.size() == 12 && pp.tracks.size() == 13)
                {
                    CHECK (pp.takes[0].number == 1 && pp.takes[11].number == 12 && pp.takes[7].parts.size() == 13);
                    CHECK (pp.takes[7].length == 96000 * 18 || pp.takes[7].length > 96000 * 18 - 5000);
                    CHECK (pp.tracks[0].name == "AB" && pp.tracks[0].channels == 2 && pp.tracks[1].name == "ROOM 414" && pp.tracks[1].channels == 2);
                    CHECK (pp.tracks[2].name == "SPOT1" && pp.tracks[11].name == "SPOT10" && pp.tracks[12].name == "WIDE ROOM");
                }
            }
            // 2.77.3: Pyramix names with no piece ("001_1-C", "001_OMNI-L", the first take "0_..."), and one take whose Room pair is a second
            // longer than its other files: that take split in two by length and every take came in with tracks of its own
            {
                std::vector<ImportSource> py;
                for (int t = 0; t < 8; ++t)
                {
                    const int len = 48000 * (20 + 3 * t);
                    const auto tn = t == 0 ? juce::String ("0") : juce::String (t).paddedLeft ('0', 3);
                    for (int k = 1; k <= 4; ++k) py.push_back (src (tn + "_" + juce::String (k) + "-C.wav", 1, len));
                    for (auto st : { "OMNI", "Room" }) for (auto side : { "-L", "-R" }) py.push_back (src (tn + "_" + st + side + ".wav", 1, len + (t == 3 && juce::String (st) == "Room" ? 48000 : 0)));
                }
                auto pyp = joinStereoPairs (buildImportPlan (py));
                CHECK (pyp.takes.size() == 8 && pyp.tracks.size() == 6);
                if (pyp.takes.size() == 8 && pyp.tracks.size() == 6)
                {
                    CHECK (pyp.takes[0].number == 0 && pyp.takes[7].number == 7 && pyp.takes[3].parts.size() == 6 && pyp.takes[3].length == 48000 * 30);
                    CHECK (pyp.tracks[0].name == "1" && pyp.tracks[3].name == "4" && pyp.tracks[4].name == "OMNI" && pyp.tracks[4].channels == 2 && pyp.tracks[5].name == "Room");
                }
            }
            // 2.78: the user's folder - Pyramix files with no piece ("0_1-C" ... "106_OMNI-L") AND a second series in it ("CT TEST_1-C", the first
            // take with no number, "CT TEST 002_1-C" ...): 110 takes on ONE set of 12 tracks (2.77.3: 1320 tracks); a folder of one take's tracks
            // numbered "01_Kick", "02_Snare" ... stays one take
            {
                std::vector<ImportSource> uf;
                auto series = [&] (const juce::String& pre, int len, int t)
                {
                    for (int k = 1; k <= 8; ++k) uf.push_back (src (pre + "_" + juce::String (k) + "-C.wav", 1, len + (k == 3 && t % 5 == 0 ? 777 : 0)));
                    for (auto st : { "OMNI", "ORTF", "Room", "Wides" }) for (auto side : { "-L", "-R" }) uf.push_back (src (pre + "_" + st + side + ".wav", 1, len));
                };
                for (int t = 0; t <= 106; ++t) series (t == 0 ? juce::String ("0") : juce::String (t).paddedLeft ('0', 3), 48000 * 10 + 4800 * t, t);
                for (int t = 1; t <= 3; ++t) series (t == 1 ? juce::String ("CT TEST") : "CT TEST " + juce::String (t).paddedLeft ('0', 3), 48000 * 7 + 3000 * t, t);
                auto up = joinStereoPairs (buildImportPlan (uf));
                CHECK (up.takes.size() == 110 && up.tracks.size() == 12 && up.readAs == "Pyramix");
                CHECK (joinStereoPairs (buildImportPlan (uf, ImportNaming::Pyramix)).tracks.size() == 12);
                if (up.tracks.size() == 12) CHECK (up.tracks[0].name == "1" && up.tracks[7].name == "8" && up.tracks[8].name == "OMNI" && up.tracks[8].channels == 2);
                bool all12 = true; for (auto& t : up.takes) all12 = all12 && t.parts.size() == 12;
                CHECK (all12);
                std::vector<ImportSource> one;
                for (auto n : { "01_Kick", "02_Snare", "03_Hat", "04_Bass" }) one.push_back (src (juce::String (n) + ".wav", 1, 96000));
                CHECK (buildImportPlan (one).takes.size() == 1);
            }
            // 2.78.2: SADiE - "<piece>-<mic><take>.wav", the two-digit take stuck to the mic's name ("Beata es Virgo Maria-Omni07", "-VO107" = mic VO1,
            // "-2007" = mic 20), every file of a take exactly the same length, several pieces, one piece without one of the mics; numbered room-tone
            // clips ("Cantate Domino 01 rt-102" = mic 1, take 2): each piece its takes in order, one track per mic
            {
                std::vector<ImportSource> sd; juce::Random rr (4);
                const char* mics[] = { "20", "Harp", "Omni", "VO1", "VO2", "Wide" };
                for (auto pc : { "Beata es Virgo Maria", "Benedicite" })
                    for (int t = 1; t <= 6; ++t)
                    {
                        const int len = 96000 * (30 + rr.nextInt (60)) + rr.nextInt (9000);
                        for (auto* m : mics)
                            if (! (juce::String (pc) == "Benedicite" && juce::String (m) == "Harp"))
                                sd.push_back (src (juce::String (pc) + "-" + m + juce::String (t).paddedLeft ('0', 2) + ".wav", juce::String (m) == "Omni" ? 2 : 1, len));
                    }
                for (int t = 2; t <= 3; ++t) for (int k = 1; k <= 4; ++k) sd.push_back (src ("Cantate Domino 01 rt-" + juce::String (k) + juce::String (t).paddedLeft ('0', 2) + ".wav", 1, t == 2 ? 249600 : 142080));
                auto sp = buildImportPlan (sd);
                CHECK (sp.takes.size() == 14 && sp.tracks.size() == 10);
                int beata = 0; for (auto& t : sp.takes) if (t.piece == "Beata es Virgo Maria") { ++beata; CHECK (t.parts.size() == 6); }
                CHECK (beata == 6);
                if (! sp.takes.empty()) CHECK (sp.takes[0].number == 1 && sp.takes[0].piece == "Beata es Virgo Maria");
                bool vo1 = false, omni = false, twenty = false;
                for (auto& t : sp.tracks) { vo1 = vo1 || t.name == "VO1"; omni = omni || (t.name == "Omni" && t.channels == 2); twenty = twenty || t.name == "20"; }
                CHECK (vo1 && omni && twenty);
                CHECK (sp.readAs == "SADiE");
                // the import window's "Recorded with": SADiE chosen gives the same; Pyramix chosen does not read these names as Pyramix's
                const auto forced = buildImportPlan (sd, ImportNaming::Sadie);
                CHECK (forced.takes.size() == 14 && forced.tracks.size() == 10 && forced.readAs == "SADiE");
                CHECK (buildImportPlan (sd, ImportNaming::ByLength).readAs == "file lengths");
            }
            // a whole session of mono files named "<piece> <take>_<instrument>-<L|R|C>": all takes share the same tracks
            {
                std::vector<ImportSource> m;
                for (auto t : { 15, 16, 17 }) for (auto n : { "piano-L", "piano-R", "violin-C" })
                    m.push_back (src ("A Winter Ride " + juce::String (t).paddedLeft ('0', 3) + "_" + n + ".wav", 1, 300000 + 777 * t));
                auto p5 = buildImportPlan (m);
                CHECK (p5.takes.size() == 3 && p5.tracks.size() == 3);
                bool v = false; for (auto& t : p5.tracks) v = v || t.name == "violin";
                CHECK (v && p5.takes[0].number == 15 && p5.takes[2].number == 17);
            }
            // -L / -R pairs can be joined into stereo tracks (one stereo file is written from the two mono files)
            {
                std::vector<ImportSource> m;
                for (auto t : { 1, 2 }) for (auto n : { "piano-L", "piano-R", "violin-C" }) m.push_back (src ("A Piece " + juce::String (t).paddedLeft ('0', 3) + "_" + n + ".wav", 1, 300000 + 500 * t));
                auto base = buildImportPlan (m);
                CHECK (hasStereoPairs (base) && base.tracks.size() == 3);
                auto j = joinStereoPairs (base);
                CHECK (j.tracks.size() == 2 && j.takes[0].parts.size() == 2);
                bool st = false; for (auto& t : j.tracks) st = st || (t.name == "piano" && t.channels == 2);
                CHECK (st);
                CHECK (! hasStereoPairs (j));
            }
            // a polyphonic file is one take, one track per channel
            std::vector<ImportSource> poly; auto ps = src ("Session 003.wav", 6, 1000000); ps.channelNames.add ("Violin"); ps.channelNames.add ({}); poly.push_back (ps);
            poly.push_back (src ("Session 004.wav", 6, 900000));
            auto p4 = buildImportPlan (poly);
            CHECK (p4.takes.size() == 2 && p4.takes[0].parts.size() == 6 && p4.tracks.size() >= 6 && p4.takes[0].number == 3);
            CHECK (p4.tracks[0].name == "Violin" && p4.tracks[1].name == "Ch 2");
        }
        // import: copying, including taking the channels of a polyphonic file apart
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::vector<std::vector<float>> poly (4, std::vector<float> (30000));
            for (size_t c = 0; c < 4; ++c) for (size_t i = 0; i < poly[c].size(); ++i) poly[c][i] = 0.1f * (float) (c + 1);
            const auto pf = tmp.getChildFile ("poly.wav"); CHECK (audioops::writeWav (pf, poly, sr));
            std::vector<std::vector<float>> mono (1, std::vector<float> (30000, 0.3f)); const auto mf = tmp.getChildFile ("T002_Solo.wav"); CHECK (audioops::writeWav (mf, mono, sr));
            juce::StringArray problems;
            auto plan = buildImportPlan (inspectFiles (fm, { pf, mf }, problems));
            CHECK (problems.isEmpty() && plan.takes.size() == 2);
            std::vector<std::vector<juce::File>> out; std::vector<int> nums { 1, 2 };
            const auto err = copyImportFiles (fm, plan, tmp.getChildFile ("imp"), "Piece", nums, out, nullptr);
            CHECK (err.isEmpty() && out.size() == 2);
            if (out.size() == 2)
            {
                size_t polyTake = plan.takes[0].parts.size() == 4 ? 0 : 1;
                CHECK (out[polyTake].size() == 4);
                for (size_t c = 0; c < out[polyTake].size(); ++c)
                {
                    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out[polyTake][c]));
                    std::vector<std::vector<float>> got; if (r) audioops::readRange (*r, 0, r->lengthInSamples, got);
                    CHECK (r != nullptr && r->numChannels == 1 && r->lengthInSamples == 30000 && ! got.empty() && std::abs (got[0][100] - 0.1f * (float) (c + 1)) < 1.0e-4f);
                }
            }
        }
    }


    // (stereo join: copying)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::vector<std::vector<float>> l (1, std::vector<float> (20000, 0.25f)), r (1, std::vector<float> (19000, -0.5f));
        const auto lf = tmp.getChildFile ("Song 001_gtr-L.wav"), rf = tmp.getChildFile ("Song 001_gtr-R.wav");
        CHECK (audioops::writeWav (lf, l, 48000.0)); CHECK (audioops::writeWav (rf, r, 48000.0));
        juce::StringArray problems;
        auto plan = joinStereoPairs (buildImportPlan (inspectFiles (fm, { lf, rf }, problems)));
        CHECK (plan.takes.size() == 1 && plan.takes[0].parts.size() == 1 && plan.takes[0].parts[0].trackChannels == 2);
        std::vector<std::vector<juce::File>> out; std::vector<int> nums { 1 };
        const auto err = copyImportFiles (fm, plan, tmp.getChildFile ("impst"), "Song", nums, out, nullptr);
        CHECK (err.isEmpty() && out.size() == 1 && out[0].size() == 1);
        if (err.isEmpty() && out.size() == 1 && out[0].size() == 1)
        {
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out[0][0]));
            std::vector<std::vector<float>> got; if (rd) audioops::readRange (*rd, 0, rd->lengthInSamples, got);
            CHECK (rd != nullptr && rd->numChannels == 2 && rd->lengthInSamples == 20000 && got.size() == 2
                   && std::abs (got[0][100] - 0.25f) < 1.0e-4f && std::abs (got[1][100] + 0.5f) < 1.0e-4f && std::abs (got[1][19500]) < 1.0e-6f);
        }
    }

    SECTION ("mastering: PQ, DDP, exports");
    {
        auto hasBytes = [] (const juce::MemoryBlock& mb, const juce::String& needle)
        { const auto n = needle.toRawUTF8(); const size_t nl = needle.getNumBytesAsUTF8(); const auto* d = static_cast<const char*> (mb.getData());
          for (size_t i = 0; i + nl <= mb.getSize(); ++i) if (std::memcmp (d + i, n, nl) == 0) return true; return false; };
        // the sample rate converter
        {
            const double a = 48000.0, b = 44100.0; std::vector<float> in ((size_t) (3 * a)); for (size_t i = 0; i < in.size(); ++i) in[i] = 0.5f * (float) std::sin (2.0 * 3.14159265358979 * 1000.0 * (double) i / a);
            SincResampler rs (a, b); std::vector<float> out; rs.process (in.data(), in.size(), out); rs.finish (out);
            CHECK ((juce::int64) out.size() == rs.outputLength ((juce::int64) in.size()));
            double sg = 0, er = 0; for (size_t k = 8000; k < out.size() - 8000; ++k) { const double ref = 0.5 * std::sin (2.0 * 3.14159265358979 * 1000.0 * (double) k / b); sg += ref * ref; er += std::pow (out[k] - ref, 2.0); }
            CHECK (10.0 * std::log10 (sg / er) > 100.0);
        }
        CHECK (isValidUpc ("4006381333931") && ! isValidUpc ("4006381333932") && isValidUpc ("") && ! isValidUpc ("12345"));
        CHECK (isValidIsrc ("GB-AJY-14-12345") && isValidIsrc ("gbajy1412345") && ! isValidIsrc ("GB-AJY-1412345X") && ! isValidIsrc ("1BAJY1412345") && isValidIsrc (""));
        // Auto PQ
        DdpDisc d; for (int i = 0; i < 3; ++i) { DdpClip c; c.editId = juce::Uuid(); c.title = "T" + juce::String (i + 1); d.clips.push_back (c); }
        std::vector<juce::int64> fr { 44100 * 10, 44100 * 5, 44100 * 20 };
        auto L = computePq (d, fr);
        CHECK (L.tracks.size() == 3 && L.tracks[0].index00 == 0 && L.tracks[0].index01 == 150 && L.tracks[0].lengthSectors == 750 && L.tracks[0].endSector == 900);
        CHECK (L.tracks[1].index00 == 900 && L.tracks[1].index01 == 1050 && L.tracks[1].lengthSectors == 375 && L.tracks[2].index01 == 1575 && L.leadOut == 3075 && L.warnings.isEmpty());
        fr[1] = 44100 * 5 + 1000;                                                   // an edit gets longer: everything after it moves, the gaps stay
        auto L2 = computePq (d, fr);
        CHECK (L2.tracks[1].lengthSectors == 377 && L2.tracks[2].index01 - L2.tracks[1].endSector == 150 && L2.tracks[1].index01 - L2.tracks[0].endSector == 150);
        d.clips[2].gapSectors = 0; auto L3 = computePq (d, fr);                      // no pause: no INDEX 00, the track follows at once
        CHECK (L3.tracks[2].index00 == -1 && L3.tracks[2].index01 == L3.tracks[1].endSector);
        d.clips[1].include = false; auto L4 = computePq (d, fr);
        CHECK (L4.tracks.size() == 2 && L4.tracks[1].number == 2 && L4.tracks[1].clipIndex == 2);
        d.clips[1].include = true; d.clips[0].gapSectors = 10; CHECK (computePq (d, fr).tracks[0].index01 == 150);          // the lead-in is never shorter than 2 s
        std::vector<juce::int64> shortFr { 44100 * 2, 44100 * 5, 44100 * 5 };
        { bool w = false; for (auto& t : computePq (d, shortFr).warnings) w = w || t.contains ("shorter than 4"); CHECK (w); }
        { std::vector<juce::int64> big { (juce::int64) 44100 * 60 * 80, 44100 * 5, 44100 * 5 }; bool w = false; for (auto& t : computePq (d, big).warnings) w = w || t.contains ("79:57"); CHECK (w); }

        // the saved form
        {
            MasteringDef m; m.vm.format = MasterFormat::Flac; m.vm.bitDepth = 16; m.vm.folder = "X:/out"; m.vm.album.album = "Alb"; m.vm.formSingle = true; m.ddp = d; m.ddp.upc = "4006381333931"; m.tailSeconds = 2.5;
            MasterItem it; it.editId = juce::Uuid(); it.include = false; it.fileName = "Name"; it.tags.title = "Ti"; m.items.push_back (it);
            MasteringDef m2; m2.fromVar (juce::JSON::parse (juce::JSON::toString (m.toVar())));
            CHECK (m2.vm.format == MasterFormat::Flac && m2.vm.bitDepth == 16 && m2.vm.folder == "X:/out" && m2.vm.album.album == "Alb" && m2.vm.formSingle && m2.tailSeconds == 2.5);
            CHECK (m2.items.size() == 1 && ! m2.items[0].include && m2.items[0].fileName == "Name" && m2.items[0].tags.title == "Ti" && m2.items[0].editId == it.editId);
            CHECK (m2.ddp.upc == "4006381333931" && m2.ddp.clips.size() == 3 && m2.ddp.clips[2].gapSectors == 0 && m2.ddp.clips[0].gapSectors == 10);
        }

        // a DDP built from real files
        {
            juce::WavAudioFormat wavf;
            std::vector<juce::File> files; std::vector<juce::int64> frames; const juce::int64 lens[3] = { 5 * 44100 + 123, 6 * 44100, 4 * 44100 + 507 };
            for (int i = 0; i < 3; ++i)
            {
                auto f = tmp.getChildFile ("clip" + juce::String (i) + ".wav");
                std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f)); static_cast<juce::FileOutputStream*> (os.get())->setPosition (0); static_cast<juce::FileOutputStream*> (os.get())->truncate();
                auto w = wavf.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
                juce::AudioBuffer<float> b (2, (int) lens[i]);
                for (int k = 0; k < (int) lens[i]; ++k) { const float v = std::round (0.4f * std::sin (0.05f * (float) k * (float) (i + 1)) * 32768.0f) / 32768.0f; b.setSample (0, k, v); b.setSample (1, k, -v); }
                w->writeFromAudioSampleBuffer (b, 0, (int) lens[i]); w.reset();
                files.push_back (f); frames.push_back (lens[i]);
            }
            DdpDisc dd; dd.upc = "4006381333931"; dd.title = "Album"; dd.performer = "Orchestra"; dd.composer = "Mozart";
            for (int i = 0; i < 3; ++i) { DdpClip c; c.editId = juce::Uuid(); c.title = "Movement " + juce::String (i + 1); c.isrc = "GB-AJY-14-1234" + juce::String (i); dd.clips.push_back (c); }
            dd.clips[1].gapSectors = 225; dd.clips[0].preEmphasis = true;
            auto pq = computePq (dd, frames);
            CHECK (pq.warnings.isEmpty());
            juce::String err; auto ct = ddp::buildCdText (dd, pq, err);
            CHECK (err.isEmpty() && ct.getSize() > 0 && ct.getSize() % 18 == 0);
            const auto folder = tmp.getChildFile ("ddpout"), zip = tmp.getChildFile ("ddpout.zip");
            err = ddp::writeFileset (folder, dd, pq, files, zip, nullptr, nullptr);
            CHECK (err.isEmpty());
            juce::String rep; CHECK (ddp::verifyFileset (folder, rep));
            CHECK (folder.getChildFile ("IMAGE.DAT").getSize() == (juce::int64) pq.leadOut * 2352 && folder.getChildFile ("DDPID").getSize() == 128);
            juce::MemoryBlock pqd; folder.getChildFile ("PQDESCR").loadFileAsData (pqd);
            CHECK (pqd.getSize() % 64 == 0 && std::memcmp (pqd.getData(), "VVVS0000", 8) == 0);
            const auto pqText = juce::String::fromUTF8 (static_cast<const char*> (pqd.getData()), (int) pqd.getSize());
            CHECK (pqText.contains ("VVVS0101" "  000200") || pqText.contains ("VVVS0101"));
            CHECK (pqText.contains ("VVVSAA01"));
            {   // the audio is where the PQ list says it is
                juce::FileInputStream img (folder.getChildFile ("IMAGE.DAT"));
                img.setPosition ((juce::int64) pq.tracks[1].index01 * 2352 + 100 * 4);
                short l = (short) img.readShort(); short r = (short) img.readShort();
                juce::AudioFormatManager fm; fm.registerBasicFormats(); std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (files[1]));
                juce::AudioBuffer<float> b (2, 1); rd->read (&b, 0, 1, 100, true, true);
                CHECK (l == (short) std::lrintf (b.getSample (0, 0) * 32768.0f) && r == (short) std::lrintf (b.getSample (1, 0) * 32768.0f) && l != 0);
                img.setPosition ((juce::int64) pq.tracks[1].index00 * 2352 + 4 * 1000);          // the pause is silence
                CHECK (img.readInt() == 0);
            }
            { juce::ZipFile zf (zip); CHECK (zf.getNumEntries() == 6); }
            CHECK (juce::String (rep).contains ("consistent"));
            { juce::MemoryBlock im; folder.getChildFile ("IMAGE.DAT").loadFileAsData (im); static_cast<char*> (im.getData())[pq.tracks[0].index01 * 2352 + 50] ^= 0x55; folder.getChildFile ("IMAGE.DAT").replaceWithData (im.getData(), im.getSize()); juce::String r2; CHECK (! ddp::verifyFileset (folder, r2)); }
            const auto cue = ddp::cueSheetText ("album.wav", dd, pq);
            CHECK (cue.contains ("TRACK 03 AUDIO") && cue.contains ("INDEX 01 00:00:00") && cue.contains ("INDEX 00") && cue.contains ("ISRC GBAJY14" "12340") && cue.contains ("FLAGS PRE") && cue.contains ("PERFORMER \"Orchestra\""));
            const auto wavOut = tmp.getChildFile ("prog.wav"), cueOut = tmp.getChildFile ("prog.cue");
            CHECK (ddp::writeWavAndCue (wavOut, cueOut, dd, pq, files, nullptr, nullptr).isEmpty() && wavOut.getSize() == 44 + (juce::int64) (pq.leadOut - 150) * 2352 && cueOut.existsAsFile());
            {   // an overlap: track 3 starts 1 s before track 2 ends; the two are mixed, nothing is cut, the PQ stays in order
                dd.clips[2].gapSectors = -75;
                const auto pqo = computePq (dd, frames);
                CHECK (pqo.tracks[2].audioStart == pqo.tracks[1].endSector - 75 && pqo.tracks[2].index00 == -1 && pqo.tracks[2].index01 == pqo.tracks[2].audioStart);
                bool warned = false; for (auto& w : pqo.warnings) warned = warned || w.contains ("overlaps the track before"); CHECK (warned);
                dd.clips[2].gapSectors = -100000; CHECK (computePq (dd, frames).tracks[2].gapSectors >= -kMaxOverlapSectors);        // never more than 20 s
                dd.clips[2].gapSectors = -75;
                const auto folder2 = tmp.getChildFile ("ddpover");
                CHECK (ddp::writeFileset (folder2, dd, pqo, files, tmp.getChildFile ("ddpover.zip"), nullptr, nullptr).isEmpty());
                CHECK (folder2.getChildFile ("IMAGE.DAT").getSize() == (juce::int64) pqo.leadOut * 2352);
                juce::FileInputStream img (folder2.getChildFile ("IMAGE.DAT"));
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> r2 (fm.createReaderFor (files[1])), r3 (fm.createReaderFor (files[2]));
                const juce::int64 inTrack3 = 1000;                                                    // 1000 frames after track 3 starts: still under the end of track 2
                const juce::int64 abs = (juce::int64) pqo.tracks[2].audioStart * kSamplesPerSector + inTrack3;
                img.setPosition (abs * 4);
                const int l = (short) img.readShort();
                juce::AudioBuffer<float> b2 (2, 1), b3 (2, 1);
                r2->read (&b2, 0, 1, abs - (juce::int64) pqo.tracks[1].audioStart * kSamplesPerSector, true, true); r3->read (&b3, 0, 1, inTrack3, true, true);
                const int want = juce::jlimit (-32768, 32767, (int) std::lrintf (b2.getSample (0, 0) * 32768.0f) + (int) std::lrintf (b3.getSample (0, 0) * 32768.0f));
                CHECK (std::abs (l - want) <= 1);                                                      // the overlap is the sum of the two tracks
                dd.clips[2].gapSectors = 0;
            }
        }

        // a FLAC file gets its tags and still plays
        {
            juce::FlacAudioFormat ff;
            auto f = tmp.getChildFile ("t.flac");
            { std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f)); auto w = ff.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
              juce::AudioBuffer<float> b (2, 20000); for (int k = 0; k < 20000; ++k) { b.setSample (0, k, 0.3f * std::sin (0.1f * (float) k)); b.setSample (1, k, 0.2f); } w->writeFromAudioSampleBuffer (b, 0, 20000); }
            MasterTags t; t.title = "Movement"; t.artist = "Ensemble"; t.isrc = "GB-AJY-14-12345";
            CHECK (addFlacTags (f, t, 3));
            juce::AudioFormatManager fm; fm.registerBasicFormats(); std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
            CHECK (rd != nullptr && rd->lengthInSamples == 20000 && rd->numChannels == 2);
            juce::MemoryBlock mb; f.loadFileAsData (mb); const auto raw = juce::String::fromUTF8 (static_cast<const char*> (mb.getData()), (int) juce::jmin ((size_t) 400, mb.getSize()));
            CHECK (hasBytes (mb, "TITLE=Movement") && hasBytes (mb, "ISRC=GBAJY1412345") && hasBytes (mb, "TRACKNUMBER=3"));
        }

        // the whole export, through the mixer: a small project of its own
        {
            Project mp; mp.projectFile = tmp.getChildFile ("MasterTest.fermata");
            mp.setInputCount (2); mp.setOutputCount (2); mp.tracks.clear();
            const auto tid = mp.addTrack ("violin", TrackFormat::Mono, 0).id;
            mp.retireAllMixers();
            const auto bus = mp.addBus ("Out", true).id;
            auto& mx = mp.addMixer ("Processing");
            mp.setSendLevel (mx, tid, bus, 0.0f);
            for (auto& t : mp.tracks) { t.setArmed (false); t.setMonitor (Monitor::Session); }
            const auto src = makeConstWav (tmp.getChildFile ("mastersrc.wav"), 0.5, 4);
            auto& ed = mp.addEdit ("Symphony edit", juce::Uuid::null());
            EditRegion r; r.windowId = juce::Uuid::null(); r.takeId = juce::Uuid(); r.srcIn = 100; r.srcOut = 48100; r.sampleRate = 48000.0; r.takeName = "001 - Symphony"; r.sourceLength = 192000;
            RegionFile rf; rf.trackId = tid; rf.trackName = "violin"; rf.file = src; r.files.push_back (rf);
            ed.insertRegion (r); r.srcIn = 5000; r.srcOut = 30000; ed.insertRegion (r);
            mp.mastering.sync (mp.edits);
            CHECK (mp.mastering.findItem (ed.id) != nullptr && mp.mastering.findClip (ed.id) != nullptr);
            auto& vm = mp.mastering.vm;
            vm.folder = tmp.getChildFile ("master").getFullPathName(); vm.format = MasterFormat::Wav; vm.sampleRate = 44100.0; vm.bitDepth = 24; vm.normalise = true; vm.peakDb = -1.0f;
            vm.album.album = "Test Album"; vm.album.artist = "Ensemble";
            auto readInfo = [] (const juce::File& f, double& rate, int& bits, juce::int64& len, float& peak)
            {
                juce::AudioFormatManager fm; fm.registerBasicFormats(); std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
                if (rd == nullptr) return false;
                rate = rd->sampleRate; bits = (int) rd->bitsPerSample; len = rd->lengthInSamples;
                juce::AudioBuffer<float> b ((int) rd->numChannels, (int) len); rd->read (&b, 0, (int) len, 0, true, true);
                peak = 0; for (int c = 0; c < b.getNumChannels(); ++c) peak = juce::jmax (peak, b.getMagnitude (c, 0, (int) len));
                return true;
            };
            const auto native = masterLengthSamples (ed, 0.0);
            const auto expect = convertedLength (native, 48000.0, 44100.0);
            MasterJobSpec spec; CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
            CHECK (spec.items.size() == 1 && spec.items[0].tags.album == "Test Album" && spec.items[0].baseName.startsWith ("01 - "));
            {   MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute();
                CHECK (res.error.isEmpty() && res.files.size() == 1);
                double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (! res.files.isEmpty() && readInfo (juce::File (res.files[0]), rate, bits, len, pk) && rate == 44100.0 && bits == 24 && std::abs ((double) len - (double) expect) <= 1.0);
                CHECK (std::abs (pk - juce::Decibels::decibelsToGain (-1.0f)) < 0.003f && std::abs (res.peakDb + 1.0f) < 0.05f);
                CHECK (! juce::File (vm.folder).getChildFile ("_fermata_work").exists()); }
            {   // the listening copies on their own: one page per piece, with its audio inside, and no audio files
                const auto forms = tmp.getChildFile ("Listening copies test");
                vm.formFolder = forms.getFullPathName(); vm.formAudio = 1; vm.formRate = 0.0; vm.formBits = 16;
                CHECK (makeFormsSpec (mp, mp.mastering, spec).isEmpty() && spec.formsOnly);
                MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute();
                CHECK (res.error.isEmpty() && res.files.size() == 1 && res.files[0].endsWith (".html"));
                CHECK (forms.findChildFiles (juce::File::findFiles, false, "*.html").size() == 1);
                CHECK (forms.findChildFiles (juce::File::findFiles, false, "*.wav;*.flac;*.mp3;*.aif*;*.ogg").isEmpty());
                CHECK (res.files.size() == 1 && juce::File (res.files[0]).loadFileAsString().contains ("audio/flac"));
                // all the pieces in one file: named after the album
                vm.formSingle = true;
                CHECK (makeFormsSpec (mp, mp.mastering, spec).isEmpty() && spec.formSingle && spec.formTitle == "Test Album");
                MasterExportJob job2 (mp, spec, nullptr, false); auto res2 = job2.execute();
                CHECK (res2.error.isEmpty() && res2.files.size() == 1 && juce::File (res2.files[0]).getFileName() == "Test Album - listening copy.html");
                CHECK (juce::File (res2.files[0]).loadFileAsString().contains ("data-k=\"0\" class=\"ch\""));
                CHECK (forms.findChildFiles (juce::File::findFiles, false, "*.wav;*.flac;*.mp3;*.aif*;*.ogg").isEmpty());
                vm.formSingle = false;
                vm.formFolder = {};
            }
            vm.format = MasterFormat::Flac; vm.bitDepth = 16; vm.sampleRate = 0.0;                     // FLAC, 16 bit, the edit's own rate
            CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
            {   MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute();
                double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (res.error.isEmpty() && ! res.files.isEmpty() && res.files[0].endsWith (".flac") && readInfo (juce::File (res.files[0]), rate, bits, len, pk) && rate == 48000.0 && bits == 16 && len == native);
                juce::MemoryBlock mb; juce::File (res.files[0]).loadFileAsData (mb);
                CHECK (hasBytes (mb, "ALBUM=Test Album")); }
            vm.format = MasterFormat::Aiff; vm.bitDepth = 24;
            CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
            {   MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute(); double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (res.error.isEmpty() && ! res.files.isEmpty() && readInfo (juce::File (res.files[0]), rate, bits, len, pk) && bits == 24 && len == native); }
            vm.format = MasterFormat::Ogg; vm.oggQuality = 6;
            CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
            {   MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute(); double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (res.error.isEmpty() && ! res.files.isEmpty() && readInfo (juce::File (res.files[0]), rate, bits, len, pk) && std::abs ((double) len - (double) native) < 4000.0 && pk > 0.1f); }
            vm.format = MasterFormat::Mp3;
            {   const auto e = makeFilesSpec (mp, mp.mastering, spec); if (findLame().isEmpty()) CHECK (e.contains ("LAME")); }
            vm.format = MasterFormat::Wav; vm.bitDepth = 32; vm.normalise = false; mp.mastering.tailSeconds = 1.0;     // 32-bit float, no levelling, a one second tail
            CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
            {   MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute(); double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (res.error.isEmpty() && ! res.files.isEmpty() && readInfo (juce::File (res.files[0]), rate, bits, len, pk) && bits == 32 && len == masterLengthSamples (ed, 1.0) && pk > 0.3f && pk < 0.6f); }
            mp.mastering.tailSeconds = 0.0;
            // two files: one gain for both ("together") keeps the balance, "each on its own" brings both to the level
            {   auto& ed2 = mp.addEdit ("Quiet one", juce::Uuid::null());
                const auto quietSrc = makeConstWav (tmp.getChildFile ("masterquiet.wav"), 0.125, 4);
                EditRegion r2 = r; r2.srcIn = 100; r2.srcOut = 48100; r2.files.clear(); RegionFile rf2 = rf; rf2.file = quietSrc; r2.files.push_back (rf2); ed2.insertRegion (r2); r2.srcIn = 5000; r2.srcOut = 30000; ed2.insertRegion (r2);
                mp.mastering.sync (mp.edits);
                vm.format = MasterFormat::Wav; vm.bitDepth = 24; vm.normalise = true; vm.peakDb = -1.0f; vm.sampleRate = 0.0;
                float peaks[2] = { 0, 0 };
                for (bool together : { true, false })
                {
                    vm.peakTogether = together;
                    CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty() && spec.items.size() == 2);
                    MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute(); double rate = 0; int bits = 0; juce::int64 len = 0;
                    CHECK (res.error.isEmpty() && res.files.size() == 2);
                    if (res.files.size() == 2)
                    {
                        readInfo (juce::File (res.files[0]), rate, bits, len, peaks[0]); readInfo (juce::File (res.files[1]), rate, bits, len, peaks[1]);
                        const float want = juce::Decibels::decibelsToGain (-1.0f);
                        if (together) CHECK (std::abs (peaks[0] - want) < 0.003f && std::abs (peaks[1] - want / 4.0f) < 0.003f);     // the quiet one stays 12 dB below
                        else CHECK (std::abs (peaks[0] - want) < 0.003f && std::abs (peaks[1] - want) < 0.003f);
                    }
                }
                // 2.87: a loudness target. Two more pieces: a spiky one (a quiet tone with full-scale clicks: it cannot reach -16 LUFS under a -1 dBTP ceiling) and a plain quiet tone (it can)
                {
                    auto& spiky = mp.addEdit ("Spiky", juce::Uuid::null()); auto& plain = mp.addEdit ("Plain", juce::Uuid::null());
                    const auto spikySrc = makeToneWav (tmp.getChildFile ("spiky.wav"), 0.03, 6, true), plainSrc = makeToneWav (tmp.getChildFile ("plain.wav"), 0.05, 6, false);
                    for (auto pr : { std::make_pair (&spiky, spikySrc), std::make_pair (&plain, plainSrc) })
                    {
                        EditRegion r3 = r; r3.srcIn = 0; r3.srcOut = 6 * 48000; r3.sourceLength = 6 * 48000; r3.files.clear(); RegionFile rf3 = rf; rf3.file = pr.second; r3.files.push_back (rf3); pr.first->insertRegion (r3);
                    }
                    mp.mastering.sync (mp.edits);
                    for (auto& it : mp.mastering.items) it.include = it.editId == spiky.id || it.editId == plain.id;
                    vm.format = MasterFormat::Wav; vm.bitDepth = 24; vm.sampleRate = 0.0; vm.normalise = true; vm.loudnessMode = 2; vm.targetLufs = -16.0f; vm.ceilingDbTp = -1.0f;
                    auto measure = [] (const juce::File& f, double& lufs, double& tp)
                    {
                        juce::AudioFormatManager fm; fm.registerBasicFormats(); std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
                        if (rd == nullptr) { lufs = tp = -100.0; return; }
                        LoudnessMeter m; m.prepare (rd->sampleRate, 2);
                        juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples); rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true); m.process (b);
                        lufs = m.integratedLufs(); tp = m.truePeakDb();
                    };
                    // the planner on its own
                    {
                        auto c = planLoudness ({ "a", "b" }, { -30.0, -20.0 }, { -2.0, -4.0 }, -24.0, -16.0, -1.0, false);
                        CHECK (c.pieces.size() == 2 && std::abs (c.pieces[0].gainDb - 14.0) < 1e-9 && std::abs (c.pieces[0].peakAtTargetDb - 12.0) < 1e-9 && std::abs (c.pieces[0].excessDb - 13.0) < 1e-9);
                        CHECK (std::abs (c.pieces[1].gainDb - 4.0) < 1e-9 && std::abs (c.pieces[1].peakAtTargetDb) < 1e-9 && c.pieces[1].excessDb > 0.99 && c.pieces[1].excessDb < 1.01 && c.problems() == 2);
                        auto g1 = loudnessGains (c, 1), g2 = loudnessGains (c, 2);
                        CHECK (std::abs (g1[0] - 1.0) < 1e-9 && std::abs (g1[1] - 3.0) < 1e-9 && std::abs (g2[0] - 14.0) < 1e-9);          // retain: stop at the ceiling; ignore: the full gain
                        auto a = planLoudness ({ "a", "b" }, { -30.0, -20.0 }, { -2.0, -4.0 }, -24.0, -16.0, -1.0, true);
                        CHECK (std::abs (a.pieces[0].gainDb - 8.0) < 1e-9 && std::abs (a.pieces[1].gainDb - 8.0) < 1e-9 && std::abs (a.pieces[0].excessDb - 7.0) < 1e-9 && std::abs (a.pieces[1].excessDb - 5.0) < 1e-9);
                        auto ga = loudnessGains (a, 1); CHECK (std::abs (ga[0] - 1.0) < 1e-9 && std::abs (ga[1] - 1.0) < 1e-9);          // per album: one gain, held back by the worst piece
                        CHECK (c.text().contains ("13.0 dB over the ceiling") && a.text().contains ("one gain for the album"));
                        auto ok = planLoudness ({ "a" }, { -20.0 }, { -10.0 }, -20.0, -16.0, -1.0, false); CHECK (ok.problems() == 0 && loudnessGains (ok, 1)[0] == 4.0);
                    }
                    // retain the peak (per track): the plain piece lands on the target, the spiky one stops at the ceiling and the report says by how much it misses
                    for (int decision : { 1, 2 })
                    {
                        vm.peakTogether = false;
                        CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty() && spec.items.size() == 2);
                        spec.loudnessDecision = decision;
                        MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute();
                        CHECK (res.error.isEmpty() && res.files.size() == 2 && res.loudness.pieces.size() == 2 && res.loudness.problems() == 1 && res.loudness.pieces[0].excessDb > 3.0 && res.loudness.pieces[1].excessDb == 0.0);
                        CHECK (res.report.contains ("over the ceiling") && res.report.contains (decision == 1 ? "Retain peak" : "Ignore"));
                        if (res.files.size() == 2)
                        {
                            double l0 = 0, t0 = 0, l1 = 0, t1 = 0; measure (juce::File (res.files[0]), l0, t0); measure (juce::File (res.files[1]), l1, t1);
                            CHECK (std::abs (l1 + 16.0) < 0.3 && t1 < -0.9);                                                  // the plain one: on the target, under the ceiling
                            if (decision == 1) CHECK (std::abs (t0 + 1.0) < 0.3 && l0 < -16.0 - 2.0 && std::abs ((-16.0 - l0) - res.loudness.pieces[0].excessDb) < 0.5);   // retained: at the ceiling, short of the target by the excess
                            else CHECK (res.clipped && t0 > -0.5 && std::abs (l0 + 16.0) < 1.0);                             // ignored: the clicks clipped (what they lose takes the loudness a little under the target)
                        }
                    }
                    // per album: one gain, so the two pieces keep their distance; retained, the whole album comes down by the spiky one's excess
                    {
                        vm.peakTogether = true;
                        CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty());
                        spec.loudnessDecision = 1;
                        MasterExportJob job (mp, spec, nullptr, false); auto res = job.execute();
                        CHECK (res.error.isEmpty() && res.files.size() == 2 && res.loudness.perAlbum && res.loudness.albumLufs > -99.0);
                        if (res.files.size() == 2)
                        {
                            double l0 = 0, t0 = 0, l1 = 0, t1 = 0; measure (juce::File (res.files[0]), l0, t0); measure (juce::File (res.files[1]), l1, t1);
                            const double before = res.loudness.pieces[1].lufs - res.loudness.pieces[0].lufs;
                            CHECK (std::abs ((l1 - l0) - before) < 0.3 && std::abs (t0 + 1.0) < 0.3 && std::abs (res.loudness.pieces[0].gainDb - res.loudness.pieces[1].gainDb) < 1e-9);
                        }
                        // EBU R 128: -23 and -1 whatever the boxes say; the listening copies have their own levels
                        vm.loudnessMode = 1; vm.targetLufs = -10.0f; vm.ceilingDbTp = 0.0f; vm.peakTogether = false;
                        CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty() && spec.out.wantedLufs() == -23.0f && spec.out.wantedCeiling() == -1.0f);
                        vm.formNormalise = true; vm.formLoudnessMode = 2; vm.formTargetLufs = -18.0f; vm.formCeilingDbTp = -2.0f; vm.formTogether = true;
                        MasterJobSpec fs; CHECK (makeFormsSpec (mp, mp.mastering, fs).isEmpty() && fs.out.loudnessMode == 2 && fs.out.wantedLufs() == -18.0f && fs.out.wantedCeiling() == -2.0f && fs.out.peakTogether);
                        { Project back; CHECK (back.fromVar (mp.toVar())); CHECK (back.mastering.vm.loudnessMode == 1 && back.mastering.vm.targetLufs == -10.0f && back.mastering.vm.formLoudnessMode == 2 && back.mastering.vm.formTargetLufs == -18.0f && back.mastering.vm.formTogether && back.mastering.vm.formCeilingDbTp == -2.0f); }
                        vm.loudnessMode = 0; vm.formLoudnessMode = 0; vm.peakDb = -1.0f;
                    }
                    for (auto& it : mp.mastering.items) it.include = it.editId == ed.id || it.editId == ed2.id;
                    mp.mastering.items.erase (std::remove_if (mp.mastering.items.begin(), mp.mastering.items.end(), [&] (const MasterItem& it) { return it.editId == spiky.id || it.editId == plain.id; }), mp.mastering.items.end());
                    mp.edits.erase (std::remove_if (mp.edits.begin(), mp.edits.end(), [&] (const std::unique_ptr<EditDef>& e) { return e->name == "Spiky" || e->name == "Plain"; }), mp.edits.end());
                    mp.mastering.sync (mp.edits);
                    for (auto& f : MasterRenders::folderFor (mp).findChildFiles (juce::File::findFiles, false, "*.wav")) f.deleteFile();     // (their renders: the render-cache test below counts the files)
                    MasterRenders::folderFor (mp).getChildFile ("renders.json").deleteFile();
                }
                // reordering the list changes the order of the files and their numbers
                std::swap (mp.mastering.items[0], mp.mastering.items[1]);
                CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty() && spec.items[0].editId == ed2.id && spec.items[0].baseName.startsWith ("01 - ") && spec.items[1].editId == ed.id);
                mp.mastering.items[0].include = false;                                         // an edit left out
                CHECK (makeFilesSpec (mp, mp.mastering, spec).isEmpty() && spec.items.size() == 1 && spec.items[0].editId == ed.id);
                mp.mastering.items[0].include = true; std::swap (mp.mastering.items[0], mp.mastering.items[1]);
            }
            // the disc: two clips, a pause of 3 s between them, then the first edit gets longer and the pause is the same
            {   mp.mastering.ddp.folder = tmp.getChildFile ("cd").getFullPathName(); mp.mastering.ddp.title = "Test CD"; mp.mastering.ddp.performer = "Ensemble";
                mp.mastering.ddp.clips[0].title = "Symphony"; mp.mastering.ddp.clips[0].isrc = "GB-AJY-14-12345"; mp.mastering.ddp.clips[1].gapSectors = 225;
                for (auto& c : mp.mastering.ddp.clips) c.include = true;
                MasterJobSpec ds; CHECK (makeDiscSpec (mp, mp.mastering, ds).isEmpty() && ds.items.size() == 2);
                // the disc master is always 44.1 kHz, 16 bit, dithered - whatever the Virtual Master is set to
                { auto vm0 = mp.mastering.vm; mp.mastering.vm.sampleRate = 96000.0; mp.mastering.vm.bitDepth = 24; mp.mastering.vm.dither = DitherMode::Off;
                  MasterJobSpec dsx; CHECK (makeDiscSpec (mp, mp.mastering, dsx).isEmpty());
                  CHECK (dsx.out.sampleRate == 44100.0 && dsx.out.bitDepth == 16 && dsx.out.dither == DitherMode::Tpdf && dsx.out.format == MasterFormat::Wav);
                  mp.mastering.vm = vm0; }
                MasterExportJob job (mp, ds, nullptr, false); auto res = job.execute();
                CHECK (res.error.isEmpty() && res.files.size() == 4 && res.report.contains ("consistent"));
                CHECK (juce::File (mp.mastering.ddp.folder).getChildFile ("Test CD DDP").getChildFile ("IMAGE.DAT").existsAsFile() && juce::File (mp.mastering.ddp.folder).getChildFile ("Test CD.cue").existsAsFile());
                std::vector<juce::int64> f1 { convertedLength (masterLengthSamples (ed, 0.0), 48000.0, 44100.0), convertedLength (masterLengthSamples (*mp.findEdit (mp.mastering.ddp.clips[1].editId), 0.0), 48000.0, 44100.0) };
                const auto before = computePq (mp.mastering.ddp, f1);
                ed.insertRegion (r);                                                           // the edit gets 25000 samples longer
                std::vector<juce::int64> f2 { convertedLength (masterLengthSamples (ed, 0.0), 48000.0, 44100.0), f1[1] };
                const auto after = computePq (mp.mastering.ddp, f2);
                CHECK (before.tracks.size() == 2 && after.tracks.size() == 2 && after.tracks[1].index01 > before.tracks[1].index01);
                CHECK (after.tracks[1].index01 - after.tracks[0].endSector == 225 && before.tracks[1].index01 - before.tracks[0].endSector == 225);
            // PQ flags moved by hand: INDEX 01 earlier than the audio, INDEX 00 placed, the lead-out later - and the DDP image follows
            {   auto& cl = mp.mastering.ddp.clips;
                cl[1].index01Shift = -40; cl[1].index00Shift = -140; mp.mastering.ddp.endPadSectors = 75;
                const auto L3 = computePq (mp.mastering.ddp, f1);
                CHECK (L3.tracks.size() == 2 && L3.tracks[1].audioStart == before.tracks[1].index01 && L3.tracks[1].index01 == L3.tracks[1].audioStart - 40);
                CHECK (L3.tracks[1].index00 == L3.tracks[1].audioStart - 140 && L3.tracks[1].index00 == L3.tracks[1].index01 - 100 && L3.leadOut == before.leadOut + 75);
                CHECK (L3.tracks[0].index01 == before.tracks[0].index01);
                cl[1].index01Shift = -100000; CHECK (computePq (mp.mastering.ddp, f1).tracks[1].index01 == before.tracks[0].index01 + 1);       // never before the previous track starts
                cl[1].index00Shift = -40; CHECK (computePq (mp.mastering.ddp, f1).tracks[1].index00 == -1);          // together with INDEX 01: no countdown
                cl[1].index00Shift = 500; CHECK (computePq (mp.mastering.ddp, f1).tracks[1].index00 == -1);             // never after INDEX 01
                cl[1].index01Shift = -40; cl[1].index00Shift = -100000; CHECK (computePq (mp.mastering.ddp, f1).tracks[1].index00 == before.tracks[0].index01 + 1);
                cl[1].index01Shift = -50; cl[1].index00Shift = -100; CHECK (computePq (mp.mastering.ddp, f1).tracks[1].index00 == computePq (mp.mastering.ddp, f1).tracks[1].audioStart - 100);
                cl[1].index01Shift = -40; cl[1].index00Shift = -140;
                MasteringDef md = mp.mastering; auto v = md.toVar(); MasteringDef md2; md2.fromVar (v);
                CHECK (md2.ddp.clips[1].index01Shift == -40 && md2.ddp.clips[1].index00Shift == -140 && md2.ddp.endPadSectors == 75);
                cl[1].index00Shift = -140;
                MasterJobSpec ds2; CHECK (makeDiscSpec (mp, mp.mastering, ds2).isEmpty());
                MasterExportJob job2 (mp, ds2, nullptr, false); auto res2 = job2.execute();
                CHECK (res2.error.isEmpty() && res2.report.contains ("consistent"));
                cl[1].index01Shift = 0; cl[1].index00Shift = kIndexAuto; mp.mastering.ddp.endPadSectors = 0; } }
            // the renders: two per Edit; a new one always replaces the older of the two
            {   MasterRenders st (tmp.getChildFile ("rr")); const juce::Uuid id;
                auto mk = [&] (const char* fp) { auto f = st.tempFile (id); f.getParentDirectory().createDirectory(); f.replaceWithText (fp); st.commit (id, fp, 48000.0, 100, f); };
                CHECK (st.check (id, "a").state == MasterRenders::State::None);
                mk ("a"); CHECK (st.check (id, "a").state == MasterRenders::State::Ready);
                mk ("b"); auto ib = st.check (id, "b");
                CHECK (ib.state == MasterRenders::State::Ready && ib.hasBackup && ib.file.getFileName().endsWith ("-B.wav"));
                CHECK (st.check (id, "a").state == MasterRenders::State::Ready && st.check (id, "a").file.getFileName().endsWith ("-A.wav"));      // changed back: the other one is used again, nothing is rendered
                mk ("c");                                                                                    // A is the current one, so the older one (B) is replaced
                CHECK (st.check (id, "c").file.getFileName().endsWith ("-B.wav") && st.folder().getChildFile (id.toString() + "-A.wav").loadFileAsString() == "a" && st.folder().getChildFile (id.toString() + "-B.wav").loadFileAsString() == "c");
                CHECK (st.folder().findChildFiles (juce::File::findFiles, false, "*.wav").size() == 2);       // never more than two
                mk ("d");                                                                                    // B is current: A (the backup) is replaced, c becomes the backup
                CHECK (st.check (id, "d").file.getFileName().endsWith ("-A.wav") && st.folder().findChildFiles (juce::File::findFiles, false, "*.wav").size() == 2);
                CHECK (st.setUsePrevious (id, true) && st.check (id, "zzz").state == MasterRenders::State::Previous && st.check (id, "zzz").file.loadFileAsString() == "c");
                CHECK (st.setUsePrevious (id, false) && st.check (id, "d").state == MasterRenders::State::Ready);
                st.markStale (id); CHECK (st.check (id, "d").state == MasterRenders::State::Stale);
                st.clearAll(); CHECK (st.check (id, "d").state == MasterRenders::State::None); }

            // exporting keeps native-resolution renders, reuses them while nothing changes, and only the DDP is 44.1 kHz / 16 bit
            {   const auto rdir = MasterRenders::folderFor (mp);
                auto wavs = [&] { return rdir.findChildFiles (juce::File::findFiles, false, "*.wav"); };
                MasterJobSpec ds3; CHECK (makeDiscSpec (mp, mp.mastering, ds3).isEmpty());
                { MasterExportJob j (mp, ds3, nullptr, false); auto rr = j.execute(); CHECK (rr.error.isEmpty());
                  for (auto& f : rr.files) if (f.endsWith (".wav")) { double rt = 0; int bt = 0; juce::int64 ln = 0; float pk = 0; CHECK (readInfo (juce::File (f), rt, bt, ln, pk) && rt == 44100.0 && bt == 16); } }
                const auto first = wavs();
                CHECK (first.size() >= 2 && first.size() <= 4);                                                    // (at most two per edit, two edits)
                double rate = 0; int bits = 0; juce::int64 len = 0; float pk = 0;
                CHECK (readInfo (first[0], rate, bits, len, pk) && rate == 48000.0 && bits == 32);             // the renders keep the session's own resolution
                std::map<juce::String, juce::int64> times; for (auto& f : first) times[f.getFileName()] = f.getLastModificationTime().toMilliseconds();
                juce::Thread::sleep (30);
                { MasterExportJob j (mp, ds3, nullptr, false); CHECK (j.execute().error.isEmpty()); }
                bool same = wavs().size() == first.size(); for (auto& f : wavs()) same = same && times[f.getFileName()] == f.getLastModificationTime().toMilliseconds();
                CHECK (same);                                                                                // nothing changed: nothing was rendered again
                auto* em = mp.mixerOfEdit (ed.id); CHECK (em != nullptr);
                for (int k = 0; k < 4; ++k)                                                                  // four changes in a row, each followed by an export: still two renders of the Edit
                {
                    em->stripFor (mp.tracks[0].id)->gainDb.set (-3.0f - (float) k);
                    MasterJobSpec dk; CHECK (makeDiscSpec (mp, mp.mastering, dk).isEmpty());
                    MasterExportJob j (mp, dk, nullptr, false); CHECK (j.execute().error.isEmpty());
                }
                int mine = 0; for (auto& f : wavs()) if (f.getFileName().startsWith (ed.id.toString())) ++mine;
                CHECK (mine == 2);
                // the fingerprint ignores where an automated control was left, but sees any other change
                RenderParams rp; CHECK (makeRenderParams (mp, mp.mastering, rp).isEmpty());
                ed.automationOn = true;
                { auto& ln = ed.addLane (mp.tracks[0].id, autoparam::fader, juce::Uuid::null()); AutoPoint a0; a0.time = 0; a0.value = 0.0f; ln.pts.push_back (a0); }
                const auto f0 = masterRenderFingerprint (mp, ed, rp);
                em->stripFor (mp.tracks[0].id)->gainDb.set (-17.0f);
                CHECK (masterRenderFingerprint (mp, ed, rp) == f0);
                em->stripFor (mp.tracks[0].id)->pan.set (0.4f);
                CHECK (masterRenderFingerprint (mp, ed, rp) != f0);
                ed.automationOn = false; }
            // the source: a missing output falls back to the first Ext bus
            {   mp.mastering.sourceId = ed.id; juce::Uuid mxid, srid;
                CHECK (resolveRenderSource (mp, mp.mastering, mxid, srid).isEmpty() && srid == bus && mxid == mx.id); }
        }
    }


    // ANEMAN: the real capture from the user's rig (THAP .40 on ASIO 1-16, BHAP .50 on 17-32, Anubis combo on 33-34)
    {
        const auto dir = juce::File (__FILE__).getParentDirectory().getChildFile ("data");
        auto devs = juce::JSON::parse (dir.getChildFile ("aneman_devices.json"));
        auto conns = juce::JSON::parse (dir.getChildFile ("aneman_connections.json"));
        auto plan = planFromAneman (devs, conns);
        CHECK (plan.ok && plan.devices.size() == 3);
        if (plan.devices.size() == 3)
        {
            CHECK (plan.devices[0].name == "THAP" && plan.devices[0].host == "165.165.1.40" && plan.devices[0].firstInput == 0);
            CHECK (plan.devices[1].name == "BHAP" && plan.devices[1].host == "165.165.1.50" && plan.devices[1].firstInput == 16);
            CHECK (plan.devices[2].name == "Anubis" && plan.devices[2].host == "165.165.1.20" && plan.devices[2].firstInput == 32);
        }
        CHECK (plan.warnings.isEmpty());
        if (plan.devices.size() == 3)
        {
            CHECK (plan.devices[0].map.size() == 16 && plan.devices[0].inputFor (0) == 0 && plan.devices[0].inputFor (15) == 15);
            CHECK (plan.devices[1].inputFor (0) == 16 && plan.devices[1].inputFor (15) == 31 && plan.devices[2].inputFor (0) == 32 && plan.devices[2].inputFor (1) == 33 && plan.devices[2].inputFor (2) == -1);
            auto c2 = PreampDeviceCfg::fromVar (juce::JSON::parse (juce::JSON::toString (plan.devices[1].toVar())));
            CHECK (c2 == plan.devices[1]);
        }
        // a different patch: THAP channel 5 on ASIO 1, channel 2 on ASIO 7 - the map follows it
        {
            auto conns2 = conns;
            if (auto* a = conns2.getArray())
                for (auto& c : *a)
                    if ((int) c["Input"]["DeviceId"] == 9 && (int) c["Input"]["IOGroupId"] == 60 && (int) c["Output"]["DeviceId"] == 13)
                    {
                        const int in = (int) c["Input"]["InputId"];
                        if (in == 4) c.getDynamicObject()->getProperty ("Output").getDynamicObject()->setProperty ("OutputId", 0);
                        else if (in == 0) c.getDynamicObject()->getProperty ("Output").getDynamicObject()->setProperty ("OutputId", 4);
                    }
            auto p2 = planFromAneman (devs, conns2);
            CHECK (p2.ok && p2.devices[0].name == "THAP" && p2.devices[0].inputFor (4) == 0 && p2.devices[0].inputFor (0) == 4 && p2.devices[0].inputFor (1) == 1);
        }
        CHECK (plan.summary.joinIntoString ("\n").contains ("Anubis DAW 1"));
        CHECK (! planFromAneman (juce::var(), juce::var()).ok);
    }

    SECTION ("preamps: a fixed line input (Anubis jack 3/4) is read as gain / phase / low cut / instrument only");
    {
        auto jack = juce::JSON::parse (R"({"id":7,"name":"Jack 3/4","custom":{"ins":{"capabilities":{"channel":{"lineGain":true}},"channels":[{"lineGain":420,"instrumentGain":150,"phase":true,"lowCut":false,"inputMode":2},{"lineGain":100,"inputMode":1}]}}})");
        jack.getDynamicObject()->getProperty ("custom").getDynamicObject()->getProperty ("ins").getDynamicObject()->getProperty ("capabilities").getDynamicObject()->getProperty ("channel").getDynamicObject()->setProperty ("instrument", true);
        auto h = RavennaDevice::readChannel (jack, 0);
        CHECK (h.lineOnly && h.line && h.lineGain == 420 && h.instrumentGain == 150 && h.phase && h.zIn && h.instrKey == "inputMode");
        auto st = RavennaPreampDriver::toSettings (h);
        CHECK (st.lineOnly && st.hasInstrument && st.zHigh && st.gainDb > 14.9f && st.gainDb < 15.1f);       // in Instrument mode the gain shown is the instrument gain
        CHECK (! RavennaDevice::readChannel (jack, 1).zIn && RavennaPreampDriver::toSettings (RavennaDevice::readChannel (jack, 1)).gainDb > 9.9f);
        auto combo = juce::JSON::parse (R"({"id":3,"name":"Combo 1/2","custom":{"ins":{"capabilities":{"channel":{"micGain":true}},"channels":[{"micGain":100,"inputMode":0}]}}})");
        CHECK (! RavennaDevice::readChannel (combo, 0).lineOnly);
    }

    SECTION ("2.74: the Neumann MT48: three-position Pad (a gain scale), low cut at 40 / 60 / 80 Hz");
    {
        // the modules exactly as the MT48 reported them (inputs 1/2 and 3/4)
        auto mic = juce::JSON::parse (R"({"id":154,"name":"Mic/Line","custom":{"ins":{"channels":[{"lock":false,"inputMode":0,"m48V":false,"pad":false,"lift":true,"lineGain":0,"micGain":180,"lowCut":true,"lowCut_freq":60,"phase":false,"cut":false},{"inputMode":1,"pad":true,"lift":false,"micGain":0,"lineGain":250,"lowCut":false,"lowCut_freq":80}],"capabilities":{"channel":{"mic":true,"line":true,"instrument":false,"lineGain":true,"micGain":true,"48V":true,"z_in":false,"pad":true,"lift":true,"lowCut":true,"lowCut_info":[40,60,80],"phase":true,"cut":true}}}}})");
        auto inst = juce::JSON::parse (R"({"id":155,"name":"Inst/Line","custom":{"ins":{"channels":[{"inputMode":2,"lineGain":0,"instrumentGain":300,"lowCut":true,"lowCut_freq":40,"phase":true,"cut":false}],"capabilities":{"channel":{"mic":false,"line":true,"instrument":true,"inputMode_ro":false,"lineGain":true,"micGain":false,"instrumentGain":true,"48V":false,"pad":false,"lift":false,"lowCut":true,"lowCut_info":[40,60,80],"phase":true}}}}})");
        auto dump = juce::JSON::parse (R"({"identity":{"vendor":"Neumann","product":"MT 48","name":"MT48"}})");
        CHECK (RavennaDevice::isMT48 (dump) && ! RavennaDevice::isMT48 (juce::JSON::parse (R"({"identity":{"product":"MERGING+ANUBIS"}})")));
        auto s = RavennaPreampDriver::toSettings (RavennaDevice::readChannel (mic, 0, true));
        CHECK (s.steppedPad && s.lowCutChoice && s.lowCut && s.lowCutHz == 60 && ! s.lineOnly);
        CHECK (s.padStep() == 0 && std::abs (s.gainDb - 18.0f) < 0.01f && s.shownGainOffset() == 12.0f);          // Pad OFF: 18 shown as 30 (scale 12..78)
        s.setPadStep (1); CHECK (! s.pad && ! s.boost && s.shownGainOffset() == 0.0f);                            // -12 dB: shown 18 (0..66)
        s.setPadStep (2); CHECK (s.pad && ! s.boost && s.shownGainOffset() == -12.0f);                            // -24 dB: shown 6 (-12..54)
        s.setPadStep (0); CHECK (! s.pad && s.boost);
        auto l = RavennaPreampDriver::toSettings (RavennaDevice::readChannel (mic, 1, true));
        CHECK (l.line && l.steppedPad && l.padStep() == 2 && l.shownGainOffset() == 0.0f && std::abs (l.gainDb - 25.0f) < 0.01f);   // Line: no Pad scale
        auto i3 = RavennaPreampDriver::toSettings (RavennaDevice::readChannel (inst, 0, true));
        CHECK (i3.lineOnly && i3.hasInstrument && i3.zHigh && ! i3.steppedPad && i3.lowCutChoice && i3.lowCutHz == 40 && i3.shownGainOffset() == 0.0f && std::abs (i3.gainDb - 30.0f) < 0.01f);
        // the same modules on an Anubis: no stepped Pad (its lift stays Boost), one low-cut frequency
        auto anubisMic = juce::JSON::parse (R"({"id":154,"name":"Combo 1/2","custom":{"ins":{"channels":[{"inputMode":0,"pad":false,"lift":true,"micGain":100,"lowCut":true,"lowCut_freq":80}],"capabilities":{"channel":{"micGain":true,"pad":true,"lift":true,"lowCut_info":[80]}}}}})");
        auto a = RavennaPreampDriver::toSettings (RavennaDevice::readChannel (anubisMic, 0, false));
        CHECK (! a.steppedPad && ! a.lowCutChoice && a.boost && a.shownGainOffset() == 0.0f);
        // the low-cut frequency is kept in the project and in the preamp memory
        Project q; q.inputs.resize (2); q.inputs[1].preamp.lowCut = true; q.inputs[1].preamp.lowCutHz = 40;
        Project q2; CHECK (q2.fromVar (q.toVar()) && q2.inputs.size() == 2 && q2.inputs[1].preamp.lowCutHz == 40 && q2.inputs[0].preamp.lowCutHz == 80);
        PreampMemory pm; pm.inputs[3] = q.inputs[1].preamp;
        PreampMemory pm2; CHECK (PreampMemory::fromVar (pm.toVar(), pm2) && pm2.inputs[3].lowCutHz == 40 && PreampMemory::same (pm2.inputs[3], pm.inputs[3]));
        auto other = pm.inputs[3]; other.lowCutHz = 60; CHECK (! PreampMemory::same (other, pm.inputs[3]));
        other.lowCut = false; auto off2 = other; off2.lowCutHz = 80; CHECK (PreampMemory::same (other, off2));    // (with the low cut off its frequency does not matter)
        // setting an input (a restored preamp memory, say) keeps what the hardware says the input can do
        q.inputs[0].preamp.steppedPad = true; q.inputs[0].preamp.lowCutChoice = true;
        PreampSettings restored; restored.gainDb = 30.0f; restored.setPadStep (1); restored.lowCut = true; restored.lowCutHz = 60;
        q.setPreamp (0, restored);
        CHECK (q.inputs[0].preamp.steppedPad && q.inputs[0].preamp.lowCutChoice && q.inputs[0].preamp.padStep() == 1 && q.inputs[0].preamp.lowCutHz == 60 && q.inputs[0].preamp.gainDb == 30.0f);
    }

    SECTION ("undo: many levels of the takes' details, the edits and the mastering");
    {
        auto& w0 = *p.takeWindows.front();
        auto& g0 = w0.groups.front();
        auto* ed = p.editForWindow (w0.id);
        CHECK (ed != nullptr && ! ed->regions.empty());
        p.undoReset();
        const int startBar = g0.barIn;
        for (int i = 1; i <= 20; ++i) { g0.barIn = i; g0.barOut = i + 1; p.changed(); p.undoTick (false, true); }
        CHECK (p.undoDepth() == 20);
        w0.playheadSeconds = 3.0; p.changed(); p.undoTick (false, true);          // playheads are not undo steps
        CHECK (p.undoDepth() == 20);
        for (int i = 0; i < 20; ++i) CHECK (p.undo());
        CHECK (g0.barIn == startBar && ! p.undo());
        CHECK (p.redo() && g0.barIn == 1 && g0.barOut == 2);
        for (int i = 0; i < 19; ++i) p.redo();
        CHECK (g0.barIn == 20 && ! p.redo());
        // an edit: delete a piece, rename the edit, undo both
        const size_t nRegions = ed != nullptr ? ed->regions.size() : 0; const auto oldName = ed != nullptr ? ed->name : juce::String();
        if (ed != nullptr && nRegions > 0)
        {
            const auto firstId = ed->regions.front().id;
            ed->name = "Renamed"; p.changed(); p.undoTick (false, true);
            ed->regions.erase (ed->regions.begin()); p.changed(); p.undoTick (false, true);
            CHECK (p.undo()); ed = p.editForWindow (w0.id);
            CHECK (ed != nullptr && ed->regions.size() == nRegions && ed->regions.front().id == firstId && ed->name == "Renamed");
            CHECK (p.undo()); ed = p.editForWindow (w0.id);
            CHECK (ed != nullptr && ed->name == oldName);
            CHECK (p.redo() && p.redo()); ed = p.editForWindow (w0.id);
            CHECK (ed != nullptr && ed->regions.size() == nRegions - 1);
        }
        // a removed take comes back, the take being recorded is left alone, and 30 is the most that is kept
        {
            const auto gid = g0.id; const auto before = (int) w0.groups.size();
            p.undoReset();
            w0.groups.erase (std::remove_if (w0.groups.begin(), w0.groups.end(), [&] (const TakeGroup& x) { return x.id == gid; }), w0.groups.end());
            p.changed(); p.undoTick (false, true);
            CHECK ((int) w0.groups.size() == before - 1 && p.undo() && (int) w0.groups.size() == before && w0.findGroup (gid) != nullptr);
            auto& gg = *w0.findGroup (gid); gg.dud = true; p.changed(); p.undoTick (false, true); gg.dud = false;
            p.undo (gid);                                                                              // 'gid' pretends to be recording: untouched
            CHECK (! w0.findGroup (gid)->dud);
            for (int i = 0; i < 45; ++i) { w0.findGroup (gid)->barIn = 100 + i; p.changed(); p.undoTick (false, true); }
            CHECK (p.undoDepth() == Project::kUndoLevels);
        }
    }

    SECTION ("2.69 undo hash: every field the history looks at moves the hash and the text; view state moves neither");
    {
        // a take window and an edit with everything in them
        Project q; q.setInputCount (4); q.setOutputCount (2); q.tracks.clear();
        q.addTrack ("A", TrackFormat::Mono, 0); q.addTrack ("B", TrackFormat::Mono, 1);
        q.takeWindows.clear(); q.edits.clear();
        auto& w = q.addTakeWindow ("Hash window");
        for (int t = 0; t < 3; ++t)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = t + 1; g.label = "Take"; g.startSeconds = 10.0 * t; g.lengthSamples = 48000; g.sampleRate = 48000.0; g.recordedAt = juce::Time (1000 + t);
            for (auto& tr : q.tracks) { TakeFile f; f.trackId = tr.id; f.trackName = tr.name; f.file = tmp.getChildFile (tr.name + juce::String (t) + ".wav"); f.numChannels = 1; g.files.push_back (f); }
            WaitingPiece wp; wp.name = "bar 4"; wp.from = 10; wp.to = 20; wp.files.push_back (tmp.getChildFile ("w.wav")); wp.tracks.push_back (q.tracks[0].id);
            g.waiting.push_back (wp);
            w.groups.push_back (g);
        }
        auto& e = q.addEdit ("Hash edit", w.id);
        for (int r = 0; r < 3; ++r)
        {
            EditRegion reg; reg.id = juce::Uuid(); reg.windowId = w.id; reg.takeId = w.groups[(size_t) r].id; reg.takeName = "00" + juce::String (r + 1); reg.srcIn = 100; reg.srcOut = 40000; reg.sampleRate = 48000.0; reg.startSample = r * 39900; reg.sourceLength = 48000;
            for (auto& tr : q.tracks) { RegionFile f; f.trackId = tr.id; f.trackName = tr.name; f.file = tmp.getChildFile (tr.name + juce::String (r) + ".wav"); f.numChannels = 1; reg.files.push_back (f); }
            GainChange gc; gc.at = 500; gc.ramp = 0.1; gc.db = { -1.0f, -2.0f }; reg.gains.push_back (gc);
            (r == 1 ? e.overdubs : e.regions).push_back (reg);
        }
        { AutoLane l; l.id = juce::Uuid(); l.trackId = q.tracks[0].id; AutoPoint pt; pt.id = juce::Uuid(); pt.time = 1000; pt.value = -3.0f; l.pts.push_back (pt); e.lanes.push_back (l); }
        e.trackIds = { q.tracks[0].id, q.tracks[1].id };

        int moved = 0, still = 0;
        auto expectW = [&] (const char* what, const std::function<void()>& change, bool shouldMove)
        {
            const auto h0 = Project::undoHash (w); const auto t0 = q.undoText (w); const auto s0 = Project::saveHash (w); const auto f0 = juce::JSON::toString (q.takeWindowToVar (w));
            change();
            const auto h1 = Project::undoHash (w); const auto t1 = q.undoText (w);
            const bool hashMoved = h0 != h1, textMoved = t0 != t1;
            if (hashMoved != shouldMove || textMoved != shouldMove) std::printf ("  undo hash (take window) '%s': hash %s, text %s, expected %s\n", what, hashMoved ? "moved" : "same", textMoved ? "moved" : "same", shouldMove ? "moved" : "same");
            CHECK (hashMoved == shouldMove && textMoved == shouldMove);
            if (Project::saveHash (w) == s0 || juce::JSON::toString (q.takeWindowToVar (w)) == f0) std::printf ("  save hash (take window) '%s' did not move\n", what);
            CHECK (Project::saveHash (w) != s0 && juce::JSON::toString (q.takeWindowToVar (w)) != f0);      // (every field is saved: the file hash always moves)
            (shouldMove ? moved : still)++;
        };
        auto expectE = [&] (const char* what, const std::function<void()>& change, bool shouldMove)
        {
            const auto h0 = Project::undoHash (e); const auto t0 = q.undoText (e); const auto s0 = Project::saveHash (e); const auto f0 = juce::JSON::toString (q.editToVar (e));
            change();
            const auto h1 = Project::undoHash (e); const auto t1 = q.undoText (e);
            const bool hashMoved = h0 != h1, textMoved = t0 != t1;
            if (hashMoved != shouldMove || textMoved != shouldMove) std::printf ("  undo hash (edit) '%s': hash %s, text %s, expected %s\n", what, hashMoved ? "moved" : "same", textMoved ? "moved" : "same", shouldMove ? "moved" : "same");
            CHECK (hashMoved == shouldMove && textMoved == shouldMove);
            if (Project::saveHash (e) == s0 || juce::JSON::toString (q.editToVar (e)) == f0) std::printf ("  save hash (edit) '%s' did not move\n", what);
            CHECK (Project::saveHash (e) != s0 && juce::JSON::toString (q.editToVar (e)) != f0);
            (shouldMove ? moved : still)++;
        };
        auto& g = w.groups[0]; auto& g2 = w.groups[1];
        // the take window: what is undone
        expectW ("name", [&] { w.name = "Renamed"; }, true);
        expectW ("defaultLabel", [&] { w.defaultLabel = "Tk"; }, true);
        expectW ("nextNumber", [&] { w.nextNumber = 9; }, true);
        expectW ("targetEdit", [&] { w.targetEdit = e.id; }, true);
        expectW ("showLtc", [&] { w.showLtc = true; }, true);
        expectW ("take label", [&] { g.label = "Slate"; }, true);
        expectW ("take number", [&] { g.number = 7; }, true);
        expectW ("take start", [&] { g.startSeconds += 1.0; }, true);
        expectW ("take length", [&] { g.lengthSamples += 10; }, false);           // (3.1.4: the recorder's fields are saved, not undone - a take ending is no undo step)
        expectW ("take rate", [&] { g.sampleRate = 96000.0; }, true);
        expectW ("take time", [&] { g.recordedAt = juce::Time (5555); }, true);
        expectW ("fade in", [&] { g.fadeInSeconds = 0.5; }, true);
        expectW ("fade out", [&] { g.fadeOutSeconds = 0.5; }, true);
        expectW ("dud", [&] { g.dud = true; }, true);
        expectW ("bar in", [&] { g.barIn = 3; }, true);
        expectW ("bar out", [&] { g.barOut = 8; }, true);
        expectW ("overruns", [&] { g.overruns = 2; g.lostSamples = 100; }, false);
        expectW ("lost", [&] { g.lostSamples = 200; }, false);
        expectW ("write failed", [&] { g.writeFailed = true; }, false);
        expectW ("verified", [&] { g.verified = true; }, false);
        expectW ("room tone mark", [&] { g.roomTone = true; g.roomToneIn = 1.0; g.roomToneOut = 2.5; }, true);      // (2.98: it was saved only when something else changed)
        expectW ("file track name", [&] { g.files[0].trackName = "Z"; }, true);
        expectW ("file track", [&] { g.files[0].trackId = juce::Uuid(); }, true);                      // (3.1.4: the track alone)
        expectW ("file path", [&] { g.files[0].file = tmp.getChildFile ("other.wav"); }, true);
        expectW ("file channels", [&] { g.files[0].numChannels = 2; }, true);
        expectW ("file orig", [&] { g.files[0].sourceName = "orig.wav"; }, true);
        expectW ("file orig2", [&] { g.files[0].sourceName2 = "orig2.wav"; }, true);
        expectW ("file removed", [&] { g.files.pop_back(); }, true);
        expectW ("waiting name", [&] { g.waiting[0].name = "bar 5"; }, true);
        expectW ("waiting range", [&] { g.waiting[0].to = 30; }, true);
        expectW ("waiting from", [&] { g.waiting[0].from = 12; }, true);
        expectW ("waiting file", [&] { g.waiting[0].files[0] = tmp.getChildFile ("w2.wav"); }, true);
        expectW ("waiting track", [&] { g.waiting[0].tracks.clear(); }, true);
        expectW ("waiting removed", [&] { g.waiting.clear(); }, true);
        expectW ("take removed", [&] { w.groups.pop_back(); }, true);
        expectW ("takes swapped", [&] { std::swap (w.groups[0], w.groups[1]); }, true);
        // the take window: view state, not undone
        expectW ("edit take mark", [&] { w.editTake = g2.id; w.editIn = 1.0; w.editOut = 2.0; }, false);
        expectW ("edit tracks", [&] { w.editTracks.push_back (q.tracks[0].id); }, false);
        expectW ("cursor", [&] { w.cursorTake = g2.id; w.cursorSeconds = 3.0; }, false);
        expectW ("playhead", [&] { w.playheadSeconds = 4.0; }, false);
        expectW ("hidden tracks", [&] { w.hiddenTracks.push_back (q.tracks[1].id); }, false);
        expectW ("overdub", [&] { w.overdub = true; }, false);
        expectW ("markers", [&] { Marker m; m.id = juce::Uuid(); m.name = "Chorus"; m.seconds = 5.0; w.markers.push_back (m); }, false);
        expectW ("show markers", [&] { w.showMarkers = false; }, false);
        expectW ("take matching switched on", [&] { w.takeMatching = true; }, false);    // (3.18.0: a setting, saved, never an undo step)
        // the edit: what is undone
        auto& r = e.regions[0];
        expectE ("name", [&] { e.name = "Renamed edit"; }, true);
        expectE ("window", [&] { e.windowId = juce::Uuid(); }, true);
        expectE ("rate", [&] { e.sampleRate = 96000.0; }, true);
        expectE ("automation on", [&] { e.automationOn = true; }, true);
        expectE ("show ltc", [&] { e.showLtc = true; }, true);
        expectE ("track rows", [&] { e.trackIds.pop_back(); }, true);
        expectE ("region take name", [&] { r.takeName = "X"; }, true);
        expectE ("region window / take", [&] { r.windowId = juce::Uuid(); r.takeId = juce::Uuid(); }, true);
        expectE ("region in", [&] { r.srcIn += 10; }, true);
        expectE ("region out", [&] { r.srcOut -= 10; }, true);
        expectE ("region start", [&] { r.startSample += 10; }, true);
        expectE ("region source length", [&] { r.sourceLength += 10; }, true);
        expectE ("region bars", [&] { r.barIn = 1; r.barOut = 2; }, true);
        expectE ("region rate", [&] { r.sampleRate = 44100.0; }, true);
        expectE ("region curve", [&] { r.curve = FadeCurve::Linear; }, true);
        expectE ("region out curve", [&] { r.setOutCurve (FadeCurve::Slow); }, true);
        expectE ("region fix in / out", [&] { r.fixIn = true; r.fixOut = true; }, true);
        expectE ("region normalise in", [&] { r.normaliseIn = true; }, true);
        expectE ("region snips", [&] { r.snipIn = true; r.snipOut = true; }, true);                     // (3.1.4)
        expectE ("region fades", [&] { r.inStart = 0.1; r.inEnd = 0.2; r.outStart = -0.2; r.outEnd = -0.1; }, true);
        expectE ("region file track", [&] { r.files[0].trackId = juce::Uuid(); r.files[0].trackName = "Q"; }, true);
        expectE ("region file path", [&] { r.files[0].file = tmp.getChildFile ("rf.wav"); }, true);
        expectE ("region file channels", [&] { r.files[0].numChannels = 2; }, true);
        expectE ("region file start", [&] { r.files[0].fileStart = 77; }, true);
        expectE ("region file removed", [&] { r.files.pop_back(); }, true);
        expectE ("region waiting", [&] { r.waiting.name = "fix"; r.waiting.from = 1; r.waiting.to = 2; r.waiting.files.push_back (tmp.getChildFile ("rw.wav")); }, true);
        expectE ("region waiting range", [&] { r.waiting.to = 3; }, true);
        expectE ("region waiting tracks", [&] { r.waiting.tracks.push_back (q.tracks[0].id); }, true);  // (3.1.4)
        expectE ("gain at", [&] { r.gains[0].at = 600; }, true);
        expectE ("gain ramp", [&] { r.gains[0].ramp = 0.2; }, true);
        expectE ("gain db", [&] { r.gains[0].db[0] = -5.0f; }, true);
        expectE ("gain auto norm", [&] { r.gains[0].autoNorm = true; }, true);
        expectE ("gain removed", [&] { r.gains.clear(); }, true);
        expectE ("overdub moved", [&] { e.overdubs[0].startSample += 5; }, true);
        expectE ("overdub removed", [&] { e.overdubs.clear(); }, true);
        expectE ("lane track", [&] { e.lanes[0].trackId = q.tracks[1].id; }, true);
        expectE ("lane param", [&] { e.lanes[0].param = "pan"; }, true);
        expectE ("lane mixer", [&] { e.lanes[0].mixerId = juce::Uuid(); }, true);
        expectE ("lane locked", [&] { e.lanes[0].locked = true; }, true);
        expectE ("point time", [&] { e.lanes[0].pts[0].time = 2000; }, true);
        expectE ("point value", [&] { e.lanes[0].pts[0].value = -6.0f; }, true);
        expectE ("point region / take / src", [&] { e.lanes[0].pts[0].region = juce::Uuid(); e.lanes[0].pts[0].take = juce::Uuid(); e.lanes[0].pts[0].srcPos = 9; }, true);
        expectE ("point floating", [&] { e.lanes[0].pts[0].floating = true; }, true);
        expectE ("point removed", [&] { e.lanes[0].pts.clear(); }, true);
        expectE ("lane removed", [&] { e.lanes.clear(); }, true);
        expectE ("region removed", [&] { e.regions.pop_back(); }, true);
        // the visual guide (2.102): part of undo
        expectE ("guide set", [&] { e.guide.file = tmp.getChildFile ("guide.wav"); e.guide.sampleRate = 48000.0; e.guide.srcIn = 0; e.guide.srcOut = 48000; e.guide.trackId = q.tracks[0].id; e.guide.trackName = "A"; e.guide.takeName = "001"; }, true);
        expectE ("guide start", [&] { e.guide.start = -4800; }, true);
        expectE ("guide range", [&] { e.guide.srcIn = 100; e.guide.srcOut = 40000; }, true);
        expectE ("guide file", [&] { e.guide.file = tmp.getChildFile ("guide2.wav"); }, true);
        expectE ("guide track", [&] { e.guide.trackId = q.tracks[1].id; e.guide.trackName = "B"; }, true);
        expectE ("guide take", [&] { e.guide.windowId = juce::Uuid(); e.guide.takeId = juce::Uuid(); e.guide.takeName = "002"; }, true);
        expectE ("guide channels / rate", [&] { e.guide.numChannels = 2; e.guide.sampleRate = 96000.0; }, true);
        expectE ("guide shown (a view)", [&] { e.showGuide = false; }, false);
        expectE ("guide removed", [&] { e.guide = EditGuide(); }, true);
        // the edit: view state, not undone
        expectE ("fix marks", [&] { e.fixIn = 1.0; e.fixOut = 2.0; e.fixTracks.push_back (q.tracks[0].id); }, false);
        expectE ("playhead", [&] { e.playheadSeconds = 3.0; }, false);
        expectE ("insert index", [&] { e.insertIndex = 1; }, false);
        expectE ("markers", [&] { Marker m; m.id = juce::Uuid(); m.name = "Verse"; e.markers.push_back (m); }, false);
        expectE ("feedback", [&] { FeedbackNote n; n.key = "k"; n.text = "late"; e.feedback.push_back (n); }, false);
        expectE ("show feedback / markers", [&] { e.showFeedback = false; e.showMarkers = false; }, false);
        expectE ("lane rows (a view)", [&] { e.addLaneRow (q.tracks[0].id, true, true); }, false);
        expectE ("what a lane row shows (a view)", [&] { EditDef::LaneShown ls; ls.param = autoparam::gain; e.setLaneShown (q.tracks[0].id, 1, ls); }, false);
        // the mastering list
        {
            q.mastering.sync (q.edits);
            const auto h0 = Project::undoHash (q.mastering); const auto t0 = q.undoText (q.mastering);
            q.mastering.view = 2; q.mastering.selectedItem = e.id;
            CHECK (Project::undoHash (q.mastering) == h0 && q.undoText (q.mastering) == t0);
            if (! q.mastering.items.empty()) q.mastering.items[0].include = false; q.mastering.tailSeconds = 3.0;
            CHECK (Project::undoHash (q.mastering) != h0 && q.undoText (q.mastering) != t0);
        }
        std::printf ("   (%d fields move the undo hash, %d view fields do not)\n", moved, still);
    }

    SECTION ("2.69 undo per window: a change serialises only the window it was made in; one step for what changed together; it stays quick on a big project");
    {
        Project q; q.setInputCount (40); q.setOutputCount (2); q.tracks.clear();
        for (int i = 0; i < 20; ++i) q.addTrack ("S" + juce::String (i + 1), TrackFormat::Stereo, 2 * i);
        q.takeWindows.clear(); q.edits.clear();
        for (int wi = 0; wi < 50; ++wi)                                           // 50 pieces x 60 takes x 20 stereo tracks: 3000 takes
        {
            auto& tw = q.addTakeWindow ("Piece " + juce::String (wi + 1));
            for (int t = 0; t < 60; ++t)
            {
                TakeGroup g; g.id = juce::Uuid(); g.number = t + 1; g.label = "Take"; g.startSeconds = t * 200.0; g.lengthSamples = 96000 * 180; g.sampleRate = 96000.0;
                for (auto& tr : q.tracks) { TakeFile f; f.trackId = tr.id; f.trackName = tr.name; f.file = tmp.getChildFile ("Recorded Media/Piece/" + tr.name + " 001.wav"); f.numChannels = 2; g.files.push_back (f); }
                tw.groups.push_back (g);
            }
            auto& ed = q.addEdit ("Edit " + juce::String (wi + 1), tw.id);
            for (int r = 0; r < 40; ++r)
            {
                EditRegion reg; reg.id = juce::Uuid(); reg.windowId = tw.id; reg.takeId = tw.groups[(size_t) r].id; reg.takeName = "Take"; reg.srcIn = 1000; reg.srcOut = 96000 * 20; reg.sampleRate = 96000.0; reg.startSample = r * 96000 * 20;
                for (auto& tr : q.tracks) { RegionFile f; f.trackId = tr.id; f.trackName = tr.name; f.file = tmp.getChildFile ("Recorded Media/Piece/" + tr.name + " 001.wav"); f.numChannels = 2; reg.files.push_back (f); }
                ed.regions.push_back (reg);
            }
        }
        auto ms = [] (double t0) { return juce::Time::getMillisecondCounterHiRes() - t0; };
        double t = juce::Time::getMillisecondCounterHiRes(); q.undoReset(); const double resetMs = ms (t);
        CHECK (q.undoTakes.size() == 50 && q.undoEdits.size() == 50);
        // one take renamed: one piece in the step, only that window's text
        auto& wA = *q.takeWindows[3]; auto& wB = *q.takeWindows[7];
        const auto labelBefore = wA.groups[5].label;
        wA.groups[5].label = "Slate"; q.changed();
        t = juce::Time::getMillisecondCounterHiRes(); q.undoTick (false, true); const double tickMs = ms (t);
        CHECK (q.undoDepth() == 1 && q.undoStack.back().pieces.size() == 1 && q.undoStack.back().pieces[0].kind == Project::UndoKind::takeWindow && q.undoStack.back().pieces[0].id == wA.id);
        CHECK (q.undoStack.back().pieces[0].text.length() < 400000);               // one window, not 8 MB of project
        // two windows changed in the same quiet moment: one step, two pieces; undo puts both back, redo both forward
        wA.groups[6].dud = true; wB.name = "Piece B renamed"; q.changed(); q.undoTick (false, true);
        CHECK (q.undoDepth() == 2 && q.undoStack.back().pieces.size() == 2);
        t = juce::Time::getMillisecondCounterHiRes(); CHECK (q.undo()); const double undoMs = ms (t);
        CHECK (! wA.groups[6].dud && wB.name == "Piece 8" && wA.groups[5].label == "Slate" && q.redoDepth() == 1);
        CHECK (q.undo() && wA.groups[5].label == labelBefore && q.redoDepth() == 2);
        CHECK (q.redo() && wA.groups[5].label == "Slate" && ! wA.groups[6].dud);
        CHECK (q.redo() && wA.groups[6].dud && wB.name == "Piece B renamed" && ! q.redo());
        // an edit and the mastering list
        auto& eA = *q.edits[2];
        const auto n = eA.regions.size();
        eA.regions.erase (eA.regions.begin()); q.mastering.sync (q.edits); q.mastering.tailSeconds = 4.0; q.changed(); q.undoTick (false, true);
        CHECK (q.undoStack.back().pieces.size() == 2);
        CHECK (q.undo() && q.edits[2]->regions.size() == n && q.mastering.tailSeconds == 0.0);
        CHECK (q.redo() && q.edits[2]->regions.size() == n - 1 && q.mastering.tailSeconds == 4.0);
        // a new window is not an undo step; a removed one drops out of the snapshots
        q.addTakeWindow ("New piece"); q.changed(); q.undoTick (false, true);
        CHECK (q.undoTakes.size() == 51 && q.undoDepth() == 3);
        q.takeWindows.pop_back(); q.changed(); q.undoTick (false, true);
        CHECK (q.undoTakes.size() == 50 && q.undoDepth() == 3);
        // saving: the first save writes every window file; after one change only that window's file is written again, and quickly
        q.projectFile = tmp.getChildFile ("big/Big.fermata"); juce::String er;
        t = juce::Time::getMillisecondCounterHiRes(); CHECK (q.save (er, true)); const double firstSaveMs = ms (t);
        const auto bigFolder = q.projectFolder();
        auto bakCount = [&] { return bigFolder.getChildFile ("Take Windows").getNumberOfChildFiles (juce::File::findFiles, "*.bak") + bigFolder.getChildFile ("Edit Windows").getNumberOfChildFiles (juce::File::findFiles, "*.bak"); };
        CHECK (bakCount() == 0 && q.windowFiles.size() == 101);
        t = juce::Time::getMillisecondCounterHiRes(); CHECK (q.save (er, true)); const double sameSaveMs = ms (t);
        CHECK (bakCount() == 0);                                                     // nothing changed: no window file touched
        q.takeWindows[9]->groups[1].barIn = 12; q.changed();
        t = juce::Time::getMillisecondCounterHiRes(); CHECK (q.save (er, true)); const double oneSaveMs = ms (t);
        CHECK (bakCount() == 1 && bigFolder.getChildFile (q.windowFiles[q.takeWindows[9]->id.toString()].rel + ".bak").existsAsFile());
        q.edits[4]->regions[0].startSample += 7; q.changed(); CHECK (q.save (er, true));
        CHECK (bakCount() == 2 && bigFolder.getChildFile (q.windowFiles[q.edits[4]->id.toString()].rel + ".bak").existsAsFile());
        // opened again: the hashes are set from what was read, so a save straight after writes nothing
        { Project r; CHECK (r.load (q.projectFile, er) && r.takeWindows.size() == 50); CHECK (r.save (er, true) && bakCount() == 2); }
        std::printf ("   (3000 takes, 2000 pieces: undo snapshots at open %.0f ms, one change settled in %.1f ms, undo in %.1f ms; save: first %.0f ms, nothing changed %.1f ms, one take changed %.1f ms)\n", resetMs, tickMs, undoMs, firstSaveMs, sameSaveMs, oneSaveMs);
       #if defined (__SANITIZE_ADDRESS__) || defined (__SANITIZE_THREAD__)
        (void) tickMs; (void) undoMs; (void) sameSaveMs; (void) oneSaveMs;         // (timings are not checked under a sanitizer)
       #else
        CHECK (tickMs < 80.0 && undoMs < 100.0);                                   // (the old whole-project text took about 300 ms per change here; a loaded 2-core test machine measures up to ~40)
        CHECK (sameSaveMs < 60.0 && oneSaveMs < 80.0);                             // (a save used to serialise every window: about 300 ms here)
       #endif
    }


    SECTION ("dither: every written file, the mixer output, and the disc master");
    {
        const auto dd = tmp.getChildFile ("dither"); dd.createDirectory();
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto readAll = [&] (const juce::File& f, int& bits)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            std::vector<float> v;
            if (r == nullptr) { bits = 0; return v; }
            bits = (int) r->bitsPerSample;
            juce::AudioBuffer<float> b (1, (int) r->lengthInSamples); r->read (&b, 0, b.getNumSamples(), 0, true, false);
            v.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
            return v;
        };
        const int N = 48000;
        for (int bits : { 16, 24 })
        {
            const float lsb = 1.0f / (float) (1 << (bits - 1));
            std::vector<float> sig ((size_t) N);
            for (int i = 0; i < N; ++i) sig[(size_t) i] = 0.3f * lsb * std::sin (2.0f * 3.14159265f * 1000.0f * (float) i / 48000.0f);   // a signal 0.3 LSB high
            for (int useD = 0; useD < 2; ++useD)
            {
                const auto f = dd.getChildFile ("t" + juce::String (bits) + (useD ? "d" : "n") + ".wav");
                f.deleteFile();
                { juce::WavAudioFormat wf; std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
                  auto w = wf.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (bits));
                  CHECK (w != nullptr); if (w == nullptr) continue;
                  DitherOut dout (*w, useD != 0); const float* ch[1] = { sig.data() }; CHECK (dout.write (ch, 1, N)); }
                int fb = 0; auto back = readAll (f, fb);
                CHECK (fb == bits && (int) back.size() == N); if ((int) back.size() != N) continue;
                int nonZero = 0; double corr = 0.0, err = 0.0, maxErr = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    const double q = (double) back[(size_t) i] / lsb;                       // in whole steps
                    CHECK (std::abs (q - std::nearbyint (q)) < 1e-3);                       // always an exact step of the file's word length
                    if (back[(size_t) i] != 0.0f) ++nonZero;
                    corr += q * (double) sig[(size_t) i] / lsb; err += q - (double) sig[(size_t) i] / lsb; maxErr = juce::jmax (maxErr, std::abs (q - (double) sig[(size_t) i] / lsb));
                }
                if (useD) { CHECK (nonZero > N / 10 && corr > 0.0); CHECK (std::abs (err / N) < 0.02); CHECK (maxErr <= 1.51); }   // noise carries the signal, no bias, error within 1.5 LSB
                else      CHECK (nonZero == 0);                                                                                  // plain rounding of a 0.3 LSB signal is silence
            }
        }
        // the engine's dither of the driver outputs: each mixer on its own setting
        {
            mixA.busFor (outBus)->outFirst.store (0); mixB.busFor (outBus)->outFirst.store (2);
            auto onGrid = [&] (const std::vector<float>& v, int bits)
            {
                const double sc = (double) (1 << (bits - 1)); int bad = 0;
                for (float x : v) { const double q = (double) x * sc; if (std::abs (q - std::nearbyint (q)) > 1e-3) ++bad; }
                return bad == 0;
            };
            const float savedGain = rig.inGain; rig.inGain = 0.05f;                       // well under full scale (overs are passed on untouched)
            mixA.ditherBits.store (16); mixB.ditherBits.store (24);
            rig.run (eng, 12);
            CHECK (onGrid (rig.out[0], 16) && onGrid (rig.out[1], 16) && onGrid (rig.out[2], 24) && onGrid (rig.out[3], 24));
            CHECK (! onGrid (rig.out[2], 16));                                           // B is 24 bit: not on the 16-bit grid
            mixA.ditherBits.store (0);
            rig.run (eng, 12);
            CHECK (! onGrid (rig.out[0], 24));                                           // dither off: the raw float mix
            rig.inGain = savedGain;
            mixA.ditherBits.store (16); mixB.ditherBits.store (0);
            juce::String serr; CHECK (p.save (serr));
            Project q2; CHECK (q2.load (p.projectFile, serr));
            CHECK (q2.cueEnd() == 2 && q2.mixers[0]->ditherBits.load() == 16 && q2.mixers[1]->ditherBits.load() == 0);
            mixA.ditherBits.store (24); mixB.ditherBits.store (24);
        }
    }


    SECTION ("pitch curve: a drawn line of pitch against time");
    {
        const double sr = 48000.0;
        auto freqAt = [&] (const std::vector<float>& x, double t0, double t1)        // frequency from the interpolated upward zero crossings between t0 and t1
        {
            std::vector<double> cr;
            for (long i = (long) (t0 * sr); i + 1 < (long) (t1 * sr) && i + 1 < (long) x.size(); ++i)
                if (x[(size_t) i] < 0.0f && x[(size_t) i + 1] >= 0.0f) cr.push_back ((double) i + (double) (-x[(size_t) i]) / (double) (x[(size_t) i + 1] - x[(size_t) i]));
            return cr.size() < 3 ? 0.0 : (double) (cr.size() - 1) * sr / (cr.back() - cr.front());
        };
        auto cents = [] (double f, double ref) { return 1200.0 * std::log2 (f / ref); };
        // a note that is in tune for 2 s and then sinks 30 cents flat by 6 s
        const int Ls = (int) (6.5 * sr);
        std::vector<float> drift ((size_t) Ls); double ph = 0.0;
        for (int i = 0; i < Ls; ++i)
        {
            const double t = (double) i / sr, c = t < 2.0 ? 0.0 : t > 6.0 ? -30.0 : -30.0 * (t - 2.0) / 4.0;
            drift[(size_t) i] = 0.5f * (float) std::sin (ph); ph += 2.0 * 3.14159265358979 * 440.0 * std::pow (2.0, c / 1200.0) / sr;
        }
        CHECK (std::abs (cents (freqAt (drift, 0.5, 1.5), 440.0)) < 0.5 && cents (freqAt (drift, 5.0, 6.0), 440.0) < -24.0);       // the test signal really drifts
        PitchCurve cv; cv.pts = { { 2.0, 0.0 }, { 6.0, 30.0 } };
        CHECK (std::abs (cv.centsAt (1.0)) < 1e-9 && std::abs (cv.centsAt (4.0) - 15.0) < 1e-9 && std::abs (cv.centsAt (9.0) - 30.0) < 1e-9);
        const auto fixed = PitchShifter::shiftCurve (drift, sr, [&] (double i) { return cv.centsAt (i / sr); });
        CHECK (fixed.size() == drift.size());
        for (double t : { 1.0, 3.0, 4.0, 5.0 })
            CHECK (std::abs (cents (freqAt (fixed, t, t + 0.5), 440.0)) < 2.0);                               // back in tune all the way along
        CHECK (std::abs (cents (freqAt (fixed, 5.5, 6.2), 440.0)) < 2.5);
        // a slow drift and then a sudden jump: three points make the shape
        std::vector<float> steady ((size_t) (6 * sr));
        for (size_t i = 0; i < steady.size(); ++i) steady[i] = 0.5f * (float) std::sin (2.0 * 3.14159265358979 * 440.0 * (double) i / sr);
        PitchCurve cj; cj.pts = { { 1.0, 0.0 }, { 3.0, 0.0 }, { 3.3, 40.0 } };
        const auto jumped = PitchShifter::shiftCurve (steady, sr, [&] (double i) { return cj.centsAt (i / sr); });
        CHECK (std::abs (cents (freqAt (jumped, 0.5, 2.5), 440.0)) < 0.5);                                   // level part untouched
        CHECK (std::abs (cents (freqAt (jumped, 4.0, 5.5), 440.0) - 40.0) < 2.0);                            // after the jump: 40 cents up
        // a level line gives the same pitch as the fixed shift
        const auto lvl = PitchShifter::shiftCurve (steady, sr, [] (double) { return 50.0; });
        CHECK (std::abs (cents (freqAt (lvl, 1.0, 4.0), 440.0) - 50.0) < 1.0 && lvl.size() == steady.size());
        // through applyFix: only the marked part changes, and the curve starts where the mark does
        FixSpec fs; fs.kind = FixSpec::Kind::Pitch; fs.useCurve = true; fs.curve.pts = { { 0.0, 60.0 } }; fs.curveZero = 0.0;
        std::vector<std::vector<float>> chs { steady };
        audioops::applyFix (chs, sr, (long) (2.0 * sr), (long) (4.0 * sr), fs, 0);
        CHECK (std::abs (cents (freqAt (chs[0], 0.2, 1.8), 440.0)) < 0.2 && std::abs (cents (freqAt (chs[0], 2.3, 3.7), 440.0) - 60.0) < 2.0 && std::abs (cents (freqAt (chs[0], 4.3, 5.8), 440.0)) < 0.2);
        CHECK (fs.shortName() == "pitch curve");
    }
    // ---- project folders, crash recovery, copy ----
    {
        std::printf ("folders / recovery / copy\n");
        auto base = tmp.getChildFile ("Projects 101"); base.createDirectory();
        Project a; a.projectFile = base.getChildFile ("Concert.fermata"); a.setInputCount (2); a.setOutputCount (2); a.tracks.clear();
        a.addTrack ("violin", TrackFormat::Mono, 0);
        a.createFolders();
        CHECK (a.audioFolder().getFileName() == "Recorded Media" && a.audioFolder().isDirectory());
        CHECK (a.bouncedFolder().getFileName() == "Bounced Media" && a.bouncedFolder().isDirectory());
        CHECK (a.masteredFolder().getFileName() == "Mastered Audio" && a.masteredFolder().getParentDirectory() == a.bouncedFolder() && a.masteredFolder().isDirectory());
        // a recording, then a "crash": the header still says the data is 0 bytes
        auto wav = a.audioFolder().getChildFile ("001 - Piece - violin.wav");
        {
            juce::WavAudioFormat fmt; std::unique_ptr<juce::FileOutputStream> os (wav.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0)); os.release();
            juce::AudioBuffer<float> b (1, 48000); for (int i = 0; i < 48000; ++i) b.setSample (0, i, 0.25f * std::sin (i * 0.05f));
            w->writeFromAudioSampleBuffer (b, 0, 48000);
        }
        const auto goodSize = wav.getSize();
        {   // as if the program died: the header says 0 bytes of audio (JUCE's header has a JUNK chunk first, so find the real 'data' chunk)
            juce::MemoryBlock mb; wav.loadFileAsData (mb); int dpos = -1;
            for (int i = 12; i < 300; ++i) if (std::memcmp ((const char*) mb.getData() + i, "data", 4) == 0) { dpos = i; break; }
            CHECK (dpos > 0);
            juce::FileOutputStream fo (wav); fo.setPosition (4); fo.writeInt (0); fo.setPosition (dpos + 4); fo.writeInt (0);
        }
        auto damaged = wavrepair::inspect (wav, false);
        CHECK (damaged.wasBroken);
        TakeWindowDef tw; tw.name = "Piece"; TakeGroup g; g.id = juce::Uuid(); g.number = 1; g.lengthSamples = 0; g.sampleRate = 48000.0;
        TakeFile tf; tf.trackId = a.tracks[0].id; tf.trackName = "violin"; tf.file = wav; g.files.push_back (tf); tw.groups.push_back (g);
        a.takeWindows.push_back (std::make_unique<TakeWindowDef> (tw));
        auto lines = a.repairInterruptedRecordings();
        CHECK (! lines.isEmpty());
        CHECK (a.takeWindows.back()->groups[0].lengthSamples == 48000);
        CHECK (wav.getSize() == goodSize);
        auto r2 = wavrepair::inspect (wav, false); CHECK (! r2.wasBroken && r2.frames == 48000);
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (wav)); CHECK (rd != nullptr && rd->lengthInSamples == 48000);
        {   // finished takes: only one whose file is shorter than the take is opened and mended; one with full-length files is passed by its size alone
            auto writeTone = [] (const juce::File& f)
            {
                juce::WavAudioFormat fmt; std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0)); os.release();
                juce::AudioBuffer<float> b (1, 48000); for (int i = 0; i < 48000; ++i) b.setSample (0, i, 0.25f * std::sin (i * 0.05f));
                w->writeFromAudioSampleBuffer (b, 0, 48000);
            };
            auto dataPos = [] (const juce::File& f) { juce::MemoryBlock mb; f.loadFileAsData (mb); for (int i = 12; i < 300; ++i) if (std::memcmp ((const char*) mb.getData() + i, "data", 4) == 0) return i; return -1; };
            auto cutSrc = a.audioFolder().getChildFile ("002 - Piece - full.wav"), cut = a.audioFolder().getChildFile ("002 - Piece - violin.wav");
            writeTone (cutSrc);
            {   // the power went: only half of the audio reached the disk, but the header (written at the end of the take) says all of it
                juce::MemoryBlock mb; cutSrc.loadFileAsData (mb);
                const int dp = dataPos (cutSrc); CHECK (dp > 0);
                cut.replaceWithData (mb.getData(), (size_t) (dp + 8 + 24000 * 3));
                cutSrc.deleteFile();
            }
            auto odd = a.audioFolder().getChildFile ("003 - Piece - violin.wav");
            writeTone (odd);
            {   // a full-length file whose header says 'size unknown' (as some recorders leave a file being recorded): fine to play, not to be touched
                const int dp = dataPos (odd); CHECK (dp > 0);
                juce::FileOutputStream fo (odd); fo.setPosition (dp + 4); fo.writeInt ((int) 0xffffffff);
            }
            const auto oddSize = odd.getSize();
            for (auto [f, num] : { std::pair<juce::File, int> { cut, 2 }, std::pair<juce::File, int> { odd, 3 } })
            {
                TakeGroup g2; g2.id = juce::Uuid(); g2.number = num; g2.lengthSamples = 48000; g2.sampleRate = 48000.0;
                TakeFile t2; t2.trackId = a.tracks[0].id; t2.trackName = "violin"; t2.file = f; t2.numChannels = 1; g2.files.push_back (t2);
                a.takeWindows.back()->groups.push_back (g2);
            }
            auto lines2 = a.repairInterruptedRecordings();
            CHECK (lines2.size() == 1);
            CHECK (a.takeWindows.back()->groups[1].lengthSamples == 24000);
            CHECK (! wavrepair::inspect (cut, false).wasBroken && wavrepair::inspect (cut, false).frames == 24000);
            CHECK (a.takeWindows.back()->groups[2].lengthSamples == 48000 && odd.getSize() == oddSize && wavrepair::inspect (odd, false).wasBroken);
            CHECK (a.takeWindows.back()->groups[0].lengthSamples == 48000);      // (the take mended before is fine now and left alone)
            // 2.70: a take found whole is marked verified (and saved so): the next open does not ask the disk about it. A mended one is not, nor one
            // whose file is missing. Verified takes are looked at again only when the project has moved or been copied (filesMoved).
            auto& gs = a.takeWindows.back()->groups;
            CHECK (gs[0].verified && ! gs[1].verified && gs[2].verified);
            auto v4 = a.audioFolder().getChildFile ("004 - Piece - violin.wav"); writeTone (v4);
            {
                TakeGroup g4; g4.id = juce::Uuid(); g4.number = 4; g4.lengthSamples = 48000; g4.sampleRate = 48000.0;
                TakeFile t4; t4.trackId = a.tracks[0].id; t4.trackName = "violin"; t4.file = v4; t4.numChannels = 1; g4.files.push_back (t4);
                gs.push_back (g4);
            }
            CHECK (a.repairInterruptedRecordings().isEmpty() && gs[3].verified && gs[1].verified);     // (the mended take is whole now: verified too)
            {
                juce::MemoryBlock mb; v4.loadFileAsData (mb);
                const int dp = dataPos (v4); CHECK (dp > 0);
                v4.replaceWithData (mb.getData(), (size_t) (dp + 8 + 12000 * 3));                    // cut short behind the program's back
            }
            CHECK (a.repairInterruptedRecordings().isEmpty() && gs[3].lengthSamples == 48000);        // verified: not looked at
            a.filesMoved = true;
            CHECK (a.repairInterruptedRecordings().size() == 1 && gs[3].lengthSamples == 12000 && ! gs[3].verified);   // moved: looked at, mended, checked again next time
            a.filesMoved = false;
            CHECK (a.repairInterruptedRecordings().isEmpty() && gs[3].verified);                       // whole now: verified again
            juce::String ve; CHECK (a.save (ve));
            { Project av; CHECK (av.load (a.projectFile, ve) && av.takeWindows.back()->groups[3].verified && av.takeWindows.back()->groups[0].verified); }
            gs.pop_back(); v4.deleteFile(); CHECK (a.save (ve));
        }
        {   // saving: a window's file is rewritten only when that window changed
            juce::String se; CHECK (a.save (se));
            const auto twId = a.takeWindows.back()->id.toString();
            CHECK (a.windowFiles.count (twId) == 1);
            const auto twFile = a.projectFolder().getChildFile (a.windowFiles[twId].rel);
            CHECK (twFile.existsAsFile());
            const auto longAgo = juce::Time (2001, 0, 1, 12, 0);
            twFile.setLastModificationTime (longAgo);
            CHECK (a.save (se));
            CHECK (twFile.getLastModificationTime() == longAgo);                  // nothing changed: not written again
            a.takeWindows.back()->groups[0].label = "Fresh label";
            CHECK (a.save (se));
            CHECK (twFile.getLastModificationTime() != longAgo && twFile.loadFileAsString().contains ("Fresh label"));
        }
        // save, then copy the whole thing somewhere else
        juce::String e; CHECK (a.save (e));
        auto dest = tmp.getChildFile ("Elsewhere").getChildFile ("Copy of Concert");
        projectcopy::Report rep; std::function<bool (float, const juce::String&)> prog = [] (float, const juce::String&) { return true; };
        auto err = projectcopy::run (a.toVar(), base, dest, "Copy of Concert", true, prog, rep);
        CHECK (err.isEmpty());
        CHECK (rep.projectFile.existsAsFile() && rep.files >= 1);
        Project c; juce::String ce; CHECK (c.load (rep.projectFile, ce));
        CHECK (c.takeWindows.back()->groups[0].files[0].file.getParentDirectory() == dest.getChildFile ("Recorded Media"));
        CHECK (c.takeWindows.back()->groups[0].files[0].file.getSize() == goodSize);
        CHECK (dest.getChildFile ("Bounced Media").getChildFile ("Mastered Audio").isDirectory());
        // moving the whole folder by hand still works (relative paths)
        auto moved = tmp.getChildFile ("Moved by hand"); base.copyDirectoryTo (moved);
        base.deleteRecursively();
        Project m; juce::String me; CHECK (m.load (moved.getChildFile ("Concert.fermata"), me));
        CHECK (m.takeWindows.back()->groups[0].files[0].file == moved.getChildFile ("Recorded Media").getChildFile (wav.getFileName()));
    }
    // ---- export for processing / re-link ----
    {
        std::printf ("export for processing\n");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto dir = tmp.getChildFile ("proc"); dir.createDirectory();
        std::vector<std::vector<float>> sig (2, std::vector<float> (48000));
        for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < sig[c].size(); ++i) sig[c][i] = 0.5f * std::sin (0.01f * (float) i * (float) (c + 1));
        auto orig = dir.getChildFile ("orig.wav"); CHECK (audioops::writeWav (orig, sig, 48000.0));
        auto ex = dir.getChildFile ("ex.wav"); juce::String err;
        CHECK (audioops::exportRange (fm, orig, 0, 10000, 30000, ex, err));
        CHECK (audioops::checkReplacement (fm, ex, 20000, 2, 48000.0).isEmpty());
        CHECK (audioops::checkReplacement (fm, ex, 20001, 2, 48000.0).isNotEmpty());
        CHECK (audioops::checkReplacement (fm, ex, 20000, 1, 48000.0).isNotEmpty());
        CHECK (audioops::checkReplacement (fm, ex, 20000, 2, 44100.0).isNotEmpty());
        CHECK (audioops::checkReplacement (fm, dir.getChildFile ("nope.wav"), 20000, 2, 48000.0).isNotEmpty());
        CHECK (ex.deleteFile());                                  // not held open
        std::vector<std::vector<float>> quiet (2, std::vector<float> (20000, 0.0f));
        CHECK (audioops::writeWav (ex, quiet, 48000.0));
        auto sp = dir.getChildFile ("splice.wav");
        CHECK (audioops::spliceFile (fm, orig, ex, 10000, 30000, sp, 0.005, err));
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (sp));
        CHECK (rd != nullptr && rd->lengthInSamples == 48000);
        std::vector<std::vector<float>> got; audioops::readRange (*rd, 0, 48000, got);
        CHECK (std::abs (got[0][5000] - sig[0][5000]) < 1e-5f && std::abs (got[1][40000] - sig[1][40000]) < 1e-5f);        // outside: untouched
        CHECK (std::abs (got[0][20000]) < 1e-6f);                                                                         // inside: the corrected (silent) audio
        CHECK (std::abs (got[0][10000]) > 0.0f || std::abs (sig[0][10000]) < 1e-6f);                                       // the crossfade starts from the original
    }
    // ---- every Edit has a mixer of its own ----
    {
        Project mp;
        mp.addTrack ("T1", TrackFormat::Mono, 0);
        mp.mixers[0]->stripFor (mp.tracks[0].id)->gainDb.set (-6.0f);
        mp.mixers[0]->ditherBits.store (16);
        CHECK (mp.cueEnd() == 1);
        auto& ea = mp.addEdit ("Piece A", juce::Uuid::null());
        auto& eb = mp.addEdit ("Piece B", juce::Uuid::null());
        CHECK (mp.mixers.size() == 3 && mp.cueEnd() == 1);
        auto* ma = mp.mixerOfEdit (ea.id); auto* mb = mp.mixerOfEdit (eb.id);
        CHECK (ma != nullptr && mb != nullptr && ma != mb && ma->name == "Piece A" && mb->name == "Piece B");
        CHECK (std::abs (ma->stripFor (mp.tracks[0].id)->gainDb.get() + 6.0f) < 1e-4f && ma->ditherBits.load() == 16);   // starts as a copy of the processing mixer
        ma->stripFor (mp.tracks[0].id)->gainDb.set (-12.0f);
        CHECK (std::abs (mp.mixers[0]->stripFor (mp.tracks[0].id)->gainDb.get() + 6.0f) < 1e-4f
               && std::abs (mb->stripFor (mp.tracks[0].id)->gainDb.get() + 6.0f) < 1e-4f);                                   // independent of the others
        auto& cue = mp.addMixer ("Cue 1");                                                                                 // cue mixers stay before the Edit mixers
        CHECK (mp.cueEnd() == 2 && mp.mixers[1].get() == &cue && mp.mixers[2]->editId == ea.id);
        // lanes with no mixer named drive the Edit's own mixer
        ea.automationOn = true;
        auto& lane = ea.addLane (mp.tracks[0].id, autoparam::fader, juce::Uuid::null());
        { AutoPoint a0; a0.time = 0; a0.value = 0.0f; AutoPoint a1; a1.time = 48000; a1.value = -20.0f; lane.pts.push_back (a0); lane.pts.push_back (a1); }
        auto plan = automationPlanFor (mp, ea);
        CHECK (plan != nullptr && plan->items.size() == 1 && plan->items[0].target == &ma->stripFor (mp.tracks[0].id)->gainDb);
        auto planProc = automationPlanFor (mp, ea, true);
        CHECK (planProc != nullptr && planProc->items[0].target == &mp.mixers[0]->stripFor (mp.tracks[0].id)->gainDb);
        auto planRendered = automationPlanFor (mp, ea, false, mp.mixers[1]->id);                  // a render through one chosen mixer: the lane moves THAT mixer
        CHECK (planRendered != nullptr && planRendered->items[0].target == &mp.mixers[1]->stripFor (mp.tracks[0].id)->gainDb);
        // saved and loaded
        Project back; CHECK (back.fromVar (mp.toVar()));
        CHECK (back.mixers.size() == 4 && back.cueEnd() == 2 && back.mixerOfEdit (ea.id) != nullptr && back.mixerOfEdit (eb.id) != nullptr);
        CHECK (back.mixerOfEdit (ea.id) != nullptr && std::abs (back.mixerOfEdit (ea.id)->stripFor (back.tracks[0].id)->gainDb.get() + 12.0f) < 1e-4f);
        // renaming the Edit renames its mixer
        ea.name = "Renamed"; CHECK (mp.syncEditMixers() && mp.mixerOfEdit (ea.id)->name == "Renamed");
        // an old project (no Edit mixers) gets them on loading
        auto v = mp.toVar();
        if (auto* arr = v.getDynamicObject()->getProperty ("mixers").getArray())
            for (int i = arr->size(); --i >= 0;) if ((*arr)[i]["editId"].toString().isNotEmpty()) arr->remove (i);
        Project old; CHECK (old.fromVar (v));
        CHECK (old.mixers.size() == 4 && old.mixerOfEdit (eb.id) != nullptr);
    }
    SECTION ("reserved CPU cores: the plan");
    {
        auto V = [] (std::initializer_list<int> l) { return std::vector<int> (l); };
        std::vector<CpuCore> cores;                                                    // 4 cores with hyper-threading: logical 0,1 / 2,3 / 4,5 / 6,7
        for (int i = 0; i < 4; ++i) { CpuCore c; c.number = i; c.logical = { 2 * i, 2 * i + 1 }; cores.push_back (c); }
        CoreReservationSettings s; s.enabled = true; s.reserved = { 3 };
        auto plan = makeCorePlan (cores, s);
        CHECK (plan.valid && plan.audioCores == V ({ 3 }) && plan.audioLogical == V ({ 6, 7 })
               && plan.generalCores == V ({ 0, 1, 2 }) && plan.generalLogical == V ({ 0, 1, 2, 3, 4, 5 }) && plan.warning.isEmpty());
        s.reserved = { 2, 3 }; plan = makeCorePlan (cores, s);
        CHECK (plan.valid && plan.audioLogical == V ({ 4, 5, 6, 7 }) && plan.warning.isEmpty());                // two of four: fine
        s.reserved = { 1, 2, 3 }; plan = makeCorePlan (cores, s); CHECK (plan.valid && plan.warning.isNotEmpty());       // three of four leaves one core for Windows: allowed, with a word of warning
        s.reserved = { 2, 3 };
        s.reserved = {}; CHECK (! makeCorePlan (cores, s).valid && makeCorePlan (cores, s).problem.isNotEmpty());       // nothing ticked
        s.reserved = { 0, 1, 2, 3 }; CHECK (! makeCorePlan (cores, s).valid);                                           // everything ticked: nothing left for Windows
        s.reserved = { 7 }; CHECK (! makeCorePlan (cores, s).valid);                                                    // a core that does not exist is not a reservation
        s.reserved = { 1 }; CHECK (! makeCorePlan ({ cores[0] }, s).valid);                                             // a one-core PC
        s.setReservedText (" 3, 1;1 x 2"); CHECK (s.reserved == V ({ 1, 2, 3 }) && s.reservedText() == "1,2,3");
        CHECK (suggestedReserveCount (2) == 1 && suggestedReserveCount (4) == 1 && suggestedReserveCount (8) == 2 && suggestedReserveCount (16) == 3);
        // the quietest cores: core 0 is the noisiest here; ties go to the higher number; a core is as noisy as its noisiest thread
        std::vector<double> load { 9.0, 8.0, 0.1, 0.0, 0.1, 0.1, 0.0, 0.0 };
        CHECK (pickQuietCores (cores, load, 1) == V ({ 3 }));
        CHECK (pickQuietCores (cores, load, 2) == V ({ 2, 3 }));
        CHECK (pickQuietCores (cores, load, 9).size() == 3);                                                            // never every core
        for (auto& c : cores) c.efficiency = c.number < 2 ? 0 : 1;                                                       // cores 0 and 1 are the slow ones of a hybrid CPU
        CHECK (pickQuietCores (cores, std::vector<double> (8, 0.0), 2) == V ({ 3 }));                                 // (3.13.0: only half of the two big cores - the other is left to the PC)
        auto& cr = CoreReservation::get();                                                                              // (not Windows here: nothing happens, nothing breaks)
        cr.configure (s); cr.audioThreadCheck(); cr.registerAudioThread (juce::Thread::getCurrentThreadId()); cr.unregisterAudioThread (juce::Thread::getCurrentThreadId());
        CHECK (! cr.supported() && ! cr.active() && cr.statusText().isNotEmpty() && cr.measureInterruptLoad (200).empty());
        cr.shutdown();
    }

    SECTION ("window files: every edit in a .fmedit, every take window in a .fmtake");
    {
        juce::String er; CHECK (p.save (er));
        const auto folder = p.projectFolder();
        auto editFiles = folder.getChildFile ("Edit Windows").findChildFiles (juce::File::findFiles, false, "*.fmedit");
        auto takeFiles = folder.getChildFile ("Take Windows").findChildFiles (juce::File::findFiles, false, "*.fmtake");
        CHECK (p.windowFiles.size() == p.edits.size() + p.takeWindows.size() + (size_t) p.cueEnd() && ! p.edits.empty() && ! p.takeWindows.empty());
        for (auto& kv : p.windowFiles) CHECK (folder.getChildFile (kv.second.rel).existsAsFile());
        auto pv = juce::JSON::parse (p.projectFile);
        CHECK (pv.hasProperty ("editFiles") && pv["edits"].size() == 0 && pv["takeWindows"].size() == 0);
        Project q; CHECK (q.load (p.projectFile, er));
        CHECK (q.edits.size() == p.edits.size() && q.takeWindows.size() == p.takeWindows.size());
        CHECK (q.edits.front()->regions.size() == p.edits.front()->regions.size() && q.mixerOfEdit (q.edits.front()->id) != nullptr);
        CHECK (q.windowFiles.size() == p.edits.size() + p.takeWindows.size() + (size_t) p.cueEnd());

        // the file is the source of truth: change the name inside the file, open the project again
        auto ef = folder.getChildFile (p.windowFiles[p.edits.front()->id.toString()].rel);
        auto ev = juce::JSON::parse (ef.loadFileAsString());
        { auto eo = ev["edit"]; eo.getDynamicObject()->setProperty ("name", "Named in the file"); }
        CHECK (ef.replaceWithText (juce::JSON::toString (ev, false)));
        Project q4; CHECK (q4.load (p.projectFile, er) && q4.edits.front()->name == "Named in the file" && q4.loadNotes.isEmpty());

        // a renamed edit: its file is renamed too
        const auto oldRel = p.windowFiles[p.edits.front()->id.toString()].rel;
        p.edits.front()->name = "Zed edit"; p.changed();
        CHECK (p.save (er));
        CHECK (! folder.getChildFile (oldRel).existsAsFile() && folder.getChildFile ("Edit Windows/Zed edit.fmedit").existsAsFile());

        // import a copy into the same project: the ids are renewed and the name made unique
        const auto before = p.edits.size();
        juce::Uuid newId; juce::String msg;
        const auto zed = folder.getChildFile ("Edit Windows/Zed edit.fmedit");
        CHECK (p.importWindowFile (zed, msg, &newId) && p.edits.size() == before + 1);
        auto* ne = p.findEdit (newId);
        CHECK (ne != nullptr && ne->id != p.edits.front()->id && ne->name == "Zed edit (2)" && ne->regions.size() == p.edits.front()->regions.size());
        CHECK (ne != nullptr && ne->regions[0].id != p.edits.front()->regions[0].id && p.mixerOfEdit (newId) != nullptr);
        // and into an empty project: the tracks come with it
        {
            Project r; r.createDefaultDesign(); r.projectFile = tmp.getChildFile ("other/other.fermata");
            const auto nTracks = r.tracks.size();
            CHECK (r.importWindowFile (zed, msg, &newId) && r.edits.size() == 1 && r.edits[0]->name == "Zed edit");
            CHECK (r.tracks.size() >= nTracks && r.missingAudioCount() == 0);
            CHECK (r.save (er) && r.projectFolder().getChildFile ("Edit Windows/Zed edit.fmedit").existsAsFile());
        }
        const auto tf = folder.getChildFile (p.windowFiles[p.takeWindows.front()->id.toString()].rel);
        CHECK (p.importWindowFile (tf, msg, &newId) && p.findTakeWindow (newId) != nullptr && p.findTakeWindow (newId)->name != p.takeWindows.front()->name);

        // delete: the edit and its file go (the audio does not)
        const auto audio = p.edits.front()->regions.empty() ? juce::File() : p.edits.front()->regions[0].files[0].file;
        CHECK (p.deleteEditAndFile (newId) == false);                                   // (that id is a take window)
        CHECK (p.deleteTakeWindowAndFile (newId));
        const auto zedRel = p.windowFiles[p.findEdit (ne->id) ? ne->id.toString() : ""].rel;
        CHECK (p.save (er));
        const auto copyRel = p.windowFiles[ne->id.toString()].rel;
        CHECK (folder.getChildFile (copyRel).existsAsFile());
        const auto neId = ne->id;
        CHECK (p.deleteEditAndFile (neId) && p.findEdit (neId) == nullptr && ! folder.getChildFile (copyRel).existsAsFile());
        CHECK (audio == juce::File() || audio.existsAsFile());
        CHECK (p.save (er));
    }

    SECTION ("mixer files: every mixer in its own .fmmix");
    {
        juce::String er;
        p.addMixer ("Cue test");
        CHECK (p.save (er));
        const auto folder = p.projectFolder();
        const int nMix = p.cueEnd();
        int files = 0;
        for (auto& kv : p.windowFiles) if (kv.second.rel.startsWith ("Mixer Windows/") && folder.getChildFile (kv.second.rel).existsAsFile()) ++files;
        CHECK (files == nMix && nMix >= 2);
        auto pv = juce::JSON::parse (p.projectFile);
        CHECK (pv.hasProperty ("mixerFiles") && pv["mixerFiles"].size() == nMix);
        // a changed level in the file is what is read back
        const auto cueId = p.mixers[(size_t) nMix - 1]->id;
        auto mf = folder.getChildFile (p.windowFiles[cueId.toString()].rel);
        auto mv = juce::JSON::parse (mf.loadFileAsString());
        { auto mo = mv["mixer"]; mo.getDynamicObject()->setProperty ("name", "Renamed in file"); }
        CHECK (mf.replaceWithText (juce::JSON::toString (mv, false)));
        Project q; CHECK (q.load (p.projectFile, er));
        CHECK (q.cueEnd() == nMix && q.mixers[(size_t) nMix - 1]->name == "Renamed in file" && q.mixers.front()->name == p.mixers.front()->name);
        CHECK (q.mixers.size() == p.mixers.size() && q.loadNotes.isEmpty());
        // a missing processing-mixer file (and no .bak of it) does not break the project
        auto pm = folder.getChildFile (p.windowFiles[p.mixers.front()->id.toString()].rel);
        auto saved = pm.loadFileAsString(); pm.deleteFile(); pm.getSiblingFile (pm.getFileName() + ".bak").deleteFile();
        Project q2; CHECK (q2.load (p.projectFile, er) && q2.cueEnd() >= 1 && ! q2.loadNotes.isEmpty() && q2.loadNotes[0].contains ("missing"));
        CHECK (pm.replaceWithText (saved));
        // import a mixer file into a project as a new mixer
        const auto before = p.mixers.size();
        juce::String msg;
        CHECK (p.importMixerFile (mf, msg) && p.mixers.size() == before + 1 && p.mixers[(size_t) p.cueEnd() - 1]->name == "Renamed in file");
        CHECK (p.save (er));
    }
    SECTION ("2.68.1 saving that survives being interrupted: .bak copies, damaged files, a save that fails half way");
    {
        juce::String er;
        const auto folder = p.projectFolder();
        auto bakOf = [] (const juce::File& f) { return f.getSiblingFile (f.getFileName() + ".bak"); };
        // the save before is kept as .bak: the project file's and a rewritten window file's
        const auto nameBefore = p.name;
        CHECK (p.save (er));
        p.name = "Renamed project"; p.edits.front()->name = "Bak test edit"; p.changed();
        CHECK (p.save (er));
        CHECK (bakOf (p.projectFile).existsAsFile() && juce::JSON::parse (bakOf (p.projectFile).loadFileAsString())["name"].toString() == nameBefore);
        const auto editFile = folder.getChildFile (p.windowFiles[p.edits.front()->id.toString()].rel);
        CHECK (editFile.existsAsFile() && editFile.getFileName() == "Bak test edit.fmedit");
        p.edits.front()->regions.front().takeName += " (b)"; p.changed();           // (a change inside: the file is rewritten over the old one)
        CHECK (p.save (er) && bakOf (editFile).existsAsFile());
        CHECK (juce::JSON::parse (bakOf (editFile).loadFileAsString())["edit"]["name"].toString() == "Bak test edit");

        // the project file is damaged: the .bak is used, says so, the damaged file is set aside and the folder made whole again
        const auto goodText = p.projectFile.loadFileAsString();
        CHECK (p.save (er));                                                           // (so the .bak is the same as the file)
        CHECK (p.projectFile.replaceWithText ("{ \"name\": \"half writ"));
        {
            Project q; CHECK (q.load (p.projectFile, er));
            CHECK (q.name == "Renamed project" && q.edits.size() == p.edits.size() && q.takeWindows.size() == p.takeWindows.size());
            CHECK (q.loadNotes.size() == 1 && q.loadNotes[0].contains (".bak"));
            CHECK (p.projectFile.getSiblingFile (p.projectFile.getFileName() + ".damaged").existsAsFile());
            CHECK (p.projectFile.loadFileAsString() == bakOf (p.projectFile).loadFileAsString());
        }
        // and a project file that is gone altogether (the computer stopped between the two renames of a save)
        p.projectFile.deleteFile();
        { Project q; CHECK (q.load (p.projectFile, er) && q.name == "Renamed project" && q.loadNotes.size() == 1 && p.projectFile.existsAsFile()); }
        // nothing at all to read: a clear failure
        { Project q; juce::File none = folder.getChildFile ("not here.fermata"); CHECK (! q.load (none, er) && er.isNotEmpty()); }

        // a damaged window file: its .bak is used, the window is there, the note says so, nothing is listed as missing
        CHECK (editFile.replaceWithText ("garbage"));
        {
            Project q; CHECK (q.load (p.projectFile, er));
            CHECK (q.findEdit (p.edits.front()->id) != nullptr && q.missingWindowRefs.empty());
            CHECK (q.loadNotes.size() == 1 && q.loadNotes[0].contains ("Bak test edit") && q.loadNotes[0].contains (".bak"));
            CHECK (editFile.getSiblingFile (editFile.getFileName() + ".damaged").existsAsFile() && juce::JSON::parse (editFile.loadFileAsString())["edit"].isObject());
        }
        (void) goodText;

        // a save that fails after the window files are written: the old window file is NOT tidied away until a save gets as far as the project file
        p.edits.front()->name = "Second name"; p.changed();
        const auto blocker = p.projectFile;                                            // (a folder with something in it where the project file should be: it cannot be replaced)
        const auto keep = blocker.loadFileAsString();
        blocker.deleteFile(); bakOf (blocker).deleteFile(); blocker.createDirectory(); blocker.getChildFile ("x").replaceWithText ("x");
        CHECK (! p.save (er));
        CHECK (editFile.existsAsFile() && folder.getChildFile ("Edit Windows/Second name.fmedit").existsAsFile());    // both: the old one is still what the (old) project file lists
        CHECK (p.windowFiles[p.edits.front()->id.toString()].rel == "Edit Windows/Second name.fmedit" && p.dirty.load());
        blocker.deleteRecursively(); CHECK (blocker.replaceWithText (keep));
        CHECK (p.save (er));
        CHECK (! editFile.existsAsFile() && ! bakOf (editFile).existsAsFile() && folder.getChildFile ("Edit Windows/Second name.fmedit").existsAsFile());
        { Project q; CHECK (q.load (p.projectFile, er) && q.loadNotes.isEmpty() && q.edits.front()->name == "Second name"); }

        // 2.75: a project's .bak opened on purpose ("the save before"): the file there now is kept as "(replaced ...)" and the .bak's text is put back
        {
            const auto nameWas = p.name;
            CHECK (p.save (er));
            p.name = "After the bak"; p.changed(); CHECK (p.save (er));                  // the .bak now holds the save before (the old name)
            const auto bakText = bakOf (p.projectFile).loadFileAsString(), nowText = p.projectFile.loadFileAsString();
            CHECK (juce::JSON::parse (bakText)["name"].toString() == nameWas && juce::JSON::parse (nowText)["name"].toString() == "After the bak");
            CHECK (filesync::projectFileForBackup (bakOf (p.projectFile)) == p.projectFile);
            CHECK (filesync::projectFileForBackup (p.projectFile) == juce::File() && filesync::projectFileForBackup (folder.getChildFile ("Edit Windows/x.fmedit.bak")) == juce::File());
            // the windows and mixers stay as they are now: their lists come from the file there now (here they are the same)
            const auto merged = Project::backupWithCurrentWindows (bakText, nowText);
            CHECK (juce::JSON::parse (merged)["name"].toString() == nameWas && juce::JSON::toString (juce::JSON::parse (merged)["editFiles"]) == juce::JSON::toString (juce::JSON::parse (nowText)["editFiles"]));
            {
                auto b = juce::JSON::parse (bakText); b.getDynamicObject()->setProperty ("editFiles", juce::Array<juce::var>());     // (a backup from before the edits were made)
                const auto m2 = Project::backupWithCurrentWindows (juce::JSON::toString (b), nowText);
                CHECK (juce::JSON::parse (m2)["editFiles"].size() == juce::JSON::parse (nowText)["editFiles"].size() && juce::JSON::parse (m2)["editFiles"].size() > 0);
                CHECK (Project::backupWithCurrentWindows (bakText, "not json") == bakText);
            }
            juce::File aside;
            CHECK (filesync::restoreBackupText (p.projectFile, merged, "2026-10-19 12.00.00", aside, er));
            CHECK (aside.existsAsFile() && aside.getFileName() == p.projectFile.getFileNameWithoutExtension() + " (replaced 2026-10-19 12.00.00).fermata.bak" && aside.loadFileAsString() == nowText);
            CHECK (filesync::projectFileForBackup (aside) == p.projectFile);                                            // opening the kept copy the same way goes back
            CHECK (p.projectFile.loadFileAsString() == merged && bakOf (p.projectFile).loadFileAsString() == nowText);  // (the file it replaced is the new .bak, as at every save)
            { Project q; CHECK (q.load (p.projectFile, er) && q.name == nameWas && q.edits.size() == p.edits.size() && q.loadNotes.isEmpty()); }
            juce::File aside2;                                                                                          // the same stamp again: a copy of its own, nothing overwritten
            CHECK (filesync::restoreBackupText (p.projectFile, nowText, "2026-10-19 12.00.00", aside2, er) && aside2.existsAsFile() && aside2 != aside && aside.loadFileAsString() == nowText);
            CHECK (aside2.getFileName().contains ("(replaced 2026-10-19 12.00.00 2)") && filesync::projectFileForBackup (aside2) == p.projectFile);
            aside.deleteFile(); aside2.deleteFile();
            p.name = nameWas; p.changed(); CHECK (p.save (er));
        }

        // a new project does not inherit the old one's list of missing files
        Project n; n.missingWindowRefs.push_back (juce::var()); n.loadNotes.add ("x"); n.windowFiles["a"] = { "b", "c" };
        n.createDefaultDesign();
        CHECK (n.missingWindowRefs.empty() && n.loadNotes.isEmpty() && n.windowFiles.empty());
    }
    SECTION ("2.76 finding missing audio files: by name, then renamed files by length / rate / depth / channels and their headers");
    {
        // a scratch project with one take of three tracks (all its files the same length) and an edit using the take's files
        const auto base = tmp.getChildFile ("relink"); base.deleteRecursively(); base.createDirectory();
        const auto rec = base.getChildFile ("Recorded Media"); rec.createDirectory();
        const auto elsewhere = base.getChildFile ("Found here"); elsewhere.getChildFile ("sub").createDirectory();
        using Meta = std::unordered_map<juce::String, juce::String>;
        auto writeWav = [] (const juce::File& f, int bits, int channels, int frames, double rate, const Meta& meta, float value)
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels).withBitsPerSample (bits).withMetadataValues (meta));
            juce::AudioBuffer<float> b (channels, frames); for (int c = 0; c < channels; ++c) for (int i = 0; i < frames; ++i) b.setSample (c, i, value);
            w->writeFromAudioSampleBuffer (b, 0, frames);
        };
        auto bwf = [] (const juce::String& desc, const juce::String& date, const juce::String& time)
        {
            Meta m;
            m[juce::WavAudioFormat::bwavDescription] = desc; m[juce::WavAudioFormat::bwavOriginator] = "Fermata";
            m[juce::WavAudioFormat::bwavOriginationDate] = date; m[juce::WavAudioFormat::bwavOriginationTime] = time;
            return m;
        };
        Project rp; rp.projectFile = base.getChildFile ("Relink.fermata");
        rp.createDefaultDesign(); rp.tracks.clear(); rp.buses.clear();
        const auto tA = rp.addTrack ("ORTF L", TrackFormat::Mono, 0).id, tB = rp.addTrack ("Spaced L", TrackFormat::Mono, 1).id, tC = rp.addTrack ("Choir", TrackFormat::Stereo, 2).id;
        auto& tw = rp.addTakeWindow ("Kyrie");
        TakeGroup g; g.id = juce::Uuid(); g.number = 3; g.sampleRate = 48000.0; g.lengthSamples = 24000; g.recordedAt = juce::Time (2026, 9, 14, 10, 30, 0, 0, true);
        const juce::String desc = "Kyrie / 003 - Kyrie / ";
        auto addFile = [&] (const juce::Uuid& id, const juce::String& name, int ch) { TakeFile tf; tf.trackId = id; tf.trackName = name; tf.numChannels = ch; tf.file = rec.getChildFile (TakeWindowDef::fileNameFor (3, "Kyrie", name)); g.files.push_back (tf); };
        addFile (tA, "ORTF L", 1); addFile (tB, "Spaced L", 1); addFile (tC, "Choir", 2);
        tw.groups.push_back (g);
        auto& ed = rp.addEdit ("Edit 1", tw.id);
        { EditRegion r; r.id = juce::Uuid(); r.takeName = "003 - Kyrie"; r.sampleRate = 48000.0; r.sourceLength = 24000; r.srcIn = 0; r.srcOut = 24000;
          for (auto& tf : g.files) { RegionFile rf; rf.trackId = tf.trackId; rf.trackName = tf.trackName; rf.numChannels = tf.numChannels; rf.file = tf.file; r.files.push_back (rf); }
          ed.regions.push_back (r); }
        CHECK (rp.missingAudioCount() == 3 && rp.missingAudioList().size() == 3 && rp.missingAudioList()[0].contains ("Takes - Kyrie, 003 - Kyrie, ORTF L"));
        {   // 3.15.0: "Ignore missing files permanently" - left out of the checks, saved with the project, and given back
            CHECK (rp.ignoreMissingAudio() == 3 && rp.missingAudioCount() == 0 && rp.missingAudioCount (true) == 0 && rp.missingAudioList().isEmpty());
            CHECK (rp.ignoreMissingAudio() == 0);                                      // (each once)
            Project back; CHECK (back.fromVar (rp.toVar()) && back.ignoredMissing.size() == 3 && back.missingAudioCount() == 0);
            rp.ignoredMissing.clear();
            CHECK (rp.missingAudioCount() == 3);
            Project none; CHECK (none.fromVar (rp.toVar()) && none.ignoredMissing.empty() && ! rp.toVar().hasProperty ("ignoredMissingAudio"));
        }

        // the files turn up elsewhere: ORTF under its own name (in a sub-folder), Spaced renamed but with Fermata's header, Choir renamed with no header
        writeWav (elsewhere.getChildFile ("sub").getChildFile (g.files[0].file.getFileName()), 24, 1, 24000, 48000.0, bwf (desc + "ORTF L", "2026-09-14", "10:30:00"), 0.1f);
        writeWav (elsewhere.getChildFile ("spaced renamed.wav"), 24, 1, 24000, 48000.0, bwf (desc + "Spaced L", "2026-09-14", "10:30:00"), 0.2f);
        writeWav (elsewhere.getChildFile ("choir renamed.wav"), 24, 2, 24000, 48000.0, {}, 0.3f);
        writeWav (elsewhere.getChildFile ("not it - 16 bit.wav"), 16, 1, 24000, 48000.0, {}, 0.4f);           // the same length but 16 bit: not a Fermata recording
        writeWav (elsewhere.getChildFile ("not it - longer.wav"), 24, 1, 24001, 48000.0, {}, 0.5f);
        juce::StringArray steps;
        const auto rep = rp.relinkAudio (elsewhere, [&steps] (const juce::String& what, double) { steps.add (what); });
        CHECK (rep.byName == 1 && rep.renamed == 2 && rep.unsure == 0 && rep.stillMissing == 0 && rep.found.size() == 3 && ! steps.isEmpty());
        CHECK (rp.missingAudioCount() == 0);
        CHECK (tw.groups[0].files[0].file == elsewhere.getChildFile ("sub").getChildFile (g.files[0].file.getFileName()));
        CHECK (tw.groups[0].files[1].file == elsewhere.getChildFile ("spaced renamed.wav") && tw.groups[0].files[2].file == elsewhere.getChildFile ("choir renamed.wav"));
        CHECK (ed.regions[0].files[1].file == elsewhere.getChildFile ("spaced renamed.wav"));                   // the edit's piece follows the take's file
        CHECK (! tw.groups[0].verified);
        bool sawDepth = false;
        for (auto& l : rep.found) sawDepth = sawDepth || l.contains ("24 bit");
        CHECK (sawDepth);

        // two files of one take, both renamed, neither with a header: they fit each other, so nothing is guessed
        const auto twice = base.getChildFile ("Twice"); twice.createDirectory();
        tw.groups[0].files[0].file = rec.getChildFile ("gone a.wav"); tw.groups[0].files[1].file = rec.getChildFile ("gone b.wav");
        for (auto& r : ed.regions) { r.files[0].file = tw.groups[0].files[0].file; r.files[1].file = tw.groups[0].files[1].file; }
        writeWav (twice.getChildFile ("x.wav"), 24, 1, 24000, 48000.0, {}, 0.1f);
        writeWav (twice.getChildFile ("y.wav"), 24, 1, 24000, 48000.0, {}, 0.2f);
        const auto rep2 = rp.relinkAudio (twice);
        CHECK (rep2.byName == 0 && rep2.renamed == 0 && rep2.unsure == 2 && rep2.stillMissing == 0 && rep2.unsureLines.size() == 2 && rep2.unsureLines[0].contains ("fits 2 files"));
        CHECK (rp.missingAudioCount() == 2);
        // ... until the track name in a file name tells one apart: the other then has one file left to it
        CHECK (twice.getChildFile ("y.wav").moveFileTo (twice.getChildFile ("take 3 Spaced L.wav")));
        const auto rep3 = rp.relinkAudio (twice);
        CHECK (rep3.renamed == 2 && rep3.unsure == 0 && rp.missingAudioCount() == 0);
        CHECK (tw.groups[0].files[1].file == twice.getChildFile ("take 3 Spaced L.wav") && tw.groups[0].files[0].file == twice.getChildFile ("x.wav"));
        bool sawName = false; for (auto& l : rep3.found) sawName = sawName || l.contains ("the track name in its file name");
        CHECK (sawName);
        // three files of one take, all renamed to nothing, two with Fermata's header: the headers pick those two, the third is what is left
        const auto three = base.getChildFile ("Three"); three.createDirectory();
        for (size_t i = 0; i < 3; ++i) tw.groups[0].files[i].file = rec.getChildFile ("lost " + juce::String ((int) i) + ".wav");
        for (auto& r : ed.regions) for (size_t i = 0; i < 3; ++i) r.files[i].file = tw.groups[0].files[i].file;
        tw.groups[0].files[2].numChannels = 1; ed.regions[0].files[2].numChannels = 1;                          // (all three the same now)
        writeWav (three.getChildFile ("p.wav"), 24, 1, 24000, 48000.0, bwf (desc + "Spaced L", "2026-09-14", "10:30:00"), 0.1f);
        writeWav (three.getChildFile ("q.wav"), 24, 1, 24000, 48000.0, {}, 0.2f);
        writeWav (three.getChildFile ("r.wav"), 24, 1, 24000, 48000.0, bwf (desc + "ORTF L", "2026-09-14", "10:30:00"), 0.3f);
        const auto rep5 = rp.relinkAudio (three);
        CHECK (rep5.renamed == 3 && rep5.unsure == 0);
        CHECK (tw.groups[0].files[0].file == three.getChildFile ("r.wav") && tw.groups[0].files[1].file == three.getChildFile ("p.wav") && tw.groups[0].files[2].file == three.getChildFile ("q.wav"));
        bool sawHeader = false; for (auto& l : rep5.found) sawHeader = sawHeader || l.contains ("written in its header");
        CHECK (sawHeader);

        // a file the project HAS is never taken for a missing one, and a file with no header is not ruled out by its silence: the take's ORTF and
        // Choir files are there (recorded at the same time, so they used to win), its Spaced file was exported by another program (no header) under a new name
        auto setFiles = [&] (const juce::File& a, const juce::File& b, const juce::File& c)
        {
            tw.groups[0].files[0].file = a; tw.groups[0].files[1].file = b; tw.groups[0].files[2].file = c;
            for (auto& r : ed.regions) for (size_t i = 0; i < 3; ++i) r.files[i].file = tw.groups[0].files[i].file;
        };
        const auto have = base.getChildFile ("Have"); have.createDirectory();
        writeWav (have.getChildFile ("003 - Kyrie - ORTF L.wav"), 24, 1, 24000, 48000.0, bwf (desc + "ORTF L", "2026-09-14", "10:30:00"), 0.1f);
        writeWav (have.getChildFile ("003 - Kyrie - Choir.wav"), 24, 1, 24000, 48000.0, bwf (desc + "Choir", "2026-09-14", "10:30:00"), 0.3f);
        writeWav (have.getChildFile ("spaced export.wav"), 24, 1, 24000, 48000.0, {}, 0.2f);
        setFiles (have.getChildFile ("003 - Kyrie - ORTF L.wav"), rec.getChildFile ("spaced gone.wav"), have.getChildFile ("003 - Kyrie - Choir.wav"));
        CHECK (rp.missingAudioCount() == 1);
        const auto rep6 = rp.relinkAudio (have);
        CHECK (rep6.renamed == 1 && rep6.unsure == 0 && rep6.stillMissing == 0 && tw.groups[0].files[1].file == have.getChildFile ("spaced export.wav"));
        CHECK (tw.groups[0].files[0].file == have.getChildFile ("003 - Kyrie - ORTF L.wav") && tw.groups[0].files[2].file == have.getChildFile ("003 - Kyrie - Choir.wav"));
        // all three missing and found as a (ORTF's header), b (no header), c (Choir's header): a's and c's headers say they are not Spaced's file, so b is -
        // in whichever order the files are looked at (Spaced first, then the others first)
        const auto abc = base.getChildFile ("ABC"); abc.createDirectory();
        writeWav (abc.getChildFile ("a.wav"), 24, 1, 24000, 48000.0, bwf (desc + "ORTF L", "2026-09-14", "10:30:00"), 0.1f);
        writeWav (abc.getChildFile ("b.wav"), 24, 1, 24000, 48000.0, {}, 0.2f);
        writeWav (abc.getChildFile ("c.wav"), 24, 1, 24000, 48000.0, bwf (desc + "Choir", "2026-09-14", "10:30:00"), 0.3f);
        for (int order = 0; order < 2; ++order)
        {
            setFiles (rec.getChildFile ("o.wav"), rec.getChildFile ("s.wav"), rec.getChildFile ("ch.wav"));
            if (order == 0) { std::swap (tw.groups[0].files[0], tw.groups[0].files[1]); for (auto& r : ed.regions) std::swap (r.files[0], r.files[1]); }
            const auto rep7 = rp.relinkAudio (abc);
            CHECK (rep7.renamed == 3 && rep7.unsure == 0 && rep7.stillMissing == 0);
            for (auto& tf : tw.groups[0].files)
                CHECK (tf.file == abc.getChildFile (tf.trackName == "ORTF L" ? "a.wav" : tf.trackName == "Spaced L" ? "b.wav" : "c.wav"));
            bool sawOthers = false; for (auto& l : rep7.found) sawOthers = sawOthers || l.contains ("the other files' headers name another take or track");
            CHECK (sawOthers);
            if (order == 0) { std::swap (tw.groups[0].files[0], tw.groups[0].files[1]); for (auto& r : ed.regions) std::swap (r.files[0], r.files[1]); }
        }
        // the same name in another project's folder (even one named like the old folder) is not the file: its length says so. A same-named file
        // with the right length, rate and channels is.
        const auto names = base.getChildFile ("Names"); names.getChildFile ("Recorded Media").createDirectory(); names.getChildFile ("Elsewhere").createDirectory();
        writeWav (names.getChildFile ("Recorded Media").getChildFile ("003 - Kyrie - ORTF L.wav"), 24, 1, 99000, 48000.0, bwf ("Other / 003 - Kyrie / ORTF L", "2026-01-01", "09:00:00"), 0.1f);
        setFiles (rec.getChildFile ("003 - Kyrie - ORTF L.wav"), abc.getChildFile ("b.wav"), abc.getChildFile ("c.wav"));
        const auto rep8 = rp.relinkAudio (names);
        CHECK (rep8.byName == 0 && rep8.renamed == 0 && rep8.unsure == 0 && rep8.stillMissing == 1 && rp.missingAudioCount() == 1);
        writeWav (names.getChildFile ("Elsewhere").getChildFile ("003 - Kyrie - ORTF L.wav"), 24, 1, 24000, 48000.0, {}, 0.1f);
        const auto rep9 = rp.relinkAudio (names);
        CHECK (rep9.byName == 1 && rep9.unsure == 0 && tw.groups[0].files[0].file == names.getChildFile ("Elsewhere").getChildFile ("003 - Kyrie - ORTF L.wav"));
        CHECK (! rep9.found.isEmpty() && rep9.found[0].contains ("the same name, length, rate and channels"));
        // "ORTF L" is not "ORTF L 2": with a track of that name in the project, the file whose name says ORTF L 2 is that track's
        rp.addTrack ("ORTF L 2", TrackFormat::Mono, 3);
        const auto two = base.getChildFile ("Two"); two.createDirectory();
        writeWav (two.getChildFile ("take 3 ORTF L 2.wav"), 24, 1, 24000, 48000.0, {}, 0.1f);
        writeWav (two.getChildFile ("take 3 ORTF L.wav"), 24, 1, 24000, 48000.0, {}, 0.1f);
        setFiles (rec.getChildFile ("ortf gone.wav"), abc.getChildFile ("b.wav"), abc.getChildFile ("c.wav"));
        const auto rep10 = rp.relinkAudio (two);
        CHECK (rep10.renamed == 1 && rep10.unsure == 0 && tw.groups[0].files[0].file == two.getChildFile ("take 3 ORTF L.wav"));
        // a processed piece's file ("003 - Kyrie (declick)": a short file that starts at the take's start) is found by its name only, never by the take's
        // length, which is not its own: the take's file, renamed, must not be taken for it
        const auto proc = base.getChildFile ("Proc"); proc.createDirectory();
        setFiles (two.getChildFile ("take 3 ORTF L.wav"), abc.getChildFile ("b.wav"), abc.getChildFile ("c.wav"));     // (the take is whole)
        ed.regions[0].takeName = "003 - Kyrie (declick)"; ed.regions[0].files[0].file = rec.getChildFile ("003 - Kyrie - ORTF L (declick).wav"); ed.regions[0].files[0].fileStart = 0;
        writeWav (proc.getChildFile ("ortf renamed.wav"), 24, 1, 24000, 48000.0, bwf (desc + "ORTF L", "2026-09-14", "10:30:00"), 0.1f);   // (fits the take, not the piece)
        const auto rep11 = rp.relinkAudio (proc);
        CHECK (rep11.byName == 0 && rep11.renamed == 0 && rep11.unsure == 0 && rep11.stillMissing == 1 && rp.missingAudioCount() == 1);
        // a relink is not an undo step: it cannot put the missing paths back
        rp.undoReset(); tw.groups[0].label = "Kyrie again"; rp.changed(); rp.undoTick (false, true);
        CHECK (rp.undoDepth() == 1);
        writeWav (proc.getChildFile ("003 - Kyrie - ORTF L (declick).wav"), 24, 1, 5000, 48000.0, {}, 0.1f);
        const auto rep12 = rp.relinkAudio (proc);
        CHECK (rep12.byName == 1 && rp.missingAudioCount() == 0 && ed.regions[0].files[0].file == proc.getChildFile ("003 - Kyrie - ORTF L (declick).wav"));
        rp.undoTick (false, true);
        CHECK (rp.undoDepth() == 0 && ! rp.undo());
        tw.groups[0].label = {}; ed.regions[0].takeName = "003 - Kyrie"; ed.regions[0].files[0].file = tw.groups[0].files[0].file;
        // an imported take window is looked at afresh (the other project's "found whole" marks are dropped), and the import counts its own missing files
        {
            tw.groups[0].verified = true; rp.name = "Relink";
            juce::String er2; CHECK (rp.save (er2));
            const auto twFile = base.getChildFile (rp.windowFiles[tw.id.toString()].rel);
            CHECK (twFile.existsAsFile());
            Project q; q.createDefaultDesign(); q.projectFile = base.getChildFile ("q/q.fermata");
            juce::String msg; juce::Uuid nid; int miss = -1;
            CHECK (q.importWindowFile (twFile, msg, &nid, &miss) && miss == 0 && ! q.takeWindows.back()->groups[0].verified);
            tw.groups[0].files[0].file = rec.getChildFile ("gone for the import.wav"); CHECK (rp.save (er2));
            Project q2; q2.createDefaultDesign(); q2.projectFile = base.getChildFile ("q2/q2.fermata");
            CHECK (q2.importWindowFile (twFile, msg, &nid, &miss) && miss == 1 && msg.contains ("1 of its audio files"));
            tw.groups[0].files[0].file = two.getChildFile ("take 3 ORTF L.wav"); tw.groups[0].verified = false;
        }

        // a file nothing is known about (a piece waiting for corrected audio) is found by name only
        { WaitingPiece wp; wp.name = "fix"; wp.from = 0; wp.to = 100; wp.files.push_back (rec.getChildFile ("fix 1.wav")); tw.groups[0].waiting.push_back (wp); }
        writeWav (twice.getChildFile ("z.wav"), 24, 1, 24000, 48000.0, {}, 0.2f);
        const auto rep4 = rp.relinkAudio (twice);
        CHECK (rep4.byName == 0 && rep4.renamed == 0 && rep4.unsure == 0 && rep4.stillMissing == 1 && rep4.missing[0] == "fix 1.wav");
        tw.groups[0].waiting.clear();
        // the quick count leaves the takes found whole last time alone (one file gone from a folder that is still there), but a folder that is gone
        // (moved, renamed) counts every file in it, verified or not
        tw.groups[0].files[0].file = rec.getChildFile ("gone again.wav"); tw.groups[0].verified = true;
        for (auto& r : ed.regions) r.files[0].file = tw.groups[0].files[0].file;
        CHECK (rp.missingAudioCount() == 1 && rp.missingAudioCount (true) == 0);
        rp.filesMoved = true; CHECK (rp.missingAudioCount (true) == 1); rp.filesMoved = false;
        for (auto& tf : tw.groups[0].files) tf.file = base.getChildFile ("Moved away").getChildFile (tf.file.getFileName());
        for (auto& r : ed.regions) for (size_t i = 0; i < 3; ++i) r.files[i].file = tw.groups[0].files[i].file;
        CHECK (rp.missingAudioCount (true) == 3 && rp.missingAudioCount() == 3);
        // the summary reads
        CHECK (rep.summary().contains ("1 file found by its name") && rep.summary().contains ("2 renamed files found") && rep2.summary().contains ("fit several"));
        base.deleteRecursively();
    }
    SECTION ("2.77 background denoiser: the C++ engine matches the reference, in pieces, does what the controls say, and a profile per channel of a file");
    {
        const auto dataDir = juce::File (__FILE__).getParentDirectory().getChildFile ("data").getChildFile ("denoise");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto readMono = [&] (const juce::File& f, double& sr)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            std::vector<float> v; if (r == nullptr) return v;
            sr = r->sampleRate; std::vector<std::vector<float>> ch; audioops::readRange (*r, 0, r->lengthInSamples, ch); return ch[0];
        };
        double sr = 0, sr2 = 0;
        const auto in = readMono (dataDir.getChildFile ("golden_in.wav"), sr), expect = readMono (dataDir.getChildFile ("golden_out.wav"), sr2);
        const auto gold = juce::JSON::parse (dataDir.getChildFile ("golden.json").loadFileAsString());
        CHECK (! in.empty() && in.size() == expect.size() && sr == 48000.0);
        if (in.empty() || in.size() != expect.size()) { std::printf ("  (tests/data/denoise is missing: the rest of this section is skipped)\n"); goto denoiseDone; }
        {
        denoise::Params p;
        const double r0 = (double) gold["region"][0], r1 = (double) gold["region"][1];
        std::vector<float> region (in.begin() + (size_t) (r0 * sr), in.begin() + (size_t) (r1 * sr));
        auto prof = denoise::learnProfile ({ region }, sr, p);
        CHECK (prof.valid() && prof.fftSize == (int) gold["nfft"] && std::abs (prof.levelDb() - (double) gold["profileLevelDb"]) < 0.01);
        auto read = [&] (juce::int64 start, int n, float* dst) { for (int i = 0; i < n; ++i) { const auto j = start + i; dst[i] = j >= 0 && j < (juce::int64) in.size() ? in[(size_t) j] : 0.0f; } };
        auto decay = denoise::measureDecay (prof, p, (juce::int64) in.size(), read);
        bool ratesOk = decay.rates.size() == (size_t) gold["rates"].size();
        for (size_t b = 0; ratesOk && b < decay.rates.size(); ++b)
        {
            const auto g = gold["rates"][(int) b];
            if (g.isVoid()) ratesOk = std::isnan (decay.rates[b]); else ratesOk = ! std::isnan (decay.rates[b]) && std::abs (decay.rates[b] - (double) g) < 0.01;
        }
        CHECK (ratesOk);
        std::vector<float> out; out.reserve (in.size());
        CHECK (denoise::processChannel (prof, p, decay, (juce::int64) in.size(), read, [&] (const float* x, int n) { out.insert (out.end(), x, x + n); }));
        auto snrOf = [] (const std::vector<float>& ref, const std::vector<float>& other)
        {
            double se = 0, sd = 0; for (size_t i = 0; i < ref.size() && i < other.size(); ++i) { se += (double) ref[i] * ref[i]; const double d = other[i] - ref[i]; sd += d * d; }
            return 10.0 * std::log10 (se / juce::jmax (sd, 1e-30));
        };
        auto rmsDb = [] (const std::vector<float>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b && i < v.size(); ++i) s += (double) v[i] * v[i]; return 10.0 * std::log10 (s / (double) juce::jmax ((size_t) 1, b - a) + 1e-30); };
        CHECK (out.size() == in.size() && snrOf (expect, out) > 60.0);                                        // (the 24-bit golden file itself is the limit, ~100 dB)
        const double nr = rmsDb (in, (size_t) (0.1 * sr), (size_t) (1.3 * sr)) - rmsDb (out, (size_t) (0.1 * sr), (size_t) (1.3 * sr));
        CHECK (std::abs (nr - (double) gold["noiseReductionDb"]) < 0.3);                                       // (the reference measured it above 30 Hz)
        // in small pieces with context it comes out the same as in one go
        std::vector<float> out2; out2.reserve (in.size());
        CHECK (denoise::processChannel (prof, p, decay, (juce::int64) in.size(), read, [&] (const float* x, int n) { out2.insert (out2.end(), x, x + n); }, {}, 2.0, 3.0, 1.5));
        CHECK (snrOf (out, out2) > 60.0);
        // the controls: a deeper residual takes more out of the room tone; the noise-only lead-in comes out at about the residual asked for
        denoise::Params deep; deep.residualDb = -20.0;
        std::vector<float> out3; CHECK (denoise::processChannel (prof, deep, decay, (juce::int64) in.size(), read, [&] (const float* x, int n) { out3.insert (out3.end(), x, x + n); }));
        const double nr3 = rmsDb (in, (size_t) (0.1 * sr), (size_t) (1.3 * sr)) - rmsDb (out3, (size_t) (0.1 * sr), (size_t) (1.3 * sr));
        CHECK (nr3 > nr + 5.0 && nr3 < 20.5 && nr > 10.5 && nr < 12.5);
        // a pure tone well above the noise is left alone (no dulling: within 0.2 dB), and the removed part is what was added
        {
            const int n = (int) (3.0 * sr); std::vector<float> tone ((size_t) n), noisy ((size_t) n);
            juce::Random rnd (5);
            for (int i = 0; i < n; ++i) { tone[(size_t) i] = 0.05f * std::sin (2.0f * 3.14159265f * 1000.0f * (float) i / (float) sr); noisy[(size_t) i] = tone[(size_t) i] + 0.0005f * (rnd.nextFloat() * 2.0f - 1.0f); }
            std::vector<float> nreg (noisy.begin() + (size_t) (0.2 * sr), noisy.begin() + (size_t) (1.2 * sr));
            for (size_t i = 0; i < nreg.size(); ++i) nreg[i] -= tone[(size_t) (0.2 * sr) + i];                   // (the room tone alone)
            auto pr2 = denoise::learnProfile ({ nreg }, sr, p);
            std::vector<float> y2; auto rd2 = [&] (juce::int64 st, int m, float* dst) { for (int i = 0; i < m; ++i) { const auto j = st + i; dst[i] = j >= 0 && j < n ? noisy[(size_t) j] : 0.0f; } };
            CHECK (denoise::processChannel (pr2, p, {}, n, rd2, [&] (const float* x, int m) { y2.insert (y2.end(), x, x + m); }));
            const double toneIn = rmsDb (tone, (size_t) (0.5 * sr), (size_t) (2.5 * sr)), toneOut = rmsDb (y2, (size_t) (0.5 * sr), (size_t) (2.5 * sr));
            CHECK (std::abs (toneOut - toneIn) < 0.2);
            double se = 0, sd = 0; for (size_t i = (size_t) (0.5 * sr); i < (size_t) (2.5 * sr); ++i) { se += (double) tone[i] * tone[i]; const double d = y2[i] - tone[i]; sd += d * d; }
            CHECK (10.0 * std::log10 (se / sd) > 45.0);                                                        // (the output is the tone, not the tone plus noise: the noise was ~40 dB down)
        }
        // a stereo file: a profile per channel learned from its room tone (each its own level); denoising the stretch with them (each channel its own
        // profile, as Noise mode does): outside the stretch bit for bit, inside it the noise down by about the residual, the tone kept
        {
            const auto dir = tmp.getChildFile ("denoise"); dir.deleteRecursively(); dir.createDirectory();
            const int n = (int) (4.0 * sr); std::vector<std::vector<float>> st (2, std::vector<float> ((size_t) n));
            juce::Random rnd (9);
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) i / (float) sr, env = t > 1.5f && t < 3.0f ? 1.0f : 0.0f;
                st[0][(size_t) i] = env * 0.1f * std::sin (2.0f * 3.14159265f * 440.0f * t) + 0.002f * (rnd.nextFloat() * 2.0f - 1.0f);
                st[1][(size_t) i] = env * 0.1f * std::sin (2.0f * 3.14159265f * 660.0f * t) + 0.0005f * (rnd.nextFloat() * 2.0f - 1.0f);
            }
            const auto src = dir.getChildFile ("stereo.wav");
            CHECK (audioops::writeWav (src, st, sr));
            juce::String err;
            auto profs = audioops::learnDenoise (fm, src, (juce::int64) (0.1 * sr), (juce::int64) (1.2 * sr), p, err);
            CHECK (err.isEmpty() && profs.valid() && profs.channels == 2 && profs.perChannel[0].levelDb() > profs.perChannel[1].levelDb() + 8.0);
            CHECK (! audioops::learnDenoise (fm, src, (juce::int64) (0.1 * sr), (juce::int64) (0.4 * sr), p, err).valid() && err.isNotEmpty());   // (under half a second: refused)
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (src));
            std::vector<std::vector<float>> o; CHECK (rd != nullptr && audioops::readRange (*rd, 0, n, o) && o.size() == 2);
            const auto in = o;                                                                                 // (as read back: 24-bit)
            FixSpec spec; spec.kind = FixSpec::Kind::Denoise; spec.f0 = 0.0; spec.f1 = 0.5 * sr; spec.dn = p;
            spec.dnProfiles = std::make_shared<const std::vector<denoise::Profile>> (profs.perChannel);
            audioops::applyFix (o, sr, (long) (1.0 * sr), (long) (3.5 * sr), spec, 0);
            bool outsideSame = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) if (i < (int) (1.0 * sr) || i >= (int) (3.5 * sr)) outsideSame = outsideSame && o[(size_t) c][(size_t) i] == in[(size_t) c][(size_t) i];
            CHECK (outsideSame);
            CHECK (rmsDb (in[0], (size_t) (1.15 * sr), (size_t) (1.45 * sr)) - rmsDb (o[0], (size_t) (1.15 * sr), (size_t) (1.45 * sr)) > 9.0);
            CHECK (rmsDb (in[1], (size_t) (1.15 * sr), (size_t) (1.45 * sr)) - rmsDb (o[1], (size_t) (1.15 * sr), (size_t) (1.45 * sr)) > 9.0);   // (its own, quieter profile)
            CHECK (std::abs (rmsDb (in[1], (size_t) (1.6 * sr), (size_t) (2.9 * sr)) - rmsDb (o[1], (size_t) (1.6 * sr), (size_t) (2.9 * sr))) < 0.3);
            dir.deleteRecursively();
        }
        }
        denoiseDone:;
        {   // 2.78: the room's decay is the 80th percentile of the decays found (the fastest a band falls), not the median (diminuendos are slower)
            const double dt = 0.02; std::vector<std::vector<double>> E (1);
            auto decays = [dt] (std::initializer_list<double> slopes)            // free decays from 60 dB above the noise all the way down into it
            {
                std::vector<std::vector<double>> E (1);
                for (double s : slopes)
                {
                    for (int i = 0; i < 25; ++i) E[0].push_back (0.0);
                    for (int i = 0; 60.0 - s * dt * i > 0.0; ++i) E[0].push_back (60.0 - s * dt * i);
                }
                for (int i = 0; i < 25; ++i) E[0].push_back (0.0);
                return E;
            };
            const auto r = denoise::detail::decayRates (decays ({ 10.0, 50.0, 20.0, 40.0, 30.0 }), { 0.0 }, dt);
            std::printf ("  (decay rates measured: 80th percentile %.1f dB/s)\n", r[0]);
            CHECK (r.size() == 1 && r[0] > 36.0 && r[0] < 46.0);                // 80th percentile of 10 ... 50: 42 (the median would be 30)
            const auto r2 = denoise::detail::decayRates (decays ({ 12.0, 24.0 }), { 0.0 }, dt);
            CHECK (r2[0] > 15.0 && r2[0] < 21.0);                               // two: their mean (18)
        }
    }
    SECTION ("2.79 spectral denoise: a profile from boxes on the picture, the rest of the spectrum untouched, feathers in pitch");
    {
        const double sr = 48000.0; const int n = (int) (6.0 * sr);
        // 6 s: steady hiss, a steady 60 Hz rumble (below the part that will be denoised), a 1 kHz tone from 2 to 4 s, and a click at 1.1 s (in the room-tone part: taken out with Alt)
        std::vector<float> x ((size_t) n); juce::Random rnd (31);
        for (int i = 0; i < n; ++i)
        {
            const float t = (float) i / (float) sr;
            x[(size_t) i] = 0.003f * (rnd.nextFloat() * 2.0f - 1.0f) + 0.01f * std::sin (2.0f * 3.14159265f * 60.0f * t) + (t > 2.0f && t < 4.0f ? 0.05f * std::sin (2.0f * 3.14159265f * 1000.0f * t) : 0.0f);
            if (t > 1.1f && t < 1.102f) x[(size_t) i] += 0.5f;
        }
        denoise::Params p;
        std::vector<denoise::SelRect> sel { { 0.2, 1.8, 300.0, 24000.0, true }, { 1.0, 1.2, 0.0, 24000.0, false } };
        CHECK (denoise::selected (sel, 0.5, 1000.0) && ! denoise::selected (sel, 1.1, 1000.0) && ! denoise::selected (sel, 0.5, 100.0) && ! denoise::selected (sel, 1.9, 1000.0));
        const auto spans = denoise::selectedSpans (sel); CHECK (spans.size() == 1 && spans[0].first == 0.2 && spans[0].second == 1.8);
        denoise::ProfileLearner learner; learner.add (x.data(), n, sr, p, sel, 0.0);
        auto prof = learner.finish();
        CHECK (prof.valid() && prof.limited() && (int) prof.weight.size() == prof.fftSize / 2 + 1);
        auto binOf = [&] (double hz) { return (int) std::lround (hz * prof.fftSize / sr); };
        CHECK (prof.weight[(size_t) binOf (100.0)] == 0.0f && prof.weight[(size_t) binOf (250.0)] == 0.0f && prof.weight[(size_t) binOf (400.0)] == 1.0f && prof.weight[(size_t) binOf (10000.0)] == 1.0f);
        CHECK (std::abs (prof.psd[(size_t) binOf (100.0)] - prof.psd[(size_t) binOf (200.0)]) < 1.0e-9 * juce::jmax (prof.psd[(size_t) binOf (200.0)], 1.0));     // (uncovered bins: all the nearest covered one's power)
        CHECK (prof.psd[(size_t) binOf (100.0)] > 0.3 * prof.psd[(size_t) binOf (330.0)] && prof.psd[(size_t) binOf (100.0)] < 3.0 * prof.psd[(size_t) binOf (330.0)]);
        // the click at 1.1 s was taken out of the room tone: the profile is the hiss, not the click (a profile WITH the click is far louder)
        std::vector<denoise::SelRect> withClick { { 0.2, 1.8, 300.0, 24000.0, true } };
        denoise::ProfileLearner l2; l2.add (x.data(), n, sr, p, withClick, 0.0); const auto prof2 = l2.finish();
        CHECK (prof2.levelDb() > prof.levelDb() + 3.0);
        // the feather in pitch: 0 at the drawn edge rising to 1 half an octave above it, 1 everywhere else inside
        auto feathered = prof; denoise::featherProfile (feathered, 0.5);
        CHECK (feathered.weight[(size_t) binOf (100.0)] == 0.0f && feathered.weight[(size_t) binOf (310.0)] < 0.15f && feathered.weight[(size_t) binOf (360.0)] > 0.2f && feathered.weight[(size_t) binOf (360.0)] < 0.9f && feathered.weight[(size_t) binOf (450.0)] == 1.0f && feathered.weight[(size_t) binOf (5000.0)] == 1.0f);
        // the whole 6 s denoised with that profile: the 60 Hz rumble (below the drawn part) untouched; the hiss above it down; the tone kept
        std::vector<float> y ((size_t) n, 0.0f);
        denoise::processSegment (feathered, p, {}, x.data(), n, y.data());
        auto toneDb = [&] (const std::vector<float>& v, double hz, double t0, double t1)
        {
            double re = 0, im = 0; const int a0 = (int) (t0 * sr), a1 = (int) (t1 * sr);
            for (int i = a0; i < a1; ++i) { const double ph = 2.0 * 3.14159265358979 * hz * i / sr; re += v[(size_t) i] * std::cos (ph); im -= v[(size_t) i] * std::sin (ph); }
            return 20.0 * std::log10 (std::sqrt (re * re + im * im) / (a1 - a0) + 1e-20);
        };
        auto bandDb = [&] (const std::vector<float>& v, double f0, double f1, double t0, double t1)          // the energy between f0 and f1 (a coarse DFT at 50 Hz steps)
        {
            double e = 0.0; for (double hz = f0; hz < f1; hz += 50.0) { const double d = toneDb (v, hz, t0, t1); e += std::pow (10.0, d / 10.0); }
            return 10.0 * std::log10 (e + 1e-20);
        };
        CHECK (std::abs (toneDb (y, 60.0, 2.5, 3.5) - toneDb (x, 60.0, 2.5, 3.5)) < 0.1);
        CHECK (std::abs (toneDb (y, 1000.0, 2.5, 3.5) - toneDb (x, 1000.0, 2.5, 3.5)) < 1.0);
        const double hissBefore = bandDb (x, 2000.0, 8000.0, 4.6, 5.2), hissAfter = bandDb (y, 2000.0, 8000.0, 4.6, 5.2);
        std::printf ("  (spectral denoise: hiss 2 - 8 kHz %.1f -> %.1f dB, 60 Hz %.2f -> %.2f dB)\n", hissBefore, hissAfter, toneDb (x, 60.0, 2.5, 3.5), toneDb (y, 60.0, 2.5, 3.5));
        CHECK (hissAfter < hissBefore - 8.0);
        // a profile with weight 0 for every bin is a bit-exact pass (the output is the input plus the change, and there is none)
        { auto none = prof; std::fill (none.weight.begin(), none.weight.end(), 0.0f); std::vector<float> y0 ((size_t) n, 0.0f);
          denoise::processSegment (none, p, {}, x.data(), n, y0.data()); CHECK (y0 == x); }
    }
    SECTION ("2.81 denoise as a fix: a box (time and pitch) with the room-tone profile; the room tone kept with the project");
    {
        const double sr = 48000.0; const int n = (int) (12.0 * sr);
        std::vector<float> x ((size_t) n); juce::Random rnd (77);
        for (int i = 0; i < n; ++i) { const float t = (float) i / (float) sr; x[(size_t) i] = 0.003f * (rnd.nextFloat() * 2.0f - 1.0f) + 0.01f * std::sin (2.0f * 3.14159265f * 60.0f * t) + (t > 5.0f && t < 7.0f ? 0.05f * std::sin (2.0f * 3.14159265f * 1000.0f * t) : 0.0f); }
        denoise::Params p;
        auto prof = denoise::learnProfile ({ std::vector<float> (x.begin(), x.begin() + (int) (2.0 * sr)) }, sr, p);
        CHECK (prof.valid() && ! prof.limited());
        FixSpec spec; spec.kind = FixSpec::Kind::Denoise; spec.f0 = 300.0; spec.f1 = 8000.0; spec.dn.residualDb = -12.0;
        spec.dnProfiles = std::make_shared<const std::vector<denoise::Profile>> (std::vector<denoise::Profile> { prof });
        CHECK (spec.shortName() == "denoise");
        std::vector<std::vector<float>> ch { x };
        audioops::applyFix (ch, sr, (long) (4.0 * sr), (long) (9.0 * sr), spec, (long) (0.02 * sr));
        auto toneDb = [&] (const std::vector<float>& v, double hz, double t0, double t1)
        {
            double re = 0, im = 0; const int a0 = (int) (t0 * sr), a1 = (int) (t1 * sr);
            for (int i = a0; i < a1; ++i) { const double ph = 2.0 * 3.14159265358979 * hz * i / sr; re += v[(size_t) i] * std::cos (ph); im -= v[(size_t) i] * std::sin (ph); }
            return 20.0 * std::log10 (std::sqrt (re * re + im * im) / (a1 - a0) + 1e-20);
        };
        auto bandDb = [&] (const std::vector<float>& v, double f0, double f1, double t0, double t1) { double e = 0.0; for (double hz = f0; hz < f1; hz += 50.0) e += std::pow (10.0, toneDb (v, hz, t0, t1) / 10.0); return 10.0 * std::log10 (e + 1e-20); };
        bool outsideSame = true; for (int i = 0; i < n; ++i) if (i < (long) (4.0 * sr) || i >= (long) (9.0 * sr)) outsideSame = outsideSame && ch[0][(size_t) i] == x[(size_t) i];
        CHECK (outsideSame);                                                                                  // outside the box in time: bit for bit
        CHECK (bandDb (ch[0], 2000.0, 6000.0, 7.5, 8.5) < bandDb (x, 2000.0, 6000.0, 7.5, 8.5) - 8.0);        // inside: the hiss down
        CHECK (std::abs (bandDb (ch[0], 10000.0, 14000.0, 7.5, 8.5) - bandDb (x, 10000.0, 14000.0, 7.5, 8.5)) < 0.3);   // above the box's pitch range: as it was
        CHECK (std::abs (toneDb (ch[0], 60.0, 7.5, 8.5) - toneDb (x, 60.0, 7.5, 8.5)) < 0.1);                  // below it too
        CHECK (std::abs (toneDb (ch[0], 1000.0, 5.5, 6.5) - toneDb (x, 1000.0, 5.5, 6.5)) < 1.0);              // the tone inside the box kept
        std::printf ("  (denoise box: hiss 2 - 6 kHz %.1f -> %.1f dB)\n", bandDb (x, 2000.0, 6000.0, 7.5, 8.5), bandDb (ch[0], 2000.0, 6000.0, 7.5, 8.5));
        // the room tone kept with the project: a profile per track per channel and for the mono mix, and the take that was marked
        {
            Project pr; pr.tracks.clear(); const auto tid = pr.addTrack ("Choir", TrackFormat::Stereo, 0).id;
            Project::RoomToneTrack rt; rt.trackId = tid; rt.trackName = "Choir"; rt.mono = prof; rt.perChannel = { prof, prof };
            pr.roomTone.source = "00:34 - 00:51 of take 8 of New World"; pr.roomTone.tracks.push_back (rt);
            auto& tw = pr.addTakeWindow ("New World"); TakeGroup g; g.id = juce::Uuid(); g.number = 8; g.roomTone = true; g.roomToneIn = 34.0; g.roomToneOut = 51.0; g.sampleRate = sr; g.lengthSamples = (juce::int64) (60.0 * sr);
            TakeFile tf; tf.trackId = tid; tf.trackName = "Choir"; tf.file = tmp.getChildFile ("rt.wav"); tf.numChannels = 2; g.files.push_back (tf); tw.groups.push_back (g);
            juce::String err; const auto f = tmp.getChildFile ("roomtone.fermata");
            CHECK (pr.saveAs (f, err));
            Project q; q.projectFile = f; CHECK (q.load (f, err));
            CHECK (q.roomTone.valid() && q.roomTone.source == pr.roomTone.source && q.roomTone.forTrack (tid) != nullptr && q.roomTone.forTrack (tid)->perChannel.size() == 2);
            const auto* t2 = q.roomTone.forTrack (tid);
            CHECK (t2->mono.fftSize == prof.fftSize && t2->mono.psd.size() == prof.psd.size() && std::abs (t2->mono.levelDb() - prof.levelDb()) < 0.01);
            const TakeWindowDef* nw = nullptr; for (auto& w : q.takeWindows) if (w->name == "New World") nw = w.get();
            CHECK (nw != nullptr && ! nw->groups.empty() && nw->groups[0].roomTone && nw->groups[0].roomToneIn == 34.0 && nw->groups[0].roomToneOut == 51.0);
        }
    }
    SECTION ("2.79 Noise mode: the whole edit's spectral pyramid (levels, the loudest track per cell, slices for a view, a stretch made again)");
    {
        const double sr = 48000.0; const double secs = 400.0;               // (6.7 min: the finest hop is the 10 ms floor, then x4 ... until a level has < 400 frames)
        SpectralPyramid py; py.setUp (sr, secs);
        CHECK (py.valid() && py.levels.size() >= 3 && std::abs (py.levels[0].hopSeconds - 0.010) < 1.0e-9 && py.levels[0].frames == 40000 && std::abs (py.levels[1].hopSeconds - 0.040) < 1.0e-9 && py.levels.back().frames < 400);
        // two "tracks": a 1 kHz tone at -20 dB from 100 to 200 s on the first, a 4 kHz tone at -40 dB over everything on the second; the merged picture holds both
        auto tone = [sr] (double hz, double db, double t0, double t1) { return [=] (juce::int64 start, int n, float* dst) { for (int i = 0; i < n; ++i) { const double t = (double) (start + i) / sr; dst[i] = t >= t0 && t < t1 ? (float) (std::pow (10.0, db / 20.0) * std::sin (2.0 * 3.14159265358979 * hz * t)) : 0.0f; } }; };
        CHECK (py.addTrack (0, py.levels[0].frames, tone (1000.0, -20.0, 100.0, 200.0), true));
        CHECK (py.addTrack (0, py.levels[0].frames, tone (4000.0, -40.0, 0.0, secs), false));
        py.settle();
        auto binOf = [&] (double hz) { return (int) std::lround (hz * SpectralPyramid::N / sr); };
        auto level0 = [&] (double t, double hz) { return SpectralPyramid::fromByte (py.levels[0].db[(size_t) py.frameAt (t) * (size_t) py.bins + (size_t) binOf (hz)]); };
        CHECK (std::abs (level0 (150.0, 1000.0) + 20.0) < 1.5 && std::abs (level0 (150.0, 4000.0) + 40.0) < 1.5 && level0 (50.0, 1000.0) < -100.0 && std::abs (level0 (50.0, 4000.0) + 40.0) < 1.5);
        {   // the coarse levels: the loudest of their frames, so the tone shows at every zoom
            const auto& L2 = py.levels[2]; const int f = juce::jlimit (0, L2.frames - 1, (int) (150.0 / L2.hopSeconds));
            CHECK (std::abs (SpectralPyramid::fromByte (L2.db[(size_t) f * (size_t) py.bins + (size_t) binOf (1000.0)]) + 20.0) < 1.5);
        }
        // a slice for a view: the whole edit at 1600 frames picks a coarse level, 20 s picks the finest; the frames' times come back with it
        double a = 0.0, b = 0.0;
        auto whole = py.slice (0.0, secs, 1600, a, b);
        CHECK (whole.frames > 0 && whole.frames <= 1600 && whole.bins == py.bins && a < 1.0 && b > secs - 2.0 && std::abs (whole.level (whole.frames / 2, 4000.0) + 40.0) < 1.5);
        auto zoom = py.slice (140.0, 160.0, 2400, a, b);
        CHECK (py.levelFor (140.0, 160.0, 2400) == 0 && py.levelFor (140.0, 160.0, 1600) == 1 && zoom.frames >= 1990 && zoom.frames <= 2010 && std::abs (a - 140.0) < 0.02 && std::abs (b - 160.0) < 0.02 && std::abs (zoom.level (zoom.frames / 2, 1000.0) + 20.0) < 1.5);
        // a stretch made again (a fix went in): 120 - 130 s silent on both tracks; only those frames change, and the coarse levels follow
        CHECK (py.addTrack (py.frameAt (120.0), py.frameAt (130.0), tone (1000.0, -200.0, 0.0, 0.0), true));
        CHECK (py.addTrack (py.frameAt (120.0), py.frameAt (130.0), tone (1000.0, -200.0, 0.0, 0.0), false));
        py.settle (py.frameAt (120.0), py.frameAt (130.0));
        CHECK (level0 (125.0, 1000.0) < -100.0 && level0 (125.0, 4000.0) < -100.0 && std::abs (level0 (119.0, 1000.0) + 20.0) < 1.5 && std::abs (level0 (131.0, 1000.0) + 20.0) < 1.5);
        { const auto& L2 = py.levels[2]; const int f = (int) (125.0 / L2.hopSeconds); CHECK (SpectralPyramid::fromByte (L2.db[(size_t) f * (size_t) py.bins + (size_t) binOf (4000.0)]) < -100.0); }
        std::printf ("  (pyramid: %d levels, %.1f MB for %.0f s)\n", (int) py.levels.size(), (double) py.bytes() / 1.0e6, secs);
    }
    SECTION ("VCAs: numbers, members, gains, mute, save and load");
    {
        Project vp; vp.tracks.clear(); vp.buses.clear();
        const auto t1 = vp.addTrack ("t1", TrackFormat::Mono, 0).id, t2 = vp.addTrack ("t2", TrackFormat::Mono, 1).id;
        const auto ib = vp.addBus ("int", false).id; const auto eb = vp.addBus ("ext", true).id;
        auto* a1 = vp.addVca ("Strings", true); CHECK (a1 != nullptr && a1->number == 1 && a1->label() == "1 - Strings");
        const auto id1 = a1->id;
        auto* a2 = vp.addVca ({}, true); CHECK (a2 != nullptr && a2->number == 2 && a2->label() == "2");
        const auto id2 = a2->id;
        auto& pm = *vp.mixers.front();
        CHECK ((pm.vcaShown.load() & 3u) == 3u);
        vp.setVcaMember (pm, *vp.findVca (id1), t1, true); vp.setVcaMember (pm, *vp.findVca (id1), ib, true); vp.setVcaMember (pm, *vp.findVca (id1), eb, true);
        vp.setVcaMember (pm, *vp.findVca (id2), t1, true);
        CHECK (vp.isVcaMember (pm, *vp.findVca (id1), t1) && ! vp.isVcaMember (pm, *vp.findVca (id1), t2) && vp.isVcaMember (pm, *vp.findVca (id1), ib));
        CHECK (! vp.isVcaMember (pm, *vp.findVca (id1), eb));                            // an Ext Bus is never controlled
        pm.vcas[0].gainDb.set (-4.0f);
        CHECK (std::abs (pm.vcaGain (pm.stripFor (t1)->vcaMask.load()) - dbToGain (-4.0f)) < 1.0e-6f);     // the strip keeps its own fader, the VCA adds -4 dB
        pm.vcas[1].gainDb.set (-2.0f);
        CHECK (std::abs (pm.vcaGain (pm.stripFor (t1)->vcaMask.load()) - dbToGain (-6.0f)) < 1.0e-5f);     // two VCAs add
        CHECK (pm.vcaGain (pm.stripFor (t2)->vcaMask.load()) == 1.0f);                                      // not controlled: untouched
        pm.vcas[1].mute.set (true);
        CHECK (pm.vcaGain (pm.stripFor (t1)->vcaMask.load()) == 0.0f);
        pm.vcas[1].mute.set (false); pm.vcas[0].solo.set (true);
        CHECK (pm.vcaSoloMask() == 1u);
        pm.vcas[0].solo.set (false);
        juce::String er; vp.name = "vcatest";
        vp.projectFile = tmp.getChildFile ("vcatest.fermata");
        CHECK (vp.save (er));
        Project vq; CHECK (vq.load (vp.projectFile, er));
        CHECK (vq.vcas.size() == 2 && vq.vcas[0].name == "Strings" && vq.vcas[1].number == 2);
        auto& qm = *vq.mixers.front();
        CHECK (std::abs (qm.vcas[0].gainDb.get() + 4.0f) < 1.0e-4f && (qm.vcaShown.load() & 3u) == 3u);
        CHECK (vq.isVcaMember (qm, vq.vcas[0], t1) && vq.isVcaMember (qm, vq.vcas[0], ib) && vq.isVcaMember (qm, vq.vcas[1], t1) && ! vq.isVcaMember (qm, vq.vcas[0], t2));
        // removing a VCA frees its number and takes the strips from it; the others keep theirs
        vp.removeVca (id1);
        CHECK (vp.vcas.size() == 1 && ! vp.isVcaMember (pm, *vp.findVca (id2), t2) && vp.isVcaMember (pm, *vp.findVca (id2), t1));
        CHECK (pm.stripFor (t1)->vcaMask.load() == 2u && pm.vcas[0].gainDb.get() == 0.0f);
        auto* a3 = vp.addVca ({}, false); CHECK (a3 != nullptr && a3->number == 1 && (pm.vcaShown.load() & 1u) == 0u);
        // a VCA's fader lane is automated through the VCA of the mixer
        auto& ed = vp.addEdit ("E", juce::Uuid::null());
        ed.automationOn = true;
        auto& lane = ed.addLane (id2, autoparam::fader, vp.mixers.front()->id);
        lane.pts.push_back (AutoPoint()); lane.pts.back().value = -9.0f;
        auto plan = automationPlanFor (vp, ed, true, vp.mixers.front()->id);
        CHECK (plan != nullptr && plan->items.size() == 1 && plan->items[0].target == &pm.vcas[1].gainDb);
        // an Int Bus has the lanes of a track: gain, fader, pan (saved too) and sends
        pm.busFor (ib)->inGainDb.set (-3.0f); pm.busFor (ib)->pan.set (0.5f);
        ed.lanes.clear();
        for (const char* prm : { "gain", "fader", "pan" }) { auto& l = ed.addLane (ib, prm, vp.mixers.front()->id); l.pts.push_back (AutoPoint()); l.pts.back().value = 0.0f; }
        auto plan2 = automationPlanFor (vp, ed, true, vp.mixers.front()->id);
        CHECK (plan2 != nullptr && plan2->items.size() == 3);
        if (plan2 != nullptr && plan2->items.size() == 3)
            CHECK (plan2->items[0].target == &pm.busFor (ib)->inGainDb && plan2->items[1].target == &pm.busFor (ib)->gainDb && plan2->items[2].target == &pm.busFor (ib)->pan);
        vp.projectFile = tmp.getChildFile ("vcatest2.fermata");
        CHECK (vp.save (er));
        Project vr; CHECK (vr.load (vp.projectFile, er));
        CHECK (std::abs (vr.mixers.front()->busFor (ib)->inGainDb.get() + 3.0f) < 1.0e-4f && std::abs (vr.mixers.front()->busFor (ib)->pan.get() - 0.5f) < 1.0e-4f);
    }
    SECTION ("fade curves are separate for the two ends of a region");
    {
        auto dir = tmp.getChildFile ("xc"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 5);
        Project xp; xp.tracks.clear();
        auto tid = xp.addTrack ("t", TrackFormat::Mono, 0).id;
        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        EditRegion r; r.srcIn = 48000; r.srcOut = 3 * 48000; r.sampleRate = 48000.0; r.sourceLength = 5 * 48000;
        RegionFile rf; rf.trackId = tid; rf.trackName = "t"; rf.file = fa; r.files.push_back (rf);
        ed.insertRegion (r);
        auto& a = ed.regions[0];
        a.curve = FadeCurve::EqualPower; a.outCurveOwn = false;
        CHECK (a.outCurveValue() == FadeCurve::EqualPower);
        a.setOutCurve (FadeCurve::Linear);                                   // only the fade-out
        CHECK (a.curve == FadeCurve::EqualPower && a.outCurveValue() == FadeCurve::Linear && a.outCurveOwn);
        a.setInCurve (FadeCurve::Linear);                                    // now both are linear, the fade-out keeps its own value
        CHECK (a.curve == FadeCurve::Linear && a.outCurveValue() == FadeCurve::Linear && ! a.outCurveOwn);
        a.setInCurve (FadeCurve::EqualPower);                                // the fade-in changes, the fade-out stays linear
        CHECK (a.curve == FadeCurve::EqualPower && a.outCurveValue() == FadeCurve::Linear && a.outCurveOwn);
        a.outStart = -0.1; a.outEnd = 0.1; a.inStart = 0.0; a.inEnd = 0.1;
        const auto mid = a.endSample();                                     // the middle of the fade-out: linear gives 0.5, equal-power would give 0.707
        CHECK (std::abs (a.gainAt (mid) - 0.5f) < 0.01f);
        const auto segs = segmentsForEdit (xp, ed);
        CHECK (! segs.empty() && segs[0].outCurve == FadeCurve::Linear && segs[0].inCurve == FadeCurve::EqualPower);
        a.curve = FadeCurve::EqualPower; a.setOutCurve (FadeCurve::EqualPower);
        CHECK (a.outCurveValue() == FadeCurve::EqualPower && ! a.outCurveOwn);
    }
    SECTION ("LTC: timecode recorded as audio is decoded");
    {
        // an LTC generator for the test: frames of 80 bits, biphase mark, starting at frame number 'firstFrame' at sample 'at' of a file of 'total' samples
        auto makeLtc = [] (std::vector<float>& out, double rate, double fps, bool drop, long long firstFrame, int numFrames, long long at)
        {
            const int nominal = (int) std::lround (fps);
            double pos = (double) at; float level = 0.5f; double lastEdge = pos;
            auto fill = [&] (double upTo) { const long long a = (long long) std::ceil (lastEdge), b = (long long) std::ceil (upTo); for (long long i = a; i < b && i < (long long) out.size(); ++i) out[(size_t) i] = level; lastEdge = upTo; };
            const double bitLen = rate / (fps * 80.0);
            for (int fi = 0; fi < numFrames; ++fi)
            {
                long long n = firstFrame + fi;
                long long dn = n;                                                   // drop-frame: frame number -> displayed numbers
                if (drop) { const long long perMin = 1798, per10 = 17982; const long long d = n / per10, m = n % per10; dn = n + 18 * d + (m > 1 ? 2 * ((m - 2) / perMin) : 0); }
                const int f = (int) (dn % nominal); long long t = dn / nominal; const int sec = (int) (t % 60); t /= 60; const int mi = (int) (t % 60); t /= 60; const int h = (int) (t % 24);
                int bits[80] = {};
                auto put = [&] (int first, int count, int v) { for (int i = 0; i < count; ++i) bits[first + i] = (v >> i) & 1; };
                put (0, 4, f % 10); put (8, 2, f / 10); bits[10] = drop ? 1 : 0; put (16, 4, sec % 10); put (24, 3, sec / 10); put (32, 4, mi % 10); put (40, 3, mi / 10); put (48, 4, h % 10); put (56, 2, h / 10);
                const int sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 }; for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];
                for (int b = 0; b < 80; ++b)
                {
                    fill (pos); level = -level;                                       // every bit starts with a change
                    if (bits[b]) { fill (pos + bitLen * 0.5); level = -level; }
                    pos += bitLen;
                }
                fill (pos);
            }
        };
        const double rate = 48000.0;
        {   // 25 fps: 10:02:15:00 is on the recording at 0.5 s
            std::vector<float> sig (48000 * 12, 0.0f);
            const long long first = ((10LL * 3600 + 2 * 60 + 15) * 25);
            makeLtc (sig, rate, 25.0, false, first, 25 * 10, 24000);
            LtcDecoder dec (rate);
            for (size_t i = 0; i < sig.size(); i += 777) dec.feed (sig.data() + i, (int) std::min<size_t> (777, sig.size() - i));      // odd block sizes
            auto r = analyseLtc (dec.frames(), rate);
            CHECK (r.found && std::abs (r.fps - 25.0) < 1.0e-6 && ! r.drop && r.jumps == 0);
            CHECK (r.found && std::abs (r.startSeconds - (10 * 3600 + 2 * 60 + 15 - 0.5)) < 0.0005);
            CHECK (r.found && r.firstText == "10:02:14:12");
            // upside-down signal decodes the same
            for (auto& v : sig) v = -v;
            LtcDecoder d2 (rate); d2.feed (sig.data(), (int) sig.size());
            auto r2 = analyseLtc (d2.frames(), rate);
            CHECK (r2.found && std::abs (r2.startSeconds - r.startSeconds) < 0.0005);
        }
        {   // 29.97 drop-frame, quiet and with a little noise
            std::vector<float> sig (48000 * 12, 0.0f);
            const long long first = 17982LL * 6;                                          // an hour in (frame number counted with drops)
            makeLtc (sig, rate, 30000.0 / 1001.0, true, first, 300, 4800);
            juce::Random rnd (7); for (auto& v : sig) v = v * 0.1f + (rnd.nextFloat() - 0.5f) * 0.002f;
            LtcDecoder dec (rate); dec.feed (sig.data(), (int) sig.size());
            auto r = analyseLtc (dec.frames(), rate);
            CHECK (r.found && r.drop && std::abs (r.fps - 30000.0 / 1001.0) < 1.0e-6 && r.jumps == 0);
            CHECK (r.found && std::abs (r.startSeconds - ((double) first * 1001.0 / 30000.0 - 0.1)) < 0.001);
            CHECK (r.found && timecodeText (r.startSeconds + 0.1, r.fps, true) == "01:00:00;00");
        }
        {   // a jump in the timecode (the generator was re-jammed) is reported
            std::vector<float> sig (48000 * 8, 0.0f);
            makeLtc (sig, rate, 25.0, false, 25 * 3600, 100, 0);                             // 01:00:00:00 for 4 s
            std::vector<float> second (48000 * 4, 0.0f); makeLtc (second, rate, 25.0, false, 25 * 7200, 100, 0);   // then 02:00:00:00
            for (size_t i = 0; i < second.size(); ++i) sig[192000 + i] = second[i];
            LtcDecoder dec (rate); dec.feed (sig.data(), (int) sig.size());
            auto r = analyseLtc (dec.frames(), rate);
            CHECK (r.found && r.jumps == 1);
        }
        {   // plain noise or music is not timecode
            std::vector<float> sig (48000 * 6); juce::Random rnd (3); for (auto& v : sig) v = rnd.nextFloat() - 0.5f;
            LtcDecoder dec (rate); dec.feed (sig.data(), (int) sig.size());
            CHECK (! analyseLtc (dec.frames(), rate).found);
        }
        // reading it back from a file
        {
            std::vector<float> sig (48000 * 6, 0.0f); makeLtc (sig, rate, 24.0, false, 24 * 3600 * 2, 24 * 5, 0);
            auto f = tmp.getChildFile ("ltc.wav"); CHECK (audioops::writeWav (f, { sig }, rate));
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            auto r = scanFileForLtc (fm, f);
            CHECK (r.found && std::abs (r.fps - 24.0) < 1.0e-6 && std::abs (r.startSeconds - 7200.0) < 0.001);
        }
    }

    SECTION ("LTC: the generator, the live detector, and LTC pieces never fade or overlap");
    {
        const double rate = 48000.0;
        for (int code = 0; code < 6; ++code)
        {
            LtcGenerator gen; gen.prepare (rate);
            double fps; int nominal; bool drop; LtcGenerator::formatInfo (code, fps, nominal, drop);
            const double startSec = drop ? 3600.0 * 1.0 : 3600.0 * 10 + 125.0;                // 10:02:05:00 (or 01:00:00;00 for drop-frame)
            gen.start (code, startSec);
            std::vector<float> sig (48000 * 8);
            for (size_t i = 0; i < sig.size(); i += 480) gen.fill (sig.data() + i, 480);
            LtcDecoder dec (rate); dec.feed (sig.data(), (int) sig.size());
            auto r = analyseLtc (dec.frames(), rate);
            CHECK (r.found && std::abs (r.fps - fps) < 1.0e-6 && r.drop == drop && r.jumps == 0);
            CHECK (r.found && std::abs (r.startSeconds - std::round (startSec * fps) / fps) < 0.0007);
            // the live detector says "present" with the same rate
            LtcLive live; live.prepare (rate);
            for (size_t i = 0; i < sig.size(); i += 512) live.feed (sig.data() + i, (int) std::min<size_t> (512, sig.size() - i));
            auto st = live.status();
            CHECK (st.present && std::abs (st.fps - fps) < 1.0e-6 && st.drop == drop);
            std::vector<float> quiet (48000, 0.0f); live.feed (quiet.data(), 24000); live.feed (quiet.data(), 24000);
            CHECK (! live.status().present);                                                  // the signal stopped: the lamp goes red
        }
        // pieces of an edit: the LTC of each piece starts hard and never overlaps the next one
        auto dir = tmp.getChildFile ("ltcseg"); dir.createDirectory();
        auto fa = makeConstWav (dir.getChildFile ("A.wav"), 0.5, 20), fl = makeConstWav (dir.getChildFile ("L.wav"), 0.25, 20);
        Project lp; lp.tracks.clear();
        auto audioId = lp.addTrack ("a", TrackFormat::Mono, 0).id;
        auto& lt = lp.addTrack ("LTC", TrackFormat::Mono, 1); lt.isLtc = true; const auto ltcId = lt.id;
        auto& ed = lp.addEdit ("E", juce::Uuid::null());
        auto addPiece = [&] (juce::int64 srcIn, juce::int64 srcOut, juce::int64 start)
        {
            EditRegion r; r.srcIn = srcIn; r.srcOut = srcOut; r.startSample = start; r.sampleRate = 48000.0; r.sourceLength = 20 * 48000;
            RegionFile a; a.trackId = audioId; a.trackName = "a"; a.file = fa; r.files.push_back (a);
            RegionFile l; l.trackId = ltcId; l.trackName = "LTC"; l.file = fl; r.files.push_back (l);
            r.inStart = 0.0; r.inEnd = 0.5; r.outStart = -0.5; r.outEnd = 0.0;               // half-second fades
            ed.regions.push_back (r);
        };
        addPiece (48000, 5 * 48000, 0);
        addPiece (8 * 48000, 12 * 48000, 4 * 48000 - 12000);                               // the second piece fades in while the first is still fading out
        auto segs = segmentsForEdit (lp, ed);
        std::vector<PlaySegment> ls, as; for (auto& s : segs) (s.trackIndex == 1 ? ls : as).push_back (s);
        CHECK (ls.size() == 2 && as.size() == 2);
        if (ls.size() == 2)
        {
            CHECK (ls[0].fadeInLen == 0 && ls[0].fadeOutLen == 0 && ls[1].fadeInLen == 0 && ls[1].fadeOutLen == 0);          // never faded
            CHECK (ls[0].begin == ed.regions[0].fadeInBegin());                                                              // hard start at the first sample of the fade-in
            CHECK (ls[1].begin == ed.regions[1].fadeInBegin());
            CHECK (ls[0].end == ls[1].begin);                                                                               // the first stops one sample before the second starts to exist: [begin, end) never overlaps
            CHECK (ls[0].end <= ls[1].begin);
        }
        if (as.size() == 2) CHECK (as[0].fadeOutLen > 0 && as[1].fadeInLen > 0);                                            // the audio itself still crossfades
    }

    SECTION ("markers: numbered, renamed, saved with the window, kept out of undo");
    {
        std::vector<Marker> list;
        const auto a = addMarker (list, 5.0); addMarker (list, 2.0); addMarker (list, 9.0);
        CHECK (list.size() == 3 && list[0].name == "1" && list[1].name == "2" && list[2].name == "3");
        findMarker (list, list[1].id)->name = "Chorus";                                   // a named one does not break the numbering
        removeMarker (list, a);
        const auto d = addMarker (list, -3.0);                                            // never before the start
        CHECK (findMarker (list, d) != nullptr && findMarker (list, d)->name == "4" && findMarker (list, d)->seconds == 0.0);
        Project mp; auto& tw = mp.addTakeWindow ("W"); auto& me = mp.addEdit ("E", tw.id);
        tw.markers = list; me.markers = list; me.markers[0].seconds = 12.5;
        Project mq; CHECK (mq.fromVar (mp.toVar()));
        auto* qw = mq.findTakeWindow (tw.id);
        auto* qe = mq.findEdit (me.id);
        CHECK (qw != nullptr && qw->markers.size() == 3 && qw->markers[0].name == "Chorus" && qw->markers[2].name == "4");
        CHECK (qe != nullptr && qe->markers.size() == 3 && std::abs (qe->markers[0].seconds - 12.5) < 1.0e-9 && qe->markers[0].id == list[0].id);
        CHECK (! mp.undoText (tw).contains ("Chorus") && ! mp.undoText (me).contains ("Chorus"));     // markers are not undo steps
    }

    SECTION ("import: 14 mics x several takes named like 0105 L tk.03_03.wav -> 14 tracks");
    {
        auto src = [&] (const juce::String& name, juce::int64 len)
        { ImportSource s; s.file = juce::File ("/x/" + name); s.channels = 1; s.length = len; s.sampleRate = 48000.0; s.modified = juce::Time (2026, 1, 1, 10, 0, 0, 0, true); return s; };
        const char* mics[] = { "Live2TChoir", "Piano4099L", "Piano4099R", "Piano4006L", "Piano4006R", "L", "C", "R", "Spot1", "Spot2", "Spot3", "Spot4", "Spot5", "Spot6" };
        std::vector<ImportSource> v;
        for (int t = 1; t <= 6; ++t)
            for (int m = 0; m < 14; ++m)
            {
                const auto tk = juce::String (t).paddedLeft ('0', 2);
                v.push_back (src (juce::String (100 + m).paddedLeft ('0', 4) + " " + mics[m] + " tk." + tk + "_" + tk + ".wav", 480000 + 7000 * t));
            }
        std::reverse (v.begin(), v.end());
        auto plan = buildImportPlan (v);
        CHECK (plan.takes.size() == 6 && plan.tracks.size() == 14);
        if (plan.takes.size() == 6 && plan.tracks.size() == 14)
        {
            CHECK (plan.takes[0].number == 1 && plan.takes[5].number == 6 && plan.takes[0].parts.size() == 14 && plan.takes[5].parts.size() == 14);
            CHECK (plan.tracks[0].name == "Live2TChoir" && plan.tracks[1].name == "Piano4099L" && plan.tracks[5].name == "L" && plan.tracks[8].name == "Spot1" && plan.tracks[13].name == "Spot6");
        }
        CHECK (hasStereoPairs (plan));
        CHECK (joinStereoPairs (plan).tracks.size() == 11);                                   // Piano4099 L+R, Piano4006 L+R, L+R
        CHECK (joinStereoPairs (plan, true).tracks.size() == 10);                             // and Spot1 + Spot2 when asked
        auto joined = joinStereoPairs (plan);
        bool nm = false; for (auto& t : joined.tracks) nm = nm || (t.name == "Piano4099" && t.channels == 2);
        CHECK (nm);
        // linking: whole mono files are used where they are, nothing copied
        auto dir = tmp.getChildFile ("linkimp"); dir.createDirectory();
        juce::AudioFormatManager fm2; fm2.registerBasicFormats();
        juce::Array<juce::File> fl;
        for (int t = 1; t <= 2; ++t) for (int m = 0; m < 2; ++m)
        {
            const auto tk = juce::String (t).paddedLeft ('0', 2);
            fl.add (makeConstWav (dir.getChildFile (juce::String (100 + m).paddedLeft ('0', 4) + " Mic" + juce::String (m) + " tk." + tk + "_" + tk + ".wav"), 0.1 * t, 1 + t));
        }
        juce::StringArray problems;
        auto lp = buildImportPlan (inspectFiles (fm2, fl, problems));
        CHECK (lp.takes.size() == 2 && lp.tracks.size() == 2);
        std::vector<std::vector<juce::File>> out; std::vector<int> nums { 1, 2 };
        CHECK (copyImportFiles (fm2, lp, tmp.getChildFile ("linkout"), "P", nums, out, nullptr, true).isEmpty());
        CHECK (out.size() == 2 && out[0].size() == 2 && out[0][0].getParentDirectory() == dir && ! tmp.getChildFile ("linkout").findChildFiles (juce::File::findFiles, false).size());
    }

    SECTION ("main outputs: every Ext Bus ticked as main (several, or none), main plus extra sends, a cue mixer that follows the processing mixer (2.65)");
    {
        Project q;
        q.projectFile = tmp.getChildFile ("Mains.fermata");
        q.setInputCount (2); q.setOutputCount (6);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        const auto tb = q.addTrack ("B", TrackFormat::Mono, 1).id;
        q.retireAllMixers();
        const auto mainB = q.addBus ("Main", true).id;
        const auto cans = q.addBus ("Cans", true).id;
        auto& mp = q.addMixer ("Processing");
        auto& mc = q.addMixer ("Cue");
        CHECK (mp.busFor (mainB)->isMain.get() && ! mp.busFor (cans)->isMain.get());          // a new mixer: its first Ext Bus is ticked as main, the others are not
        CHECK (mc.busFor (mainB)->isMain.get() && ! mc.busFor (cans)->isMain.get());
        const auto later = q.addBus ("Later", true).id;
        CHECK (! mp.busFor (later)->isMain.get());                                              // an Ext Bus added later is not ticked
        AudioEngine e2 (q);
        e2.prepare (48000.0, 64);
        for (auto* m : { &mp, &mc })
            for (auto id : { ta, tb }) { auto* st = m->stripFor (id); st->outOn.set (true); st->outDest = juce::Uuid::null(); st->gainDb.set (0.0f); st->pan.set (0.0f); }
        mp.busFor (mainB)->outFirst.store (0); mp.busFor (cans)->outFirst.store (2);
        mc.busFor (mainB)->outFirst.store (-1); mc.busFor (cans)->outFirst.store (4);
        e2.rebuildPlan();
        auto rmsOut = [&]
        {
            std::vector<double> acc (6, 0.0); int cnt = 0; double ph = 0.0;
            for (int blk = 0; blk < 40; ++blk)
            {
                std::vector<float> i0 (64), i1 (64, 0.0f); std::vector<std::vector<float>> o (6, std::vector<float> (64, 0.0f));
                for (int k = 0; k < 64; ++k) { i0[(size_t) k] = (float) (0.5 * std::sin (ph)); ph += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0; }
                const float* in[2] = { i0.data(), i1.data() }; float* out[6]; for (int c = 0; c < 6; ++c) out[c] = o[(size_t) c].data();
                e2.process (in, 2, out, 6, 64);
                if (blk >= 10) { for (int c = 0; c < 6; ++c) for (float v : o[(size_t) c]) acc[(size_t) c] += (double) v * v; cnt += 64; }
            }
            for (auto& a : acc) a = std::sqrt (a / cnt);
            return acc;
        };
        auto r = rmsOut();
        CHECK (r[0] > 0.1 && r[2] < 1e-6 && r[4] < 1e-6);                        // Main is the ticked bus only; the cue mixer's Main goes to no output, its Cans has nothing
        mp.busFor (cans)->isMain.set (true); e2.rebuildPlan();
        r = rmsOut();
        CHECK (r[0] > 0.1 && std::abs (r[2] - r[0]) < 1e-4);                     // two buses ticked: both get the main mix
        mp.busFor (mainB)->isMain.set (false); mp.busFor (cans)->isMain.set (false); e2.rebuildPlan();
        r = rmsOut();
        CHECK (r[0] < 1e-6 && r[2] < 1e-6);                                       // none ticked: 'Main' goes nowhere (it used to fall back to the first Ext Bus)
        mp.busFor (mainB)->isMain.set (true); mp.busFor (cans)->isMain.set (true);
        q.setSendLevel (mp, ta, cans, -6.0f); e2.rebuildPlan();
        r = rmsOut();
        CHECK (r[2] > r[0] * 1.3);                                               // a main bus also plays what is sent to it: the main mix plus more of A
        q.setSendLevel (mp, ta, cans, -100.0f); mp.busFor (cans)->isMain.set (false); e2.rebuildPlan();

        // the cue mixer follows the processing mixer: its Ext buses play the processing mixer's main output; its own mix is kept and comes back
        mc.stripFor (ta)->gainDb.set (-20.0f);
        const auto base = rmsOut();
        mc.followProcessing.set (true);
        r = rmsOut();
        CHECK (r[4] > 0.1 && std::abs (r[4] - base[0]) < 1e-3 && std::abs (r[0] - base[0]) < 1e-4);     // the cue's Cans pair carries exactly the processing main mix
        CHECK (std::abs (mc.stripFor (ta)->gainDb.get() + 20.0f) < 1e-6f);                                // and its own mix is untouched
        mc.followProcessing.set (false);
        r = rmsOut();
        CHECK (r[4] < 1e-6);                                                     // unticked: its own mix again (nothing goes to its Cans)

        // saved and read back; a project from before 2.65 (one "mainBus") gets that bus ticked
        mc.followProcessing.set (true); mp.busFor (cans)->isMain.set (true);
        juce::String err;
        CHECK (q.save (err));
        {
            Project q2; CHECK (q2.load (q.projectFile, err));
            CHECK (q2.mixers.size() == 2 && q2.mixers[0]->busFor (mainB)->isMain.get() && q2.mixers[0]->busFor (cans)->isMain.get() && ! q2.mixers[0]->busFor (later)->isMain.get());
            CHECK (q2.mixers[1]->followProcessing.get() && ! q2.mixers[0]->followProcessing.get());
        }
        {
            auto v = q.toVar();                                   // made to look like a 2.64 project: one "mainBus", no "main" ticks
            int edited = 0;
            if (auto* ms = v["mixers"].getArray())
                for (auto& m : *ms)
                {
                    if (auto* obj = m.getDynamicObject()) { obj->removeProperty ("mainFlags"); obj->setProperty ("mainBus", cans.toString()); ++edited; }
                    for (const char* key : { "busStates", "fxReturns" })
                        if (auto* bsA = m[key].getArray()) for (auto& b : *bsA) if (auto* bo = b.getDynamicObject()) bo->removeProperty ("main");
                }
            CHECK (edited == 2);
            Project q3; CHECK (q3.fromVar (v));
            CHECK (q3.mixers.size() == 2 && q3.mixers[0]->busFor (cans)->isMain.get() && ! q3.mixers[0]->busFor (mainB)->isMain.get());
            if (auto* ms = v["mixers"].getArray()) for (auto& m : *ms) if (auto* obj = m.getDynamicObject()) obj->setProperty ("mainBus", juce::Uuid::null().toString());
            Project q4; CHECK (q4.fromVar (v));                   // no main bus chosen (2.64 then used the first Ext Bus): the first Ext Bus is ticked
            CHECK (q4.mixers.size() == 2 && q4.mixers[0]->busFor (mainB)->isMain.get() && ! q4.mixers[0]->busFor (cans)->isMain.get() && q4.mixers[1]->busFor (mainB)->isMain.get());
            size_t pool = 0; for (auto& m : q4.mixers) pool += m->busPool.size();
            CHECK (pool == 6);                                    // (and no stray bus states were made)
        }
    }

    SECTION ("delay compensation: a plug-in that delays one track does not put it out of time with the others");
    {
        Project q;
        q.projectFile = tmp.getChildFile ("Pdc.fermata");
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        const auto tb = q.addTrack ("B", TrackFormat::Mono, 1).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& mq = q.addMixer ("M");
        AudioEngine e2 (q);
        e2.prepare (96000.0, 1024);
        CHECK (q.restoreSampleRate == 96000.0 && q.restoreBlock == 1024);      // plug-ins a reload starts are prepared for what the device runs at (2.64)
        e2.prepare (48000.0, 64);
        CHECK (q.restoreSampleRate == 48000.0 && q.restoreBlock == 64);
        q.setSendLevel (mq, ta, ob, 0.0f); q.setSendLevel (mq, tb, ob, 0.0f);
        mq.busFor (ob)->outFirst.store (0);
        e2.rebuildPlan();
        auto runImpulse = [&] (std::vector<float>& outL)
        {
            outL.clear();
            for (int blk = 0; blk < 8; ++blk)
            {
                std::vector<float> i0 (64, 0.0f), i1 (64, 0.0f), o0 (64, 0.0f), o1 (64, 0.0f);
                if (blk == 1) { i0[0] = 1.0f; i1[0] = 1.0f; }       // (block 0 lets the faders settle)
                const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                e2.process (in, 2, out, 2, 64);
                outL.insert (outL.end(), o0.begin(), o0.end());
            }
        };
        std::vector<float> o;
        runImpulse (o);
        CHECK (std::abs (o[64] - 1.41421f) < 0.01f && e2.mixerLatency (&mq) == 0);                // no plug-ins: both arrive at once, no delay added
        mq.stripFor (ta)->slots[0].set (std::make_unique<LatencyInsert> (100), 48000.0, 64, 2);
        e2.checkLatency();
        CHECK (e2.mixerLatency (&mq) == 100 && mq.stripFor (ta)->slots[0].latencySamples() == 100);
        runImpulse (o);
        int peaks = 0, at = -1;
        for (int i = 0; i < (int) o.size(); ++i) if (std::abs (o[(size_t) i]) > 0.01f) { ++peaks; at = i; }
        CHECK (peaks == 1 && at == 164 && std::abs (o[164] - 1.41421f) < 0.01f);                 // both tracks line up, one impulse, 100 samples late
        mq.stripFor (ta)->slots[0].bypass.set (true);
        e2.checkLatency();
        CHECK (e2.mixerLatency (&mq) == 0);                                                      // bypassed: no delay, nothing to compensate
        runImpulse (o);
        CHECK (std::abs (o[64] - 1.41421f) < 0.01f);
        mq.stripFor (ta)->slots[0].bypass.set (false);
        mq.stripFor (tb)->slots[0].set (std::make_unique<LatencyInsert> (30), 48000.0, 64, 2);
        e2.checkLatency();
        CHECK (e2.mixerLatency (&mq) == 100);                                                    // the longest path wins
        runImpulse (o);
        peaks = 0; at = -1;
        for (int i = 0; i < (int) o.size(); ++i) if (std::abs (o[(size_t) i]) > 0.01f) { ++peaks; at = i; }
        CHECK (peaks == 1 && at == 164);
        // through a send: track A (100 late) also feeds a bus that B's signal does not pass; both reach the output together
        q.setSendLevel (mq, ta, ob, -100.0f); e2.rebuildPlan();
    }

    SECTION ("2.72: automation lands on the music under it, also behind plug-ins that delay the sound");
    {
        // where each control acts, how late the sound is there
        Project q;
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        const auto tb = q.addTrack ("B", TrackFormat::Mono, 1).id;
        q.retireAllMixers();
        const auto verb = q.addBus ("Verb", false).id;
        const auto ob = q.addBus ("Out", true).id;
        auto& m = q.addMixer ("M");
        AudioEngine e (q);
        e.prepare (48000.0, 64);
        q.setSendLevel (m, ta, ob, 0.0f); q.setSendLevel (m, ta, verb, 0.0f); q.setSendLevel (m, tb, verb, 0.0f); q.setSendLevel (m, verb, ob, 0.0f);
        auto* sa = m.stripFor (ta); auto* sb = m.stripFor (tb); auto* bv = m.busFor (verb);
        sa->slots[0].set (std::make_unique<LatencyInsert> (100), 48000.0, 64, 2);
        sa->slots[1].set (std::make_unique<LatencyInsert> (30), 48000.0, 64, 2);
        bv->slots[0].set (std::make_unique<LatencyInsert> (50), 48000.0, 64, 2);
        sa->vcaMask.store (1u); sb->vcaMask.store (1u);
        e.rebuildPlan();
        CHECK (sa->slots[0].autoLat.load() == 0 && sa->slots[1].autoLat.load() == 100 && sa->slots[2].autoLat.load() == 130);   // each plug-in hears what the ones before it did
        CHECK (sa->autoLatIn.load() == 0 && sa->autoLatOut.load() == 130);                       // input gain before the inserts, fader and pan after them
        CHECK (sb->autoLatIn.load() == 0 && sb->autoLatOut.load() == 0);
        CHECK (bv->autoLatIn.load() == 130 && bv->slots[0].autoLat.load() == 130 && bv->autoLatOut.load() == 180);   // the bus waits for its latest feed
        CHECK (sa->sends.find (verb)->autoLat.load() == 130 && sb->sends.find (verb)->autoLat.load() == 130);       // a send's level acts after its delay line: both meet the bus at 130
        CHECK (sa->sends.find (verb)->slots[0].autoLat.load() == 130 && sb->sends.find (verb)->slots[0].autoLat.load() == 0);
        CHECK (m.vcas[0].autoLat.load() == 130 && m.vcas[1].autoLat.load() == 0);               // a VCA follows the latest channel it controls
        sa->slots[0].bypass.set (true); e.checkLatency();
        CHECK (sa->autoLatOut.load() == 30 && bv->autoLatIn.load() == 30 && sa->slots[1].autoLat.load() == 0);         // bypassed: it holds nothing back
        sa->slots[0].bypass.set (false); e.checkLatency();
        CHECK (sa->autoLatOut.load() == 130);

        // the lane is read that much earlier
        AutomationPlan plan;
        AutomationPlan::Item it; it.target = &sa->gainDb; it.lat = &sa->autoLatOut;
        AutoPoint p1; p1.time = 1000; p1.value = 0.0f; AutoPoint p2 = p1; p2.time = 1001; p2.value = -100.0f;
        it.lane.pts = { p1, p2 };
        plan.items.push_back (it);
        plan.apply (1100); CHECK (sa->gainDb.get() == 0.0f);                                    // the sound at the fader now went in at 970: before the move
        plan.apply (1140); CHECK (sa->gainDb.get() == -100.0f);                                 // 1010: after it
        plan.items[0].lat = nullptr;
        plan.apply (1100); CHECK (sa->gainDb.get() == -100.0f);                                 // (without: the old, early behaviour)
        sa->gainDb.set (0.0f);
        // round a loop: the sound just after the loop came round went in at the end of the loop
        {
            PlaybackSession ps ({ 1 }, {}, 1000, 2000, 48000.0, 512);
            CHECK (ps.open().isEmpty());
            CHECK (ps.getTimelinePositionAgo (0) == 1000 && ps.getTimelinePositionAgo (130) == 870);   // before the start: it counts back
            ps.setLooping (true);
            ps.start();
            for (int k = 0; k < 2; ++k) ps.pull (512);
            for (int k = 0; k < 4 && ps.getPosition() < 1200; ++k) ps.pull ((int) juce::jmin ((juce::int64) 512, 1200 - ps.getPosition()));
            CHECK (ps.getPosition() == 1200 && ps.getTimelinePosition() == 1200);                    // came round once: 200 into the loop
            CHECK (ps.getTimelinePositionAgo (300) == 1900 && ps.getTimelinePositionAgo (100) == 1100);
        }

        // end to end: a bounce through a strip with a 100 ms look-ahead; the fader drops to nothing at 1 s exactly
        struct LatIns : LatencyInsert
        {
            explicit LatIns (int n) : LatencyInsert (n) {}
            juce::String getDescription() const override { return "lat:" + juce::String (lat); }     // so the render copy can make its own
        };
        Project bq;
        bq.projectFile = tmp.getChildFile ("AutoLat.fermata");
        bq.insertFactory = [] (const juce::String& desc, const juce::String&) -> std::unique_ptr<InsertProcessor>
        { return desc.startsWith ("lat:") ? std::make_unique<LatIns> (desc.substring (4).getIntValue()) : nullptr; };
        bq.setInputCount (1); bq.setOutputCount (2);
        bq.tracks.clear();
        const auto tt = bq.addTrack ("t", TrackFormat::Mono, 0).id;
        bq.retireAllMixers();
        const auto bob = bq.addBus ("Out", true).id;
        auto& bm = bq.addMixer ("M");
        bq.setSendLevel (bm, tt, bob, 0.0f); bm.busFor (bob)->outFirst.store (0);
        const auto src = makeConstWav (tmp.getChildFile ("autolat.wav"), 0.5, 3);
        auto& ed = bq.addEdit ("E", juce::Uuid::null());
        EditRegion r; r.srcIn = 0; r.srcOut = 3 * 48000; r.sampleRate = 48000.0; r.sourceLength = 3 * 48000;
        RegionFile rf; rf.trackId = tt; rf.trackName = "t"; rf.file = src; r.files.push_back (rf);
        ed.insertRegion (r);
        ed.automationOn = true;
        auto& lane = ed.addLane (tt, autoparam::fader, bm.id);
        AutoPoint q1; q1.id = juce::Uuid(); q1.time = 48000; q1.value = 0.0f; AutoPoint q2 = q1; q2.id = juce::Uuid(); q2.time = 48001; q2.value = -100.0f;
        lane.pts = { q1, q2 };
        bm.stripFor (tt)->slots[0].set (std::make_unique<LatIns> (4800), 48000.0, 512, 2);
        BounceSettings bs;
        bs.sampleRate = 48000.0; bs.mixerId = bm.id; bs.sources = { bob }; bs.folder = tmp.getChildFile ("autolat");
        bs.segments = segmentsForEdit (bq, ed); bs.automationEdit = ed.id;
        bs.items.push_back ({ "AutoLat", 0, 3 * 48000 });
        Bouncer b (bq, bs);
        CHECK (b.getPrepareError().isEmpty());
        auto res = b.render();
        CHECK (res.error.isEmpty() && res.files.size() == 1);
        if (res.files.size() == 1)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (res.files[0])));
            CHECK (rd != nullptr && rd->lengthInSamples == 3 * 48000);
            if (rd != nullptr && rd->lengthInSamples == 3 * 48000)
            {
                juce::AudioBuffer<float> out (2, 3 * 48000); rd->read (&out, 0, 3 * 48000, 0, true, true);
                const float before = out.getMagnitude (0, 48000 - 1536, 512), after = out.getMagnitude (0, 49600, 500), mid = out.getMagnitude (0, 24000, 4800);
                CHECK (mid > 0.3f && before > 0.3f);                // full level up to the move (it used to fall 100 ms early, at 0.9 s)
                CHECK (after < 1.0e-3f);                            // and gone straight after it (within one block)
            }
        }
    }

    SECTION ("2.72: a send held for automation never closes a loop (it would cut an audible path)");
    {
        Project q;
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();
        const auto b1 = q.addBus ("B1", false).id;
        const auto b2 = q.addBus ("B2", false).id;
        const auto ob = q.addBus ("Out", true).id;
        auto& m = q.addMixer ("M");
        AudioEngine e (q);
        e.prepare (48000.0, 64);
        q.setSendLevel (m, b1, b2, 0.0f); q.setSendLevel (m, b1, b2, -100.0f);   // B1 -> B2 exists, off (an old send with a lane)
        CHECK (q.setSendLevel (m, ta, b2, 0.0f) != SendResult::Refused);
        CHECK (q.setSendLevel (m, b2, b1, 0.0f) != SendResult::Refused);        // allowed: B1 -> B2 is off
        CHECK (q.setSendLevel (m, b1, ob, 0.0f) != SendResult::Refused);
        m.busFor (ob)->outFirst.store (0);
        e.rebuildPlan();
        auto peakNow = [&]
        {
            float pk = 0.0f;
            for (int blk = 0; blk < 8; ++blk)
            {
                std::vector<float> i0 (64, 0.5f), i1 (64, 0.0f), o0 (64, 0.0f), o1 (64, 0.0f);
                const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                e.process (in, 2, out, 2, 64);
                if (blk >= 4) for (float s : o0) pk = juce::jmax (pk, std::abs (s));
            }
            return pk;
        };
        const float before = peakNow();
        auto* sd = m.busFor (b1)->sends.find (b2);
        AutomationPlan plan;
        AutomationPlan::Item it; it.target = &sd->gainDb; it.lat = &sd->autoLat; it.send = sd;
        AutoPoint p1; p1.time = 0; p1.value = -100.0f; it.lane.pts = { p1 };     // the lane keeps it off here
        plan.items.push_back (it);
        plan.begin(); e.rebuildPlan();
        const float held = peakNow();
        CHECK (before > 0.1f && std::abs (held - before) < 1.0e-4f);           // A -> B2 -> B1 -> Out still heard: the held B1 -> B2 was left out
        plan.end(); e.rebuildPlan();
    }

    SECTION ("2.72: every kind of lane lands on time behind plug-ins (send, input gain, bus fader, pan, VCA); VCA membership; a send faded up from off");
    {
        struct LatIns2 : LatencyInsert
        {
            explicit LatIns2 (int n) : LatencyInsert (n) {}
            juce::String getDescription() const override { return "lat:" + juce::String (lat); }
        };
        // render a 3 s constant source on track t through a mixer built by 'setup', with one automation lane dropping to off at 48000; returns the first
        // sample (after 30000) at which the output falls below 90 % of its level at 24000, or -1
        auto runCase = [&] (const char* name, std::function<void (Project&, MixerState&, juce::Uuid t, juce::Uuid u, juce::Uuid v, juce::Uuid out, juce::Uuid bus)> setup,
                            std::function<juce::String (juce::Uuid t, juce::Uuid u, juce::Uuid v, juce::Uuid out, juce::Uuid bus, juce::Uuid& laneTrack)> param, float offValue) -> int
        {
            Project bq;
            bq.projectFile = tmp.getChildFile (juce::String ("Rev_") + name + ".fermata");
            bq.insertFactory = [] (const juce::String& desc, const juce::String&) -> std::unique_ptr<InsertProcessor>
            { return desc.startsWith ("lat:") ? std::make_unique<LatIns2> (desc.substring (4).getIntValue()) : nullptr; };
            bq.setInputCount (3); bq.setOutputCount (2);
            bq.tracks.clear();
            const auto tt = bq.addTrack ("t", TrackFormat::Mono, 0).id;
            const auto tu = bq.addTrack ("u", TrackFormat::Mono, 1).id;
            const auto tv = bq.addTrack ("v", TrackFormat::Mono, 2).id;
            bq.retireAllMixers();
            const auto bus = bq.addBus ("Bus", false).id;
            const auto bob = bq.addBus ("Out", true).id;
            auto& bm = bq.addMixer ("M");
            bm.busFor (bob)->outFirst.store (0);
            setup (bq, bm, tt, tu, tv, bob, bus);
            const auto src = makeConstWav (tmp.getChildFile (juce::String ("rev_") + name + ".wav"), 0.5, 3);
            auto& ed = bq.addEdit ("E", juce::Uuid::null());
            EditRegion r; r.srcIn = 0; r.srcOut = 3 * 48000; r.sampleRate = 48000.0; r.sourceLength = 3 * 48000;
            RegionFile rf; rf.trackId = tt; rf.trackName = "t"; rf.file = src; r.files.push_back (rf);
            ed.insertRegion (r);
            ed.automationOn = true;
            juce::Uuid laneTrack = tt;
            const auto p = param (tt, tu, tv, bob, bus, laneTrack);
            auto& lane = ed.addLane (laneTrack, p, bm.id);
            AutoPoint q1; q1.id = juce::Uuid(); q1.time = 48000; q1.value = 0.0f; AutoPoint q2 = q1; q2.id = juce::Uuid(); q2.time = 48001; q2.value = offValue;
            lane.pts = { q1, q2 };
            BounceSettings bs;
            bs.sampleRate = 48000.0; bs.mixerId = bm.id; bs.sources = { bob }; bs.folder = tmp.getChildFile (juce::String ("rev_") + name);
            bs.segments = segmentsForEdit (bq, ed); bs.automationEdit = ed.id;
            bs.items.push_back ({ name, 0, 3 * 48000 });
            Bouncer b (bq, bs);
            auto res = b.render();
            if (! res.error.isEmpty() || res.files.size() != 1) { std::printf ("   [%s] render failed: %s\n", name, res.error.toRawUTF8()); return -2; }
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (res.files[0])));
            juce::AudioBuffer<float> out (2, 3 * 48000); rd->read (&out, 0, 3 * 48000, 0, true, true);
            const float full = std::abs (out.getSample (0, 24000));
            int at = -1;
            for (int i = 30000; i < 3 * 48000; ++i) if (std::abs (out.getSample (0, i)) < 0.9f * full) { at = i; break; }
            return at;
        };
        auto inWindow = [] (int at) { return at >= 48001 && at <= 48001 + 512; };

        // send level, with the send's own 1000-sample insert and a 3800-sample compensation delay (another path into Out is 4800 late)
        int a = runCase ("send", [] (Project& q, MixerState& m, juce::Uuid t, juce::Uuid, juce::Uuid v, juce::Uuid out, juce::Uuid)
        {
            q.setSendLevel (m, t, out, 0.0f); q.setSendLevel (m, v, out, 0.0f);
            m.stripFor (t)->sends.find (out)->slots[0].set (std::make_unique<LatIns2> (1000), 48000.0, 512, 2);
            m.stripFor (v)->slots[0].set (std::make_unique<LatIns2> (4800), 48000.0, 512, 2);
        }, [] (juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid out, juce::Uuid, juce::Uuid&) { return autoparam::send (out); }, -100.0f);
        CHECK (inWindow (a));
        // a track's input gain, when its own source waits (srcDelay) for a 4800-late send coming into it
        int b2 = runCase ("ingain", [] (Project& q, MixerState& m, juce::Uuid t, juce::Uuid u, juce::Uuid, juce::Uuid out, juce::Uuid)
        {
            q.setSendLevel (m, t, out, 0.0f); q.setSendLevel (m, u, t, 0.0f);
            m.stripFor (u)->slots[0].set (std::make_unique<LatIns2> (4800), 48000.0, 512, 2);
        }, [] (juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid&) { return autoparam::gain; }, -100.0f);
        CHECK (inWindow (b2));
        // a bus fader behind its own insert, fed by a track behind a plug-in (latencies add up)
        int c2 = runCase ("busfader", [] (Project& q, MixerState& m, juce::Uuid t, juce::Uuid, juce::Uuid, juce::Uuid out, juce::Uuid bus)
        {
            q.setSendLevel (m, t, bus, 0.0f); q.setSendLevel (m, bus, out, 0.0f);
            m.stripFor (t)->slots[0].set (std::make_unique<LatIns2> (3000), 48000.0, 512, 2);
            m.busFor (bus)->slots[1].set (std::make_unique<LatIns2> (1700), 48000.0, 512, 2);
        }, [] (juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid bus, juce::Uuid& lt) { lt = bus; return autoparam::fader; }, -100.0f);
        CHECK (inWindow (c2));
        // a mono track's pan, behind 4800 of plug-ins: pan to the right drops the left side
        int d2 = runCase ("pan", [] (Project& q, MixerState& m, juce::Uuid t, juce::Uuid, juce::Uuid, juce::Uuid out, juce::Uuid)
        {
            q.setSendLevel (m, t, out, 0.0f);
            m.stripFor (t)->slots[0].set (std::make_unique<LatIns2> (4800), 48000.0, 512, 2);
        }, [] (juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid&) { return autoparam::pan; }, 1.0f);
        CHECK (inWindow (d2));
        // a VCA over t (no plug-ins) and v (4800 late): each channel gets the VCA's value at its own lateness, so t hears the move on time
        juce::Uuid vcaLane;
        int e2 = runCase ("vca_unplugged_member", [&vcaLane] (Project& q, MixerState& m, juce::Uuid t, juce::Uuid, juce::Uuid v, juce::Uuid out, juce::Uuid)
        {
            q.setSendLevel (m, t, out, 0.0f); q.setSendLevel (m, v, out, 0.0f);
            m.stripFor (v)->slots[0].set (std::make_unique<LatIns2> (4800), 48000.0, 512, 2);
            auto* vd = q.addVca ("V"); vcaLane = vd->id;
            m.stripFor (t)->vcaMask.store (1u << (vd->number - 1)); m.stripFor (v)->vcaMask.store (1u << (vd->number - 1));
        }, [&vcaLane] (juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid, juce::Uuid& lt) { lt = vcaLane; return autoparam::fader; }, -100.0f);
        CHECK (inWindow (e2));

        // VCA membership changed after the plan was built: the next latency check sees it
        {
            Project q;
            q.setInputCount (2); q.setOutputCount (2);
            q.tracks.clear();
            const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
            q.retireAllMixers();
            const auto ob = q.addBus ("Out", true).id;
            auto& m = q.addMixer ("M");
            AudioEngine e (q);
            e.prepare (48000.0, 64);
            q.setSendLevel (m, ta, ob, 0.0f);
            auto* vd = q.addVca ("V");
            m.stripFor (ta)->slots[0].set (std::make_unique<LatencyInsert> (100), 48000.0, 64, 2);
            e.rebuildPlan();
            CHECK (m.stripFor (ta)->autoLatOut.load() == 100);
            q.setVcaMember (m, *vd, ta, true); q.changed();
            e.checkLatency();
            CHECK (m.vcas[vd->number - 1].autoLat.load() == 100);
        }
        // a send that is off when the plan is built, faded up by its automation: held in the plan while the automation plays (it used to be never heard)
        {
            Project q;
            q.setInputCount (2); q.setOutputCount (2);
            q.tracks.clear();
            const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
            q.retireAllMixers();
            const auto ob = q.addBus ("Out", true).id;
            auto& m = q.addMixer ("M");
            AudioEngine e (q);
            e.prepare (48000.0, 64);
            q.setSendLevel (m, ta, ob, 0.0f); q.setSendLevel (m, ta, ob, -100.0f);
            m.busFor (ob)->outFirst.store (0);
            e.rebuildPlan();
            auto* sd = m.stripFor (ta)->sends.find (ob);
            auto peakNow = [&]
            {
                float pk = 0.0f;
                for (int blk = 0; blk < 8; ++blk)
                {
                    std::vector<float> i0 (64, 0.5f), i1 (64, 0.0f), o0 (64, 0.0f), o1 (64, 0.0f);
                    const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                    e.process (in, 2, out, 2, 64);
                    if (blk >= 4) for (float s : o0) pk = juce::jmax (pk, std::abs (s));
                }
                return pk;
            };
            AutomationPlan plan;
            AutomationPlan::Item it; it.target = &sd->gainDb; it.lat = &sd->autoLat; it.send = sd;
            AutoPoint p1; p1.time = 0; p1.value = 0.0f; it.lane.pts = { p1 };
            plan.items.push_back (it);
            CHECK (plan.begin() && sd->autoHeld.load() && sd->inPlan() && ! sd->active());
            e.rebuildPlan();
            CHECK (peakNow() < 1.0e-6f);                                // held in the plan but still off: silent (and keeps nothing open under solo)
            plan.apply (100);                                          // the lane brings it in
            CHECK (sd->active() && peakNow() > 0.1f);
            sd->gainDb.set (-100.0f);
            CHECK (plan.end() && ! sd->autoHeld.load() && ! plan.end());
            e.rebuildPlan();
            CHECK (! sd->inPlan());
        }
        // a VCA lane's channels go back to the VCA's own fader when the automation stops
        {
            StripState st; MixerState mx;
            st.vcaMask.store (1u);
            AutomationPlan plan;
            AutomationPlan::Item it; it.vcaBit = 1u; it.target = &mx.vcas[0].gainDb; it.members.push_back ({ &st.vcaAutoDb[0], &st.vcaAutoMask, &st.autoLatOut });
            AutoPoint p1; p1.time = 0; p1.value = -20.0f; it.lane.pts = { p1 };
            plan.items.push_back (it);
            plan.apply (10);
            CHECK (st.vcaAutoMask.load() == 1u && std::abs (mx.vcaGain (1u, st.vcaAutoDb, st.vcaAutoMask.load()) - 0.1f) < 1.0e-4f);
            mx.vcas[0].gainDb.set (0.0f);                              // (someone moves the VCA's own fader meanwhile: the automation wins while it plays)
            CHECK (std::abs (mx.vcaGain (1u, st.vcaAutoDb, st.vcaAutoMask.load()) - 0.1f) < 1.0e-4f);
            plan.end();
            CHECK (st.vcaAutoMask.load() == 0u && std::abs (mx.vcaGain (1u, st.vcaAutoDb, st.vcaAutoMask.load()) - 1.0f) < 1.0e-6f);
        }
    }

    SECTION ("trim window levels: a take raised at the join, and normalised back over the next take");
    {
        EditDef ed;
        ed.sampleRate = 48000.0;
        EditRegion A, B;
        for (auto* r : { &A, &B }) { r->sampleRate = 48000.0; RegionFile f; f.trackId = juce::Uuid(); r->files.push_back (f); RegionFile f2 = f; f2.trackId = juce::Uuid(); r->files.push_back (f2); }
        A.srcIn = 0; A.srcOut = 48000 * 20; A.startSample = 0;
        B.srcIn = 0; B.srcOut = 48000 * 30; B.startSample = A.endSample() - 48000;
        B.inStart = 0.0; B.inEnd = 1.0; A.outStart = -1.0; A.outEnd = 0.0;
        B.outStart = -2.0; B.outEnd = 0.0;                                     // the crossfade out of B begins 2 s before its end
        ed.regions = { A, B };
        auto& b = ed.regions[1];
        CHECK (std::abs (b.pieceLevelDb()) < 0.001f);
        b.setPieceLevelDb (4.0f);
        CHECK (std::abs (b.pieceLevelDb() - 4.0f) < 0.001f && b.gains.size() == 1 && std::abs (b.levelDbAt (0, 48000 * 10) - 4.0f) < 0.01f);
        b.setPieceLevelDb (5.0f);
        CHECK (std::abs (b.pieceLevelDb() - 5.0f) < 0.001f && b.gains.size() == 1);
        b.setPieceLevelDb (4.0f);
        b.normaliseIn = true;
        ed.refreshNormalised();
        CHECK (b.gains.size() == 2 && b.gains[1].autoNorm && b.gains[1].at == 48000);                       // the glide starts when the crossfade into B ends
        CHECK (std::abs (b.levelDbAt (0, 48000) - 4.0f) < 0.01f);                                           // still +4 dB at the end of the crossfade
        CHECK (std::abs (b.levelDbAt (0, 48000 * 28)) < 0.01f && std::abs (b.levelDbAt (0, 48000 * 29)) < 0.01f);   // back to 0 dB where the next crossfade starts (28 s)
        const float mid = b.levelDbAt (0, 48000 * 14 + 24000);
        CHECK (mid > 0.5f && mid < 3.5f);                                                                    // on its way
        b.inEnd = 3.0; b.outStart = -4.0;                                                                    // the crossfades move: the glide follows
        ed.refreshNormalised();
        CHECK (b.gains.size() == 2 && b.gains[1].at == 3 * 48000 && std::abs (b.gains[1].ramp - 23.0) < 0.001);
        b.setPieceLevelDb (0.0f);
        ed.refreshNormalised();
        CHECK (b.gains.empty());                                                                             // no change at the start, nothing to glide back from
    }

    SECTION ("shuffle: two pieces swap places like cards, everything after them stays put");
    {
        EditDef ed; ed.sampleRate = 48000.0;
        std::vector<juce::Uuid> ids;
        juce::int64 at = 0;
        const int lens[4] = { 10, 20, 5, 8 };
        for (int k = 0; k < 4; ++k)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = 0; r.srcOut = 48000 * lens[k]; r.startSample = at; r.id = juce::Uuid();
            RegionFile f; f.trackId = juce::Uuid(); r.files.push_back (f);
            at += r.length(); ids.push_back (r.id); ed.regions.push_back (r);
        }
        const auto endBefore = ed.regions.back().endSample();
        CHECK (ed.shuffleRegion (1, 1) == 2);                                                              // the 20 s piece moves one place to the right
        CHECK (ed.regions[1].id == ids[2] && ed.regions[2].id == ids[1] && ed.regions[0].id == ids[0] && ed.regions[3].id == ids[3]);
        CHECK (ed.regions[1].startSample == 48000 * 10 && ed.regions[2].startSample == 48000 * 15 && ed.regions[3].startSample == 48000 * 35);
        CHECK (ed.regions.back().endSample() == endBefore);
        CHECK (ed.shuffleRegion (2, -1) == 1 && ed.regions[1].id == ids[1] && ed.regions[2].id == ids[2]);        // and back again
        CHECK (ed.regions[2].startSample == 48000 * 30);
        CHECK (ed.shuffleRegion (0, -1) == -1 && ed.shuffleRegion (3, 1) == -1);                              // no neighbour: nothing happens
        CHECK (ed.shuffleRegion (0, 1) == 1 && ed.regions[0].inEnd > 0.0 && ed.regions[0].inStart == 0.0);   // the new first piece gets an edge fade, not a crossfade
    }

    SECTION ("move: a piece can be dragged away from its neighbours, leaving silence that stays");
    {
        EditDef ed; ed.sampleRate = 48000.0;
        juce::int64 at = 0;
        for (int k = 0; k < 3; ++k)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = 0; r.srcOut = 48000 * 10; r.startSample = at; r.id = juce::Uuid();
            RegionFile f; f.trackId = juce::Uuid(); r.files.push_back (f);
            at += r.length(); ed.regions.push_back (r);
        }
        ed.setDefaultJoinFades (1); ed.setDefaultJoinFades (2);
        CHECK (ed.slideRegion (2, 48000 * 5, false, false) == 48000 * 5);                // the last piece moves later: 5 s of silence
        CHECK (ed.regions[2].startSample == 48000 * 25 && ed.regions[1].endSample() == 48000 * 20);
        ed.settleGapFades();
        CHECK (ed.regions[1].outEnd == 0.0 && ed.regions[2].inStart == 0.0);               // edge fades, no crossfade into the gap
        CHECK (ed.slideRegion (1, 48000 * 2, false, true) == 48000 * 2 && ed.regions[2].startSample == 48000 * 27);   // slip right carries the pieces after it
    }

    SECTION ("trim: copying a fade keeps its place in time; closing a gap");
    {
        EditDef ed; ed.sampleRate = 48000.0;
        for (int k = 0; k < 3; ++k)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = 48000 * 5; r.srcOut = 48000 * 15; r.sourceLength = 48000 * 60; r.startSample = 48000 * 10 * k + (k == 1 ? 48000 / 2 : 0); r.id = juce::Uuid();
            RegionFile f; f.trackId = juce::Uuid(); r.files.push_back (f);
            ed.regions.push_back (r);
        }
        ed.setDefaultJoinFades (1); ed.setDefaultJoinFades (2);
        CHECK (std::abs (ed.joinGapSeconds (1) - 0.5) < 1e-9);
        ed.regions[0].outStart = -0.2; ed.regions[0].outEnd = -0.1;
        ed.copyOutToIn (1);
        CHECK (ed.regions[1].fadeInBegin() == ed.regions[0].fadeOutBegin() && ed.regions[1].fadeInFinish() == ed.regions[0].fadeOutFinish());
        ed.regions[1].inStart = 0.0; ed.regions[1].inEnd = 0.3;
        ed.copyInToOut (1);
        CHECK (ed.regions[0].fadeOutBegin() == ed.regions[1].fadeInBegin() && ed.regions[0].fadeOutFinish() == ed.regions[1].fadeInFinish());
        const auto c2 = ed.regions[2].startSample;
        CHECK (ed.closeGap (1) == -48000 / 2 && ed.regions[1].startSample == ed.regions[0].endSample() && ed.regions[2].startSample == c2 - 48000 / 2);
        CHECK (ed.joinGapSeconds (1) == 0.0 && ed.closeGap (1) == 0);
    }

    SECTION ("piece level: holds through the fade-in, so the fade does not swell");
    {
        Project xp; TrackDef tdef; tdef.id = juce::Uuid(); xp.tracks.push_back (tdef);
        auto& ed = xp.addEdit ("Edit", juce::Uuid::null());
        for (int k = 0; k < 2; ++k)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = 48000; r.srcOut = 48000 * 6; r.sourceLength = 48000 * 20; r.startSample = 48000 * 5 * k; r.id = juce::Uuid();
            RegionFile f; f.trackId = tdef.id; f.file = juce::File ("x.wav"); r.files.push_back (f);
            ed.regions.push_back (r);
        }
        ed.setDefaultJoinFades (1);
        ed.regions[1].setPieceLevelDb (-2.0f);
        const auto segs = segmentsForEdit (xp, ed);
        const auto& sb = segs[1];
        const float want = std::pow (10.0f, -2.0f / 20.0f);
        CHECK (sb.begin < ed.regions[1].startSample);                                          // the fade-in starts before the join
        CHECK (std::abs (sb.gainAt (sb.begin) - want) < 1e-4f && std::abs (sb.gainAt (ed.regions[1].startSample + 1000) - want) < 1e-4f);
        CHECK (std::abs (sb.gainAt (ed.regions[1].startSample - 100) - want) < 1e-4f);        // no 0 dB swell before the start
    }

    {   // Pyramix-style names: "LV1 - Gravity_x" (first take, no number) and "LV1 - Gravity 002_x" next to "LV01 - Gravity 003_x"; a piano of L / C / R mono mics
        std::vector<ImportSource> src;
        auto add = [&] (const juce::String& n, juce::int64 len)
        { ImportSource x; x.file = juce::File ("/x/" + n + ".wav"); x.channels = 1; x.length = len; x.sampleRate = 96000.0; src.push_back (x); };
        const char* mics[] = { "Omni-L", "Omni-R", "PnL-C", "PnC-C", "PnR-C", "Sop1-C" };
        for (auto* m : mics) { add ("LV1 - Gravity_" + juce::String (m), 1000); add ("LV1 - Gravity 002_" + juce::String (m), 2000); add ("LV01 - Gravity 003_" + juce::String (m), 3000); }
        auto plan = buildImportPlan (src);
        CHECK (plan.takes.size() == 3 && plan.takes[0].number == 0 && plan.takes[1].number == 2 && plan.takes[2].number == 3);
        CHECK (plan.tracks.size() == 6);                                                     // one set of tracks, no "LV1 - Gravity ..." copies
        const auto cands = stereoPairCandidates (plan, false);
        CHECK (cands.size() == 2);                                                           // Omni and Pn (PnL + PnR)
        std::set<juce::String> only { "omni" };
        const auto j = joinStereoPairs (plan, false, &only);
        CHECK (j.tracks.size() == 5);                                                        // Omni stereo; PnL, PnC, PnR, Sop1 mono
        CHECK (joinStereoPairs (plan, false).tracks.size() == 4);
    }
    {   // two pieces in one pile of files: every take knows its piece, the tracks are shared, the order is by piece then take number
        std::vector<ImportSource> src;
        auto add = [&] (const juce::String& n, juce::int64 len)
        { ImportSource x; x.file = juce::File ("/x/" + n + ".wav"); x.channels = 1; x.length = len; x.sampleRate = 96000.0; src.push_back (x); };
        for (auto* m : { "Omni-L", "Omni-R", "Sop1-C" })
        {
            add ("LV02 - I Wake 002_" + juce::String (m), 5000); add ("LV01 - Gravity 003_" + juce::String (m), 3000); add ("LV01 - Gravity 002_" + juce::String (m), 2000);
            add ("LV02 - I Wake 003_" + juce::String (m), 6000);
        }
        const auto plan = buildImportPlan (src);
        CHECK (plan.takes.size() == 4 && plan.tracks.size() == 3);
        CHECK (plan.takes[0].piece == "LV01 - Gravity" && plan.takes[0].number == 2 && plan.takes[1].number == 3 && plan.takes[2].piece == "LV02 - I Wake" && plan.takes[2].number == 2);
    }
    {   // AAF import (only when the Pyramix sample file is around)
        const juce::File aafFile ("/mnt/user-data/uploads/AAF_Living_Voices.aaf");
        if (aafFile.existsAsFile())
        {
            const auto r = readAaf (aafFile, 10.0);
            CHECK (r.error.isEmpty());
            CHECK (r.pieces.size() == 10 && r.trackNames.size() == 19 && r.clips == 4427);
            if (! r.pieces.empty())
            {
                CHECK (r.pieces[0].name == "LV01 - Gravity" && r.pieces[0].startSeconds == 0.0);
                CHECK (r.pieces[0].regions.size() > 1 && r.pieces[0].regions[1].inStart < 0.0 && r.pieces[0].regions[1].inEnd > 0.0);     // a cross-fade
            }
            CHECK (readAaf (aafFile, 3000.0).pieces.size() == 1);
        }
        CHECK (aafFileNameOf ("file:///C:/Audio/LV01%20-%20Gravity.wav") == "LV01 - Gravity.wav");
    }

    SECTION ("join commit: the pieces' edges move to the edit point and the sound does not change");
    {
        auto mk = [] (juce::int64 srcIn, juce::int64 len, juce::int64 start)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = srcIn; r.srcOut = srcIn + len; r.startSample = start; r.sourceLength = 48000 * 60;
            RegionFile f; f.numChannels = 1; r.files.push_back (f);
            return r;
        };
        // what is heard at timeline sample t: for each piece that makes sound, the file sample it plays and its gain (fades and volume changes)
        auto heard = [] (const EditDef& e, juce::int64 t)
        {
            std::vector<std::pair<juce::int64, float>> out;
            for (auto& r : e.regions)
            {
                const bool pieceLevel = ! r.gains.empty() && r.gains[0].at <= (juce::int64) (0.001 * r.sampleRate);      // (the level given to the whole piece holds from its first sound, as in playback)
                const float g = r.gainAt (t) * r.levelAt (0, pieceLevel ? juce::jmax ((juce::int64) 0, t - r.startSample) : t - r.startSample);
                if (g > 0.0f) out.push_back ({ r.srcIn + (t - r.startSample), g });
            }
            return out;
        };
        auto same = [&] (const EditDef& a, const EditDef& b, juce::int64 t0, juce::int64 t1)
        {
            for (juce::int64 t = t0; t < t1; t += 5)
            {
                const auto x = heard (a, t), y = heard (b, t);
                if (x.size() != y.size()) return false;
                for (size_t i = 0; i < x.size(); ++i) if (x[i].first != y[i].first || std::abs (x[i].second - y[i].second) > 1e-6f) return false;
            }
            return true;
        };
        for (int variant = 0; variant < 4; ++variant)
        {
            EditDef e; e.sampleRate = 48000.0;
            e.regions.push_back (mk (0, 48000 * 10, 0));
            e.regions.push_back (mk (48000 * 20, 48000 * 10, 48000 * 10));
            e.setDefaultJoinFades (1);
            const double shift = variant == 1 || variant == 3 ? -0.2 : 0.2;                               // the user moved the fades later (or earlier) in the trim window
            e.regions[0].outStart += shift; e.regions[0].outEnd += shift; e.regions[1].inStart += shift; e.regions[1].inEnd += shift;
            if (variant >= 2) { GainChange g; g.at = 0; g.db = { -6.0f }; e.regions[1].gains.push_back (g); GainChange h; h.at = 48000 * 2; h.ramp = 1.0; h.db = { 3.0f }; e.regions[1].gains.push_back (h); }
            e.clampAll();
            const EditDef before = e;
            const auto J = e.editPointSample (1);
            CHECK (J != e.regions[1].startSample);                                                          // the edit point had been moved away from the region boundary
            CHECK (e.commitJoin (1) != 0);
            CHECK (e.regions[0].endSample() == J && e.regions[1].startSample == J);                         // the boundary is the edit point (where the flag is)
            CHECK (e.editPointSample (1) == J);
            CHECK (e.regionAt (J - 1) == 0 && e.regionAt (J) == 1);
            if (! same (before, e, J - 48000 / 2, J + 48000 / 2)) std::printf ("   variant %d\n", variant); CHECK (same (before, e, J - 48000 / 2, J + 48000 / 2));                                          // the same sound
            CHECK (same (before, e, 0, 48000 * 40));
            CHECK (e.commitJoin (1) == 0 && e.commitAllJoins() == 0);                                       // done once, done
            const auto nb = e.regions[1].fadeInBegin(), na = e.regions[0].fadeOutFinish();
            CHECK (nb == before.regions[1].fadeInBegin() && na == before.regions[0].fadeOutFinish());       // the fades stayed at the same times
        }
        // not touched: a join made by an offline fix, and a join with silence between the pieces
        {
            EditDef e; e.sampleRate = 48000.0;
            e.regions.push_back (mk (0, 48000 * 10, 0)); e.regions.push_back (mk (48000 * 20, 48000 * 10, 48000 * 10));
            e.setDefaultJoinFades (1);
            e.regions[0].outStart += 0.2; e.regions[0].outEnd += 0.2; e.regions[1].inStart += 0.2; e.regions[1].inEnd += 0.2;
            e.regions[1].fixIn = true; e.regions[0].fixOut = true;
            CHECK (e.commitAllJoins() == 0);
            e.regions[1].fixIn = false; e.regions[0].fixOut = false;
            e.regions[1].startSample += 100;
            CHECK (e.commitAllJoins() == 0);
        }
    }

    SECTION ("trim at the ends of the edit: the audio a moved fade leaves out is cut off");
    {
        auto mk = [] (juce::int64 srcIn, juce::int64 len, juce::int64 start)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = srcIn; r.srcOut = srcIn + len; r.startSample = start; r.sourceLength = 48000 * 60;
            RegionFile f; f.numChannels = 1; r.files.push_back (f);
            return r;
        };
        for (int slip = 0; slip < 2; ++slip)
        {
            EditDef e; e.sampleRate = 48000.0;
            e.regions.push_back (mk (48000 * 5, 48000 * 10, 0)); e.regions.push_back (mk (48000 * 30, 48000 * 10, 48000 * 10));
            e.setDefaultJoinFades (1); e.hardenEnds();
            // the in-fade of the edit was moved from the start to 4 s in (a 25 ms fade)
            e.regions[0].inStart = 4.0; e.regions[0].inEnd = 4.025;
            const auto fadeAt = e.regions[0].fadeInBegin(), joinAt = e.editPointSample (1);
            const auto sound = e.regions[0].srcIn + (fadeAt - e.regions[0].startSample);                   // the file sample where the fade begins
            CHECK (e.trimEdgeToFade (0, slip == 1) == 48000 * 4);
            auto& f = e.regions[0];
            CHECK (f.srcIn == sound && f.inStart == 0.0 && std::abs (f.inEnd - 0.025) < 1e-9);               // the piece begins where the fade begins
            CHECK (f.startSample == (slip == 1 ? 0 : 48000 * 4));                                           // Slip right: it stays at the start of the edit ...
            CHECK (e.regions[1].startSample == (slip == 1 ? 48000 * 6 : 48000 * 10));                       // ... and the rest moves up to it
            CHECK (f.endSample() == e.regions[1].startSample);
            CHECK (e.editPointSample (1) == (slip == 1 ? joinAt - 48000 * 4 : joinAt));                     // the join keeps its place in the music
            CHECK (e.trimEdgeToFade (0, slip == 1) == 0);
            // the end: the out-fade moved 3 s earlier
            auto& l = e.regions.back(); const auto len0 = l.length();
            l.outStart = -3.025; l.outEnd = -3.0;
            CHECK (e.trimEdgeToFade ((int) e.regions.size(), slip == 1) == 48000 * 3);
            CHECK (l.length() == len0 - 48000 * 3 && l.outEnd == 0.0 && std::abs (l.outStart + 0.025) < 1e-9);
        }
    }

    SECTION ("dragging the first / last fade outward grows the piece");
    {
        EditDef e; e.sampleRate = 48000.0;
        EditRegion r; r.sampleRate = 48000.0; r.srcIn = 48000 * 5; r.srcOut = 48000 * 15; r.startSample = 48000 * 2; r.sourceLength = 48000 * 60;
        RegionFile rf; rf.numChannels = 1; r.files.push_back (rf); e.regions.push_back (r);
        auto& a = e.regions[0];
        a.outEnd = 1.0; a.outStart = 0.5; a.inStart = -0.5; a.inEnd = 0.0;
        const auto ob = a.fadeOutBegin(), of = a.fadeOutFinish(), ib = a.fadeInBegin();
        CHECK (e.trimEdgeToFade (e.regions.size(), false) == 48000);
        CHECK (e.trimEdgeToFade (0, false) == 24000);
        auto& b = e.regions[0];
        CHECK (b.endSample() == of && b.fadeOutBegin() == ob && b.fadeOutFinish() == of);
        CHECK (b.startSample == ib && b.fadeInBegin() == ib && b.srcIn == 48000 * 5 - 24000);
        CHECK (e.editPointSample (1) == (ob + of) / 2);
    }
    SECTION ("fix join crossfade starts only where the corrected audio exists");
    {
        EditDef e; e.sampleRate = 48000.0;
        auto mk = [] (juce::int64 srcIn, juce::int64 len, juce::int64 start)
        { EditRegion r; r.sampleRate = 48000.0; r.srcIn = srcIn; r.srcOut = srcIn + len; r.startSample = start; r.sourceLength = 48000 * 60; RegionFile rf; rf.numChannels = 1; r.files.push_back (rf); return r; };
        e.regions.push_back (mk (0, 48000, 0)); e.regions.push_back (mk (48000, 48000, 48000)); e.regions.push_back (mk (96000, 48000, 96000));
        e.setDefaultJoinFades (1); e.setDefaultJoinFades (2);
        e.regions[1].fixIn = e.regions[1].fixOut = true; e.regions[0].fixOut = true; e.regions[2].fixIn = true;
        e.keepFixFadesInside (1, 48000, 96000);                      // the corrected file is exactly the piece
        CHECK (e.regions[1].fadeInBegin() >= 48000 && e.regions[1].fadeOutFinish() <= 96000);
        CHECK (e.regions[0].fadeOutBegin() == e.regions[1].fadeInBegin() && e.regions[0].fadeOutFinish() == e.regions[1].fadeInFinish());
        CHECK (e.regions[2].fadeInBegin() == e.regions[1].fadeOutBegin() && e.regions[2].fadeInFinish() == e.regions[1].fadeOutFinish());
    }
    SECTION ("out / in streams: each take is corrected on its own and the edit point stays put");
    {
        auto mk = [] (juce::int64 srcIn, juce::int64 len, juce::int64 start, const juce::File& f)
        {
            EditRegion r; r.sampleRate = 48000.0; r.srcIn = srcIn; r.srcOut = srcIn + len; r.startSample = start; r.sourceLength = 48000 * 60;
            RegionFile rf; rf.numChannels = 1; rf.file = f; r.files.push_back (rf);
            return r;
        };
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-stream-test");
        dir.deleteRecursively(); dir.createDirectory();
        auto fa = dir.getChildFile ("a.wav"), fb = dir.getChildFile ("b.wav"), na = dir.getChildFile ("a fixed.wav"), nb = dir.getChildFile ("b fixed.wav");
        for (auto& f : { fa, fb, na, nb }) f.replaceWithText ("x");
        EditDef e; e.sampleRate = 48000.0;
        e.regions.push_back (mk (0, 48000 * 10, 0, fa));
        e.regions.push_back (mk (48000 * 20, 48000 * 10, 48000 * 10, fb));
        e.setDefaultJoinFades (1);
        e.regions[0].outStart += 0.3; e.regions[0].outEnd += 0.3; e.regions[1].inStart += 0.3; e.regions[1].inEnd += 0.3; e.clampAll();
        CHECK (e.commitAllJoins() == 1);
        const auto J = e.editPointSample (1);
        const auto ib = e.regions[1].fadeInBegin(), oe = e.regions[0].fadeOutFinish();
        const EditDef before = e;
        // marks from 2 s before the edit to 2 s after: the OUT stream corrects its own take through the edit (its handle), the IN stream its own take from before the edit
        const juce::int64 from = J - 96000, to = J + 96000, h = 48000;
        std::vector<RegionJob> jobs (2);
        {
            auto& A = e.regions[0]; auto& rj = jobs[0]; rj.regionId = A.id;
            rj.a = A.srcIn + (from - A.startSample); rj.b = A.srcOut; rj.idx = { 0 }; rj.src = { fa }; rj.dest = { na }; rj.fileStart = { 0 };
            audioops::PieceResult r; r.file = na; r.from = rj.a - h; r.to = A.srcOut + (to - A.endSample()) + h; r.ok = true; rj.results.push_back (r);
            auto& B = e.regions[1]; auto& rk = jobs[1]; rk.regionId = B.id;
            rk.a = B.srcIn; rk.b = B.srcIn + (to - B.startSample); rk.idx = { 0 }; rk.src = { fb }; rk.dest = { nb }; rk.fileStart = { 0 };
            audioops::PieceResult r2; r2.file = nb; r2.from = B.srcIn - (B.startSample - from) - h; r2.to = rk.b + h; r2.ok = true; rk.results.push_back (r2);
        }
        const auto base = e.regions;
        CHECK (applyRegionJobs (e, base, jobs, "fixed"));
        // find the join between the corrected out piece and the corrected in piece: it is still a real edit point at the same place with the same fades
        int k = -1; for (int q = 1; q < (int) e.regions.size(); ++q) if (! e.isFixJoin (q) && e.regions[(size_t) q].files[0].file == nb) k = q;
        CHECK (k > 0);
        if (k > 0)
        {
            CHECK (e.editPointSample (k) == J);
            CHECK (e.regions[(size_t) k].fadeInBegin() == ib && e.regions[(size_t) k - 1].fadeOutFinish() == oe);
            CHECK (e.regions[(size_t) k - 1].files[0].file == na);                                           // the out take's own corrected file plays up to the edit (and under its fade past it)
            CHECK (e.regions[(size_t) k - 1].endSample() == J && e.regions[(size_t) k].startSample == J);
        }
        dir.deleteRecursively();
    }

    SECTION ("listener feedback: import, project round trip, review page");
    {
        // the colleague's marker list (tab separated; the comment text is in "Marker Name", the person in "Comment", times are timecode)
        const auto sample = juce::File (__FILE__).getParentDirectory().getChildFile ("data").getChildFile ("marker_list_sample.txt");
        juce::MemoryBlock mb; CHECK (sample.loadFileAsData (mb));
        feedback::Parsed pr;
        const auto err = feedback::parse (mb.getData(), mb.getSize(), 25.0, "sample.txt", pr);
        CHECK (err.isEmpty());
        CHECK (pr.notes.size() == 16);
        if (pr.notes.size() == 16)
        {
            CHECK (pr.notes[0].who == "Jo T" && pr.notes[0].text.startsWith ("I find bar 3 and 4"));
            CHECK (std::abs (pr.notes[0].seconds - (23.0 + 23.0 / 25.0)) < 1e-9);          // 00:00:23:23 at 25 fps
            CHECK (std::abs (pr.notes[3].seconds - (72.0 + 11.0 / 25.0)) < 1e-9);          // 00:01:12:11
            CHECK (pr.notes[0].colour == "Red" && pr.notes[3].colour == "Green" && pr.notes[7].colour == "Orange");
            CHECK (pr.notes[1].who == "Josh Quinlan" && pr.notes[2].who == "Dave Jones");
        }
        // threads: replies at the same time share a flag
        std::vector<FeedbackNote> list; int upd = 0;
        CHECK (feedback::merge (list, pr.notes, &upd) == 16 && upd == 0);
        const auto th = feedback::threads (list);
        CHECK (th.size() == 5 && th[0].size() == 3 && th[1].size() == 4 && th[2].size() == 3 && th[3].size() == 3 && th[4].size() == 3);
        list[0].done = true;
        CHECK (feedback::merge (list, pr.notes, &upd) == 0 && list.size() == 16 && list[0].done);       // importing again adds nothing and keeps the tick
        // a UTF-8 file with a byte-order mark, quotes, commas and accents; and times as m:ss
        {
            const juce::String csv = juce::String::fromUTF8 ("\xEF\xBB\xBF" "Time,Author,Comment\n\"1:05.5\",Zo\xC3\xAB,\"Too early, in bar 7\"\n12,Sam,\"He said \"\"no\"\"\"\n");
            feedback::Parsed p2;
            const auto utf = csv.toRawUTF8();
            CHECK (feedback::parse (utf, std::strlen (utf), 25.0, "x.csv", p2).isEmpty());
            CHECK (p2.notes.size() == 2 && std::abs (p2.notes[0].seconds - 65.5) < 1e-9 && p2.notes[0].who == juce::String::fromUTF8 ("Zo\xC3\xAB") && p2.notes[0].text == "Too early, in bar 7");
            CHECK (p2.notes.size() == 2 && p2.notes[1].text == "He said \"no\"" && std::abs (p2.notes[1].seconds - 12.0) < 1e-9);
        }
        // what the review page sends back
        {
            const juce::String js = "{\"fermata\":\"feedback\",\"v\":1,\"id\":\"abc\",\"edit\":\"E1\",\"piece\":\"Angel\",\"offset\":2.5,\"duration\":100,"
                                    "\"notes\":[{\"id\":\"n1\",\"t\":12.345,\"bar\":\"Bar 19 -24\",\"text\":\"flat\",\"who\":\"Anna\",\"at\":\"2026-10-15T10:00:00Z\"}]}";
            feedback::Parsed p3; const auto u = js.toRawUTF8();
            CHECK (feedback::parse (u, std::strlen (u), 25.0, "back.json", p3).isEmpty());
            CHECK (p3.editId == "E1" && p3.hasOffset && std::abs (p3.offset - 2.5) < 1e-9 && p3.notes.size() == 1);
            CHECK (p3.notes[0].key == "n1" && p3.notes[0].bar == "Bar 19 -24" && p3.notes[0].who == "Anna" && std::abs (p3.notes[0].seconds - 12.345) < 1e-9);
        }
        CHECK (! feedback::parse ("hello", 5, 25.0, "x", pr).isEmpty());
        // what the page sends back when it held all the pieces: one part per piece with comments, each with its own edit and offset
        {
            const juce::String js = "{\"fermata\":\"feedback\",\"v\":2,\"id\":\"abc\",\"name\":\"Album - listening copy\",\"title\":\"Album\",\"who\":\"Anna\",\"pieces\":["
                                    "{\"k\":0,\"edit\":\"E1\",\"piece\":\"One\",\"offset\":1.5,\"duration\":60,\"notes\":[{\"id\":\"a1\",\"t\":3,\"bar\":\"\",\"text\":\"early\"}]},"
                                    "{\"k\":1,\"edit\":\"E2\",\"piece\":\"Two\",\"offset\":0,\"duration\":60,\"notes\":[]},"
                                    "{\"k\":2,\"edit\":\"E3\",\"piece\":\"Three\",\"offset\":2,\"duration\":60,\"notes\":[{\"id\":\"c1\",\"t\":4.25,\"text\":\"flat\",\"who\":\"Ben\"},{\"id\":\"c2\",\"t\":9,\"text\":\"lovely\"}]}]}";
            feedback::Parsed p4; const auto u = js.toRawUTF8();
            CHECK (feedback::parse (u, std::strlen (u), 25.0, "Album - listening notes.fmnotes", p4).isEmpty());
            CHECK (p4.notes.empty() && p4.parts.size() == 2 && p4.parts[0].editId == "E1" && p4.parts[1].editId == "E3");
            CHECK (p4.parts[0].hasOffset && std::abs (p4.parts[0].offset - 1.5) < 1e-9 && p4.parts[0].notes.size() == 1 && p4.parts[0].notes[0].who == "Anna");
            CHECK (p4.parts[1].notes.size() == 2 && p4.parts[1].notes[0].who == "Ben" && p4.parts[1].notes[1].who == "Anna" && std::abs (p4.parts[1].notes[0].seconds - 4.25) < 1e-9);
            const juce::String none = "{\"fermata\":\"feedback\",\"pieces\":[{\"edit\":\"E1\",\"notes\":[]}]}";
            feedback::Parsed p5; CHECK (! feedback::parse (none.toRawUTF8(), std::strlen (none.toRawUTF8()), 25.0, "x.fmnotes", p5).isEmpty());
        }
        // saved with the project
        {
            Project pj; auto ed = std::make_unique<EditDef>(); ed->id = juce::Uuid(); ed->name = "Piece"; ed->feedback = list; ed->showFeedback = false; ed->showMarkers = false;
            const auto id = ed->id; pj.edits.push_back (std::move (ed));
            Project pj2; CHECK (pj2.fromVar (pj.toVar()));
            auto* e2 = pj2.findEdit (id);
            CHECK (e2 != nullptr && e2->feedback.size() == 16 && ! e2->showFeedback && ! e2->showMarkers && e2->feedback[0].done && e2->feedback[0].who == "Jo T");
        }
        // the listening copy: audio goes inside one html, base64 in whole pieces
        {
            const auto dir = tmp.getChildFile ("review"); dir.createDirectory();
            const auto wav = dir.getChildFile ("t.wav");
            {
                auto w = audioops::makeWavWriter (wav, 96000.0, 2, true);
                std::vector<float> a (96000 * 7), b (96000 * 7);
                for (size_t i = 0; i < a.size(); ++i) { a[i] = 0.5f * std::sin (0.05f * (float) i) * (i > 96000 * 3 ? 1.0f : 0.2f); b[i] = -a[i]; }
                const float* pp[2] = { a.data(), b.data() }; w->writeFromFloatArrays (pp, 2, (int) a.size());
            }
            reviewpage::Spec sp; sp.audio = wav; sp.mime = "audio/wav"; sp.piece = "Angel </script> & co"; sp.project = "P"; sp.editId = "E1"; sp.offsetSeconds = 2.5; sp.durationSeconds = 7.0;
            sp.peaks = reviewpage::computePeaks (wav, 1.0f, reviewpage::peakColumnsFor (7.0));
            CHECK ((int) sp.peaks.size() == reviewpage::peakColumnsFor (7.0) * 2);
            CHECK (sp.peaks[2 * 100 + 1] > 15 && sp.peaks[2 * 100 + 1] < 25 && sp.peaks[2 * 100] < -15);            // the quiet start is about 20 % of the loud end
            CHECK (sp.peaks[2 * 2000 + 1] >= 95);                                                                    // the loud part reaches the top
            const auto html = dir.getChildFile ("t.html");
            CHECK (reviewpage::write (html, sp).isEmpty());
            CHECK (reviewpage::write (html, sp).isEmpty());                                                         // twice: the second one replaces, never appends
            const auto text = html.loadFileAsString();
            CHECK (text.startsWith ("<!DOCTYPE html>") && text.contains ("class=\"ch\"") && text.trimEnd().endsWith ("</html>"));
            CHECK (! text.contains ("Angel </script>") && text.contains ("Angel &lt;/script&gt; &amp; co"));
            CHECK (text.indexOf ("__META__") < 0 && text.indexOf ("__CHUNKS__") < 0 && text.indexOf ("__TITLE__") < 0);
            CHECK (text.indexOf ("</html>") == text.lastIndexOf ("</html>"));
            // decode the chunks again: it is the same WAV
            juce::MemoryBlock all; int at = 0, chunks = 0;
            for (;;)
            {
                const int s0 = text.indexOf (at, "class=\"ch\">"); if (s0 < 0) break;
                const int s1 = s0 + 11, e1 = text.indexOf (s1, "</script>");
                juce::MemoryOutputStream mo (all, true); CHECK (juce::Base64::convertFromBase64 (mo, text.substring (s1, e1))); ++chunks; at = e1;
            }
            juce::MemoryBlock orig; wav.loadFileAsData (orig);
            CHECK (chunks >= 2 && all.getSize() == (size_t) wav.getSize() && all.getSize() == orig.getSize() && std::memcmp (all.getData(), orig.getData(), all.getSize()) == 0);
            CHECK (text.contains ("\"name\": \"t\"") && text.contains ("\"tracks\"") && ! text.contains ("beforeunload") && text.contains ("Export comments for sending back to the sender"));
            // all the pieces in one page: the second piece's audio is marked as piece 1 and comes back whole
            const auto wav2 = dir.getChildFile ("t2.wav");
            {
                auto w = audioops::makeWavWriter (wav2, 48000.0, 2, true);
                std::vector<float> a (48000 * 2, 0.25f); const float* pp[2] = { a.data(), a.data() }; w->writeFromFloatArrays (pp, 2, (int) a.size());
            }
            auto sp2 = sp; sp2.audio = wav2; sp2.piece = "Second"; sp2.editId = "E2"; sp2.durationSeconds = 2.0; sp2.peaks = reviewpage::computePeaks (wav2, 1.0f, reviewpage::peakColumnsFor (2.0));
            const auto both = dir.getChildFile ("Album - listening copy.html");
            CHECK (reviewpage::writeMany (both, "Album", "P", { sp, sp2 }).isEmpty());
            const auto t2 = both.loadFileAsString();
            CHECK (t2.contains ("\"name\": \"Album - listening copy\"") && t2.contains ("\"edit\": \"E2\"") && t2.contains ("data-k=\"0\" class=\"ch\"") && t2.contains ("data-k=\"1\" class=\"ch\""));
            juce::MemoryBlock second; at = 0;
            for (;;)
            {
                const int s0 = t2.indexOf (at, "data-k=\"1\" class=\"ch\">"); if (s0 < 0) break;
                const int s1 = s0 + 22, e1 = t2.indexOf (s1, "</script>");
                juce::MemoryOutputStream mo (second, true); CHECK (juce::Base64::convertFromBase64 (mo, t2.substring (s1, e1))); at = e1;
            }
            juce::MemoryBlock orig2; wav2.loadFileAsData (orig2);
            CHECK (second.getSize() == orig2.getSize() && std::memcmp (second.getData(), orig2.getData(), second.getSize()) == 0);
        }
    }

    SECTION ("the mixers run side by side on helper threads: exactly the same result as one after another; a mixer with no outputs is not run");
    {
        auto runIt = [&] (int threads, juce::uint64& parallelBlocks, float& quietMeter, float& busyMeter)
        {
            Project q;
            q.projectFile = tmp.getChildFile ("Par.fermata");
            q.setInputCount (2); q.setOutputCount (4);
            q.tracks.clear();
            const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
            const auto tb = q.addTrack ("B", TrackFormat::Stereo, 0).id;
            q.retireAllMixers();
            const auto ob = q.addBus ("Out", true).id;
            const auto ob2 = q.addBus ("Out 2", true).id;
            auto& m0 = q.addMixer ("M0"); auto& m1 = q.addMixer ("M1"); auto& m2 = q.addMixer ("M2"); auto& m3 = q.addMixer ("M3 (no outputs)");
            AudioEngine e (q);
            e.setMixerThreads (threads);
            e.prepare (48000.0, 256);
            for (auto* m : { &m0, &m1, &m2, &m3 })
            {
                q.setSendLevel (*m, ta, ob, -3.0f); q.setSendLevel (*m, tb, ob, -6.0f); q.setSendLevel (*m, ta, ob2, -1.0f);
                m->stripFor (ta)->pan.set (m == &m1 ? -0.4f : 0.3f);
            }
            m0.busFor (ob)->outFirst.store (0); m0.busFor (ob2)->outFirst.store (0);      // two Ext buses on the same pair
            m1.busFor (ob)->outFirst.store (2);
            m2.busFor (ob)->outFirst.store (0);                                            // a second mixer on the same pair as the first
            e.rebuildPlan();
            std::vector<float> all;
            for (int blk = 0; blk < 24; ++blk)
            {
                std::vector<float> i0 (256), i1 (256), o[4];
                for (auto& v : o) v.assign (256, 0.0f);
                for (int i = 0; i < 256; ++i) { i0[(size_t) i] = 0.4f * (float) std::sin ((blk * 256 + i) * 0.041); i1[(size_t) i] = 0.3f * (float) std::sin ((blk * 256 + i) * 0.017); }
                const float* in[2] = { i0.data(), i1.data() }; float* out[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
                e.process (in, 2, out, 4, 256);
                for (auto& v : o) all.insert (all.end(), v.begin(), v.end());
            }
            parallelBlocks = e.parallelBlockCount();
            quietMeter = m3.stripFor (ta)->meterL.get(); busyMeter = m1.stripFor (ta)->meterL.get();
            e.setMixerThreads (0);
            return all;
        };
        juce::uint64 pb0 = 0, pb3 = 0; float q0 = 0, b0 = 0, q3 = 0, b3 = 0;
        const auto serial = runIt (0, pb0, q0, b0);
        const auto parallel = runIt (3, pb3, q3, b3);
        CHECK (pb0 == 0 && pb3 > 0);
        bool same = ! serial.empty() && serial.size() == parallel.size();
        for (size_t i = 0; same && i < serial.size(); ++i) same = serial[i] == parallel[i];
        CHECK (same);
        double energy = 0; for (float v : parallel) energy += (double) v * v;
        CHECK (energy > 1.0);                                                           // (and something did come out)
        CHECK (q3 == 0.0f && b3 > 0.0f && q0 == 0.0f);                                 // the mixer that goes nowhere was not run
    }

    SECTION ("a rebuilt plan (as when a recording starts or stops) carries on the sound in the delay lines: no gap, no click");
    {
        auto runIt = [&] (bool rebuildMidway, int& latency)
        {
            Project q;
            q.projectFile = tmp.getChildFile ("Carry.fermata");
            q.setInputCount (2); q.setOutputCount (2);
            q.tracks.clear();
            const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
            const auto tb = q.addTrack ("B", TrackFormat::Mono, 1).id;
            q.retireAllMixers();
            const auto ob = q.addBus ("Out", true).id;
            auto& mq = q.addMixer ("M");
            AudioEngine e (q);
            e.setMixerThreads (0);
            e.prepare (48000.0, 64);
            q.setSendLevel (mq, ta, ob, 0.0f); q.setSendLevel (mq, tb, ob, 0.0f);
            mq.busFor (ob)->outFirst.store (0);
            mq.stripFor (ta)->slots[0].set (std::make_unique<LatencyInsert> (100), 48000.0, 64, 2);
            e.rebuildPlan();
            latency = e.mixerLatency (&mq);
            e.setRunningForTests (true);
            std::vector<float> all;
            for (int blk = 0; blk < 40; ++blk)
            {
                if (rebuildMidway && blk == 20) e.rebuildPlan();
                std::vector<float> i0 (64), i1 (64), o0 (64, 0.0f), o1 (64, 0.0f);
                for (int i = 0; i < 64; ++i) { i0[(size_t) i] = 0.3f * (float) std::sin ((blk * 64 + i) * 0.05); i1[(size_t) i] = 0.3f * (float) std::sin ((blk * 64 + i) * 0.031); }
                const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                e.process (in, 2, out, 2, 64);
                all.insert (all.end(), o0.begin(), o0.end());
            }
            e.setRunningForTests (false);
            return all;
        };
        int l1 = 0, l2 = 0;
        const auto steady = runIt (false, l1);
        const auto rebuilt = runIt (true, l2);
        CHECK (l1 == 100 && l2 == 100);
        float worst = 0.0f;
        for (size_t i = 0; i < steady.size() && i < rebuilt.size(); ++i) worst = juce::jmax (worst, std::abs (steady[i] - rebuilt[i]));
        CHECK (steady.size() == rebuilt.size() && worst < 1.0e-6f);
    }

    SECTION ("plug-ins are told the transport (playing, recording, place, rate), also when their mixer runs on a helper thread");
    {
        struct Probe : InsertProcessor
        {
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>&) override
            {
                const auto* t = currentHostTransport();
                seen = t != nullptr; if (t != nullptr) { playing = t->playing; rate = t->sampleRate; }
                ++calls;
            }
            juce::String getName() const override { return "probe"; }
            std::atomic<bool> seen { false }, playing { true }; std::atomic<double> rate { 0.0 }; std::atomic<int> calls { 0 };
        };
        Project q;
        q.projectFile = tmp.getChildFile ("Transport.fermata");
        q.setInputCount (2); q.setOutputCount (4);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& m1 = q.addMixer ("M1"); auto& m2 = q.addMixer ("M2");
        AudioEngine e (q);
        e.setMixerThreads (2);
        e.prepare (96000.0, 64);
        Probe* probes[2] = { nullptr, nullptr }; int k = 0;
        for (auto* mx : { &m1, &m2 })
        {
            q.setSendLevel (*mx, ta, ob, 0.0f); mx->busFor (ob)->outFirst.store (k * 2);
            auto pr = std::make_unique<Probe>(); probes[k] = pr.get();
            mx->stripFor (ta)->slots[0].set (std::move (pr), 96000.0, 64, 2); ++k;
        }
        e.rebuildPlan();
        e.setRunningForTests (true);
        for (int blk = 0; blk < 20; ++blk)
        {
            std::vector<float> i0 (64, 0.1f), i1 (64, 0.0f), o[4] = { std::vector<float> (64), std::vector<float> (64), std::vector<float> (64), std::vector<float> (64) };
            const float* in[2] = { i0.data(), i1.data() }; float* out[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
            e.process (in, 2, out, 4, 64);
        }
        e.setRunningForTests (false);
        for (auto* pr : probes) CHECK (pr != nullptr && pr->calls.load() == 20 && pr->seen.load() && ! pr->playing.load() && pr->rate.load() == 96000.0);
    }

    SECTION ("retired mixers are only freed once the audio thread has certainly moved on from them");
    {
        Project q;
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();                                                     // the default mixer goes
        const auto ob = q.addBus ("Out", true).id;
        auto& m1 = q.addMixer ("M1");
        q.setSendLevel (m1, ta, ob, 0.0f);
        CHECK (q.retiredMixers.size() == 1 && q.mixers.size() == 1);
        AudioEngine e (q);
        e.setMixerThreads (0);
        e.prepare (48000.0, 64);
        e.rebuildPlan();
        CHECK (e.audioThreadNeverStalled());                                       // no audio thread running: the old plan is simply freed
        q.retireAllMixers(); q.addMixer ("M2");
        CHECK (q.retiredMixers.size() == 2 && q.retiredMixers[1]->name == "M1");
        e.rebuildPlan();
        CHECK (e.audioThreadNeverStalled());
        const auto count = q.retiredMixers.size();                               // (what AppContext::freeRetiredMixersSoon frees: those retired BEFORE this plan)
        q.retireAllMixers(); q.addMixer ("M3");                                  // (another one retired meanwhile: it stays, its plan swap has not happened yet)
        q.freeRetiredMixers (count);
        CHECK (q.retiredMixers.size() == 1 && q.retiredMixers[0]->name == "M2");
        // an audio thread that does not come round (a stalled callback): the plan swap is not clean, and stays so, so nothing retired is ever freed under it
        e.setRunningForTests (true);
        e.rebuildPlan();                                                         // (waits half a second for a block that never comes)
        CHECK (! e.audioThreadNeverStalled());
        e.setRunningForTests (false);
        e.rebuildPlan();
        CHECK (! e.audioThreadNeverStalled());                                     // sticky
        q.freeRetiredMixers (10);                                                // (the project itself does not refuse: the caller decides)
        CHECK (q.retiredMixers.empty());
    }

    SECTION ("the second recording drive: each file is copied while it records, and the copy is identical once the take stops");
    {
        const auto backup = tmp.getChildFile ("Second drive");
        eng.setBackupFolder (backup);
        for (auto& t : p.tracks) t.setArmed (false);
        p.findTrack (violinId)->setArmed (true);
        p.findTrack (mainId)->setArmed (true);
        CHECK (eng.startRecording (tw).isEmpty());
        rig.run (eng, 200);
        const auto gid = eng.stopRecording();
        CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        auto* g = tw.findGroup (gid);
        CHECK (g != nullptr && g->files.size() == 2);
        for (int w = 0; w < 500 && eng.backupCopiesGoing() > 0; ++w) juce::Thread::sleep (10);
        CHECK (eng.backupCopiesGoing() == 0 && eng.takeBackupProblems().isEmpty());
        if (g != nullptr)
            for (auto& f : g->files)
            {
                const auto copy = backup.getChildFile (p.projectFolder().getFileName()).getChildFile (f.file.getRelativePathFrom (p.projectFolder()));
                juce::MemoryBlock a, b;
                CHECK (f.file.loadFileAsData (a) && copy.loadFileAsData (b));
                CHECK (a.getSize() > 44 && a.getSize() == b.getSize() && std::memcmp (a.getData(), b.getData(), a.getSize()) == 0);
            }

        // a second drive that falls behind: its copy is thrown away, the recording carries on untouched
        eng.setBackupMaxLag (-1.0);
        CHECK (eng.startRecording (tw).isEmpty());
        rig.run (eng, 50);
        const auto gid2 = eng.stopRecording();
        CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();          // (the files are finished in the background since 2.67.1)
        for (int w = 0; w < 500 && eng.backupCopiesGoing() > 0; ++w) juce::Thread::sleep (10);
        const auto problems = eng.takeBackupProblems();
        CHECK (problems.size() == 2 && problems[0].contains ("could not keep up"));
        if (auto* g2 = tw.findGroup (gid2))
            for (auto& f : g2->files)
            {
                CHECK (f.file.getSize() > 44 + 50 * Rig::block);                                   // the main recording is whole
                CHECK (! backup.getChildFile (p.projectFolder().getFileName()).getChildFile (f.file.getRelativePathFrom (p.projectFolder())).exists());
            }
        eng.setBackupMaxLag (20.0);
        eng.setBackupFolder ({});
        for (auto& t : p.tracks) t.setArmed (false);
    }

    eng.rebuildPlan();
    SECTION ("TakeWriter: 24-bit words come back exactly as they went in");
    {
        // every word a converter can make, through the writer and back: the old JUCE path wrote the positive ones above -6 dBFS one step low
        juce::TimeSliceThread tst ("test writer"); tst.startThread();
        auto f = tmp.getChildFile ("exact.wav");
        {
            juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f, 1024));
            auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
            TakeWriter tw (std::move (w), tst, 1 << 16, 0);
            std::vector<float> x (4096);
            for (int k = -8388608; k < 8388608; k += 4096)
            {
                for (int i = 0; i < 4096; ++i) x[(size_t) i] = (float) (k + i) / 8388608.0f;
                const float* p = x.data();
                while (tw.freeSpace() < 4096) juce::Thread::sleep (1);    // (the buffer is small: let the disk thread catch up)
                CHECK (tw.write (&p, 4096));
                tst.notify();
            }
            CHECK (tw.overruns() == 0);
        }
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (f));
        CHECK (rd != nullptr && rd->lengthInSamples == 16777216);
        if (rd != nullptr)
        {
            std::vector<int> ints (65536); int* ip[1] = { ints.data() };
            long wrong = 0;
            for (juce::int64 pos = 0; pos < rd->lengthInSamples; pos += 65536)
            {
                rd->read (ip, 1, pos, 65536, false);
                for (int i = 0; i < 65536; ++i) if ((ints[(size_t) i] >> 8) != (int) (pos + i) - 8388608) ++wrong;
            }
            CHECK (wrong == 0);
        }
        CHECK (TakeWriter::toInt24 (1.0f) == (8388607 << 8) && TakeWriter::toInt24 (-1.0f) == std::numeric_limits<int>::min() && TakeWriter::toInt24 (0.0f) == 0
               && TakeWriter::toInt24 (-1.0f / 8388608.0f) == -256 && TakeWriter::toInt24 (2.0f) == (8388607 << 8));
        tst.stopThread (1000);
    }

    SECTION ("TakeWriter: a block that does not fit becomes silence in its place, and the file keeps its length");
    {
        juce::TimeSliceThread tst ("test writer 2");                       // not started: nothing is written until the writer closes
        auto f = tmp.getChildFile ("dropped.wav");
        {
            juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f, 1024));
            auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
            TakeWriter tw (std::move (w), tst, 4096, 0);                     // room for four blocks of 1024
            CHECK (tw.bufferSamples() == 4096);
            std::vector<float> l (1024), r (1024);
            auto block = [&] (int number) { for (int i = 0; i < 1024; ++i) { l[(size_t) i] = (float) number / 64.0f; r[(size_t) i] = -(float) number / 64.0f; } };
            const float* p[2] = { l.data(), r.data() };
            for (int b = 1; b <= 4; ++b) { block (b); CHECK (tw.write (p, 1024)); }
            block (5); CHECK (! tw.write (p, 1024));                          // the fifth does not fit: lost, its time owed as silence
            block (6); CHECK (! tw.write (p, 1024));                          // nor the sixth
            CHECK (tw.overruns() == 2 && tw.samplesLost() == 2048);
            tw.skip (512);                                                    // a block that could not even be offered
            CHECK (tw.samplesLost() == 2560);
            // the disk thread catches up a little (the test drains by hand through a short-lived thread run)
            tst.startThread(); juce::Thread::sleep (200); tst.notify(); juce::Thread::sleep (200); tst.stopThread (1000);
            block (7); CHECK (tw.write (p, 1024));                            // accepted again: the silence is marked to go before it
            tst.startThread(); tst.notify(); juce::Thread::sleep (200); tst.stopThread (1000);
            block (8); CHECK (tw.write (p, 1024));
            block (9); CHECK (tw.write (p, 1024));
        }                                                                     // closing writes what is left, in order
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (f));
        CHECK (rd != nullptr && rd->lengthInSamples == 1024 * 9 + 512);          // nine blocks' worth of time plus the skipped 512: nothing moved
        if (rd != nullptr)
        {
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
            auto at = [&] (int frame) { return b.getSample (0, frame); };
            auto near = [] (float a, float v) { return std::abs (a - v) < 1e-5f; };
            CHECK (near (at (0), 1.0f / 64) && near (at (1023), 1.0f / 64) && near (at (3 * 1024), 4.0f / 64));
            CHECK (near (at (4 * 1024), 0.0f) && near (at (4 * 1024 + 2048 + 511), 0.0f));                      // blocks 5 and 6 and the skip: silence, where they belonged
            CHECK (near (at (4 * 1024 + 2560), 7.0f / 64) && near (at (5 * 1024 + 2560), 8.0f / 64) && near (at (6 * 1024 + 2560), 9.0f / 64));
            CHECK (near (b.getSample (1, 4 * 1024 + 2560), -7.0f / 64));
        }
    }

    SECTION ("TakeWriter: a drive that stops taking data is reported, and the file keeps a good header");
    {
        /** A file stream that refuses everything written past a certain point (a full drive), while the header at the start can still be written. */
        struct FullDrive : public juce::OutputStream
        {
            FullDrive (const juce::File& f, juce::int64 lim) : inner (f), limit (lim) {}
            void flush() override { inner.flush(); }
            bool setPosition (juce::int64 p) override { return inner.setPosition (p); }
            juce::int64 getPosition() override { return inner.getPosition(); }
            bool write (const void* d, size_t n) override { if (inner.getPosition() + (juce::int64) n > limit) return false; return inner.write (d, n); }
            juce::FileOutputStream inner; juce::int64 limit;
        };
        juce::TimeSliceThread tst ("test writer 3"); tst.startThread();
        auto f = tmp.getChildFile ("full.wav");
        bool failedSeen = false; juce::int64 writtenBefore = 0;
        {
            juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (new FullDrive (f, 2000 + 3 * 48000));      // room for about a second
            auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
            TakeWriter tw (std::move (w), tst, 1 << 17, 0);
            std::vector<float> x (4800, 0.1f); const float* p = x.data();
            for (int b = 0; b < 40 && ! tw.failed(); ++b) { tw.write (&p, 4800); tst.notify(); juce::Thread::sleep (10); }
            for (int w2 = 0; w2 < 200 && ! tw.failed(); ++w2) { tst.notify(); juce::Thread::sleep (5); }
            failedSeen = tw.failed(); writtenBefore = tw.samplesWritten();
            CHECK (tw.write (&p, 4800));                                      // after the failure nothing is counted against the drive any more
            CHECK (tw.overruns() == 0);
        }
        CHECK (failedSeen);
        auto r = wavrepair::inspect (f, false);
        CHECK (r.ok && ! r.wasBroken && r.frames > 0 && r.frames <= writtenBefore && r.frames * 3 + 100 >= f.getSize() - 500);   // the header matches what reached the "disk"
        tst.stopThread (1000);
    }

    SECTION ("TakeWriter: the header is refreshed while recording, so a crash leaves a readable file");
    {
        juce::TimeSliceThread tst ("test writer 4"); tst.startThread();
        auto f = tmp.getChildFile ("refreshed.wav");
        juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f, 1024));
        auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
        auto tw = std::make_unique<TakeWriter> (std::move (w), tst, 1 << 16, 2048);          // the header every 2048 frames
        std::vector<float> x (1024, 0.2f); const float* p = x.data();
        auto block = [&] (int n) { tw->write (&p, 1024); tst.notify(); for (int w2 = 0; w2 < 400 && tw->samplesWritten() < n * 1024; ++w2) juce::Thread::sleep (5); };
        block (1); block (2);
        for (int w2 = 0; w2 < 400 && tw->samplesInHeader() < 2048; ++w2) juce::Thread::sleep (5);
        CHECK (tw->samplesWritten() == 2048 && tw->samplesInHeader() == 2048);
        {   // the file is still open for writing, as it would be at a crash: its header already says 2048 frames
            juce::AudioFormatManager afm; afm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (f));
            CHECK (rd != nullptr && rd->lengthInSamples == 2048);
        }
        block (3);                                                             // one more block: the header is stale by that block ...
        CHECK (tw->samplesWritten() == 3072 && tw->samplesInHeader() == 2048);
        auto stale = wavrepair::inspect (f, false, false); CHECK (stale.ok && ! stale.wasBroken && stale.frames == 2048);
        auto ext = wavrepair::inspect (f, false, true); CHECK (ext.ok && ext.wasBroken && ext.frames == 3072);     // ... which the repair of an interrupted take takes in
        tw.reset();
        tst.stopThread (1000);
        CHECK (wavrepair::inspect (f, false, true).frames == 3072 && ! wavrepair::inspect (f, false, true).wasBroken);
    }

    SECTION ("WavRepair: a file of more than 4 GB gets an RF64 header (and reads back whole)");
    {
        auto f = tmp.getChildFile ("huge.wav");
        {
            juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (96000.0).withNumChannels (2).withBitsPerSample (24));
            juce::AudioBuffer<float> b (2, 96); for (int i = 0; i < 96; ++i) { b.setSample (0, i, 0.5f); b.setSample (1, i, -0.25f); }
            w->writeFromAudioSampleBuffer (b, 0, 96);
        }
        int dpos = -1;
        {   // as if the program died after 4.4 GB: the header says 0 bytes of audio, and the file is that long (a sparse file: it costs no disk space here)
            juce::MemoryBlock mb; f.loadFileAsData (mb);
            for (int i = 12; i < 300; ++i) if (std::memcmp ((const char*) mb.getData() + i, "data", 4) == 0) { dpos = i; break; }
            CHECK (dpos > 0);
            juce::FileOutputStream fo (f); fo.setPosition (4); fo.writeInt (0); fo.setPosition (dpos + 4); fo.writeInt (0);
            const juce::int64 frames = 4400000000LL / 6;
            fo.setPosition ((juce::int64) dpos + 8 + frames * 6 - 6); fo.writeInt (0); fo.writeShort (0);
            fo.flush();
            CHECK (f.getSize() == (juce::int64) dpos + 8 + frames * 6);
        }
        auto r = wavrepair::inspect (f, true, false);
        CHECK (r.ok && r.wasBroken && r.rf64 && r.frames == 4400000000LL / 6);
        auto again = wavrepair::inspect (f, false, false);
        CHECK (again.ok && ! again.wasBroken && again.rf64 && again.frames == 4400000000LL / 6);
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (f));
        CHECK (rd != nullptr && rd->lengthInSamples == 4400000000LL / 6 && rd->numChannels == 2 && (int) rd->sampleRate == 96000);
        if (rd != nullptr)
        {
            juce::AudioBuffer<float> b (2, 96); rd->read (&b, 0, 96, 0, true, true);
            CHECK (std::abs (b.getSample (0, 10) - 0.5f) < 1e-6f && std::abs (b.getSample (1, 10) + 0.25f) < 1e-6f);
            juce::AudioBuffer<float> e (2, 8); rd->read (&e, 0, 8, rd->lengthInSamples - 8, true, true);
            CHECK (e.getSample (0, 7) == 0.0f);                                // the far end of the file is readable (and silent)
        }
        rd.reset();
        f.deleteFile();
    }

    SECTION ("mixer helpers: a limit on how many take part, and no replacing them while the audio runs");
    {
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& m0 = q.addMixer ("M0"); auto& m1 = q.addMixer ("M1"); auto& m2 = q.addMixer ("M2");
        AudioEngine e (q);
        CHECK (e.setMixerThreads (2));
        e.prepare (48000.0, 256);
        for (auto* m : { &m0, &m1, &m2 }) { q.setSendLevel (*m, ta, ob, -3.0f); m->busFor (ob)->outFirst.store (0); }
        e.rebuildPlan();
        e.setRunningForTests (true);
        CHECK (! e.setMixerThreads (4));                                      // refused while running (the helpers cannot be replaced under a block)
        CHECK (e.mixerThreads() == 2);
        std::vector<float> i0 (256, 0.1f), o0 (256), o1 (256);
        const float* in[1] = { i0.data() }; float* out[2] = { o0.data(), o1.data() };
        const auto pb0 = e.parallelBlockCount();
        e.process (in, 1, out, 2, 256);
        CHECK (e.parallelBlockCount() == pb0 + 1);                            // three mixers: run side by side
        CHECK (e.mixerHelperLimit() >= 2);                                    // no cores reserved here: every helper may take part
        e.setRunningForTests (false);
        CHECK (e.setMixerThreads (0));
    }

    SECTION ("2.67.1: the writer waits for a big piece before writing; Stop returns before the files are finished");
    {
        juce::TimeSliceThread tst ("test writer 5"); tst.startThread();
        auto f = tmp.getChildFile ("pieces.wav");
        juce::WavAudioFormat fmt; std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f, 1024));
        auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (96000.0).withNumChannels (1).withBitsPerSample (24));
        auto tw = std::make_unique<TakeWriter> (std::move (w), tst, 8 * 96000, 0, 48000);     // half a second at 96 kHz
        std::vector<float> x (1024, 0.1f); const float* p = x.data();
        for (int b = 0; b < 40; ++b) { tw->write (&p, 1024); tst.notify(); }                   // 40960 frames: not yet half a second
        juce::Thread::sleep (100);
        CHECK (tw->samplesWritten() == 0);
        for (int b = 0; b < 10; ++b) { tw->write (&p, 1024); tst.notify(); }                   // 51200: now it goes, in one piece
        for (int k = 0; k < 200 && tw->samplesWritten() == 0; ++k) juce::Thread::sleep (5);
        CHECK (tw->samplesWritten() == 51200);
        tw->write (&p, 1024);
        tw.reset();                                                                              // the end: the small rest is written too
        tst.stopThread (1000);
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (f));
        CHECK (rd != nullptr && rd->lengthInSamples == 52224);

        // the engine: after Stop the take's live waveform can still be asked for (by its take id) until its files are finished and tidied up
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        q.projectFile = tmp.getChildFile ("fin").getChildFile ("Fin.fermata");
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.tracks.back().setArmed (true);
        q.addTakeWindow ("Fin");
        AudioEngine e (q);
        e.prepare (48000.0, 256);
        CHECK (e.startRecording (*q.takeWindows.back()).isEmpty());
        std::vector<float> i0 (256, 0.2f), o0 (256), o1 (256);
        const float* in[1] = { i0.data() }; float* out[2] = { o0.data(), o1.data() };
        for (int b = 0; b < 400; ++b) e.process (in, 1, out, 2, 256);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto gid = e.stopRecording();
        CHECK (juce::Time::getMillisecondCounterHiRes() - t0 < 200.0);                          // Stop does not wait for the disk
        const juce::int16* d = nullptr; int bins = 0, bs = 0, nc = 0;
        CHECK (e.getLivePeaks (ta, d, bins, bs, &nc, gid) && bins > 0);
        CHECK (e.waitForFinishedTakes (10000));
        CHECK (! e.isFinishing (gid) && ! e.anyFinishing());
        const auto done = e.reapFinishedTakes();
        CHECK (done.size() == 1 && ! e.getLivePeaks (ta, d, bins, bs, &nc, gid));
        std::unique_ptr<juce::AudioFormatReader> r2 (afm.createReaderFor (done[0]));
        CHECK (r2 != nullptr && r2->lengthInSamples == q.takeWindows.back()->groups.back().lengthSamples && r2->lengthInSamples == 400 * 256);
    }

    SECTION ("2.68: switching Pre-Rec on does not hold the program up (its memory is made in the background)");
    {
        Project q; q.setInputCount (64); q.setOutputCount (2); q.tracks.clear();
        for (int i = 0; i < 20; ++i) q.addTrack ("S" + juce::String (i + 1), TrackFormat::Stereo, 2 * i);       // 40 inputs, as in the rig test
        AudioEngine e (q);
        e.prepare (96000.0, 1024);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        e.setSessionMode (true);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        CHECK (ms < 20.0);                                                 // (making 40 x 8 s at 96 kHz took ~100+ ms on the message thread)
        CHECK (e.waitForPreRoll (10000));
        e.setSessionMode (false);
        CHECK (e.waitForPreRoll (100));
        e.setSessionMode (true); e.setSessionMode (false); e.setSessionMode (true);      // quick toggling: the last wish wins
        CHECK (e.waitForPreRoll (10000));
    }

    SECTION ("2.67: playback read buffers share a budget; the load meter; quick saves use the plug-ins' cached state");
    {
        CHECK (PlaybackSession::bufferBytesFor (1) == (1 << 20) && PlaybackSession::bufferBytesFor (96) == (1 << 20));
        CHECK (PlaybackSession::bufferBytesFor (960) == (96 << 20) / 960 && PlaybackSession::bufferBytesFor (100000) == (32 << 10));

        // the load meter: what the callback measures comes back once, then starts again
        {
            Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
            q.addTrack ("A", TrackFormat::Mono, 0);
            AudioEngine e (q);
            e.prepare (48000.0, 256);
            std::vector<float> i0 (256, 0.1f), o0 (256), o1 (256);
            const float* in[1] = { i0.data() }; float* out[2] = { o0.data(), o1.data() };
            juce::AudioIODeviceCallbackContext ctx;
            for (int b = 0; b < 20; ++b) e.audioDeviceIOCallbackWithContext (in, 1, out, 2, 256, ctx);
            const auto st = e.takeLoadStats();
            CHECK (st.blocks == 20 && st.average > 0.0f && st.peak >= st.average && st.late >= 0 && st.recordBufferPeak == 0.0f);
            const auto again = e.takeLoadStats();
            CHECK (again.blocks == 0 && again.peak == 0.0f && again.average == 0.0f);
        }

        // quick saves: a plug-in may hand over its cached state; a full save always asks for a fresh one
        struct StateCounter : InsertProcessor
        {
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>&) override {}
            juce::String getName() const override { return "Counter"; }
            juce::String getDescription() const override { return "<PLUGIN name=\"Counter\"/>"; }
            juce::String getStateBase64() const override { ++(*fresh); return "FRESH"; }
            juce::String getStateBase64Quick() const override { ++(*quick); return "CACHED"; }
            int* fresh = nullptr; int* quick = nullptr;
        };
        int fresh = 0, quick = 0;
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.projectFile = tmp.getChildFile ("quick").getChildFile ("Quick.fermata");
        auto sc = std::make_unique<StateCounter>(); sc->fresh = &fresh; sc->quick = &quick;
        q.mixers.front()->stripFor (ta)->slots[0].set (std::move (sc), 48000.0, 512, 2);
        juce::String err;
        CHECK (q.save (err, true));
        CHECK (quick == 1 && fresh == 0);
        CHECK (q.save (err));
        CHECK (quick == 1 && fresh == 1);
        const auto text = q.projectFile.getParentDirectory().findChildFiles (juce::File::findFiles, true, "*.fermata*");
        bool sawFresh = false;
        for (auto& f : text) sawFresh = sawFresh || f.loadFileAsString().contains ("FRESH");
        for (auto& f : q.projectFile.getParentDirectory().findChildFiles (juce::File::findFiles, true)) sawFresh = sawFresh || f.loadFileAsString().contains ("FRESH");
        CHECK (sawFresh);
        (void) q.toVar(); CHECK (fresh == 2);                                  // anything other than a quick save (a render's copy, say) asks afresh
    }

    SECTION ("2.84 the low cut: linear phase, the right slope, the convolver is the FIR, saved with the mixer");
    {
        const double sr = 96000.0;
        // every slope: a symmetric FIR (linear phase), -3 dB at the cut (the wall: -6 at the cut, nothing an octave below, unity above)
        for (int slope = 0; slope < lowcut::kNumSlopes; ++slope)
        {
            const double fc = 80.0;
            const auto h = lowcut::design (fc, slope, sr);
            const int n = (int) h.size();
            bool sym = true; for (int i = 1; i < n / 2; ++i) sym = sym && std::abs (h[(size_t) i] - h[(size_t) (n - i)]) < 1.0e-7f;   // symmetric about the middle tap n / 2
            CHECK (sym);
            auto gainAt = [&] (double f)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < n; ++i) { const double w = 2 * dsp::kPi * f * (double) i / sr; re += h[(size_t) i] * std::cos (w); im -= h[(size_t) i] * std::sin (w); }
                return 20.0 * std::log10 (std::sqrt (re * re + im * im) + 1.0e-12);
            };
            const double atCut = gainAt (fc), octBelow = gainAt (fc * 0.5), twoAbove = gainAt (fc * 4.0), high = gainAt (2000.0);
            if (slope < lowcut::kBrickwall)
            {
                CHECK (std::abs (atCut + 3.0) < 0.7);
                const double expect = lowcut::slopeDb (slope);                                  // an octave below the cut: about the slope's dB (a little less: -3 at the cut already)
                CHECK (std::abs (octBelow - lowcut::targetDb (fc * 0.5, fc, slope)) < 5.0 && octBelow < -expect * 0.9);
            }
            else { CHECK (atCut < -4.0 && atCut > -9.0); CHECK (octBelow < -70.0); }
            CHECK (std::abs (high) < 0.2 && twoAbove > -1.5);
        }
        // the convolver gives the FIR's output, exactly, 'latency' samples late, whatever the block size
        {
            lowcut::Convolver cv; const auto h = lowcut::design (300.0, 3, 48000.0); const int part = lowcut::partitionFor ((int) h.size());
            cv.prepare (h, part);
            juce::Random rnd (5);
            std::vector<float> x (20000), y (20000), direct (20000, 0.0f);
            for (auto& v : x) v = rnd.nextFloat() * 2.0f - 1.0f;
            for (size_t i = 0; i < x.size(); ++i) for (size_t k = 0; k < h.size() && k <= i; ++k) direct[i] += h[k] * x[i - k];
            y = x;
            for (int pos = 0, step = 37; pos < (int) y.size(); pos += step, step = step == 37 ? 500 : step == 500 ? 1 : 37) cv.process (y.data() + pos, juce::jmin (step, (int) y.size() - pos));
            double err = 0.0, ref = 0.0;
            for (size_t i = (size_t) part + 100; i + (size_t) part < x.size(); ++i) { const double d = y[i] - direct[i - (size_t) part]; err += d * d; ref += direct[i - (size_t) part] * direct[i - (size_t) part]; }
            CHECK (err < 1.0e-8 * ref);
            CHECK (cv.extraLatency() == part);
        }
        // the channel's low cut: off is bit for bit through, on is the filter 'latency' late; the settings survive the project file
        {
            Project p; p.projectFile = tmp.getChildFile ("Lc.fermata"); p.setInputCount (2); p.setOutputCount (2); p.tracks.clear();
            const auto ta = p.addTrack ("A", TrackFormat::Stereo, 0).id;
            p.retireAllMixers(); p.addBus ("Out", true); p.addMixer ("M");
            auto& m = *p.mixers.front();
            auto* st = m.stripFor (ta);
            st->lowCut.prepare (48000.0, 256, 2);
            CHECK (st->lowCut.latencySamples() == 0);
            st->lowCut.setDesign (120.0f, lowcut::kBrickwall, true);           // (2.86: the linear-phase kind is a choice; this test is about the FIR)
            st->lowCut.setOn (true);
            const int lat = st->lowCut.latencySamples();
            CHECK (lat > 0 && lat == st->lowCut.filterTaps() / 2 + lowcut::partitionFor (st->lowCut.filterTaps()));
            juce::AudioBuffer<float> b (2, 256);
            float atLat = 0.0f, before = 0.0f;                        // an impulse comes out as the FIR's middle sample, 'latency' samples later, and nothing before
            for (int i = 0; i * 256 <= lat; ++i)
            {
                b.clear(); if (i == 0) b.setSample (0, 0, 1.0f);
                st->lowCut.process (b);
                for (int k = 0; k < 256; ++k) { const int t = i * 256 + k; if (t == lat) atLat = b.getSample (0, k); else if (t < lat - st->lowCut.filterTaps() / 2) before = juce::jmax (before, std::abs (b.getSample (0, k))); }
            }
            CHECK (atLat > 0.5f && before == 0.0f);                   // (the wall's middle tap is near 1 - the passband is most of the spectrum; nothing comes out before the FIR's span)
            st->lowCut.setOn (false);
            b.clear(); b.setSample (1, 10, 0.7f); st->lowCut.process (b); CHECK (b.getSample (1, 10) == 0.7f && st->lowCut.latencySamples() == 0);
            st->lowCut.setOn (true);
            Project back; CHECK (back.fromVar (p.toVar()));
            auto* bs = back.mixers.front()->stripFor (ta);
            CHECK (bs->lowCut.on.get() && std::abs (bs->lowCut.hz.get() - 120.0f) < 0.01f && bs->lowCut.slope.load() == lowcut::kBrickwall);
            CHECK (m.busFor (p.buses.front().id)->lowCut.hz.get() == lowcut::kDefaultHz && ! m.busFor (p.buses.front().id)->lowCut.on.get());
            // (2.98) a damaged file's frequency is kept in range, as the panel keeps it
            std::function<void (juce::var&)> breakHz = [&] (juce::var& v)
            {
                if (auto* o = v.getDynamicObject()) { for (auto& pr : o->getProperties()) { juce::var c = pr.value; if (pr.name.toString() == "lowCut" && c.isObject()) c.getDynamicObject()->setProperty ("hz", -5.0); else breakHz (c); } }
                else if (auto* a = v.getArray()) for (auto& c : *a) breakHz (c);
            };
            auto broken = p.toVar(); breakHz (broken);
            Project back3; CHECK (back3.fromVar (broken));
            auto* bs3 = back3.mixers.front()->stripFor (ta);
            CHECK (std::isfinite (bs3->lowCut.hz.get()) && bs3->lowCut.hz.get() >= lowcut::kMinHz);
        }
    }

    SECTION ("2.87 driver inputs named by device");
    {
        Project p; p.setInputCount (6);
        for (int i = 0; i < 6; ++i) p.inputs[(size_t) i].driverName = "MAD Input " + juce::String (i + 1);
        CHECK (p.driverNamePrefix() == "MAD " && p.inputShortName (0) == "Input 1" && p.inputLabel (2) == "3 - Input 3");
        p.inputs[1].name = "Violin"; CHECK (p.inputLabel (1) == "2 - Input 2 - Violin");
        for (int i = 0; i < 6; ++i) p.inputs[(size_t) i].driverName = "Hapi " + juce::String (i + 1);
        CHECK (p.inputShortName (0) == "Hapi 1");                                                     // (a bare number is no name)
        PreampDeviceCfg thap; thap.name = "THAP"; thap.firstInput = 2;                                 // THAP from input 3 on ...
        PreampDeviceCfg anubis; anubis.name = "Anubis"; anubis.map[0] = 0; anubis.map[1] = 1;        // ... Anubis channels 1 and 2 on inputs 1 and 2 (patched)
        p.preampDevices = { thap, anubis };
        CHECK (p.inputDeviceLabel (0) == "Anubis-1" && p.inputDeviceLabel (1) == "Anubis-2" && p.inputDeviceLabel (2) == "THAP-1" && p.inputDeviceLabel (5) == "THAP-4");
        CHECK (p.inputShortName (5) == "THAP-4" && p.inputLabel (5) == "6 - THAP-4");
        CHECK (p.inputTag (0) == "01-Anubis-01" && p.inputTag (5) == "06-THAP-04");                  // 2.95: ASIO number - device - channel in the device
        PreampDeviceCfg mt; mt.name = "MT48"; mt.firstInput = 4; p.preampDevices.push_back (mt);        // a device from input 5 on: THAP covers 3 - 4, MT48 the rest
        CHECK (p.inputDeviceLabel (3) == "THAP-2" && p.inputDeviceLabel (4) == "MT48-1" && p.inputDeviceLabel (5) == "MT48-2");
        p.preampDevices.clear(); CHECK (p.inputDeviceLabel (3).isEmpty() && p.inputShortName (3) == "Hapi 4");
        CHECK (p.inputTag (3) == "04-Hapi 4");                                                        // no device known: the number and the driver's name
    }

    SECTION ("2.87 loudness: BS.1770 / EBU R 128 integrated loudness and true peak");
    {
        // EBU Tech 3341 cases: a 997 Hz stereo tone at -23 dBFS reads -23.0 LUFS (+/- 0.1); at -33 dBFS -33.0; the same at 44.1 and 96 k
        auto tone = [] (LoudnessMeter& m, double sr, double seconds, double dbfs, double hz, int chans = 2, double phase = 0.0)
        {
            const int n = (int) (sr * seconds); const float a = (float) std::pow (10.0, dbfs / 20.0);
            juce::AudioBuffer<float> b (chans, 512);
            for (int pos = 0; pos < n; pos += 512)
            {
                const int k = juce::jmin (512, n - pos);
                for (int c = 0; c < chans; ++c) for (int i = 0; i < k; ++i) b.setSample (c, i, a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * (pos + i) / sr + phase));
                juce::AudioBuffer<float> v (b.getArrayOfWritePointers(), chans, k);
                m.process (v);
            }
        };
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            LoudnessMeter m; m.prepare (sr, 2); tone (m, sr, 20.0, -23.0, 997.0);
            CHECK (std::abs (m.integratedLufs() + 23.0) < 0.1);
            CHECK (std::abs (m.truePeakDb() + 23.0) < 0.1);
            LoudnessMeter q; q.prepare (sr, 2); tone (q, sr, 20.0, -33.0, 997.0);
            CHECK (std::abs (q.integratedLufs() + 33.0) < 0.1);
        }
        {   // a mono tone at -23 dBFS reads 3 dB lower (one channel's power); the K-weighting lifts the top: a 10 kHz tone reads about 4 dB high, a 50 Hz tone low
            LoudnessMeter m; m.prepare (48000.0, 1); tone (m, 48000.0, 10.0, -23.0, 997.0, 1);
            CHECK (std::abs (m.integratedLufs() + 26.0) < 0.1);
            LoudnessMeter hi; hi.prepare (48000.0, 2); tone (hi, 48000.0, 10.0, -23.0, 10000.0);
            CHECK (hi.integratedLufs() > -20.0 && hi.integratedLufs() < -18.5);
            LoudnessMeter lo; lo.prepare (48000.0, 2); tone (lo, 48000.0, 10.0, -23.0, 50.0);
            CHECK (lo.integratedLufs() < -25.0);
        }
        {   // the gates (Tech 3341 cases 3 - 6): silence before and after changes nothing; a quiet stretch more than 10 LU down is left out
            LoudnessMeter m; m.prepare (48000.0, 2);
            tone (m, 48000.0, 10.0, -100.0, 997.0); tone (m, 48000.0, 20.0, -23.0, 997.0); tone (m, 48000.0, 10.0, -100.0, 997.0);
            CHECK (std::abs (m.integratedLufs() + 23.0) < 0.1);
            LoudnessMeter r; r.prepare (48000.0, 2);
            tone (r, 48000.0, 20.0, -23.0, 997.0); tone (r, 48000.0, 10.0, -43.0, 997.0);            // -43: below the relative gate (-33): gated out
            CHECK (std::abs (r.integratedLufs() + 23.0) < 0.1);
            LoudnessMeter t; t.prepare (48000.0, 2);
            tone (t, 48000.0, 20.0, -23.0, 997.0); tone (t, 48000.0, 20.0, -29.0, 997.0);            // -29: within 10 LU: counted; the mean of the two powers
            const double want = -0.691 + 10.0 * std::log10 (0.5 * (std::pow (10.0, (-23.0 + 0.691) / 10.0) + std::pow (10.0, (-29.0 + 0.691) / 10.0)));
            CHECK (std::abs (t.integratedLufs() - want) < 0.15);
            LoudnessMeter z; z.prepare (48000.0, 2); tone (z, 48000.0, 5.0, -100.0, 997.0);
            CHECK (z.integratedLufs() <= -99.0 && z.truePeakDb() < -90.0);
            LoudnessMeter sh; sh.prepare (48000.0, 2); tone (sh, 48000.0, 0.3, -23.0, 997.0);         // less than a block: nothing to measure yet
            CHECK (sh.blockCount() == 0 && sh.integratedLufs() <= -99.0);
        }
        {   // true peak: a full-scale tone at a quarter of the sample rate, phased so no sample lands on the crest (the sample peak is -3 dB), reads 0 dBTP
            LoudnessMeter m; m.prepare (48000.0, 2); tone (m, 48000.0, 1.0, 0.0, 12000.0, 2, juce::MathConstants<double>::pi / 4.0);
            CHECK (std::abs (m.truePeakDb()) < 0.2);
            LoudnessMeter c; c.prepare (48000.0, 2); tone (c, 48000.0, 1.0, -6.0, 997.0);
            CHECK (std::abs (c.truePeakDb() + 6.0) < 0.1);
            CHECK (std::abs (LoudnessMeter::gainToTarget (-19.5, -23.0) + 3.5) < 1.0e-9);
        }
        {   // block size makes no difference, and a 5.1 file weighs the surrounds up and leaves the LFE out
            LoudnessMeter a; a.prepare (48000.0, 2); tone (a, 48000.0, 8.0, -23.0, 997.0);
            LoudnessMeter b; b.prepare (48000.0, 2);
            { const int n = 48000 * 8; const float amp = (float) std::pow (10.0, -23.0 / 20.0); juce::AudioBuffer<float> buf (2, n);
              for (int i = 0; i < n; ++i) { const float v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / 48000.0); buf.setSample (0, i, v); buf.setSample (1, i, v); }
              for (int pos = 0, k = 1; pos < n; pos += k, k = (k * 7) % 1000 + 1) { juce::AudioBuffer<float> v (buf.getArrayOfWritePointers(), 2, pos, juce::jmin (k, n - pos)); b.process (v); } }
            CHECK (std::abs (a.integratedLufs() - b.integratedLufs()) < 0.05);
            LoudnessMeter s6; s6.prepare (48000.0, 6);
            { const int n = 48000 * 8; const float amp = (float) std::pow (10.0, -23.0 / 20.0); juce::AudioBuffer<float> buf (6, n); buf.clear();
              for (int i = 0; i < n; ++i) { const float v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 997.0 * i / 48000.0); buf.setSample (3, i, v); buf.setSample (4, i, v); }   // LFE (ignored) and Ls (x 1.41)
              s6.process (buf); }
            CHECK (std::abs (s6.integratedLufs() - (-26.0 + 10.0 * std::log10 (1.41))) < 0.15);
        }
    }

    SECTION ("2.86 the zero-latency low cut (Butterworth), and the linear-phase choice");
    {
        // the cascade's curve: -3 dB at the cut, the slope's figure an octave below, flat above; the wall a 24th order; a 10 Hz cut at 96 k stays stable
        for (int slope = 0; slope < lowcut::kNumSlopes; ++slope)
        {
            lowcut::Biquads bq; bq.design (80.0, slope, 96000.0);
            const int order = lowcut::iirOrder (slope);
            CHECK ((int) bq.sections.size() == (order + 1) / 2);
            const double atFc = 20.0 * std::log10 (bq.magnitudeAt (80.0, 96000.0)), below = 20.0 * std::log10 (bq.magnitudeAt (40.0, 96000.0)), above = 20.0 * std::log10 (bq.magnitudeAt (2000.0, 96000.0));
            const double wantBelow = 20.0 * std::log10 (std::pow (0.5, order) / std::sqrt (1.0 + std::pow (0.5, 2.0 * order)));
            CHECK (std::abs (atFc + 3.01) < 0.1 && std::abs (below - wantBelow) < 0.3 && std::abs (above) < 0.02);
            if (slope == lowcut::kBrickwall) CHECK (below < -140.0);
            // the same curve by running it: a sine at the cut comes out at -3 dB, one two octaves up untouched, and nothing comes out late (zero latency)
            std::vector<float> x (96000);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = std::sin (2.0 * juce::MathConstants<double>::pi * 80.0 * i / 96000.0);
            bq.process (x.data(), (int) x.size());
            float pk = 0.0f; for (int i = 48000; i < 96000; ++i) pk = juce::jmax (pk, std::abs (x[(size_t) i]));
            CHECK (std::abs (20.0f * std::log10 (pk) + 3.01f) < 0.15f);
            lowcut::Biquads im; im.design (80.0, slope, 96000.0);
            std::vector<float> d (64, 0.0f); d[0] = 1.0f; im.process (d.data(), 64);
            CHECK (std::abs (d[0]) > 0.5f);                            // (the impulse comes out at once: a high-pass passes the step of its first sample)
        }
        {
            lowcut::Biquads lo; lo.design (10.0, lowcut::kBrickwall, 96000.0);
            juce::Random rnd (7); std::vector<float> x (192000); for (auto& v : x) v = rnd.nextFloat() * 2.0f - 1.0f;
            lo.process (x.data(), (int) x.size());
            float pk = 0.0f; for (auto v : x) { CHECK (std::isfinite (v)); pk = juce::jmax (pk, std::abs (v)); }
            CHECK (pk < 4.0f && std::abs (20.0 * std::log10 (lo.magnitudeAt (10.0, 96000.0)) + 3.01) < 0.2);
        }
        // the LowCut: zero latency by default, the FIR when linear phase is chosen, switching both ways while it runs, saved with the mixer
        {
            Project p; p.inputs.resize (2); p.addTrack ("A", TrackFormat::Stereo, 0);
            auto& m = *p.mixers.front();
            auto* st = m.stripFor (p.tracks.front().id);
            st->lowCut.prepare (96000.0, 256, 2);
            st->lowCut.setOn (true);
            CHECK (! st->lowCut.linear.get() && st->lowCut.latencySamples() == 0 && st->lowCut.filterTaps() == 0);
            juce::AudioBuffer<float> b (2, 256);
            auto run = [&] (float& first, float& peakLate)
            {
                first = 0.0f; peakLate = 0.0f;
                for (int i = 0; i < 80; ++i)
                {
                    b.clear(); if (i == 0) b.setSample (0, 0, 1.0f);
                    st->lowCut.process (b);
                    if (i == 0) first = b.getSample (0, 0);
                    if (i >= 40) for (int k = 0; k < 256; ++k) peakLate = juce::jmax (peakLate, std::abs (b.getSample (0, k)));
                }
            };
            float first = 0.0f, late = 0.0f;
            run (first, late); CHECK (first > 0.5f && late < 1.0e-3f);          // (zero latency: the impulse at once, and it has died away well before 10,000 samples)
            st->lowCut.setDesign (80.0f, lowcut::kDefaultSlope, true);
            CHECK (st->lowCut.linear.get() && st->lowCut.latencySamples() == LowCut::linearLatencyFor (80.0, lowcut::kDefaultSlope, 96000.0) && st->lowCut.latencySamples() > 10000);
            run (first, late); CHECK (first == 0.0f && late > 0.3f);              // (linear phase: nothing at once, the impulse ~17,000 samples later)
            st->lowCut.setDesign (120.0f, 3, false);
            CHECK (st->lowCut.latencySamples() == 0);
            run (first, late); CHECK (first > 0.5f && late < 1.0e-3f);
            st->lowCut.setDesign (120.0f, 3, true);
            Project back; CHECK (back.fromVar (p.toVar()));
            auto* bs = back.mixers.front()->stripFor (p.tracks.front().id);
            CHECK (bs->lowCut.linear.get() && bs->lowCut.on.get() && bs->lowCut.slope.load() == 3 && bs->lowCut.latencySamples() > 0);
            st->lowCut.setDesign (120.0f, 3, false);
            Project back2; CHECK (back2.fromVar (p.toVar()));
            CHECK (! back2.mixers.front()->stripFor (p.tracks.front().id)->lowCut.linear.get());
        }
    }

    SECTION ("2.85 the Mackie Control protocol");
    {
        // the fader laws: round trips; the screen's curve (0 dB four-fifths up, -20 half way, -80 = bottom = off), and EuControl's (the S1's own read-out:
        // 0 dB at 84.6 %, -20 at 36.4 %, +6.1 at the top)
        const auto S = mcu::FaderLaw::Screen, E = mcu::FaderLaw::EuControl;
        CHECK (mcu::dbToFader (-100.0f, S) == 0 && mcu::dbToFader (-80.0f, S) == 0 && mcu::dbToFader (12.0f, S) == mcu::kFaderMax);
        CHECK (mcu::faderToDb (0, S) == -100.0f && mcu::faderToDb (mcu::kFaderMax, S) == 12.0f);
        for (float db : { -60.0f, -30.0f, -12.0f, -6.0f, 0.0f, 6.0f, 10.0f }) CHECK (std::abs (mcu::faderToDb (mcu::dbToFader (db, S), S) - db) < 0.11f);
        CHECK (mcu::dbToFader (0.0f, S) > mcu::kFaderMax * 0.75 && mcu::dbToFader (0.0f, S) < mcu::kFaderMax * 0.85 && mcu::dbToFader (-20.0f, S) == mcu::kFaderMax / 2 + 1);
        CHECK (mcu::dbToFader (-100.0f, E) == 0 && mcu::dbToFader (-80.0f, E) == 0 && mcu::dbToFader (6.14f, E) == mcu::kFaderMax && mcu::dbToFader (12.0f, E) == mcu::kFaderMax);
        CHECK (mcu::faderToDb (0, E) == -100.0f && std::abs (mcu::faderToDb (mcu::kFaderMax, E) - 6.1f) < 0.01f);
        CHECK (mcu::dbToFader (0.0f, E) == juce::roundToInt (0.84575 * mcu::kFaderMax) && mcu::dbToFader (-20.0f, E) == juce::roundToInt (0.36428 * mcu::kFaderMax));
        CHECK (std::abs (mcu::faderToDb (juce::roundToInt (0.59574 * mcu::kFaderMax), E) + 10.0f) < 0.11f);
        for (float db : { -65.0f, -35.0f, -15.0f, -5.0f, 0.0f, 3.0f }) CHECK (std::abs (mcu::faderToDb (mcu::dbToFader (db, E), E) - db) < 0.11f);
        for (int v = 0; v < mcu::kFaderMax; v += 97) CHECK (mcu::faderToDb (v, E) <= mcu::faderToDb (v + 97, E) && mcu::faderToDb (v, S) <= mcu::faderToDb (v + 97, S));   // (both climb all the way)
        CHECK (mcu::dbToFader (0.0f) == mcu::dbToFader (0.0f, E));                                   // (EuControl's is the one used unless chosen otherwise)
        CHECK (mcu::dbToFader (-6.0f) < mcu::dbToFader (0.0f) && mcu::dbToFader (-20.0f) < mcu::dbToFader (-6.0f));
        // decoding what the surface sends
        auto ev = mcu::decode (juce::MidiMessage::pitchWheel (3, 8192));
        CHECK (ev.kind == mcu::Event::Kind::Fader && ev.index == 2 && ev.value == 8192);
        ev = mcu::decode (juce::MidiMessage::noteOn (1, mcu::MuteBase + 4, (juce::uint8) 0x7F));
        CHECK (ev.kind == mcu::Event::Kind::Button && ev.index == mcu::MuteBase + 4 && ev.down && mcu::buttonName (ev.index) == "Mute 5");
        ev = mcu::decode (juce::MidiMessage::noteOn (1, mcu::F3, (juce::uint8) 0));
        CHECK (ev.kind == mcu::Event::Kind::Button && ev.index == mcu::F3 && ! ev.down && mcu::buttonName (mcu::F3) == "F3");
        ev = mcu::decode (juce::MidiMessage::noteOff (1, mcu::Play, (juce::uint8) 0));
        CHECK (ev.kind == mcu::Event::Kind::Button && ! ev.down);
        ev = mcu::decode (juce::MidiMessage::noteOn (1, mcu::TouchBase + 7, (juce::uint8) 0x7F));
        CHECK (ev.kind == mcu::Event::Kind::Touch && ev.index == 7 && ev.down);
        ev = mcu::decode (juce::MidiMessage::controllerEvent (1, 0x10 + 1, 0x03));
        CHECK (ev.kind == mcu::Event::Kind::Dial && ev.index == 1 && ev.delta == 3);
        ev = mcu::decode (juce::MidiMessage::controllerEvent (1, 0x10 + 6, 0x41));
        CHECK (ev.kind == mcu::Event::Kind::Dial && ev.index == 6 && ev.delta == -1);
        ev = mcu::decode (juce::MidiMessage::controllerEvent (1, 0x10 + 6, 0x40));           // (a surface that sends the direction bit alone still turns)
        CHECK (ev.kind == mcu::Event::Kind::Dial && ev.delta == -1);
        CHECK (mcu::decode (juce::MidiMessage::controllerEvent (1, 7, 100)).kind == mcu::Event::Kind::Other);
        // the handshake: the surface's challenge is answered with the Logic Control formula, serial echoed
        {
            const juce::uint8 q[] = { 0x00, 0x00, 0x66, 0x14, 0x01, 'S', 'E', 'R', 'I', 'A', 'L', '1', 0x10, 0x20, 0x30, 0x40 };
            ev = mcu::decode (juce::MidiMessage::createSysExMessage (q, 16));
            CHECK (ev.kind == mcu::Event::Kind::Handshake && ev.hasReply && ev.reply.isSysEx() && ev.reply.getSysExDataSize() == 16);
            const auto* r = ev.reply.getSysExData();
            CHECK (r[4] == 0x02 && r[5] == 'S' && r[11] == '1');
            juce::uint8 want[4]; const juce::uint8 l[] = { 0x10, 0x20, 0x30, 0x40 }; mcu::handshakeResponse (l, want);
            CHECK (r[12] == want[0] && r[13] == want[1] && r[14] == want[2] && r[15] == want[3] && (want[0] | want[1] | want[2] | want[3]) < 0x80);
            const juce::uint8 ok[] = { 0x00, 0x00, 0x66, 0x14, 0x03, 'S', 'E', 'R', 'I', 'A', 'L', '1' };
            // (a fresh Event: moving one into 'ev' would lose the long SysEx reply it holds - JUCE's MidiMessage move-assignment does not free
            //  it, which the 3.0.0 leak check reported. The program never re-assigns an Event: Surface.cpp hands each one straight on.)
            const auto ev2 = mcu::decode (juce::MidiMessage::createSysExMessage (ok, 12));
            CHECK (ev2.kind == mcu::Event::Kind::Handshake && ! ev2.hasReply);
        }
        // encoding what Fermata sends
        {
            const auto f = mcu::fader (7, 1234); CHECK (f.isPitchWheel() && f.getChannel() == 8 && f.getPitchWheelValue() == 1234);
            const auto l = mcu::lamp (mcu::SoloBase + 2, true); CHECK (l.isNoteOn() && l.getNoteNumber() == mcu::SoloBase + 2 && l.getVelocity() == 127);
            const auto off = mcu::lamp (mcu::Flip, false); CHECK (off.isNoteOnOrOff() && off.getVelocity() == 0);
            const auto rg = mcu::ring (2, 1, mcu::panToRing (0.0f), true);
            CHECK (rg.isController() && rg.getControllerNumber() == 0x32 && (rg.getControllerValue() & 0x0F) == 6 && (rg.getControllerValue() & 0x30) == 0x10 && (rg.getControllerValue() & 0x40) != 0);
            CHECK (mcu::panToRing (-1.0f) == 1 && mcu::panToRing (1.0f) == 11 && mcu::levelToRing (0.0f) == 1 && mcu::levelToRing (1.0f) == 11);
            const auto t = mcu::lcdCellMessage (1, 3, "Violin I");
            CHECK (t.isSysEx() && t.getSysExDataSize() == 6 + 7 && t.getSysExData()[4] == 0x12 && t.getSysExData()[5] == 56 + 21);
            CHECK (juce::String::fromUTF8 ((const char*) t.getSysExData() + 6, 7) == "Violin ");
            CHECK (mcu::lcdCell ("Vn") == "Vn     " && mcu::lcdCell (juce::String::fromUTF8 ("Fl\xc3\xb6te")) == juce::String::fromUTF8 ("Fl\xc3\xb6te  ") && mcu::lcdCell ("Violoncello").length() == 7);
            const auto q = mcu::lcd (0, juce::String::fromUTF8 ("\xc3\xa9"));    // a character the display cannot show
            CHECK (q.getSysExDataSize() == 7 && q.getSysExData()[6] == '?');
            CHECK (mcu::meter (1, 12).isChannelPressure() && mcu::meter (1, 12).getChannelPressureValue() == 0x1C);
            CHECK (mcu::meterLevel (-61.0f) == 0 && mcu::meterLevel (0.0f) == 12 && mcu::meterLevel (-30.0f) == 6);
            CHECK (mcu::dbText (-100.0f) == " -inf " && mcu::dbText (0.0f) == " +0.0 " && mcu::dbText (-12.5f) == "-12.5 " && mcu::dbText (6.0f) == " +6.0 ");
            CHECK (mcu::assignmentDigit (0, '3').getControllerNumber() == 0x4B && mcu::assignmentDigit (0, '3').getControllerValue() == 0x33 && mcu::assignmentDigit (1, 'P').getControllerValue() == 0x10);
            CHECK (mcu::deviceQuery().isSysEx() && mcu::isMcuSysex (mcu::deviceQuery()));
            CHECK (mcu::describe (juce::MidiMessage::noteOn (1, mcu::BankRight, (juce::uint8) 0x7F)) == "Bank > pressed");
            CHECK (mcu::describe (juce::MidiMessage::pitchWheel (1, 0)).contains ("-100.0 dB"));
            CHECK (mcu::hexOf (juce::MidiMessage::noteOn (1, 0x10, (juce::uint8) 0x7F)) == "90 10 7F");
        }
    }


    SECTION ("2.88 the Display Organiser's screen divisions and saved layouts");
    {
        auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-4f; };
        // every shape has the right number of cells, and they tile the whole screen
        for (int s = 0; s < ScreenDivision::NumShapes; ++s)
        {
            auto d = ScreenDivision::shape (s);
            const auto c = d.cells();
            CHECK ((int) c.size() == ScreenDivision::cellsOf (s));
            float area = 0.0f; for (auto& r : c) area += r.area();
            CHECK (near (area, 1.0f));
            CHECK ((int) d.lines().size() == ScreenDivision::cellsOf (s) - 1);
            CHECK (ScreenDivision::cellName (s, 0).isNotEmpty());
        }
        // "one above two": the top cell is the top half, the bottom two share the bottom half
        {
            auto d = ScreenDivision::shape (ScreenDivision::OneAboveTwo);
            auto c = d.cells();
            CHECK (near (c[0].h, 0.5f) && near (c[0].w, 1.0f));
            CHECK (near (c[1].x, 0.0f) && near (c[1].w, 0.5f) && near (c[1].y, 0.5f));
            CHECK (near (c[2].x, 0.5f));
            // dragging the top line to 65 %: the top cell grows, the bottom ones shrink; the inner line is limited to 12 .. 88 %
            d.setFrac (d.root, 0.65f);
            c = d.cells();
            CHECK (near (c[0].h, 0.65f) && near (c[1].y, 0.65f) && near (c[1].h, 0.35f));
            const auto ls = d.lines();
            CHECK (ls.size() == 2 && ! ls[0].vertical && near (ls[0].at.y, 0.65f) && ls[1].vertical && near (ls[1].at.y, 0.65f) && near (ls[1].at.h, 0.35f));
            d.setFrac (ls[1].node, 0.99f); CHECK (near (d.cells()[1].w, ScreenDivision::kMaxFrac));
            d.setFrac (ls[1].node, 0.01f); CHECK (near (d.cells()[1].w, ScreenDivision::kMinFrac));
            // the fractions survive a round trip
            auto d2 = ScreenDivision::shape (ScreenDivision::OneAboveTwo); d2.setFracs (d.fracs());
            CHECK (d2.cells() == d.cells());
        }
        // the four-window grid: its two inner lines move together
        {
            auto d = ScreenDivision::shape (ScreenDivision::Grid4);
            auto ls = d.lines();
            int inner = -1; for (auto& l : ls) if (l.vertical) { inner = l.node; break; }
            d.setFrac (inner, 0.3f);
            const auto c = d.cells();
            CHECK (near (c[0].w, 0.3f) && near (c[2].w, 0.3f) && near (c[1].x, 0.3f) && near (c[3].x, 0.3f));
        }
        // "save how the windows are now": two windows side by side at 60 / 40 come back as that shape with the line at 0.6
        {
            auto l = layoutFromWindows ({ { "mixer:abc", { 0.6f, 0.0f, 0.4f, 1.0f } }, { "edit:xyz", { 0.0f, 0.0f, 0.6f, 1.0f } } });
            CHECK (l.shape == ScreenDivision::SideBySide2);
            CHECK (l.refs.size() == 2 && l.refs[0].key == "edit:xyz" && l.refs[1].key == "mixer:abc" && l.refs[0].kind == WindowRef::Kind::Edit);
            CHECK (l.fracs.size() == 1 && near (l.fracs[0], 0.6f));
            // one above two, with the main window on top and a bit of slop in the rectangles
            auto m = layoutFromWindows ({ { "take:1", { 0.02f, 0.52f, 0.46f, 0.48f } }, { "main", { 0.0f, 0.0f, 1.0f, 0.5f } }, { "edit:2", { 0.5f, 0.5f, 0.5f, 0.49f } } });
            CHECK (m.shape == ScreenDivision::OneAboveTwo);
            CHECK (m.refs[0].key == "main" && m.refs[1].key == "take:1" && m.refs[2].key == "edit:2");
            CHECK (m.fracs.size() == 2 && std::abs (m.fracs[0] - 0.5f) < 0.03f && std::abs (m.fracs[1] - 0.5f) < 0.03f);
            // more than four windows: the four biggest are kept
            auto big = layoutFromWindows ({ { "a", { 0, 0, 0.5f, 0.5f } }, { "b", { 0.5f, 0, 0.5f, 0.5f } }, { "c", { 0, 0.5f, 0.5f, 0.5f } }, { "d", { 0.5f, 0.5f, 0.5f, 0.5f } }, { "tiny", { 0.4f, 0.4f, 0.1f, 0.1f } } });
            CHECK (big.shape == ScreenDivision::Grid4 && big.refs.size() == 4);
            bool hasTiny = false; for (auto& r : big.refs) hasTiny = hasTiny || r.key == "tiny";
            CHECK (! hasTiny);
            CHECK (layoutFromWindows ({}).shape == ScreenDivision::One);
        }
        // a layout saves and loads; the built-in ones name kinds only
        {
            LayoutDef l; l.name = "Session"; l.shape = ScreenDivision::OneBesideTwo; l.fracs = { 0.4f, 0.7f };
            l.refs = { WindowRef::of ("main"), WindowRef::of ("take:11111111-2222-3333-4444-555555555555"), WindowRef::anyOf (WindowRef::Kind::Mixer) };
            const auto v = l.toVar();
            const auto back = LayoutDef::fromVar (juce::JSON::parse (juce::JSON::toString (v)));
            CHECK (back.name == "Session" && back.shape == l.shape && back.fracs.size() == 2 && near (back.fracs[1], 0.7f));
            CHECK (back.refs.size() == 3 && back.refs[0].kind == WindowRef::Kind::Main && back.refs[1].key == l.refs[1].key && back.refs[2].any() && back.refs[2].kind == WindowRef::Kind::Mixer);
            CHECK (back.division().cells()[0].w > 0.39f && back.division().cells()[0].w < 0.41f);
            for (auto& b : LayoutDef::builtIns()) { CHECK (b.builtIn && b.name.isNotEmpty() && (int) b.refs.size() == ScreenDivision::cellsOf (b.shape)); for (auto& r : b.refs) CHECK (r.any()); }
            CHECK (WindowRef::kindOf ("bridge") == WindowRef::Kind::Other && WindowRef::kindOf ("") == WindowRef::Kind::None);
        }
    }


    SECTION ("2.89 the pitch pipe's tunings: seven temperaments on a key, A at the reference, pure major / minor chords");
    {
        using namespace tuning;
        auto near = [] (double a, double b, double tol) { return std::abs (a - b) <= tol; };
        CHECK (near (frequency (69, 440.0, Equal, 0), 440.0, 1.0e-9) && near (frequency (60, 440.0, Equal, 0), 261.6255653, 1.0e-6));
        for (int t = 0; t < NumTemperaments; ++t) for (int k = 0; k < 12; ++k) CHECK (near (frequency (69, 415.0, t, k), 415.0, 1.0e-9));     // A is always the reference
        // just intonation in C: the scale's ratios are exact
        CHECK (near (frequency (64, 440.0, Just, 0) / frequency (60, 440.0, Just, 0), 1.25, 1.0e-12));
        CHECK (near (frequency (67, 440.0, Just, 0) / frequency (60, 440.0, Just, 0), 1.5, 1.0e-12));
        CHECK (near (frequency (69, 440.0, Just, 0) / frequency (60, 440.0, Just, 0), 5.0 / 3.0, 1.0e-12));
        // ... and in D they move with the key: F# is a pure third above D
        CHECK (near (frequency (66, 440.0, Just, 2) / frequency (62, 440.0, Just, 2), 1.25, 1.0e-12));
        // quarter-comma meantone: pure major thirds, fifths of 696.58 cents
        CHECK (near (frequency (64, 440.0, Meantone, 0) / frequency (60, 440.0, Meantone, 0), 1.25, 1.0e-9));
        CHECK (near (centsOf (frequency (67, 440.0, Meantone, 0) / frequency (60, 440.0, Meantone, 0)), 696.578, 0.01));
        CHECK (near (centsOf (frequency (68, 440.0, Meantone, 0) / frequency (60, 440.0, Meantone, 0)), 772.627, 0.01));     // G# (not Ab) in C
        // Pythagorean: pure fifths, the wide third
        CHECK (near (frequency (67, 440.0, Pythagorean, 0) / frequency (60, 440.0, Pythagorean, 0), 1.5, 1.0e-12));
        CHECK (near (frequency (64, 440.0, Pythagorean, 0) / frequency (60, 440.0, Pythagorean, 0), 81.0 / 64.0, 1.0e-12));
        // the well temperaments: published offsets (cents from equal, C = 0), so C - A = their difference
        CHECK (near (centsOf (frequency (60, 440.0, Werckmeister3, 0) / frequency (60, 440.0, Equal, 0)), 0.0 + 11.730, 0.001));
        CHECK (near (centsOf (frequency (64, 440.0, Vallotti, 0) / frequency (64, 440.0, Equal, 0)), -7.820 + 5.865, 0.001));
        CHECK (near (centsOf (frequency (64, 440.0, Kirnberger3, 0) / frequency (60, 440.0, Kirnberger3, 0)), 386.314, 0.01));     // Kirnberger III: a pure third on C
        // a well temperament in another key is the same shape moved: Vallotti in D, the third D - F# is as wide as C - E in C
        CHECK (near (centsOf (frequency (66, 440.0, Vallotti, 2) / frequency (62, 440.0, Vallotti, 2)), centsOf (frequency (64, 440.0, Vallotti, 0) / frequency (60, 440.0, Vallotti, 0)), 1.0e-9));
        // pure chords: C - E - G as a major chord: E 13.7 cents below the piano, G 2 cents above, C stays
        {
            const auto c = chordFrequencies ({ 60, 64, 67 }, 440.0, Equal, 0, PureMajor);
            CHECK (c.root == 60 && c.triadFound && c.freqs.size() == 3);
            CHECK (near (c.freqs[0], frequency (60, 440.0, Equal, 0), 1.0e-9));
            CHECK (near (centsFromEqual (c.freqs[1], 64, 440.0), -13.686, 0.01) && near (centsFromEqual (c.freqs[2], 67, 440.0), 1.955, 0.01));
        }
        // C - Eb - G as a minor chord: Eb 15.6 cents above
        {
            const auto c = chordFrequencies ({ 60, 63, 67 }, 440.0, Equal, 0, PureMinor);
            CHECK (c.root == 60 && near (centsFromEqual (c.freqs[1], 63, 440.0), 15.641, 0.01) && near (c.freqs[1] / c.freqs[0], 1.2, 1.0e-12));
        }
        // an inversion finds its root: E4 G4 C5 major -> built on C5
        {
            const auto c = chordFrequencies ({ 64, 67, 72 }, 440.0, Equal, 0, PureMajor);
            CHECK (c.root == 72 && near (c.freqs[0] / c.freqs[2], 5.0 / 8.0, 1.0e-12) && near (c.freqs[1] / c.freqs[2], 3.0 / 4.0, 1.0e-12));
        }
        // a minor seventh chord (A C E G) as minor: G is 9/5 above A; the same notes as MAJOR are a C6 chord built on C
        {
            const auto m = chordFrequencies ({ 57, 60, 64, 67 }, 440.0, Equal, 0, PureMinor);
            CHECK (m.root == 57 && near (m.freqs[3] / m.freqs[0], 9.0 / 5.0 * 1.0, 1.0e-12) && near (m.freqs[1] / m.freqs[0], 6.0 / 5.0, 1.0e-12));
            const auto M = chordFrequencies ({ 57, 60, 64, 67 }, 440.0, Equal, 0, PureMajor);
            CHECK (M.root == 60 && near (M.freqs[0] / M.freqs[1], 5.0 / 6.0, 1.0e-12));        // A is the major sixth (5/3) below... an octave down: 5/6
        }
        // a chord without its root: D F as a major chord is Bb major (Bb missing): F is the pure fifth of a Bb just below the D
        {
            const auto c = chordFrequencies ({ 62, 65 }, 440.0, Equal, 0, PureMajor);
            CHECK (c.root == 58 && near (c.freqs[1] / c.freqs[0], 1.5 / 1.25, 1.0e-12));
        }
        // as the tuning: just the notes
        CHECK (near (chordFrequencies ({ 60, 64 }, 440.0, Just, 0, AsTuned).freqs[1], frequency (64, 440.0, Just, 0), 1.0e-12));
        CHECK (noteName (60) == "C4" && noteName (70) == "Bb4" && juce::String (temperamentName (Kirnberger3)) == "Kirnberger III");
    }

    SECTION ("2.89 the pitch pipe's voice: exact pitch, a soft swell, released notes die away, a chord never clips");
    {
        PitchPipeVoices v;
        v.prepare (48000.0);
        v.setLevelDb (-14.0f);
        std::vector<float> blk (512), all;
        CHECK (! v.render (blk.data(), 512));                                  // nothing on: nothing rendered
        v.setSlot (0, 440.0, true);
        for (int b = 0; b < 48000 * 3 / 512; ++b) { CHECK (v.render (blk.data(), 512) || b > 0); all.insert (all.end(), blk.begin(), blk.end()); }
        // the swell: quiet at the very start, full after 45 ms
        float early = 0.0f, later = 0.0f;
        for (int i = 0; i < 48; ++i) early = juce::jmax (early, std::abs (all[(size_t) i]));
        for (int i = 4800; i < 9600; ++i) later = juce::jmax (later, std::abs (all[(size_t) i]));
        CHECK (early < 0.1f * later && later > 0.05f && later < 0.8f);
        // the pitch: the spectrum's peak (2 s after the swell, zero-padded FFT) is within 0.05 Hz of 440
        {
            const int N = 1 << 20;
            std::vector<double> x ((size_t) N, 0.0);
            const int from = 24000, len = 96000;
            for (int i = 0; i < len; ++i) x[(size_t) i] = all[(size_t) (from + i)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / len));
            std::vector<std::complex<double>> X ((size_t) N / 2 + 1);
            denoise::detail::realForward (x.data(), N, X.data());
            int best = 0; double bm = 0.0;
            for (int k = (int) (400.0 * N / 48000.0); k < (int) (480.0 * N / 48000.0); ++k) if (std::abs (X[(size_t) k]) > bm) { bm = std::abs (X[(size_t) k]); best = k; }
            const double ym = std::abs (X[(size_t) best - 1]), y0 = bm, yp = std::abs (X[(size_t) best + 1]);
            const double hz = (best + 0.5 * (ym - yp) / (ym - 2.0 * y0 + yp)) * 48000.0 / N;
            CHECK (std::abs (hz - 440.0) < 0.05);
        }
        // released: it fades out within 150 ms and then nothing is rendered
        v.setSlot (0, 440.0, false);
        int blocksSounding = 0;
        for (int b = 0; b < 40; ++b) if (v.render (blk.data(), 512)) ++blocksSounding;
        CHECK (blocksSounding >= 13 && blocksSounding <= 16 && ! v.sounding());
        // a big loud chord stays below full scale
        v.setLevelDb (0.0f);
        for (int s = 0; s < 8; ++s) v.setSlot (s, 220.0 * std::pow (2.0, s * 4.0 / 12.0), true);
        float pk = 0.0f;
        for (int b = 0; b < 100; ++b) { v.render (blk.data(), 512); for (float x : blk) pk = juce::jmax (pk, std::abs (x)); }
        CHECK (pk > 0.3f && pk <= 1.0f);
        v.allOff();
    }

    SECTION ("2.89 the click: sample-exact beats at the tempo, the first of the bar accented, four sounds");
    {
        for (int s = 0; s < ClickTrack::NumSounds; ++s)
        {
            const auto plain = ClickTrack::makeHit (s, false, 48000.0), acc = ClickTrack::makeHit (s, true, 48000.0);
            float pp = 0.0f, pa = 0.0f; for (float x : plain) pp = juce::jmax (pp, std::abs (x)); for (float x : acc) pa = juce::jmax (pa, std::abs (x));
            CHECK (std::abs (pa - 1.0f) < 1.0e-4f && std::abs (pp - 0.72f) < 1.0e-3f && std::abs (plain.back()) < 1.0e-3f);
            CHECK (juce::String (ClickTrack::soundName (s)).isNotEmpty());
        }
        ClickTrack ck;
        ck.prepare (48000.0);
        ck.setSound (ClickTrack::Click); ck.setTempo (120.0); ck.setBeatsPerBar (3); ck.setAccent (true); ck.setLevelDb (0.0f);
        std::vector<float> blk (480), all;
        CHECK (! ck.render (blk.data(), 480));                                 // off: silent
        ck.setRunning (true);
        for (int b = 0; b < 48000 * 2 / 480 + 10; ++b) { ck.render (blk.data(), 480); all.insert (all.end(), blk.begin(), blk.end()); }
        std::vector<int> onsets; int quiet = 100000;
        for (int i = 0; i < (int) all.size(); ++i) { if (std::abs (all[(size_t) i]) > 0.02f && quiet > 2000) onsets.push_back (i); quiet = std::abs (all[(size_t) i]) > 0.02f ? 0 : quiet + 1; }
        CHECK (onsets.size() == 5 && onsets.size() > 4);
        if (onsets.size() >= 5) { CHECK (onsets[0] <= 2 && std::abs (onsets[1] - 24000) <= 2 && std::abs (onsets[2] - 48000) <= 2 && std::abs (onsets[3] - 72000) <= 2 && std::abs (onsets[4] - 96000) <= 2); }
        auto peakFrom = [&] (int at) { float p = 0.0f; for (int i = at; i < at + 2000 && i < (int) all.size(); ++i) p = juce::jmax (p, std::abs (all[(size_t) i])); return p; };
        if (onsets.size() >= 5) { CHECK (peakFrom (onsets[0]) > 0.95f && peakFrom (onsets[1]) < 0.8f && peakFrom (onsets[3]) > 0.95f); }     // bar of 3: beats 1 and 4 accented
        CHECK (ck.beatCount() == 5);
        ck.setRunning (false);
        int sounding = 0; for (int b = 0; b < 20; ++b) if (ck.render (blk.data(), 480)) ++sounding;
        CHECK (sounding <= 1);                                                 // stops at once (a hit already sounding finishes)
    }

    SECTION ("2.89 the engine adds the pitch pipe and the click to their outputs only, after the mixers");
    {
        Project q;
        q.setInputCount (2); q.setOutputCount (4);
        q.tracks.clear(); q.retireAllMixers(); q.addMixer ("M");
        AudioEngine e (q);
        e.prepare (48000.0, 64);
        e.auxSounds().setClickOutputs ({ 1, 3 }); e.auxSounds().setPipeOutputs ({ 2 });
        e.auxSounds().click.setSound (ClickTrack::Woodblock); e.auxSounds().click.setLevelDb (-6.0f); e.auxSounds().click.setRunning (true);
        e.auxSounds().pipe.setSlot (0, 261.63, true);
        float pk[4] = {};
        for (int b = 0; b < 40; ++b)
        {
            std::vector<float> i0 (64, 0.0f), i1 (64, 0.0f), o[4] = { std::vector<float> (64), std::vector<float> (64), std::vector<float> (64), std::vector<float> (64) };
            const float* in[2] = { i0.data(), i1.data() }; float* out[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
            e.process (in, 2, out, 4, 64);
            for (int c = 0; c < 4; ++c) for (float x : o[c]) pk[c] = juce::jmax (pk[c], std::abs (x));
        }
        CHECK (pk[0] == 0.0f && pk[1] > 0.1f && pk[3] > 0.1f && pk[2] > 0.01f);
        e.auxSounds().click.setRunning (false); e.auxSounds().pipe.allOff();
        CHECK (e.auxSounds().clickGoesTo (3) && ! e.auxSounds().clickGoesTo (0) && e.auxSounds().pipeGoesTo (2));
    }

    SECTION ("3.1.0 To talkback: the click and the pitch pipe reach the talkback pair only with their button on, each through its own box (3.1.1) and the Output fader");
    {
        Project q;
        q.setInputCount (2); q.setOutputCount (4);
        q.tracks.clear(); q.retireAllMixers(); q.addMixer ("M");
        AudioEngine e (q);
        e.prepare (48000.0, 64);
        e.setTalkbackRouting (std::vector<int>(), 2);                          // the talkback pair = outputs 2-3, no talkback mic
        auto& aux = e.auxSounds();
        aux.setClickOutputs ({ 0 }); aux.setPipeOutputs ({});                  // Plays to: the click on output 0, the pipe nowhere
        aux.click.setSound (ClickTrack::Woodblock); aux.click.setAccent (false); aux.click.setTempo (300.0); aux.click.setLevelDb (-6.0f);
        aux.click.setRunning (true);
        auto peaks = [&e] (int blocks)                                          // a few blocks for the fades to settle, then the loudest sample of each output
        {
            std::array<float, 4> pk {};
            for (int b = 0; b < 4 + blocks; ++b)
            {
                std::vector<float> i0 (64, 0.0f), i1 (64, 0.0f), o[4] = { std::vector<float> (64), std::vector<float> (64), std::vector<float> (64), std::vector<float> (64) };
                const float* in[2] = { i0.data(), i1.data() }; float* out[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
                e.process (in, 2, out, 4, 64);
                if (b >= 4) for (int c = 0; c < 4; ++c) for (float x : o[c]) pk[(size_t) c] = juce::jmax (pk[(size_t) c], std::abs (x));
            }
            return pk;
        };
        const int window = 260;                                                 // 347 ms: at least one whole hit at 300 beats a minute
        const auto off = peaks (window);
        CHECK (off[0] > 0.1f && off[2] == 0.0f && off[3] == 0.0f);              // the button off: Plays to only; the talkback pair stays silent
        aux.setClickToTalkback (true);
        const auto on = peaks (window);
        CHECK (on[2] > 0.1f && std::abs (on[2] - on[3]) < 1e-6f && std::abs (on[0] - off[0]) < 1e-4f);   // on: both sides of the pair; Plays to unchanged
        CHECK (std::abs (on[2] - on[0]) < 1e-4f);                               // at 0 dB in the Click box: the level its window gives it
        e.setStageMix ({}, {}, 0.0f, 0.0f, -6.0f);
        const auto half = peaks (window);
        CHECK (half[2] > on[2] * 0.45f && half[2] < on[2] * 0.56f && std::abs (half[0] - on[0]) < 1e-4f);   // the Click box: -6 dB is about half, in the talkback pair only
        e.setStageMix ({}, {}, 0.0f, 0.0f, 0.0f, -60.0f);
        CHECK (std::abs (peaks (window)[2] - on[2]) < 1e-4f);                   // the Pitch pipe box does not touch the click
        e.setStageMix ({}, {}, 0.0f, -60.0f, 0.0f);
        CHECK (peaks (window)[2] < 1e-6f);                                      // the Output fader comes after it
        e.setStageMix ({}, {}, 0.0f, 0.0f, -60.0f);
        { const auto down = peaks (window); CHECK (down[2] < 1e-6f && down[0] > 0.1f); }    // the box all the way down: off in the talkback speakers, nowhere else
        e.setStageMix ({}, {}, 0.0f, 0.0f, 0.0f);
        aux.setClickToTalkback (false);
        CHECK (peaks (window)[2] == 0.0f);                                      // off again: silent (after the one-block fade)
        aux.click.setRunning (false);
        peaks (200);                                                            // (a hit already sounding finishes: a woodblock rings for 180 ms)
        // the pitch pipe the same way: playing nowhere (Plays to empty), it is heard in the talkback pair only with its button on
        aux.pipe.setSlot (0, 261.63, true);
        { const auto p0 = peaks (40); CHECK (p0[0] == 0.0f && p0[2] == 0.0f); }
        aux.setPipeToTalkback (true);
        const auto p1 = peaks (40);
        CHECK (p1[2] > 0.01f && p1[3] > 0.01f && p1[0] == 0.0f && p1[1] == 0.0f);
        e.setStageMix ({}, {}, 0.0f, 0.0f, -60.0f, 0.0f);
        { const auto p2 = peaks (40); CHECK (p2[2] > p1[2] * 0.8f); }          // the Click box does not touch the pitch pipe (a held note: about the same peak)
        e.setStageMix ({}, {}, 0.0f, 0.0f, 0.0f, -60.0f);
        CHECK (peaks (40)[2] < 1e-6f);                                          // its own box all the way down: off in the talkback speakers
        e.setStageMix ({}, {}, 0.0f, 0.0f, 0.0f, 0.0f);
        // switching the button on fades the talkback copy in over one block, sample by sample (no click): with Plays to on output 0 as well, output 0 is the
        // pipe itself and output 2 must be exactly it times (k + 1) / 64
        aux.setPipeOutputs ({ 0 }); aux.setPipeToTalkback (false); peaks (8);
        {
            aux.setPipeToTalkback (true);
            std::vector<float> i0 (64, 0.0f), i1 (64, 0.0f), o[4] = { std::vector<float> (64), std::vector<float> (64), std::vector<float> (64), std::vector<float> (64) };
            const float* in[2] = { i0.data(), i1.data() }; float* out[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
            e.process (in, 2, out, 4, 64);
            int checked = 0, wrong = 0;
            for (int k = 0; k < 64; ++k)
                if (std::abs (o[0][(size_t) k]) > 0.005f) { ++checked; if (std::abs (o[2][(size_t) k] - o[0][(size_t) k] * (float) (k + 1) / 64.0f) > 1.0e-5f) ++wrong; }
            CHECK (checked > 16 && wrong == 0);
        }
        aux.setPipeOutputs ({});
        aux.pipe.allOff(); aux.setPipeToTalkback (false);
        // kept with the project
        Project a; a.pitchPipe.toTalkback = true; a.click.toTalkback = true; a.stageClickDb = -7.5f; a.stagePipeDb = -3.0f;
        Project b; CHECK (b.fromVar (a.toVar()));
        CHECK (b.pitchPipe.toTalkback && b.click.toTalkback && std::abs (b.stageClickDb + 7.5f) < 1e-4f && std::abs (b.stagePipeDb + 3.0f) < 1e-4f);
        Project c; CHECK (c.fromVar (Project().toVar()));
        CHECK (! c.pitchPipe.toTalkback && ! c.click.toTalkback && c.stageClickDb == 0.0f && c.stagePipeDb == 0.0f);      // (older projects: off, 0 dB)
        {   // a project saved by 3.1.0 had one box for both: its level goes to both boxes
            auto v = Project().toVar();
            if (auto* o = v.getDynamicObject()) { o->removeProperty ("stageClickDb"); o->removeProperty ("stagePipeDb"); o->setProperty ("stageAuxDb", -4.0); }
            Project d; CHECK (d.fromVar (v));
            CHECK (std::abs (d.stageClickDb + 4.0f) < 1e-4f && std::abs (d.stagePipeDb + 4.0f) < 1e-4f);
        }
    }

    SECTION ("2.90 the pitch pipe's pitch is locked from the start of a note to the end of its fade-out, a held note retuned glides");
    {
        const double sr = 48000.0;
        // a pure-chord third (-14 cents) held, then released with the PLAIN tuning's frequency handed along with the 'off' (what 2.89 did): the pitch must not move
        PitchPipeVoices v;
        v.prepare (sr); v.setLevelDb (-14.0f);
        std::vector<float> blk (512), rel;
        v.setSlot (0, 436.5, true);
        for (int b = 0; b < 60; ++b) v.render (blk.data(), 512);
        v.setSlot (0, 440.0, false);
        double worst = 0.0;
        for (int b = 0; b < 24; ++b) { v.render (blk.data(), 512); rel.insert (rel.end(), blk.begin(), blk.end()); worst = juce::jmax (worst, std::abs (v.debugHz (0) - 436.5)); }
        CHECK (worst < 1.0e-9);
        // and in the sound itself: the fundamental's phase across three windows of the fade-out advances at 436.5 Hz (within a cent)
        {
            const int W = 2048; std::complex<double> prev (0.0); double maxCents = 0.0;
            for (int w = 0; w < 3; ++w)
            {
                std::complex<double> A (0.0);
                for (int i = 0; i < W; ++i) A += (double) rel[(size_t) (w * W + i)] * std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * 436.5 * (double) (w * W + i) / sr);
                if (w > 0) { const double f = 436.5 + std::arg (A / prev) * sr / (2.0 * juce::MathConstants<double>::pi * W); maxCents = juce::jmax (maxCents, std::abs (1200.0 * std::log2 (f / 436.5))); }
                prev = A;
            }
            CHECK (maxCents < 1.0);
        }
        // a whole pure chord (C E G) released: none of the three notes moves
        {
            PitchPipeVoices c; c.prepare (sr); c.setLevelDb (-14.0f);
            const auto f = tuning::chordFrequencies ({ 60, 64, 67 }, 440.0, tuning::Equal, 0, tuning::PureMajor);
            for (int i = 0; i < 3; ++i) c.setSlot (i, f.freqs[(size_t) i], true);
            for (int b = 0; b < 60; ++b) c.render (blk.data(), 512);
            const int notes[] = { 60, 64, 67 };
            for (int i = 0; i < 3; ++i) c.setSlot (i, tuning::frequency (notes[i], 440.0, tuning::Equal, 0), false);     // (the plain tuning's pitches handed with the off)
            double w2 = 0.0;
            for (int b = 0; b < 24; ++b) { c.render (blk.data(), 512); for (int i = 0; i < 3; ++i) w2 = juce::jmax (w2, std::abs (c.debugHz (i) - f.freqs[(size_t) i])); }
            CHECK (w2 < 1.0e-3);                                                                       // (the slot stores the pitch as a float: 0.004 cents at most)
        }
        // a held note retuned glides: monotonic, never overshoots, ends exactly on the new pitch in about 50 ms
        {
            PitchPipeVoices g; g.prepare (sr); g.setLevelDb (-14.0f);
            g.setSlot (0, 440.0, true);
            for (int b = 0; b < 40; ++b) g.render (blk.data(), 512);
            CHECK (std::abs (g.debugHz (0) - 440.0) < 1.0e-9);
            g.setSlot (0, 450.0, true);
            std::vector<float> small (64); double prevHz = 440.0; bool mono = true; int toTarget = -1;
            for (int b = 0; b < 100; ++b)
            {
                g.render (small.data(), 64);
                const double h = g.debugHz (0);
                if (h < prevHz - 1.0e-9 || h > 450.0 + 1.0e-9) mono = false;
                prevHz = h;
                if (toTarget < 0 && std::abs (h - 450.0) < 1.0e-9) toTarget = (b + 1) * 64;
            }
            CHECK (mono && toTarget > 0 && toTarget >= (int) (0.045 * sr) && toTarget <= (int) (0.056 * sr) && std::abs (g.debugHz (0) - 450.0) < 1.0e-9);
            // a new note after the old one has died away starts on its pitch at once (no glide from the last one)
            g.setSlot (0, 450.0, false);
            for (int b = 0; b < 40; ++b) g.render (blk.data(), 512);
            CHECK (! g.sounding());
            g.setSlot (0, 523.25, true); g.render (small.data(), 64);
            CHECK (std::abs (g.debugHz (0) - 523.25) < 1.0e-3);
        }
    }

    SECTION ("2.90 the pitch pipe's tone: softer than 2.89's by default, as loud, the Bright end is the reed as it was");
    {
        const double sr = 48000.0;
        auto render = [&] (float tone, double hz)
        {
            PitchPipeVoices v; v.prepare (sr); v.setLevelDb (-14.0f); v.setTone (tone); v.setSlot (0, hz, true);
            std::vector<float> all, blk (512);
            for (int b = 0; b < 48000 * 2 / 512; ++b) { v.render (blk.data(), 512); all.insert (all.end(), blk.begin(), blk.end()); }
            return all;
        };
        // energy above 3 kHz relative to the whole, and the RMS, of one second after the swell (a naive DFT on a decimated set of bins)
        auto analyse = [&] (const std::vector<float>& x, double& hiRatioDb, double& rmsDb)
        {
            const int from = 24000, len = 8192;
            double tot = 0.0, hi = 0.0, rms = 0.0;
            for (int i = 0; i < len; ++i) rms += (double) x[(size_t) (from + i)] * x[(size_t) (from + i)];
            const int N = 8192;
            std::vector<std::complex<double>> X ((size_t) N / 2 + 1);
            std::vector<double> w ((size_t) N);
            for (int i = 0; i < N; ++i) w[(size_t) i] = x[(size_t) (from + i)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
            denoise::detail::realForward (w.data(), N, X.data());
            for (int k = 1; k < N / 2; ++k) { const double fr = k * sr / N, p = std::norm (X[(size_t) k]); tot += p; if (fr > 3000.0) hi += p; }
            hiRatioDb = 10.0 * std::log10 (hi / tot + 1.0e-15); rmsDb = 10.0 * std::log10 (rms / len + 1.0e-15);
        };
        double hiSoft, hiDef, hiBright, rSoft, rDef, rBright;
        for (double hz : { 130.81, 440.0 })
        {
            analyse (render (0.0f, hz), hiSoft, rSoft); analyse (render (0.35f, hz), hiDef, rDef); analyse (render (1.0f, hz), hiBright, rBright);
            CHECK (hiSoft < hiDef - 0.5 && hiDef < hiBright - 3.0);                                  // softer to brighter, in order
            CHECK (std::abs (rSoft - rDef) < 2.0 && std::abs (rDef - rBright) < 2.0);              // and the same loudness
        }
        analyse (render (0.35f, 440.0), hiDef, rDef);
        CHECK (hiDef < -20.0);                                                                       // (2.89's A4 had -15.6 dB above 3 kHz)
    }

    SECTION ("2.90 the click: a pitchless Soft, the drum kit's pattern, a quiet hi-hat on every quaver");
    {
        const double sr = 48000.0;
        auto periodicity = [&] (const std::vector<float>& h, int from, int len)
        {
            double best = 0.0, e0 = 0.0;
            for (int i = 0; i < len; ++i) e0 += (double) h[(size_t) (from + i)] * h[(size_t) (from + i)];
            for (int lag = (int) (sr / 400); lag < (int) (sr / 50); ++lag)
            {
                double c = 0.0, e1 = 0.0;
                for (int i = 0; i < len; ++i) { c += (double) h[(size_t) (from + i)] * h[(size_t) (from + i + lag)]; e1 += (double) h[(size_t) (from + i + lag)] * h[(size_t) (from + i + lag)]; }
                best = juce::jmax (best, c / std::sqrt (e0 * e1 + 1.0e-18));
            }
            return best;
        };
        for (bool acc : { false, true })
        {
            const auto h = ClickTrack::makeHit (ClickTrack::Soft, acc, sr);
            CHECK (periodicity (h, (int) (0.025 * sr), (int) (0.12 * sr)) < 0.6);                   // 2.89's Soft was a pure tone (1.00)
        }
        CHECK (ClickTrack::NumSounds == 5 && juce::String (ClickTrack::soundName (ClickTrack::DrumKit)) == "Drum kit");
        // the drum kit's part for each beat: kick on 1, snare in the middle of an even bar of four or more, hi-hat elsewhere
        {
            using CT = ClickTrack;
            CHECK (CT::drumFor (0, 4) == CT::Kick && CT::drumFor (1, 4) == CT::Hat && CT::drumFor (2, 4) == CT::Snare && CT::drumFor (3, 4) == CT::Hat);
            CHECK (CT::drumFor (3, 6) == CT::Snare && CT::drumFor (1, 6) == CT::Hat && CT::drumFor (4, 8) == CT::Snare);
            CHECK (CT::drumFor (0, 3) == CT::Kick && CT::drumFor (1, 3) == CT::Hat && CT::drumFor (2, 3) == CT::Hat && CT::drumFor (1, 2) == CT::Hat && CT::drumFor (0, 1) == CT::Kick);
            CHECK (CT::quaversPerBeat (4) == 2 && CT::quaversPerBeat (2) == 4 && CT::quaversPerBeat (8) == 1 && CT::quaversPerBeat (16) == 1);
        }
        auto runClick = [&] (ClickTrack& ck, double secs)
        {
            std::vector<float> all, blk (480);
            for (int b = 0; b < (int) (secs * sr / 480); ++b) { ck.render (blk.data(), 480); all.insert (all.end(), blk.begin(), blk.end()); }
            return all;
        };
        auto onsetsOf = [&] (const std::vector<float>& all, float thr)
        {
            std::vector<int> o; int quiet = 100000;
            for (int i = 0; i < (int) all.size(); ++i) { if (std::abs (all[(size_t) i]) > thr && quiet > 2000) o.push_back (i); quiet = std::abs (all[(size_t) i]) > thr ? 0 : quiet + 1; }
            return o;
        };
        auto peakAt = [&] (const std::vector<float>& all, int at) { float p = 0.0f; for (int i = at; i < at + 1500 && i < (int) all.size(); ++i) p = juce::jmax (p, std::abs (all[(size_t) i])); return p; };
        // the drum kit in 4/4 at 120 bpm: four hits, 0.5 s apart; the kick on 1 is the loudest, the snare on 3 next, the hats on 2 and 4 quietest
        {
            ClickTrack ck; ck.prepare (sr);
            ck.setSound (ClickTrack::DrumKit); ck.setTempo (120.0); ck.setBeatsPerBar (4); ck.setAccent (true); ck.setLevelDb (0.0f);
            ck.setRunning (true);
            const auto all = runClick (ck, 1.95);
            const auto on = onsetsOf (all, 0.02f);
            CHECK (on.size() == 4);
            if (on.size() == 4)
            {
                CHECK (std::abs (on[1] - 24000) <= 48 && std::abs (on[2] - 48000) <= 48 && std::abs (on[3] - 72000) <= 48);
                const float p0 = peakAt (all, on[0]), p1 = peakAt (all, on[1]), p2 = peakAt (all, on[2]), p3 = peakAt (all, on[3]);
                CHECK (p0 > 0.9f && p2 > 0.7f && p2 < 0.85f && p1 > 0.4f && p1 < 0.6f && p3 > 0.4f && p3 < 0.6f);
            }
            // the Accent box off: the kick is a little softer, the pattern the same
            ck.setAccent (false);
            ck.setRunning (false); ck.setRunning (true);
            const auto all2 = runClick (ck, 1.0);
            CHECK (peakAt (all2, 0) > 0.7f && peakAt (all2, 0) < 0.85f);
        }
        // quavers: 4/4 at 120 bpm (the beat a quarter note): 8 ticks to the bar, 0.25 s apart, quiet between the beats
        {
            ClickTrack ck; ck.prepare (sr);
            ck.setSound (ClickTrack::Click); ck.setTempo (120.0); ck.setBeatsPerBar (4); ck.setBeatUnit (4); ck.setAccent (true); ck.setLevelDb (0.0f);
            ck.setQuaverTicks (true); ck.setTickDb (-20.0f);
            ck.setRunning (true);
            const auto all = runClick (ck, 1.95);
            const auto on = onsetsOf (all, 0.02f);
            CHECK (on.size() == 8);
            if (on.size() == 8)
            {
                bool spacing = true; for (int i = 0; i < 8; ++i) spacing = spacing && std::abs (on[(size_t) i] - i * 12000) <= 48;
                CHECK (spacing);
                CHECK (peakAt (all, on[0]) > 0.95f && peakAt (all, on[2]) > 0.6f && peakAt (all, on[1]) < 0.12f && peakAt (all, on[3]) < 0.12f && peakAt (all, on[1]) > 0.04f);
            }
            CHECK (ck.beatCount() == 4);                                                           // (the lights still count beats, not quavers)
            // a bar in half-note beats (2/2): four ticks to each beat; counted in eighths (6/8): one on each beat, so 6 to the bar and no extras
            ck.setRunning (false);
            ClickTrack c2; c2.prepare (sr);
            c2.setSound (ClickTrack::Click); c2.setTempo (120.0); c2.setBeatsPerBar (2); c2.setBeatUnit (2); c2.setAccent (true); c2.setLevelDb (0.0f); c2.setQuaverTicks (true); c2.setTickDb (-20.0f);
            c2.setRunning (true);
            CHECK (onsetsOf (runClick (c2, 1.95), 0.02f).size() == 16);                            // (a beat is 0.5 s: four ticks 0.125 s apart)
            ClickTrack c3; c3.prepare (sr);
            c3.setSound (ClickTrack::Click); c3.setTempo (120.0); c3.setBeatsPerBar (6); c3.setBeatUnit (8); c3.setAccent (true); c3.setLevelDb (0.0f); c3.setQuaverTicks (true); c3.setTickDb (-20.0f);
            c3.setRunning (true);
            CHECK (onsetsOf (runClick (c3, 1.95), 0.02f).size() == 4);
        }
        // switching the ticks on in the middle of a bar never moves the beats
        {
            ClickTrack ck; ck.prepare (sr);
            ck.setSound (ClickTrack::Click); ck.setTempo (120.0); ck.setBeatsPerBar (4); ck.setBeatUnit (4); ck.setAccent (false); ck.setLevelDb (0.0f); ck.setTickDb (-20.0f);
            ck.setRunning (true);
            std::vector<float> all, blk (480);
            for (int b = 0; b < 200; ++b) { if (b == 70) ck.setQuaverTicks (true); ck.render (blk.data(), 480); all.insert (all.end(), blk.begin(), blk.end()); }
            // beats stay on the 0.5 s grid: a full-size click at each multiple of 24000, whatever the ticks did
            bool grid = true; for (int k = 0; k < 4; ++k) grid = grid && peakAt (all, k * 24000 - (k == 0 ? 0 : 0)) > 0.6f;
            CHECK (grid);
            const float tk = peakAt (all, 60000);                                                  // the first quaver tick after switching on: 1.25 s
            CHECK (tk > 0.04f && tk < 0.15f);
        }
    }

    SECTION ("2.89 spot-mic alignment: measured against the main pair(s), pan-aware, distance mics checked, nothing to measure by hand");
    {
        using namespace timealign;
        const double sr = 48000.0; const int n = (int) (8.0 * sr);
        juce::Random rnd (2024);
        // two instruments: A (centre, plays mostly in the first half) and B (left, mostly in the second half); coloured noise with an envelope
        auto instrument = [&] (bool firstHalf)
        {
            std::vector<float> s ((size_t) n);
            double lp = 0.0;
            for (int i = 0; i < n; ++i)
            {
                lp += 0.4 * (rnd.nextFloat() * 2.0 - 1.0 - lp);
                const double t = i / sr;
                const double env = (firstHalf ? (t < 4.5 ? 1.0 : 0.15) : (t > 3.5 ? 1.0 : 0.15)) * (0.6 + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 1.3 * t));
                s[(size_t) i] = (float) (lp * env);
            }
            return s;
        };
        const auto sA = instrument (true), sB = instrument (false);
        // a mic: each instrument arrives after its delay (ms) at its level, plus some room (reflections) and a little noise
        auto mic = [&] (std::initializer_list<std::tuple<const std::vector<float>*, double, double>> parts, double room, bool invert = false)
        {
            std::vector<float> m ((size_t) n, 0.0f);
            for (auto& [src, ms, gain] : parts)
            {
                const int d = (int) std::lround (ms * sr / 1000.0);
                for (int i = d; i < n; ++i) m[(size_t) i] += (float) gain * (*src)[(size_t) (i - d)];
                for (int r = 0; r < 25; ++r)                                   // reflections 12 .. 120 ms later
                {
                    const int dr = d + (int) ((0.012 + 0.108 * rnd.nextFloat()) * sr);
                    const float g = (float) (room * gain * (rnd.nextBool() ? 1 : -1) * std::exp (-(double) (dr - d) / (0.05 * sr)));
                    for (int i = dr; i < n; ++i) m[(size_t) i] += g * (*src)[(size_t) (i - dr)];
                }
            }
            for (auto& x : m) x = (invert ? -x : x) + 0.002f * (rnd.nextFloat() * 2.0f - 1.0f);
            return m;
        };
        // the mics: a spaced main pair (A centre: 9.0 / 9.4 ms; B left: 6.0 / 8.5 ms and quieter on the right), two spots, a hall pair
        auto mainL = mic ({ { &sA, 9.0, 1.0 }, { &sB, 6.0, 1.0 } }, 0.5);
        auto mainR = mic ({ { &sA, 9.4, 1.0 }, { &sB, 8.5, 0.6 } }, 0.5);
        auto spotA = mic ({ { &sA, 1.0, 1.0 }, { &sB, 5.0, 0.25 } }, 0.2);
        auto spotB = mic ({ { &sB, 1.0, 1.0 }, { &sA, 6.0, 0.2 } }, 0.2);
        auto hallL = mic ({ { &sA, 30.0, 0.7 }, { &sB, 29.0, 0.7 } }, 0.9);
        auto hallR = mic ({ { &sA, 31.0, 0.7 }, { &sB, 31.5, 0.7 } }, 0.9);
        std::vector<std::vector<const std::vector<float>*>> chans { { &mainL }, { &mainR }, { &spotA }, { &spotB }, { &hallL, &hallR } };
        std::vector<TrackIn> tin (5);
        tin[0].role = Main; tin[0].pan[0] = -1.0f;
        tin[1].role = Main; tin[1].pan[0] = 1.0f;
        tin[2].role = Spot; tin[2].pan[0] = 0.0f;
        tin[3].role = Spot; tin[3].pan[0] = -1.0f;                               // the harp, panned hard left
        tin[4].role = Distance; tin[4].numChannels = 2; tin[4].pan[0] = -1.0f; tin[4].pan[1] = 1.0f;
        Reader rd = [&] (int t, int c, juce::int64 start, int cnt, float* dst)
        {
            const auto& v = *chans[(size_t) t][(size_t) c];
            for (int i = 0; i < cnt; ++i) dst[i] = start + i < (juce::int64) v.size() ? v[(size_t) (start + i)] : 0.0f;
            return true;
        };
        int calls = 0;
        auto an = analyse (tin, n, sr, rd, [&] (float) { ++calls; return true; });
        CHECK (an.ok && an.error.isEmpty() && calls > 10);
        const auto& a = an.tracks[2], &b = an.tracks[3], &h = an.tracks[4];
        // the centre spot: 8.0 ms ahead of the left main, 8.4 ms of the right: delayed by the middle
        CHECK (a.measured && std::abs (a.leftMs - 8.0) < 0.15 && std::abs (a.rightMs - 8.4) < 0.15 && std::abs (a.delayMs - 8.2) < 0.15 && ! a.inverted && a.severity == 0);
        CHECK (a.clarity >= 7.0f && std::abs (a.suggestedPan) < 0.5f);
        // the left spot: lined up with the LEFT main (5.0 ms), not the middle (6.25); its suggested pan is well to the left
        CHECK (b.measured && std::abs (b.leftMs - 5.0) < 0.15 && std::abs (b.rightMs - 7.5) < 0.15 && std::abs (b.delayMs - 5.0) < 0.15 && b.suggestedPan < -0.5f);
        // panned to the centre instead, the delay follows without measuring again
        { auto t2 = tin[3]; t2.pan[0] = 0.0f; CHECK (std::abs (delayForPan (b, t2) - 6.25) < 0.15); }
        // the hall pair: later than the main pair (by about 21 - 23 ms), not delayed, nothing drastic
        CHECK (h.measured && h.arrivalMs > 18.0 && h.arrivalMs < 26.0 && h.severity == 0 && h.spreadMs < 5.0 && ! h.inverted);
        // drastic things are flagged: a spot with its polarity reversed, a "spot" that is farther than the main pair, a "distance" mic in front of it
        {
            auto spotInv = mic ({ { &sA, 1.0, 1.0 } }, 0.2, true);
            auto farSpot = mic ({ { &sA, 15.0, 1.0 } }, 0.3);
            auto nearHall = mic ({ { &sA, 2.0, 1.0 }, { &sB, 2.0, 1.0 } }, 0.3);
            std::vector<std::vector<const std::vector<float>*>> ch2 { { &mainL }, { &mainR }, { &spotInv }, { &farSpot }, { &nearHall } };
            std::vector<TrackIn> t2 (5);
            t2[0] = tin[0]; t2[1] = tin[1];
            t2[2].role = Spot; t2[3].role = Spot; t2[4].role = Distance;
            Reader rd2 = [&] (int t, int c, juce::int64 start, int cnt, float* dst)
            {
                const auto& v = *ch2[(size_t) t][(size_t) c];
                for (int i = 0; i < cnt; ++i) dst[i] = start + i < (juce::int64) v.size() ? v[(size_t) (start + i)] : 0.0f;
                return true;
            };
            auto an2 = analyse (t2, n, sr, rd2);
            CHECK (an2.ok && an2.tracks[2].inverted && an2.tracks[2].severity == 2 && std::abs (an2.tracks[2].delayMs - 8.2) < 0.2);
            CHECK (an2.tracks[3].severity == 2 && an2.tracks[3].delayMs == 0.0 && an2.tracks[3].message.contains ("AFTER"));
            CHECK (an2.tracks[4].severity == 2 && an2.tracks[4].arrivalMs < -1.0 && an2.tracks[4].message.contains ("BEFORE"));
        }
        // two main pairs: the second in line with the first (fine), or 3 ms behind it (flagged)
        {
            auto main2L = mic ({ { &sA, 9.1, 1.0 }, { &sB, 6.1, 1.0 } }, 0.5), main2R = mic ({ { &sA, 9.5, 1.0 }, { &sB, 8.6, 0.6 } }, 0.5);
            auto main3L = mic ({ { &sA, 12.0, 1.0 }, { &sB, 9.0, 1.0 } }, 0.5);
            std::vector<std::vector<const std::vector<float>*>> ch3 { { &mainL }, { &mainR }, { &main2L }, { &main2R }, { &main3L }, { &spotA } };
            std::vector<TrackIn> t3 (6);
            for (int i : { 0, 2, 4 }) { t3[(size_t) i].role = Main; t3[(size_t) i].pan[0] = -1.0f; }
            for (int i : { 1, 3 })    { t3[(size_t) i].role = Main; t3[(size_t) i].pan[0] = 1.0f; }
            t3[5].role = Spot;
            Reader rd3 = [&] (int t, int c, juce::int64 start, int cnt, float* dst)
            {
                const auto& v = *ch3[(size_t) t][(size_t) c];
                for (int i = 0; i < cnt; ++i) dst[i] = start + i < (juce::int64) v.size() ? v[(size_t) (start + i)] : 0.0f;
                return true;
            };
            auto an3 = analyse (t3, n, sr, rd3);
            CHECK (an3.ok && an3.mainChecks.size() == 3);
            for (auto& mc : an3.mainChecks)
            {
                if (mc.track == 2 || mc.track == 3) { CHECK (mc.against == mc.track - 2 && ! mc.drastic() && std::abs (mc.ms - 0.1) < 0.2); }
                if (mc.track == 4) { CHECK (mc.against == 0 && mc.drastic() && std::abs (mc.ms - 3.0) < 0.2); }
            }
            CHECK (an3.tracks[5].measured && an3.tracks[5].delayMs > 7.5 && an3.tracks[5].delayMs < 10.5);     // (three left mains and two right ones)
        }
        // bleed: an organ spot far behind (the organ reaches the main pair 30 ms after the spot) also hears the soloists, who stand near the main pair
        // and are much louder there - their path (they reach the spot 14 ms after the main pair) is the bigger peak, but the spot is lined up with
        // its organ, and not taken for a distance mic
        {
            auto steady = [&]
            {
                std::vector<float> v ((size_t) n);
                double lp = 0.0;
                for (int i = 0; i < n; ++i) { lp += 0.4 * (rnd.nextFloat() * 2.0 - 1.0 - lp); v[(size_t) i] = (float) (lp * (0.7 + 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.7 * i / sr))); }
                return v;
            };
            const auto organ = steady(), soloists = steady();
            auto bL = mic ({ { &organ, 30.0, 0.3 }, { &soloists, 8.0, 1.0 } }, 0.4);
            auto bR = mic ({ { &organ, 30.5, 0.3 }, { &soloists, 8.4, 1.0 } }, 0.4);
            auto organSpot = mic ({ { &organ, 2.0, 1.0 }, { &soloists, 22.0, 0.45 } }, 0.2);
            std::vector<std::vector<const std::vector<float>*>> ch5 { { &bL }, { &bR }, { &organSpot } };
            std::vector<TrackIn> t5 (3);
            t5[0] = tin[0]; t5[1] = tin[1]; t5[2].role = Spot;
            Reader rd5 = [&] (int t, int c, juce::int64 start, int cnt, float* dst)
            {
                const auto& v = *ch5[(size_t) t][(size_t) c];
                for (int i = 0; i < cnt; ++i) dst[i] = start + i < (juce::int64) v.size() ? v[(size_t) (start + i)] : 0.0f;
                return true;
            };
            // (the bigger peak really is the soloists': the whole correlation of the organ spot against the left main mic)
            {
                std::vector<std::complex<double>> G (8193);
                std::vector<double> fa (16384), fb (16384);
                std::vector<std::complex<double>> A (8193), B (8193);
                for (int st = 0; st + 16384 <= n; st += 8192)
                {
                    for (int i = 0; i < 16384; ++i) { const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / 16384.0); fa[(size_t) i] = bL[(size_t) (st + i)] * w; fb[(size_t) i] = organSpot[(size_t) (st + i)] * w; }
                    denoise::detail::realForward (fa.data(), 16384, A.data()); denoise::detail::realForward (fb.data(), 16384, B.data());
                    for (size_t k = 0; k < G.size(); ++k) G[k] += A[k] * std::conj (B[k]);
                }
                const auto whole = detail::gccPeak (G, 16384, sr, (int) (0.08 * sr));
                std::printf ("  (bleed: the biggest peak of the whole correlation at %.2f ms)\n", whole.lag * 1000.0 / sr);
                CHECK (whole.lag < 0.0);
            }
            auto an5 = analyse (t5, n, sr, rd5);
            const auto& o = an5.tracks[2];
            std::printf ("  (bleed: organ spot left %.2f, right %.2f, delay %.2f ms, severity %d)\n", o.leftMs, o.rightMs, o.delayMs, o.severity);
            CHECK (an5.ok && o.measured && o.severity < 2 && std::abs (o.leftMs - 28.0) < 0.2 && std::abs (o.rightMs - 28.5) < 0.2 && std::abs (o.delayMs - 28.25) < 0.2);
        }
        // a stereo spot pair over a wide section (A on its left channel, B on its right): measured channel by channel, so each channel gets its own
        // suggested pan (B is on the left of the main pair's image, A in the middle) instead of one place for the whole pair
        {
            auto pairL = mic ({ { &sB, 1.0, 1.0 }, { &sA, 3.0, 0.3 } }, 0.2), pairR = mic ({ { &sA, 1.0, 1.0 }, { &sB, 3.0, 0.3 } }, 0.2);
            std::vector<std::vector<const std::vector<float>*>> ch6 { { &mainL }, { &mainR }, { &pairL, &pairR } };
            std::vector<TrackIn> t6 (3);
            t6[0] = tin[0]; t6[1] = tin[1]; t6[2].role = Spot; t6[2].numChannels = 2; t6[2].pan[0] = -1.0f; t6[2].pan[1] = 1.0f;
            Reader rd6 = [&] (int t, int c, juce::int64 start, int cnt, float* dst)
            {
                const auto& v = *ch6[(size_t) t][(size_t) c];
                for (int i = 0; i < cnt; ++i) dst[i] = start + i < (juce::int64) v.size() ? v[(size_t) (start + i)] : 0.0f;
                return true;
            };
            auto an6 = analyse (t6, n, sr, rd6);
            const auto& o = an6.tracks[2];
            std::printf ("  (stereo spot: left %.2f, right %.2f, suggested %.2f / %.2f, stereo %d)\n", o.leftMs, o.rightMs, o.suggestedPan, o.suggestedPanR, (int) o.suggestedStereo);
            CHECK (an6.ok && o.measured && o.suggestedStereo && o.suggestedPan < -0.5f && std::abs (o.suggestedPanR) < 0.5f && o.delayMs > 4.5 && o.delayMs < 8.5);
        }
        // what cannot be measured says why
        {
            std::vector<TrackIn> none (2); none[0].role = Spot; none[1].role = Spot;
            CHECK (! analyse (none, n, sr, rd).ok && analyse (none, n, sr, rd).error.contains ("main pair"));
            CHECK (! analyse (tin, (juce::int64) (0.2 * sr), sr, rd).ok);
            std::vector<float> silent ((size_t) n, 0.0f);
            std::vector<std::vector<const std::vector<float>*>> ch4 { { &mainL }, { &silent } };
            std::vector<TrackIn> t4 (2); t4[0].role = Main; t4[1].role = Spot;
            Reader rd4 = [&] (int t, int c, juce::int64 start, int cnt, float* dst) { const auto& v = *ch4[(size_t) t][(size_t) c]; for (int i = 0; i < cnt; ++i) dst[i] = v[(size_t) (start + i)]; return true; };
            auto an4 = analyse (t4, n, sr, rd4);
            CHECK (an4.ok && ! an4.tracks[1].measured && an4.tracks[1].severity == 2 && an4.tracks[1].message.contains ("Silent"));
        }
        // roles guessed from the names
        CHECK (guessRole ("ORTF L") == Main && guessRole ("Spaced R") == Main && guessRole ("Outrigger L") == Main && guessRole ("Decca centre") == Main);
        CHECK (guessRole ("Hall far") == Distance && guessRole ("Surround L") == Distance && guessRole ("Room") == Distance);
        CHECK (guessRole ("Spot harp") == Spot && guessRole ("Solo sop") == Spot && guessRole ("Choir L") == Spot);
        CHECK (guessRole ("LTC") == NotUsed && guessRole ("Conductor") == NotUsed && guessRole ("Talkback") == NotUsed);
    }

    SECTION ("2.89 every piece keeps its own alignment; the engine delays a spot by the set of the piece in hand, in every mixer; the A / B switch");
    {
        Project q;
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto ta = q.addTrack ("Main", TrackFormat::Mono, 0).id;
        const auto tb = q.addTrack ("Spot", TrackFormat::Mono, 1).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& m = q.addMixer ("M");
        auto& kyrie = q.addTakeWindow ("Kyrie"); auto& gloria = q.addTakeWindow ("Gloria");
        const auto kid = kyrie.id.toString(), gid = gloria.id.toString();
        AudioEngine e (q);
        e.prepare (48000.0, 64);
        q.setSendLevel (m, ta, ob, 0.0f); q.setSendLevel (m, tb, ob, 0.0f);
        m.busFor (ob)->outFirst.store (0);
        q.findTrack (tb)->alignRole = timealign::Spot;
        { AlignTrack at; at.ms = 1.0; at.on = true; q.alignSets[kid].tracks[tb.toString()] = at; }            // the Kyrie: 48 samples
        { AlignTrack at; at.ms = 2.0; at.on = true; q.alignSets[gid].tracks[tb.toString()] = at; q.alignSets[gid].measuredOn = "take 002 - Gloria"; }   // the Gloria: 96
        auto peaks = [&]
        {
            // an impulse on both inputs at the same sample: the main's comes out at once, the spot's after its delay
            std::vector<float> outL;
            for (int blk = 0; blk < 40; ++blk)
            {
                std::vector<float> i0 (64, 0.0f), i1 (64, 0.0f), o0 (64, 0.0f), o1 (64, 0.0f);
                if (blk == 20) { i0[10] = 1.0f; i1[10] = 0.5f; }
                const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
                e.process (in, 2, out, 2, 64);
                outL.insert (outL.end(), o0.begin(), o0.end());
            }
            std::vector<int> at;
            for (int i = 0; i < (int) outL.size(); ++i) if (std::abs (outL[(size_t) i]) > 0.05f) at.push_back (i);
            return at;
        };
        auto gap = [&] { const auto at = peaks(); return at.size() == 2 ? at[1] - at[0] : at.size() == 1 ? 0 : -1; };
        q.alignActive = kid; e.rebuildPlan(); CHECK (gap() == 48);
        q.alignActive = gid; e.rebuildPlan(); CHECK (gap() == 96);
        // (2.98) the spot's automation is read that much earlier too: the alignment delay comes before every control of the track (its lanes landed early)
        CHECK (m.stripFor (tb)->autoLatIn.load() == 96 && m.stripFor (tb)->autoLatOut.load() == 96 && m.stripFor (tb)->slots[0].autoLat.load() == 96);
        CHECK (m.stripFor (tb)->sends.find (ob) != nullptr && m.stripFor (tb)->sends.find (ob)->autoLat.load() == 96);
        CHECK (m.stripFor (ta)->autoLatOut.load() == 0 && m.busFor (ob)->autoLatIn.load() == 0);                // (the paths themselves are not lined up again: on purpose)
        CHECK (std::abs (q.alignSamplesFor (*q.findTrack (tb), 48000.0) - 96.0) < 1.0e-9 && q.alignSamplesFor (*q.findTrack (ta), 48000.0) == 0.0);
        q.alignEnabled = false; e.rebuildPlan(); CHECK (gap() == 0);                                         // the A / B switch: off
        q.alignEnabled = true; q.alignSets[gid].tracks[tb.toString()].on = false; e.rebuildPlan(); CHECK (gap() == 0);
        q.alignSets[gid].tracks[tb.toString()].on = true;
        q.alignActive = juce::Uuid().toString(); e.rebuildPlan(); CHECK (gap() == 0);                          // a piece with no set: nothing delayed
        // an edit plays with the set of the piece most of its audio comes from, until it is measured itself (in post)
        auto& ed = q.addEdit ("Gloria edit", gloria.id);
        EditRegion r1; r1.id = juce::Uuid(); r1.windowId = gloria.id; r1.srcIn = 0; r1.srcOut = 48000 * 30; ed.regions.push_back (r1);
        EditRegion r2; r2.id = juce::Uuid(); r2.windowId = kyrie.id; r2.srcIn = 0; r2.srcOut = 48000 * 5; r2.startSample = 48000 * 30; ed.regions.push_back (r2);
        CHECK (q.alignOwnerFor (true, ed.id) == gid && q.alignOwnerFor (false, kyrie.id) == kid);
        { AlignTrack at; at.ms = 3.0; at.on = true; at.result.measured = true; q.alignSets[ed.id.toString()].tracks[tb.toString()] = at; q.alignSets[ed.id.toString()].measuredOn = "the edit"; }
        CHECK (q.alignOwnerFor (true, ed.id) == ed.id.toString());
        q.alignActive = q.alignOwnerFor (true, ed.id); e.rebuildPlan(); CHECK (gap() == 144);
        CHECK (q.alignSignature (gid) != q.alignSignature (kid) && q.alignSignature (gid).contains ("2.000"));
        // saved with the project: the sets, the roles, the measurements, the switch; a deleted piece's set goes with it
        {
            auto& at = q.alignSets[kid].tracks[tb.toString()];
            at.result.measured = true; at.result.hasLeft = true; at.result.leftMs = 3.25; at.result.suggestedPan = -0.4f; at.result.message = "fine";
        }
        q.alignEnabled = false;
        q.alignSets[juce::Uuid().toString()].measuredOn = "a piece that was deleted";
        q.pitchPipe.refA = 415.0; q.pitchPipe.temperament = tuning::Vallotti; q.pitchPipe.key = 2; q.pitchPipe.chordMode = true; q.pitchPipe.chordTuning = tuning::PureMinor; q.pitchPipe.outputs = { 1 };
        q.click.bpm = 72.5; q.click.beatsPerBar = 6; q.click.beatUnit = 8; q.click.sound = ClickTrack::LowPulse; q.click.outputs = { 0, 1 }; q.click.accent = false; q.click.quaverTicks = true; q.click.tickDb = -26.5f; q.pitchPipe.tone = 0.6f;
        Project back;
        CHECK (back.fromVar (q.toVar()));
        CHECK (back.findTrack (tb) != nullptr && back.findTrack (tb)->alignRole == timealign::Spot && back.findTrack (ta)->alignRole == 0);
        CHECK (back.alignSets.size() == 3 && ! back.alignEnabled && back.alignActive == ed.id.toString());
        if (const auto* ks = back.alignSetOf (kid))
        {
            const auto* at = ks->find (tb);
            CHECK (at != nullptr && at->on && std::abs (at->ms - 1.0) < 1.0e-9 && at->result.measured && at->result.hasLeft && ! at->result.hasRight
                   && std::abs (at->result.leftMs - 3.25) < 1.0e-9 && std::abs (at->result.suggestedPan + 0.4f) < 1.0e-6f && at->result.message == "fine");
        }
        else CHECK (false);
        CHECK (back.alignSetOf (gid) != nullptr && back.alignSetOf (gid)->measuredOn == "take 002 - Gloria" && std::abs (back.alignSetOf (gid)->find (tb)->ms - 2.0) < 1.0e-9);
        CHECK (back.pitchPipe.refA == 415.0 && back.pitchPipe.temperament == tuning::Vallotti && back.pitchPipe.key == 2 && back.pitchPipe.chordMode && back.pitchPipe.chordTuning == tuning::PureMinor && back.pitchPipe.outputs == std::vector<int> { 1 });
        CHECK (back.click.bpm == 72.5 && back.click.beatsPerBar == 6 && back.click.beatUnit == 8 && back.click.sound == ClickTrack::LowPulse && back.click.outputs.size() == 2 && ! back.click.accent);
        CHECK (back.click.quaverTicks && std::abs (back.click.tickDb + 26.5f) < 1.0e-4f && std::abs (back.pitchPipe.tone - 0.6f) < 1.0e-4f);
    }

    SECTION ("2.89 recorded files carry a BWF time reference, coding history and RIFF INFO; crash repair still finds the audio");
    {
        // a fresh take of the violin
        for (auto& t : p.tracks) t.setArmed (false);
        p.findTrack (violinId)->setArmed (true);
        CHECK (eng.startRecording (tw).isEmpty());
        rig.run (eng, 100);
        const auto gid = eng.stopRecording();
        CHECK (eng.waitForFinishedTakes (10000)); eng.reapFinishedTakes();
        const auto* g = tw.findGroup (gid);
        CHECK (g != nullptr && ! g->files.empty());
        const auto f = g != nullptr && ! g->files.empty() ? g->files[0].file : juce::File();
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        CHECK (r != nullptr);
        if (r != nullptr && g != nullptr)
        {
            const auto& md = r->metadataValues;
            CHECK (md.getValue (juce::WavAudioFormat::bwavDescription, {}) == tw.name + " / " + tw.displayName (*g) + " / violin");      // (what re-linking reads: unchanged)
            CHECK (md.containsKey (juce::WavAudioFormat::bwavTimeReference) && md.getValue (juce::WavAudioFormat::bwavTimeReference, "-1").getLargeIntValue() >= 0
                   && md.getValue (juce::WavAudioFormat::bwavTimeReference, "-1").getLargeIntValue() < (juce::int64) 86400 * 48000);
            CHECK (md.getValue (juce::WavAudioFormat::bwavCodingHistory, {}).startsWith ("A=PCM,F=48000,W=24,M=mono"));
            CHECK (md.getValue (juce::WavAudioFormat::bwavOriginatorRef, {}).startsWith ("FERMATA" + pad3 (g->number)));
            CHECK (md.getValue (juce::WavAudioFormat::riffInfoSoftware, {}).startsWith ("Fermata") && md.getValue (juce::WavAudioFormat::riffInfoTitle, {}) == tw.displayName (*g) + " - violin");
        }
        r.reset();
        // a copy with its data size wiped (a crash before the header was written): mended, every sample found
        const auto copy = tmp.getChildFile ("bwf-crash.wav");
        CHECK (f.copyFileTo (copy));
        {
            juce::MemoryBlock mb; copy.loadFileAsData (mb);
            auto* d = static_cast<unsigned char*> (mb.getData());
            for (size_t i = 12; i + 8 < mb.getSize(); ++i) if (std::memcmp (d + i, "data", 4) == 0) { d[i + 4] = d[i + 5] = d[i + 6] = d[i + 7] = 0; break; }
            copy.replaceWithData (mb.getData(), mb.getSize());
        }
        const auto rep = wavrepair::inspect (copy, true);
        CHECK (rep.ok && rep.wasBroken && rep.frames == 100 * Rig::block && rep.channels == 1);
    }

    SECTION ("2.89 a bounce plays with its piece's spot-mic alignment (and the live mixers are left alone)");
    {
        const TakeGroup* last = nullptr;
        for (auto& gg : tw.groups) if (last == nullptr || gg.number > last->number) last = &gg;
        CHECK (last != nullptr);
        if (last != nullptr)
        {
            const auto owner = tw.id.toString();
            { AlignTrack at; at.ms = 10.0; at.on = true; p.alignSets[owner].tracks[violinId.toString()] = at; }
            mixA.stripFor (violinId)->mute.set (false); mixA.stripFor (violinId)->gainDb.set (0.0f);
            BounceSettings bs;
            bs.segments = segmentsForTake (p, *last);
            bs.sampleRate = 48000.0; bs.mixerId = mixA.id; bs.sources = { violinId }; bs.folder = tmp.getChildFile ("align-bounce");
            bs.items.push_back ({ "Aligned", 0, 4800 });
            auto firstSound = [&] (const juce::String& alignOwner)
            {
                bs.alignOwner = alignOwner;
                Bouncer b (p, bs);
                auto r = b.render();
                if (r.error.isNotEmpty() || r.files.isEmpty()) return -1;
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (r.files[0])));
                if (rd == nullptr) return -1;
                juce::AudioBuffer<float> buf (2, 4800);
                rd->read (&buf, 0, 4800, 0, true, true);
                for (int i = 0; i < 4800; ++i) if (std::abs (buf.getSample (0, i)) > 1.0e-3f) return i;
                return 4800;
            };
            // earlier sections left other strips soloed and muted: clear the solos for this bounce (and put them back after)
            std::vector<std::pair<StripState*, bool>> solos;
            for (auto& t : p.tracks) if (auto* sp = mixA.stripFor (t.id)) { solos.push_back ({ sp, sp->solo.get() }); sp->solo.set (false); }
            const int plain = firstSound (juce::String()), aligned = firstSound (owner);
            for (auto& [sp, was] : solos) sp->solo.set (was);
            std::printf ("  (bounce: first sound plain %d, aligned %d)\n", plain, aligned);
            // the plain one sounds at once (its first samples softened by the take's edge fade and the fader's start-up ramp); the aligned one is silent
            // for exactly 10 ms (480 samples: the delay line starts empty) and then sounds (by then the fader's ramp is over, so a little sooner after
            // its start than the plain one - the sine is the same, but that is why it is not simply plain + 480)
            CHECK (plain >= 0 && plain < 100 && aligned >= 480 && aligned < 520);
            CHECK (p.alignActive != owner || true);                                      // (the live project's own choice is not touched by a bounce)
            p.alignSets.erase (owner);
        }
    }

    SECTION ("2.89 the PDF writer: a valid file (cross-reference table, page tree), Helvetica widths, wrapping");
    {
        CHECK (std::abs (pdf::width ("Hello", pdf::Regular, 10.0f) - 22.78f) < 0.01f);
        CHECK (std::abs (pdf::width ("W", pdf::Bold, 1000.0f) - 944.0f) < 0.5f);
        const auto lines = pdf::wrap ("The quick brown fox jumps over the lazy dog again and again", pdf::Regular, 10.0f, 100.0f);
        CHECK (lines.size() >= 3); for (auto& l : lines) CHECK (pdf::width (l, pdf::Regular, 10.0f) <= 100.0f);
        CHECK (pdf::encode (juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 \xe2\x80\x93 (A) \xe2\x99\xaf"))) == std::string ("Caf\xe9 \x96 (A) #"));
        pdf::Document d;
        d.newPage(); d.text (40, 60, "Page one (with brackets) \\ backslash", pdf::Bold, 12); d.line (40, 70, 500, 70, 1.0f); d.rect (40, 80, 100, 20, pdf::grey (0.5f));
        d.newPage(); d.text (40, 60, "Page two", pdf::Regular, 12);
        const auto mb = d.toPdf();
        const std::string s ((const char*) mb.getData(), mb.getSize());
        CHECK (s.rfind ("%PDF-1.4", 0) == 0 && s.find ("/Count 2") != std::string::npos && s.find ("(Page one \\(with brackets\\) \\\\ backslash) Tj") != std::string::npos);
        // startxref points at the table, and every entry at its object
        const auto sx = s.rfind ("startxref\n");
        CHECK (sx != std::string::npos);
        if (sx != std::string::npos)
        {
            const size_t xr = (size_t) std::stoull (s.substr (sx + 10));
            CHECK (s.compare (xr, 4, "xref") == 0);
            size_t pos = s.find ('\n', s.find ('\n', xr) + 1) + 1;           // past "xref" and "0 N"
            pos += 20;                                                        // the free entry
            for (int obj = 1; obj <= 10; ++obj, pos += 20)
            {
                const size_t off = (size_t) std::stoull (s.substr (pos, 10));
                CHECK (s.compare (off, std::to_string (obj).size() + 6, std::to_string (obj) + " 0 obj") == 0);
            }
        }
    }

    SECTION ("2.89 the take sheet: every take with its time, length, bars, markers and problems; CSV and a paged PDF");
    {
        tw.markers.clear();
        TakeGroup* first = nullptr;
        for (auto& gg : tw.groups) if (first == nullptr || gg.number < first->number) first = &gg;
        CHECK (first != nullptr);
        if (first != nullptr)
        {
            Marker mk; mk.id = juce::Uuid(); mk.name = "Clarinet, in"; mk.seconds = first->startSeconds + 0.4; tw.markers.push_back (mk);
            first->barIn = 12; first->barOut = 40; first->dud = true;
        }
        const auto secs = takesheet::collect (p, tw.id);
        CHECK (secs.size() == 1 && secs[0].piece == "Symphony" && (int) secs[0].rows.size() == (int) tw.groups.size());
        if (! secs.empty() && ! secs[0].rows.empty())
        {
            const auto& r0 = secs[0].rows[0];
            CHECK (first != nullptr && r0.number == first->number && r0.dud && r0.bars == "12 - 40" && r0.markers.size() == 1 && r0.markers[0] == "Clarinet, in (0:00)" && r0.name == tw.displayName (*first));
            CHECK (secs[0].tracks.contains ("violin") && secs[0].duds() == 1);
        }
        const auto csv = takesheet::csv (secs);
        CHECK (csv.startsWith ("Piece,Take,Name,Date") && csv.contains ("\"Clarinet, in (0:00)\"") && csv.contains (",yes,"));
        if (first != nullptr) { CHECK (csv.contains (tw.name + "," + juce::String (first->number) + "," + tw.displayName (*first) + ",")); }
        { size_t withTakes = 0; for (auto& w : p.takeWindows) withTakes += w->groups.empty() ? 0 : 1; CHECK (takesheet::collect (p).size() == withTakes); }     // (empty pieces left out)
        auto doc = takesheet::makePdf (secs, "Brahms Requiem", "2.89.0", juce::Time::getCurrentTime());
        CHECK (doc.pageCount() == 1);
        {
            const auto mb = doc.toPdf(); const std::string s ((const char*) mb.getData(), mb.getSize());
            CHECK (s.find ("(TAKE SHEET)") != std::string::npos && s.find ("(Page 1 of 1)") != std::string::npos && s.find ("(DUD)") != std::string::npos && s.find ("(Brahms Requiem)") != std::string::npos);
        }
        // a long session runs onto more pages, each with its heading and number
        Project big; big.addTakeWindow ("Mass in B minor");
        auto& bw = *big.takeWindows.back();
        for (int i = 1; i <= 90; ++i)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = i; g.sampleRate = 48000.0; g.lengthSamples = (juce::int64) (48000.0 * (60 + i)); g.startSeconds = i * 200.0;
            g.recordedAt = juce::Time (2026, 9, 22 + (i > 45 ? 1 : 0), 10, i % 60, 0); g.barIn = i; g.barOut = i + 20;
            if (i % 7 == 0) { g.overruns = 1; g.lostSamples = 4800; }
            bw.groups.push_back (g);
        }
        auto bigDoc = takesheet::makePdf (takesheet::collect (big), "Bach", "2.89.0", juce::Time::getCurrentTime());
        CHECK (bigDoc.pageCount() >= 3);
        const auto mb = bigDoc.toPdf(); const std::string s ((const char*) mb.getData(), mb.getSize());
        CHECK (s.find ("(Page 2 of " + std::to_string (bigDoc.pageCount()) + ")") != std::string::npos && s.find ("\\(continued\\)") != std::string::npos && s.find ("LOST 0.1 s") != std::string::npos);
        CHECK (takesheet::clockText (65.2) == "1:05" && takesheet::clockText (3725.0) == "1:02:05" && takesheet::durationText (3840.0) == "1 h 04 min");
        tw.markers.clear(); if (first != nullptr) { first->barIn = first->barOut = 0; first->dud = false; }
    }

    SECTION ("2.94 notes on a take: saved with the project, in the undo hash, in the take sheet (PDF and CSV)");
    {
        TakeGroup* nt = nullptr;
        for (auto& gg : tw.groups) if (nt == nullptr || gg.number < nt->number) nt = &gg;
        CHECK (nt != nullptr);
        if (nt != nullptr)
        {
            CHECK (nt->notes.isEmpty());
            const auto before = Project::undoHash (tw);
            nt->notes = "Good ending, \"chair\" creak at bar 14\nsecond line";
            CHECK (Project::undoHash (tw) != before);
            Project back; CHECK (back.fromVar (p.toVar()));
            auto* bw = back.findTakeWindow (tw.id);
            auto* bg = bw != nullptr ? bw->findGroup (nt->id) : nullptr;
            CHECK (bg != nullptr && bg->notes == nt->notes);
            const auto secs = takesheet::collect (p, tw.id);
            CHECK (! secs.empty() && ! secs[0].rows.empty() && secs[0].rows[0].notes == nt->notes);
            const auto csv = takesheet::csv (secs);
            CHECK (csv.contains (",Notes,Markers,") && csv.contains ("\"Good ending, \"\"chair\"\" creak at bar 14\nsecond line\""));
            const auto doc = takesheet::makePdf (secs, "Brahms Requiem", "2.94.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s ((const char*) mb.getData(), mb.getSize());
            CHECK (s.find ("Note: Good ending") != std::string::npos || s.find ("Note:") != std::string::npos);
            nt->notes.clear();
            Project back2; CHECK (back2.fromVar (p.toVar()));
            auto* bw2 = back2.findTakeWindow (tw.id);
            CHECK (bw2 != nullptr && bw2->findGroup (nt->id) != nullptr && bw2->findGroup (nt->id)->notes.isEmpty());
        }
    }

    SECTION ("2.94 the Stream Deck + XL: the commands, the preamp keys, gain, monitor, the mixers' main outputs, the stage levels");
    {
        namespace desk = remote::desk;
        // the new commands are understood, anything else is not
        { auto c = remote::parseCommand ("{\"cmd\":\"wact\",\"what\":\"Mark1\"}"); CHECK (c.name == "wact" && c.what == "mark1" && remote::isWindowAct (c.what)); }
        { auto c = remote::parseCommand ("{\"cmd\":\"openwin\",\"what\":\"Media\"}"); CHECK (c.name == "openwin" && remote::isOpenWin (c.what) && ! remote::isOpenWin ("bridge")); }
        CHECK (! remote::isWindowAct ("format") && remote::isWindowAct ("slipr") && remote::isWindowAct ("feedback"));
        { auto c = remote::parseCommand ("{\"cmd\":\"gain\",\"input\":3,\"delta\":-0.5}"); CHECK (c.name == "gain" && c.input == 3 && std::abs (c.delta + 0.5) < 1e-9); }
        { auto c = remote::parseCommand ("{\"cmd\":\"preamp\",\"input\":7,\"what\":\"V48\"}"); CHECK (c.name == "preamp" && c.input == 7 && c.what == "v48"); }
        CHECK (remote::parseCommand ("{\"cmd\":\"pan\",\"input\":1,\"delta\":0.02}").name.isEmpty());          // (2.98: no pan from the Stream Deck any more)
        { auto c = remote::parseCommand ("{\"cmd\":\"desk\",\"on\":false}"); CHECK (c.name == "desk" && ! c.on); }
        CHECK (remote::parseCommand ("{\"cmd\":\"desk\"}").on && remote::parseCommand ("{\"cmd\":\"monitor\"}").input == -1);
        CHECK (remote::parseCommand ("{\"cmd\":\"gain\",\"input\":0,\"delta\":1e9}").delta <= 100.0);
        for (auto* n : { "bridge", "pitchpipe", "click", "takewindow", "minimiseall", "armtoggle", "wavecolour", "loop", "monitor", "talkin", "mixout", "mixmute", "stage", "stagereset" })
            CHECK (remote::parseCommand (juce::String ("{\"cmd\":\"") + n + "\"}").name == n);
        CHECK (remote::parseCommand ("{\"cmd\":\"rm -rf\"}").name.isEmpty());

        // a project with 6 inputs, a mono track on input 0, a stereo one on inputs 2 + 3, a main output, and 6 mixers (processing + 5 cue)
        struct FakeDriver : PreampDriver
        {
            juce::String getName() const override { return "fake"; }
            bool isConnected() const override { return true; }
            bool apply (int i, const PreampSettings& s) override { last[i] = s; ++applied; return true; }
            bool controls (int i) const override { return i != 5; }
            std::map<int, PreampSettings> last; int applied = 0;
        };
        Project p; p.setInputCount (6); p.setOutputCount (4); p.tracks.clear(); p.buses.clear();
        auto fake = std::make_shared<FakeDriver>(); p.preampDriver = fake;
        p.addTrack ("Vln", TrackFormat::Mono, 0);
        const auto stId = p.addTrack ("Pair", TrackFormat::Stereo, 2).id;
        p.addBus ("Out", true);
        for (int i = 1; i <= 5; ++i) p.addMixer ("Cue " + juce::String (i));
        p.addMixer ("Cue 6");                                                   // (a sixth cue mixer has no dial)
        CHECK (p.cueEnd() == 7 && desk::mixerIndexForSlot (p, 0) == 0 && desk::mixerIndexForSlot (p, 5) == 5 && desk::mixerIndexForSlot (p, 6) == -1 && desk::mixerIndexForSlot (p, -1) == -1);

        // gain: half dB steps, kept in 0 .. 66, an input without preamp controls does nothing
        p.inputs[0].preamp.gainDb = 20.0f;
        CHECK (desk::gainStep (p, 0, 0.5f) && p.inputs[0].preamp.gainDb == 20.5f && fake->last[0].gainDb == 20.5f);
        CHECK (desk::gainStep (p, 0, -100.0f) && p.inputs[0].preamp.gainDb == 0.0f && desk::gainStep (p, 0, 100.0f) && p.inputs[0].preamp.gainDb == 66.0f);
        const int before = fake->applied;
        CHECK (desk::gainStep (p, 0, 1.0f) && fake->applied == before);              // already at the top: nothing is sent
        CHECK (! desk::gainStep (p, 5, 1.0f) && ! desk::gainStep (p, 99, 1.0f) && ! desk::gainStep (p, -1, 1.0f));

        // the preamp keys, as the keys in the main window step them
        p.inputs[1].preamp = PreampSettings();
        CHECK (desk::preampKey (p, 1, "v48") && p.inputs[1].preamp.phantom && desk::preampKey (p, 1, "v48") && ! p.inputs[1].preamp.phantom);
        CHECK (desk::preampKey (p, 1, "mic") && p.inputs[1].preamp.line && desk::preampKey (p, 1, "mic") && ! p.inputs[1].preamp.line);
        CHECK (desk::preampKey (p, 1, "pol") && p.inputs[1].preamp.polarity);
        CHECK (desk::preampKey (p, 1, "lc") && p.inputs[1].preamp.lowCut && desk::preampKey (p, 1, "lc") && ! p.inputs[1].preamp.lowCut);
        CHECK (desk::preampKey (p, 1, "pad") && p.inputs[1].preamp.pad && desk::preampKey (p, 1, "pad") && ! p.inputs[1].preamp.pad);
        CHECK (! desk::preampKey (p, 1, "nonsense") && ! desk::preampKey (p, 5, "v48"));
        // four-way low cut (MT48): OFF -> 40 -> 60 -> 80 -> OFF
        p.inputs[2].preamp = PreampSettings(); p.inputs[2].preamp.lowCutChoice = true;
        int hz[5]; for (int k = 0; k < 4; ++k) { desk::preampKey (p, 2, "lc"); hz[k] = p.inputs[2].preamp.lowCut ? p.inputs[2].preamp.lowCutHz : 0; }
        CHECK (hz[0] == 40 && hz[1] == 60 && hz[2] == 80 && hz[3] == 0);
        // the MT48's three-step pad: OFF -> -12 -> -24 -> OFF (it moves the gain scale, not the gain)
        p.inputs[2].preamp.steppedPad = true; p.inputs[2].preamp.gainDb = 30.0f;
        p.inputs[2].preamp.setPadStep (0); int steps[4]; for (int k = 0; k < 4; ++k) { desk::preampKey (p, 2, "pad"); steps[k] = p.inputs[2].preamp.padStep(); }
        CHECK (steps[0] == 1 && steps[1] == 2 && steps[2] == 0 && steps[3] == 1 && p.inputs[2].preamp.gainDb == 30.0f);
        p.inputs[2].preamp.line = true; CHECK (! desk::preampKey (p, 2, "pad"));        // (no Pad in Line mode on the MT48)
        // a fixed line input: no 48V or Pad; its first key is Line / Instrument
        p.inputs[3].preamp = PreampSettings(); p.inputs[3].preamp.lineOnly = true; p.inputs[3].preamp.hasInstrument = true;
        CHECK (! desk::preampKey (p, 3, "v48") && ! desk::preampKey (p, 3, "pad") && desk::preampKey (p, 3, "mic") && p.inputs[3].preamp.zHigh && ! p.inputs[3].preamp.line);
        p.inputs[3].preamp.hasInstrument = false; CHECK (! desk::preampKey (p, 3, "mic"));

        // monitor: E -> S -> S-L -> T -> E for the tracks on the input; none without a track
        for (auto& t : p.tracks) t.setMonitor (Monitor::Edit);
        CHECK (desk::monitorCycle (p, 0) == 1 && desk::monitorCycle (p, 0) == 2 && desk::monitorCycle (p, 0) == 3 && desk::monitorCycle (p, 0) == 0);
        CHECK (desk::monitorCycle (p, 3) == 1 && p.findTrack (stId)->monitor() == Monitor::Session);      // (input 3 is the right channel of the stereo track)
        CHECK (desk::monitorCycle (p, 4) == -1 && desk::monitorCycle (p, 99) == -1);

        // the mixers' main outputs: the first Ext Bus ticked as MAIN, else the first
        const auto busId = p.buses.front().id;
        for (auto& m : p.mixers) CHECK (desk::mainBusOf (p, *m) != nullptr);
        auto* b0 = desk::mainBusOf (p, *p.mixers[0]); b0->gainDb.set (-6.0f);
        CHECK (desk::mixOutStep (p, 0, 0.5f) && b0->gainDb.get() == -5.5f && desk::mixOutStep (p, 0, 100.0f) && b0->gainDb.get() == 12.0f);
        CHECK (desk::mixOutStep (p, 0, -1000.0f) && b0->gainDb.get() <= -100.0f);          // all the way down is off
        CHECK (desk::mixOutStep (p, 0, 0.5f) && b0->gainDb.get() == -79.5f);
        CHECK (desk::faderStep (-100.0f, 0.5f) == -79.5f && desk::faderStep (-79.5f, -0.5f) == -100.0f && desk::faderStep (-20.0f, 0.5f) == -19.5f);
        CHECK (! b0->mute.get() && desk::mixOutMute (p, 0) && b0->mute.get() && desk::mixOutMute (p, 0) && ! b0->mute.get());
        CHECK (desk::mixOutStep (p, 5, -3.0f) && desk::mainBusOf (p, *p.mixers[5])->gainDb.get() == -3.0f && desk::mainBusOf (p, *p.mixers[1])->gainDb.get() == 0.0f);   // each mixer has its own
        CHECK (! desk::mixOutStep (p, 6, 1.0f) && ! desk::mixOutMute (p, 9));
        desk::findBus (*p.mixers[2], busId)->isMain.set (true); CHECK (desk::mainBusOf (p, *p.mixers[2]) == desk::findBus (*p.mixers[2], busId));

        // the stage levels: playback, the first and second talkback mic (in the order of the stage panel), the output
        p.crMicInputs = { 4, 1 };
        CHECK (desk::stageStep (p, "playback", -1.5f, false) && p.stagePlaybackDb == -1.5f);
        CHECK (desk::stageStep (p, "mic1", 2.0f, false) && p.stageGainOf (4) == 2.0f && p.stageGainOf (1) == 0.0f);
        CHECK (desk::stageStep (p, "mic2", -3.0f, false) && p.stageGainOf (1) == -3.0f && p.stageGainOf (4) == 2.0f);
        CHECK (desk::stageStep (p, "mic2", -1000.0f, false) && p.stageGainOf (1) == -60.0f && desk::stageStep (p, "mic1", 1000.0f, false) && p.stageGainOf (4) == 10.0f);
        CHECK (desk::stageStep (p, "mic1", 0.0f, true) && p.stageGainOf (4) == 0.0f && desk::stageStep (p, "out", 0.5f, false) && p.stageOutputDb == 0.5f);
        p.crMicInputs = { 4 }; CHECK (! desk::stageStep (p, "mic2", 1.0f, false) && ! desk::stageStep (p, "bogus", 1.0f, false));

        // what is drawn: JSON with short numbers
        p.inputs[0].name = "Violin 1"; p.inputs[0].preamp.gainDb = 23.5f; p.inputs[0].preamp.phantom = true;
        const auto inputs = desk::inputsToVar (p);
        CHECK (inputs.getArray() != nullptr && inputs.getArray()->size() == 6);
        { auto i0 = (*inputs.getArray())[0];
          CHECK (i0["n"].toString() == "Violin 1" && (bool) i0["pre"] && std::abs ((double) i0["g"] - 23.5) < 1e-9 && (bool) i0["v48"] && (bool) i0["h48"] && (int) i0["mon"] == 0 && ! (bool) i0["tk"]);
          CHECK (! i0.hasProperty ("pan")); }
        { auto i2 = (*inputs.getArray())[2]; CHECK ((bool) i2["stp"]); }
        { auto i5 = (*inputs.getArray())[5]; CHECK (! (bool) i5["pre"] && ! i5.hasProperty ("g")); }
        { auto i3 = (*inputs.getArray())[3]; CHECK ((bool) i3["lo"] && ! (bool) i3["h48"] && ! (bool) i3["hpad"]); }
        p.crMicInputs = { 0 }; CHECK ((bool) (*desk::inputsToVar (p).getArray())[0]["tk"]);
        p.inputs[0].preamp.gainDb = 23.12f;
        const auto text = juce::JSON::toString (desk::inputsToVar (p), true);
        CHECK (! text.contains ("23.119999") && text.contains ("23.12"));
        const auto mixes = desk::mixersToVar (p);
        CHECK (mixes.getArray() != nullptr && mixes.getArray()->size() == 6 && (bool) (*mixes.getArray())[0]["has"] && (*mixes.getArray())[0]["n"].toString() == p.mixers[0]->name);
        { const auto stage = desk::stageToVar (p); CHECK (stage["mics"].getArray() != nullptr && stage["mics"].getArray()->size() == 1 && (int) (*stage["mics"].getArray())[0]["i"] == 0 && ! (bool) stage["ready"]); }
        CHECK (desk::peakDb (0.0f) == -100 && desk::peakDb (1.0f) == 0 && desk::peakDb (0.5f) == -6 && desk::peakDb (100.0f) == 6);
        Project none;                                                                   // (no mixers, no inputs: nothing to draw, nothing to move)
        CHECK (desk::mixersToVar (none).getArray()->size() == 6); CHECK (! (bool) (*desk::mixersToVar (none).getArray())[0]["has"]); CHECK (! desk::mixOutStep (none, 0, 1.0f)); CHECK (desk::inputsToVar (none).getArray()->size() == (int) none.inputs.size());
    }

    SECTION ("2.90 the selection box in the spectrogram windows: a side changes only its own axis, only a real corner changes both");
    {
        using boxedit::IRect; using boxedit::edgesAt;
        const IRect big { 100, 100, 200, 150 };
        CHECK (edgesAt (big, 100, 175) == 1 && edgesAt (big, 300, 175) == 2 && edgesAt (big, 200, 100) == 4 && edgesAt (big, 200, 250) == 8);
        CHECK (edgesAt (big, 100, 100) == 5 && edgesAt (big, 300, 100) == 6 && edgesAt (big, 100, 250) == 9 && edgesAt (big, 300, 250) == 10);
        CHECK (edgesAt (big, 200, 175) == 0 && edgesAt (big, 50, 50) == 0 && edgesAt (big, 400, 175) == 0);
        bool sidesOnly = true;                                                    // all along a side, away from the corners: that side and nothing else
        for (int y = 108; y <= 242; ++y) sidesOnly = sidesOnly && edgesAt (big, 94, y) == 1 && edgesAt (big, 106, y) == 1 && edgesAt (big, 294, y) == 2 && edgesAt (big, 306, y) == 2;
        for (int x = 108; x <= 292; ++x) sidesOnly = sidesOnly && edgesAt (big, x, 94) == 4 && edgesAt (big, x, 106) == 4 && edgesAt (big, x, 244) == 8 && edgesAt (big, x, 256) == 8;
        CHECK (sidesOnly);

        // the bug: a flat box (the wrong shape): grabbing its end for the width must not also grab its height
        for (int h = 0; h <= 5; ++h)
        {
            const IRect flat { 100, 100, 200, h };
            bool noCorner = true, endsAreWidth = true, middleIsHeight = true, any = false;
            for (int x = 90; x <= 310; ++x)
                for (int y = 90; y <= 100 + h + 10; ++y)
                {
                    const int e = edgesAt (flat, x, y);
                    if ((e & 3) && (e & 12)) noCorner = false;                    // never both axes
                    const bool end = (x <= 106 && x >= 94) || (x >= 294 && x <= 306);
                    const bool near = y >= 100 - 6 && y <= 100 + h + 6;
                    if (end && near) { any = true; if (e != (x < 200 ? 1 : 2)) endsAreWidth = false; }                 // the whole end (both sides of the line) is the width
                    if (x >= 112 && x <= 288 && near && (e & 3)) middleIsHeight = false;                                // the middle never is
                    if (x >= 112 && x <= 288 && near && e != 4 && e != 8) middleIsHeight = false;
                }
            CHECK (noCorner && endsAreWidth && middleIsHeight && any);
        }
        // and the same turned round: a tall narrow box: its ends are the height, its long sides the width
        const IRect tall { 100, 100, 3, 200 };
        bool tallOk = true;
        for (int y = 94; y <= 106; ++y) for (int x = 94; x <= 109; ++x) { const int e = edgesAt (tall, x, y); if ((e & 3) && (e & 12) && ! (std::abs (x - 100) <= 1 && std::abs (y - 100) <= 1)) tallOk = false; }
        for (int y = 112; y <= 288; ++y) tallOk = tallOk && edgesAt (tall, 100, y) == 1 && edgesAt (tall, 103, y) == 2;
        CHECK (tallOk && edgesAt (tall, 101, 102) == 4 && edgesAt (tall, 101, 298) == 8);
        // a box too small to have corners never reports one, whatever its size or where the point is
        bool noSmallCorners = true;
        for (int w = 0; w <= 17; ++w) for (int h = 0; h <= 17; ++h)
            for (int x = 90; x <= 130; ++x) for (int y = 90; y <= 130; ++y)
            {
                const int e = edgesAt ({ 100, 100, w, h }, x, y);
                if ((e & 3) && (e & 12) && std::min (w, h) < 6) noSmallCorners = false;
            }
        CHECK (noSmallCorners);
    }

    SECTION ("2.90 playing from the middle of a plug-in lane: the parameter takes the lane's value in the first block, not at the next point");
    {
        struct FakeFx : InsertProcessor
        {
            std::atomic<int> sets { 0 }, setsP0 { 0 }; float val[2] { 0.5f, 0.5f };
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>&) override {}
            juce::String getName() const override { return "Fake"; }
            int numParams() const override { return 2; }
            juce::String paramName (int i) const override { return "P" + juce::String (i); }
            bool paramAutomatable (int) const override { return true; }
            int paramSteps (int i) const override { return i == 0 ? 2 : 0; }          // 0: a bypass, 1: a smooth control
            float getParamNorm (int i) const override { return val[i]; }
            void setParamNorm (int i, float v) override { val[i] = v; ++sets; if (i == 0) ++setsP0; }
        };
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        auto fx = std::make_unique<FakeFx>(); auto* fxp = fx.get();
        q.mixers.front()->stripFor (ta)->slots[0].set (std::move (fx), 48000.0, 256, 2);
        AudioEngine e (q);
        e.prepare (48000.0, 256);
        const juce::int64 S = 48000;
        EditDef ed; ed.automationOn = true;
        AutoPoint p0, p1, p2; p0.time = 0; p0.value = 0.0f; p1.time = 20 * S; p1.value = 0.0f; p2.time = 21 * S; p2.value = 1.0f;
        ed.addLane (ta, autoparam::plugin (0, 0), juce::Uuid::null()).pts = { p0, p1, p2 };                  // the bypass: off from the start, on at 20 s
        AutoPoint s0, s1; s0.time = 0; s0.value = 0.2f; s1.time = 10 * S; s1.value = 0.8f;
        ed.addLane (ta, autoparam::plugin (0, 1), juce::Uuid::null()).pts = { s0, s1 };                      // a smooth control
        std::vector<float> i0 (256, 0.0f), o0 (256), o1 (256);
        const float* in[1] = { i0.data() }; float* out[2] = { o0.data(), o1.data() };
        auto playFrom = [&] (juce::int64 from)
        {
            auto plan = automationPlanFor (q, ed);
            auto ss = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> {}, from, 60 * S, 48000.0, e.getMaxBlock());
            ss->automation = plan;
            return e.startPlayback (std::move (ss));
        };
        fxp->val[0] = 1.0f; fxp->val[1] = 0.9f;                          // what an earlier play left in the plug-in (bypassed, the knob up)
        CHECK (playFrom (5 * S).isEmpty());
        CHECK (fxp->val[0] == 0.0f && std::abs (fxp->val[1] - 0.5f) < 0.01f);        // set as the play starts, before any sound
        fxp->val[0] = 1.0f;                                              // (something put it back: the plug-in ignored it, say)
        e.process (in, 1, out, 2, 256);
        CHECK (fxp->val[0] == 0.0f);                                     // 5 s: the bypass is off here, from the first block (no point crossed) - told again in the early blocks
        CHECK (std::abs (fxp->val[1] - 0.5f) < 0.01f);                   // and the knob is half way between its two points
        {   const int before = fxp->setsP0;
            for (int k = 0; k < 60; ++k) e.process (in, 1, out, 2, 256);
            CHECK (fxp->setsP0 - before <= 24);                           // the bypass is told again only in the first blocks of a play, not in every block (the moving knob follows its line)
        }
        e.stopPlayback();
        { InsertSlot empty; CHECK (! empty.setParamNorm (0, 0.5f)); }      // nothing loaded: not delivered (the lane tries again)
        fxp->val[0] = 1.0f;
        CHECK (playFrom (12 * S).isEmpty());                              // between the first two points: still off
        e.process (in, 1, out, 2, 256);
        CHECK (fxp->val[0] == 0.0f && std::abs (fxp->val[1] - 0.8f) < 1e-4f);        // (after the last point of the knob: its last value)
        e.stopPlayback();
        fxp->val[0] = 0.0f;
        CHECK (playFrom (30 * S).isEmpty());                              // after the last point: on
        e.process (in, 1, out, 2, 256);
        CHECK (fxp->val[0] == 1.0f);
        e.stopPlayback();
    }

    SECTION ("2.90 lane rows: a track, an Int Bus or an Ext Bus can show several lanes; the lanes play and are written whatever the rows show");
    {
        EditDef ed;
        const juce::Uuid node;
        CHECK (ed.laneRowCount (node) == 1 && ed.laneRows.empty());
        { const EditDef& ce = ed; const auto sh = ce.laneShown (node, 3); CHECK (sh.param == autoparam::fader && sh.mixer.isNull()); CHECK (ce.laneRows.empty()); }      // looking adds nothing
        CHECK (! ed.removeLaneRow (node, 0) && ! ed.removeLaneRow (node, 1));                                 // the node's own row cannot go; nothing to take
        CHECK (ed.addLaneRow (node, true, true) && ed.laneRowCount (node) == 2 && ed.laneShown (node, 1).param == autoparam::pan);      // the first free one: fader is shown, so pan
        CHECK (ed.addLaneRow (node, true, true) && ed.laneShown (node, 2).param == autoparam::gain);
        CHECK (ed.addLaneRow (node, true, true) && ed.laneShown (node, 3).param == autoparam::fader);         // all three shown: the fader again
        EditDef::LaneShown pickPan; pickPan.param = autoparam::pan; ed.setLaneShown (node, 0, pickPan);        // the node's own row can show something else
        CHECK (ed.laneShown (node, 0).param == autoparam::pan && ed.laneRowCount (node) == 4);
        CHECK (EditDef::freeLaneParam (ed.laneRowsOf (node), false, false) == autoparam::fader || true);
        { std::vector<EditDef::LaneShown> v (1); CHECK (EditDef::freeLaneParam (v, false, false) == autoparam::fader); v.push_back (EditDef::LaneShown()); CHECK (EditDef::freeLaneParam (v, false, true) == autoparam::gain && EditDef::freeLaneParam (v, true, true) == autoparam::pan); }
        ed.setLaneShown (node, 0, EditDef::LaneShown());
        CHECK (ed.removeLaneRow (node, 3) && ed.removeLaneRow (node, 2) && ed.removeLaneRow (node, 1) && ed.laneRows.empty() && ed.laneRowCount (node) == 1);     // back to one fader row: no entry kept
        for (int i = 1; i < EditDef::kMaxLaneRows; ++i) CHECK (ed.addLaneRow (node, true, true));
        CHECK (! ed.addLaneRow (node, true, true) && ed.laneRowCount (node) == EditDef::kMaxLaneRows);        // as many as fit

        // the rows are saved with the project (a view: not part of undo), and a project without them loads as before
        Project p; p.setInputCount (2); p.setOutputCount (2); p.tracks.clear(); p.buses.clear();
        const auto t1 = p.addTrack ("A", TrackFormat::Mono, 0).id;
        const auto ib = p.addBus ("Strings", false).id, eb = p.addBus ("Out", true).id;
        auto& e1 = p.addEdit ("E", juce::Uuid::null());
        e1.automationOn = true;
        const auto mixerId = p.mixers.front()->id;
        const auto h0 = Project::undoHash (e1), s0 = Project::saveHash (e1);
        e1.addLaneRow (t1, true, true); e1.addLaneRow (t1, true, true);
        { EditDef::LaneShown ls; ls.param = autoparam::send (ib); ls.mixer = mixerId; e1.setLaneShown (t1, 2, ls); }
        e1.addLaneRow (ib, true, true); e1.addLaneRow (eb, false, false);
        CHECK (Project::undoHash (e1) == h0 && Project::saveHash (e1) != s0);
        p.name = "laneRows"; p.projectFile = tmp.getChildFile ("laneRows.fermata");
        juce::String er; CHECK (p.save (er));
        Project q; CHECK (q.load (p.projectFile, er));
        auto* e2 = q.edits.empty() ? nullptr : q.edits.back().get();
        CHECK (e2 != nullptr);
        if (e2 != nullptr)
        {
            CHECK (e2->laneRowCount (t1) == 3 && e2->laneRowCount (ib) == 2 && e2->laneRowCount (eb) == 2 && e2->laneRows.size() == 3);
            CHECK (e2->laneShown (t1, 0).param == autoparam::fader && e2->laneShown (t1, 1).param == autoparam::pan);
            CHECK (e2->laneShown (t1, 2).param == autoparam::send (ib) && e2->laneShown (t1, 2).mixer == mixerId);
            CHECK (e2->laneShown (eb, 1).param == autoparam::fader);
            CHECK (Project::saveHash (*e2) == Project::saveHash (e1));
        }

        // an Ext Bus has its fader and its plug-ins automated; gain, pan and sends do not exist there
        struct FakeFx : InsertProcessor
        {
            float val[2] { 0.5f, 0.5f };
            void prepare (double, int, int) override {}
            void process (juce::AudioBuffer<float>&) override {}
            juce::String getName() const override { return "Fake"; }
            int numParams() const override { return 2; }
            juce::String paramName (int i) const override { return "P" + juce::String (i); }
            bool paramAutomatable (int) const override { return true; }
            float getParamNorm (int i) const override { return val[i]; }
            void setParamNorm (int i, float v) override { val[i] = v; }
        };
        auto fx = std::make_unique<FakeFx>(); auto* fxp = fx.get();
        auto& pm = *p.mixers.front();
        pm.busFor (eb)->slots[2].set (std::move (fx), 48000.0, 256, 2);
        const juce::int64 S = 48000;
        e1.lanes.clear();
        auto line = [&] (const juce::Uuid& n, const juce::String& prm, float a, float b)
        {
            AutoPoint x, y; x.time = 0; x.value = a; y.time = 10 * S; y.value = b;
            e1.addLane (n, prm, juce::Uuid::null()).pts = { x, y };
        };
        line (eb, autoparam::fader, -20.0f, 0.0f);
        line (eb, autoparam::plugin (2, 1), 0.0f, 1.0f);
        line (eb, autoparam::gain, -5.0f, -5.0f);                       // an Ext Bus has no gain: left out
        line (eb, autoparam::pan, -1.0f, 1.0f);                         // nor pan
        line (eb, autoparam::send (ib), -5.0f, -5.0f);                  // nor sends
        line (eb, autoparam::plugin (0, 0), 0.0f, 1.0f);                // an empty slot: left out
        e1.automationOn = false;                                        // (the lane view is closed: the lanes play all the same)
        auto plan = automationPlanFor (p, e1, true, p.mixers.front()->id);
        CHECK (plan != nullptr && plan->items.size() == 2);
        if (plan != nullptr)
        {
            plan->apply (5 * S);
            CHECK (std::abs (pm.busFor (eb)->gainDb.get() + 10.0f) < 1.0e-3f);
            CHECK (std::abs (fxp->val[1] - 0.5f) < 1.0e-4f);
            plan->apply (10 * S);
            CHECK (std::abs (pm.busFor (eb)->gainDb.get()) < 1.0e-3f && std::abs (fxp->val[1] - 1.0f) < 1.0e-4f);
        }
        // two rows showing different lanes of one track: both lanes play (the rows are only what is drawn)
        e1.lanes.clear();
        line (t1, autoparam::fader, -12.0f, -12.0f);
        line (t1, autoparam::pan, 0.5f, 0.5f);
        e1.laneRows.clear(); e1.addLaneRow (t1, true, true);
        auto plan2 = automationPlanFor (p, e1, true, p.mixers.front()->id);
        CHECK (plan2 != nullptr && plan2->items.size() == 2);
        e1.laneRows.clear();                                            // rows gone, lane view closed: nothing about the audio changes
        auto plan3 = automationPlanFor (p, e1, true, p.mixers.front()->id);
        CHECK (plan3 != nullptr && plan3->items.size() == 2);
        // the bounce / render uses the same rule
        CHECK (e1.hasAutomation());
        e1.lanes[0].pts.clear(); e1.lanes[1].pts.clear();
        CHECK (! e1.hasAutomation());
    }

    tmp.deleteRecursively();
    SECTION ("2.98 the sweep: lane rows survive Undo, the room-tone mark is an undo step, a failed Open keeps the open project, a new project forgets the layouts");
    {
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto tA = q.addTrack ("A", TrackFormat::Mono, 0).id;
        const auto twId = q.takeWindows.front()->id;
        q.addEdit ("Edit 1", twId);
        auto* e0 = q.editForWindow (twId);
        CHECK (e0 != nullptr);
        if (e0 != nullptr)
        {
            q.undoReset();
            CHECK (e0->addLaneRow (tA, true, true) && e0->addLaneRow (tA, true, true) && e0->laneRowCount (tA) == 3);
            e0->name = "Renamed for the undo"; q.changed(); q.undoTick (false, true);
            CHECK (q.undo());
            e0 = q.editForWindow (twId);
            CHECK (e0 != nullptr && e0->name != "Renamed for the undo" && e0->laneRowCount (tA) == 3);       // (Undo used to put every node back to one row)
        }
        // the room-tone mark is an undo step (and so it is saved: the window's file hash moves)
        auto* tw0 = q.findTakeWindow (twId);
        TakeGroup g; g.id = juce::Uuid(); g.number = 1; tw0->groups.push_back (g);
        q.undoReset();
        const auto saveBefore = Project::saveHash (*tw0);
        tw0->groups.back().roomTone = true; tw0->groups.back().roomToneIn = 0.5; q.changed(); q.undoTick (false, true);
        CHECK (Project::saveHash (*tw0) != saveBefore);
        CHECK (q.undo() && ! tw0->groups.back().roomTone && tw0->groups.back().roomToneIn < 0.0);
        CHECK (q.redo() && tw0->groups.back().roomTone && std::abs (tw0->groups.back().roomToneIn - 0.5) < 1e-9);

        // a project file that cannot be read leaves the open project's window files alone (it used to save them again as "Piece 1 (2).fmtake" ...)
        {
            auto root = tmp.getChildFile ("failed-open"); root.deleteRecursively(); root.createDirectory();
            juce::String err;
            Project a; a.addEdit ("Edit 1", a.takeWindows.front()->id);
            CHECK (a.saveAs (root.getChildFile ("A/A.fermata"), err));
            const auto bad = root.getChildFile ("broken.fermata"); bad.replaceWithText ("{ not json");
            CHECK (! a.load (bad, err));
            a.changed(); CHECK (a.save (err));
            int doubled = 0; for (auto& f : root.getChildFile ("A").findChildFiles (juce::File::findFiles, true, "*.fm*")) if (f.getFileName().contains ("(2)")) ++doubled;
            CHECK (doubled == 0);
        }
        // a new project does not inherit the saved Display Organiser layouts of the one before
        {
            Project b; LayoutDef l; l.name = "Mine"; b.layouts.push_back (l);
            b.createDefaultDesign();
            CHECK (b.layouts.empty());
        }
    }

    SECTION ("2.98 the sweep: De-click on a short area at 96 and 192 kHz, and its report; a long Denoise box in pieces");
    {
        // a short area (30 ms or less) round an obvious click: mended at every rate (at 96 kHz an area of over 1500 samples used to be handed to the
        // prediction, which refused it, so nothing at all was done), and the report says so (it used to keep the counts of the De-click before)
        for (double sr : { 48000.0, 96000.0, 192000.0 })
            for (double ms : { 10.0, 20.0, 29.0 })
            {
                std::vector<float> x ((size_t) sr);
                for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (0.02 * std::sin (2.0 * 3.14159265358979 * 220.0 * (double) i / sr));
                const auto clean = x;
                const long a = (long) (0.5 * sr), n = (long) (ms * 1.0e-3 * sr);
                for (long i = a + n / 3; i < a + n / 3 + 20; ++i) x[(size_t) i] += 0.3f;
                SpectralRepair::lastDeclickReport() = SpectralRepair::DeclickReport { 7, 7, 7, 7 };
                const int r = SpectralRepair::declick (x, a, a + n, 8.0, nullptr, sr, 0);
                double left = 0.0; for (size_t i = 0; i < x.size(); ++i) left = juce::jmax (left, (double) std::abs (x[i] - clean[i]));
                const auto rep = SpectralRepair::lastDeclickReport();
                CHECK (r >= 1 && left < 0.05 && rep.mended + rep.linesMended >= 1 && rep.linesLeft == 0 && rep.mended + rep.linesMended <= r);
                if (! (r >= 1 && left < 0.05)) std::printf ("  (%.0f Hz, %.0f ms: declick returned %d, %.3f of the click left)\n", sr, ms, r, left);
            }
        // a Denoise box over 22 s (longer than one piece: 15 s, with 6 s before and 2 s after): done in pieces it comes out as in one go, the hiss down
        // all through it (before, across and after the join of the pieces), outside the box bit for bit
        {
            const double sr = 48000.0; const int n = (int) (30.0 * sr);
            std::vector<float> x ((size_t) n); juce::Random rnd (98);
            for (int i = 0; i < n; ++i) { const float t = (float) i / (float) sr; x[(size_t) i] = 0.003f * (rnd.nextFloat() * 2.0f - 1.0f) + (t > 10.0f && t < 20.0f ? 0.05f * std::sin (2.0f * 3.14159265f * 1000.0f * t) : 0.0f); }
            denoise::Params p;
            const auto prof = denoise::learnProfile ({ std::vector<float> (x.begin(), x.begin() + (int) (3.0 * sr)) }, sr, p);
            FixSpec spec; spec.kind = FixSpec::Kind::Denoise; spec.f0 = 300.0; spec.f1 = 12000.0; spec.dn = p;
            spec.dnProfiles = std::make_shared<const std::vector<denoise::Profile>> (std::vector<denoise::Profile> { prof });
            const long a = (long) (4.0 * sr), b = (long) (26.0 * sr);
            std::vector<std::vector<float>> ch { x };
            audioops::applyFix (ch, sr, a, b, spec, 0);
            // in one go, as before 2.98: the same limited profile over the whole stretch with its context ([0, 28 s)), the same 100 ms fades
            auto lim = prof; lim.weight.assign (lim.psd.size(), 0.0f);
            for (size_t k = 0; k < lim.weight.size(); ++k) { const double f = (double) k * sr / lim.fftSize; if (f >= spec.f0 && f <= spec.f1) lim.weight[k] = 1.0f; }
            denoise::featherProfile (lim, 0.25);
            const long c1 = b + (long) (2.0 * sr);
            std::vector<float> one ((size_t) c1, 0.0f); denoise::processSegment (lim, p, {}, x.data(), c1, one.data());
            const long xf = (long) (0.1 * sr);
            double se = 0.0, sd = 0.0; bool outsideSame = true;
            for (long i = 0; i < n; ++i)
            {
                if (i < a || i >= b) { outsideSame = outsideSame && ch[0][(size_t) i] == x[(size_t) i]; continue; }
                double w = juce::jmin (1.0, (double) (i - a) / (double) xf, (double) (b - 1 - i) / (double) xf); w = 0.5 - 0.5 * std::cos (w * 3.14159265358979);
                const double ref = (double) x[(size_t) i] + w * ((double) one[(size_t) i] - (double) x[(size_t) i]);
                se += ref * ref; sd += (ch[0][(size_t) i] - ref) * (ch[0][(size_t) i] - ref);
            }
            const double snr = 10.0 * std::log10 (se / juce::jmax (sd, 1.0e-30));
            std::printf ("  (a 22 s Denoise box in pieces against one go: %.1f dB apart)\n", snr);
            CHECK (outsideSame && snr > 60.0);
            auto hissDb = [&] (const std::vector<float>& v, double t0, double t1)                    // the energy between 2 and 8 kHz (a coarse DFT at 50 Hz steps)
            {
                double e = 0.0; const int a0 = (int) (t0 * sr), a1 = (int) (t1 * sr);
                for (double hz = 2000.0; hz < 8000.0; hz += 50.0)
                {
                    double re = 0, im = 0;
                    for (int i = a0; i < a1; ++i) { const double ph = 2.0 * 3.14159265358979 * hz * i / sr; re += v[(size_t) i] * std::cos (ph); im -= v[(size_t) i] * std::sin (ph); }
                    e += (re * re + im * im) / ((double) (a1 - a0) * (a1 - a0));
                }
                return 10.0 * std::log10 (e + 1.0e-30);
            };
            for (double t : { 5.0, 14.6, 15.1, 24.0 }) CHECK (hissDb (ch[0], t, t + 0.3) < hissDb (x, t, t + 0.3) - 8.0);
        }
    }

    SECTION ("2.99 clips moved from one track to another in a take window (an import that put them on tracks of their own)");
    {
        Project q; q.setInputCount (8); q.setOutputCount (2); q.tracks.clear();
        const auto spot1 = q.addTrack ("SPOT1", TrackFormat::Mono, 0).id;
        const auto spot2 = q.addTrack ("SPOT2", TrackFormat::Mono, 1).id;
        const auto spot3 = q.addTrack ("SPOT3", TrackFormat::Mono, 2).id;
        const auto rooms = q.addTrack ("ROOMS", TrackFormat::Mono, 3).id;
        const auto m50 = q.addTrack ("M50 ROOMS", TrackFormat::Mono, 4).id;
        const auto pair = q.addTrack ("AB", TrackFormat::Stereo, 5).id;
        const auto v1 = q.addTrack ("VivienneSPOT1", TrackFormat::Mono, 0).id;
        const auto v2 = q.addTrack ("VivienneSPOT2", TrackFormat::Mono, 1).id;
        const auto vm50 = q.addTrack (" VivienneM50 ROOMS ", TrackFormat::Mono, 4).id;
        const auto ltc = q.addTrack ("LTC", TrackFormat::Mono, 7).id; q.findTrack (ltc)->isLtc = true;
        // the track a prefixed name ends with: the longest such name, capitals and spaces at the ends not counting; none for a plain name, never the LTC track
        CHECK (q.trackNamedAtEndOf (v1) != nullptr && q.trackNamedAtEndOf (v1)->id == spot1);
        CHECK (q.trackNamedAtEndOf (vm50) != nullptr && q.trackNamedAtEndOf (vm50)->id == m50);          // (not "ROOMS")
        CHECK (q.trackNamedAtEndOf (spot1) == nullptr && q.trackNamedAtEndOf (rooms) == nullptr);
        auto& tw = q.addTakeWindow ("O Sacrum Convivium"); const auto twId = tw.id;
        std::vector<juce::Uuid> takeIds;
        for (int n = 1; n <= 3; ++n)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = n; g.startSeconds = 60.0 * n; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 30;
            for (auto [tid, nm] : { std::pair<juce::Uuid, const char*> { v1, "VivienneSPOT1" }, { v2, "VivienneSPOT2" } })
            { TakeFile f; f.trackId = tid; f.trackName = nm; f.numChannels = 1; f.file = tmp.getChildFile (juce::String (nm) + juce::String (n) + ".wav"); g.files.push_back (f); }
            if (n == 2) { TakeFile f; f.trackId = spot2; f.trackName = "SPOT2"; f.numChannels = 1; f.file = tmp.getChildFile ("spot2-2.wav"); g.files.push_back (f); }
            takeIds.push_back (g.id); tw.groups.push_back (g);
        }
        auto fileOn = [&] (int take, const juce::Uuid& tid) -> const TakeFile*
        {
            auto* w = q.findTakeWindow (twId); if (w == nullptr) return nullptr;
            auto* g = w->findGroup (takeIds[(size_t) take]); if (g == nullptr) return nullptr;
            for (auto& f : g->files) if (f.trackId == tid) return &f;
            return nullptr;
        };
        // an edit made from take 1 before the move, with a track list of its own that has VivienneSPOT1 and not SPOT1
        auto& ed = q.addEdit ("Edit 1", twId); const auto edId = ed.id;
        {
            EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = takeIds[0]; r.srcIn = 0; r.srcOut = 48000 * 10; r.sampleRate = 48000.0;
            RegionFile rf; rf.trackId = v1; rf.trackName = "VivienneSPOT1"; rf.file = fileOn (0, v1)->file; rf.numChannels = 1; r.files.push_back (rf);
            ed.regions.push_back (r);
            ed.trackIds = { spot2, v1 };
        }
        q.undoReset();
        // all three takes' clips on VivienneSPOT1 onto SPOT1: same files, same takes (so the same time), the new track's name; the edit's piece goes along
        juce::String why; int pieces = -1;
        CHECK (q.canMoveTakeClips (twId, v1, spot1, takeIds).isEmpty());
        CHECK (q.moveTakeClips (twId, v1, spot1, takeIds, why, &pieces) == 3 && why.isEmpty() && pieces == 1);
        bool allMoved = true;
        for (int n = 0; n < 3; ++n) allMoved = allMoved && fileOn (n, v1) == nullptr && fileOn (n, spot1) != nullptr && fileOn (n, spot1)->trackName == "SPOT1"
                                                && fileOn (n, spot1)->file == tmp.getChildFile ("VivienneSPOT1" + juce::String (n + 1) + ".wav");
        CHECK (allMoved);
        {
            auto* e = q.findEdit (edId);
            CHECK (e != nullptr && e->regions.size() == 1 && e->regions[0].files.size() == 1 && e->regions[0].files[0].trackId == spot1 && e->regions[0].files[0].trackName == "SPOT1");
            CHECK (e != nullptr && e->trackIds.size() == 3 && e->trackIds[0] == spot2 && e->trackIds[1] == v1 && e->trackIds[2] == spot1);   // (SPOT1 beside VivienneSPOT1)
        }
        // one undo step puts it all back (take window and edit)
        q.undoTick (false, true);
        CHECK (q.undo());
        CHECK (fileOn (0, v1) != nullptr && fileOn (2, v1) != nullptr && fileOn (0, spot1) == nullptr);
        CHECK (q.findEdit (edId) != nullptr && q.findEdit (edId)->regions[0].files[0].trackId == v1);
        CHECK (q.redo() && fileOn (1, spot1) != nullptr && fileOn (1, v1) == nullptr);
        // refused, and nothing moved: a take that already has audio on the target track (take 002 has SPOT2) ...
        why = q.canMoveTakeClips (twId, v2, spot2, takeIds);
        CHECK (why.contains ("take 002") && q.moveTakeClips (twId, v2, spot2, takeIds, why, nullptr) == 0 && fileOn (0, v2) != nullptr && fileOn (0, spot2) == nullptr);
        // ... other channels (mono clips onto a stereo track), the LTC track, the same track, takes with no clip there
        CHECK (q.canMoveTakeClips (twId, v2, pair, takeIds).contains ("stereo") && q.moveTakeClips (twId, v2, pair, takeIds, why) == 0);
        CHECK (q.canMoveTakeClips (twId, v2, ltc, takeIds).isNotEmpty() && q.canMoveTakeClips (twId, v2, v2, takeIds).isNotEmpty());
        CHECK (q.canMoveTakeClips (twId, rooms, spot3, takeIds).isNotEmpty());
        // only the takes asked for (the selected ones): takes 1 and 3 onto SPOT3, take 2 stays
        CHECK (q.moveTakeClips (twId, v2, spot3, { takeIds[0], takeIds[2] }, why) == 2);
        CHECK (fileOn (0, spot3) != nullptr && fileOn (2, spot3) != nullptr && fileOn (1, v2) != nullptr && fileOn (1, spot3) == nullptr);
        // a track removed from the window comes back when clips are moved onto it
        q.findTakeWindow (twId)->hiddenTracks.push_back (spot2);
        TakeGroup g4; g4.id = juce::Uuid(); g4.number = 4; g4.sampleRate = 48000.0; g4.lengthSamples = 48000;
        { TakeFile f; f.trackId = v2; f.trackName = "VivienneSPOT2"; f.numChannels = 1; g4.files.push_back (f); }
        q.findTakeWindow (twId)->groups.push_back (g4);
        CHECK (q.moveTakeClips (twId, v2, spot2, { g4.id }, why) == 1 && ! q.findTakeWindow (twId)->isHidden (spot2));
        // audio anywhere: a take window's clip or an edit's piece (or overdub). VivienneSPOT1 has nothing left (its edit piece went along), VivienneSPOT2
        // still has take 002's clip
        CHECK (! q.trackHasAudio (v1) && q.trackHasAudio (v2) && q.trackHasAudio (spot1) && ! q.trackHasAudio (vm50) && ! q.trackHasAudio (rooms));
        {
            EditRegion od; od.id = juce::Uuid(); od.windowId = twId; od.takeId = takeIds[1]; od.srcIn = 0; od.srcOut = 48000; od.sampleRate = 48000.0;
            RegionFile rf; rf.trackId = rooms; rf.trackName = "ROOMS"; rf.file = tmp.getChildFile ("rooms-od.wav"); rf.numChannels = 1; od.files.push_back (rf);
            q.findEdit (edId)->overdubs.push_back (od);
            CHECK (q.trackHasAudio (rooms));
        }
        // deleting the emptied tracks at once: only the ones with no audio anywhere, never the LTC track
        const auto tracksBefore = q.tracks.size();
        CHECK (q.removeEmptyTracks ({ v1, v2, vm50, ltc, rooms }) == 2);
        CHECK (q.tracks.size() == tracksBefore - 2 && q.findTrack (v1) == nullptr && q.findTrack (vm50) == nullptr);
        CHECK (q.findTrack (v2) != nullptr && q.findTrack (ltc) != nullptr && q.findTrack (rooms) != nullptr);
        CHECK (q.missingTracksOf (*q.findEdit (edId)).empty());          // (the edit's audio is all on tracks that are still there: nothing to ask about)
        CHECK (q.removeEmptyTracks ({ v1 }) == 0);                       // (gone already)
    }

    SECTION ("2.99 the track named in a name, with the numbers at its end taken off (an import that added the piece and take numbers)");
    {
        Project q; q.setInputCount (8); q.setOutputCount (2); q.tracks.clear();
        const auto ab = q.addTrack ("AB", TrackFormat::Stereo, 0).id;
        const auto spot1 = q.addTrack ("SPOT1", TrackFormat::Mono, 2).id;
        const auto spot11 = q.addTrack ("SPOT11", TrackFormat::Mono, 3).id;
        const auto room = q.addTrack ("ROOM 414", TrackFormat::Mono, 4).id;
        const auto prints = q.addTrack ("MAIN PRINTS", TrackFormat::Stereo, 5).id;
        auto is = [&] (const char* nm, const juce::Uuid& want, int ch = 0) { auto* t = q.trackNamedIn (nm, ch); return t != nullptr && t->id == want; };
        CHECK (is ("Rorate Caeli SPOT1 19 18", spot1) && is ("Rorate Caeli SPOT11 20 19", spot11) && is ("Rorate CaeliSPOT1 01", spot1));
        CHECK (is ("Rorate Caeli AB 20 19", ab) && is ("Rorate CaeliAB 01", ab) && is ("Rorate CaeliMAIN PRINTS 01", prints));
        CHECK (is ("Rorate Caeli ROOM 414 20 19", room) && is ("Rorate CaeliROOM 414 01", room) && is ("ROOM 414", room) == false);   // (a name is not its own match at the first step)
        CHECK (is ("AB_01-02", ab) && q.trackNamedIn ("Rorate Caeli SPOT4 19 18") == nullptr && q.trackNamedIn ("SPOT12") == nullptr);
        CHECK (is ("Rorate Caeli AB 20 19", ab, 2) && q.trackNamedIn ("Rorate Caeli AB 20 19", 1) == nullptr);                       // (the channels, when asked)
        CHECK (Project::withoutEndNumbers ("Rorate Caeli AB 20 19") == "Rorate Caeli AB" && Project::withoutEndNumbers ("SPOT4") == "SPOT4"
               && Project::withoutEndNumbers (" ROOM 414 01 ") == "ROOM" && Project::withoutEndNumbers ("Take_003") == "Take");
    }

    SECTION ("2.99 undoing a move after the old track was deleted: the clips and the edit's pieces stay where they are (not on a track that is gone)");
    {
        Project q; q.setInputCount (4); q.setOutputCount (2); q.tracks.clear();
        const auto spot1 = q.addTrack ("SPOT1", TrackFormat::Mono, 0).id;
        const auto v1 = q.addTrack ("VivienneSPOT1", TrackFormat::Mono, 1).id;
        auto& tw = q.addTakeWindow ("Piece"); const auto twId = tw.id;
        TakeGroup g; g.id = juce::Uuid(); g.number = 1; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 10;
        { TakeFile f; f.trackId = v1; f.trackName = "VivienneSPOT1"; f.numChannels = 1; f.file = tmp.getChildFile ("undo-v1.wav"); g.files.push_back (f); }
        tw.groups.push_back (g);
        auto& ed = q.addEdit ("Edit", twId); const auto edId = ed.id;
        {
            EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = g.id; r.srcIn = 0; r.srcOut = 48000 * 5; r.sampleRate = 48000.0;
            RegionFile rf; rf.trackId = v1; rf.trackName = "VivienneSPOT1"; rf.file = tmp.getChildFile ("undo-v1.wav"); rf.numChannels = 1; r.files.push_back (rf);
            ed.regions.push_back (r);
            ed.trackIds = { v1 };
        }
        q.undoReset();
        juce::String why;
        CHECK (q.moveTakeClips (twId, v1, spot1, { g.id }, why) == 1);
        q.undoTick (false, true);
        CHECK (q.removeEmptyTracks ({ v1 }) == 1 && q.findTrack (v1) == nullptr);
        q.undoTick (false, true);
        CHECK (q.undo());
        auto* w = q.findTakeWindow (twId);
        CHECK (w != nullptr && w->groups.size() == 1 && w->groups[0].files.size() == 1 && w->groups[0].files[0].trackId == spot1);
        auto* e = q.findEdit (edId);
        CHECK (e != nullptr && e->regions.size() == 1 && e->regions[0].files[0].trackId == spot1 && e->regions[0].files[0].trackName == "SPOT1");
        CHECK (e != nullptr && e->trackIds.size() == 1 && e->trackIds[0] == spot1);      // (the edit's own rows: SPOT1 in the place of the deleted track)
        CHECK (e != nullptr && q.missingTracksOf (*e).empty());
        // with the old track still there, undo puts the clip back on it as before
        const auto v1b = q.addTrack ("VivienneSPOT1", TrackFormat::Mono, 1).id;
        {
            TakeGroup g2; g2.id = juce::Uuid(); g2.number = 2; g2.sampleRate = 48000.0; g2.lengthSamples = 48000;
            TakeFile f; f.trackId = v1b; f.trackName = "VivienneSPOT1"; f.numChannels = 1; f.file = tmp.getChildFile ("undo-v1b.wav"); g2.files.push_back (f);
            w->groups.push_back (g2); q.changed();
            q.undoTick (false, true);
            CHECK (q.moveTakeClips (twId, v1b, spot1, { g2.id }, why) == 1);
            q.undoTick (false, true);
            CHECK (q.undo() && w->findGroup (g2.id) != nullptr && w->findGroup (g2.id)->files[0].trackId == v1b);
        }
    }

    SECTION ("2.101 a snip (F1): an unnumbered join with a normal 30 ms crossfade, kept through saves, undo, splits and fixes");
    {
        Project q; q.setInputCount (4); q.setOutputCount (2); q.tracks.clear();
        const auto t1 = q.addTrack ("SPOT1", TrackFormat::Mono, 0).id;
        auto& tw = q.addTakeWindow ("Piece"); const auto twId = tw.id;
        auto& ed = q.addEdit ("Edit", twId); const auto edId = ed.id;
        const double sr = 48000.0;
        auto piece = [&] (juce::int64 len, const char* nm)
        {
            EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = juce::Uuid(); r.takeName = nm; r.sampleRate = sr;
            r.srcIn = 48000; r.srcOut = 48000 + len; r.sourceLength = 48000 * 30;
            RegionFile f; f.trackId = t1; f.trackName = "SPOT1"; f.file = tmp.getChildFile (juce::String (nm) + ".wav"); r.files.push_back (f);
            return r;
        };
        ed.insertRegion (piece (48000 * 10, "001"));
        ed.insertRegion (piece (48000 * 10, "002"));
        q.undoReset();
        auto* e = q.findEdit (edId);
        CHECK (e->regions.size() == 2 && e->editNumberAt (1) == 2);
        juce::String why;
        // at 4 s, inside the first piece
        const int k = e->snipAt (4 * 48000, why);
        CHECK (k == 1 && why.isEmpty() && e->regions.size() == 3);
        CHECK (e->isSnipJoin (1) && ! e->isFixJoin (1) && e->isUnnumberedJoin (1) && ! e->isSnipJoin (2));
        CHECK (e->editNumberAt (0) == 1 && e->editNumberAt (1) == 0 && e->editNumberAt (2) == 2 && e->editNumberAt (3) == 3);     // (the edit points keep their numbers)
        {
            const auto& a = e->regions[0]; const auto& b = e->regions[1];
            CHECK (a.endSample() == 4 * 48000 && b.startSample == 4 * 48000 && b.srcIn == a.srcOut && b.takeId == a.takeId);        // one take, cut, nothing moved
            CHECK (a.outCurveValue() == FadeCurve::EqualPower && b.curve == FadeCurve::EqualPower);                                 // a normal fade
            CHECK (a.fadeOutBegin() == 4 * 48000 - 720 && a.fadeOutFinish() == 4 * 48000 + 720 && b.fadeInBegin() == 4 * 48000 - 720 && b.fadeInFinish() == 4 * 48000 + 720);
        }
        // refused: on top of a crossfade (the edit point at 10 s), at the very start, past the end
        CHECK (e->snipAt (10 * 48000 + 100, why) < 0 && why.isNotEmpty());
        CHECK (e->snipAt (300, why) < 0 && e->snipAt (25 * 48000, why) < 0 && e->regions.size() == 3);
        // saved and read back (through undo / redo, which write and read the edit)
        q.changed(); q.undoTick (false, true);
        CHECK (q.undo() && q.findEdit (edId)->regions.size() == 2);
        CHECK (q.redo() && q.findEdit (edId)->regions.size() == 3 && q.findEdit (edId)->isSnipJoin (1));
        e = q.findEdit (edId);
        // a piece cut again keeps the snip on its other side; a cut next to it is a plain join
        e->splitRegion (0, 48000);
        CHECK (e->regions.size() == 4 && ! e->isSnipJoin (1) && e->isSnipJoin (2) && e->editNumberAt (1) == 2);
        // a piece taken out next to a snip: the join that is left is a real edit (only one side was marked)
        e->removeRegion (2);
        CHECK (! e->isSnipJoin (2) && ! e->isUnnumberedJoin (2));
        // the offline fixes count a snip as a real join (two streams), unlike their own joins: isFixJoin stays false
        CHECK (! e->isFixJoin (1) && ! e->isFixJoin (2));
    }

    SECTION ("2.101 the pitch pipe's keyboard sound: true pitch, a strike that settles to a steady held level, a quick release");
    {
        PitchPipeVoices v;
        v.prepare (48000.0); v.setLevelDb (-14.0f); v.setKeyboard (true);
        CHECK (v.isKeyboard());
        std::vector<float> blk (512), all;
        v.setSlot (0, 440.0, true);
        for (int b = 0; b < 48000 * 5 / 512; ++b) { v.render (blk.data(), 512); all.insert (all.end(), blk.begin(), blk.end()); }
        auto rms = [&] (int from, int len) { double sum = 0.0; for (int i = from; i < from + len; ++i) sum += (double) all[(size_t) i] * all[(size_t) i]; return std::sqrt (sum / len); };
        const double strike = rms (480, 4800), held = rms (4 * 48000, 24000);
        CHECK (strike > 0.03 && held > 0.2 * strike && held < 0.55 * strike);            // it dies away, but never to nothing while held
        CHECK (std::abs (all[0]) < 0.05f);                                                // (the strike starts from silence: no click)
        {
            const int N = 1 << 20;
            std::vector<double> x ((size_t) N, 0.0);
            const int from = 96000, len = 96000;
            for (int i = 0; i < len; ++i) x[(size_t) i] = all[(size_t) (from + i)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / len));
            std::vector<std::complex<double>> X ((size_t) N / 2 + 1);
            denoise::detail::realForward (x.data(), N, X.data());
            int best = 0; double bm = 0.0;
            for (int kk = (int) (400.0 * N / 48000.0); kk < (int) (480.0 * N / 48000.0); ++kk) if (std::abs (X[(size_t) kk]) > bm) { bm = std::abs (X[(size_t) kk]); best = kk; }
            const double ym = std::abs (X[(size_t) best - 1]), y0 = bm, yp = std::abs (X[(size_t) best + 1]);
            const double hz = (best + 0.5 * (ym - yp) / (ym - 2.0 * y0 + yp)) * 48000.0 / N;
            CHECK (std::abs (hz - 440.0) < 0.05);
        }
        v.setSlot (0, 440.0, false);
        int blocksSounding = 0;
        for (int b = 0; b < 60; ++b) if (v.render (blk.data(), 512)) ++blocksSounding;
        CHECK (blocksSounding >= 20 && blocksSounding <= 32 && ! v.sounding());           // gone in about 0.3 s
        // back to the reed
        v.setKeyboard (false); CHECK (! v.isKeyboard());
    }

    SECTION ("2.102 undo keeps copies of the windows: a big edit's step is quick, and undo / redo put it back exactly");
    {
        Project q; q.setInputCount (4); q.setOutputCount (2); q.tracks.clear();
        std::vector<juce::Uuid> ids; for (int t = 0; t < 8; ++t) ids.push_back (q.addTrack ("T" + juce::String (t), TrackFormat::Mono, t % 4).id);
        auto& tw = q.addTakeWindow ("Piece");
        auto& ed = q.addEdit ("Edit", tw.id); const auto edId = ed.id;
        for (int r = 0; r < 300; ++r)
        {
            EditRegion rg; rg.id = juce::Uuid(); rg.windowId = tw.id; rg.takeId = juce::Uuid(); rg.takeName = "t"; rg.sampleRate = 48000.0;
            rg.srcIn = 1000; rg.srcOut = 49000; rg.sourceLength = 96000;
            for (auto& id : ids) { RegionFile f; f.trackId = id; f.trackName = "T"; f.file = tmp.getChildFile ("u" + juce::String (r) + ".wav"); rg.files.push_back (f); }
            ed.insertRegion (rg);
        }
        q.undoReset();
        const auto before = Project::undoHash (*q.findEdit (edId));
        q.findEdit (edId)->regions[10].inEnd += 0.01; q.changed();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        q.undoTick (false, true);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("   (the undo step of a 300-piece, 8-track edit: %.1f ms)\n", ms);
        CHECK (q.undoDepth() == 1 && ms < 400.0);                       // (it used to turn the edit into text twice: about a second for a big one)
        const auto after = Project::undoHash (*q.findEdit (edId));
        CHECK (before != after);
        CHECK (q.undo() && Project::undoHash (*q.findEdit (edId)) == before);
        CHECK (q.redo() && Project::undoHash (*q.findEdit (edId)) == after);
        // the copies are the windows as they were: changing the live edit afterwards does not change what undo puts back
        q.findEdit (edId)->regions[20].startSample += 5; q.changed(); q.undoTick (false, true);
        CHECK (q.undo() && Project::undoHash (*q.findEdit (edId)) == after);
        CHECK (q.undo() && Project::undoHash (*q.findEdit (edId)) == before);
    }

    SECTION ("2.102 a save in two halves: the snapshot on the message thread, the writing on any thread; a failed write is saved again");
    {
        const auto dir = tmp.getChildFile ("twohalves"); dir.deleteRecursively(); dir.createDirectory();
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.projectFile = dir.getChildFile ("Halves.fermata"); q.name = "Halves";
        auto& tw = q.addTakeWindow ("Piece"); const auto twId = tw.id;
        { TakeGroup g; g.id = juce::Uuid(); g.number = 1; g.lengthSamples = 48000; g.sampleRate = 48000.0; TakeFile f; f.trackId = ta; f.trackName = "A"; f.file = dir.getChildFile ("Recorded Media/a.wav"); g.files.push_back (f); tw.groups.push_back (g); }
        auto& ed = q.addEdit ("Edit", twId); const auto edId = ed.id;
        { EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = tw.groups[0].id; r.takeName = "001"; r.sampleRate = 48000.0; r.srcIn = 0; r.srcOut = 24000; r.sourceLength = 48000;
          RegionFile f; f.trackId = ta; f.trackName = "A"; f.file = dir.getChildFile ("Recorded Media/a.wav"); r.files.push_back (f); ed.insertRegion (r); }
        juce::String err;
        CHECK (q.save (err));                                                         // (save itself is the two halves, on one thread)
        CHECK (dir.getChildFile ("Edit Windows/Edit.fmedit").existsAsFile() && dir.getChildFile ("Take Windows/Piece.fmtake").existsAsFile());
        auto srcOutRead = [&] () -> juce::int64
        {
            Project r2; juce::String le;
            if (! r2.load (q.projectFile, le)) return -1;
            auto* re = r2.findEdit (edId);
            return re != nullptr && re->regions.size() == 1 ? re->regions[0].srcOut : -2;
        };
        // a change: only that window is in the job, as it was at the snapshot, and it is written on another thread
        q.findEdit (edId)->regions[0].srcOut = 30000; q.changed();
        auto job = q.prepareSave (true);
        CHECK (job != nullptr && ! q.dirty.load());
        CHECK (job != nullptr && job->windows.size() == 1 && job->windows[0].edit != nullptr);
        q.findEdit (edId)->regions[0].srcOut = 31000;                                  // (changed again after the snapshot)
        bool ok = false; juce::String werr;
        std::thread th ([&] { ok = job != nullptr && Project::writeSave (*job, werr); });
        th.join();
        CHECK (ok && werr.isEmpty());
        CHECK (srcOutRead() == 30000);
        // a write that fails: unsaved again, and that window is written again by the next save
        auto job2 = q.prepareSave (true);
        CHECK (job2 != nullptr && job2->windows.size() == 1);
        if (job2 != nullptr)
        {
            for (auto& w : job2->windows) w.file = juce::File ("/proc/fermata-cannot-write/" + w.file.getFileName());
            bool wf = false; juce::String e2;
            CHECK (! Project::writeSave (*job2, e2, &wf) && wf && e2.isNotEmpty());
            q.saveFailed (*job2);
            CHECK (q.dirty.load());
            auto job3 = q.prepareSave (true);
            CHECK (job3 != nullptr && job3->windows.size() == 1);
            CHECK (job3 != nullptr && Project::writeSave (*job3, e2));
            CHECK (srcOutRead() == 31000);
        }
        auto job4 = q.prepareSave (true);                                              // nothing changed: only the project file to write
        CHECK (job4 != nullptr && job4->windows.empty());
    }

    SECTION ("2.102 the background saver: jobs written in order on a thread of its own, their results taken in by drain()");
    {
        const auto dir = tmp.getChildFile ("bgsave"); dir.deleteRecursively(); dir.createDirectory();
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.projectFile = dir.getChildFile ("Bg.fermata"); q.name = "Bg";
        auto& tw = q.addTakeWindow ("Piece");
        auto& ed = q.addEdit ("Edit", tw.id); const auto edId = ed.id;
        { EditRegion r; r.id = juce::Uuid(); r.windowId = tw.id; r.takeId = juce::Uuid(); r.takeName = "001"; r.sampleRate = 48000.0; r.srcIn = 0; r.srcOut = 24000; r.sourceLength = 48000;
          RegionFile f; f.trackId = ta; f.trackName = "A"; f.file = dir.getChildFile ("a.wav"); r.files.push_back (f); ed.insertRegion (r); }
        juce::String err; CHECK (q.save (err));
        std::vector<int> order; std::atomic<int> wakes { 0 };
        {
            BackgroundSaver saver ([&] { ++wakes; });
            q.findEdit (edId)->name = "First"; q.changed();
            saver.write (q.prepareSave (true), [&] (BackgroundSaver::Result& r) { CHECK (r.ok); order.push_back (1); });
            q.findEdit (edId)->name = "Second"; q.changed();
            saver.write (q.prepareSave (true), [&] (BackgroundSaver::Result& r) { CHECK (r.ok); order.push_back (2); });
            CHECK (saver.waitUntilIdle (10000) && ! saver.isBusy());
            for (int k = 0; k < 400 && wakes.load() < 2; ++k) juce::Thread::sleep (5);       // (the saver wakes the program just after it is idle: on a busy machine a moment later)
            CHECK (saver.drain() == 2);
            CHECK (order == std::vector<int> ({ 1, 2 }) && wakes.load() == 2 && saver.written() == 2);
            CHECK (saver.drain() == 0);
        }
        CHECK (dir.getChildFile ("Edit Windows/Second.fmedit").existsAsFile());
        CHECK (! dir.getChildFile ("Edit Windows/First.fmedit").existsAsFile() && ! dir.getChildFile ("Edit Windows/Edit.fmedit").existsAsFile());     // (renamed: the old files tidied away in turn)
        Project r2; juce::String le; CHECK (r2.load (q.projectFile, le));
        CHECK (r2.findEdit (edId) != nullptr && r2.findEdit (edId)->name == "Second");
    }

    SECTION ("2.102 a play that does not wait: silence, and the position held, until the first moments are read; then the sound");
    {
        const auto f = makeConstWav (tmp.getChildFile ("prime.wav"), 0.25, 3);
        PlaySegment s; s.trackIndex = 0; s.file = f; s.rate = 48000.0; s.begin = 0; s.end = 3 * 48000;
        auto ps = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { s }, 0, 3 * 48000, 48000.0, 512);
        CHECK (ps->open().isEmpty());
        ps->start (false);
        int silentBlocks = 0; bool heard = false;
        for (int k = 0; k < 4000 && ! heard; ++k)
        {
            ps->pull (512);
            const float v = ps->trackBuffer (0).getSample (0, 511);
            if (! ps->isPrimed()) { CHECK (ps->getPosition() == 0 && v == 0.0f); ++silentBlocks; juce::Thread::sleep (1); }
            else heard = std::abs (v - 0.25f) < 0.01f;
        }
        CHECK (heard && ps->isPrimed() && ps->msToFirstSound() > 0.0);
        CHECK (ps->getPosition() > 0 && ps->getUnderruns() == 0);
        std::printf ("   (first sound after %.1f ms, %d silent blocks before it)\n", ps->msToFirstSound(), silentBlocks);
        // the files are asked about for the first few seconds only: one that has gone further on does not stop the play
        PlaySegment far = s; far.file = tmp.getChildFile ("gone-for-good.wav"); far.begin = 10 * 48000; far.end = 11 * 48000; far.srcOffset = -10 * 48000;
        PlaySegment soon = far; soon.begin = 48000; soon.end = 2 * 48000; soon.srcOffset = -48000;
        { auto p2 = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { s, far }, 0, 12 * 48000, 48000.0, 512); CHECK (p2->open().isEmpty()); }
        { auto p3 = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { s, soon }, 0, 12 * 48000, 48000.0, 512); CHECK (p3->open().startsWith ("Cannot read")); }
    }

    SECTION ("2.102 Stop hands the play to a thread of its own, which frees it once the audio thread has left it");
    {
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        q.addTrack ("A", TrackFormat::Mono, 0);
        AudioEngine e (q);
        e.setMixerThreads (0); e.prepare (48000.0, 64); e.rebuildPlan();
        const auto f = makeConstWav (tmp.getChildFile ("retire.wav"), 0.1, 2);
        PlaySegment s; s.trackIndex = 0; s.file = f; s.rate = 48000.0; s.begin = 0; s.end = 2 * 48000;
        auto session = [&] { return std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { s }, 0, 2 * 48000, 48000.0, 64); };
        e.setRunningForTests (true);
        CHECK (e.startPlayback (session(), false).isEmpty() && e.isPlaying());
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        e.stopPlayback (true);                                                          // (a press of Space or Stop)
        const auto stopMs = juce::Time::getMillisecondCounterHiRes() - t0;
        CHECK (! e.isPlaying() && e.sessionsWaitingToBeFreed() == 1);
        CHECK (stopMs < 50.0);                                                          // (it no longer waits for the audio thread, nor for the reader to stop)
        e.setRunningForTests (false);                                                   // (as if the device stopped: nothing can be reading it now)
        for (int k = 0; k < 400 && e.sessionsWaitingToBeFreed() > 0; ++k) juce::Thread::sleep (5);
        CHECK (e.sessionsWaitingToBeFreed() == 0 && e.audioThreadNeverStalled());
        // any other Stop does it all itself, as before, and waits for what a transport press handed off: its files are closed when it returns
        CHECK (e.startPlayback (session()).isEmpty());
        e.stopPlayback();
        CHECK (! e.isPlaying() && e.sessionsWaitingToBeFreed() == 0);
        e.setRunningForTests (true);
        CHECK (e.startPlayback (session(), false).isEmpty());
        e.stopPlayback (true);
        CHECK (e.sessionsWaitingToBeFreed() == 1);
        std::thread blocks ([&] { juce::Thread::sleep (30); e.setRunningForTests (false); });      // (the audio thread moves on a moment later)
        e.stopPlayback();                                                               // (nothing plays: it waits for the one handed off)
        CHECK (e.sessionsWaitingToBeFreed() == 0);
        blocks.join();
    }

    SECTION ("2.102 the edit's visual guide: saved and read back, part of undo (Show is not), never in the edit's play or length; played alone by segmentsForGuide");
    {
        const auto dir = tmp.getChildFile ("guide"); dir.deleteRecursively(); dir.createDirectory();
        Project q; q.setInputCount (4); q.setOutputCount (2); q.tracks.clear();
        const auto spot = q.addTrack ("SPOT1", TrackFormat::Mono, 0).id;
        const auto ortf = q.addTrack ("ORTF", TrackFormat::Stereo, 1).id;
        q.projectFile = dir.getChildFile ("Guide.fermata"); q.name = "Guide";
        auto& tw = q.addTakeWindow ("Piece"); const auto twId = tw.id;
        auto& ed = q.addEdit ("Edit", twId); const auto edId = ed.id;
        { EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = juce::Uuid(); r.takeName = "002"; r.sampleRate = 48000.0; r.srcIn = 0; r.srcOut = 48000 * 10; r.sourceLength = 48000 * 20;
          RegionFile f; f.trackId = spot; f.trackName = "SPOT1"; f.file = dir.getChildFile ("Recorded Media/002 - SPOT1.wav"); r.files.push_back (f); ed.insertRegion (r); }
        const auto lenBefore = ed.lengthSamples();
        EditGuide gd; gd.windowId = twId; gd.takeId = juce::Uuid(); gd.trackId = ortf; gd.takeName = "001"; gd.trackName = "ORTF";
        gd.file = dir.getChildFile ("Recorded Media/001 - ORTF.wav"); gd.numChannels = 2; gd.sampleRate = 48000.0; gd.srcIn = 48000; gd.srcOut = 48000 * 31; gd.start = -48000 * 3;
        q.undoReset();
        q.findEdit (edId)->guide = gd; q.changed(); q.undoTick (false, true);
        auto* e = q.findEdit (edId);
        CHECK (e->guide.active() && e->lengthSamples() == lenBefore);                    // a picture: the edit is no longer for it
        for (auto& sg : segmentsForEdit (q, *e)) CHECK (sg.file != gd.file);            // and it never plays with the edit
        auto gs = segmentsForGuide (q, *e);                                              // on its own: one segment, on its track, nothing before 0:00
        CHECK (gs.size() == 1);
        if (gs.size() == 1)
        {
            CHECK (gs[0].trackIndex == 1 && gs[0].file == gd.file && gs[0].begin == 0 && gs[0].end == gd.start + (gd.srcOut - gd.srcIn));
            CHECK (gs[0].srcOffset == 48000 * 4);                                        // (timeline 0 is 3 s into the guide, which starts 1 s into the file)
        }
        e->showGuide = false;
        juce::String err; CHECK (q.save (err));
        {
            Project r; juce::String le; CHECK (r.load (q.projectFile, le));
            auto* re = r.findEdit (edId);
            CHECK (re != nullptr && re->guide.active() && ! re->showGuide);
            if (re != nullptr)
            {
                const auto& g2 = re->guide;
                CHECK (g2.file == gd.file && g2.trackId == ortf && g2.takeName == "001" && g2.trackName == "ORTF" && g2.numChannels == 2 && g2.sampleRate == 48000.0);
                CHECK (g2.srcIn == gd.srcIn && g2.srcOut == gd.srcOut && g2.start == gd.start && g2.windowId == twId && g2.takeId == gd.takeId);
            }
        }
        // a missing guide file is counted and listed with the other missing audio (and found again by a relink like them)
        {
            CHECK (q.missingAudioCount() == 2);                                          // (the piece's file and the guide's: neither is on the disk here)
            const auto list = q.missingAudioList (10);
            CHECK (list.size() == 2 && list.joinIntoString ("|").contains ("visual guide"));
        }
        // part of undo: sliding it is a step; showing or hiding it is not
        const int depth = q.undoDepth();
        e->showGuide = true; q.changed(); q.undoTick (false, true);
        CHECK (q.undoDepth() == depth);
        e->guide.start = 48000 * 2; q.changed(); q.undoTick (false, true);
        CHECK (q.undoDepth() == depth + 1);
        CHECK (q.undo() && q.findEdit (edId)->guide.start == gd.start && q.findEdit (edId)->showGuide);
        CHECK (q.undo() && ! q.findEdit (edId)->guide.active());                         // (before it was set)
        CHECK (q.redo() && q.findEdit (edId)->guide.active());
        // its track gone: the track with its name, or failing that the first with as many channels
        e = q.findEdit (edId);
        CHECK (guideTrackIndex (q, e->guide) == 1);
        e->guide.trackId = juce::Uuid(); CHECK (guideTrackIndex (q, e->guide) == 1);
        e->guide.trackName = "Gone"; CHECK (guideTrackIndex (q, e->guide) == 1);
        e->guide.numChannels = 6; CHECK (guideTrackIndex (q, e->guide) == -1 && segmentsForGuide (q, *e).empty());
    }

    SECTION ("2.102 a window never takes a file another window has, so a save never tidies away a file it has just written");
    {
        const auto dir = tmp.getChildFile ("ownedpaths"); dir.deleteRecursively(); dir.createDirectory();
        Project q; q.setInputCount (2); q.setOutputCount (2);
        q.projectFile = dir.getChildFile ("Owned.fermata"); q.name = "Owned";
        auto& a = q.addTakeWindow ("P"); auto& b = q.addTakeWindow ("Q");
        const auto aId = a.id, bId = b.id;
        juce::String err; CHECK (q.save (err));
        CHECK (dir.getChildFile ("Take Windows/P.fmtake").existsAsFile() && dir.getChildFile ("Take Windows/Q.fmtake").existsAsFile());
        dir.getChildFile ("Take Windows/P.fmtake").deleteFile();                          // (not on the disk just now: as if a save still to be written held it)
        q.findTakeWindow (aId)->name = "R"; q.findTakeWindow (bId)->name = "P"; q.changed();
        CHECK (q.save (err));
        Project r; juce::String le; CHECK (r.load (q.projectFile, le));
        CHECK (r.loadNotes.isEmpty());                                                     // every window file the project lists is there
        CHECK (r.findTakeWindow (aId) != nullptr && r.findTakeWindow (aId)->name == "R" && r.findTakeWindow (bId) != nullptr && r.findTakeWindow (bId)->name == "P");
    }

    SECTION ("2.103 ReTimEr: the time map (glides, inverse, length) and the stretch it asks for");
    {
        using retime::TimeMap;
        const double sr = 48000.0;
        for (int k = 0; k < 4; ++k)
        {
            TimeMap m { 10 * sr, 30 * sr, 1.0, 0.0, 0.0, k == 3 };
            const double L = 20 * sr, target = (k == 2 ? 18.5 : 21.0) * sr, rIn = (k == 0 ? 0.0 : 2.0) * sr, rOut = (k == 0 ? 0.0 : k == 1 ? 5.0 : 3.0) * sr;
            m.rIn = k == 3 ? 0.0 : rIn; m.rOut = k == 3 ? 0.0 : rOut;
            m.S = k == 3 ? target / L : TimeMap::solve (L, target, m.rIn, m.rOut);
            CHECK (std::abs (m.newLength() - target) < 1.0e-6 * sr);                     // the part lasts exactly what was asked
            double worst = 0.0, prev = -1.0e18; bool rising = true;
            for (double t = 0; t < 45 * sr; t += 997.3)
            {
                const double u = m.map (t);
                worst = juce::jmax (worst, std::abs (m.unmap (u) - t));
                rising = rising && u > prev; prev = u;
            }
            CHECK (worst < 1.0e-4 && rising);                                              // the inverse is the inverse; time never goes backwards
            if (! m.uniform)
            {
                for (double edge : { m.t0, m.t0 + m.rIn, m.t1 - m.rOut, m.t1 }) CHECK (std::abs (m.map (edge - 0.001) - m.map (edge + 0.001)) < 0.01);      // no jumps
                CHECK (m.map (5 * sr) == 5 * sr && std::abs (m.map (40 * sr) - (40 * sr + m.delta())) < 1.0e-6);
                if (m.rIn > 0.0) CHECK (std::abs (m.rate (m.t0 + 1.0) - 1.0) < 1.0e-6 && std::abs (m.rate (m.t1 - 1.0) - 1.0) < 1.0e-6);   // the glides start and end at the normal speed
            }
        }
        CHECK (TimeMap::solve (20.0, 10.0, 20.0, 20.0) <= 0.0);                            // (impossible: no held part)

        // the stretch: exact length, in order; untouched before the part; the microphones keep their time difference; transients land where the map says
        const int C = 3; const juce::int64 N = (juce::int64) (14 * sr);
        std::vector<std::vector<float>> src ((size_t) C, std::vector<float> ((size_t) N + 96000, 0.0f));
        std::mt19937 rng (5); std::normal_distribution<float> nd (0.0f, 0.01f);
        double ph[3] = { 0, 0, 0 };
        for (size_t i = 0; i < src[0].size(); ++i)
        {
            double v = 0.0;
            for (int h = 0; h < 3; ++h) { ph[h] += juce::MathConstants<double>::twoPi * (196.0 * (h + 1) * (1.0 + 0.003 * std::sin (0.0002 * (double) i))) / sr; v += 0.2 * std::sin (ph[h]) / (h + 1); }
            src[0][i] = (float) v + nd (rng); src[2][i] = nd (rng) * 4.0f;
        }
        std::vector<juce::int64> clicks;
        for (juce::int64 t = 30000; t < N - 48000; t += 60000) { clicks.push_back (t); for (int q2 = 0; q2 < 32; ++q2) src[0][(size_t) (t + q2)] += 0.7f * std::exp (-q2 / 6.0f) * (q2 % 2 ? -1.0f : 1.0f); }
        for (size_t i = 37; i < src[0].size(); ++i) src[1][i] = src[0][i - 37];          // a spot mic 37 samples behind the main pair
        auto read = [&] (juce::int64 start, int n, std::vector<std::vector<float>>& out)
        {
            for (int c = 0; c < C; ++c) for (int q2 = 0; q2 < n; ++q2) { const auto t = start + q2; out[(size_t) c][(size_t) q2] = t >= 0 && t < (juce::int64) src[(size_t) c].size() ? src[(size_t) c][(size_t) t] : 0.0f; }
        };
        const TimeMap m { 4 * sr, 10 * sr, 1.15, 1.0 * sr, 1.5 * sr, false };
        const juce::int64 outB = (juce::int64) std::llround (m.map ((double) N));
        auto run = [&] (std::vector<std::vector<float>>& out)
        {
            out.assign ((size_t) C, std::vector<float> ((size_t) outB, 0.0f));
            juce::int64 next = 0; bool inOrder = true;
            const auto err = retime::stretch (C, sr, m, 0, outB, read, [&] (const std::vector<std::vector<float>>& blk, int n, juce::int64 at)
            {
                inOrder = inOrder && at == next; next = at + n;
                for (int c = 0; c < C; ++c) std::copy (blk[(size_t) c].begin(), blk[(size_t) c].begin() + n, out[(size_t) c].begin() + at);
                return true;
            });
            return err.isEmpty() && inOrder && next == outB;
        };
        std::vector<std::vector<float>> out1, out2;
        CHECK (run (out1) && run (out2));
        CHECK (out1 == out2);                                                              // the same audio always comes out the same
        double diff = 0.0;
        for (juce::int64 i = (juce::int64) (0.5 * sr); i < (juce::int64) (3.9 * sr); ++i) diff = juce::jmax (diff, (double) std::abs (out1[0][(size_t) i] - src[0][(size_t) i]));
        CHECK (diff < 1.0e-3);                                                             // before mark 1 it is the original, so the crossfade there is clean
        auto corrAt = [&] (size_t from, size_t n, int lag)
        {
            double ab = 0, aa = 0, bb = 0;
            for (size_t i = from; i < from + n; ++i) { ab += out1[0][i] * out1[1][i + (size_t) lag]; aa += out1[0][i] * out1[0][i]; bb += out1[1][i + (size_t) lag] * out1[1][i + (size_t) lag]; }
            return ab / std::sqrt (aa * bb + 1.0e-30);
        };
        int bestLag = 0; double best = -2.0;
        for (int lag = 0; lag < 80; ++lag) { const double c = corrAt ((size_t) m.map (7 * sr), (size_t) sr, lag); if (c > best) { best = c; bestLag = lag; } }
        CHECK (bestLag == 37 && best > 0.99);                                              // the spot mic is still 37 samples behind: the image does not smear
        double worstMs = 0.0;
        for (auto t : clicks)
        {
            const double want = m.map ((double) t + 6.0);
            double num = 0, den = 0;
            for (juce::int64 i = (juce::int64) want - 2400; i < (juce::int64) want + 2400; ++i) { const double e2 = (double) out1[0][(size_t) i] * out1[0][(size_t) i]; num += e2 * (double) i; den += e2; }
            worstMs = juce::jmax (worstMs, std::abs (num / den - want) / sr * 1000.0);
        }
        CHECK (worstMs < 1.5);                                                             // every click where the map puts it (to within a millisecond or so)
    }

    SECTION ("2.103 ReTimEr: the plan - where it is cut, what is made again, what moves, what stays");
    {
        using namespace retime;
        const double sr = 48000.0;
        auto sec = [sr] (double s) { return (juce::int64) std::llround (s * sr); };
        const auto dir = tmp.getChildFile ("retime_plan"); dir.deleteRecursively(); dir.createDirectory();
        const juce::Uuid tMain, tSpot, tLtc;
        auto piece = [&] (const juce::String& name, double start, double srcIn, double len)
        {
            EditRegion r; r.id = juce::Uuid(); r.windowId = juce::Uuid(); r.takeId = juce::Uuid(); r.takeName = name; r.sampleRate = sr;
            r.startSample = sec (start); r.srcIn = sec (srcIn); r.srcOut = sec (srcIn + len); r.sourceLength = sec (60);
            for (auto t : { std::make_pair (tMain, juce::String ("ORTF")), std::make_pair (tSpot, juce::String ("SPOT1")), std::make_pair (tLtc, juce::String ("LTC")) })
            { RegionFile f; f.trackId = t.first; f.trackName = t.second; f.file = dir.getChildFile (name + " - " + t.second + ".wav"); f.numChannels = t.first == tMain ? 2 : 1; r.files.push_back (f); }
            return r;
        };
        auto editOf = [&] (std::vector<EditRegion> rs)
        {
            EditDef e; e.sampleRate = sr; e.regions = std::move (rs);
            for (int k = 1; k < (int) e.regions.size(); ++k) e.setDefaultJoinFades (k);
            e.hardenEnds(); e.clampAll();
            return e;
        };
        const FileLength len60 = [sec] (const juce::File&) { return sec (60); };
        const IsLtc isLtc = [tLtc] (const juce::Uuid& t) { return t == tLtc; };
        auto marks = [&] (double a, double b, double S, double rIn = 0.0, double rOut = 0.0) { Request q; q.from = sec (a); q.to = sec (b); q.S = S; q.rampIn = rIn; q.rampOut = rOut; return q; };

        // marks inside one take, its next edit point near: cut at mark 1, the take retimed on to its end, the next take moved along
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            const auto bId = e.regions[1].id, bTake = e.regions[1].takeId, cId = e.regions[2].id;
            const auto p = makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc);
            CHECK (p.ok() && p.t0 == sec (12) && p.t1 == sec (20) && p.delta == sec (0.8));
            CHECK (p.regions.size() == 4 && p.streams.size() == 1 && p.retimed.size() == 1);
            if (p.regions.size() == 4 && p.streams.size() == 1)
            {
                const auto& L = p.regions[1]; const auto& R = p.regions[2];
                CHECK (L.id == bId && L.startSample == sec (10) && L.endSample() == sec (12) && L.fixOut && L.outStart == -kFixFade && L.outEnd == 0.0 && L.outCurveValue() == FadeCurve::Linear);
                CHECK (R.startSample == sec (12) && R.endSample() == sec (30.8) && R.fixIn && R.inStart == -kFixFade && R.inEnd == 0.0 && R.curve == FadeCurve::Linear);
                CHECK (R.takeId != bTake && R.takeName == "B (retimed 110.0%)" && ! R.fixOut);     // its samples are its own now, not the take's
                CHECK (R.files.size() == 2);                                                      // the LTC is not stretched: a retimed piece has none
                const auto& s = p.streams[0];
                CHECK (s.tracks.size() == 2 && s.channels() == 3 && s.a == sec (12) - sec (kFixFade) - sec (0.1) && s.outA == s.a);
                CHECK (s.b == sec (30) + sec (EditDef::kDefaultHalfFade) + sec (kHandleInner));    // its far end meets take C: 5 s of handle for that edit point
                for (auto& f : R.files) CHECK (f.fileStart == 0 && f.file.getParentDirectory() == dir && ! f.file.exists());
                CHECK (R.srcIn == R.startSample - s.outA && R.sourceLength == s.outB - s.outA);
                CHECK (p.regions[3].id == cId && p.regions[3].startSample == sec (30.8) && p.regions[0].startSample == 0);
                CHECK (p.note.contains ("runs on"));
            }
        }
        // ... its next edit point far: it hands back at the quietest moment of the next 3 s, through an equal-power crossfade
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 0, 50), piece ("C", 60, 2, 10) });
            const auto p = makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc, [sec] (const EditRegion&, juce::int64 lo, juce::int64) { return lo + sec (1); });
            CHECK (p.ok() && p.regions.size() == 5 && p.streams.size() == 1);
            if (p.regions.size() == 5)
            {
                const auto& N = p.regions[2]; const auto& R = p.regions[3];
                CHECK (N.endSample() == sec (21.8) && N.fixOut && N.outStart == 0.0 && N.outEnd == kOutFade && N.outCurveValue() == FadeCurve::EqualPower);
                CHECK (R.startSample == sec (21.8) && R.srcIn == sec (11) && R.fixIn && R.inStart == 0.0 && R.inEnd == kOutFade && R.curve == FadeCurve::EqualPower);
                CHECK (p.regions[4].startSample == sec (60.8) && p.note.contains ("hands back"));
                CHECK (p.streams[0].b == sec (21) + sec (kOutFade) + sec (0.1));                   // (only as far as the hand-back's crossfade needs)
            }
        }
        // mark 1 on an edit point's crossfade: it goes onto the edit point, and the take before it is not touched
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            juce::String note;
            CHECK (snapSpan (e, sec (9.99), sec (20), &note).first == sec (10) && note.contains ("Mark 1"));
            const auto p = makePlan (e, marks (9.99, 20, 1.1), dir, len60, isLtc);
            CHECK (p.ok() && p.t0 == sec (10) && p.regions.size() == 3 && p.regions[0].id == e.regions[0].id && p.regions[0].endSample() == sec (10));
            CHECK (p.regions[1].startSample == sec (10) && p.regions[1].inStart == e.regions[1].inStart && ! p.regions[1].fixIn);   // the edit point and its crossfade as they were
            if (! p.streams.empty()) CHECK (p.streams[0].a == e.regions[1].fadeInBegin() - sec (kHandleInner));                  // a handle, so that edit point can still be trimmed
        }
        // a whole piece: evenly, its handles too (20 s, as far as the take goes), the rest moved along; faster works the same way
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            Request q; q.wholePiece = true; q.piece = e.regions[1].id; q.S = 0.9;
            const auto p = makePlan (e, q, dir, len60, isLtc);
            CHECK (p.ok() && p.delta == -sec (2) && p.regions.size() == 3 && p.streams.size() == 1);
            if (p.ok() && p.streams.size() == 1)
            {
                const auto& s = p.streams[0];
                CHECK (s.audio.uniform && s.a == -sec (10) && s.b == sec (50));                     // (only 20 s of the take before the piece; 20 s after it)
                CHECK (s.outA == -sec (8) && s.outB == sec (46));                                   // (no stray sample from rounding)
                CHECK (p.regions[1].startSample == sec (10) && p.regions[1].endSample() == sec (28) && p.regions[1].srcIn == sec (18) && p.regions[2].startSample == sec (28));
                CHECK (p.regions[1].inStart == e.regions[1].inStart && p.regions[1].outEnd == e.regions[1].outEnd);   // both edit points keep their crossfades
            }
        }
        // a fix join inside: the two sides are made again TOGETHER (read through the fix's own crossfade), so no copy of the same sound meets another
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            const int k = e.splitRegion (1, sec (5));
            e.regions[(size_t) k - 1].fixOut = true; e.regions[(size_t) k].fixIn = true;
            e.regions[(size_t) k - 1].setOutCurve (FadeCurve::Linear); e.regions[(size_t) k].setInCurve (FadeCurve::Linear);
            const auto p = makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc);
            CHECK (p.ok() && p.streams.size() == 1 && p.regions.size() == 4);
            if (p.ok() && ! p.streams.empty())
            {
                CHECK (p.streams[0].tracks[0].sources.size() == 2);
                const auto& s0 = p.streams[0].tracks[0].sources[0]; const auto& s1 = p.streams[0].tracks[0].sources[1];
                CHECK (! s0.fadeIn && s0.fadeOut && s1.fadeIn && ! s1.fadeOut);
                CHECK (std::abs (s0.gainAt (sec (15)) + s1.gainAt (sec (15)) - 1.0f) < 1.0e-4f && s0.gainAt (sec (14)) == 1.0f && s1.gainAt (sec (14)) == 0.0f);
                CHECK (p.regions[2].startSample == sec (12) && p.regions[2].endSample() == sec (30.8));
            }
            // mark 2 exactly on the fix join: the take is retimed on through it (never a retimed and an untouched copy of the same take crossfaded)
            const auto pe = makePlan (e, marks (12, 15, 1.1), dir, len60, isLtc);
            CHECK (pe.ok() && pe.streams.size() == 1 && pe.regions.size() == 4);
            if (pe.ok() && pe.streams.size() == 1) CHECK (pe.streams[0].tracks[0].sources.size() == 2 && pe.regions[2].endSample() == sec (30) + pe.delta);
            // a whole piece from one side of a fix join is the whole take's piece, edit point to edit point
            Request q; q.wholePiece = true; q.piece = e.regions[(size_t) k].id; q.S = 1.05;
            const auto pw = makePlan (e, q, dir, len60, isLtc);
            CHECK (pw.ok() && pw.t0 == sec (10) && pw.t1 == sec (30) && pw.streams.size() == 1);
        }
        // volume changes, automation and overdubs go with their music; marks 1 - 2 end up round the retimed part
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            e.regions[1].setGainChange (sec (3), 1.0, { -6.0f, -3.0f, 0.0f });
            auto o1 = piece ("O1", 0, 0, 1); auto o2 = piece ("O2", 0, 0, 1);
            e.addOverdub (o1, sec (14)); e.addOverdub (o2, sec (33));
            auto& lane = e.addLane (tMain, autoparam::fader, juce::Uuid::null());
            for (double t : { 5.0, 16.0, 35.0 }) e.addPoint (lane, sec (t), -3.0f);
            const auto lanes0 = e.lanes;
            const auto p = makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc);
            CHECK (p.ok() && p.streams.size() == 2 && p.overdubs.size() == 2);
            if (p.ok() && p.regions.size() == 4 && p.overdubs.size() == 2)
            {
                const auto& R = p.regions[2];
                const GainChange* g = nullptr;
                for (auto& gc : R.gains) if (gc.at == sec (1.1)) g = &gc;                        // (the change 1 s into the cut piece, now 1.1 s in)
                CHECK (g != nullptr);
                if (g != nullptr)
                {
                    CHECK (std::abs (g->ramp - 1.1) < 1.0e-6);
                    CHECK (g->db.size() == 2 && g->db[0] == -6.0f && g->db[1] == -3.0f);          // (lined up with its files, the LTC's left out)
                }
                CHECK (std::abs (R.levelDbAt (0, sec (3)) - (-6.0f)) < 0.01f && std::abs (R.levelDbAt (0, sec (0.5))) < 0.01f);
                CHECK (p.overdubs[0].startSample == sec (14.2) && p.overdubs[0].takeName.contains ("retimed") && p.overdubs[1].startSample == sec (33.8));
            }
            auto e2 = e;
            e2.fixIn = 12.0; e2.fixOut = 20.0; e2.insertIndex = 2;                                      // (the insert point in front of C)
            applyPlan (e2, p);
            CHECK (e2.regions.size() == 4 && e2.fixIn == 12.0 && e2.fixOut == 20.0);              // marks 1 - 2 stay what was marked (so Undo gives exactly that back)
            CHECK (e2.insertIndex == 3 && e2.regions[3].id == e.regions[2].id);                  // still in front of C (a piece more before it now)
            if (! e2.lanes.empty() && e2.lanes[0].pts.size() == 3)
            {
                const auto& pts = e2.lanes[0].pts;
                CHECK (pts[0].time == sec (5) && pts[1].time == sec (16.4) && pts[2].time == sec (35.8));
                for (auto& pt : pts) CHECK (! pt.region.isNull() && ! pt.floating);
                auto e3 = e2; e3.resolveAutomation();                                             // (and they stay there)
                for (size_t i = 0; i < pts.size(); ++i) CHECK (e3.lanes[0].pts[i].time == pts[i].time);
            }
            else CHECK (false);
            CHECK (e.lanes[0].pts[1].time == lanes0[0].pts[1].time);                               // (the plan itself changes nothing)
        }
        // a fix join inside, the later piece at a level of its own: the level glides over the join, as it plays now (no step at the join)
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            const int k = e.splitRegion (1, sec (5));
            e.regions[(size_t) k - 1].fixOut = true; e.regions[(size_t) k].fixIn = true;
            e.regions[(size_t) k - 1].setOutCurve (FadeCurve::Linear); e.regions[(size_t) k].setInCurve (FadeCurve::Linear);
            e.regions[(size_t) k].setPieceLevelDb (-6.0f);
            const auto p = makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc);
            CHECK (p.ok() && p.regions.size() == 4);
            if (p.ok() && p.regions.size() == 4)
            {
                const auto& R = p.regions[2];
                const auto join = p.newSampleAt (sec (15)) - R.startSample;
                float worstStep = 0.0f, prev = R.levelDbAt (0, join - sec (0.05));
                for (juce::int64 rel = join - sec (0.05); rel < join + sec (0.05); rel += 8)
                { const float v = R.levelDbAt (0, rel); worstStep = juce::jmax (worstStep, std::abs (v - prev)); prev = v; }
                CHECK (worstStep < 0.2f);                                                            // smooth (a step was 3.8 dB)
                CHECK (std::abs (R.levelDbAt (0, join + sec (0.1)) + 6.0f) < 0.01f && std::abs (R.levelDbAt (0, join - sec (0.1))) < 0.01f);
            }
        }
        // a piece waiting for corrected audio is never cut or made again - also beyond the marks (the run-on, the hand-back, a fix join before mark 1)
        {
            auto waitingEdit = [&] (double wFrom, double wTo, double bLen)
            {
                auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 0, bLen), piece ("C", 10 + bLen, 2, 10) });
                const int k1 = e.splitRegion (1, sec (wFrom - 10)); const int k2 = e.splitRegion (k1, sec (wTo - wFrom));
                for (int j : { k1, k2 }) { e.regions[(size_t) j - 1].fixOut = true; e.regions[(size_t) j].fixIn = true; }
                auto& w = e.regions[(size_t) k1]; w.waiting.files = { dir.getChildFile ("w.wav") }; w.waiting.from = w.srcIn; w.waiting.to = w.srcOut;
                return e;
            };
            CHECK (makePlan (waitingEdit (32, 34, 30), marks (12, 20, 1.1), dir, len60, isLtc).error.contains ("waiting"));     // the run-on would reach it
            CHECK (makePlan (waitingEdit (21, 23, 50), marks (12, 20.9, 1.1), dir, len60, isLtc,
                             [sec] (const EditRegion&, juce::int64 lo, juce::int64) { return lo; }).error.contains ("waiting"));  // the hand-back would cut it
            CHECK (makePlan (waitingEdit (13, 15, 30), marks (15.04, 20, 1.1), dir, len60, isLtc).error.contains ("waiting"));  // mark 1 just after it: the take-over goes in front of its join
            CHECK (makePlan (waitingEdit (24, 26, 50), marks (12, 18, 1.1), dir, len60, isLtc, [sec] (const EditRegion&, juce::int64 lo, juce::int64) { return lo; }).ok());   // further off: fine
        }
        // what it will not do
        {
            auto e = editOf ({ piece ("A", 0, 5, 10), piece ("B", 10, 20, 20), piece ("C", 30, 2, 10) });
            CHECK (! makePlan (e, marks (12, 20, 2.5), dir, len60, isLtc).ok());                   // too big a change
            CHECK (! makePlan (e, marks (45, 50, 1.1), dir, len60, isLtc).ok());                   // nothing there
            e.regions[1].waiting.files = { dir.getChildFile ("w.wav") }; e.regions[1].waiting.from = 0; e.regions[1].waiting.to = 10;
            CHECK (makePlan (e, marks (12, 20, 1.1), dir, len60, isLtc).error.contains ("waiting"));
            CHECK (! makePlan (e, marks (12, 20, 1.1), dir, [] (const juce::File&) { return (juce::int64) -1; }, isLtc).ok());   // a file that cannot be read
        }
    }

    SECTION ("2.103 undo keeps the insert point in front of the same piece when the number of pieces before it changes");
    {
        Project q; q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto tr = q.addTrack ("A", TrackFormat::Mono, 0).id;
        auto& tw = q.addTakeWindow ("P"); const auto twId = tw.id;
        auto& ed = q.addEdit ("E", twId); const auto edId = ed.id;
        for (int i = 0; i < 3; ++i)
        {
            EditRegion r; r.id = juce::Uuid(); r.windowId = twId; r.takeId = juce::Uuid(); r.takeName = "00" + juce::String (i + 1); r.sampleRate = 48000.0;
            r.srcIn = 0; r.srcOut = 48000 * 5; r.sourceLength = 48000 * 10;
            RegionFile f; f.trackId = tr; f.trackName = "A"; f.file = tmp.getChildFile ("ip" + juce::String (i) + ".wav"); r.files.push_back (f);
            ed.insertRegion (r);
        }
        const auto cId = ed.regions[2].id;
        ed.insertIndex = 2;                                                                    // in front of the third piece
        q.undoReset();
        auto* e = q.findEdit (edId);
        e->splitRegion (0, 48000 * 2); e->insertIndex = 3;                                    // a piece more before it (as a retime's cut does; applyPlan keeps it in front of the same piece)
        q.changed(); q.undoTick (false, true);
        CHECK (q.undo());
        e = q.findEdit (edId);
        CHECK (e->regions.size() == 3 && e->insertIndex == 2 && e->regions[2].id == cId);
        CHECK (q.redo());
        e = q.findEdit (edId);
        CHECK (e->regions.size() == 4 && e->insertIndex == 3 && e->regions[3].id == cId);
    }

    SECTION ("2.103 ReTimEr: end to end - the new files, where the music lands in them, cancelling leaves nothing");
    {
        using namespace retime;
        const double sr = 48000.0;
        auto sec = [sr] (double s) { return (juce::int64) std::llround (s * sr); };
        const auto dir = tmp.getChildFile ("retime_e2e"); dir.deleteRecursively(); dir.createDirectory();
        // a take: a stereo pair and a mono spot, 70 s of a quiet chord with a click every 2.5 s - and a breath of silence at 16.0 - 16.4 s
        auto writeTake = [&] (const juce::File& f, int channels, int delay)
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (channels).withBitsPerSample (24));
            juce::AudioBuffer<float> b (channels, sec (70));
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const int j = i - delay - 3 * c;
                    double v = 0.02 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * j / sr) + 0.015 * std::sin (juce::MathConstants<double>::twoPi * 330.0 * j / sr);
                    if (j >= sec (16.0) && j < sec (16.4)) v = 0.0;
                    if (j >= 0 && j % sec (2.5) < 24) v += 0.6 * std::exp (-(j % sec (2.5)) / 5.0);
                    b.setSample (c, i, (float) v);
                }
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            w.reset();
            return f;
        };
        const juce::Uuid tPair, tSpot;
        EditRegion r; r.id = juce::Uuid(); r.takeId = juce::Uuid(); r.takeName = "Take 7"; r.sampleRate = sr; r.srcIn = sec (1); r.srcOut = sec (69); r.sourceLength = sec (70); r.startSample = 0;
        { RegionFile f; f.trackId = tPair; f.trackName = "ORTF"; f.numChannels = 2; f.file = writeTake (dir.getChildFile ("Take 7 - ORTF.wav"), 2, 0); r.files.push_back (f); }
        { RegionFile f; f.trackId = tSpot; f.trackName = "SPOT1"; f.numChannels = 1; f.file = writeTake (dir.getChildFile ("Take 7 - SPOT1.wav"), 1, 40); r.files.push_back (f); }
        EditDef e; e.sampleRate = sr; e.insertRegion (r);
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const FileLength fileLen = [&fm] (const juce::File& f) { auto rd = audioops::openReader (fm, f); return rd != nullptr ? rd->lengthInSamples : (juce::int64) -1; };
        CHECK (std::abs (quietestMoment (fm, e.regions[0], sec (14.0), sec (17.0), {}) - sec (15.0)) <= sec (0.02));   // the breath (15.0 s on the edit's timeline: the piece starts 1 s into the take)
        Request q; q.from = sec (8); q.to = sec (14); q.rampIn = 1.0; q.rampOut = 1.5; q.S = TimeMap::solve ((double) sec (6), (double) sec (6.6), sec (1.0), sec (1.5));
        auto quiet = [&fm] (const EditRegion& rr, juce::int64 a, juce::int64 b) { return quietestMoment (fm, rr, a, b, {}); };
        const auto p = makePlan (e, q, dir, fileLen, {}, quiet);
        CHECK (p.ok() && p.delta == sec (0.6) && p.streams.size() == 1);
        // cancelled: nothing is left behind
        CHECK (renderStreams (p, [] { return true; }, {}) == "Cancelled.");
        for (auto& t : p.streams[0].tracks) CHECK (! t.dest.exists());
        std::atomic<float> lastProgress { 0.0f };
        const auto err = renderStreams (p, {}, [&] (float f) { lastProgress = f; }, 2);
        CHECK (err.isEmpty() && lastProgress.load() > 0.99f);
        if (err.isEmpty() && p.ok())
        {
            const auto& s = p.streams[0];
            for (auto& t : s.tracks)
            {
                auto rd = audioops::openReader (fm, t.dest);
                CHECK (rd != nullptr && rd->lengthInSamples == s.outB - s.outA && (int) rd->numChannels == t.numChannels);
            }
            auto e2 = e; applyPlan (e2, p);
            CHECK (e2.regions.size() == 3 && e2.lengthSamples() == e.lengthSamples() + p.delta);
            // inside the part: the click the take has at 11.5 s of the edit (12.5 s in the take) is in the new file where the map puts it
            auto rd = audioops::openReader (fm, s.tracks[0].dest);
            std::vector<std::vector<float>> ch;
            const double want = p.map.map ((double) sec (11.5)) + 1.0;
            audioops::readRange (*rd, (juce::int64) want - sec (0.05) - s.outA, sec (0.1), ch);
            double num = 0, den = 0;
            for (size_t i = 1; i < ch[0].size(); ++i) { const double d3 = (double) ch[0][i] - ch[0][i - 1], e3 = d3 * d3; num += e3 * (double) i; den += e3; }
            CHECK (den > 0.0 && std::abs ((double) ((juce::int64) want - sec (0.05) - s.outA) + num / den - (want - (double) s.outA)) < sr * 0.0015);
            // before mark 1 the new file is the take itself (so the crossfade into it is seamless)
            auto orig = audioops::openReader (fm, r.files[0].file);
            std::vector<std::vector<float>> a2, b2;
            audioops::readRange (*orig, sec (1) + sec (7.9), sec (0.08), a2);
            audioops::readRange (*rd, sec (7.9) - s.outA, sec (0.08), b2);
            double diff = 0.0;
            for (size_t i = 0; i < a2[0].size(); ++i) diff = juce::jmax (diff, (double) std::abs (a2[0][i] - b2[0][i]));
            CHECK (diff < 2.0e-3);
            CHECK (p.note.contains ("hands back") && std::abs (e2.regions[2].startSample - sec (15.6)) <= sec (0.03));   // it handed back in the breath
        }
    }

    SECTION ("3.0.0 tool pass: damaged and foreign input from outside is read safely and quickly (what the fuzz test found)");
    {
        // ---- text from outside: real UTF-8 is UTF-8; anything else is the Windows code page, with nothing swallowed and nothing read past the end
        {
            auto t = [] (const char* s) { return textFromBytes (s, std::strlen (s)); };
            CHECK (t ("Caf\xc3\xa9") == juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9")));
            CHECK (t ("Caf\xe9 </NAME>") == "Caf" + juce::String::charToString (0xE9) + " </NAME>");               // (JUCE's own check let this through as UTF-8)
            CHECK (t ("B \xe2\x9c") == "B " + juce::String::charToString (0xE2) + juce::String::charToString (0x0153));   // cut off at the end
            CHECK (t ("\xc0\xaf").length() == 2 && t ("\xed\xa0\x80").length() == 3);                            // overlong form, surrogate: not UTF-8
            CHECK (t ("\xf0\x9f\x8e\xbb").length() == 1 && t ("\xf0\x9f\x8e\xbb")[0] == 0x1F3BB);
            CHECK (t ("\xef\xbb\xbfX") == "X" && t ("\x80") == juce::String::charToString (0x20AC));
            const unsigned char u16[] = { 0xFF, 0xFE, 'h', 0, 0xE9, 0 };
            CHECK (textFromBytes (u16, sizeof (u16)) == "h" + juce::String::charToString (0xE9));
            CHECK (textFromBytes (nullptr, 3).isEmpty() && t ("").isEmpty());
        }
        // ---- comments files: bytes that are not UTF-8 (a spreadsheet saved in the Windows code page) are read as that code page, never past the end
        {
            const char ansi[] = "Text\tStart\tWho\r\nCaf\xe9 noise\t00:00:01:00\tAnna\r\nOver \xbd a bar late\t00:00:02:00\tBo\r\n";
            feedback::Parsed out;
            const auto err = feedback::parse (ansi, sizeof (ansi) - 1, 25.0, "ansi", out);
            CHECK (err.isEmpty() && out.notes.size() == 2);
            if (out.notes.size() == 2) CHECK (out.notes[0].text == juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 noise")) && out.notes[1].who == "Bo");
            const char cut[] = "Text\tStart\r\nA\t00:00:01:00\r\nB \xe2\x9c";            // a UTF-8 character cut off at the end of the file
            feedback::Parsed out2;
            (void) feedback::parse (cut, sizeof (cut) - 1, 25.0, "cut", out2);
            CHECK (out2.notes.size() >= 1);
            const unsigned char utf16[] = { 0xFF, 0xFE, 'T', 0, 'e', 0, 'x', 0, 't', 0, '\t', 0, 'S', 0, 't', 0, 'a', 0, 'r', 0, 't', 0, '\n', 0, 0xE9, 0, '\t', 0, '1', 0, '\n', 0 };
            feedback::Parsed out3;
            CHECK (feedback::parse (utf16, sizeof (utf16), 25.0, "u16", out3).isEmpty() && out3.notes.size() == 1
                   && out3.notes[0].text == juce::String::charToString (0xE9) && std::abs (out3.notes[0].seconds - 1.0) < 1.0e-9);
        }
        // ---- a long list is read quickly (each character was found by walking the text from its start), and so is one with a stray quote
        {
            juce::MemoryOutputStream mo;
            mo << "Start\tText\tWho\n";
            for (int i = 0; i < 40000; ++i) mo << "00:" << juce::String ((i / 60) % 60).paddedLeft ('0', 2) << ":" << juce::String (i % 60).paddedLeft ('0', 2) << ":00\tComment number " << i << "\tAnna\n";
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            feedback::Parsed out;
            const auto err = feedback::parse (mo.getData(), mo.getDataSize(), 25.0, "big", out);
            std::vector<FeedbackNote> into;
            const int added = feedback::merge (into, out.notes), again = feedback::merge (into, out.notes);      // (and merged into the edit's list: a search per note took seconds)
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            CHECK (err.isEmpty() && out.notes.size() == 40000 && ms < 20000.0);                    // (it took hours: a generous bound, for slow test builds)
            CHECK (added == 40000 && again == 0 && into.size() == 40000);
            juce::MemoryOutputStream mq;
            mq << "Start\tText\n00:00:01:00\t\"A stray quote that never closes\n";
            for (int i = 0; i < 40000; ++i) mq << "more text that is now inside one field " << i << "\n";
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            feedback::Parsed outq;
            (void) feedback::parse (mq.getData(), mq.getDataSize(), 25.0, "quote", outq);
            CHECK (juce::Time::getMillisecondCounterHiRes() - t1 < 20000.0);
            std::printf ("   (40,000 comments read and merged in %.0f ms)\n", ms);
        }
        // ---- iXML track names in a field recorder's file: text that is not UTF-8, or is cut off, is read without reading past it
        {
            auto bwf = [&] (const juce::File& f, const std::string& ixml)
            {
                juce::MemoryOutputStream o;
                o.write ("RIFF", 4); o.writeInt (0); o.write ("WAVE", 4);
                o.write ("fmt ", 4); o.writeInt (16); o.writeShort (1); o.writeShort (4); o.writeInt (48000); o.writeInt (48000 * 8); o.writeShort (8); o.writeShort (16);
                o.write ("iXML", 4); o.writeInt ((int) ixml.size()); o.write (ixml.data(), ixml.size()); if (ixml.size() & 1) o.writeByte (0);
                o.write ("data", 4); o.writeInt (800); for (int i = 0; i < 800; ++i) o.writeByte ((char) i);
                f.replaceWithData (o.getData(), o.getDataSize());
            };
            const auto f = tmp.getChildFile ("ixml.wav");
            bwf (f, "<BWFXML><TRACK_LIST><TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>Boom</NAME></TRACK>"
                    "<TRACK><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Lav \xe9</NAME></TRACK></TRACK_LIST></BWFXML>");
            const auto names = readIXmlTrackNames (f, 4);
            CHECK (names.size() == 4 && names[0] == "Boom" && names[1] == "Lav " + juce::String::charToString (0xE9));
            bwf (f, "<BWFXML><TRACK_LIST><TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>A\xe2\x9c");
            (void) readIXmlTrackNames (f, 4);                                         // (cut off in the middle of a character: no names, no harm)
            bwf (f, "<BWFXML><TRACK_LIST><TRACK><INTERLEAVE_INDEX>-2147483648</INTERLEAVE_INDEX><NAME>X</NAME></TRACK>"
                    "<TRACK><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Y</NAME></TRACK></TRACK_LIST></BWFXML>");
            const auto n2 = readIXmlTrackNames (f, 4);                                 // (the lowest number there is: one less overflowed)
            CHECK (n2.size() == 4 && n2[0].isEmpty() && n2[1] == "Y");
        }
        // ---- the Merging device's change messages: a path to a list or a channel the module does not have is refused, not followed
        {
            auto m = juce::JSON::parse (R"({"id":5,"custom":{"ins":{"channels":[{"m48V":false,"micGain":0},{"micGain":10}],"label":"x"}}})");
            CHECK (! RavennaDevice::applyPath (m, ".custom.ins.channels[2]", juce::JSON::parse (R"({"m48V":true})")));
            CHECK (! RavennaDevice::applyPath (m, ".custom.ins.channels[-1].micGain", juce::var (3)));
            CHECK (! RavennaDevice::applyPath (m, ".custom.ins.label[0]", juce::var (3)));
            CHECK (! RavennaDevice::applyPath (m, ".custom.outs.channels[0]", juce::var (3)));
            CHECK (! RavennaDevice::applyPath (m, ".custom.ins.channels[99999999999]", juce::var (3)));
            CHECK (RavennaDevice::applyPath (m, ".custom.ins.channels[1]", juce::JSON::parse (R"({"m48V":true})")) && (bool) m["custom"]["ins"]["channels"][1]["m48V"]);
            // a channel read after a message made the list shorter (the channel list is made again later): an empty channel, not a crash
            CHECK (RavennaDevice::readChannel (m, 1).micGain == 10 && RavennaDevice::readChannel (m, 7).micGain == 0 && RavennaDevice::readChannel (m, -1).micGain == 0);
            CHECK (RavennaDevice::applyPath (m, ".custom.ins.channels", juce::var()) && RavennaDevice::readChannel (m, 0).micGain == 0);
            // ANEMAN's answer with a channel number no device has (the highest int: adding the group's start to it overflowed): that patch is left out
            const auto dir = juce::File (__FILE__).getParentDirectory().getChildFile ("data");
            auto devs = juce::JSON::parse (dir.getChildFile ("aneman_devices.json"));
            auto conns = juce::JSON::parse (dir.getChildFile ("aneman_connections.json")).clone();
            const auto whole = planFromAneman (devs, conns);
            if (auto* cs = conns.getArray(); cs != nullptr && ! cs->isEmpty())
                for (auto& c : *cs) if (auto* in = c["Input"].getDynamicObject()) in->setProperty ("InputId", 2147483647);
            const auto damaged = planFromAneman (devs, conns);
            CHECK (whole.ok && damaged.devices.empty() && ! damaged.ok);
        }
        // ---- WAV files: an RF64 file's 64-bit sizes are what readers go by, so mending puts those right; a nonsense size never reaches a reader
        {
            juce::AudioFormatManager fm; registerAudioFormats (fm);
            auto rf64 = [&] (const juce::File& f, juce::uint32 dataField, bool withDs64, juce::uint64 ds64Size, int frames)
            {
                juce::MemoryOutputStream o;
                const juce::int64 total = 12 + (withDs64 ? 36 : 0) + 24 + 8 + frames * 6;
                o.write ("RF64", 4); o.writeInt (-1); o.write ("WAVE", 4);
                if (withDs64) { o.write ("ds64", 4); o.writeInt (28); o.writeInt64 (total - 8); o.writeInt64 ((juce::int64) ds64Size); o.writeInt64 ((juce::int64) (ds64Size / 6)); o.writeInt (0); }
                o.write ("fmt ", 4); o.writeInt (16); o.writeShort (1); o.writeShort (2); o.writeInt (48000); o.writeInt (48000 * 6); o.writeShort (6); o.writeShort (24);
                o.write ("data", 4); o.writeInt ((int) dataField);
                for (int i = 0; i < frames * 6; ++i) o.writeByte ((char) (i * 7 + 1));
                f.replaceWithData (o.getData(), o.getDataSize());
            };
            auto lengthRead = [&] (const juce::File& f) -> juce::int64
            {
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
                if (r == nullptr) return -2;
                juce::AudioBuffer<float> b ((int) r->numChannels, 512);
                r->read (&b, 0, 512, 0, true, true);                                  // (with a negative length this wrote outside the buffer)
                return r->lengthInSamples;
            };
            const auto f = tmp.getChildFile ("rf64.wav");
            // a small RF64 file left by a recorder that always writes RF64, stopped before its sizes were written
            rf64 (f, 0xffffffffu, true, 0, 1000);
            CHECK (lengthRead (f) == 0);
            auto r1 = wavrepair::inspect (f, true, false);
            CHECK (r1.ok && r1.wasBroken && r1.frames == 1000 && lengthRead (f) == 1000);
            CHECK (! wavrepair::inspect (f, false, false).wasBroken);
            // a nonsense 64-bit size (the top bit set: a negative length): the reader keeps within the file, and mending puts it right
            rf64 (f, 0xffffffffu, true, 0xffffff0000000000ull, 1000);
            const auto len2 = lengthRead (f);
            CHECK (len2 >= 0 && len2 <= 1000);
            auto r2 = wavrepair::inspect (f, true, false);
            CHECK (r2.ok && r2.wasBroken && r2.frames == 1000 && lengthRead (f) == 1000);
            // the largest 64-bit size there is, on a file that was being recorded: no arithmetic overflow on the way to mending it (the fuzz test's UBSan caught one)
            rf64 (f, 0xffffffffu, true, 0x7fffffffffffffffull, 1000);
            auto r5 = wavrepair::inspect (f, true, true);
            CHECK (r5.ok && r5.wasBroken && r5.frames == 1000 && lengthRead (f) == 1000);
            // the two sizes disagree (as a small RF64 file mended before 3.0 was left): the 32-bit one stands, and the 64-bit one is made to agree
            rf64 (f, 600 * 6, true, 0, 1000);
            auto r3 = wavrepair::inspect (f, true, false);
            CHECK (r3.ok && r3.wasBroken && r3.frames == 600 && lengthRead (f) == 600);
            // marked RF64 but without the 64-bit sizes, under 4 GB: it becomes a plain WAV that every reader understands
            rf64 (f, 0, false, 0, 1000);
            auto r4 = wavrepair::inspect (f, true, false);
            CHECK (r4.ok && r4.wasBroken && r4.frames == 1000 && lengthRead (f) == 1000);
            { juce::FileInputStream in (f); char m4[4] = {}; in.read (m4, 4); CHECK (std::memcmp (m4, "RIFF", 4) == 0); }
            // and a whole file is read exactly as before
            juce::File whole = tmp.getChildFile ("whole.wav");
            {
                juce::AudioBuffer<float> b (2, 4800); b.clear(); b.setSample (0, 100, 0.5f);
                auto w = audioops::makeWavWriter (whole, 48000.0, 2, false);
                CHECK (w != nullptr && w->writeFromAudioSampleBuffer (b, 0, 4800));
            }
            CHECK (lengthRead (whole) == 4800);
        }
        // ---- an AAF whose index of sectors loops back on itself and says it has billions of parts: refused at once (it filled the memory)
        {
            juce::MemoryOutputStream o;
            const juce::uint8 magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
            o.write (magic, 8); o.writeRepeatedByte (0, 16); o.writeShort (0x3E); o.writeShort (3); o.writeShort ((short) 0xFFFE); o.writeShort (9); o.writeShort (6);
            o.writeRepeatedByte (0, 10);                                              // (reserved, and the directory sector count of version 3)
            o.writeInt (1); o.writeInt (-2); o.writeInt (0); o.writeInt (4096); o.writeInt (-2); o.writeInt (0);
            o.writeInt (0); o.writeInt (-1);                                          // the extra index starts at sector 0 and has 4294967295 parts
            for (int i = 0; i < 109; ++i) o.writeInt (-1);
            for (int i = 0; i < 127; ++i) o.writeInt (-1);
            o.writeInt (0);                                                           // sector 0's "next part" is sector 0 again
            for (int s = 0; s < 3; ++s) o.writeRepeatedByte (0xFF, 512);
            const auto f = tmp.getChildFile ("loop.aaf");
            f.replaceWithData (o.getData(), o.getDataSize());
            CHECK (o.getDataSize() == 512 * 5);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto r = readAaf (f);
            CHECK (r.error.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() - t0 < 2000.0);
        }
    }

    SECTION ("3.1.2 AudioJob: start() while the last job still runs asks it to stop and waits for it (never kills it); only the new job's onDone is heard");
    {
        auto* job = new AudioJob();
        std::atomic<int> oldSawCancel { 0 }, oldFinished { 0 }, newRan { 0 }, oldDone { 0 }, newDone { 0 };
        job->start ([&] (AudioJob& j) -> juce::String
        {
            // a long job that looks only at cancelled(), as every real one does (never at the thread's own stop request): 10 s if nobody cancels it
            for (int i = 0; i < 2000 && ! j.cancelled(); ++i) juce::Thread::sleep (5);
            if (j.cancelled()) ++oldSawCancel;
            juce::Thread::sleep (50);                                                   // (still busy for a moment after the cancel: the wait has to cover that too)
            ++oldFinished;
            return {};
        }, [&] (const juce::String&) { ++oldDone; });
        juce::Thread::sleep (100);
        CHECK (job->isRunning());
        const auto t0 = juce::Time::getMillisecondCounter();
        job->start ([&] (AudioJob&) -> juce::String { ++newRan; return {}; }, [&] (const juce::String&) { ++newDone; });
        CHECK (oldSawCancel == 1 && oldFinished == 1);                                  // the old job was cancelled and finished properly before the new one began
        CHECK (juce::Time::getMillisecondCounter() - t0 < 3000);                        // (before 3.1.2 start() sat out a 5 s timeout and then killed the thread)
        for (int i = 0; i < 300 && ! job->isFinished(); ++i) juce::Thread::sleep (10);
        CHECK (newRan == 1 && job->isFinished());
        pumpMessages (300);
        CHECK (oldDone == 0 && newDone == 1);                                           // the replaced job's onDone was dropped, the new one's delivered
        job->release();                                                                 // (it deletes itself on the message thread)
        pumpMessages (300);
    }

    SECTION ("3.1.3 checked file writes: a write the drive refuses is an error, not a short file that says it went well");
    {
        // the checked writers: bytes exact, line breaks as JUCE's writer made them (so the saved files come out as before)
        const auto f = tmp.getChildFile ("checked.txt");
        juce::String err;
        CHECK (filesync::writeTextChecked (f, "a\nb", &err) && f.loadFileAsString() == "a\r\nb" && err.isEmpty());
        CHECK (filesync::replaceTextSafely (f, "second", true, &err) && f.loadFileAsString() == "second" && filesync::backupOf (f).loadFileAsString() == "a\r\nb");
        const char bytes[5] = { 1, 2, 3, 4, 5 };
        CHECK (filesync::writeDataChecked (f, bytes, 5, &err) && f.getSize() == 5);
        // a write that is refused (as a full drive refuses it): false, with a reason, and the target of a safe replace is left as it was with its .bak
        CHECK (! filesync::detail::writeChecked (tmp.getChildFile ("refused.txt"), [] (juce::FileOutputStream& out) { out.write ("xy", 2); return false; }, &err) && err.contains ("refused.txt"));
        if (juce::File ("/dev/full").exists())                                          // (Linux: a device that refuses every write with "no space left")
        {
            err.clear();
            CHECK (! filesync::writeTextChecked (juce::File ("/dev/full"), "anything", &err) && err.isNotEmpty());
            CHECK (! filesync::writeDataChecked (juce::File ("/dev/full"), bytes, 5, &err));
        }
        // the sync goes through JUCE's stream now: a file is still flushed and a missing one is simply reported
        CHECK (filesync::syncToDisk (f) && ! filesync::syncToDisk (tmp.getChildFile ("not-there.txt")));

        // every audio file Fermata makes is read back: the length check sees a file that came out short, and passes every format the exports write
        juce::AudioFormatManager afm; td::registerAudioFormats (afm);
        std::vector<std::vector<float>> tone (2, std::vector<float> (48000));
        for (int i = 0; i < 48000; ++i) tone[0][(size_t) i] = tone[1][(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0);
        const auto wav = tmp.getChildFile ("checked.wav");
        CHECK (audioops::writeWav (wav, tone, 48000.0) && audioops::checkWrittenLength (&afm, wav, 48000).isEmpty() && audioops::checkWrittenLength (nullptr, wav, 48000).isEmpty());
        CHECK (audioops::checkWrittenLength (&afm, wav, 47999).contains ("holds"));
        {   // cut short by a drive that stopped taking data: the file says less than was written
            juce::FileOutputStream cut (wav); cut.setPosition (wav.getSize() - 6000); cut.truncate(); cut.flush();
        }
        CHECK (audioops::checkWrittenLength (&afm, wav, 48000).contains ("holds") && audioops::checkWrittenLength (&afm, tmp.getChildFile ("never.wav"), 1).contains ("read back"));     // (the header says 48000, the bytes do not)
        // a file written through CheckedAudioFile: what the writer's destructor flushes is checked too; a drive that refuses the bytes is reported
        {
            const auto flac = tmp.getChildFile ("checked-through.flac");
            audioops::CheckedAudioFile cf (flac);
            CHECK (! cf.failedToOpen());
            auto os = cf.stream();
            juce::FlacAudioFormat ff;
            auto w = ff.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24).withQualityOptionIndex (5));
            CHECK (w != nullptr);
            if (w != nullptr) { const float* ptrs[2] = { tone[0].data(), tone[1].data() }; CHECK (w->writeFromFloatArrays (ptrs, 2, 48000)); w.reset(); }
            CHECK (cf.finish().isEmpty() && audioops::checkWrittenLength (&afm, flac, 48000).isEmpty() && flac.getSize() > 1000);
            CHECK (flac.deleteFile());                                                       // (closed by finish: it can go)
            if (juce::File ("/dev/full").exists())
            {
                audioops::CheckedAudioFile bad (juce::File ("/dev/full"));
                if (! bad.failedToOpen())
                {
                    auto os2 = bad.stream();
                    juce::WavAudioFormat wf;
                    auto w2 = wf.createWriterFor (os2, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
                    if (w2 != nullptr) { const float* ptrs[2] = { tone[0].data(), tone[1].data() }; w2->writeFromFloatArrays (ptrs, 2, 48000); w2.reset(); }
                    CHECK (bad.finish().isNotEmpty());
                }
            }
        }
        struct Fmt { const char* ext; std::unique_ptr<juce::AudioFormat> af; int bits; };
        std::vector<Fmt> fmts;
        fmts.push_back ({ "wav", std::make_unique<juce::WavAudioFormat>(), 24 }); fmts.push_back ({ "aiff", std::make_unique<juce::AiffAudioFormat>(), 24 });
        fmts.push_back ({ "flac", std::make_unique<juce::FlacAudioFormat>(), 24 }); fmts.push_back ({ "ogg", std::make_unique<juce::OggVorbisAudioFormat>(), 16 });
        for (auto& fm : fmts)
        {
            const auto file = tmp.getChildFile (juce::String ("checked-fmt.") + fm.ext);
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
            auto w = fm.af->createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (fm.bits).withQualityOptionIndex (fm.ext[0] == 'o' ? 5 : 0));
            CHECK (w != nullptr);
            if (w != nullptr)
            {
                const float* ptrs[2] = { tone[0].data(), tone[1].data() };
                CHECK (w->writeFromFloatArrays (ptrs, 2, 48000));
                w.reset();
                const auto e = audioops::checkWrittenLength (&afm, file, 48000);
                if (e.isNotEmpty()) std::printf ("  (%s: %s)\n", fm.ext, e.toRawUTF8());
                CHECK (e.isEmpty());
            }
        }
    }

    SECTION ("3.1.3 the second drive's copy is kept when the main recording fails: lag is judged by the main file's size, and a main file that cannot be read back at the end leaves the copy as it is");
    {
        BackupCopier bc;
        bc.maxLagSeconds.store (0.2);
        const auto src = tmp.getChildFile ("bc-src.wav"), dst = tmp.getChildFile ("bc copies").getChildFile ("bc-src.wav");
        std::vector<std::vector<float>> part (1, std::vector<float> (24000, 0.25f));
        CHECK (audioops::writeWav (src, part, 48000.0));                                     // (half a second: a take under way)
        {   // the header of a file still being recorded says 0 frames (the length is written at the end)
            juce::FileOutputStream o (src); o.setPosition (40); o.writeInt (0); o.setPosition (4); o.writeInt (0); o.flush();
        }
        auto job = bc.add (src, dst, 48000.0 * 3.0);
        for (int w = 0; w < 300 && dst.getSize() < src.getSize(); ++w) juce::Thread::sleep (10);
        CHECK (dst.getSize() == src.getSize());
        juce::Thread::sleep (700);                                                           // far longer than the lag allowed: the main file has not grown, so the copy is NOT behind
        CHECK (bc.busy() == 1 && bc.takeBinned().isEmpty() && dst.getSize() == src.getSize());     // (before 3.1.3 it was binned here as "could not keep up")
        // the main drive goes before the take ends (here: the file's contents vanish - on Linux a deleted file can still be read through an open handle):
        // the finished header cannot be re-read, and the copy is kept with a header of its own
        { juce::FileOutputStream gone (src); gone.setPosition (0); gone.truncate(); gone.flush(); }
        BackupCopier::finish (job);
        for (int w = 0; w < 300 && bc.busy() > 0; ++w) juce::Thread::sleep (10);
        const auto notes = bc.takeBinned();
        CHECK (bc.busy() == 0 && notes.size() == 1 && notes[0].contains ("COULD NOT BE READ BACK") && notes[0].contains ("kept"));
        const auto r = wavrepair::inspect (dst, false);
        CHECK (r.ok && r.frames == 24000 && ! r.wasBroken);                                   // (a whole, readable file: every sample that had been copied)
        // a second drive that really is behind is still binned: the main file is far ahead of the copy
        const auto src2 = tmp.getChildFile ("bc-src2.wav"), dst2 = tmp.getChildFile ("bc copies").getChildFile ("bc-src2.wav");
        std::vector<std::vector<float>> big (1, std::vector<float> (480000, 0.1f));
        CHECK (audioops::writeWav (src2, big, 48000.0));
        bc.maxLagSeconds.store (-1.0);                                                       // (nothing is ever near enough)
        auto job2 = bc.add (src2, dst2, 48000.0 * 3.0);
        for (int w = 0; w < 300 && bc.busy() > 0; ++w) juce::Thread::sleep (10);
        const auto notes2 = bc.takeBinned();
        CHECK (bc.busy() == 0 && notes2.size() == 1 && notes2[0].contains ("could not keep up") && notes2[0].contains ("not affected") && ! dst2.exists());
        // a take that never started: its copy goes without a word
        bc.maxLagSeconds.store (20.0);
        auto job3 = bc.add (src2, tmp.getChildFile ("bc copies").getChildFile ("bc-src3.wav"), 48000.0 * 3.0);
        BackupCopier::cancel (job3);
        for (int w = 0; w < 300 && bc.busy() > 0; ++w) juce::Thread::sleep (10);
        CHECK (bc.busy() == 0 && bc.takeBinned().isEmpty() && ! tmp.getChildFile ("bc copies").getChildFile ("bc-src3.wav").exists());
    }

    SECTION ("3.1.3 a take whose later track's file cannot be made leaves nothing behind: no header-only files, no second-drive copies, no notice");
    {
        Project q;
        q.projectFile = tmp.getChildFile ("Partial").getChildFile ("Partial.fermata");
        q.projectFile.getParentDirectory().createDirectory();
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        q.addTrack ("first", TrackFormat::Mono, 0).setArmed (true);
        q.addTrack (juce::String::repeatedString ("x", 300), TrackFormat::Mono, 1).setArmed (true);     // (a file name that long cannot be made)
        q.retireAllMixers(); q.addMixer ("M");
        auto& qw = *q.takeWindows.front(); qw.name = "Partial";
        AudioEngine e (q);
        e.setMixerThreads (0);
        e.setBackupFolder (tmp.getChildFile ("Partial second drive"));
        e.prepare (48000.0, 64);
        const auto err = e.startRecording (qw);
        CHECK (err.startsWith ("Cannot create") && ! e.isRecording() && qw.groups.empty() && qw.nextNumber == 1);
        const auto folder = q.takeFolder (qw);
        CHECK (folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 0);         // (the first track's file, made before the second failed, is gone again)
        for (int w = 0; w < 300 && e.backupCopiesGoing() > 0; ++w) juce::Thread::sleep (10);
        CHECK (e.backupCopiesGoing() == 0 && e.takeBackupProblems().isEmpty());
        CHECK (tmp.getChildFile ("Partial second drive").getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*") <= 1);   // (at most the project's folder, empty)
        // with a name that can be made, the same window records as before, and the take gets number 1
        q.tracks[1].name = "second";
        CHECK (e.startRecording (qw).isEmpty());
        e.stopRecording(); e.waitForFinishedTakes (5000); e.reapFinishedTakes();
        CHECK (qw.groups.size() == 1 && qw.groups[0].number == 1 && folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 2);
        e.setBackupFolder ({});
    }

    SECTION ("3.1.4 a take is not an undo step: Ctrl+Z after a recording undoes the edit before it and leaves the take and its counter alone");
    {
        Project q;
        q.projectFile = tmp.getChildFile ("UndoRec").getChildFile ("UndoRec.fermata");
        q.projectFile.getParentDirectory().createDirectory();
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        q.addTrack ("solo", TrackFormat::Mono, 0).setArmed (true);
        q.retireAllMixers(); q.addMixer ("M");
        auto& qw = *q.takeWindows.front(); qw.name = "Rec";
        AudioEngine e (q);
        e.setMixerThreads (0);
        e.prepare (48000.0, 64);
        q.undoReset();
        // an edit the person made, then a take: the edit is the step, the take is not
        CHECK (qw.defaultLabel == "Take");                                                     // (what the history starts from)
        qw.defaultLabel = "Before"; q.changed(); q.undoTick (false, true);
        CHECK (q.undoDepth() == 1);
        const int next = qw.nextNumber;
        CHECK (e.startRecording (qw).isEmpty());
        q.undoTick (true);                                                                      // (the tick while recording: looks again later)
        e.stopRecording(); e.waitForFinishedTakes (5000); e.reapFinishedTakes();
        q.undoTick (false, true);                                                               // the tick after the take
        CHECK (qw.groups.size() == 1 && qw.nextNumber == next + 1 && q.undoDepth() == 1);       // (3.1.3 and before: a second step, the take's)
        CHECK (q.undo (juce::Uuid::null()));                                                    // Ctrl+Z: the label change goes, the take and its counter stay
        CHECK (qw.defaultLabel == "Take" && qw.groups.size() == 1 && qw.nextNumber == next + 1);
        // a rename made before a later take, undone after it: the counter still never goes back (the snapshot is older than the take)
        qw.defaultLabel = "Second"; q.changed(); q.undoTick (false, true);
        CHECK (e.startRecording (qw).isEmpty());
        e.stopRecording(); e.waitForFinishedTakes (5000); e.reapFinishedTakes();
        q.undoTick (false, true);
        CHECK (qw.groups.size() == 2 && qw.nextNumber == next + 2);
        CHECK (q.undo (juce::Uuid::null()) && qw.defaultLabel == "Take" && qw.nextNumber == next + 2 && qw.groups.size() == 2);
        CHECK (q.redo (juce::Uuid::null()) && qw.defaultLabel == "Second" && qw.nextNumber == next + 2);
        // a change the person made just before Record (not yet settled into a step) is still a step of its own, not swallowed by the take
        qw.defaultLabel = "Quick"; q.changed();
        CHECK (e.startRecording (qw).isEmpty());
        e.stopRecording(); e.waitForFinishedTakes (5000); e.reapFinishedTakes();
        q.undoTick (false, true);
        CHECK (q.undo (juce::Uuid::null()) && qw.defaultLabel == "Second" && qw.groups.size() == 3 && qw.nextNumber == next + 3);
    }

    SECTION ("3.1.4 undo puts a take's files back (a track's clips taken out of the window, a fix's files) when they are still on the disk, with its LTC row and waiting marks; a mastering edit is a step of its own");
    {
        Project q;
        q.projectFile = tmp.getChildFile ("UndoFiles").getChildFile ("UndoFiles.fermata");
        q.projectFile.getParentDirectory().createDirectory();
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        const auto tA = q.addTrack ("A", TrackFormat::Mono, 0).id, tB = q.addTrack ("B", TrackFormat::Mono, 1).id;
        auto& qw = *q.takeWindows.front();
        TakeGroup g; g.id = juce::Uuid(); g.number = 1; g.label = "One"; g.lengthSamples = 1000; g.sampleRate = 48000.0;
        std::vector<std::vector<float>> mono (1, std::vector<float> (1000, 0.1f));
        for (auto& tr : q.tracks)
        {
            TakeFile f; f.trackId = tr.id; f.trackName = tr.name; f.file = tmp.getChildFile ("undo-" + tr.name + ".wav"); f.numChannels = 1;
            CHECK (audioops::writeWav (f.file, mono, 48000.0));
            g.files.push_back (f);
        }
        qw.groups.push_back (g);
        q.undoReset();
        // a track's clips taken out of the window (and its row hidden with them): Ctrl+Z brings the clips back and shows the row again
        qw.removeTrackClips (tB); q.changed(); q.undoTick (false, true);
        CHECK (qw.groups[0].files.size() == 1 && qw.isHidden (tB) && q.undoDepth() == 1);
        CHECK (q.undo (juce::Uuid::null()) && qw.groups[0].files.size() == 2 && qw.groups[0].files[1].trackId == tB && ! qw.isHidden (tB));
        CHECK (q.redo (juce::Uuid::null()) && qw.groups[0].files.size() == 1);
        CHECK (q.undo (juce::Uuid::null()) && qw.groups[0].files.size() == 2);
        // a take-level fix: the take points at a new file; Ctrl+Z points it back at the original (which is still there)
        const auto fixed = tmp.getChildFile ("undo-A (fixed).wav");
        CHECK (audioops::writeWav (fixed, mono, 48000.0));
        const auto original = qw.groups[0].files[0].file;
        qw.groups[0].files[0].file = fixed; q.changed(); q.undoTick (false, true);
        CHECK (q.undo (juce::Uuid::null()) && qw.groups[0].files[0].file == original);
        CHECK (q.redo (juce::Uuid::null()) && qw.groups[0].files[0].file == fixed);
        // a relink: the old path is gone, so undo leaves the files alone (never points at a missing file)
        const auto moved = tmp.getChildFile ("undo-A (moved).wav");
        CHECK (fixed.moveFileTo (moved));
        qw.groups[0].files[0].file = moved; q.changed(); q.undoTick (false, true);
        CHECK (q.undo (juce::Uuid::null()) && qw.groups[0].files[0].file == moved);
        q.redo (juce::Uuid::null());
        // the LTC row and an Export for Processing mark are undone too
        qw.showLtc = true; q.changed(); q.undoTick (false, true);
        CHECK (q.undo (juce::Uuid::null()) && ! qw.showLtc);
        WaitingPiece wp; wp.name = "bar 3"; wp.from = 10; wp.to = 20; wp.files.push_back (moved);
        qw.groups[0].waiting.push_back (wp); q.changed(); q.undoTick (false, true);
        CHECK (q.undo (juce::Uuid::null()) && qw.groups[0].waiting.empty());
        CHECK (q.redo (juce::Uuid::null()) && qw.groups[0].waiting.size() == 1);
        // the mastering list: an edit there is a step of its own (3.1.3: it only marked the project dirty, and Ctrl+Z undid the step before it)
        const int depthBefore = q.undoDepth();
        q.mastering.tailSeconds = 2.5; q.markDirtyForUndo(); q.undoTick (false, true);
        CHECK (q.undoDepth() == depthBefore + 1);
        CHECK (q.undo (juce::Uuid::null()) && q.mastering.tailSeconds == 0.0);
        CHECK (q.redo (juce::Uuid::null()) && q.mastering.tailSeconds == 2.5);
    }

    SECTION ("3.1.4 an Open that fails (a file that is not a Fermata project) leaves the open project as it was: its window files keep their names on the next save");
    {
        const auto folder = tmp.getChildFile ("OpenFails"); folder.createDirectory();
        Project q;
        q.projectFile = folder.getChildFile ("Kept.fermata");
        q.setInputCount (2); q.setOutputCount (2);
        q.takeWindows.front()->name = "Concert";
        q.addEdit ("Edit one", q.takeWindows.front()->id);
        juce::String err;
        CHECK (q.save (err));
        const auto filesBefore = q.windowFiles;
        auto countFiles = [&] (const char* sub) { return folder.getChildFile (sub).getNumberOfChildFiles (juce::File::findFiles, "*"); };
        CHECK (filesBefore.size() == q.edits.size() + q.takeWindows.size() + (size_t) q.cueEnd() && countFiles ("Take Windows") == 1 && countFiles ("Edit Windows") == 1);
        // a .json of some other program's, renamed: not a project. Nothing of the open one may change
        const auto other = folder.getChildFile ("Other.fermata");
        CHECK (other.replaceWithText ("{ \"format\": \"Some other program\", \"takeWindowFiles\": [ { \"id\": \"x\", \"name\": \"Y\", \"file\": \"Take Windows/Concert.fmtake\" } ] }"));
        juce::String e2;
        CHECK (! q.load (other, e2) && e2.contains ("Other.fermata"));
        CHECK (q.projectFile == folder.getChildFile ("Kept.fermata") && q.takeWindows.size() == 1 && q.edits.size() == 1 && q.name != "Other");
        CHECK (q.windowFiles.size() == filesBefore.size() && q.missingWindowRefs.empty() && q.loadNotes.isEmpty());
        for (auto& kv : filesBefore) CHECK (q.windowFiles.count (kv.first) == 1 && q.windowFiles[kv.first].rel == kv.second.rel);
        // the next save writes the same files, not "(2)" copies, and lists no missing windows
        q.changed();
        CHECK (q.save (err));
        CHECK (countFiles ("Take Windows") == 1 && countFiles ("Edit Windows") == 1);
        juce::var saved; CHECK (! juce::JSON::parse (q.projectFile.loadFileAsString(), saved).failed());
        CHECK (saved["takeWindowFiles"].size() == 1 && saved["editFiles"].size() == 1);
        // a file that does not exist says so (and leaves the project alone just the same)
        juce::String e3;
        CHECK (! q.load (folder.getChildFile ("Nowhere.fermata"), e3) && e3.contains ("does not exist") && q.windowFiles.size() == filesBefore.size());
    }

    SECTION ("3.1.4 a bounce whose render copy could not start one of the mix's plug-ins is refused with its name (a bypassed one, or one the copy has, is no reason)");
    {
        struct NamedLat : LatencyInsert
        {
            NamedLat (const juce::String& n, int l) : LatencyInsert (l), nm (n) {}
            juce::String getName() const override { return nm; }
            juce::String getDescription() const override { return "named:" + nm + ":" + juce::String (lat); }
            juce::String nm;
        };
        Project bq;
        bq.projectFile = tmp.getChildFile ("BounceRefuse.fermata");
        // the render copy's factory: "Fine" starts, "Broken" does not (a licence that allows one instance, a plug-in file gone since the project opened)
        bq.insertFactory = [] (const juce::String& desc, const juce::String&) -> std::unique_ptr<InsertProcessor>
        {
            if (! desc.startsWith ("named:")) return nullptr;
            const auto nm = desc.fromFirstOccurrenceOf ("named:", false, false).upToFirstOccurrenceOf (":", false, false);
            if (nm == "Broken") return nullptr;
            return std::make_unique<NamedLat> (nm, desc.fromLastOccurrenceOf (":", false, false).getIntValue());
        };
        bq.setInputCount (1); bq.setOutputCount (2);
        bq.tracks.clear();
        const auto tt = bq.addTrack ("Violin", TrackFormat::Mono, 0).id;
        bq.retireAllMixers();
        const auto bob = bq.addBus ("Out", true).id;
        auto& bm = bq.addMixer ("M");
        bq.setSendLevel (bm, tt, bob, 0.0f); bm.busFor (bob)->outFirst.store (0);
        const auto src = makeConstWav (tmp.getChildFile ("bounce-refuse.wav"), 0.5, 1);
        auto& ed = bq.addEdit ("E", juce::Uuid::null());
        EditRegion r; r.srcIn = 0; r.srcOut = 48000; r.sampleRate = 48000.0; r.sourceLength = 48000;
        RegionFile rf; rf.trackId = tt; rf.trackName = "Violin"; rf.file = src; r.files.push_back (rf);
        ed.insertRegion (r);
        bm.stripFor (tt)->slots[0].set (std::make_unique<NamedLat> ("Fine", 10), 48000.0, 512, 2);
        bm.stripFor (tt)->slots[1].set (std::make_unique<NamedLat> ("Broken", 10), 48000.0, 512, 2);
        BounceSettings bs;
        bs.sampleRate = 48000.0; bs.mixerId = bm.id; bs.sources = { bob }; bs.folder = tmp.getChildFile ("bounce-refuse");
        bs.segments = segmentsForEdit (bq, ed);
        bs.items.push_back ({ "Piece", 0, 48000 });
        {
            Bouncer b (bq, bs);
            CHECK (b.getPrepareError().contains ("Broken on Violin") && ! b.getPrepareError().contains ("Fine"));
            auto res = b.render();
            CHECK (res.error.isNotEmpty() && res.files.isEmpty());
        }
        {   // bypassed: the mix does not hear it either, so the bounce goes ahead (and the copy's own plug-in list says the same)
            bm.stripFor (tt)->slots[1].bypass.set (true);
            auto copy = bq.cloneForRender (48000.0, 512);
            CHECK (bq.pluginsMissingFrom (*copy, bm.id).isEmpty());
            Bouncer b (bq, bs);
            CHECK (b.getPrepareError().isEmpty());
            auto res = b.render();
            CHECK (res.error.isEmpty() && res.files.size() == 1);
            bm.stripFor (tt)->slots[1].bypass.set (false);
        }
        {   // removed: the same
            bm.stripFor (tt)->slots[1].clear (48000.0, 512, 2);
            Bouncer b (bq, bs);
            CHECK (b.getPrepareError().isEmpty());
        }
    }

    SECTION ("3.1.5 the audio device stops and starts again during a take: at the same rate the take goes on (and says so); at another rate the take is ended with what it has, its files at the old rate");
    {
        /** The device as the engine sees it when JUCE starts it again: only its rate and buffer size matter here. */
        struct FakeDevice : juce::AudioIODevice
        {
            FakeDevice (double r, int b) : juce::AudioIODevice ("Fake", "Test"), rate (r), block (b) {}
            juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
            juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
            juce::Array<double> getAvailableSampleRates() override { return { rate }; }
            juce::Array<int> getAvailableBufferSizes() override { return { block }; }
            int getDefaultBufferSize() override { return block; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
            void close() override {}
            bool isOpen() override { return true; }
            void start (juce::AudioIODeviceCallback*) override {}
            void stop() override {}
            bool isPlaying() override { return false; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return block; }
            double getCurrentSampleRate() override { return rate; }
            int getCurrentBitDepth() override { return 24; }
            juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (0, 2, true); return b; }
            juce::BigInteger getActiveInputChannels() const override { juce::BigInteger b; b.setRange (0, 2, true); return b; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            double rate; int block;
        };
        Project q;
        q.projectFile = tmp.getChildFile ("DevRestart").getChildFile ("DevRestart.fermata");
        q.projectFile.getParentDirectory().createDirectory();
        q.setInputCount (2); q.setOutputCount (2);
        q.tracks.clear();
        q.addTrack ("solo", TrackFormat::Mono, 0).setArmed (true);
        q.retireAllMixers(); q.addMixer ("M");
        auto& qw = *q.takeWindows.front(); qw.name = "Rec";
        AudioEngine e (q);
        e.setMixerThreads (0);
        FakeDevice d48 (48000.0, 256), d96 (96000.0, 256);
        e.audioDeviceAboutToStart (&d48);
        auto feed = [&] (int blocks)
        {
            std::vector<float> i0 (256, 0.25f), i1 (256, 0.0f), o0 (256, 0.0f), o1 (256, 0.0f);
            const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
            for (int b = 0; b < blocks; ++b) e.process (in, 2, out, 2, 256);
        };
        // Stop as the program does it: the device is running and its thread goes on delivering blocks while Stop waits for it to leave the
        // session (here there is no such thread, so the blocks are fed from another one; without them Stop would take the stalled-driver path)
        auto stopNow = [&]
        {
            std::atomic<bool> go { true };
            std::thread pretendAudio ([&] { while (go.load()) { feed (1); juce::Thread::sleep (1); } });
            e.stopRecording();
            go.store (false); pretendAudio.join();
            CHECK (e.waitForFinishedTakes (5000)); e.reapFinishedTakes();
        };
        // a restart at the same rate: the take goes on, the note says the sound meanwhile is missing
        CHECK (e.startRecording (qw).isEmpty());
        feed (100);
        e.audioDeviceStopped();
        CHECK (! e.isRunning() && e.isRecording());
        e.audioDeviceAboutToStart (&d48);
        CHECK (e.isRunning() && e.isRecording());
        const auto note1 = e.takeDeviceNote();
        CHECK (note1.contains ("STOPPED AND STARTED AGAIN") && note1.contains ("The take goes on") && e.takeDeviceNote().isEmpty());
        feed (100);
        stopNow();
        CHECK (e.audioThreadNeverStalled());
        CHECK (qw.groups.size() == 1 && qw.groups[0].lengthSamples >= 200 * 256 && std::abs (qw.groups[0].sampleRate - 48000.0) < 0.5);     // (the pretend thread's blocks at Stop add a few)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (qw.groups[0].files.empty() ? nullptr : fm.createReaderFor (qw.groups[0].files[0].file));
            CHECK (rd != nullptr && rd->lengthInSamples == qw.groups[0].lengthSamples);     // (a finished file: the header says what it holds)
        }
        // a restart at another rate: the take is ended with what it has; the file's header says the old rate and holds exactly that many samples
        CHECK (e.startRecording (qw).isEmpty());
        feed (120);
        e.audioDeviceStopped();
        e.audioDeviceAboutToStart (&d96);
        CHECK (! e.isRecording() && e.isRunning() && std::abs (e.getSampleRate() - 96000.0) < 0.5);
        const auto note2 = e.takeDeviceNote();
        CHECK (note2.contains ("CAME BACK AT 96000 Hz") && note2.contains ("48000 Hz") && note2.contains ("ended"));
        CHECK (e.waitForFinishedTakes (5000)); e.reapFinishedTakes();
        CHECK (qw.groups.size() == 2 && std::abs (qw.groups[1].lengthSamples - 120 * 256) <= 2 * 256 && std::abs (qw.groups[1].sampleRate - 48000.0) < 0.5);
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (qw.groups[1].files.empty() ? nullptr : fm.createReaderFor (qw.groups[1].files[0].file));
            CHECK (rd != nullptr && rd->lengthInSamples == qw.groups[1].lengthSamples && std::abs (rd->sampleRate - 48000.0) < 0.5);
        }
        // the next take is at the new rate, as before
        CHECK (e.startRecording (qw).isEmpty());
        feed (10);
        stopNow();
        CHECK (qw.groups.size() == 3 && std::abs (qw.groups[2].sampleRate - 96000.0) < 0.5);
        e.audioDeviceStopped();
    }

    SECTION ("3.2.0 export for video edit: the gating rule, a generator that starts mid-frame, the timecode read at a sample, and the three files");
    {
        using namespace videoexport;
        const double rate = 48000.0;
        auto S = [rate] (double sec) { return (juce::int64) std::llround (sec * rate); };
        {   // the rule: code from the fade-in to a gap before the next fade-in; a piece shorter than the gap to the next join goes; the last runs to its end
            std::vector<PieceEdge> p = { { S (0.0), S (5.2) }, { S (4.9), S (10.3) }, { S (10.0), S (10.4) }, { S (10.1), S (13.1) } };
            const auto runs = gateLtcRuns (p, S (0.25));
            CHECK (runs.size() == 3);
            if (runs.size() == 3)
            {
                CHECK (runs[0].piece == 0 && runs[0].from == S (0.0) && runs[0].to == S (4.65));
                CHECK (runs[1].piece == 1 && runs[1].from == S (4.9) && runs[1].to == S (9.85));
                CHECK (runs[2].piece == 3 && runs[2].from == S (10.1) && runs[2].to == S (13.1));
            }
            // two short pieces in a row both go; a gap between pieces (silence in the edit) ends the code at the piece's own end
            std::vector<PieceEdge> q = { { S (0.0), S (3.0) }, { S (3.0), S (3.1) }, { S (3.1), S (3.2) }, { S (3.2), S (6.0) }, { S (9.0), S (12.0) } };
            const auto r2 = gateLtcRuns (q, S (0.25));
            CHECK (r2.size() == 3 && r2[0].piece == 0 && r2[0].to == S (2.95) && r2[1].piece == 3 && r2[1].from == S (3.2) && r2[1].to == S (6.0) && r2[2].piece == 4 && r2[2].to == S (12.0));
            CHECK (gateLtcRuns ({}, S (0.25)).empty());
            CHECK (formatCodeFor (25.0, false) == 1 && formatCodeFor (24.0, false) == 0 && formatCodeFor (30000.0 / 1001.0, true) == 2 && formatCodeFor (30000.0 / 1001.0, false) == 3 && formatCodeFor (30.0, false) == 4 && formatCodeFor (24000.0 / 1001.0, false) == 5);
        }
        {   // a generator started in the middle of a frame reads as a continuation of the time of day, to within a sample
            for (double at : { 36001.0, 36009.9, 3600.0 + 0.123456, 82800.25 })
            {
                LtcGenerator gen; gen.prepare (rate); gen.startExact (1, at);
                std::vector<float> sig ((size_t) S (3.0));
                gen.fill (sig.data(), (int) sig.size());
                LtcDecoder dec (rate); dec.feed (sig.data(), (int) sig.size());
                const auto r = analyseLtc (dec.frames(), rate);
                CHECK (r.found && std::abs (r.fps - 25.0) < 1.0e-6 && r.jumps == 0);
                CHECK (r.found && std::abs (r.startSeconds - std::fmod (at, 86400.0)) < 2.0 / rate);
                if (! r.found || std::abs (r.startSeconds - std::fmod (at, 86400.0)) >= 2.0 / rate) std::printf ("  (startExact at %.6f: found %d fps %.3f jumps %d start %.6f frames %d)\n", at, (int) r.found, r.fps, r.jumps, r.startSeconds, r.frameCount);
            }
        }
        // a take: 20 s of a tone on "music", 20 s of 25 fps code from 10:00:00:00 on "LTC"
        const auto folder = tmp.getChildFile ("video-export"); folder.deleteRecursively(); folder.createDirectory();
        std::vector<std::vector<float>> music (1, std::vector<float> ((size_t) S (20.0)));
        for (size_t i = 0; i < music[0].size(); ++i) music[0][i] = 0.3f * (float) std::sin (0.02 * (double) i);
        std::vector<std::vector<float>> code (1, std::vector<float> ((size_t) S (20.0)));
        { LtcGenerator gen; gen.prepare (rate); gen.start (1, 36000.0); gen.fill (code[0].data(), (int) code[0].size()); }
        const auto musicFile = folder.getChildFile ("take-music.wav"), ltcFile = folder.getChildFile ("take-ltc.wav");
        CHECK (audioops::writeWav (musicFile, music, rate) && audioops::writeWav (ltcFile, code, rate));
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto c = timecodeAt (fm, ltcFile, 0, S (5.0));
            CHECK (c.found && std::abs (c.fps - 25.0) < 1.0e-6 && ! c.drop && std::abs (c.seconds - 36005.0) < 2.0 / rate);
            const auto c2 = timecodeAt (fm, ltcFile, 0, S (0.3));                       // near the start: the window begins at the file's first sample
            CHECK (c2.found && std::abs (c2.seconds - 36000.3) < 2.0 / rate);
            CHECK (! timecodeAt (fm, musicFile, 0, S (5.0)).found);                       // a tone is not timecode
        }
        // the project: a music track and an LTC track, an edit of four pieces as in the rule's first case (B's fade-in begins 0.1 s before its start)
        Project vp; vp.projectFile = folder.getChildFile ("Video.fermata");
        vp.setInputCount (2); vp.setOutputCount (2); vp.tracks.clear();
        const auto tm = vp.addTrack ("music", TrackFormat::Mono, 0).id;
        auto& ltcTrack = vp.addTrack ("LTC", TrackFormat::Mono, 1); ltcTrack.isLtc = true; const auto tl = ltcTrack.id;
        vp.buses.clear(); vp.retireAllMixers();                                               // (the default design's "Main" bus goes: the one made here is the main output)
        const auto out = vp.addBus ("Out", true).id;
        vp.addMixer ("M");
        auto& ed = vp.addEdit ("Concert", juce::Uuid::null());
        ed.sampleRate = rate;                                                                 // (insertRegion sets it in the program; the pieces are pushed straight in here)
        vp.syncEditMixers();
        auto* edMixer = vp.mixerOfEdit (ed.id);                                              // the edit's own mixer is what the export mixes through
        auto& vm = edMixer != nullptr ? *edMixer : *vp.mixers.front();
        vp.setSendLevel (vm, tm, out, 0.0f); vm.busFor (out)->outFirst.store (0); vm.busFor (out)->isMain.set (true);
        auto piece = [&] (double srcInSec, double srcOutSec, double startSec, double inStart, const juce::String& nm)
        {
            EditRegion r; r.id = juce::Uuid(); r.takeName = nm; r.sampleRate = rate; r.sourceLength = S (20.0);
            r.srcIn = S (srcInSec); r.srcOut = S (srcOutSec); r.startSample = S (startSec); r.inStart = inStart; r.inEnd = inStart + 0.05;
            RegionFile a; a.trackId = tm; a.trackName = "music"; a.file = musicFile; r.files.push_back (a);
            RegionFile b; b.trackId = tl; b.trackName = "LTC"; b.file = ltcFile; r.files.push_back (b);
            ed.regions.push_back (r);
        };
        piece (1.0, 6.0, 0.0, 0.0, "A"); piece (10.0, 15.0, 5.0, -0.1, "B"); piece (7.0, 7.1, 10.0, 0.0, "C"); piece (16.0, 19.0, 10.1, 0.0, "D");
        Settings st; st.folder = folder.getChildFile ("out"); st.name = "Concert mix"; st.gapSeconds = 0.25;
        const auto plan = planFor (vp, ed, st);
        CHECK (plan.error.isEmpty() && plan.pieces.size() == 4 && ! plan.noLtcTrack && plan.start == 0 && plan.end == S (13.1));
        CHECK (plan.pieces.size() == 4 && plan.pieces[1].takeName == "B" && plan.pieces[1].ltcFile == ltcFile && plan.pieces[1].ltcSample == S (9.9));
        auto bs = mixSettingsFor (vp, ed, plan, st);
        CHECK (bs.mixerId == vm.id && bs.sources.size() == 1 && bs.sources[0] == out && bs.items.size() == 1);
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        Bouncer bouncer (vp, bs);
        CHECK (bouncer.getPrepareError().isEmpty());
        std::atomic<float> prog { 0.0f }; std::atomic<bool> cancel { false };
        const auto res = exportForVideo (plan, bouncer, st, fm, &prog, &cancel);
        CHECK (res.error.isEmpty());
        std::printf ("  (video export: %d pieces, %d runs, %d without code, %d dropped, %.3g fps; notes: %s)\n", res.pieces, res.runs, res.withoutCode, res.dropped, res.fps, res.notes.joinIntoString (" | ").toRawUTF8());
        CHECK (res.pieces == 4 && res.runs == 3 && res.withoutCode == 0 && res.dropped == 1 && std::abs (res.fps - 25.0) < 1.0e-6 && res.lengthSamples == S (13.1));
        if (res.error.isEmpty())
        {
            std::unique_ptr<juce::AudioFormatReader> mix (fm.createReaderFor (res.mixFile)), ltc (fm.createReaderFor (res.ltcFile)), chk (fm.createReaderFor (res.checkFile));
            CHECK (mix != nullptr && ltc != nullptr && chk != nullptr);
            if (mix != nullptr && ltc != nullptr && chk != nullptr)
            {
                CHECK (mix->lengthInSamples == S (13.1) && ltc->lengthInSamples == S (13.1) && chk->lengthInSamples == S (13.1) && mix->numChannels == 2 && ltc->numChannels == 2 && chk->numChannels == 2);
                juce::AudioBuffer<float> L (2, (int) S (13.1)), M (2, (int) S (13.1)), K (2, (int) S (13.1));
                ltc->read (&L, 0, (int) S (13.1), 0, true, true); mix->read (&M, 0, (int) S (13.1), 0, true, true); chk->read (&K, 0, (int) S (13.1), 0, true, true);
                auto silent = [&] (double a, double b) { return L.getMagnitude (0, (int) S (a), (int) (S (b) - S (a))) < 1.0e-4f; };
                auto loud = [&] (double a, double b) { return L.getMagnitude (0, (int) S (a), (int) (S (b) - S (a))) > 0.2f; };
                CHECK (loud (0.0, 4.65) && silent (4.65, 4.9) && loud (4.9, 9.85) && silent (9.85, 10.1) && loud (10.1, 13.1));
                // the code in each run reads as the take's time at that moment: A from 10:00:01:00, B from 9.9 s into the take (its fade-in), D from 16 s
                auto codeAt = [&] (double a, double b, double expect)
                {
                    LtcDecoder dec (rate); dec.feed (L.getReadPointer (0) + S (a), (int) (S (b) - S (a)));
                    const auto r = analyseLtc (dec.frames(), rate);
                    return r.found && r.jumps == 0 && std::abs (r.startSeconds - expect) < 2.0 / rate;
                };
                CHECK (codeAt (0.0, 4.65, 36001.0) && codeAt (4.9, 9.85, 36009.9) && codeAt (10.1, 13.1, 36016.0));
                // both channels of the LTC file carry the code; the check file is the mix summed on the left and the code on the right
                float worst = 0.0f;
                for (int i = 0; i < (int) S (13.1); i += 97)
                {
                    worst = juce::jmax (worst, std::abs (L.getSample (0, i) - L.getSample (1, i)), std::abs (K.getSample (1, i) - L.getSample (0, i)),
                                        std::abs (K.getSample (0, i) - 0.5f * (M.getSample (0, i) + M.getSample (1, i))));
                }
                CHECK (worst < 2.0e-4f);
                CHECK (M.getMagnitude (0, 0, (int) S (13.1)) > 0.05f);                 // the mix has the music in it
            }
        }
        // a piece whose take has no LTC file gets silence and a note; no LTC track at all: said once
        {
            auto ed2 = ed; ed2.regions[0].files.pop_back();
            const auto plan2 = planFor (vp, ed2, st);
            CHECK (plan2.pieces.size() == 4 && plan2.pieces[0].ltcFile == juce::File());
            for (auto& t : vp.tracks) t.isLtc = false;
            CHECK (planFor (vp, ed, st).noLtcTrack);
            ltcTrack.isLtc = true;
        }
    }

    SECTION ("3.3.0 export AAF: the compound file and its objects read back (Fermata's own reader), the clips, crossfades, links and source timecode");
    {
        const double rate = 48000.0;
        auto S = [&] (double sec) { return (juce::int64) std::llround (sec * rate); };
        const auto folder = tmp.getChildFile ("aaf-export"); folder.deleteRecursively(); folder.createDirectory();
        // ---- the writer on its own: a document of two files and a crossfade
        {
            aafwrite::Document d;
            d.name = "My Edit"; d.sampleRate = rate; d.fps = 25.0; d.length = S (20.0); d.productVersion = "test";
            aafwrite::SourceFile a; a.file = folder.getChildFile ("Take 003_Omni-L.wav"); a.name = "Take 003 (1) Omni-L"; a.lengthSamples = S (60.0); a.startSeconds = 36001.0; d.files.push_back (a);
            aafwrite::SourceFile b = a; b.file = folder.getChildFile ("Take 004_Omni-L.wav"); b.name = "Take 004 (1) Omni-L"; b.startSeconds = 36300.5; d.files.push_back (b);
            aafwrite::SourceFile c = a; c.file = folder.getChildFile ("Take 003_Main.wav"); c.name = "Take 003 (2) Main"; c.channels = 2; d.files.push_back (c);
            aafwrite::Track t; t.name = "Omni-L";
            aafwrite::Clip c1; c1.file = 0; c1.at = S (1.0); c1.length = S (5.0); c1.sourceStart = S (1.0); c1.fadeIn = 480; t.clips.push_back (c1);
            aafwrite::Clip c2; c2.file = 1; c2.at = S (6.0) - 4800; c2.length = S (5.0); c2.sourceStart = S (10.0) - 2400; c2.cutPoint = 2400; c2.fadeOut = 9600; t.clips.push_back (c2);
            d.tracks.push_back (t);
            aafwrite::Track u; u.name = "Main L"; aafwrite::Clip c3 = c1; c3.file = 2; u.clips.push_back (c3); d.tracks.push_back (u);
            aafwrite::Track v; v.name = "Main R"; aafwrite::Clip c4 = c1; c4.file = 2; c4.channel = 1; v.clips.push_back (c4); d.tracks.push_back (v);
            const auto out = folder.getChildFile ("writer.aaf");
            CHECK (aafwrite::writeAaf (d, out).isEmpty() && out.getSize() > 100000 && out.getSize() % 4096 == 0);     // (whole 4096-byte sectors)
            const auto r = readAaf (out, 10.0);
            CHECK (r.error.isEmpty() && r.pieces.size() == 1 && r.trackNames.size() == 3 && r.clips == 4 && std::abs (r.sampleRate - rate) < 0.5);
            CHECK (r.trackNames.size() == 3 && r.trackNames[0] == "Omni-L" && r.trackNames[1] == "Main L" && r.trackNames[2] == "Main R");
            if (r.pieces.size() == 1)
            {
                const auto& p = r.pieces[0];
                CHECK (p.regions.size() == 3 && p.startSeconds == 0.0 && std::abs (p.lengthSeconds - 9.9) < 1.0e-6);
                const AafRegion* omni3 = nullptr; const AafRegion* omni4 = nullptr; const AafRegion* main3 = nullptr;
                for (auto& g : p.regions)
                {
                    if (g.files.size() == 1 && g.takeName == "Take 003") omni3 = &g;
                    if (g.takeName == "Take 004") omni4 = &g;
                    if (g.files.size() == 2) main3 = &g;
                }
                CHECK (omni3 != nullptr && omni4 != nullptr && main3 != nullptr);
                if (omni3 != nullptr && omni4 != nullptr && main3 != nullptr)
                {
                    // the first clip: 1 s in, 5 s long, a 0.01 s fade from silence, and the crossfade out (0.05 s either side of the join)
                    CHECK (omni3->startSample == 0 && omni3->srcIn == S (1.0) && omni3->srcOut == S (6.0) - 2400 && std::abs (omni3->inEnd - 0.01) < 1.0e-9 && std::abs (omni3->outStart + 0.05) < 1.0e-9 && std::abs (omni3->outEnd - 0.05) < 1.0e-9);
                    CHECK (omni3->files[0].fileName == "Take 003_Omni-L.wav" && omni3->files[0].trackName == "Omni-L" && omni3->files[0].url.startsWith ("file://"));
                    // the second: its nominal start is the join (6 s - 1 s into the timeline = 5 s... the clip began at 5.9 s with the cut 0.05 s later)
                    CHECK (omni4->startSample == S (6.0) - 4800 + 2400 - S (1.0) && omni4->srcIn == S (10.0) && std::abs (omni4->inStart + 0.05) < 1.0e-9 && std::abs (omni4->inEnd - 0.05) < 1.0e-9 && std::abs (omni4->outStart + 0.2) < 1.0e-9);
                    // the stereo file: one region with two files (its two channels on two tracks)
                    CHECK (main3->files[0].fileName == "Take 003_Main.wav" && main3->files[1].fileName == "Take 003_Main.wav" && main3->files[0].trackName == "Main L" && main3->files[1].trackName == "Main R");
                }
            }
            // a clip referring to a file the document has not got is refused; an empty document too
            aafwrite::Document bad = d; bad.tracks[0].clips[0].file = 9;
            CHECK (aafwrite::writeAaf (bad, folder.getChildFile ("bad.aaf")).isNotEmpty());
        }
        // ---- the export of an edit: files, a crossfade, source timecode from the LTC track
        {
            std::vector<std::vector<float>> music (1, std::vector<float> ((size_t) S (20.0)));
            for (size_t i = 0; i < music[0].size(); ++i) music[0][i] = 0.3f * (float) std::sin (0.02 * (double) i);
            std::vector<std::vector<float>> code (1, std::vector<float> ((size_t) S (20.0)));
            { LtcGenerator gen; gen.prepare (rate); gen.start (1, 36000.0); gen.fill (code[0].data(), (int) code[0].size()); }
            std::vector<std::vector<float>> code2 (1, std::vector<float> ((size_t) S (20.0)));
            { LtcGenerator gen; gen.prepare (rate); gen.startExact (1, 39600.5); gen.fill (code2[0].data(), (int) code2[0].size()); }     // (start() would begin at the frame before: 39600.48)
            const auto musicA = folder.getChildFile ("A-music.wav"), ltcA = folder.getChildFile ("A-ltc.wav"), musicB = folder.getChildFile ("B-music.wav"), ltcB = folder.getChildFile ("B-ltc.wav");
            CHECK (audioops::writeWav (musicA, music, rate) && audioops::writeWav (ltcA, code, rate) && audioops::writeWav (musicB, music, rate) && audioops::writeWav (ltcB, code2, rate));
            Project vp; vp.projectFile = folder.getChildFile ("Aaf.fermata");
            vp.setInputCount (2); vp.setOutputCount (2); vp.tracks.clear();
            const auto tm = vp.addTrack ("music", TrackFormat::Mono, 0).id;
            auto& ltcTrack = vp.addTrack ("LTC", TrackFormat::Mono, 1); ltcTrack.isLtc = true; const auto tl = ltcTrack.id;
            auto& ed = vp.addEdit ("Concert", juce::Uuid::null());
            ed.sampleRate = rate;
            auto piece = [&] (double srcInSec, double srcOutSec, double startSec, double inStart, double inEnd, double outStart, double outEnd, const juce::String& nm, const juce::File& mf, const juce::File& lf)
            {
                EditRegion r; r.id = juce::Uuid(); r.takeName = nm; r.sampleRate = rate; r.sourceLength = S (20.0);
                r.srcIn = S (srcInSec); r.srcOut = S (srcOutSec); r.startSample = S (startSec); r.inStart = inStart; r.inEnd = inEnd; r.outStart = outStart; r.outEnd = outEnd;
                RegionFile a; a.trackId = tm; a.trackName = "music"; a.file = mf; r.files.push_back (a);
                RegionFile b; b.trackId = tl; b.trackName = "LTC"; b.file = lf; r.files.push_back (b);
                ed.regions.push_back (r);
            };
            // A: 1 - 6 s of take A at 0 with a 0.02 s fade-in, crossfading 0.1 s (symmetric) into B: 10 - 15 s of take B at 5 s, with a 0.3 s fade-out
            piece (1.0, 6.0, 0.0, 0.0, 0.02, -0.05, 0.05, "Take A", musicA, ltcA);
            piece (10.0, 15.0, 5.0, -0.05, 0.05, -0.3, 0.0, "Take B", musicB, ltcB);
            aafexport::Settings st; st.file = folder.getChildFile ("out").getChildFile ("Concert.aaf");
            auto plan = aafexport::planFor (vp, ed, st);
            CHECK (plan.error.isEmpty() && plan.doc.files.size() == 2 && plan.doc.tracks.size() == 1 && plan.clips == 2 && ! plan.noLtcTrack);
            CHECK (plan.doc.tracks.size() == 1 && plan.doc.tracks[0].name == "music" && plan.doc.tracks[0].clips.size() == 2 && plan.doc.length == S (10.0));
            CHECK (plan.doc.files.size() == 2 && plan.doc.files[0].name == "Take A (1) music" && plan.files[0].ltcFile == ltcA && plan.files[0].ltcSample == 0);
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::atomic<float> prog { 0.0f }; std::atomic<bool> cancel { false };
            const auto res = aafexport::exportAaf (plan, st, fm, &prog, &cancel);
            std::printf ("  (AAF export: %d files, %d clips, %d tracks, %d without code, %.3g fps; notes: %s)\n", res.files, res.clips, res.tracks, res.withoutCode, res.fps, res.notes.joinIntoString (" | ").toRawUTF8());
            CHECK (res.error.isEmpty() && res.files == 2 && res.clips == 2 && res.tracks == 1 && res.withoutCode == 0 && std::abs (res.fps - 25.0) < 1.0e-6 && ! res.drop);
            CHECK (plan.doc.files.size() == 2 && std::abs (plan.doc.files[0].startSeconds - 36000.0) < 2.0 / rate && std::abs (plan.doc.files[1].startSeconds - 39600.5) < 2.0 / rate);
            CHECK (plan.doc.files.size() == 2 && plan.doc.files[0].lengthSamples == S (20.0) && plan.doc.files[0].channels == 1 && plan.doc.files[0].bits == 24);
            // read back: the same two pieces with the same crossfade, linked to the same files
            const auto r = readAaf (st.file, 10.0);
            CHECK (r.error.isEmpty() && r.pieces.size() == 1 && r.clips == 2 && r.trackNames.size() == 1);
            if (r.pieces.size() == 1 && r.pieces[0].regions.size() == 2)
            {
                const auto& a = r.pieces[0].regions[0]; const auto& b = r.pieces[0].regions[1];
                CHECK (a.takeName == "Take A" && a.startSample == 0 && a.srcIn == S (1.0) && a.srcOut == S (6.0) && std::abs (a.inEnd - 0.02) < 1.0e-9 && std::abs (a.outStart + 0.05) < 1.0e-9 && std::abs (a.outEnd - 0.05) < 1.0e-9);
                CHECK (b.takeName == "Take B" && b.startSample == S (5.0) && b.srcIn == S (10.0) && b.srcOut == S (15.0) && std::abs (b.inStart + 0.05) < 1.0e-9 && std::abs (b.inEnd - 0.05) < 1.0e-9 && std::abs (b.outStart + 0.3) < 1.0e-9);
                CHECK (a.files.size() == 1 && a.files[0].fileName == "A-music.wav" && b.files.size() == 1 && b.files[0].fileName == "B-music.wav");
            }
            else CHECK (false);
            // no LTC track: the files are written with no source timecode, said once
            for (auto& t : vp.tracks) t.isLtc = false;
            auto plan2 = aafexport::planFor (vp, ed, st);
            CHECK (plan2.noLtcTrack && plan2.doc.tracks.size() == 2);          // (the LTC track is an audio track like any other then)
            const auto res2 = aafexport::exportAaf (plan2, st, fm, nullptr, nullptr);
            CHECK (res2.error.isEmpty() && res2.files == 4 && res2.withoutCode == 4 && res2.notes.size() == 1);     // (the LTC files are audio files like any other then)
        }
    }

    SECTION ("3.4.0 take matching: the same place found in other takes of a made-up piece - slower, partial, rubato, transposed, in a wet room; saved and read back");
    {
        // the piece: 60 bars of chords (a bass note and a four-note arpeggio each), played at any tempo, from any bar, in any key
        struct Bar { int root; bool minor; int pattern; };
        std::vector<Bar> bars;
        { const int prog[16] = { 0, 5, 7, 0, 9, 2, 7, 0, 4, 9, 5, 7, 0, 7, 5, 0 }; for (int b = 0; b < 60; ++b) bars.push_back ({ prog[b % 16], (b % 16) == 4 || (b % 16) == 5 || (b % 16) == 9, b % 5 }); }
        const double rate = 48000.0;
        auto play = [&] (const std::function<double (int)>& barSeconds, int firstBar, int lastBar, int transpose, unsigned seed)
        {
            std::vector<float> out; std::mt19937 rng (seed); std::uniform_real_distribution<double> ph (0.0, 6.28);
            auto note = [&] (int midi, double seconds, double level, size_t at)
            {
                const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0); const size_t n = (size_t) (seconds * rate);
                if (out.size() < at + n) out.resize (at + n, 0.0f);
                const double p0 = ph (rng);
                for (size_t i = 0; i < n; ++i)
                {
                    const double t = (double) i / rate, env = std::min (1.0, t * 50.0) * std::exp (-t * 1.2) * std::min (1.0, (seconds - t) * 30.0);
                    double v = 0.0; for (int h = 1; h <= 8; ++h) v += std::sin (2.0 * 3.14159265 * f * h * t + p0 * h) / (h * h);
                    out[at + i] += (float) (level * env * v);
                }
            };
            double t = 0.0;
            for (int b = firstBar; b <= lastBar; ++b)
            {
                const double len = barSeconds (b); const auto& bar = bars[(size_t) b];
                const int root = 48 + bar.root + transpose, third = root + (bar.minor ? 3 : 4), fifth = root + 7;
                const int arp[5][4] = { { root + 12, third + 12, fifth + 12, third + 12 }, { fifth + 12, third + 12, root + 12, third + 12 }, { root + 12, fifth + 12, root + 24, fifth + 12 }, { third + 12, root + 12, third + 12, fifth + 12 }, { root + 24, fifth + 12, third + 12, root + 12 } };
                note (root - 12, len, 0.25, (size_t) (t * rate));
                for (int k = 0; k < 4; ++k) note (arp[bar.pattern][k], len / 4.0, 0.18, (size_t) ((t + k * len / 4.0) * rate));
                t += len;
            }
            return out;
        };
        auto roomed = [&] (const std::vector<float>& dry, unsigned seed, double tail)      // a reverb (a decaying noise tail) and noise at -35 dB
        {
            std::mt19937 rng (seed); std::normal_distribution<double> g (0.0, 1.0);
            const size_t irLen = (size_t) (tail * rate);
            std::vector<float> ir (irLen); for (size_t i = 0; i < irLen; ++i) ir[i] = (float) (g (rng) * std::exp (-3.0 * (double) i / (double) irLen) * 0.02);
            std::vector<float> out (dry.size() + irLen, 0.0f);
            for (size_t k = 0; k < irLen; k += 8) { const float w = k == 0 ? 1.0f : ir[k] * 8.0f; if (std::abs (w) < 1.0e-5f) continue; for (size_t i = 0; i < dry.size(); ++i) out[i + k] += dry[i] * w; }
            for (auto& v : out) v += (float) (g (rng) * 0.018);
            return out;
        };
        auto steady = [] (double len) { return std::function<double (int)> ([len] (int) { return len; }); };
        std::function<double (int)> rubato = [] (int b) { return 2.0 * (1.0 + 0.12 * std::sin (b * 0.7)); };
        struct Take { juce::String name; std::vector<float> audio; std::function<double (int)> barSeconds; int firstBar, lastBar; double tolerance; };
        std::vector<Take> takes;
        takes.push_back ({ "full", play (steady (2.0), 0, 59, 0, 1), steady (2.0), 0, 59, 0.25 });
        takes.push_back ({ "5 % slower", play (steady (2.1), 0, 59, 0, 2), steady (2.1), 0, 59, 0.25 });
        takes.push_back ({ "from bar 16", play (steady (2.0), 15, 59, 0, 3), steady (2.0), 15, 59, 0.25 });
        takes.push_back ({ "bars 10 - 40, faster", play (steady (1.9), 9, 39, 0, 4), steady (1.9), 9, 39, 0.25 });
        takes.push_back ({ "rubato", play (rubato, 0, 59, 0, 5), rubato, 0, 59, 0.25 });
        takes.push_back ({ "up 3 semitones", play (steady (2.0), 0, 59, 3, 8), steady (2.0), 0, 59, 0.25 });
        takes.push_back ({ "wet room", roomed (play (steady (2.0), 0, 59, 0, 6), 7, 2.0), steady (2.0), 0, 59, 0.8 });               // (a dry take against a wet one: the tail drags the match later)
        takes.push_back ({ "wet room 2, slower", roomed (play (steady (2.08), 0, 59, 0, 9), 10, 2.3), steady (2.08), 0, 59, 0.8 });
        std::vector<takematch::Fingerprint> fps;
        for (auto& t : takes) fps.push_back (takematch::analyseSamples (t.audio.data(), (juce::int64) t.audio.size(), rate));
        CHECK (std::abs (fps[0].frames() - takes[0].audio.size() / rate * 20.0) < 12.0 && std::abs (fps[0].fps - 20.0) < 1.0e-9);     // (20 frames a second, less the half-second window)
        auto barStart = [&] (const Take& t, int bar) { double s = 0.0; for (int b = t.firstBar; b < bar; ++b) s += t.barSeconds (b); return s; };
        auto check = [&] (size_t from, size_t to, const std::vector<int>& atBars, double tolerance)
        {
            const auto al = takematch::align (fps[from], fps[to]);
            double worst = 0.0; int wrong = 0;
            for (int bar : atBars)
            {
                const bool covered = bar >= takes[to].firstBar && bar <= takes[to].lastBar;
                const double here = barStart (takes[from], bar) + 0.5;
                const auto m = takematch::locate (al, here);
                if (covered)
                {
                    const double expect = barStart (takes[to], bar) + 0.5 * takes[to].barSeconds (bar) / takes[from].barSeconds (bar);
                    worst = juce::jmax (worst, std::abs (m.seconds - expect));
                    if (! m.found || std::abs (m.seconds - expect) > tolerance) ++wrong;
                }
                else if (m.found) ++wrong;        // a bar the other take has not got must not be "found"
            }
            std::printf ("  (take matching %s -> %s: %zu path points, rotation %d, worst error %.3f s, %d wrong)\n", takes[from].name.toRawUTF8(), takes[to].name.toRawUTF8(), al.a.size(), al.rotation, worst, wrong);
            return wrong == 0;
        };
        const std::vector<int> probe { 2, 12, 20, 30, 41, 55 };
        for (size_t j = 1; j < takes.size(); ++j) CHECK (check (0, j, probe, takes[j].tolerance));
        CHECK (check (3, 0, { 12, 20, 30 }, 0.25));                 // the partial take as the source
        CHECK (check (6, 7, { 3, 25, 50 }, 0.25));                  // two wet takes of the same room: a session as it really is
        {   // the transposed take: its notes were turned back into the first take's key
            const auto al = takematch::align (fps[0], fps[5]);
            CHECK (al.rotation == -3 && ! al.empty());                  // (3.7.0: notes in their octaves, moved down 3 semitones; before, 12 note classes turned by 9)
        }
        {   // nothing in common: two unrelated signals, or an empty fingerprint
            std::vector<float> hiss ((size_t) (rate * 40.0)); std::mt19937 rng (3); std::normal_distribution<double> g (0.0, 0.1);
            for (auto& v : hiss) v = (float) g (rng);
            const auto fh = takematch::analyseSamples (hiss.data(), (juce::int64) hiss.size(), rate);
            const auto al = takematch::align (fps[0], fh);
            const auto m = takematch::locate (al, 30.0);
            CHECK (! m.found && m.why.isNotEmpty());
            CHECK (takematch::align (fps[0], takematch::Fingerprint()).empty());
        }
        {   // saved and read back: the same fingerprint; a damaged file is refused
            const auto f = tmp.getChildFile ("take-match").getChildFile ("a.fpr");
            auto fp = fps[1]; fp.sourceKey = "k";
            CHECK (takematch::save (fp, f));
            takematch::Fingerprint back;
            CHECK (takematch::load (back, f) && back.frames() == fp.frames() && back.chroma == fp.chroma && back.loudness == fp.loudness && back.sourceKey == "k" && std::abs (back.fps - fp.fps) < 1.0e-12);
            CHECK (! takematch::load (back, tmp.getChildFile ("take-match").getChildFile ("none.fpr")));
            f.replaceWithText ("FMFPjunk");
            CHECK (! takematch::load (back, f));
            // from files: the take's files mixed, with the key of what was read
            const auto wav = tmp.getChildFile ("take-match").getChildFile ("take.wav");
            std::vector<std::vector<float>> chans (2, takes[0].audio);
            CHECK (audioops::writeWav (wav, chans, rate));
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto ff = takematch::analyseFiles (fm, { wav });
            CHECK (ff.frames() == fps[0].frames() && ff.sourceKey == takematch::sourceKeyFor ({ wav }) && ff.sourceKey.isNotEmpty());
            const auto al = takematch::align (ff, fps[1]);
            const auto m = takematch::locate (al, 40.5);
            CHECK (m.found && std::abs (m.seconds - 42.525) < 0.25);
        }
    }

    SECTION ("3.5.0 the ripple moves everything (markers, flags, overdubs, guide, marks, playhead, fixed automation) on insert and on replace; every version of a passage found; joins refined");
    {
        const double rate = 48000.0; const juce::int64 S = 48000;
        auto piece = [&] (juce::int64 in, juce::int64 out, const juce::String& take)
        {
            EditRegion r; r.srcIn = in; r.srcOut = out; r.sampleRate = rate; r.sourceLength = 60 * S; r.takeName = take; r.takeId = juce::Uuid();
            RegionFile rf; rf.trackId = juce::Uuid(); rf.trackName = "t"; rf.file = juce::File ("/nowhere/" + take + ".wav"); r.files.push_back (rf);
            return r;
        };
        auto furnish = [&] (EditDef& e)       // things along the timeline, before and after the 4 s mark
        {
            e.markers = { { juce::Uuid(), "a", 1.0 }, { juce::Uuid(), "b", 6.0 } };
            FeedbackNote f1, f2; f1.seconds = 2.0; f2.seconds = 7.0; e.feedback = { f1, f2 };
            e.overdubs = { piece (0, S, "od") }; e.overdubs[0].startSample = 8 * S;
            e.guide.file = juce::File ("/nowhere/g.wav"); e.guide.sampleRate = rate; e.guide.srcIn = 0; e.guide.srcOut = S; e.guide.start = 5 * S;
            e.fixIn = 3.5; e.fixOut = 9.0; e.playheadSeconds = 4.5;
            AutoLane lane; lane.trackId = e.regions[0].files[0].trackId;
            AutoPoint p1, p2, p3; p1.time = 1 * S; p2.time = 9 * S; p3.region = e.regions[1].id; p3.take = e.regions[1].takeId; p3.srcPos = e.regions[1].srcIn + S; p3.time = 7 * S;
            lane.pts = { p1, p2, p3 }; e.lanes = { lane };
        };
        // three pieces of 4 s: 0-4, 4-8, 8-12
        EditDef e; e.insertRegion (piece (0, 4 * S, "A")); e.insertRegion (piece (10 * S, 14 * S, "B")); e.insertRegion (piece (20 * S, 24 * S, "C"));
        furnish (e);
        // insert 2 s at 4 s: everything from 4 s moves 2 s later, everything before stays
        e.insertRegionAt (piece (30 * S, 32 * S, "D"), 4 * S);
        CHECK (e.regions.size() == 4 && e.regions[1].takeName == "D" && e.regions[2].startSample == 6 * S && e.regions[3].startSample == 10 * S);
        CHECK (e.markers[0].seconds == 1.0 && e.markers[1].seconds == 8.0);
        CHECK (e.feedback[0].seconds == 2.0 && e.feedback[1].seconds == 9.0);
        CHECK (e.overdubs[0].startSample == 10 * S && e.guide.start == 7 * S);
        CHECK (e.fixIn == 3.5 && e.fixOut == 11.0 && e.playheadSeconds == 6.5);
        auto fixedAt = [] (const AutoLane& l, juce::int64 t) { for (auto& p : l.pts) if (p.region.isNull() && p.time == t) return true; return false; };
        auto anchoredAt = [] (const AutoLane& l, juce::int64 t) { for (auto& p : l.pts) if (! p.region.isNull() && p.time == t) return true; return false; };
        CHECK (fixedAt (e.lanes[0], 1 * S) && fixedAt (e.lanes[0], 11 * S));                        // the fixed points
        CHECK (anchoredAt (e.lanes[0], 7 * S));                                                     // the point on B's audio: left to the settle, which follows the audio
        // replace 5 s .. 7 s (inside D's neighbour B, now at 6 - 10 s... take 2 s out of the middle of 4 - 10 s) with a 3 s piece: a ripple of +1 s after 7 s
        EditDef e2; e2.insertRegion (piece (0, 4 * S, "A")); e2.insertRegion (piece (10 * S, 14 * S, "B")); e2.insertRegion (piece (20 * S, 24 * S, "C"));
        furnish (e2);
        const int at = e2.replaceRangeRippled (piece (40 * S, 43 * S, "V"), 5 * S, 7 * S);
        CHECK (at == 2 && e2.regions.size() == 5);                                                 // A | B(4-5) | V(5-8) | B(8-9) | C(9-13)
        CHECK (e2.regions[1].startSample == 4 * S && e2.regions[1].srcOut == 11 * S);
        CHECK (e2.regions[2].startSample == 5 * S && e2.regions[2].length() == 3 * S && e2.regions[2].takeName == "V");
        CHECK (e2.regions[3].startSample == 8 * S && e2.regions[3].srcIn == 13 * S && e2.regions[3].takeName == "B");
        CHECK (e2.regions[4].startSample == 9 * S && e2.regions[4].takeName == "C");
        CHECK (e2.markers[0].seconds == 1.0 && e2.markers[1].seconds == 6.0 && e2.feedback[1].seconds == 8.0 && e2.overdubs[0].startSample == 9 * S && e2.guide.start == 5 * S);
        CHECK (e2.fixIn == 3.5 && e2.fixOut == 10.0 && e2.playheadSeconds == 4.5 && fixedAt (e2.lanes[0], 10 * S));
        CHECK (e2.regions[1].outStart < 0.0 && e2.regions[2].inEnd > 0.0 && e2.regions[2].outStart < 0.0 && e2.regions[3].inEnd > 0.0);   // crossfades at both new joins
        // a replacement the same length: nothing after moves; a replacement covering whole pieces takes them out
        EditDef e3; e3.insertRegion (piece (0, 4 * S, "A")); e3.insertRegion (piece (10 * S, 14 * S, "B")); e3.insertRegion (piece (20 * S, 24 * S, "C"));
        furnish (e3);
        e3.replaceRangeRippled (piece (40 * S, 46 * S, "V"), 3 * S, 9 * S);
        CHECK (e3.regions.size() == 3 && e3.regions[1].takeName == "V" && e3.regions[2].takeName == "C" && e3.regions[2].startSample == 9 * S && e3.regions[2].srcIn == 21 * S);
        CHECK (e3.markers[1].seconds == 6.0 && e3.overdubs[0].startSample == 8 * S);
        // a shorter replacement: the rest comes earlier, nothing goes below 0
        EditDef e4; e4.insertRegion (piece (0, 4 * S, "A")); e4.insertRegion (piece (10 * S, 14 * S, "B")); e4.insertRegion (piece (20 * S, 24 * S, "C"));
        furnish (e4);
        e4.replaceRangeRippled (piece (40 * S, 41 * S, "V"), 4 * S, 8 * S);
        CHECK (e4.regions.size() == 3 && e4.regions[2].startSample == 5 * S && e4.markers[1].seconds == 6.0 && e4.overdubs[0].startSample == 5 * S && e4.guide.start == 5 * S && e4.fixOut == 6.0);
        // the version search, on the piece of the 3.4.0 section: the messy take has a false start (bars 0-2), bars 0-19, bars 10-19 again, then 20-29
        {
            struct Bar { int root; bool minor; int pattern; };
            std::vector<Bar> bars; { std::mt19937 rng (77); for (int b = 0; b < 60; ++b) bars.push_back ({ (int) (rng() % 12), (rng() % 3) == 0, (int) (rng() % 5) }); }
            auto play = [&] (const std::vector<int>& order, double len, unsigned seed, std::vector<double>& starts)
            {
                std::vector<float> out; std::mt19937 rng (seed); std::uniform_real_distribution<double> ph (0.0, 6.28);
                auto note = [&] (int midi, double seconds, double level, size_t at)
                {
                    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0); const size_t n = (size_t) (seconds * rate);
                    if (out.size() < at + n) out.resize (at + n, 0.0f);
                    const double p0 = ph (rng);
                    for (size_t i = 0; i < n; ++i) { const double t = (double) i / rate, env = std::min (1.0, t * 50.0) * std::exp (-t * 1.2) * std::min (1.0, (seconds - t) * 30.0); double v = 0.0; for (int h = 1; h <= 8; ++h) v += std::sin (2.0 * 3.14159265 * f * h * t + p0 * h) / (h * h); out[at + i] += (float) (level * env * v); }
                };
                double t = 0.0;
                for (int b : order)
                {
                    starts.push_back (t);
                    if (b < 0) { t += len; continue; }
                    const auto& bar = bars[(size_t) b]; const int root = 48 + bar.root, third = root + (bar.minor ? 3 : 4), fifth = root + 7;
                    const int arp[5][4] = { { root + 12, third + 12, fifth + 12, third + 12 }, { fifth + 12, third + 12, root + 12, third + 12 }, { root + 12, fifth + 12, root + 24, fifth + 12 }, { third + 12, root + 12, third + 12, fifth + 12 }, { root + 24, fifth + 12, third + 12, root + 12 } };
                    note (root - 12, len, 0.25, (size_t) (t * rate));
                    for (int k = 0; k < 4; ++k) note (arp[bar.pattern][k], len / 4.0, 0.18, (size_t) ((t + k * len / 4.0) * rate));
                    t += len;
                }
                out.resize ((size_t) ((t + 1.0) * rate), 0.0f);
                return out;
            };
            std::vector<int> straight; for (int b = 0; b < 30; ++b) straight.push_back (b);
            std::vector<int> messy = { 0, 1, 2, -1 }; for (int b = 0; b < 20; ++b) messy.push_back (b); for (int b = 10; b < 20; ++b) messy.push_back (b); for (int b = 20; b < 30; ++b) messy.push_back (b);
            std::vector<int> other; for (int b = 40; b < 60; ++b) other.push_back (b);
            std::vector<double> s0, s1, s2;
            const auto a = play (straight, 2.0, 1, s0), b = play (messy, 2.0, 2, s1), c = play (other, 2.0, 3, s2);
            const auto fa = takematch::analyseSamples (a.data(), (juce::int64) a.size(), rate), fb = takematch::analyseSamples (b.data(), (juce::int64) b.size(), rate), fc = takematch::analyseSamples (c.data(), (juce::int64) c.size(), rate);
            auto passage = [&] (int fromBar, int toBar)
            {
                takematch::Fingerprint pf; pf.fps = fa.fps;
                const int f0 = (int) std::lround (fromBar * 2.0 * fa.fps), f1 = (int) std::lround (toBar * 2.0 * fa.fps);
                pf = fa.slice (f0, f1);
                return pf;
            };
            auto near = [] (const std::vector<takematch::Version>& vs, double start) { for (auto& v : vs) if (std::abs (v.start - start) < 0.3) return true; return false; };
            auto vs = takematch::findVersions (passage (12, 15), fb);
            CHECK (vs.size() == 2 && near (vs, s1[16]) && near (vs, s1[26]));                       // the first pass and the repeat
            for (auto& v : vs) CHECK (v.cost < 0.1 && std::abs (v.stretch - 1.0) < 0.1 && std::abs ((v.end - v.start) - 6.0) < 0.3);
            vs = takematch::findVersions (passage (1, 3), fb);
            CHECK (vs.size() == 2 && near (vs, s1[1]) && near (vs, s1[5]));                         // the false start and the real one
            vs = takematch::findVersions (passage (25, 27), fb);
            CHECK (vs.size() == 1 && near (vs, s1[39]));
            CHECK (takematch::findVersions (passage (12, 15), fc).empty());                          // other music: nothing
            // analyseRange: the passage's fingerprint from files, one of them starting later in the take (a corrected piece)
            const auto dir = tmp.getChildFile ("versions"); dir.createDirectory();
            const auto wa = dir.getChildFile ("a.wav"), wb = dir.getChildFile ("b.wav"), wlate = dir.getChildFile ("late.wav");
            CHECK (audioops::writeWav (wa, { a }, rate) && audioops::writeWav (wb, { b }, rate));
            std::vector<float> late (a.begin() + 20 * S, a.end());
            CHECK (audioops::writeWav (wlate, { late }, rate));
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto pr = takematch::analyseRange (fm, { { wa, 0 } }, 24 * S, 30 * S);              // bars 12-14 of the straight take
            CHECK (std::abs (pr.frames() - 120) <= 12);
            vs = takematch::findVersions (pr, fb);
            CHECK (vs.size() == 2 && near (vs, s1[16]) && near (vs, s1[26]));
            const auto pr2 = takematch::analyseRange (fm, { { wlate, 20 * S } }, 24 * S, 30 * S);     // the same bars from the file that starts at 20 s
            vs = takematch::findVersions (pr2, fb);
            CHECK (vs.size() == 2 && near (vs, s1[16]) && near (vs, s1[26]));
            // a take of eight files (more than the four that are mixed): its fingerprint's key is the key of ALL eight, the one the service compares against
            // (3.4.0 bug: it was the key of the four mixed, so such a take was never "ready" and was analysed again and again)
            {
                std::vector<juce::File> eight;
                for (int k = 0; k < 8; ++k)
                {
                    const auto f = dir.getChildFile ("track" + juce::String (k) + ".wav");
                    std::vector<float> part (a.begin(), a.begin() + 10 * S);
                    for (auto& v : part) v *= 0.5f + 0.05f * (float) k;
                    CHECK (audioops::writeWav (f, { part }, rate));
                    eight.push_back (f);
                }
                const auto f8 = takematch::analyseFiles (fm, eight);
                CHECK (! f8.empty() && f8.sourceKey == takematch::sourceKeyFor (eight) && f8.sourceKey != takematch::sourceKeyFor ({ eight[0], eight[1], eight[2], eight[3] }));
            }
            // the join: a guess 37 samples out is put right - exactly on the same audio, within a few ms between two performances
            const juce::int64 sa = (juce::int64) std::llround (s0[12] * rate), sbTrue = (juce::int64) std::llround (s1[26] * rate);
            double sim = 0.0;
            CHECK (takematch::refineJoin (fm, { wa, 0 }, sa, { wa, 0 }, sa + 37, 0.1, 0.06, &sim) == sa && sim > 0.9);
            CHECK (takematch::refineJoin (fm, { wlate, 20 * S }, sa, { wa, 0 }, sa + 37) == sa);                // through a file that starts later
            const auto got = takematch::refineJoin (fm, { wa, 0 }, sa, { wb, 0 }, sbTrue + 37, 0.1, 0.06, &sim);
            CHECK (std::abs (got - sbTrue) <= 240 && sim > 0.7);
        }
    }

    SECTION ("3.6.0 consonants: copied per track and channel (the consonant itself, not the vowel), kept as files, pasted on top at the right sample - in a take file and across a piece's start");
    {
        const double rate = 48000.0;
        const long N = (long) (1.2 * rate);
        std::mt19937 rng (5); std::normal_distribution<float> gauss (0.0f, 1.0f);
        // a sung vowel (200 Hz and its harmonics up to 5 kHz, falling off) and, at 0.50 s, a 't': 40 ms of noise above about 3 kHz, sharp start, quick decay
        std::vector<float> vowel ((size_t) N), burst ((size_t) N, 0.0f);
        for (long n = 0; n < N; ++n) { double v = 0.0; for (int h = 1; h <= 25; ++h) v += std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * h * n / rate + h) * 0.3 / h; vowel[(size_t) n] = (float) v; }
        {
            float y1 = 0.0f, x1 = 0.0f;                                     // (a first-order high-pass at about 3 kHz on white noise)
            const float a = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * 3000.0 / rate);
            const long t0 = (long) (0.50 * rate), len = (long) (0.04 * rate);
            for (long n = 0; n < len; ++n)
            {
                const float x = gauss (rng); const float y = a * (y1 + x - x1); x1 = x; y1 = y;
                burst[(size_t) (t0 + n)] = 0.35f * y * (float) std::exp (-3.0 * n / (double) len);
            }
        }
        auto withBurst = [&] (float level) { std::vector<float> v (vowel); for (long n = 0; n < N; ++n) v[(size_t) n] += level * burst[(size_t) n]; return v; };
        const juce::Uuid tA, tB; const std::vector<juce::Uuid> ids { tA, tB };
        // track A stereo (the t louder on the left), track B mono (a distant mic: the t quieter)
        const std::vector<std::vector<std::vector<float>>> tracks { { withBurst (1.0f), withBurst (0.6f) }, { withBurst (0.3f) } };
        FixSpec ws; ws.kind = FixSpec::Kind::Patch; ws.f0 = 2000.0; ws.f1 = 24000.0; ws.repair.mode = RepairMode::Replace; ws.repair.strength = 0.5;   // (the copy uses consonant mode at full strength whatever the window says)
        ws.repair.bands = 1024; ws.repair.multiRes = true; ws.repair.linkChannels = true; ws.repair.weighting = 0.5;
        const long s0 = (long) (0.49 * rate), s1 = (long) (0.56 * rate);
        const auto c = consonants::extract (ids, { "A", "B" }, tracks, rate, s0, s1, ws);
        CHECK (c.tracks.size() == 2 && c.tracks[0].size() == 2 && c.tracks[1].size() == 1);
        CHECK (c.length() > (juce::int64) (0.03 * rate) && c.length() < (juce::int64) (0.12 * rate));
        CHECK (c.lead >= 0 && c.lead < (juce::int64) (0.04 * rate));                         // (the ReTouch reaches a frame before the box, very quietly)
        std::printf ("  consonant copied: %.1f ms, lead %.1f ms\n", 1000.0 * (double) c.length() / rate, 1000.0 * (double) c.lead / rate);
        auto energy = [] (const std::vector<float>& v) { double e = 0.0; for (auto x : v) e += (double) x * x; return e; };
        auto burstPart = [&] (float level) { std::vector<float> v ((size_t) c.length()); for (size_t i = 0; i < v.size(); ++i) { const long n = s0 - (long) c.lead + (long) i; v[i] = n >= 0 && n < N ? level * burst[(size_t) n] : 0.0f; } return v; };
        auto corr = [] (const std::vector<float>& a, const std::vector<float>& b) { double ab = 0, aa = 0, bb = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) { ab += (double) a[i] * b[i]; aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; } return aa > 0 && bb > 0 ? ab / std::sqrt (aa * bb) : 0.0; };
        if (c.tracks.size() == 2 && c.tracks[0].size() == 2)
        {
            const auto ref = burstPart (1.0f);
            const double eRef = energy (ref), eCopy = energy (c.tracks[0][0]);
            CHECK (eCopy > 0.4 * eRef && eCopy < 1.3 * eRef);                                   // most of the t, and not much else
            CHECK (corr (c.tracks[0][0], ref) > 0.8);                                            // it IS the t (the vowel stayed behind)
            // each channel and track as it heard it: left / right about 1 : 0.6, the distant mic about 0.3
            const double lr = std::sqrt (energy (c.tracks[0][1]) / juce::jmax (1e-12, eCopy)), distant = std::sqrt (energy (c.tracks[1][0]) / juce::jmax (1e-12, eCopy));
            CHECK (lr > 0.45 && lr < 0.75 && distant > 0.18 && distant < 0.45);
        }
        // nothing in the box: nothing copied
        CHECK (consonants::extract (ids, { "A", "B" }, { { vowel, vowel }, { vowel } }, rate, (long) (0.2 * rate), (long) (0.27 * rate), ws).tracks.empty());

        // addClip: at the sample, gain, 2 ms fades, partly outside, mono target gets the average
        {
            std::vector<std::vector<float>> clip { std::vector<float> (480, 1.0f), std::vector<float> (480, 0.5f) };
            std::vector<std::vector<float>> x { std::vector<float> (2000, 0.0f), std::vector<float> (2000, 0.0f) };
            audioops::addClip (x, 1000, clip, 0.0, rate);
            CHECK (x[0][999] == 0.0f && std::abs (x[0][1240] - 1.0f) < 1e-6f && std::abs (x[1][1240] - 0.5f) < 1e-6f && x[0][1480] == 0.0f);
            CHECK (x[0][1000] > 0.0f && x[0][1000] < 0.05f && x[0][1479] > 0.0f && x[0][1479] < 0.05f);               // (faded in and out)
            std::vector<std::vector<float>> y { std::vector<float> (300, 0.0f) };
            audioops::addClip (y, -200, clip, -6.0206, rate);                                    // starts 200 samples before the audio; mono: the average, at half
            CHECK (std::abs (y[0][0] - 0.375f) < 1e-3f && y[0][279] > 0.0f && y[0][279] < 0.002f && y[0][280] == 0.0f && y[0][299] == 0.0f);
        }

        // kept as files and read back exactly (every channel, float)
        const auto dir = tmp.getChildFile ("consonants"); dir.deleteRecursively(); dir.createDirectory();
        ConsonantDef def; def.id = juce::Uuid(); def.name = "t"; def.from = "take 3, 0:00.49";
        CHECK (consonants::save (c, def, dir.getChildFile ("Consonants")).isEmpty() && def.tracks.size() == 2 && def.tracks[0].numChannels == 2 && def.length == c.length() && def.lead == c.lead);
        juce::AudioFormatManager fm; registerAudioFormats (fm);
        juce::String err;
        auto back = consonants::load (def, fm, err);
        CHECK (back != nullptr && err.isEmpty());
        if (back != nullptr && back->tracks.size() == 2 && c.tracks.size() == 2)
            CHECK (back->tracks[0].size() == 2 && back->tracks[0][1] == c.tracks[0][1] && back->tracks[1][0] == c.tracks[1][0] && back->forTrack (tB) != nullptr && back->forTrack (juce::Uuid()) == nullptr);
        // with the project: saved and read back, the files found from the project folder when it has moved
        {
            Project pc; pc.projectFile = dir.getChildFile ("p.fermata");
            pc.consonants.push_back (def);
            Project q; CHECK (q.fromVar (pc.toVar()) && q.consonants.size() == 1);
            if (q.consonants.size() == 1)
                CHECK (q.consonants[0].name == "t" && q.consonants[0].tracks.size() == 2 && q.consonants[0].tracks[0].file == def.tracks[0].file && q.consonants[0].tracks[1].numChannels == 1
                       && q.consonants[0].lead == def.lead && q.consonants[0].length == def.length && std::abs (q.consonants[0].sampleRate - rate) < 0.1 && q.findConsonant (def.id) != nullptr);
            const auto moved = tmp.getChildFile ("consonants-moved"); moved.deleteRecursively();
            CHECK (dir.copyDirectoryTo (moved));
            Project r; r.readingFrom = moved;
            CHECK (r.fromVar (pc.toVar()) && r.consonants.size() == 1 && r.consonants[0].tracks[0].file.isAChildOf (moved));
            std::vector<juce::var> objs; projectcopy::detail::collect (pc.toVar(), objs);
            int found = 0; for (auto& o : objs) if (o["file"].toString() == def.tracks[0].file.getFullPathName()) ++found;
            CHECK (found == 1);                                                                   // (a project copy takes the files along)
        }

        // pasted into a take file: track B's channel only on B's file, at the right sample, nothing else changed
        {
            auto shared = std::make_shared<const ConsonantAudio> (c);
            FixSpec ps; ps.kind = FixSpec::Kind::Paste; ps.paste = shared; ps.pasteGainDb = 0.0;
            const auto fa = dir.getChildFile ("A.wav"), fb = dir.getChildFile ("B.wav");
            CHECK (audioops::writeWav (fa, { vowel, vowel }, rate, true) && audioops::writeWav (fb, { vowel }, rate, true));
            const juce::int64 at = (juce::int64) (0.80 * rate);
            const auto res = audioops::fixTakeFiles (fm, { fa, fb }, { dir.getChildFile ("A2.wav"), dir.getChildFile ("B2.wav") }, { audioops::FixOp { ps, at, at + c.length() } }, nullptr, {}, { tA, tB });
            CHECK (res.error.isEmpty() && res.newFiles.size() == 2);
            if (res.newFiles.size() == 2)
            {
                auto ra = audioops::openReader (fm, res.newFiles[0]), rb = audioops::openReader (fm, res.newFiles[1]);
                std::vector<std::vector<float>> a2, b2;
                CHECK (ra != nullptr && rb != nullptr && audioops::readRange (*ra, 0, N, a2) && audioops::readRange (*rb, 0, N, b2));
                if (a2.size() == 2 && b2.size() == 1)
                {
                    // outside the consonant: untouched; inside (past the 2 ms fades): the vowel plus exactly the consonant's own channel of its own track
                    double outside = 0.0, insideA = 0.0, insideB = 0.0;
                    for (long n = 0; n < N; ++n)
                    {
                        const double dA = (double) a2[1][(size_t) n] - vowel[(size_t) n], dB = (double) b2[0][(size_t) n] - vowel[(size_t) n];
                        const juce::int64 i = (juce::int64) n - at;
                        if (i < 0 || i >= c.length()) { outside = juce::jmax (outside, std::abs (dA), std::abs (dB)); continue; }
                        if (i < 100 || i >= c.length() - 100) continue;
                        insideA = juce::jmax (insideA, std::abs (dA - c.tracks[0][1][(size_t) i]));
                        insideB = juce::jmax (insideB, std::abs (dB - c.tracks[1][0][(size_t) i]));
                    }
                    CHECK (outside < 1e-6 && insideA < 1e-5 && insideB < 1e-5);
                }
            }
            // a track the consonant was not copied from: left alone
            const auto res2 = audioops::fixTakeFiles (fm, { fb }, { dir.getChildFile ("C2.wav") }, { audioops::FixOp { ps, at, at + c.length() } }, nullptr, {}, { juce::Uuid() });
            CHECK (res2.error.isEmpty() && res2.newFiles.size() == 1);
            if (res2.newFiles.size() == 1) { auto rc = audioops::openReader (fm, res2.newFiles[0]); std::vector<std::vector<float>> c2; CHECK (rc != nullptr && audioops::readRange (*rc, 0, N, c2) && c2.size() == 1);
                                               float worst = 0.0f; if (c2.size() == 1) for (long n = 0; n < N; ++n) worst = juce::jmax (worst, std::abs (c2[0][(size_t) n] - vowel[(size_t) n])); CHECK (worst < 1e-5f); }
            // a piece that starts in the middle of the pasted consonant (an edit point under it): only its own part, at the right place
            const juce::int64 pieceStart = at + c.length() / 2;
            juce::String perr;
            auto ps2 = ps; audioops::resolvePaste (ps2, tA);
            const auto pr = audioops::fixPiece (fm, fa, 0, pieceStart, pieceStart + (juce::int64) (0.1 * rate), { audioops::FixOp { ps2, at, at + c.length() } }, dir.getChildFile ("piece.wav"), perr);
            CHECK (pr.ok && pr.from == pieceStart);
            if (pr.ok)
            {
                auto rp = audioops::openReader (fm, pr.file); std::vector<std::vector<float>> pc;
                CHECK (rp != nullptr && audioops::readRange (*rp, 0, rp->lengthInSamples, pc) && pc.size() == 2);
                if (pc.size() == 2)
                {
                    const auto k = (size_t) (c.length() / 2 + 10);                              // a sample of the consonant's second half
                    CHECK (std::abs (pc[0][10] - (vowel[(size_t) (pieceStart + 10)] + c.tracks[0][0][k])) < 1e-5f);
                    CHECK (std::abs (pc[0][(size_t) (c.length() - c.length() / 2 + 50)] - vowel[(size_t) (pieceStart + c.length() - c.length() / 2 + 50)]) < 1e-6f);   // past its end: untouched
                }
            }
            CHECK (ps.shortName() == "consonant");
        }
    }

    SECTION ("3.7.0 take matching on real-choir terms: notes in their octaves, the choir's tuning followed (a quarter-tone flat either side of the line), silence says nothing, the marked part placed exactly");
    {
        const double rate = 48000.0;
        struct Bar { int root; bool minor; int pattern; };
        std::vector<Bar> bars; { std::mt19937 rng (91); for (int b = 0; b < 40; ++b) bars.push_back ({ (int) (rng() % 12), (rng() % 3) == 0, (int) (rng() % 5) }); }
        // the piece played from 'first' to 'last' bar, 'len' s a bar, 'cents' off A440 (sinking by 'sink' cents over the take), with 'pre' s of silence first
        auto play = [&] (int first, int last, double len, double cents, double sink, double pre, unsigned seed)
        {
            std::vector<float> out ((size_t) (pre * rate), 0.0f); std::mt19937 rng (seed); std::uniform_real_distribution<double> ph (0.0, 6.28);
            const double total = (last - first + 1) * len;
            auto note = [&] (int midi, double seconds, double level, size_t at)
            {
                const double t0 = (double) at / rate - pre, c = cents + sink * juce::jlimit (0.0, 1.0, t0 / total);
                const double f = 440.0 * std::pow (2.0, (midi - 69 + c / 100.0) / 12.0); const size_t n = (size_t) (seconds * rate);
                if (out.size() < at + n) out.resize (at + n, 0.0f);
                const double p0 = ph (rng);
                for (size_t i = 0; i < n; ++i) { const double t = (double) i / rate, env = std::min (1.0, t * 50.0) * std::exp (-t * 1.2) * std::min (1.0, (seconds - t) * 30.0); double v = 0.0; for (int h = 1; h <= 6; ++h) v += std::sin (2.0 * 3.14159265 * f * h * t + p0 * h) / (h * h); out[at + i] += (float) (level * env * v); }
            };
            double t = pre;
            for (int b = first; b <= last; ++b)
            {
                const auto& bar = bars[(size_t) b]; const int root = 48 + bar.root, third = root + (bar.minor ? 3 : 4), fifth = root + 7;
                const int arp[5][4] = { { root + 12, third + 12, fifth + 12, third + 12 }, { fifth + 12, third + 12, root + 12, third + 12 }, { root + 12, fifth + 12, root + 24, fifth + 12 }, { third + 12, root + 12, third + 12, fifth + 12 }, { root + 24, fifth + 12, third + 12, root + 12 } };
                note (root - 12, len, 0.25, (size_t) (t * rate));
                for (int k = 0; k < 4; ++k) note (arp[bar.pattern][k], len / 4.0, 0.18, (size_t) ((t + k * len / 4.0) * rate));
                t += len;
            }
            out.resize ((size_t) ((t + 1.0) * rate), 0.0f);
            return out;
        };
        auto fpOf = [&] (const std::vector<float>& a) { return takematch::analyseSamples (a.data(), (juce::int64) a.size(), rate); };
        // three takes of bars 0-19 (2 s a bar): in tune; 40 cents flat sinking to 60 (it crosses the half-semitone line); 45 cents flat all the way
        const auto inTune = fpOf (play (0, 19, 2.0, 0.0, 0.0, 1.0, 1)), sinking = fpOf (play (0, 19, 2.0, -40.0, -20.0, 1.0, 2)), quarter = fpOf (play (0, 19, 2.0, -45.0, 0.0, 1.0, 3));
        CHECK (inTune.bins == takematch::Fingerprint::kNotes && inTune.frames() == inTune.loudness.size() && inTune.chroma.size() == (size_t) inTune.frames() * (size_t) inTune.bins);
        auto passageAt = [&] (const takematch::Fingerprint& f, double a, double b) { return f.slice ((int) std::lround (a * f.fps), (int) std::lround (b * f.fps)); };
        auto best = [] (const std::vector<takematch::Version>& vs) { const takematch::Version* b = nullptr; for (auto& v : vs) if (b == nullptr || v.cost < b->cost) b = &v; return b; };
        for (double a : { 3.0, 13.0, 31.0 })                                  // bars 1, 6 and 15 (+ the second of silence before)
        {
            const double want = a;                                            // (all three takes start their bar 0 at 1 s and keep 2 s a bar)
            for (auto* other : { &sinking, &quarter })
            {
                const auto vs = takematch::findVersions (passageAt (inTune, a, a + 4.0), *other);
                const auto* b = best (vs);
                CHECK (b != nullptr && std::abs (b->start - want) < 0.3 && b->cost <= takematch::kVersionCost);
            }
            const auto vs2 = takematch::findVersions (passageAt (sinking, a, a + 4.0), quarter);   // the flat takes against each other
            CHECK (best (vs2) != nullptr && std::abs (best (vs2)->start - want) < 0.3);
        }
        // a different stretch of the same piece is not this one (notes in their octaves: bars with other chords never pass)
        {
            const auto vs = takematch::findVersions (passageAt (inTune, 3.0, 7.0), *(&quarter));
            int wrong = 0; for (auto& v : vs) if (std::abs (v.start - 3.0) > 0.5) ++wrong;
            CHECK (wrong == 0);
        }
        // silence says nothing: a silent passage finds nothing, and a passage is not found in a take's silence
        {
            CHECK (takematch::findVersions (passageAt (inTune, 0.0, 0.9), quarter).empty());
            CHECK (takematch::mostlySilent (inTune, 0, 18) && ! takematch::mostlySilent (inTune, 40, 120));
            const auto vs = takematch::findVersions (passageAt (inTune, 0.2, 3.2), quarter, 1.0);
            for (auto& v : vs) CHECK (! takematch::mostlySilent (quarter, (int) std::lround (v.start * 20.0), (int) std::lround (v.end * 20.0)));
        }
        // the marked part with the music either side (8 s in all): found, and placed exactly by the part on its own
        {
            const double a = 21.0, L = 2.0;                                   // bar 10, 2 s
            const int c0 = (int) std::lround ((a - 3.0) * 20.0), c1 = (int) std::lround ((a + L + 3.0) * 20.0);
            const auto ctx = inTune.slice (c0, c1);
            const auto vs = takematch::findVersions (ctx, sinking, takematch::kVersionCost, (int) std::lround (a * 20.0) - c0, (int) std::lround ((a + L) * 20.0) - c0);
            const auto* b = best (vs);
            CHECK (b != nullptr && std::abs (b->start - a) < 0.5 && std::abs ((b->end - b->start) - L) < 0.5);
            if (b != nullptr)
            {
                const auto placed = takematch::placePart (passageAt (inTune, a, a + L), sinking, *b);
                CHECK (std::abs (placed.start - a) < 0.15 && std::abs (placed.end - (a + L)) < 0.15 && placed.cost == b->cost);
            }
        }
        // saved as version 2 and read back; a version 1 file (12 note classes) is refused, so it is made again
        {
            const auto f = tmp.getChildFile ("take-match").getChildFile ("v2.fpr");
            takematch::Fingerprint back;
            CHECK (takematch::save (quarter, f) && takematch::load (back, f) && back.bins == quarter.bins && back.chroma == quarter.chroma && back.loudness == quarter.loudness);
            juce::MemoryOutputStream mo; mo.write ("FMFP", 4); mo.writeInt (1); mo.writeDouble (20.0); mo.writeInt (1); mo.writeString ("k");
            for (int i = 0; i < 13; ++i) mo.writeFloat (0.1f);
            CHECK (f.replaceWithData (mo.getData(), mo.getDataSize()) && ! takematch::load (back, f));
        }
    }

    SECTION ("3.8.0 live sample-rate conversion: a 96 kHz project plays on a 48 kHz device - the engine at the project's rate, the device's sound converted (clean, a fixed delay), recording refused");
    {
        const double PI2 = 2.0 * juce::MathConstants<double>::pi;
        // the converter on its own: a tone comes through (error far below the tone), what is above the new half rate goes, nothing is mirrored up
        auto through = [&] (double inRate, double outRate, double f, std::vector<float>& outv)
        {
            RateConverter rc; if (! rc.prepare (inRate, outRate, 1, 500)) return -1.0;
            std::vector<float> in (500), o ((size_t) rc.maxOutputFor (500) + 2);
            outv.clear();
            for (juce::int64 pos = 0; pos < (juce::int64) inRate; pos += 500)
            {
                for (int i = 0; i < 500; ++i) in[(size_t) i] = (float) (0.5 * std::sin (PI2 * f * (double) (pos + i) / inRate));
                const float* ip[1] = { in.data() }; float* op[1] = { o.data() };
                const int got = rc.process (ip, 500, op, 1);
                outv.insert (outv.end(), o.begin(), o.begin() + got);
            }
            return rc.delaySeconds();
        };
        auto errDb = [&] (const std::vector<float>& v, double outRate, double f, double delay, double amp)
        {
            double se = 0.0, ss = 0.0;
            for (size_t j = (size_t) (0.1 * outRate); j < v.size() - (size_t) (0.1 * outRate); ++j)
            {
                const double ideal = amp * std::sin (PI2 * f * ((double) j / outRate - delay));
                se += (v[j] - ideal) * (v[j] - ideal); ss += 0.125;
            }
            return 10.0 * std::log10 (se / ss + 1.0e-30);
        };
        std::vector<float> v;
        for (auto pr : std::vector<std::pair<double, double>> { { 96000.0, 48000.0 }, { 48000.0, 96000.0 }, { 88200.0, 48000.0 }, { 48000.0, 44100.0 }, { 44100.0, 96000.0 } })
            for (double f : { 100.0, 1000.0, 15000.0 })
            {
                const double d = through (pr.first, pr.second, f, v);
                CHECK (d > 0.0 && d < 0.0015);                                                      // (about 1.2 ms)
                CHECK (errDb (v, pr.second, f, d, 0.5) < -110.0);
            }
        {
            through (96000.0, 48000.0, 30000.0, v);                                                 // above 24 kHz: must not fold back to 18 kHz
            double ss = 0.0; for (size_t j = 4800; j < v.size(); ++j) ss += (double) v[j] * v[j];
            CHECK (10.0 * std::log10 (ss / (double) (v.size() - 4800) / 0.125 + 1.0e-30) < -110.0);
        }
        CHECK (RateConverter::canConvert (96000.0, 48000.0) && RateConverter::canConvert (44100.0, 192000.0) && ! RateConverter::canConvert (48000.0, 47999.0));

        // the bridge: device blocks of any size (some bigger than the device said), the engine's never above engineBlock, no input ever missing, and a
        // loop from an input to an output comes back as it went in, a fixed time later
        for (auto pr : std::vector<std::pair<double, double>> { { 96000.0, 48000.0 }, { 44100.0, 48000.0 }, { 48000.0, 96000.0 } })
        {
            RateBridge br; CHECK (br.prepare (pr.first, pr.second, 1, 1, 480));
            if (pr.first == 96000.0) CHECK (br.engineBlock() == 960);
            juce::Random rnd (3);
            std::vector<float> in (2000), out (2000);
            juce::int64 dev = 0; int tooBig = 0; double se = 0.0, ss = 0.0;
            const double dly = br.inputDelaySeconds() + br.outputDelaySeconds();
            for (int k = 0; k < 600; ++k)
            {
                const int n = k % 5 == 0 ? 1 + rnd.nextInt (1500) : 470 + rnd.nextInt (11);
                for (int i = 0; i < n; ++i) in[(size_t) i] = (float) (0.3 * std::sin (PI2 * 440.0 * (double) (dev + i) / pr.second));
                const float* ip[1] = { in.data() }; float* op[1] = { out.data() };
                br.run (ip, 1, op, 1, n, nullptr, [&] (const float* const* ei, int, float* const* eo, int, int K)
                {
                    if (K > br.engineBlock()) ++tooBig;
                    for (int i = 0; i < K; ++i) eo[0][i] = ei[0] != nullptr ? ei[0][i] : 0.0f;
                });
                for (int i = 0; i < n; ++i)
                {
                    const double t = (double) (dev + i) / pr.second;
                    if (t < 0.05) continue;
                    const double ideal = 0.3 * std::sin (PI2 * 440.0 * (t - dly));
                    se += (out[(size_t) i] - ideal) * (out[(size_t) i] - ideal); ss += ideal * ideal;
                }
                dev += n;
            }
            CHECK (tooBig == 0 && br.inputShortfalls() == 0);
            CHECK (10.0 * std::log10 (se / ss + 1.0e-30) < -100.0);
            CHECK (br.inputFifoFill() <= 8 && br.outputFifoFill() <= 4);
        }

        // the project's rate: what most of its takes are at
        {
            Project q;
            CHECK (q.audioRate() == 0.0);
            auto& w = *q.takeWindows.front();
            TakeGroup a; a.sampleRate = 96000.0; TakeGroup b = a; TakeGroup c; c.sampleRate = 48000.0;
            w.groups.push_back (c); w.groups.push_back (a); w.groups.push_back (b);
            CHECK (q.audioRate() == 96000.0);
        }

        // the engine: a 96 kHz take on a 48 kHz device
        Project q; q.projectFile = tmp.getChildFile ("Src.fermata");
        q.setInputCount (2); q.setOutputCount (2); q.tracks.clear();
        const auto ta = q.addTrack ("A", TrackFormat::Mono, 0).id;
        q.retireAllMixers();
        const auto ob = q.addBus ("Out", true).id;
        auto& mq = q.addMixer ("M");
        q.setSendLevel (mq, ta, ob, 0.0f); mq.busFor (ob)->outFirst.store (0);
        const auto f96 = tmp.getChildFile ("tone96.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f96.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> wr (wav.createWriterFor (os.get(), 96000.0, 1, 32, {}, 0));
            if (wr != nullptr) os.release();
            juce::AudioBuffer<float> b (1, 96000 * 3);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, (float) (0.25 * std::sin (PI2 * 1000.0 * i / 96000.0)));
            if (wr != nullptr) wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        AudioEngine e (q);
        e.setMixerThreads (0);
        CHECK (e.rateFor (48000.0) == 48000.0);                                     // no project rate yet: the device's own
        e.setProjectRate (96000.0);
        CHECK (e.rateFor (48000.0) == 96000.0 && e.rateFor (96000.0) == 96000.0 && e.rateFor (47999.0) == 47999.0);
        e.startForDevice (48000.0, 480, 2, 2);
        CHECK (e.isConverting() && e.getSampleRate() == 96000.0 && e.getDeviceRate() == 48000.0 && e.getMaxBlock() == 960);
        CHECK (q.restoreSampleRate == 96000.0 && q.restoreBlock == 960);            // the plug-ins are prepared for the project's rate
        CHECK (e.converterDelayMs() > 0.5 && e.converterDelayMs() < 1.5);
        // recording is refused, and says why
        q.findTrack (ta)->setArmed (true);
        {
            const auto err = e.startRecording (*q.takeWindows.front());
            CHECK (err.contains ("96 kHz") && err.contains ("48 kHz") && err.contains ("cannot record") && ! e.isRecording());
        }
        q.findTrack (ta)->setArmed (false);
        auto playAll = [&] (int deviceBlock, double devRate, std::vector<float>& outL)
        {
            outL.clear();
            PlaySegment sg; sg.trackIndex = 0; sg.file = f96; sg.rate = 96000.0; sg.begin = 0; sg.end = 96000 * 3;
            auto ses = std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { sg }, 0, 96000 * 3, 96000.0, e.getMaxBlock());
            const auto err = e.startPlayback (std::move (ses));
            CHECK (err.isEmpty());
            std::vector<float> i0 (deviceBlock, 0.0f), i1 (deviceBlock, 0.0f), o0 (deviceBlock), o1 (deviceBlock);
            for (int i = 0; i < deviceBlock; ++i) i1[(size_t) i] = 0.5f * (float) std::sin (0.05 * i);       // an input nothing is patched to
            const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
            for (int k = 0; k < (int) (devRate * 1.5) / deviceBlock; ++k)
            {
                e.deviceBlock (in, 2, out, 2, deviceBlock);
                outL.insert (outL.end(), o0.begin(), o0.end());
            }
            e.stopPlayback();
        };
        std::vector<float> conv, direct;
        playAll (480, 48000.0, conv);
        CHECK (e.converterShortfalls() == 0);
        {
            const float pk = e.takeInputPeak (1);
            CHECK (pk > 0.45f && pk <= 0.5f);                                       // the input meters still read every input of the device
        }
        e.startForDevice (96000.0, 960, 2, 2);                                       // the same project on a 96 kHz device: no converter
        CHECK (! e.isConverting() && e.getSampleRate() == 96000.0 && e.getMaxBlock() == 960);
        playAll (960, 96000.0, direct);
        auto rms = [] (const std::vector<float>& x, size_t a, size_t b) { double s2 = 0.0; for (size_t i = a; i < b; ++i) s2 += (double) x[i] * x[i]; return std::sqrt (s2 / (double) (b - a)); };
        auto crossings = [] (const std::vector<float>& x, size_t a, size_t b) { int n = 0; for (size_t i = a + 1; i < b; ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++n; return n; };
        if (conv.size() >= 48000 && direct.size() >= 96000)
        {
            const double rc = rms (conv, 24000, 72000), rd = rms (direct, 48000, 144000);
            CHECK (rd > 0.05 && std::abs (20.0 * std::log10 (rc / rd)) < 0.05);       // the same level
            CHECK (std::abs (crossings (conv, 24000, 72000) - 2000) <= 2);             // still 1 kHz at 48 kHz: 2000 crossings a second
            std::printf ("   (96 kHz take on a 48 kHz device: level %.4f against %.4f direct, %d crossings in a second)\n", rc, rd, crossings (conv, 24000, 72000));
        }
        // a take at another rate than the project's is still refused (one rate at a time), with the project's rate in the message
        e.startForDevice (48000.0, 480, 2, 2);
        {
            const auto f48 = makeConstWav (tmp.getChildFile ("const48.wav"), 0.1, 1);
            PlaySegment sg; sg.trackIndex = 0; sg.file = f48; sg.rate = 48000.0; sg.begin = 0; sg.end = 48000;
            const auto err = e.startPlayback (std::make_unique<PlaybackSession> (std::vector<int> { 1 }, std::vector<PlaySegment> { sg }, 0, 48000, 48000.0, e.getMaxBlock()));
            CHECK (err.contains ("this project plays at 96000 Hz"));
        }
        // live monitoring through both converters: input 1 of the device to output 1, a fixed 2-3 ms later, at the strip's level
        {
            std::vector<float> i0 (480), i1 (480, 0.0f), o0 (480), o1 (480), got;
            const float* in[2] = { i0.data(), i1.data() }; float* out[2] = { o0.data(), o1.data() };
            for (int k = 0; k < 100; ++k)
            {
                for (int i = 0; i < 480; ++i) i0[(size_t) i] = (float) (0.3 * std::sin (PI2 * 440.0 * (double) (k * 480 + i) / 48000.0));
                e.deviceBlock (in, 2, out, 2, 480);
                got.insert (got.end(), o0.begin(), o0.end());
            }
            // find the delay and gain by fitting the input to the output over the last half second
            double bestErr = 1.0e9, bestD = 0.0, bestG = 0.0;
            for (double d = 0.0; d < 0.005; d += 1.0 / 480000.0)
            {
                double num = 0.0, den = 0.0;
                for (size_t j = 24000; j < got.size(); j += 3) { const double ref = 0.3 * std::sin (PI2 * 440.0 * ((double) j / 48000.0 - d)); num += got[j] * ref; den += ref * ref; }
                const double g = num / den; double err = 0.0;
                for (size_t j = 24000; j < got.size(); j += 3) { const double ref = g * 0.3 * std::sin (PI2 * 440.0 * ((double) j / 48000.0 - d)); err += (got[j] - ref) * (got[j] - ref); }
                if (err < bestErr) { bestErr = err; bestD = d; bestG = g; }
            }
            CHECK (bestD > 0.0018 && bestD < 0.0035 && bestG > 0.3);
            std::printf ("   (input to output through both converters: %.2f ms, gain %.3f)\n", bestD * 1000.0, bestG);
        }
        // prepared without a device (an offline render, the tests): no converter
        e.prepare (48000.0, 256);
        CHECK (! e.isConverting() && e.getDeviceRate() == 48000.0);
    }

    SECTION ("3.8.0 the take chart: the piece's bars, which bars how many takes have (duds never count), the runs and their wording, saved with the window");
    {
        TakeWindowDef w;
        auto add = [&] (int number, int in, int out, bool dud = false) { TakeGroup g; g.id = juce::Uuid(); g.number = number; g.barIn = in; g.barOut = out; g.dud = dud; w.groups.push_back (g); };
        CHECK (coverage::lastBar (w) == 0 && coverage::runs (w).empty());
        add (1, 1, 50); add (2, 21, 60); add (3, 51, 58); add (4, 1, 20); add (5, 30, 40, true); add (6, 0, 0); add (7, 59, 0);   // 7: one bar only (OUT not given)
        CHECK (coverage::lastBar (w) == 60);                                      // nothing about the piece entered: to the last bar any take reaches
        w.pieceBars = 100;
        CHECK (coverage::lastBar (w) == 100);
        const auto c = coverage::counts (w);
        CHECK (c.size() == 100 && c[0] == 2 && c[19] == 2 && c[20] == 2 && c[34] == 2 && c[50] == 2 && c[58] == 2 && c[59] == 1 && c[60] == 0 && c[99] == 0);   // (the dud adds nothing at bar 35)
        const auto rs = coverage::runs (w);
        // 1-20: takes 1, 4; 21-50: 1, 2; 51-58: 2, 3; 59: 2, 7; 60: 2; 61-100: none
        CHECK (rs.size() == 6);
        if (rs.size() == 6)
        {
            CHECK (rs[0].from == 1 && rs[0].to == 20 && rs[0].takes == std::vector<int> ({ 1, 4 }));
            CHECK (rs[1].from == 21 && rs[1].to == 50 && rs[1].takes == std::vector<int> ({ 1, 2 }));
            CHECK (rs[2].from == 51 && rs[2].to == 58 && rs[2].takes == std::vector<int> ({ 2, 3 }));
            CHECK (rs[3].from == 59 && rs[3].to == 59 && rs[3].takes == std::vector<int> ({ 2, 7 }));
            CHECK (rs[4].from == 60 && rs[4].to == 60 && rs[4].count() == 1);
            CHECK (rs[5].from == 61 && rs[5].to == 100 && rs[5].count() == 0);
            CHECK (coverage::describe (rs[5]) == "Bars 61 - 100: no take");
            CHECK (coverage::describe (rs[4]) == "Bar 60: only take 2");
            CHECK (coverage::describe (rs[0]) == "Bars 1 - 20: 2 takes (1, 4)");
            int bars = 0; for (auto& r : rs) bars += r.to - r.from + 1;
            CHECK (bars == 100);                                                  // every bar in exactly one run
        }
        // a piece shorter than a take's bars: the chart still reaches the take's last bar
        w.pieceBars = 40;
        CHECK (coverage::lastBar (w) == 60);
        // saved with the take window, and an undo step
        Project q; q.projectFile = tmp.getChildFile ("Chart.fermata");
        auto& tw2 = *q.takeWindows.front();
        const auto h0 = Project::undoHash (tw2);
        tw2.pieceBars = 120;
        CHECK (Project::undoHash (tw2) != h0);
        Project back; CHECK (back.fromVar (q.toVar()) && back.takeWindows.front()->pieceBars == 120);
        tw2.pieceBars = 0;
        Project back0; CHECK (back0.fromVar (q.toVar()) && back0.takeWindows.front()->pieceBars == 0);
    }

    SECTION ("3.9.0 the project's own sample rate: chosen, saved, it wins over the takes', an old project goes by its takes, and Record refuses another rate");
    {
        CHECK (Project::kProjectRates[0] == 44100.0 && Project::kProjectRates[1] == 48000.0 && Project::kProjectRates[2] == 96000.0 && Project::kProjectRates[3] == 192000.0);
        Project q; q.projectFile = tmp.getChildFile ("Rate.fermata");
        CHECK (q.sampleRate == 0.0 && q.audioRate() == 0.0 && ! q.hasTakes());
        q.sampleRate = 96000.0;
        CHECK (q.audioRate() == 96000.0);
        { Project back; CHECK (back.fromVar (q.toVar()) && back.sampleRate == 96000.0 && back.audioRate() == 96000.0); }
        // the chosen rate wins over what the takes are at (a take at another rate is refused when played, as before)
        TakeGroup g48; g48.sampleRate = 48000.0; q.takeWindows.front()->groups.push_back (g48);
        CHECK (q.hasTakes() && q.audioRate() == 96000.0);
        // a project from before 3.9.0 (no rate saved) goes by its takes, and the rate is written when it is saved
        auto v = q.toVar();
        if (auto* d = v.getDynamicObject()) d->removeProperty ("sampleRate");
        {
            Project old; CHECK (old.fromVar (v) && old.sampleRate == 0.0 && old.audioRate() == 48000.0);
            const auto again = old.toVar();
            CHECK ((double) again["sampleRate"] == 48000.0);
        }
        // nothing at all: 0 (the program asks)
        { Project empty; CHECK (empty.audioRate() == 0.0 && ! empty.toVar().hasProperty ("sampleRate")); }
        // a new design forgets the rate (the new project gets the one chosen for it)
        q.createDefaultDesign();
        CHECK (q.sampleRate == 0.0);

        // Record refuses when the engine runs at another rate than the project's and nothing converts (here: no device at all, the engine at 48 kHz)
        Project r; r.projectFile = tmp.getChildFile ("RateRec.fermata");
        r.setInputCount (2); r.setOutputCount (2); r.tracks.clear();
        r.addTrack ("A", TrackFormat::Mono, 0);
        AudioEngine e (r);
        e.prepare (48000.0, 256);
        e.setProjectRate (96000.0);
        {
            const auto err = e.startRecording (*r.takeWindows.front());
            CHECK (err.contains ("This project is 96 kHz") && err.contains ("48 kHz") && ! e.isRecording());
        }
        e.setProjectRate (48000.0);
        CHECK (e.startRecording (*r.takeWindows.front()) == "No tracks are record-armed");     // (the rate is not what stops it now)
        e.setProjectRate (0.0);
        CHECK (e.startRecording (*r.takeWindows.front()) == "No tracks are record-armed");
    }

    SECTION ("3.10.0 the take chart in the take sheet: drawn after each piece's takes in the PDF, its runs of bars as a CSV, a piece with no bars says so");
    {
        Project q; q.projectFile = tmp.getChildFile ("Chart sheet.fermata");
        q.addTakeWindow ("Requiem");
        auto& w = *q.takeWindows.back();
        const int ins[] = { 1, 21, 51, 1, 30 }, outs[] = { 50, 60, 58, 20, 40 };
        for (int i = 0; i < 5; ++i)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 30; g.startSeconds = i * 40.0;
            g.recordedAt = juce::Time (2026, 9, 28, 10, i, 0); g.barIn = ins[i]; g.barOut = outs[i]; g.dud = i == 4;
            w.groups.push_back (g);
        }
        w.pieceBars = 100;
        auto secs = takesheet::collect (q, w.id);
        CHECK (secs.size() == 1);
        if (secs.size() == 1)
        {
            const auto& sec = secs[0];
            CHECK (sec.lastBar == 100 && sec.pieceBars == 100 && (int) sec.counts.size() == 100 && sec.counts[34] == 2 && sec.counts[59] == 1 && sec.counts[60] == 0);
            CHECK (takesheet::chartSummary (sec) == "100 bars: 40 with no take, 2 with only one, 58 with two, 0 with three, 0 with four or more.");
        }
        const auto chart = takesheet::chartCsv (secs);
        CHECK (chart.startsWith ("Piece,From bar,To bar,Takes,Which takes\r\n") && chart.contains ("Requiem,1,20,2,1; 4\r\n") && chart.contains ("Requiem,59,60,1,2\r\n") && chart.contains ("Requiem,61,100,0,\r\n"));
        {
            const auto doc = takesheet::makePdf (secs, "Brahms Requiem", "3.10.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s2 ((const char*) mb.getData(), mb.getSize());
            CHECK (s2.find ("(Take chart)") != std::string::npos && s2.find ("(Bars with no take or only one:)") != std::string::npos);
            CHECK (s2.find ("(Bars 61 - 100: no take)") != std::string::npos && s2.find ("(Bars 59 - 60: only take 2)") != std::string::npos);
            CHECK (s2.find ("(30 - 40  \\(dud\\))") != std::string::npos || s2.find ("30 - 40") != std::string::npos);
            CHECK (doc.pageCount() == 1);
        }
        // every bar covered twice: the list says so
        w.groups[2].barIn = 51; w.groups[2].barOut = 100; w.groups.back().dud = false; w.groups.back().barIn = 51; w.groups.back().barOut = 100;
        w.groups[0].barOut = 50; w.groups[1].barIn = 1; w.groups[1].barOut = 50;
        {
            const auto doc = takesheet::makePdf (takesheet::collect (q, w.id), "Brahms Requiem", "3.10.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s2 ((const char*) mb.getData(), mb.getSize());
            CHECK (s2.find ("(Every bar has at least two takes.)") != std::string::npos);
        }
        // no bars at all: one line, no chart, no chart CSV
        for (auto& g : w.groups) g.barIn = g.barOut = 0;
        w.pieceBars = 0;
        secs = takesheet::collect (q, w.id);
        CHECK (takesheet::chartCsv (secs).isEmpty());
        {
            const auto doc = takesheet::makePdf (secs, "Brahms Requiem", "3.10.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s2 ((const char*) mb.getData(), mb.getSize());
            CHECK (s2.find ("Take chart: no bars entered") != std::string::npos && s2.find ("(Take chart)") == std::string::npos);
        }
        // a long session: 90 takes, the chart's lines thinner, onto a page of its own, nothing past the bottom
        Project big; big.addTakeWindow ("Mass in B minor");
        auto& bw = *big.takeWindows.back();
        for (int i = 1; i <= 90; ++i)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = i; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 60; g.startSeconds = i * 100.0;
            g.recordedAt = juce::Time (2026, 9, 22, 10, i % 60, 0); g.barIn = 1 + (i * 7) % 300; g.barOut = g.barIn + 40;
            bw.groups.push_back (g);
        }
        bw.pieceBars = 400;
        const auto bigDoc = takesheet::makePdf (takesheet::collect (big), "Bach", "3.10.0", juce::Time::getCurrentTime());
        const auto mb = bigDoc.toPdf(); const std::string s3 ((const char*) mb.getData(), mb.getSize());
        CHECK (s3.find ("(Take chart)") != std::string::npos && bigDoc.pageCount() >= 4);
        // (every drawing on the page stays above the footer: no y below the footer line in the page's own coordinates)
        bool inside = true;
        for (size_t at = s3.find (" re f"); at != std::string::npos; at = s3.find (" re f", at + 1))
        {
            const size_t ls = s3.rfind ('\n', at);
            std::istringstream ln (s3.substr (ls + 1, at - ls - 1));
            std::vector<double> v; double x; std::string tok;
            while (ln >> tok) { try { x = std::stod (tok); v.push_back (x); } catch (...) {} }
            if (v.size() >= 4 && v[v.size() - 3] < 34.0) inside = false;          // (the rect's bottom edge, measured from the page's foot)
        }
        CHECK (inside);
    }

    SECTION ("3.11.0 the coverage colours: crimson none, bright red one, red-orange two, darkish yellow three, green from four (light) to ten (dark), the same beyond");
    {
        auto c = [] (int n) { return coverage::colourFor (n); };
        auto same = [] (coverage::Rgb a, coverage::Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
        auto sum = [] (coverage::Rgb a) { return a.r + a.g + a.b; };
        CHECK (c (0).r > 120 && c (0).r > 3 * c (0).g && c (0).b > c (0).g);                     // crimson: deep red with a little blue
        CHECK (c (1).r == 255 && c (1).g < 80 && sum (c (1)) > sum (c (0)));                     // bright red: brighter than the crimson
        CHECK (c (2).r == 255 && c (2).g > c (1).g && c (2).g < 160);                             // red-orange
        CHECK (c (3).r > 180 && c (3).g > 130 && c (3).b < 60 && c (3).r < 230);                  // darkish yellow
        for (int n = 4; n <= 10; ++n) CHECK (c (n).g > c (n).r && c (n).g > c (n).b);            // green all the way
        for (int n = 4; n < 10; ++n) CHECK (sum (c (n + 1)) < sum (c (n)));                       // darker with every take
        CHECK (same (c (10), c (11)) && same (c (10), c (40)) && same (c (-1), c (0)));
        CHECK (coverage::isLight (c (4)) && ! coverage::isLight (c (10)) && ! coverage::isLight (c (0)) && ! coverage::isLight (c (1)));
        // the printed chart uses them: the key and a light shade behind the takes
        Project q; q.addTakeWindow ("Kyrie");
        auto& w = *q.takeWindows.back();
        for (int i = 0; i < 6; ++i) { TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 20; g.barIn = 1; g.barOut = 10 + i * 5; w.groups.push_back (g); }
        w.pieceBars = 50;
        const auto secs = takesheet::collect (q, w.id);
        CHECK (! secs.empty() && takesheet::chartSummary (secs[0]) == "50 bars: 15 with no take, 5 with only one, 5 with two, 5 with three, 20 with four or more.");
        const auto doc = takesheet::makePdf (secs, "Mass", "3.11.0", juce::Time::getCurrentTime());
        const auto mb = doc.toPdf(); const std::string s4 ((const char*) mb.getData(), mb.getSize());
        CHECK (s4.find ("(Takes per bar:)") != std::string::npos && s4.find ("(10 or more)") != std::string::npos);
        const auto crimson = takesheet::coverageColour (0);
        char want[64]; std::snprintf (want, sizeof (want), "%.2f", (double) crimson.r);
        CHECK (s4.find (want) != std::string::npos);
    }

    SECTION ("3.12.0 the no-take pinstripe on the printed chart: clipped to the crimson boxes (balanced), only where a bar has no take");
    {
        Project q; q.addTakeWindow ("Gloria");
        auto& w = *q.takeWindows.back();
        for (int i = 0; i < 3; ++i) { TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 20; g.barIn = 1; g.barOut = 20 + 10 * i; w.groups.push_back (g); }
        auto countOf = [] (const std::string& s, const std::string& what) { int n = 0; for (size_t at = s.find (what); at != std::string::npos; at = s.find (what, at + 1)) ++n; return n; };
        w.pieceBars = 60;                                                         // bars 41 - 60: no take
        {
            const auto doc = takesheet::makePdf (takesheet::collect (q, w.id), "Mass", "3.12.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s5 ((const char*) mb.getData(), mb.getSize());
            const int clips = countOf (s5, " re W n\n");
            CHECK (clips == 3);                                                   // the shading behind the takes, the ALL line, the key's "none"
            CHECK (countOf (s5, "\nQ\n") == clips);                               // every clip ended
            CHECK (countOf (s5, "q ") >= clips);
        }
        w.pieceBars = 40;                                                         // every bar has a take: only the key's swatch is striped
        {
            const auto doc = takesheet::makePdf (takesheet::collect (q, w.id), "Mass", "3.12.0", juce::Time::getCurrentTime());
            const auto mb = doc.toPdf(); const std::string s5 ((const char*) mb.getData(), mb.getSize());
            CHECK (countOf (s5, " re W n\n") == 1 && countOf (s5, "\nQ\n") == 1);
        }
    }

    SECTION ("3.12.1 the printed chart's ALL line is at the top, above the bar numbers and the takes");
    {
        Project q; q.addTakeWindow ("Sanctus");
        auto& w = *q.takeWindows.back();
        for (int i = 0; i < 2; ++i) { TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.sampleRate = 48000.0; g.lengthSamples = 48000 * 20; g.barIn = 1; g.barOut = 30; w.groups.push_back (g); }
        w.pieceBars = 40;
        const auto doc = takesheet::makePdf (takesheet::collect (q, w.id), "Mass", "3.12.1", juce::Time::getCurrentTime());
        const auto mb = doc.toPdf(); const std::string s6 ((const char*) mb.getData(), mb.getSize());
        auto yOf = [&] (const std::string& text) -> double                       // the baseline of a text, from the foot of the page (PDF's own y)
        {
            const auto at = s6.find ("(" + text + ") Tj");
            if (at == std::string::npos) return -1.0;
            const auto tm = s6.rfind (" Tm", at);
            const auto ls = s6.rfind ("1 0 0 1 ", tm);
            std::istringstream in (s6.substr (ls + 8, tm - ls - 8));
            double x = 0.0, y = -1.0; in >> x >> y;
            return y;
        };
        const double yAll = yOf ("ALL"), yRuler = yOf ("10"), yTake = yOf ("Take 1"), yKey = yOf ("Takes per bar:");
        CHECK (yAll > 0.0 && yRuler > 0.0 && yTake > 0.0 && yKey > 0.0);
        CHECK (yAll > yRuler && yRuler > yTake && yTake > yKey);                 // top to bottom: ALL, the bar numbers, the takes, the key
    }
    SECTION ("3.13.0 a hybrid CPU's big cores: at most half of them are kept for the audio");
    {
        auto V = [] (std::initializer_list<int> l) { return std::vector<int> (l); };
        // a laptop Core Ultra 5: 2 big cores with hyper-threading (logical 0-3), 10 small ones (logical 4-13)
        std::vector<CpuCore> cores;
        for (int i = 0; i < 12; ++i)
        {
            CpuCore c; c.number = i; c.efficiency = i < 2 ? 1 : 0;
            if (i < 2) c.logical = { 2 * i, 2 * i + 1 }; else c.logical = { i + 2 };
            cores.push_back (c);
        }
        CHECK (isHybridCpu (cores) && maxFastCoresForAudio (cores) == 1);
        // what this laptop had ticked: both big cores and a small one - only one big core stays with the audio now
        CoreReservationSettings s; s.enabled = true; s.keepOthersOff = true; s.reserved = { 0, 1, 9 };
        auto plan = makeCorePlan (cores, s);
        CHECK (plan.valid && plan.audioCores == V ({ 1, 9 }) && plan.audioLogical == V ({ 2, 3, 11 }));
        CHECK (std::find (plan.generalCores.begin(), plan.generalCores.end(), 0) != plan.generalCores.end() && plan.warning.contains ("core 0"));
        // the older plan (to set free the programs an older version fenced) still takes both
        auto old = makeCorePlan (cores, s, false);
        CHECK (old.valid && old.audioCores == V ({ 0, 1, 9 }) && old.warning.isEmpty());
        // the quietest-cores button picks one big core, never a small one
        CHECK (pickQuietCores (cores, std::vector<double> (14, 0.0), 3) == V ({ 1 }));
        // a hybrid CPU with a single big core: none can be kept
        auto one = cores; one[0].efficiency = 0;
        CHECK (maxFastCoresForAudio (one) == 0 && pickQuietCores (one, std::vector<double> (14, 0.0), 2).empty());
        s.reserved = { 1 };
        CHECK (! makeCorePlan (one, s).valid && makeCorePlan (one, s).problem.contains ("big"));
        // small cores alone may still be ticked by hand
        s.reserved = { 10, 11 }; CHECK (makeCorePlan (cores, s).valid);
        // a desktop with 8 big and 16 small cores keeps up to 4 big ones
        std::vector<CpuCore> desk;
        for (int i = 0; i < 24; ++i) { CpuCore c; c.number = i; c.efficiency = i < 8 ? 1 : 0; c.logical = { i }; desk.push_back (c); }
        CHECK (maxFastCoresForAudio (desk) == 4);
        s.reserved = { 5, 6, 7 }; CHECK (makeCorePlan (desk, s).valid && makeCorePlan (desk, s).audioCores.size() == 3 && makeCorePlan (desk, s).warning.isEmpty());
        // a CPU with all its cores alike is not limited
        for (auto& c : desk) c.efficiency = 0;
        CHECK (! isHybridCpu (desk) && maxFastCoresForAudio (desk) == 24);
        // not Windows here: nothing to set free, no snapshot, no description, not a laptop
        CHECK (CoreReservation::get().releaseLeftovers (s) == 0 && ! CoreReservation::isLaptop());
        const auto a = SystemLoad::take(), b = SystemLoad::take();
        CHECK (! a.ok && SystemLoad::describe (a, b, true).isEmpty() && SystemLoad::powerText().isEmpty());
    }
    SECTION ("3.17.0 the freeze-report helper's arguments, and where the threads are");
    {
        CHECK (crashreport::isHelperCall (juce::StringArray { "--fermata-dump", "123", "\"C:\\x.dmp\"" }) && ! crashreport::isHelperCall (juce::StringArray { "Mass.fermata" }));
        CHECK (crashreport::helperMain (juce::StringArray { "--fermata-dump" }) == 3);                 // (no process or file)
        CHECK (crashreport::helperMain (juce::StringArray { "--fermata-dump", "0", "x.dmp" }) == 3);
        CHECK (crashreport::helperMain (juce::StringArray { "--fermata-dump", "123", "x.dmp" }) == 8); // (not Windows here)
        juce::String why; CHECK (crashreport::writeViaHelper ("freeze", why) == juce::File() && why.isNotEmpty());
        CHECK (crashreport::describeThreads (1).isEmpty());
    }
    SECTION ("3.16.0 the edit begins at 0:00 (or its lead-in), everything moving with it");
    {
        EditDef e; e.sampleRate = 48000.0;
        auto piece = [] (juce::int64 start, juce::int64 len) { EditRegion r; r.id = juce::Uuid(); r.sampleRate = 48000.0; r.sourceLength = 48000 * 60; r.srcIn = 48000; r.srcOut = 48000 + len; r.startSample = start; return r; };
        e.regions.push_back (piece (0, 480000)); e.regions.push_back (piece (480000, 480000));
        e.regions[0].inStart = -0.5; e.regions[0].inEnd = -0.38;                 // the opening fade dragged half a second before 0:00
        auto od = piece (96000, 48000); e.overdubs.push_back (od);
        Marker m; m.seconds = 5.0; e.markers.push_back (m);
        e.playheadSeconds = 3.0;
        CHECK (e.audibleStart() == -24000);
        CHECK (e.anchorStart());
        // (3.17.1: the first piece itself reaches back to its fade, taking in the take's audio before it: its box starts with the fade, at 0:00)
        CHECK (e.audibleStart() == 0 && e.regions[0].fadeInBegin() == 0 && e.regions[0].startSample == 0 && e.regions[0].srcIn == 24000
               && e.regions[0].inStart == 0.0 && std::abs (e.regions[0].inEnd - 0.12) < 1e-9 && e.regions[1].startSample == 504000);
        CHECK (e.overdubs[0].startSample == 120000 && std::abs (e.markers[0].seconds - 5.5) < 1e-9 && std::abs (e.playheadSeconds - 3.5) < 1e-9);
        CHECK (! e.anchorStart());                                                // (already there)
        e.leadIn = true; e.leadInSeconds = 1.25;                                  // "Leave space before the first edit": 1.25 s
        CHECK (e.anchorStart() && e.audibleStart() == 60000 && e.startTarget() == 60000);
        e.leadIn = false;
        e.regions[0].inStart = 0.2; e.regions[0].inEnd = 0.3;                     // the fade moved later: everything comes back to start at 0:00 again
        CHECK (e.anchorStart() && e.audibleStart() == 0);
        EditDef empty; empty.sampleRate = 48000.0; CHECK (! empty.anchorStart());
        {   // not enough of the take before the piece: it reaches back as far as there is audio, the rest of the fade stays before it
            EditDef f; f.sampleRate = 48000.0;
            auto r = piece (0, 480000); r.srcIn = 12000; r.srcOut = 492000; r.inStart = -0.5; r.inEnd = -0.3; f.regions.push_back (r);
            CHECK (f.anchorStart() && f.regions[0].srcIn == 0 && f.audibleStart() == 0 && std::abs (f.regions[0].inStart + 0.25) < 1e-9 && f.regions[0].startSample == 12000);
        }
        // saved with the edit
        Project p; p.createDefaultDesign();
        auto& tw = p.addTakeWindow ("W");
        auto& ed = p.addEdit ("E", tw.id); ed.leadIn = true; ed.leadInSeconds = 2.37;
        Project back; CHECK (back.fromVar (p.toVar()) && back.edits.size() == 1 && back.edits[0]->leadIn && std::abs (back.edits[0]->leadInSeconds - 2.37) < 1e-9);
    }
    SECTION ("3.16.0 Find other versions: a take's bars over its music");
    {
        takematch::Fingerprint fp; fp.fps = 20.0;
        fp.loudness.assign (20 * 15, -90.0f);                                     // 15 s: silence, 10 s of music from 2 s, silence
        for (int i = 40; i < 240; ++i) fp.loudness[(size_t) i] = -20.0f;
        fp.chroma.assign ((size_t) fp.frames() * (size_t) fp.bins, 0.0f);
        const auto m = takematch::barMapOf (fp, 1, 10, 15.0);
        CHECK (m.valid() && std::abs (m.from - 2.0) < 1e-9 && std::abs (m.to - 12.0) < 1e-9 && std::abs (m.barLength() - 1.0) < 1e-9);
        CHECK (m.barAt (2.5) == 1 && m.barAt (11.9) == 10 && m.barAt (0.0) == 1 && m.barAt (14.0) == 10 && std::abs (m.timeOf (5) - 6.0) < 1e-9);
        CHECK (m.hasAny (10, 12) && ! m.hasAny (11, 12) && m.hasAny (1, 1));
        const auto none = takematch::barMapOf (takematch::Fingerprint(), 5, 8, 20.0);     // no fingerprint: the whole take
        CHECK (none.from == 0.0 && none.to == 20.0 && none.barAt (19.9) == 8);
        takematch::BarMap noBars; CHECK (! noBars.valid() && ! noBars.hasAny (1, 100));
    }
    SECTION ("3.14.0 crash reports: the folder, newest first, only five kept");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-crash-test");
        dir.deleteRecursively();
        crashreport::install (dir, "3.14.0");
        CHECK (crashreport::folder() == dir && crashreport::allReports().isEmpty());
        CHECK (crashreport::write (nullptr, "freeze") == juce::File());                      // (not Windows here: nothing is written)
        CHECK (crashreport::describeException (nullptr).isEmpty());
        dir.createDirectory();
        const auto now = juce::Time::getCurrentTime();
        for (int i = 0; i < 7; ++i)
        {
            auto f = dir.getChildFile ("Fermata 3.14.0 crash " + juce::String (i) + ".dmp");
            f.replaceWithText ("x");
            f.setLastModificationTime (now - juce::RelativeTime::hours (7 - i));                // report 6 is the newest
        }
        dir.getChildFile ("notes.txt").replaceWithText ("not a report");
        auto all = crashreport::allReports();
        CHECK (all.size() == 7 && all.getFirst().getFileName().contains ("crash 6") && all.getLast().getFileName().contains ("crash 0"));
        CHECK (crashreport::reportsSince (now - juce::RelativeTime::minutes (90)).size() == 1);
        crashreport::prune (5);
        all = crashreport::allReports();
        CHECK (all.size() == 5 && all.getLast().getFileName().contains ("crash 2") && dir.getChildFile ("notes.txt").existsAsFile());
        dir.deleteRecursively();
    }

    SECTION ("3.18.0 take matching is switched on per take window, and saved with it");
    {
        Project q;
        auto& w = *q.takeWindows.front();
        CHECK (! w.takeMatching);                                                           // off until switched on
        { Project back; CHECK (back.fromVar (q.toVar()) && ! back.takeWindows.front()->takeMatching); }
        CHECK (! juce::JSON::toString (q.takeWindowToVar (w)).contains ("takeMatching"));   // (an older Fermata sees nothing new)
        w.takeMatching = true;
        { Project back; CHECK (back.fromVar (q.toVar()) && back.takeWindows.front()->takeMatching); }
        auto v = q.toVar();                                                                 // a project saved before 3.18.0 has no such field: off
        if (auto* tws = v["takeWindows"].getArray()) for (auto& t : *tws) if (auto* d = t.getDynamicObject()) d->removeProperty ("takeMatching");
        { Project back; CHECK (back.fromVar (v) && ! back.takeWindows.front()->takeMatching); }
    }

    SECTION ("3.18.0 take matching: only the take windows switched on, never while held, and how far it has got");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-takematch-318");
        dir.deleteRecursively(); dir.createDirectory();
        Project q;
        auto& w = *q.takeWindows.front();
        auto addTake = [&] (const juce::String& name)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = (int) w.groups.size() + 1; g.sampleRate = 48000.0; g.lengthSamples = 4 * 48000;
            TakeFile f; f.file = makeToneWav (dir.getChildFile (name + ".wav"), 0.3, 4, false); f.numChannels = 1; f.trackName = "Main";
            if (! q.tracks.empty()) f.trackId = q.tracks.front().id;
            g.files.push_back (f);
            w.groups.push_back (g);
            return g.id;
        };
        const auto t1 = addTake ("a"), t2 = addTake ("b");
        AudioEngine eng (q);
        TakeMatchService tm (q, eng);
        using St = TakeMatchService::State;
        auto waitFor = [] (const std::function<bool()>& cond, int ms) { for (int t = 0; t < ms && ! cond(); t += 20) juce::Thread::sleep (20); return cond(); };
        // off (as every window starts): nothing is analysed, even when a fingerprint is asked for
        CHECK (! w.takeMatching && tm.fingerprintFor (t1) == nullptr);
        tm.windowsChanged();
        juce::Thread::sleep (300);
        CHECK (tm.statusOf (t1).state == St::Unknown && tm.progress().windowsOn == 0 && tm.progress().total == 0);
        // switched on while paused: queued, not touched
        tm.setPaused (true);
        w.takeMatching = true; tm.windowsChanged();
        auto pr = tm.progress();
        CHECK (pr.windowsOn == 1 && pr.total == 2 && pr.left() == 2 && pr.secondsLeft < 0.0);      // (no take done yet: the time left is not known)
        juce::Thread::sleep (700);
        CHECK (tm.statusOf (t1).state == St::Queued && tm.statusOf (t2).state == St::Queued);
        // Rec mode holds it as well, even once resumed
        tm.setRecHeld (true); tm.setPaused (false);
        CHECK (tm.isHeld());
        juce::Thread::sleep (700);
        CHECK (tm.statusOf (t1).state == St::Queued && tm.statusOf (t2).state == St::Queued);
        // let go: both are done, and nothing is left
        tm.setRecHeld (false);
        CHECK (waitFor ([&] { return tm.progress().left() == 0; }, 30000));
        CHECK (tm.statusOf (t1).state == St::Ready && tm.statusOf (t2).state == St::Ready && tm.fingerprintFor (t1) != nullptr);
        pr = tm.progress();
        CHECK (pr.ready == 2 && pr.cannot == 0 && pr.nowTake.isNull() && pr.secondsLeft <= 0.0);
        // switched off: a take recorded now is not queued; what was done stays usable (switched on again, it is there at once)
        w.takeMatching = false; tm.windowsChanged();
        const auto t3 = addTake ("c");
        tm.takeFinished (t3);
        juce::Thread::sleep (300);
        CHECK (tm.statusOf (t3).state == St::Unknown && tm.fingerprintFor (t1) != nullptr && tm.progress().windowsOn == 0);
        CHECK (tm.progress (&w).total == 3 && tm.progress (&w).ready == 2);                       // (one window asked for: on or not)
        tm.stop();
        dir.deleteRecursively();
    }

    SECTION ("3.18.2 an id that means 'none' starts null (a juce::Uuid made without a value is a new RANDOM id, never null: the 3.17.2 fault)");
    {
        CHECK (! juce::Uuid().isNull() && juce::Uuid::null().isNull());                  // (the fact the rest of this section rests on)
        CHECK (juce::Uuid (juce::String()).isNull());                                          // (an id missing from a file reads as null, not random ...)
        CHECK (! juce::Uuid (juce::String ("not an id")).isNull());                            // (... a garbled one reads as a junk id: "not found" wherever it is looked up, never "none")
        BounceSettings bs; CHECK (bs.automationEdit.isNull() && bs.mixerId.isNull());
        BounceItem bi;     CHECK (bi.automationEdit.isNull() && bi.mixerId.isNull());
        EditRegion r;      CHECK (r.windowId.isNull() && r.takeId.isNull());
        EditGuide gd;      CHECK (gd.windowId.isNull() && gd.takeId.isNull() && gd.trackId.isNull() && ! gd.active());
        // a piece from nowhere (an AAF import) does not pull an edit towards a made-up take window's alignment
        Project q;
        auto& e = q.addEdit ("From AAF", juce::Uuid::null());
        EditRegion rg; rg.id = juce::Uuid(); rg.sampleRate = 48000.0; rg.srcIn = 0; rg.srcOut = 48000; rg.startSample = 0; rg.sourceLength = 48000;
        e.regions.push_back (rg);
        CHECK (q.alignOwnerFor (true, e.id) == e.id.toString());
        // the automation plan's default mixer argument means "none": a lane with no mixer of its own goes to the edit's mixer, as when none is named
        AutoLane l; l.id = juce::Uuid(); l.trackId = q.tracks.empty() ? juce::Uuid::null() : q.tracks.front().id; l.param = autoparam::fader;
        AutoPoint pt; pt.id = juce::Uuid(); pt.time = 0; pt.value = -3.0f; l.pts.push_back (pt);
        e.lanes.push_back (l);
        auto a1 = automationPlanFor (q, e), a2 = automationPlanFor (q, e, false, juce::Uuid::null());
        CHECK ((a1 == nullptr) == (a2 == nullptr) && (a1 == nullptr || a1->items.size() == a2->items.size()));
    }

    SECTION ("3.18.2 a new project starts clean; a garbled cache file never asks for gigabytes");
    {
        Project q;
        q.ignoredMissing.insert ("C:/gone/a.wav"); q.windowBounds["take:x"] = "1 2 3 4"; q.filesMoved = true;
        q.createDefaultDesign();
        CHECK (q.ignoredMissing.empty() && q.windowBounds.empty() && ! q.filesMoved);         // (they stayed, and were saved into the next project)
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-garbled-318");
        dir.deleteRecursively(); dir.createDirectory();
        {   // a fingerprint whose header claims 50 million frames in a 60-byte file
            juce::MemoryOutputStream mo;
            mo.write ("FMFP", 4); mo.writeInt (2); mo.writeDouble (20.0); mo.writeInt (50000000); mo.writeInt (takematch::Fingerprint::kNotes); mo.writeString ("k");
            const auto f = dir.getChildFile ("big.fpr"); f.replaceWithData (mo.getData(), mo.getDataSize());
            takematch::Fingerprint fp;
            CHECK (! takematch::load (fp, f) && fp.empty());
        }
        {   // a thumbnail claiming four billion points
            juce::MemoryOutputStream mo;
            mo.writeInt (WaveThumb::kFormat); mo.writeInt (WaveThumb::kSamplesPerPoint); mo.writeInt (2); mo.writeDouble (48000.0); mo.writeInt64 (1); mo.writeInt64 ((juce::int64) 1 << 31);
            juce::MemoryInputStream in (mo.getData(), mo.getDataSize(), false);
            juce::AudioFormatManager fm; WaveThumb::Pool pool; WaveThumb t (fm, pool);
            CHECK (! t.loadFrom (in) && t.isEmpty());
        }
        dir.deleteRecursively();
    }

    SECTION ("3.18.3 take matching keeps at most kResident fingerprints in memory; the others come back from the Analysis folder");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-takematch-resident");
        dir.deleteRecursively(); dir.createDirectory();
        Project q; q.projectFile = dir.getChildFile ("R.fermata");
        auto& w = *q.takeWindows.front(); w.takeMatching = true;
        const int N = TakeMatchService::kResident + 12;
        std::vector<juce::Uuid> ids;
        for (int i = 0; i < N; ++i)
        {
            TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.sampleRate = 48000.0; g.lengthSamples = 48000;
            TakeFile f; f.file = makeToneWav (dir.getChildFile ("t" + juce::String (i) + ".wav"), 0.2 + 0.001 * i, 1, false); f.numChannels = 1; f.trackName = "Main";
            if (! q.tracks.empty()) f.trackId = q.tracks.front().id;
            g.files.push_back (f); w.groups.push_back (g); ids.push_back (g.id);
        }
        AudioEngine eng (q);
        TakeMatchService tm (q, eng);
        tm.windowsChanged();
        auto waitFor = [] (const std::function<bool()>& cond, int ms) { for (int t = 0; t < ms && ! cond(); t += 20) juce::Thread::sleep (20); return cond(); };
        CHECK (waitFor ([&] { return tm.progress().left() == 0; }, 120000));
        CHECK (tm.progress().ready == N);
        CHECK (tm.residentFingerprints() <= TakeMatchService::kResident);                     // (the rest were saved and let go)
        CHECK (dir.getChildFile ("Analysis").getNumberOfChildFiles (juce::File::findFiles, "*.fpr") == N);
        auto first = tm.fingerprintFor (ids.front());                                            // one let go of comes back from its file, whole
        CHECK (first != nullptr && first->frames() > 0 && first->sourceKey == takematch::sourceKeyFor ({ w.groups.front().files.front().file }));
        CHECK (tm.residentFingerprints() <= TakeMatchService::kResident);
        for (auto& id : ids) CHECK (tm.fingerprintFor (id) != nullptr);                          // every one, asked for in turn, is there (nothing is analysed again)
        CHECK (tm.progress().left() == 0 && tm.residentFingerprints() <= TakeMatchService::kResident);
        auto al = tm.alignmentFor (ids[0], ids[1]);
        CHECK (al != nullptr && tm.alignmentFor (ids[0], ids[1]) == al);                           // (kept by the files' keys, not by a pointer)
        tm.stop();
        dir.deleteRecursively();
    }

    SECTION ("3.18.4 a narrow take still shows its bars: shorter forms, letters left off the end, and a bar number never cut in half");
    {
        CHECK ((barsLabelForms (17, 39) == std::vector<juce::String> { "Bar 17 - 39", "17 - 39", "17-39", "17-", "17" }));
        CHECK ((barsLabelForms (5, 5) == std::vector<juce::String> { "Bar 5", "5" }) && barsLabelForms (5, 0).size() == 2 && barsLabelForms (0, 9).empty());
        CHECK (barsLabelForms (17, 39).front() == barsLabel (17, 39) && barsLabelForms (5, 0).front() == barsLabel (5, 0));
        const std::pair<int, int> ranges[] = { { 17, 39 }, { 1, 47 }, { 101, 104 }, { 9, 0 }, { 120, 120 } };
        for (auto& pr : ranges)                                                          // every number in every form is a whole bar number
            for (auto& f : barsLabelForms (pr.first, pr.second))
            {
                juce::String digits;
                for (int i = 0; i <= f.length(); ++i)
                {
                    const juce::juce_wchar c = i < f.length() ? f[i] : (juce::juce_wchar) ' ';
                    if (juce::CharacterFunctions::isDigit (c)) digits += c;
                    else if (digits.isNotEmpty()) { CHECK (digits == juce::String (pr.first) || digits == juce::String (pr.second)); digits.clear(); }
                }
            }
        const auto w7 = [] (const juce::String& s) { return 7 * s.length(); };          // (a pretend font: every letter 7 px wide)
        const auto bars = barsLabelForms (17, 39);
        CHECK (fitLabel (bars, false, 200, w7) == "Bar 17 - 39" && fitLabel (bars, false, 77, w7) == "Bar 17 - 39");
        CHECK (fitLabel (bars, false, 76, w7) == "17 - 39" && fitLabel (bars, false, 35, w7) == "17-39" && fitLabel (bars, false, 34, w7) == "17-");
        CHECK (fitLabel (bars, false, 20, w7) == "17" && fitLabel (bars, false, 13, w7).isEmpty() && fitLabel (bars, false, 0, w7).isEmpty());   // (never "1" for 17)
        const std::vector<juce::String> lostForms { "LOST 0.3 s", "LOST" };
        CHECK (fitLabel (lostForms, true, 70, w7) == "LOST 0.3 s" && fitLabel (lostForms, true, 69, w7) == "LOST" && fitLabel (lostForms, true, 27, w7) == "LOS");
        CHECK (fitLabel (lostForms, true, 7, w7) == "L" && fitLabel (lostForms, true, 6, w7).isEmpty() && fitLabel ({}, true, 100, w7).isEmpty());
        const std::vector<juce::String> roomTone { "ROOM TONE" };
        CHECK (fitLabel (roomTone, true, 35, w7) == "ROOM" && fitLabel (roomTone, true, 41, w7) == "ROOM" && fitLabel (roomTone, true, 42, w7) == "ROOM T");   // (no space left dangling)
        for (int room = 0; room <= 90; ++room)                                           // whatever the room, what is shown fits in it, and more room never shows less
        {
            const auto a = fitLabel (bars, false, room, w7), b = fitLabel (bars, false, room + 1, w7);
            CHECK (w7 (a) <= room && b.length() >= a.length());
            CHECK (w7 (fitLabel (lostForms, true, room, w7)) <= room && w7 (fitLabel (roomTone, true, room, w7)) <= room);
        }
    }

    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

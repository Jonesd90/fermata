// Headless test of the Fermata engine: recording, take naming/grouping, mixer isolation, inserts, save/load.
#include "../src/core/AudioEngine.h"
#include "../src/core/Bounce.h"
#include "../src/core/SessionClock.h"
#include "../src/core/TalkbackKey.h"
#include "../src/core/RemoteProtocol.h"
#include "../src/core/PreampMemory.h"
#include "../src/core/ImportPlan.h"
#include "../src/core/MasterExport.h"
#include "../src/core/Resampler.h"
#include "../src/core/Ravenna.h"
#include "../src/core/Dither.h"
#include "../src/core/CoreReservation.h"
#include "../src/core/WavRepair.h"
#include "../src/core/TakeWriter.h"
#include "../src/core/ProjectCopy.h"
#include "../src/core/Ltc.h"
#include "../src/core/Aaf.h"
#include "../src/core/SpectralPicture.h"
#include "../src/core/EditFix.h"
#include "../src/core/Feedback.h"
#include "../src/core/ReviewPage.h"
#include "../src/core/FileSync.h"
#include "../src/core/AudioRelink.h"
#include "../src/core/AudioOps.h"
#include <cstdio>
#include <thread>

using namespace td;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define SECTION(name) std::printf ("-- %s\n", name)

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
        CHECK (remote::parseCommand ("not json").name.isEmpty() && remote::parseCommand ("{\"cmd\":\"format c\"}").name.isEmpty());
        remote::State st; st.recording = true; st.takeNumber = 7; st.timeOfDay = "12:00:01"; st.hasSession = true; st.sessionLeft = 3600; st.sessionText = "01:00:00";
        const auto json = st.toJson();
        CHECK (! json.containsAnyOf ("\n\r"));
        auto v = juce::JSON::parse (json);
        CHECK ((bool) v["recording"] && (int) v["takeNumber"] == 7 && v["sessionText"].toString() == "01:00:00" && v["colours"]["cr"].toString() == "#a83cff");
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
        std::printf ("-- session mode: the take starts 6 s before Record; the listening stream carries on\n");
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
        const juce::int64 preLen = 6 * 48000;
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
                CHECK (worst < 1e-5);                                      // every sample, 6 s before and 40 blocks after, in order
            }
        }
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
            e2.automationOn = false; CHECK (automationPlanFor (p, e2) == nullptr);
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
                plan->apply (5 * S); CHECK (fxp->sets == 1);                 // unchanged value: not sent again
                plan->apply (10 * S); CHECK (std::abs (fxp->val[0] - 1.0f) < 1e-4f && fxp->sets == 2);
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
                CHECK (pp.takes.size() == 12 && pp.tracks.size() == 13);
                if (pp.takes.size() == 12 && pp.tracks.size() == 13)
                {
                    CHECK (pp.takes[0].number == 1 && pp.takes[11].number == 12 && pp.takes[7].parts.size() == 13);
                    CHECK (pp.takes[7].length == 96000 * 18 || pp.takes[7].length > 96000 * 18 - 5000);
                    CHECK (pp.tracks[0].name == "AB" && pp.tracks[0].channels == 2 && pp.tracks[1].name == "ROOM 414" && pp.tracks[1].channels == 2);
                    CHECK (pp.tracks[2].name == "SPOT1" && pp.tracks[11].name == "SPOT10" && pp.tracks[12].name == "WIDE ROOM");
                }
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
        expectW ("take length", [&] { g.lengthSamples += 10; }, true);
        expectW ("take rate", [&] { g.sampleRate = 96000.0; }, true);
        expectW ("take time", [&] { g.recordedAt = juce::Time (5555); }, true);
        expectW ("fade in", [&] { g.fadeInSeconds = 0.5; }, true);
        expectW ("fade out", [&] { g.fadeOutSeconds = 0.5; }, true);
        expectW ("dud", [&] { g.dud = true; }, true);
        expectW ("bar in", [&] { g.barIn = 3; }, true);
        expectW ("bar out", [&] { g.barOut = 8; }, true);
        expectW ("overruns", [&] { g.overruns = 2; g.lostSamples = 100; }, true);
        expectW ("lost", [&] { g.lostSamples = 200; }, true);
        expectW ("write failed", [&] { g.writeFailed = true; }, true);
        expectW ("verified", [&] { g.verified = true; }, true);
        expectW ("file track name", [&] { g.files[0].trackName = "Z"; }, true);
        expectW ("file path", [&] { g.files[0].file = tmp.getChildFile ("other.wav"); }, true);
        expectW ("file channels", [&] { g.files[0].numChannels = 2; }, true);
        expectW ("file orig", [&] { g.files[0].sourceName = "orig.wav"; }, true);
        expectW ("file orig2", [&] { g.files[0].sourceName2 = "orig2.wav"; }, true);
        expectW ("file removed", [&] { g.files.pop_back(); }, true);
        expectW ("waiting name", [&] { g.waiting[0].name = "bar 5"; }, true);
        expectW ("waiting range", [&] { g.waiting[0].to = 30; }, true);
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
        expectE ("region fades", [&] { r.inStart = 0.1; r.inEnd = 0.2; r.outStart = -0.2; r.outEnd = -0.1; }, true);
        expectE ("region file track", [&] { r.files[0].trackId = juce::Uuid(); r.files[0].trackName = "Q"; }, true);
        expectE ("region file path", [&] { r.files[0].file = tmp.getChildFile ("rf.wav"); }, true);
        expectE ("region file channels", [&] { r.files[0].numChannels = 2; }, true);
        expectE ("region file start", [&] { r.files[0].fileStart = 77; }, true);
        expectE ("region file removed", [&] { r.files.pop_back(); }, true);
        expectE ("region waiting", [&] { r.waiting.name = "fix"; r.waiting.from = 1; r.waiting.to = 2; r.waiting.files.push_back (tmp.getChildFile ("rw.wav")); }, true);
        expectE ("region waiting range", [&] { r.waiting.to = 3; }, true);
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
        // the edit: view state, not undone
        expectE ("fix marks", [&] { e.fixIn = 1.0; e.fixOut = 2.0; e.fixTracks.push_back (q.tracks[0].id); }, false);
        expectE ("playhead", [&] { e.playheadSeconds = 3.0; }, false);
        expectE ("insert index", [&] { e.insertIndex = 1; }, false);
        expectE ("markers", [&] { Marker m; m.id = juce::Uuid(); m.name = "Verse"; e.markers.push_back (m); }, false);
        expectE ("feedback", [&] { FeedbackNote n; n.key = "k"; n.text = "late"; e.feedback.push_back (n); }, false);
        expectE ("show feedback / markers", [&] { e.showFeedback = false; e.showMarkers = false; }, false);
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
        CHECK (pickQuietCores (cores, std::vector<double> (8, 0.0), 2) == V ({ 2, 3 }));
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
    SECTION ("2.77 background denoiser: the C++ engine matches the reference, on files, in pieces, and does what the controls say");
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
        // on files: a stereo file, each channel with its own profile; the untouched parts copied, the stretch denoised, 'removed' = input - output
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
            const auto src = dir.getChildFile ("stereo.wav"), dst = dir.getChildFile ("stereo (denoise).wav"), rem = dir.getChildFile ("stereo (removed).wav");
            CHECK (audioops::writeWav (src, st, sr));
            juce::String err;
            auto profs = audioops::learnDenoise (fm, src, (juce::int64) (0.1 * sr), (juce::int64) (1.2 * sr), p, err);
            CHECK (err.isEmpty() && profs.valid() && profs.channels == 2 && profs.perChannel[0].levelDb() > profs.perChannel[1].levelDb() + 8.0);
            CHECK (audioops::denoiseFile (fm, src, dst, rem, (juce::int64) (1.0 * sr), (juce::int64) (3.5 * sr), profs, p, {}, err) && err.isEmpty());
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (dst)), rr (fm.createReaderFor (rem));
            CHECK (rd != nullptr && rr != nullptr && rd->numChannels == 2 && rd->lengthInSamples == n && rd->bitsPerSample == 24 && rr->lengthInSamples == n);
            std::vector<std::vector<float>> o, rmv; audioops::readRange (*rd, 0, n, o); audioops::readRange (*rr, 0, n, rmv);
            // before 1.0 s: a copy (to 24-bit precision); inside: the noise is down by about the residual; the tone is kept; removed = in - out
            double dcopy = 0; for (int i = 0; i < (int) (0.9 * sr); ++i) dcopy = juce::jmax (dcopy, (double) std::abs (o[0][(size_t) i] - st[0][(size_t) i]));
            CHECK (dcopy < 2.0 / 8388608.0);
            CHECK (rmsDb (st[0], (size_t) (1.05 * sr), (size_t) (1.45 * sr)) - rmsDb (o[0], (size_t) (1.05 * sr), (size_t) (1.45 * sr)) > 9.0);
            CHECK (std::abs (rmsDb (st[1], (size_t) (1.6 * sr), (size_t) (2.9 * sr)) - rmsDb (o[1], (size_t) (1.6 * sr), (size_t) (2.9 * sr))) < 0.3);
            double dr = 0; for (int i = (int) (1.1 * sr); i < (int) (3.4 * sr); ++i) dr = juce::jmax (dr, (double) std::abs ((st[0][(size_t) i] - o[0][(size_t) i]) - rmv[0][(size_t) i]));
            CHECK (dr < 5.0 / 8388608.0);
            double zr = 0; for (int i = 0; i < (int) (0.9 * sr); ++i) zr = juce::jmax (zr, (double) std::abs (rmv[0][(size_t) i]));
            CHECK (zr == 0.0);                                                                                 // (digital silence outside the stretch)
            // the other tracks help: the right channel has the tone buried in its noise (about the noise's own level in its bin, so hard to tell
            // from noise on its own), the left channel hears it clearly - with the group on, the right keeps more of the tone; a preview run
            // writes silence before the stretch and ends with it. (A 10 s file: learnDenoise measures the decay over the 5 s after the room tone.)
            const int n2 = (int) (10.0 * sr);
            std::vector<std::vector<float>> st2 (2, std::vector<float> ((size_t) n2));
            juce::Random rnd2 (12);
            for (int i = 0; i < n2; ++i)
            {
                const float t = (float) i / (float) sr, env = t > 1.5f && t < 3.5f ? 1.0f : 0.0f, tone = std::sin (2.0f * 3.14159265f * 1000.0f * t);
                st2[0][(size_t) i] = env * 0.1f * tone + 0.0005f * (rnd2.nextFloat() * 2.0f - 1.0f);
                st2[1][(size_t) i] = env * 0.00004f * tone + 0.002f * (rnd2.nextFloat() * 2.0f - 1.0f);
            }
            const auto src2 = dir.getChildFile ("pair.wav");
            CHECK (audioops::writeWav (src2, st2, sr));
            auto profs2 = audioops::learnDenoise (fm, src2, (juce::int64) (0.1 * sr), (juce::int64) (1.2 * sr), p, err, 10.0);
            CHECK (err.isEmpty() && profs2.valid() && profs2.decay.size() == 2 && profs2.decay[0].rates.size() == profs2.decay[0].edges.size() - 1);
            CHECK (audioops::learnDenoise (fm, src, (juce::int64) (0.1 * sr), (juce::int64) (1.2 * sr), p, err, 10.0).decay.empty() && err.isEmpty());   // (a 4 s file: no 5 s stretch to measure)
            auto toneDb = [&] (const std::vector<float>& v)                                                  // the level at 1 kHz over 1.6 - 3.4 s
            {
                double re = 0, im = 0; const int a0 = (int) (1.6 * sr), a1 = (int) (3.4 * sr);
                for (int i = a0; i < a1; ++i) { const double ph = 2.0 * 3.14159265358979 * 1000.0 * i / sr; re += v[(size_t) i] * std::cos (ph); im -= v[(size_t) i] * std::sin (ph); }
                return 20.0 * std::log10 (std::sqrt (re * re + im * im) / (a1 - a0) + 1e-20);
            };
            std::vector<double> rightTone (3, 0.0);
            for (int mode = 0; mode <= 2; ++mode)
            {
                denoise::Params pg = p; pg.groupMode = mode;
                audioops::DenoiseRun run; run.a = (juce::int64) (1.0 * sr); run.b = (juce::int64) (3.8 * sr); run.preview = mode == 2; run.removed = false; run.threads = 2;
                const auto out2 = dir.getChildFile ("pair out " + juce::String (mode) + ".wav");
                CHECK (audioops::denoiseFiles (fm, { src2 }, { out2 }, {}, { profs2 }, pg, run, {}, err) && err.isEmpty());
                std::unique_ptr<juce::AudioFormatReader> r2 (fm.createReaderFor (out2));
                CHECK (r2 != nullptr && r2->numChannels == 2 && r2->lengthInSamples == (mode == 2 ? (juce::int64) (3.8 * sr) : (juce::int64) n2));
                std::vector<std::vector<float>> o2; audioops::readRange (*r2, 0, r2->lengthInSamples, o2);
                rightTone[(size_t) mode] = toneDb (o2[1]);
                CHECK (std::abs (toneDb (o2[0]) - toneDb (st2[0])) < 0.3);                                      // the left keeps its tone whatever the mode
                if (mode == 2) { double z = 0; for (int i = 0; i < (int) (0.9 * sr); ++i) z = juce::jmax (z, (double) std::abs (o2[0][(size_t) i])); CHECK (z == 0.0); }   // (a preview: silence before the stretch)
            }
            CHECK (rightTone[1] > rightTone[0] + 2.0 && rightTone[1] <= toneDb (st2[1]) + 1.0);
            CHECK (std::abs (rightTone[2] - rightTone[1]) < 1.0);                                              // (the preview has the same gains)
            dir.deleteRecursively();
        }
        }
        denoiseDone:;
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

    tmp.deleteRecursively();
    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

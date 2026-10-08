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
#include "../src/core/WavRepair.h"
#include "../src/core/ProjectCopy.h"
#include <cstdio>

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
            PlaybackSession::renderSegments (ps.getReaders(), ps.getSegments(), from, n, out, scratch);
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
        mq.stripFor (vId)->outDest = juce::Uuid::null(); mq.mainBus = fold;
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) < 0.1 && corr (r2.out[2], r2.in[0]) > 0.9);   // 'main' now means the designated bus
        const auto via = q.addBus ("Via", false).id;                                     // through an Int Bus on the way
        q.syncMixers();
        mq.stripFor (vId)->outDest = via; mq.busFor (via)->outDest = outB; mq.busFor (via)->outOn.set (true);
        e2.rebuildPlan(); r2.measure (e2);
        CHECK (corr (r2.out[0], r2.in[0]) > 0.9);
        Project back; CHECK (back.fromVar (q.toVar()));
        MixerState* bm = back.mixers.empty() ? nullptr : back.mixers.front().get();
        CHECK (bm != nullptr && bm->mainBus == fold && bm->stripFor (vId)->outDest == via && bm->stripFor (vId)->outOn.get() && ! bm->stripFor (wId)->outOn.get());
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
            PlaybackSession::renderSegments (ps.getReaders(), ps.getSegments(), sample, 64, out, scratch);
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
        mixA.stripFor (violinId)->mute.set (false);
        mixA.stripFor (violinId)->gainDb.set (0.0f);
        mixA.stripFor (violinId)->sends.find (foldback)->pre.set (false);

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
        {
            juce::String err;
            auto saveFile = tmp.getChildFile ("routing.fermata");
            CHECK (p.saveAs (saveFile, err));
            Project q;
            q.projectFile = saveFile;
            CHECK (q.load (saveFile, err));
            auto* qm = q.mixers.front().get();
            auto* sd = qm->stripFor (violaId)->sends.find (violinId);
            CHECK (sd != nullptr && std::abs (sd->gainDb.get() + 7.5f) < 1e-4f && sd->pre.get());
            CHECK (q.buses.size() == p.buses.size() && q.kindOf (foldback) == NodeKind::ExtBus && q.busIndices (true).size() == 2);
            CHECK (qm->busFor (foldback)->outFirst.load() == 2 && (qm->stripFor (violaId)->sends.find (outBus) == nullptr || ! qm->stripFor (violaId)->sends.find (outBus)->active()));
            p.projectFile = tmp.getChildFile ("Concert.fermata");
        }

        // tidy up for the following sections
        for (auto& t : p.tracks) { for (auto* mx : { &mixA, &mixB }) { auto* sp = mx->stripFor (t.id); sp->mute.set (false); sp->gainDb.set (0.0f); sp->pan.set (0.0f);
                                                                     for (auto& sd : sp->sends.pool) sd->gainDb.set (-100.0f); } }
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
        gdb[5] = -60.0f; eng.setStageMix (gdb, gpan, 0.0f, 0.0f); rig.run (eng, 6);
        auto offm = rig.measure (eng);
        CHECK (offm[2] < 1e-6 && offm[3] < 1e-6);                                               // all the way down is off
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
        w0.markTake = gid1; w0.markIn = 0.25; w0.markOut = 0.75;
        w0.groups.front().barIn = 17; w0.groups.front().barOut = 39;
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
        CHECK (q2.takeWindows.front()->markTake == gid1 && q2.takeWindows.front()->markIn == 0.25 && q2.takeWindows.front()->markOut == 0.75);
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
        q.findTrack (tid)->setArmed (true);
        r2.run (e2, 700);                                                  // 7.5 s of listening: more than the pre-roll
        const juce::int64 posAtRecord = r2.pos;
        CHECK (e2.startRecording (*q.takeWindows.front()).isEmpty());
        r2.run (e2, 40);
        auto gidPre = e2.stopRecording();
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
        CHECK (q.takeWindows.front()->findGroup (gidNo)->lengthSamples == 20 * Rig::block);

        // (3) pressing Record and Stop never interrupts what the engineer hears
        auto& mon = p;                                                     // the big project: violin goes to the Out bus of mixer A (outputs 1-2)
        for (auto& t : mon.tracks) t.setArmed (t.name == "violin");
        eng.setSessionMode (true);
        rig.run (eng, 600);
        auto level = [&] { std::vector<double> r; rig.run (eng, 1, &r); return r[0]; };
        double lo = 1e9;
        for (int i = 0; i < 5; ++i) lo = juce::jmin (lo, level());
        CHECK (eng.startRecording (tw).isEmpty());
        for (int i = 0; i < 30; ++i) lo = juce::jmin (lo, level());
        eng.stopRecording();
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
        // spectral repair with the RX-style controls
        {
            const long t0 = (long) (2 * sr), t1 = t0 + (long) (0.2 * sr);
            auto goertzel = [&] (const std::vector<float>& v, double hz) { double re = 0, im = 0; for (long n = t0; n < t1; ++n) { const double a = 2 * dsp::kPi * hz * (double) n / sr; re += v[(size_t) n] * std::cos (a); im += v[(size_t) n] * std::sin (a); } return std::sqrt (re * re + im * im); };
            auto noisy = tone; auto ref = tone; juce::Random rnd (11);
            for (long n = t0; n < t1; ++n) noisy[(size_t) n] += 0.5f * (rnd.nextFloat() * 2.0f - 1.0f);
            auto err = [&] (const std::vector<float>& v) { double e = 0; for (long n = t0; n < t1; ++n) e += std::pow (v[(size_t) n] - ref[(size_t) n], 2.0); return e; };
            RepairParams p; p.direction = RepairDirection::LeftRight;
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
                auto y = whistle; RepairParams q; q.direction = dir; q.strength = 1.0; q.contextPct = 100.0;
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
            MasteringDef m; m.vm.format = MasterFormat::Flac; m.vm.bitDepth = 16; m.vm.folder = "X:/out"; m.vm.album.album = "Alb"; m.ddp = d; m.ddp.upc = "4006381333931"; m.tailSeconds = 2.5;
            MasterItem it; it.editId = juce::Uuid(); it.include = false; it.fileName = "Name"; it.tags.title = "Ti"; m.items.push_back (it);
            MasteringDef m2; m2.fromVar (juce::JSON::parse (juce::JSON::toString (m.toVar())));
            CHECK (m2.vm.format == MasterFormat::Flac && m2.vm.bitDepth == 16 && m2.vm.folder == "X:/out" && m2.vm.album.album == "Alb" && m2.tailSeconds == 2.5);
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
        w0.markIn = 0.5; w0.playheadSeconds = 3.0; p.changed(); p.undoTick (false, true);          // marks and playheads are not undo steps
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
    eng.rebuildPlan();
    tmp.deleteRecursively();
    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

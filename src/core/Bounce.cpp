#include "Bounce.h"
#include "Playback.h"
#include "Dither.h"

namespace td
{
Bouncer::Bouncer (const Project& live, const BounceSettings& s) : settings (s)
{
    bool haveMixer = false;
    for (auto& m : live.mixers) haveMixer = haveMixer || m->id == s.mixerId;
    if (! haveMixer) { prepareError = "The mixer to bounce through no longer exists."; return; }
    if (s.sources.empty()) { prepareError = "Choose at least one output to bounce (an Ext bus, an Int bus or an audio track)."; return; }
    if (s.items.empty()) { prepareError = "Nothing to bounce."; return; }
    shadow = live.cloneForRender (s.sampleRate, kBlock);
    engine = std::make_unique<AudioEngine> (*shadow);
    engine->setOfflineTarget (s.mixerId, s.sources);
    engine->prepare (s.sampleRate, kBlock);
}

Bouncer::~Bouncer() = default;

juce::File Bouncer::uniqueFile (const juce::String& name) const
{
    return settings.folder.getNonexistentChildFile (sanitiseForFile (name), ".wav", true);
}

juce::String Bouncer::fileName (const BounceItem& it, const juce::String& outputName, bool several) const
{
    return (several || settings.addOutputName) ? it.name + " - " + outputName : it.name;
}

BounceResult Bouncer::render (std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    BounceResult res;
    if (prepareError.isNotEmpty()) { res.error = prepareError; return res; }
    if (! settings.folder.createDirectory()) { res.error = "Cannot create the folder " + settings.folder.getFullPathName(); return res; }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    auto openReaders = [&] (const std::vector<PlaySegment>& segs) -> bool
    {
        readers.clear();
        for (auto& s : segs)
        {
            std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (s.file));
            if (r == nullptr) { res.error = "Cannot read " + s.file.getFullPathName(); return false; }
            if (std::abs (r->sampleRate - settings.sampleRate) > 0.5) { res.error = s.file.getFileName() + " is " + juce::String (r->sampleRate, 0) + " Hz, not " + juce::String (settings.sampleRate, 0) + " Hz"; return false; }
            readers.push_back (std::move (r));
        }
        return true;
    };
    std::shared_ptr<AutomationPlan> autoPlan;                 // the automation of the edit being bounced, aimed at the render copy's mixers
    auto planForEdit = [&] (const juce::Uuid& editId, const juce::Uuid& renderedMixer) -> std::shared_ptr<AutomationPlan>
    {
        if (editId.isNull()) return nullptr;
        if (auto* ed = shadow->findEdit (editId)) return automationPlanFor (*shadow, *ed, false, renderedMixer);      // the automation must move the mixer that is actually being rendered
        return nullptr;
    };
    std::vector<juce::AudioBuffer<float>> trackBufs;
    for (auto& t : shadow->tracks) trackBufs.emplace_back (juce::jlimit (1, kMaxTrackChannels, t.channelCount()), kBlock);
    juce::AudioBuffer<float> scratch (kMaxTrackChannels, 16384);

    // the outputs: the chosen channels, in the order given (matches the engine's capture order)
    std::vector<juce::String> outNames;
    for (auto& id : settings.sources) outNames.push_back (shadow->nodeName (id));
    const int numOut = (int) outNames.size();
    const bool several = numOut > 1;
    std::vector<juce::AudioBuffer<float>> captures;
    for (int i = 0; i < numOut; ++i) captures.emplace_back (2, kBlock);

    const auto lead = (juce::int64) std::llround (settings.leadSeconds * settings.sampleRate);
    const auto tail = (juce::int64) std::llround (settings.tailSeconds * settings.sampleRate);
    double totalAll = 0.0, doneAll = 0.0;
    for (auto& it : settings.items) totalAll += (double) juce::jmax ((juce::int64) 0, it.end - it.start) + (double) lead + (double) tail;
    if (totalAll <= 0.0) { res.error = "The range to bounce is empty."; return res; }

    juce::WavAudioFormat wav;
    auto makeWriter = [&] (const juce::File& f, int bits) -> std::unique_ptr<juce::AudioFormatWriter>
    {
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
        if (os == nullptr) return nullptr;
        auto opts = juce::AudioFormatWriterOptions().withSampleRate (settings.sampleRate).withNumChannels (2).withBitsPerSample (bits);
        if (bits == 32) opts = opts.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        return wav.createWriterFor (os, opts);
    };
    float overallPeak = 0.0f;
    juce::Array<juce::File> toClean;                  // anything half-made, removed if we stop early
    auto cleanUp = [&] { for (auto& f : toClean) f.deleteFile(); };       // files of earlier, finished pieces are kept

    for (auto& it : settings.items)
    {
        const auto first = it.start - lead;
        const auto total = (it.end - it.start) + lead + tail;
        if (total <= 0) continue;
        const auto& segs = it.segments.empty() ? settings.segments : it.segments;
        if (! openReaders (segs)) return res;
        juce::Uuid renderedMixer = settings.mixerId;
        {
            bool have = false;                                   // a piece may have a mixer of its own (an Edit's mixer); if it is gone, the chosen mixer is used
            for (auto& m : shadow->mixers) have = have || m->id == it.mixerId;
            if (have && ! it.mixerId.isNull()) renderedMixer = it.mixerId;
            engine->setOfflineTarget (renderedMixer, settings.sources);
        }
        autoPlan = planForEdit (it.automationEdit.isNull() ? settings.automationEdit : it.automationEdit, renderedMixer);

        std::vector<juce::File> finals, tmps;
        std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers;
        std::vector<std::unique_ptr<DitherOut>> douts;            // 24-bit files get TPDF dither; 32-bit ones are written as they are
        std::vector<float> peaks ((size_t) numOut, 0.0f);
        for (int o = 0; o < numOut; ++o)
        {
            const auto finalFile = uniqueFile (fileName (it, outNames[(size_t) o], several));
            const auto tmpFile = settings.folder.getNonexistentChildFile ("bounce-working", ".wav", false);
            finals.push_back (finalFile); tmps.push_back (tmpFile);
            auto w = makeWriter (settings.normalise ? tmpFile : finalFile, (settings.normalise || settings.floatFiles) ? 32 : 24);
            if (w == nullptr) { writers.clear(); cleanUp(); res.error = "Cannot write " + finalFile.getFullPathName(); return res; }
            toClean.add (settings.normalise ? tmpFile : finalFile);
            writers.push_back (std::move (w));
            douts.push_back (std::make_unique<DitherOut> (*writers.back(), true, 0x2545F491u + (juce::uint32) o * 7919u));
        }

        for (juce::int64 done = 0; done < total;)
        {
            if (cancel != nullptr && cancel->load()) { writers.clear(); cleanUp(); res.cancelled = true; return res; }
            const int n = (int) juce::jmin ((juce::int64) kBlock, total - done);
            PlaybackSession::renderSegments (readers, segs, first + done, n, trackBufs, scratch);
            // the padding is silence going in (so plug-ins settle and tails ring out); only the piece itself is fed in
            const auto pos0 = first + done;
            if (pos0 < it.start || pos0 + n > it.end)
                for (auto& tb : trackBufs)
                    for (int i = 0; i < n; ++i)
                        if (pos0 + i < it.start || pos0 + i >= it.end)
                            for (int c = 0; c < tb.getNumChannels(); ++c) tb.setSample (c, i, 0.0f);
            if (autoPlan != nullptr) autoPlan->apply (first + done);
            engine->renderOffline (trackBufs, n, captures);
            for (int o = 0; o < numOut; ++o)
            {
                auto& cap = captures[(size_t) o];
                for (int c = 0; c < 2; ++c) { const auto mm = cap.findMinMax (c, 0, n); peaks[(size_t) o] = juce::jmax (peaks[(size_t) o], std::abs (mm.getStart()), std::abs (mm.getEnd())); }
                douts[(size_t) o]->write (cap, 0, n);
            }
            done += n; doneAll += n;
            if (progress != nullptr) progress->store ((float) (doneAll / totalAll));
        }
        writers.clear();

        float loudest = 0.0f;
        for (auto pk : peaks) loudest = juce::jmax (loudest, pk);
        for (int o = 0; o < numOut; ++o)
        {
            float finalPeak = peaks[(size_t) o];
            if (settings.normalise)
            {
                const auto& tmpFile = tmps[(size_t) o];
                const auto finalFile = uniqueFile (fileName (it, outNames[(size_t) o], several));
                std::unique_ptr<juce::AudioFormatReader> in (formats.createReaderFor (tmpFile));
                auto out = makeWriter (finalFile, 24);
                std::unique_ptr<DitherOut> dout; if (out != nullptr) dout = std::make_unique<DitherOut> (*out, true, 0x6A09E667u + (juce::uint32) o * 7919u);
                if (in == nullptr || out == nullptr) { in.reset(); out.reset(); cleanUp(); res.error = "Cannot write " + finalFile.getFullPathName(); return res; }
                const float ref = settings.normaliseTogether ? loudest : peaks[(size_t) o];
                const float gain = ref > 1.0e-9f ? juce::Decibels::decibelsToGain (settings.peakDb) / ref : 1.0f;
                juce::AudioBuffer<float> buf (2, 16384);
                for (juce::int64 pos = 0; pos < in->lengthInSamples; pos += 16384)
                {
                    const int n = (int) juce::jmin ((juce::int64) 16384, in->lengthInSamples - pos);
                    in->read (&buf, 0, n, pos, true, true);
                    buf.applyGain (0, n, gain);
                    dout->write (buf, 0, n);
                }
                out.reset(); in.reset();
                tmpFile.deleteFile();
                finalPeak = peaks[(size_t) o] * gain;
                res.files.add (finalFile.getFullPathName());
            }
            else
                res.files.add (finals[(size_t) o].getFullPathName());
            overallPeak = juce::jmax (overallPeak, finalPeak);
        }
        toClean.clear();
    }
    res.peakDb = juce::Decibels::gainToDecibels (overallPeak, -100.0f);
    res.clipped = overallPeak > 1.0f;
    return res;
}

// ----------------------------------------------------------------------------- background thread
BounceJob::BounceJob (const Project& live, const BounceSettings& s, std::function<void (BounceResult)> done)
    : juce::Thread ("Bounce out"), onDone (std::move (done))
{
    bouncer = std::make_unique<Bouncer> (live, s);
    if (bouncer->getPrepareError().isNotEmpty()) earlyError = bouncer->getPrepareError();
    startThread (juce::Thread::Priority::normal);
}

BounceJob::~BounceJob()
{
    cancelFlag = true;
    stopThread (10000);
}

void BounceJob::run()
{
    auto result = bouncer->render (&progress, &cancelFlag);
    auto fn = onDone;
    juce::MessageManager::callAsync ([fn, result] { if (fn) fn (result); });
}
} // namespace td

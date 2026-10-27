#include "Bounce.h"
#include "Playback.h"
#include "Dither.h"
#include "AudioOps.h"
#include <set>
#include "AudioFormats.h"

namespace td
{
Bouncer::Bouncer (const Project& live, const BounceSettings& s) : settings (s)
{
    bool haveMixer = false;
    for (auto& m : live.mixers) haveMixer = haveMixer || m->id == s.mixerId;
    if (! haveMixer) { prepareError = "The mixer to bounce through no longer exists."; return; }
    if (s.sources.empty() && s.sourcesPre.empty()) { prepareError = "Choose at least one output to bounce (an Ext bus, an Int bus or an audio track)."; return; }
    if (s.items.empty()) { prepareError = "Nothing to bounce."; return; }
    shadow = live.cloneForRender (s.sampleRate, kBlock);
    // A plug-in that would not start in the render copy used to be left out of the bounce without a word, so the file did not sound like the mix.
    // Now the bounce is refused and says which (the log says why it would not start).
    if (const auto missing = live.pluginsMissingFrom (*shadow, s.mixerId); ! missing.isEmpty())
    {
        prepareError = (missing.size() == 1 ? "A plug-in could not be started for the render" : juce::String (missing.size()) + " plug-ins could not be started for the render")
                       + ", so nothing was written (a bounce without " + (missing.size() == 1 ? "it" : "them") + " would not sound like the mix):\n\n   "
                       + missing.joinIntoString ("\n   ") + "\n\nBypass or remove " + (missing.size() == 1 ? "it" : "them") + " to bounce without; the log says why "
                       + (missing.size() == 1 ? "it" : "they") + " would not start.";
        return;
    }
    if (! s.items.empty()) shadow->alignActive = s.items.front().alignOwner.isNotEmpty() ? s.items.front().alignOwner : s.alignOwner;
    engine = std::make_unique<AudioEngine> (*shadow);
    engine->setOfflineTarget (s.mixerId, s.sources, s.sourcesPre);
    engine->prepare (s.sampleRate, kBlock);
}

Bouncer::~Bouncer() = default;

juce::File Bouncer::uniqueFile (const juce::String& name) const
{
    return settings.folder.getNonexistentChildFile (sanitiseForFile (name), ".wav", true);
}

juce::String Bouncer::fileName (const BounceItem& it, const juce::String& outputName, bool several) const
{
    return (several || settings.addOutputName || outputName.endsWith ("(before fader)")) ?it.name + " - " + outputName : it.name;
}

BounceResult Bouncer::render (std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    juce::ScopedNoDenormals noDenormals;                               // (as on the audio thread: filters dying away into silence must not slow the render)
    BounceResult res;
    if (prepareError.isNotEmpty()) { res.error = prepareError; return res; }
    if (! settings.folder.createDirectory()) { res.error = "Cannot create the folder " + settings.folder.getFullPathName(); return res; }

    juce::AudioFormatManager formats;
    td::registerAudioFormats (formats);
    // the files are opened as the render reaches them and closed once it has passed them (see PlaybackSession::renderSegments); a file that cannot be
    // read, or is at another sample rate, stops the render with its name
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::String readProblem;
    const auto opener = PlaybackSession::makeOpener (formats, PlaybackSession::bufferBytesFor (shadow->tracks.size() * 3), settings.sampleRate, &readProblem);
    auto openReaders = [&] (const std::vector<PlaySegment>& segs) -> bool
    {
        readers.clear(); readers.resize (segs.size()); readProblem.clear();
        std::set<juce::String> seen;
        for (auto& s : segs)
            if (seen.insert (s.file.getFullPathName()).second && ! s.file.existsAsFile()) { res.error = "Cannot read " + s.file.getFullPathName(); return false; }
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
    for (auto& id : settings.sourcesPre) outNames.push_back (shadow->nodeName (id) + " (before fader)");       // (captured after the post-fader ones, as the engine orders them)
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
            engine->setOfflineTarget (renderedMixer, settings.sources, settings.sourcesPre);
        }
        bool replan = autoPlan != nullptr && autoPlan->end();               // (the previous piece's automation lets go)
        autoPlan = planForEdit (it.automationEdit.isNull() ? settings.automationEdit : it.automationEdit, renderedMixer);
        if (autoPlan != nullptr && autoPlan->begin()) replan = true;         // sends this automation moves go into the plan (see SendState::autoHeld)
        if (const auto owner = it.alignOwner.isNotEmpty() ? it.alignOwner : settings.alignOwner; owner != shadow->alignActive)
        { shadow->alignActive = owner; replan = true; }                         // each piece with its own spot-mic alignment
        if (replan) engine->rebuildPlan();

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

        // delay compensation: plug-ins that hold the sound back make everything come out late by the same amount; render that much extra and throw the start away
        const juce::int64 skip = engine->offlineLatency();
        const juce::int64 renderTotal = total + skip;
        for (juce::int64 done = 0; done < renderTotal;)
        {
            if (cancel != nullptr && cancel->load()) { writers.clear(); cleanUp(); res.cancelled = true; return res; }
            const int n = (int) juce::jmin ((juce::int64) kBlock, renderTotal - done);
            PlaybackSession::renderSegments (readers, segs, first + done, n, trackBufs, scratch, &opener);
            if (readProblem.isNotEmpty()) { writers.clear(); cleanUp(); res.error = readProblem; return res; }
            PlaybackSession::releaseReadersBefore (readers, segs, first + done + n);
            // the padding is silence going in (so plug-ins settle and tails ring out); only the piece itself is fed in
            const auto pos0 = first + done;
            if (pos0 < it.start || pos0 + n > it.end)
                for (auto& tb : trackBufs)
                    for (int i = 0; i < n; ++i)
                        if (pos0 + i < it.start || pos0 + i >= it.end)
                            for (int c = 0; c < tb.getNumChannels(); ++c) tb.setSample (c, i, 0.0f);
            if (autoPlan != nullptr) autoPlan->apply (first + done);
            engine->renderOffline (trackBufs, n, captures);
            const int from = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, skip - done);          // the first 'skip' samples of the render are the lead-in of the delay
            if (from < n)
                for (int o = 0; o < numOut; ++o)
                {
                    auto& cap = captures[(size_t) o];
                    for (int c = 0; c < 2; ++c) { const auto mm = cap.findMinMax (c, from, n - from); peaks[(size_t) o] = juce::jmax (peaks[(size_t) o], std::abs (mm.getStart()), std::abs (mm.getEnd())); }
                    if (! douts[(size_t) o]->write (cap, from, n - from))          // (3.1.3: a refused write - the drive is full, or gone - stops the bounce and says so)
                    { writers.clear(); cleanUp(); res.error = "Could not write " + (settings.normalise ? tmps : finals)[(size_t) o].getFileName() + ": the drive did not take it (is it full?)"; return res; }
                }
            done += n; doneAll += n;
            if (progress != nullptr) progress->store ((float) juce::jmin (1.0, doneAll / totalAll));
        }
        writers.clear();
        for (int o = 0; o < numOut; ++o)                              // each file read back: it holds every sample that was written (3.1.3)
            if (const auto e = audioops::checkWrittenLength (&formats, settings.normalise ? tmps[(size_t) o] : finals[(size_t) o], total); e.isNotEmpty()) { cleanUp(); res.error = e; return res; }

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
                const auto frames = in->lengthInSamples;
                for (juce::int64 pos = 0; pos < frames; pos += 16384)
                {
                    const int n = (int) juce::jmin ((juce::int64) 16384, frames - pos);
                    in->read (&buf, 0, n, pos, true, true);
                    buf.applyGain (0, n, gain);
                    if (! dout->write (buf, 0, n)) { out.reset(); in.reset(); finalFile.deleteFile(); cleanUp(); res.error = "Could not write " + finalFile.getFileName() + ": the drive did not take it (is it full?)"; return res; }
                }
                out.reset(); in.reset();
                if (const auto e = audioops::checkWrittenLength (&formats, finalFile, frames); e.isNotEmpty()) { finalFile.deleteFile(); cleanUp(); res.error = e; return res; }
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
    if (autoPlan != nullptr && autoPlan->end()) engine->rebuildPlan();     // (the copy is left as it was found, should it be rendered again)
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

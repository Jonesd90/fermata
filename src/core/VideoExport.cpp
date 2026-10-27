#include "VideoExport.h"
#include "AudioOps.h"
#include "AudioFormats.h"
#include "Playback.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace td::videoexport
{
std::vector<GatedRun> gateLtcRuns (const std::vector<PieceEdge>& pieces, juce::int64 gapSamples)
{
    // the pieces that count: one whose fade-in is followed by the next fade-in within the gap is a very short edit and is left out, so the longer
    // runs on either side of it survive; done again until nothing changes (two short pieces in a row both go)
    std::vector<size_t> keep;
    for (size_t i = 0; i < pieces.size(); ++i) keep.push_back (i);
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t k = 0; k + 1 < keep.size(); ++k)
            if (pieces[keep[k + 1]].fadeInStart - pieces[keep[k]].fadeInStart < gapSamples) { keep.erase (keep.begin() + (long) k); changed = true; break; }
    }
    std::vector<GatedRun> runs;
    for (size_t k = 0; k < keep.size(); ++k)
    {
        const auto& p = pieces[keep[k]];
        GatedRun r; r.piece = keep[k]; r.from = p.fadeInStart;
        r.to = k + 1 < keep.size() ? juce::jmin (p.end, pieces[keep[k + 1]].fadeInStart - gapSamples) : p.end;
        if (r.to > r.from) runs.push_back (r);
    }
    return runs;
}

int formatCodeFor (double fps, bool drop)
{
    if (drop) return 2;
    if (std::abs (fps - 24.0) < 0.01) return 0;
    if (std::abs (fps - 30.0) < 0.01) return 4;
    if (std::abs (fps - 30000.0 / 1001.0) < 0.01) return 3;
    if (std::abs (fps - 24000.0 / 1001.0) < 0.01) return 5;
    return 1;
}

CodeAt timecodeAt (juce::AudioFormatManager& fm, const juce::File& file, int channel, juce::int64 fileSample)
{
    CodeAt c;
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { c.why = "the timecode file could not be read: " + file.getFileName(); return c; }
    if (channel < 0 || channel >= (int) reader->numChannels) channel = 0;
    const double rate = reader->sampleRate;
    // two seconds either side: the decoder needs a frame or two to find the sync word, and the frame rate is told from a run of frames
    const juce::int64 from = juce::jmax ((juce::int64) 0, fileSample - (juce::int64) (2.0 * rate));
    const juce::int64 to = juce::jmin (reader->lengthInSamples, fileSample + (juce::int64) (2.0 * rate));
    if (to <= from) { c.why = "the piece lies outside its timecode file"; return c; }
    std::vector<std::vector<float>> block;
    if (! audioops::readRange (*reader, from, to - from, block) || (int) block.size() <= channel) { c.why = "the timecode file could not be read: " + file.getFileName(); return c; }
    LtcDecoder dec (rate);
    dec.feed (block[(size_t) channel].data(), (int) block[(size_t) channel].size());
    const auto res = analyseLtc (dec.frames(), rate);
    if (! res.found || res.frameCount < 3) { c.why = "no timecode could be read there"; return c; }
    // the code at the window's first sample, carried to the sample asked for (within the window: a few seconds, so the box's own clock cannot drift away)
    c.found = true; c.fps = res.fps; c.drop = res.drop;
    c.seconds = res.startSeconds + (double) (fileSample - from) / rate;
    return c;
}

namespace
{
struct Run { juce::int64 from, to; double seconds; int code; };

/** Writes a stereo 24-bit WAV of 'length' samples, filled block by block by 'fill (buffer, start, count)' (two channels). */
juce::String writeStereo (juce::AudioFormatManager& fm, const juce::File& dest, double rate, juce::int64 length,
                          const std::function<bool (juce::AudioBuffer<float>&, juce::int64, int)>& fill, const std::atomic<bool>* cancel,
                          std::atomic<float>* progress, float p0, float p1)
{
    dest.deleteFile();
    auto w = audioops::makeWavWriter (dest, rate, 2, false);
    if (w == nullptr) return "Cannot create " + dest.getFileName();
    constexpr int block = 65536;
    juce::AudioBuffer<float> buf (2, block);
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        if (cancel != nullptr && cancel->load()) { w.reset(); dest.deleteFile(); return "Cancelled."; }
        const int n = (int) juce::jmin ((juce::int64) block, length - pos);
        buf.clear();
        if (! fill (buf, pos, n)) { w.reset(); dest.deleteFile(); return "Could not read the audio for " + dest.getFileName(); }
        if (! w->writeFromAudioSampleBuffer (buf, 0, n)) { w.reset(); dest.deleteFile(); return "Disk full or write error: " + dest.getFileName(); }
        if (progress != nullptr) progress->store (p0 + (p1 - p0) * (float) ((double) (pos + n) / (double) juce::jmax ((juce::int64) 1, length)));
    }
    w.reset();
    return audioops::checkWrittenLength (&fm, dest, length);           // (read back: every sample is in the file, 3.1.3)
}
} // namespace

Plan planFor (const Project& project, const EditDef& e, const Settings& st)
{
    Plan plan;
    if (e.isEmpty()) { plan.error = "The edit is empty."; return plan; }
    plan.rate = e.sampleRate;
    plan.start = e.audibleStart(); plan.end = e.audibleEnd();
    if (plan.end <= plan.start || plan.rate <= 0.0) { plan.error = "The edit has no audible length."; return plan; }
    plan.baseName = sanitiseForFile (st.name.isNotEmpty() ? st.name : e.name);
    std::vector<const EditRegion*> pieces;
    for (auto& r : e.regions) pieces.push_back (&r);
    if (st.includeOverdubs) for (auto& r : e.overdubs) pieces.push_back (&r);
    std::sort (pieces.begin(), pieces.end(), [] (const EditRegion* a, const EditRegion* b) { return a->fadeInBegin() < b->fadeInBegin(); });
    std::vector<juce::Uuid> ltcTracks;
    for (auto& t : project.tracks) if (t.isLtc) ltcTracks.push_back (t.id);
    plan.noLtcTrack = ltcTracks.empty();
    for (auto* r : pieces)
    {
        Plan::Piece p;
        p.edge = { r->fadeInBegin(), juce::jmax (r->fadeOutFinish(), r->endSample()) };
        p.takeName = r->takeName; p.atSeconds = (double) (r->startSample - plan.start) / plan.rate;
        for (auto& f : r->files)
            if (std::find (ltcTracks.begin(), ltcTracks.end(), f.trackId) != ltcTracks.end())
            {
                p.ltcFile = f.file;
                p.ltcSample = r->srcIn + r->samples (r->inStart) - f.fileStart;     // the take's sample that plays as the fade-in begins
                break;
            }
        plan.pieces.push_back (p);
    }
    return plan;
}

BounceSettings mixSettingsFor (const Project& project, const EditDef& e, const Plan& plan, const Settings& st)
{
    BounceSettings bs;
    bs.sampleRate = plan.rate;
    bs.segments = segmentsForEdit (project, e);
    if (e.hasAutomation()) bs.automationEdit = e.id;
    bs.alignOwner = project.alignOwnerFor (true, e.id);
    const MixerState* mixer = project.mixerOfEdit (e.id);
    if (mixer == nullptr && ! project.mixers.empty()) mixer = project.mixers.front().get();
    if (mixer != nullptr)
    {
        bs.mixerId = mixer->id;
        juce::Uuid main = juce::Uuid::null(), firstExt = juce::Uuid::null();     // (a juce::Uuid made without a value is a new random one, never null)
        for (auto& b : project.buses)
            if (b.external)
            {
                if (firstExt.isNull()) firstExt = b.id;
                for (auto& bs2 : mixer->busPool) if (bs2->busId == b.id && bs2->isMain.get() && main.isNull()) main = b.id;
            }
        if (! main.isNull()) bs.sources.push_back (main); else if (! firstExt.isNull()) bs.sources.push_back (firstExt);
    }
    bs.folder = st.folder; bs.addOutputName = false; bs.leadSeconds = bs.tailSeconds = 0.0; bs.normalise = false;
    BounceItem item; item.name = plan.baseName; item.start = plan.start; item.end = plan.end;
    bs.items.push_back (item);
    return bs;
}

Result exportForVideo (const Plan& plan, Bouncer& bouncer, const Settings& st, juce::AudioFormatManager& fm, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    Result out;
    if (plan.error.isNotEmpty()) { out.error = plan.error; return out; }
    const double rate = plan.rate;
    const juce::int64 start = plan.start, end = plan.end;
    out.lengthSamples = end - start;
    if (! st.folder.createDirectory()) { out.error = "Cannot create the folder " + st.folder.getFullPathName(); return out; }
    out.pieces = (int) plan.pieces.size();
    if (plan.noLtcTrack) out.notes.add ("The project has no LTC track (Project Designer: tick the track that records the timecode), so the LTC file is silent.");

    // ---- each piece's timecode at the moment its fade-in begins
    std::vector<PieceEdge> edges;
    std::vector<CodeAt> codes;
    for (size_t i = 0; i < plan.pieces.size(); ++i)
    {
        const auto& p = plan.pieces[i];
        if (cancel != nullptr && cancel->load()) { out.error = "Cancelled."; return out; }
        edges.push_back (p.edge);
        CodeAt c;
        if (p.ltcFile == juce::File()) c.why = plan.noLtcTrack ? juce::String ("no LTC track") : juce::String ("this take has no file on the LTC track");
        else c = timecodeAt (fm, p.ltcFile, 0, p.ltcSample);
        if (! c.found) { ++out.withoutCode; out.notes.add (p.takeName + " (at " + juce::String (p.atSeconds, 2) + " s): " + c.why + " - no code for this piece."); }
        codes.push_back (c);
        if (progress != nullptr) progress->store (0.1f * (float) (i + 1) / (float) juce::jmax ((size_t) 1, plan.pieces.size()));
    }
    {   // the frame rate: the one most pieces have (a piece at another rate is noted; its code is made at its own rate all the same)
        std::map<int, int> count;
        for (auto& c : codes) if (c.found) ++count[formatCodeFor (c.fps, c.drop)];
        int best = -1, bestN = 0;
        for (auto& kv : count) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
        if (best >= 0) { double fps; int nominal; bool drop; LtcGenerator::formatInfo (best, fps, nominal, drop); out.fps = fps; out.drop = drop; }
        if (count.size() > 1) out.notes.add ("The takes carry timecode at more than one frame rate: each piece's code is written at its own.");
    }
    // ---- the runs of code (the person's rule), clipped to the export
    const auto gapSamples = (juce::int64) std::llround (st.gapSeconds * rate);
    const auto gated = gateLtcRuns (edges, gapSamples);
    {
        std::vector<bool> kept (plan.pieces.size(), false);
        for (auto& g : gated) kept[g.piece] = true;
        for (size_t i = 0; i < plan.pieces.size(); ++i)
            if (! kept[i]) { ++out.dropped; out.notes.add (plan.pieces[i].takeName + " (at " + juce::String (plan.pieces[i].atSeconds, 2) + " s) is shorter than the gap before the next join: left out of the LTC file, as agreed."); }
    }
    std::vector<Run> runs;
    for (auto& g : gated)
    {
        const auto& c = codes[g.piece];
        if (! c.found) continue;                                        // (silence there: said in the notes)
        Run r; r.from = juce::jmax (g.from, start) - start; r.to = juce::jmin (g.to, end) - start;
        if (r.to <= r.from) continue;
        r.seconds = c.seconds + (double) (juce::jmax (g.from, start) - g.from) / rate;      // (a fade-in that begins before the export's first sample: the code at that first sample)
        r.code = formatCodeFor (c.fps, c.drop);
        runs.push_back (r);
    }
    out.runs = (int) runs.size();

    // ---- 1. the mix, as Bounce makes it
    if (bouncer.getPrepareError().isNotEmpty()) { out.error = bouncer.getPrepareError(); return out; }
    std::atomic<float> bounceProgress { 0.0f };
    auto res = bouncer.render (&bounceProgress, cancel);
    if (res.cancelled) { out.error = "Cancelled."; return out; }
    if (res.error.isNotEmpty() || res.files.isEmpty()) { out.error = res.error.isNotEmpty() ? res.error : juce::String ("The mix could not be bounced."); return out; }
    out.mixFile = juce::File (res.files[0]);
    if (progress != nullptr) progress->store (0.6f);
    {   // the mix must be the export's length to the sample (Bounce writes exactly the item's span)
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out.mixFile));
        if (rd == nullptr) { out.error = "The mix file could not be read back: " + out.mixFile.getFileName(); return out; }
        if (rd->lengthInSamples != out.lengthSamples) { out.error = "The mix came out " + juce::String (rd->lengthInSamples) + " samples long, not " + juce::String (out.lengthSamples) + "."; return out; }
    }

    // ---- 2. the timecode, no fades: the runs made again as clean code, silence between them
    out.ltcFile = out.mixFile.getSiblingFile (out.mixFile.getFileNameWithoutExtension() + " - LTC.wav");
    {
        LtcGenerator gen; gen.prepare (rate);
        size_t ri = 0; bool inRun = false;
        auto fillLtc = [&] (juce::AudioBuffer<float>& buf, juce::int64 b0, int n) -> bool
        {
            const juce::int64 b1 = b0 + n;
            juce::int64 pos = b0;
            while (pos < b1 && ri < runs.size())
            {
                if (! inRun)
                {
                    if (runs[ri].from >= b1) break;
                    pos = juce::jmax (pos, runs[ri].from);
                    gen.startExact (runs[ri].code, runs[ri].seconds);
                    inRun = true;
                }
                const int count = (int) (juce::jmin (b1, runs[ri].to) - pos);
                if (count > 0) gen.fill (buf.getWritePointer (0) + (pos - b0), count);
                pos += juce::jmax (0, count);
                if (pos >= runs[ri].to) { inRun = false; ++ri; }
            }
            buf.copyFrom (1, 0, buf, 0, 0, n);                                     // the same code on both channels
            return true;
        };
        if (const auto err = writeStereo (fm, out.ltcFile, rate, out.lengthSamples, fillLtc, cancel, progress, 0.6f, 0.8f); err.isNotEmpty()) { out.error = err; return out; }
    }

    // ---- 3. the check file: the mix summed to one channel on the left, the code on the right
    out.checkFile = out.mixFile.getSiblingFile (out.mixFile.getFileNameWithoutExtension() + " - check.wav");
    {
        std::unique_ptr<juce::AudioFormatReader> mix (fm.createReaderFor (out.mixFile)), ltc (fm.createReaderFor (out.ltcFile));
        if (mix == nullptr || ltc == nullptr) { out.error = "The files could not be read back to make the check file."; return out; }
        juce::AudioBuffer<float> a (2, 65536), b (2, 65536);
        auto fillCheck = [&] (juce::AudioBuffer<float>& buf, juce::int64 b0, int n) -> bool
        {
            if (! mix->read (&a, 0, n, b0, true, true) || ! ltc->read (&b, 0, n, b0, true, true)) return false;
            auto* l = buf.getWritePointer (0); auto* r = buf.getWritePointer (1);
            const float* ml = a.getReadPointer (0); const float* mr = a.getReadPointer (mix->numChannels > 1 ? 1 : 0); const float* code = b.getReadPointer (0);
            for (int i = 0; i < n; ++i) { l[i] = 0.5f * (ml[i] + mr[i]); r[i] = code[i]; }
            return true;
        };
        if (const auto err = writeStereo (fm, out.checkFile, rate, out.lengthSamples, fillCheck, cancel, progress, 0.8f, 1.0f); err.isNotEmpty()) { out.error = err; return out; }
    }
    if (progress != nullptr) progress->store (1.0f);
    return out;
}
} // namespace td::videoexport

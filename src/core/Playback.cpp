#include "Playback.h"

namespace td
{
PlaybackSession::PlaybackSession (std::vector<int> counts, std::vector<PlaySegment> segs, juce::int64 startPosition,
                                  juce::int64 endPosition, double sr, int block)
    : juce::Thread ("Playback reader"), channelCounts (std::move (counts)), segments (std::move (segs)),
      startPos (startPosition), endPos (endPosition), renderPos (startPosition), sampleRate (sr), maxBlock (juce::jmax (16, block))
{
    formats.registerBasicFormats();
    const int maxCh = kMaxTrackChannels;
    scratch.setSize (maxCh, 16384);
    for (int c : channelCounts)
    {
        const int ch = juce::jlimit (1, kMaxTrackChannels, c);
        ring.emplace_back (ch, kRing);
        chunk.emplace_back (ch, 8192);
        trackBuf.emplace_back (ch, maxBlock);
        ring.back().clear(); chunk.back().clear(); trackBuf.back().clear();
    }
    position = 0;
}

PlaybackSession::~PlaybackSession()
{
    stopThread (3000);
}

juce::String PlaybackSession::open()
{
    readers.clear();
    for (auto& s : segments)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (s.file));
        if (r == nullptr) return "Cannot read " + s.file.getFullPathName();
        if (std::abs (r->sampleRate - sampleRate) > 0.5)
            return s.file.getFileName() + " is " + juce::String (r->sampleRate, 0) + " Hz but the audio device is running at " + juce::String (sampleRate, 0) + " Hz";
        readers.push_back (std::move (r));
    }
    return {};
}

void PlaybackSession::start()
{
    startThread (juce::Thread::Priority::high);
    const auto deadline = juce::Time::getMillisecondCounter() + 3000;
    const int preload = looping.load() ? 16384 : kRing / 2;                // a loop keeps less in hand (see run()), so it needs less to start
    while (! renderDone.load() && fifo.getNumReady() < preload && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (5);
}

void PlaybackSession::run()
{
    while (! threadShouldExit())
    {
        if (renderPos >= endPos)
        {
            if (looping.load() && endPos > startPos) renderPos = startPos;            // loop: carry on from the start, without a gap
            else { renderDone = true; return; }
        }
        const int space = fifo.getFreeSpace();
        if (space < 4096) { wait (5); continue; }
        if (looping.load() && fifo.getNumReady() >= 32768) { wait (5); continue; }      // a loop is only read ~0.7 s ahead, so switching it off takes effect quickly
        const int n = (int) juce::jmin ((juce::int64) juce::jmin (8192, space), endPos - renderPos);

        renderSegments (readers, segments, renderPos, n, chunk, scratch);

        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        for (size_t t = 0; t < ring.size(); ++t)
            for (int c = 0; c < ring[t].getNumChannels(); ++c)
            {
                if (z1 > 0) ring[t].copyFrom (c, s1, chunk[t], c, 0, z1);
                if (z2 > 0) ring[t].copyFrom (c, s2, chunk[t], c, z1, z2);
            }
        fifo.finishedWrite (z1 + z2);
        renderPos += z1 + z2;
    }
}

void PlaybackSession::pull (int n) noexcept
{
    n = juce::jmin (n, maxBlock);
    int s1, z1, s2, z2;
    fifo.prepareToRead (n, s1, z1, s2, z2);
    const int got = z1 + z2;
    for (size_t t = 0; t < ring.size(); ++t)
    {
        auto& dst = trackBuf[t];
        for (int c = 0; c < dst.getNumChannels(); ++c)
        {
            if (z1 > 0) dst.copyFrom (c, 0, ring[t], c, s1, z1);
            if (z2 > 0) dst.copyFrom (c, z1, ring[t], c, s2, z2);
            if (got < n) dst.clear (c, got, n - got);
        }
    }
    fifo.finishedRead (got);
    position.fetch_add (got);
    if (got < n)
    {
        if (renderDone.load()) { if (fifo.getNumReady() == 0) finished = true; }
        else underruns.fetch_add (1);
    }
    else if (renderDone.load() && fifo.getNumReady() == 0)
        finished = true;
}

void PlaybackSession::renderSegments (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers, const std::vector<PlaySegment>& segs,
                                      juce::int64 from, int n, std::vector<juce::AudioBuffer<float>>& out, juce::AudioBuffer<float>& scratch)
{
    for (auto& b : out) { if (n > b.getNumSamples()) return; b.clear (0, n); }
    const juce::int64 to = from + n;
    for (size_t i = 0; i < segs.size(); ++i)
    {
        const auto& s = segs[i];
        if (s.trackIndex < 0 || s.trackIndex >= (int) out.size() || i >= readers.size()) continue;
        const juce::int64 a = juce::jmax (from, s.begin), b = juce::jmin (to, s.end);
        if (b <= a) continue;
        auto* reader = readers[i].get();
        auto& dst = out[(size_t) s.trackIndex];
        const int m = (int) (b - a);
        const int ch = juce::jmin ((int) reader->numChannels, dst.getNumChannels(), scratch.getNumChannels());
        if (ch <= 0 || m > scratch.getNumSamples()) continue;

        // read the file region, zero-filling anything outside the file
        const juce::int64 srcStart = a + s.srcOffset;
        scratch.clear (0, m);
        const juce::int64 fileLen = reader->lengthInSamples;
        const juce::int64 rs = juce::jmax ((juce::int64) 0, srcStart), re = juce::jmin (fileLen, srcStart + m);
        if (re > rs)
        {
            const int dstOff = (int) (rs - srcStart), cnt = (int) (re - rs);
            float* ptrs[kMaxTrackChannels];
            for (int c = 0; c < ch; ++c) ptrs[c] = scratch.getWritePointer (c) + dstOff;
            reader->read (ptrs, ch, rs, cnt);
        }

        // fades and volume changes
        const bool hasIn = s.fadeInLen > 0, hasOut = s.fadeOutLen > 0, hasSteps = ! s.gainSteps.empty();
        if (hasIn || hasOut || hasSteps)
            for (int k = 0; k < m; ++k)
            {
                const juce::int64 t = a + k;
                float g = 1.0f;
                if (hasIn)  g *= t < s.fadeInStart ? 0.0f : t >= s.fadeInStart + s.fadeInLen ? 1.0f
                                 : fadeGain (s.inCurve, (float) (t - s.fadeInStart) / (float) s.fadeInLen, true);
                if (hasOut) g *= t < s.fadeOutStart ? 1.0f : t >= s.fadeOutStart + s.fadeOutLen ? 0.0f
                                 : fadeGain (s.outCurve, (float) (t - s.fadeOutStart) / (float) s.fadeOutLen, false);
                if (hasSteps) g *= s.gainAt (t);
                for (int c = 0; c < ch; ++c) scratch.getWritePointer (c)[k] *= g;
            }

        for (int c = 0; c < ch; ++c)
            juce::FloatVectorOperations::add (dst.getWritePointer (c) + (a - from), scratch.getReadPointer (c), m);
    }
}

// ----------------------------------------------------------------------------- builders
static int trackIndexOf (const Project& p, const juce::Uuid& id)
{
    for (size_t i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].id == id) return (int) i;
    return -1;
}

std::vector<PlaySegment> segmentsForTake (const Project& p, const TakeGroup& g, juce::int64 shift)
{
    std::vector<PlaySegment> v;
    for (auto& f : g.files)
    {
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file; s.srcOffset = -shift;
        s.begin = shift; s.end = shift + g.lengthSamples;
        const auto inLen  = (juce::int64) std::llround (g.fadeInSeconds * g.sampleRate);
        const auto outLen = (juce::int64) std::llround (g.fadeOutSeconds * g.sampleRate);
        if (inLen > 0)  { s.fadeInStart = s.begin; s.fadeInLen = juce::jmin (inLen, g.lengthSamples / 2); }
        if (outLen > 0) { s.fadeOutLen = juce::jmin (outLen, g.lengthSamples / 2); s.fadeOutStart = s.end - s.fadeOutLen; }
        v.push_back (s);
    }
    return v;
}

static void addRegionSegments (const Project& p, const EditRegion& r, std::vector<PlaySegment>& v)
{
    for (size_t fi = 0; fi < r.files.size(); ++fi)
    {
        auto& f = r.files[fi];
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file;
        s.srcOffset = r.srcIn - r.startSample - f.fileStart;
        s.begin = r.fadeInBegin();
        s.end   = r.fadeOutFinish();
        if (r.fadeInFinish() > r.fadeInBegin())
            { s.fadeInStart = r.fadeInBegin(); s.fadeInLen = r.fadeInFinish() - r.fadeInBegin(); s.inCurve = r.curve; }
        if (r.fadeOutFinish() > r.fadeOutBegin())
            { s.fadeOutStart = r.fadeOutBegin(); s.fadeOutLen = r.fadeOutFinish() - r.fadeOutBegin(); s.outCurve = r.curve; }
        for (size_t k = 0; k < r.gains.size(); ++k)                                  // this file's volume changes, with the level each one starts from
        {
            const auto& g = r.gains[k];
            GainStep st;
            st.at = r.startSample + g.at;
            st.ramp = (juce::int64) std::llround (g.ramp * r.sampleRate);
            st.from = r.levelAfter (fi, k, g.at);
            st.to = fi < g.db.size() ? dbToLinear (g.db[fi]) : 1.0f;
            s.gainSteps.push_back (st);
        }
        if (s.end > s.begin) v.push_back (s);
    }
}

std::shared_ptr<AutomationPlan> automationPlanFor (Project& p, const EditDef& e)
{
    if (! e.automationOn) return nullptr;
    auto plan = std::make_shared<AutomationPlan>();
    for (auto& lane : e.lanes)
    {
        if (lane.pts.empty()) continue;
        MixerState* m = nullptr;
        if (lane.mixerId.isNull()) { if (! p.mixers.empty()) m = p.mixers.front().get(); }
        else for (auto& mm : p.mixers) if (mm->id == lane.mixerId) m = mm.get();
        const auto* t = p.findTrack (lane.trackId);
        if (m == nullptr || t == nullptr) continue;
        auto* strip = m->stripFor (lane.trackId);
        AutomationPlan::Item it; it.lane = lane;
        if (lane.param == autoparam::fader) it.target = &strip->gainDb;
        else if (lane.param == autoparam::pan && t->channelCount() == 1) it.target = &strip->pan;
        else if (autoparam::isSend (lane.param))
        {
            if (auto* sd = strip->sends.find (autoparam::sendDest (lane.param))) it.target = &sd->gainDb;     // only sends that exist
        }
        else if (int slot = 0, idx = 0; autoparam::parsePlugin (lane.param, slot, idx) && slot < kNumSlots)
        {
            auto& sl = strip->slots[slot];
            if (auto* proc = sl.getProcessorUnsafe(); proc != nullptr && idx < proc->numParams() && proc->paramAutomatable (idx))
            { it.slot = &sl; it.paramIndex = idx; }
        }
        if (it.target != nullptr || it.slot != nullptr) plan->items.push_back (std::move (it));
    }
    return plan->items.empty() ? nullptr : plan;
}

std::vector<PlaySegment> segmentsForEdit (const Project& p, const EditDef& e)
{
    std::vector<PlaySegment> v;
    for (auto& r : e.regions)  addRegionSegments (p, r, v);
    for (auto& r : e.overdubs) addRegionSegments (p, r, v);      // overdubs are simply added: everything plays at the same time
    return v;
}

std::vector<PlaySegment> segmentsForRegionOriginal (const Project& p, const EditRegion& r)
{
    std::vector<PlaySegment> v;
    for (auto& f : r.files)
    {
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file; s.srcOffset = -f.fileStart;
        s.begin = f.fileStart; s.end = r.sourceLength > 0 ? r.sourceLength : r.srcOut + (juce::int64) (60.0 * r.sampleRate);
        v.push_back (s);
    }
    return v;
}
} // namespace td

#include "Playback.h"
#include "CoreReservation.h"

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
        ring.emplace_back ((size_t) ch, std::vector<float> ((size_t) kRing, 0.0f));
        chunk.emplace_back (ch, 8192);
        trackBuf.emplace_back (ch, maxBlock);
        chunk.back().clear(); trackBuf.back().clear();
    }
    position = 0;
}

PlaybackSession::~PlaybackSession()
{
    CoreReservation::get().unregisterAudioThread (getThreadId());
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
    CoreReservation::get().registerAudioThread (getThreadId());          // (kept on the reserved cores, when there are any)
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
            for (size_t c = 0; c < ring[t].size(); ++c)
            {
                if (z1 > 0) juce::FloatVectorOperations::copy (ring[t][c].data() + s1, chunk[t].getReadPointer ((int) c), z1);
                if (z2 > 0) juce::FloatVectorOperations::copy (ring[t][c].data() + s2, chunk[t].getReadPointer ((int) c) + z1, z2);
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
            const float* src = ring[t][(size_t) c].data();
            if (z1 > 0) dst.copyFrom (c, 0, src + s1, z1);
            if (z2 > 0) dst.copyFrom (c, z1, src + s2, z2);
            if (got < n) dst.clear (c, got, n - got);
        }
    }
    fifo.finishedRead (got);
    const auto now = position.fetch_add (got) + got;
    clockSeq.fetch_add (1, std::memory_order_acq_rel);                    // (odd: being written)
    clockPos.store (now, std::memory_order_relaxed);
    clockMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    clockBlock.store (got, std::memory_order_relaxed);
    clockSeq.fetch_add (1, std::memory_order_release);
    if (got < n)
    {
        if (renderDone.load()) { if (fifo.getNumReady() == 0) finished = true; }
        else underruns.fetch_add (1);
    }
    else if (renderDone.load() && fifo.getNumReady() == 0)
        finished = true;
}

double PlaybackSession::getSmoothPositionSeconds() const noexcept
{
    if (sampleRate <= 0.0) return 0.0;
    for (int tries = 0; tries < 16; ++tries)
    {
        const auto s0 = clockSeq.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0) continue;
        const auto pos = clockPos.load (std::memory_order_relaxed);
        const double ms = clockMs.load (std::memory_order_relaxed);
        const int block = clockBlock.load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (clockSeq.load (std::memory_order_relaxed) != s0) continue;
        if (ms <= 0.0 || finished.load()) return (double) pos / sampleRate;
        const double ahead = juce::jlimit (0.0, (double) block, (juce::Time::getMillisecondCounterHiRes() - ms) * 0.001 * sampleRate);
        return ((double) pos + ahead) / sampleRate;
    }
    return getPositionSeconds();
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

/** LTC (timecode) is never faded or crossfaded: a piece's LTC starts hard at the first sample of the piece's fade-in and stops hard at the end of its fade-out - or, when the next
    piece begins first (a crossfade), one sample before that piece's fade-in begins, so the pieces of LTC never overlap and the next one starts the moment its piece appears. */
juce::int64 ltcEndOf (const EditRegion& r, const EditRegion* next) noexcept
{
    juce::int64 e = r.fadeOutFinish();
    if (next != nullptr && next->fadeInBegin() > r.fadeInBegin()) e = juce::jmin (e, next->fadeInBegin());
    return e;                                                         // (exclusive: the last LTC sample is e - 1)
}

static void addRegionSegments (const Project& p, const EditRegion& r, std::vector<PlaySegment>& v, const EditRegion* next = nullptr, bool overdub = false)
{
    for (size_t fi = 0; fi < r.files.size(); ++fi)
    {
        auto& f = r.files[fi];
        const int ti = trackIndexOf (p, f.trackId);
        if (ti < 0) continue;
        if (p.tracks[(size_t) ti].isLtc)
        {
            if (overdub) continue;                                    // overdubs carry no timecode
            PlaySegment l;
            l.trackIndex = ti; l.file = f.file;
            l.srcOffset = r.srcIn - r.startSample - f.fileStart;
            l.begin = r.fadeInBegin(); l.end = ltcEndOf (r, next);
            if (l.end > l.begin) v.push_back (l);
            continue;
        }
        PlaySegment s;
        s.trackIndex = ti; s.file = f.file;
        s.srcOffset = r.srcIn - r.startSample - f.fileStart;
        s.begin = r.fadeInBegin();
        s.end   = r.fadeOutFinish();
        if (r.fadeInFinish() > r.fadeInBegin())
            { s.fadeInStart = r.fadeInBegin(); s.fadeInLen = r.fadeInFinish() - r.fadeInBegin(); s.inCurve = r.curve; }
        if (r.fadeOutFinish() > r.fadeOutBegin())
            { s.fadeOutStart = r.fadeOutBegin(); s.fadeOutLen = r.fadeOutFinish() - r.fadeOutBegin(); s.outCurve = r.outCurveValue(); }
        for (size_t k = 0; k < r.gains.size(); ++k)                                  // this file's volume changes, with the level each one starts from
        {
            const auto& g = r.gains[k];
            GainStep st;
            st.at = r.startSample + g.at;
            st.ramp = (juce::int64) std::llround (g.ramp * r.sampleRate);
            st.from = r.levelAfter (fi, k, g.at);
            st.to = fi < g.db.size() ? dbToLinear (g.db[fi]) : 1.0f;
            if (k == 0 && ! g.autoNorm && g.at <= (juce::int64) (0.001 * r.sampleRate))
            {
                // the level given to the piece as a whole (the trim window's Level faders): it holds from the very first sound of the piece, the fade-in included,
                // so the fade only reaches that level, instead of rising to 0 dB and then dropping to it
                st.at = juce::jmin (st.at, s.begin); st.ramp = 0; st.from = st.to;
            }
            s.gainSteps.push_back (st);
        }
        if (s.end > s.begin) v.push_back (s);
    }
}

std::shared_ptr<AutomationPlan> automationPlanFor (Project& p, const EditDef& e, bool onProcessingMixer, const juce::Uuid& unassignedMixer)
{
    if (! e.automationOn) return nullptr;
    auto plan = std::make_shared<AutomationPlan>();
    for (auto& lane : e.lanes)
    {
        if (lane.pts.empty()) continue;
        MixerState* m = nullptr;
        if (lane.mixerId.isNull())                      // the default is the Edit's own mixer (or, in a render, the mixer that is being rendered)
        {
            if (! unassignedMixer.isNull()) for (auto& mm : p.mixers) if (mm->id == unassignedMixer) m = mm.get();
            if (m == nullptr) m = onProcessingMixer ? nullptr : p.mixerOfEdit (e.id);
            if (m == nullptr && ! p.mixers.empty()) m = p.mixers.front().get();
        }
        else for (auto& mm : p.mixers) if (mm->id == lane.mixerId) m = mm.get();
        if (const auto* vd = p.findVca (lane.trackId); vd != nullptr && m != nullptr)      // a VCA's fader lane (the only automatable thing of a VCA): trackId holds the VCA's id
        {
            if (lane.param != autoparam::fader) continue;
            AutomationPlan::Item vi; vi.lane = lane; vi.target = &m->vcas[juce::jlimit (0, kMaxVcas - 1, vd->number - 1)].gainDb;
            plan->items.push_back (std::move (vi));
            continue;
        }
        if (const auto* bd = p.findBus (lane.trackId); bd != nullptr && ! bd->external && m != nullptr)        // an Int Bus has the same lanes as a track: gain, fader, pan, sends, plug-ins
        {
            auto* bs = m->busFor (lane.trackId);
            AutomationPlan::Item bi; bi.lane = lane;
            if (lane.param == autoparam::fader) bi.target = &bs->gainDb;
            else if (lane.param == autoparam::gain) bi.target = &bs->inGainDb;
            else if (lane.param == autoparam::pan) bi.target = &bs->pan;
            else if (autoparam::isSend (lane.param)) { if (auto* sd = bs->sends.find (autoparam::sendDest (lane.param))) bi.target = &sd->gainDb; }
            else if (int slot = 0, idx = 0; autoparam::parsePlugin (lane.param, slot, idx) && slot < kNumSlots)
            {
                auto& sl = bs->slots[slot];
                if (auto* proc = sl.getProcessorUnsafe(); proc != nullptr && idx < proc->numParams() && proc->paramAutomatable (idx)) { bi.slot = &sl; bi.paramIndex = idx; }
            }
            if (bi.target != nullptr || bi.slot != nullptr) plan->items.push_back (std::move (bi));
            continue;
        }
        const auto* t = p.findTrack (lane.trackId);
        if (m == nullptr || t == nullptr) continue;
        auto* strip = m->stripFor (lane.trackId);
        AutomationPlan::Item it; it.lane = lane;
        if (lane.param == autoparam::fader) it.target = &strip->gainDb;
        else if (lane.param == autoparam::gain) it.target = &strip->inGainDb;
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
    for (size_t i = 0; i < e.regions.size(); ++i) addRegionSegments (p, e.regions[i], v, i + 1 < e.regions.size() ? &e.regions[i + 1] : nullptr);
    for (auto& r : e.overdubs) addRegionSegments (p, r, v, nullptr, true);      // overdubs are simply added: everything plays at the same time
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

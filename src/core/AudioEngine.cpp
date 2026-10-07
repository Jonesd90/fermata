#include "AudioEngine.h"
#include <algorithm>
#include <cstring>

namespace td
{
AudioEngine::AudioEngine (Project& p) : project (p)
{
    for (auto& x : inputPeak) x.store (0.0f);
    for (auto& x : outputPeak) x.store (0.0f);
    writerThread.startThread (juce::Thread::Priority::high);
}

AudioEngine::~AudioEngine()
{
    if (isRecording()) stopRecording();
    stopPlayback();
    running = false;
    delete plan.exchange (nullptr);
    delete preRollPtr.exchange (nullptr);
    writerThread.stopThread (3000);
}

// ------------------------------------------------------------------ device
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    deviceInputs  = d->getActiveInputChannels().countNumberOfSetBits();
    deviceOutputs = d->getActiveOutputChannels().countNumberOfSetBits();
    juce::Logger::writeToLog ("Engine: device starting - " + d->getTypeName() + " / " + d->getName() + ", "
                              + juce::String (d->getCurrentSampleRate()) + " Hz, buffer " + juce::String (d->getCurrentBufferSizeSamples())
                              + ", in " + juce::String (deviceInputs) + ", out " + juce::String (deviceOutputs));
    prepare (d->getCurrentSampleRate(), juce::jmax (64, d->getCurrentBufferSizeSamples()));
    running = true;
    juce::Logger::writeToLog ("Engine: device started OK");
}

void AudioEngine::audioDeviceStopped()
{
    juce::Logger::writeToLog ("Engine: device stopped");
    running = false;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    process (in, numIn, out, numOut, numSamples);
}

void AudioEngine::prepare (double sr, int block)
{
    stopPlayback();
    sampleRate = sr;
    maxBlock = juce::jmax (16, block);
    zeros.assign ((size_t) juce::jmax (maxBlock * 4, 8192), 0.0f);
    tbTap.assign (zeros.size() * 2, 0.0f);   // also the silence for the pre-roll hand-over (4096 samples at a time)
    for (auto& m : project.mixers)                // plug-ins must know the new sample rate / block size
    {
        for (auto& st : m->stripPool) { for (auto& slot : st->slots) slot.reprepare (sampleRate, maxBlock, 2);
                                        for (auto& sd : st->sends.pool) for (auto& slot : sd->slots) slot.reprepare (sampleRate, maxBlock, 2); }
        for (auto& f : m->busPool)    { for (auto& slot : f->slots) slot.reprepare (sampleRate, maxBlock, 2);
                                        for (auto& sd : f->sends.pool) for (auto& slot : sd->slots) slot.reprepare (sampleRate, maxBlock, 2); }
    }
    rebuildPlan();
}

// ------------------------------------------------------------------ plan
std::unique_ptr<AudioEngine::Plan> AudioEngine::buildPlan() const
{
    auto p = std::make_unique<Plan>();
    p->disk.assign (project.tracks.size(), nullptr);
    for (auto& t : project.tracks)
    {
        TrackPlan tp;
        tp.numCh = t.channelCount();
        for (int c = 0; c < tp.numCh; ++c) tp.inputs[c] = t.inputOf (c);
        tp.live = t.live;
        p->tracks.push_back (tp);
    }
    for (auto& m : project.mixers)
    {
        auto mp = std::make_unique<MixerPlan>();
        mp->mixer = m.get();
        std::vector<std::unique_ptr<Node>> nodes;
        auto addNode = [&] (NodeKind k, StripState* st, BusState* bs, int ti)
        {
            auto nd = std::make_unique<Node>();
            nd->kind = k; nd->strip = st; nd->bus = bs; nd->trackIndex = ti;
            nodes.push_back (std::move (nd));
        };
        for (size_t i = 0; i < project.tracks.size(); ++i) addNode (NodeKind::Track, m->stripFor (project.tracks[i].id), nullptr, (int) i);
        for (auto& b : project.buses) if (! b.external) addNode (NodeKind::IntBus, nullptr, m->busFor (b.id), -1);
        for (auto& b : project.buses) if (b.external)   addNode (NodeKind::ExtBus, nullptr, m->busFor (b.id), -1);

        auto idOf = [&] (const Node& n) { return n.kind == NodeKind::Track ? n.strip->trackId : n.bus->busId; };
        auto indexOfId = [&] (const juce::Uuid& id) { for (size_t i = 0; i < nodes.size(); ++i) if (idOf (*nodes[i]) == id) return (int) i; return -1; };

        // the sends that are on, as edges: source -> destination
        struct Edge { int src, dst; SendState* st; };
        std::vector<Edge> edges;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            SendList* list = nodes[i]->kind == NodeKind::Track ? &nodes[i]->strip->sends : nodes[i]->kind == NodeKind::IntBus ? &nodes[i]->bus->sends : nullptr;
            if (list == nullptr) continue;
            for (auto& s : list->pool)
            {
                const int d = indexOfId (s->dest);
                if (d >= 0 && d != (int) i && s->active()) edges.push_back ({ (int) i, d, s.get() });
            }
        }
        // the outputs: every Audio Track / Int Bus whose output is on goes (after its fader, at unity) to its chosen bus, or to the mixer's main Ext Bus
        {
            juce::Uuid mainId = m->mainBus;
            if (indexOfId (mainId) < 0 || nodes[(size_t) indexOfId (mainId)]->kind != NodeKind::ExtBus)
            {
                mainId = {};
                for (auto& n : nodes) if (n->kind == NodeKind::ExtBus) { mainId = n->bus->busId; break; }
            }
            if (const int mi = indexOfId (mainId); mi >= 0 && nodes[(size_t) mi]->kind == NodeKind::ExtBus) nodes[(size_t) mi]->isMainOut = true;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                Flag* on = nullptr; const juce::Uuid* dest = nullptr; SendState* st = nullptr;
                if (nodes[i]->kind == NodeKind::Track)       { on = &nodes[i]->strip->outOn; dest = &nodes[i]->strip->outDest; st = &nodes[i]->strip->outSend; }
                else if (nodes[i]->kind == NodeKind::IntBus) { on = &nodes[i]->bus->outOn;   dest = &nodes[i]->bus->outDest;   st = &nodes[i]->bus->outSend; }
                if (on == nullptr || ! on->get()) continue;
                int d = indexOfId (dest->isNull() ? mainId : *dest);
                if (d < 0 && ! dest->isNull()) d = indexOfId (mainId);       // the chosen bus is gone: fall back to main
                if (d >= 0 && d != (int) i) edges.push_back ({ (int) i, d, st });
            }
        }
        // put the channels in an order where every channel follows the ones that feed it (ties keep the list order)
        std::vector<int> pos (nodes.size(), -1), indeg (nodes.size(), 0);
        for (auto& e : edges) ++indeg[(size_t) e.dst];
        std::vector<int> ordered;
        std::vector<bool> done (nodes.size(), false);
        for (size_t round = 0; round < nodes.size(); ++round)
        {
            int pick = -1;
            for (size_t i = 0; i < nodes.size() && pick < 0; ++i) if (! done[i] && indeg[i] == 0) pick = (int) i;
            if (pick < 0) { for (size_t i = 0; i < nodes.size() && pick < 0; ++i) if (! done[i]) pick = (int) i; }   // a loop (cannot normally happen): break it
            done[(size_t) pick] = true; pos[(size_t) pick] = (int) ordered.size(); ordered.push_back (pick);
            for (auto& e : edges) if (e.src == pick) --indeg[(size_t) e.dst];
        }
        for (auto& e : edges)
            if (pos[(size_t) e.dst] > pos[(size_t) e.src])
            {
                nodes[(size_t) e.src]->sends.push_back ({ e.st, nodes[(size_t) e.dst].get() });
                nodes[(size_t) e.dst]->receives = true;
            }
        for (int idx : ordered)
        {
            auto& nd = nodes[(size_t) idx];
            if (nd->receives) nd->incoming.setSize (2, maxBlock);
            mp->order.push_back (std::move (nd));
        }
        if (m->id == offlineMixerId && ! offlineMixerId.isNull())
        {
            int next = 0;
            std::vector<int> idxOf (offlineTaps.size(), -1);
            for (size_t i = 0; i < offlineTaps.size(); ++i) idxOf[i] = next++;
            for (auto& nd : mp->order)
            {
                const auto id = nd->kind == NodeKind::Track ? nd->strip->trackId : nd->bus->busId;
                const auto it = std::find (offlineTaps.begin(), offlineTaps.end(), id);
                nd->tapIdx = it != offlineTaps.end() ? idxOf[(size_t) (it - offlineTaps.begin())] : -1;
            }
        }
        mp->chanBuf.setSize (kMaxTrackChannels, maxBlock);
        mp->pre.setSize (2, maxBlock); mp->post.setSize (2, maxBlock); mp->preSend.setSize (2, maxBlock);
        mp->sendTmp.setSize (2, maxBlock);
        p->mixers.push_back (std::move (mp));
    }
    return p;
}

void AudioEngine::rebuildPlan()
{
    const juce::ScopedLock sl (planLock);
    // mixers' strip pools must be complete before the audio thread can see them
    project.syncMixers();
    auto fresh = buildPlan();
    auto* old = plan.exchange (fresh.release());
    waitForAudioThread();
    delete old;
    ensurePreRoll();                                 // a track patched to a new input starts being kept for the pre-roll
}

void AudioEngine::setSessionMode (bool on)
{
    sessionMode.store (on);
    const juce::ScopedLock sl (planLock);
    ensurePreRoll();
}

void AudioEngine::ensurePreRoll()
{
    const juce::ScopedLock sl (planLock);
    if (isRecording()) return;                       // the ring a recording is reading from must stay where it is
    auto* cur = preRollPtr.load();
    if (! sessionMode.load() || sampleRate <= 0.0)
    {
        if (cur != nullptr) { preRollPtr.store (nullptr); waitForAudioThread(); delete cur; }
        return;
    }
    std::vector<int> used;
    for (auto& t : project.tracks)
        for (int c = 0; c < t.channelCount(); ++c)
        {
            const int idx = t.inputOf (c);
            if (idx >= 0 && idx < kMaxInputs && std::find (used.begin(), used.end(), idx) == used.end()) used.push_back (idx);
        }
    std::sort (used.begin(), used.end());
    const int preLen = (int) std::lround (kPreRollSeconds * sampleRate);
    const int cap = preLen + (int) std::lround (2.0 * sampleRate);       // two seconds of room while the pre-roll is handed to the files
    if (cur != nullptr && cur->inputs == used && cur->cap == cap) return;
    auto* fresh = new PreRoll();
    fresh->cap = cap; fresh->preLen = preLen; fresh->inputs = used;
    fresh->slotOf.assign ((size_t) kMaxInputs, -1);
    for (size_t i = 0; i < used.size(); ++i) fresh->slotOf[(size_t) used[i]] = (int) i;
    fresh->data.assign (used.size() * (size_t) cap, 0.0f);
    auto* old = preRollPtr.exchange (fresh);
    waitForAudioThread();
    delete old;
}

/** Hands the samples [from, to) of the ring to every file and live waveform of the session, in order, at most 4096 at a time. */
void AudioEngine::deliverFromRing (RecordingSession& rs, juce::int64 from, juce::int64 to)
{
    auto* pr = rs.ring;
    if (pr == nullptr) return;
    const float* ptrs[kMaxTrackChannels];
    while (from < to)
    {
        const int o = (int) (from % pr->cap);
        const int len = (int) juce::jmin<juce::int64> (to - from, juce::jmin (4096, pr->cap - o));
        for (auto& rt : rs.tracks)
        {
            for (int c = 0; c < rt.route.numCh; ++c)
            {
                const int idx = rt.route.inputs[c];
                const int slot = (idx >= 0 && idx < (int) pr->slotOf.size()) ? pr->slotOf[(size_t) idx] : -1;
                ptrs[c] = slot >= 0 ? pr->data.data() + (size_t) slot * (size_t) pr->cap + o : zeros.data();
            }
            if (! rt.writer->write (ptrs, len)) rs.overruns.fetch_add (1);
            rt.peaks->add (ptrs, rt.route.numCh, len);
        }
        from += len;
    }
}

void AudioEngine::waitForAudioThread()
{
    if (! running.load()) return;
    const auto c0 = blockCounter.load();
    const auto deadline = juce::Time::getMillisecondCounter() + 500;
    while (blockCounter.load() < c0 + 2 && juce::Time::getMillisecondCounter() < deadline && running.load())
        juce::Thread::sleep (2);
}

void AudioEngine::setOfflineTarget (const juce::Uuid& mixerId, std::vector<juce::Uuid> tapIds)
{
    offlineMixerId = mixerId; offlineTaps = std::move (tapIds);
}

void AudioEngine::renderOffline (const std::vector<juce::AudioBuffer<float>>& trackAudio, int n, std::vector<juce::AudioBuffer<float>>& captures)
{
    auto* pl = plan.load (std::memory_order_acquire);
    for (auto& c : captures) c.clear();
    if (pl == nullptr || captures.empty()) return;
    n = juce::jmin (n, maxBlock, captures[0].getNumSamples());
    for (size_t i = 0; i < pl->disk.size(); ++i) pl->disk[i] = i < trackAudio.size() ? &trackAudio[i] : nullptr;
    captureBufs = &captures;
    for (auto& mp : pl->mixers)
        if (mp->mixer->id == offlineMixerId)
            processMixer (*mp, nullptr, 0, 0, n, nullptr, 0, true, false, -1);
    captureBufs = nullptr;
}

// ------------------------------------------------------------------ audio thread
void AudioEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
{
    auto* pl = plan.load (std::memory_order_acquire);
    auto* rs = rec.load (std::memory_order_acquire);

    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], numSamples);

    // input meters
    for (int c = 0; c < numIn && c < kMaxInputs; ++c)
    {
        if (in[c] == nullptr) continue;
        const auto mag = juce::FloatVectorOperations::findMinAndMax (in[c], numSamples);
        const float pk = juce::jmax (std::abs (mag.getStart()), std::abs (mag.getEnd()));
        if (pk > inputPeak[c].load (std::memory_order_relaxed)) inputPeak[c].store (pk, std::memory_order_relaxed);
    }

    // session mode: keep the last seconds of every patched input
    auto* pr = preRollPtr.load (std::memory_order_acquire);
    const juce::int64 wp = pr != nullptr ? pr->writePos.load (std::memory_order_relaxed) : 0;
    if (pr != nullptr && numSamples <= (int) zeros.size())
        for (size_t s = 0; s < pr->inputs.size(); ++s)
        {
            const int idx = pr->inputs[s];
            const float* src = (idx < numIn && in[idx] != nullptr) ? in[idx] : zeros.data();
            float* dst = pr->data.data() + s * (size_t) pr->cap;
            const int o = (int) (wp % pr->cap);
            const int first = juce::jmin (numSamples, pr->cap - o);
            std::memcpy (dst + o, src, sizeof (float) * (size_t) first);
            if (first < numSamples) std::memcpy (dst, src + first, sizeof (float) * (size_t) (numSamples - first));
        }

    // recording
    if (rs != nullptr && rs->useRing && pr != nullptr && pr == rs->ring)
    {
        // the files are fed from the ring: first the seconds before Record was pressed, then everything since, always in order
        const juce::int64 end = wp + numSamples;
        if (rs->startPos < 0) { rs->startPos = wp; rs->first = juce::jmax<juce::int64> (0, wp - pr->preLen); rs->delivered = rs->first; }
        const juce::int64 limit = juce::jmin (end, rs->delivered + numSamples + 4096);
        deliverFromRing (*rs, rs->delivered, limit);
        rs->delivered = limit;
        rs->livePos.store (end, std::memory_order_release);
        rs->samples.store (rs->delivered - rs->first);
    }
    else if (rs != nullptr)
    {
        const float* ptrs[kMaxTrackChannels];
        const float* silence = zeros.data();
        const bool silenceOk = numSamples <= (int) zeros.size();
        for (auto& rt : rs->tracks)
        {
            for (int c = 0; c < rt.route.numCh; ++c)
            {
                const int idx = rt.route.inputs[c];
                ptrs[c] = (idx >= 0 && idx < numIn && in[idx] != nullptr) ? in[idx] : silence;
            }
            if (silenceOk && ! rt.writer->write (ptrs, numSamples))
                rs->overruns.fetch_add (1);
            rt.peaks->add (ptrs, rt.route.numCh, numSamples);
        }
        rs->samples.fetch_add (numSamples);
    }
    if (pr != nullptr) pr->writePos.store (wp + numSamples, std::memory_order_release);

    // mixers (in chunks no larger than the scratch buffers)
    auto* ps = playback.load (std::memory_order_acquire);
    // Talkback playback: only while something is playing back. The processing mixer is then run with every live input OFF, so what it
    // produces can only be recorded audio, and its main output is copied aside for the TB pair.
    const bool tbPlayNow = ps != nullptr && tbPlay.load (std::memory_order_relaxed) && tbAny.load (std::memory_order_relaxed)
                           && (size_t) numSamples <= zeros.size() && tbTap.size() >= zeros.size() * 2;
    float* const tapL = tbTap.data();
    float* const tapR = tbTap.data() + zeros.size();
    if (tbPlayNow) { juce::FloatVectorOperations::clear (tapL, numSamples); juce::FloatVectorOperations::clear (tapR, numSamples); }
    if (pl != nullptr)
        for (int offset = 0; offset < numSamples; offset += maxBlock)
        {
            const int n = juce::jmin (maxBlock, numSamples - offset);
            if (ps != nullptr && ps->automation != nullptr) ps->automation->apply (ps->getPosition());     // an edit's automation moves the faders
            if (ps != nullptr) ps->pull (n);          // disk audio replaces the live inputs while playing
            for (size_t i = 0; i < pl->disk.size(); ++i) pl->disk[i] = (ps != nullptr && (int) i < ps->numTracks()) ? &ps->trackBuffer ((int) i) : nullptr;
            // engineer audition: mixer 0 is the engineer's. While a cue mixer is audited, the engineer's own Ext buses go quiet and
            // everything the cue mixer sends to its Ext buses is also heard on the first output pair of the engineer's mixer.
            const MixerState* aud = auditionMixer.load (std::memory_order_relaxed);
            const MixerState* eng = pl->mixers.empty() ? nullptr : pl->mixers.front()->mixer;
            bool audActive = false;
            int monitorOut = -1;
            if (aud != nullptr && aud != eng)
            {
                for (auto& mp : pl->mixers) audActive = audActive || mp->mixer == aud;
                for (auto& nd : pl->mixers.front()->order)
                    if (nd->kind == NodeKind::ExtBus && nd->bus->outFirst.load (std::memory_order_relaxed) >= 0)
                    { monitorOut = nd->bus->outFirst.load (std::memory_order_relaxed); break; }
                if (monitorOut < 0) audActive = false;          // the engineer has nowhere to hear it: leave everything as it is
            }
            for (auto& mp : pl->mixers)
            {
                const bool strict = tbPlayNow && mp.get() == pl->mixers.front().get();       // the processing mixer
                processMixer (*mp, in, numIn, offset, n, out, numOut, ps != nullptr,
                              audActive && mp->mixer == eng,
                              audActive && mp->mixer == aud ? monitorOut : -1,
                              strict, strict ? tapL : nullptr, strict ? tapR : nullptr);
            }
        }

    // Mono check: a listening aid. The mixer's driver outputs carry (L+R)/2 on both sides; dither (below) is added after it.
    if (const auto* mc = plan.load (std::memory_order_acquire))
    {
        const int nch = juce::jmin (numOut, kMaxInputs);
        for (auto& mp : mc->mixers)
        {
            if (! mp->mixer->monoCheck.load (std::memory_order_relaxed)) continue;
            for (auto& nd : mp->order)
            {
                if (nd->kind != NodeKind::ExtBus) continue;
                const int first = nd->bus->outFirst.load (std::memory_order_relaxed);
                if (first < 0 || first + 1 >= nch || out[first] == nullptr || out[first + 1] == nullptr) continue;
                float* L = out[first]; float* R = out[first + 1];
                for (int i = 0; i < numSamples; ++i) { const float m = 0.5f * (L[i] + R[i]); L[i] = m; R[i] = m; }
            }
        }
    }

    // Output dither. Each mixer can dither its driver outputs to 24 or 16 bit (TPDF, +-1 LSB) so the converter, or whatever is
    // fed after it, rounds noise instead of distortion. The offline render (bounce / mastering) never goes through here:
    // those files are dithered once, when they are written.
    if (const auto* dp = plan.load (std::memory_order_acquire))
    {
        const int nch = juce::jmin (numOut, kMaxInputs);
        for (int c = 0; c < nch; ++c) outDitherBits[c] = 0;
        bool any = false;
        for (auto& mp : dp->mixers)
        {
            const int bits = mp->mixer->ditherBits.load (std::memory_order_relaxed);
            if (bits != 16 && bits != 24) continue;
            for (auto& nd : mp->order)
            {
                if (nd->kind != NodeKind::ExtBus) continue;
                const int first = nd->bus->outFirst.load (std::memory_order_relaxed);
                for (int k = 0; k < 2; ++k)
                {
                    const int ch = first + k;
                    if (first < 0 || ch >= nch) continue;
                    outDitherBits[ch] = outDitherBits[ch] == 0 ? bits : juce::jmin (outDitherBits[ch], bits);
                    any = true;
                }
            }
        }
        if (any)
            for (int c = 0; c < nch; ++c)
            {
                if (outDitherBits[c] == 0 || out[c] == nullptr) continue;
                const auto mm = juce::FloatVectorOperations::findMinAndMax (out[c], numSamples);
                if (mm.getStart() == 0.0f && mm.getEnd() == 0.0f) continue;      // digital silence stays silence
                dither::reduceInPlace (out[c], numSamples, outDitherBits[c], outDither.state[c & 255]);
            }
    }

    // The talkback outputs (any number of pairs). Whatever the mixers' routing put on them is thrown away: they only ever carry what is allowed below.
    {
        bool anyTb = false;
        for (int o = 0; o < numOut && o < kMaxInputs; ++o) if (tbMask[(size_t) o].load (std::memory_order_relaxed) != 0 && out[o] != nullptr) { anyTb = true; break; }
        if (anyTb)
        {
            bool anySel = false;
            for (int ci = 0; ci < numIn && ci < kMaxInputs && ! anySel; ++ci) anySel = crSel[(size_t) ci].load (std::memory_order_relaxed) && in[ci] != nullptr;
            const float target = (crOn.load (std::memory_order_relaxed) && anySel) ? 1.0f : 0.0f;
            const float g0 = crGain, step = (target - g0) / (float) juce::jmax (1, numSamples);
            for (int o = 0; o < numOut && o < kMaxInputs; ++o)
            {
                const int mask = tbMask[(size_t) o].load (std::memory_order_relaxed);
                if (mask == 0 || out[o] == nullptr) continue;
                float* L = out[o];
                juce::FloatVectorOperations::clear (L, numSamples);
                if (tbPlayNow)                                                   // recorded audio only (see above)
                {
                    if (mask & 1) juce::FloatVectorOperations::add (L, tapL, numSamples);
                    if (mask & 2) juce::FloatVectorOperations::add (L, tapR, numSamples);
                }
                if (target > 0.0f || crGain > 0.0f)
                    for (int ci = 0; ci < numIn && ci < kMaxInputs; ++ci)        // every selected input is added (the fade is the same for all)
                    {
                        if (! crSel[(size_t) ci].load (std::memory_order_relaxed) || in[ci] == nullptr) continue;
                        const float* src = in[ci];
                        for (int i = 0; i < numSamples; ++i) L[i] += src[i] * (g0 + step * (float) (i + 1));
                    }
            }
            crGain = target;
        }
        else crGain = 0.0f;
    }

    // output meters
    for (int c = 0; c < numOut && c < kMaxInputs; ++c)
    {
        if (out[c] == nullptr) continue;
        const auto mag = juce::FloatVectorOperations::findMinAndMax (out[c], numSamples);
        const float pk = juce::jmax (std::abs (mag.getStart()), std::abs (mag.getEnd()));
        if (pk > outputPeak[c].load (std::memory_order_relaxed)) outputPeak[c].store (pk, std::memory_order_relaxed);
    }

    samplePosition.fetch_add (numSamples);
    blockCounter.fetch_add (1);
}

static void panGains (float pan, bool stereoSource, float& gL, float& gR) noexcept
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    (void) stereoSource;        // every channel is panned on its own: constant power, -3 dB at centre, unity at the extremes
    const float a = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    gL = std::cos (a);
    gR = std::sin (a);
}

void AudioEngine::processMixer (MixerPlan& mp, const float* const* in, int numIn, int offset, int n,
                                float* const* out, int numOut, bool playing, bool silenceOwnOutput, int alsoToFirst,
                                bool playbackOnly, float* tapL, float* tapR)
{
    auto& m = *mp.mixer;
    bool anySolo = false;
    for (auto& nd : mp.order) anySolo = anySolo || (nd->kind == NodeKind::Track && nd->strip->solo.get());

    for (auto& nd : mp.order) if (nd->receives) nd->incoming.clear (0, n);

    auto* plan_ = plan.load (std::memory_order_relaxed);
    const bool recordingNow = rec.load (std::memory_order_relaxed) != nullptr;

    for (auto& ndp : mp.order)
    {
        Node& nd = *ndp;
        float* L = mp.pre.getWritePointer (0);
        float* R = mp.pre.getWritePointer (1);
        bool audible = true;
        float faderGain = 1.0f;
        float* lastGain = nullptr; float* lastPre = nullptr;
        Param *meterL = nullptr, *meterR = nullptr;

        if (nd.kind == NodeKind::Track)
        {
            auto* st = nd.strip;
            const auto& route = plan_->tracks[(size_t) nd.trackIndex];
            const int nc = route.numCh;

            // which sources reach this strip (see Monitor): the recording played from disk, and/or the live input
            const juce::AudioBuffer<float>* diskBuf = plan_->disk[(size_t) nd.trackIndex];
            const bool fromDisk = diskBuf != nullptr;
            bool liveOn = true;
            if (route.live != nullptr)
                switch (route.live->getMonitor())
                {
                    case Monitor::Edit:        liveOn = route.live->armed.get() && ! playing; break;
                    case Monitor::Session:     liveOn = ! playing; break;
                    case Monitor::SessionLive: liveOn = true; break;
                    case Monitor::Talkback:    liveOn = ! recordingNow; break;
                }
            if (playbackOnly) liveOn = false;                   // talkback playback: this pass may only contain recorded audio
            for (int c = 0; c < nc; ++c)
            {
                auto* dst = mp.chanBuf.getWritePointer (c);
                if (fromDisk && c < diskBuf->getNumChannels())
                    juce::FloatVectorOperations::copy (dst, diskBuf->getReadPointer (c), n);
                else
                    juce::FloatVectorOperations::clear (dst, n);
                const int idx = route.inputs[c];
                if (liveOn && idx >= 0 && idx < numIn && in[idx] != nullptr)
                    juce::FloatVectorOperations::add (dst, in[idx] + offset, n);
            }
            if (nd.receives)                                    // what other channels send to this track joins its own signal
            {
                if (nc == 1)
                {
                    juce::FloatVectorOperations::addWithMultiply (mp.chanBuf.getWritePointer (0), nd.incoming.getReadPointer (0), 0.5f, n);
                    juce::FloatVectorOperations::addWithMultiply (mp.chanBuf.getWritePointer (0), nd.incoming.getReadPointer (1), 0.5f, n);
                }
                else
                {
                    juce::FloatVectorOperations::add (mp.chanBuf.getWritePointer (0), nd.incoming.getReadPointer (0), n);
                    juce::FloatVectorOperations::add (mp.chanBuf.getWritePointer (1), nd.incoming.getReadPointer (1), n);
                }
            }

            juce::AudioBuffer<float> view (mp.chanBuf.getArrayOfWritePointers(), nc, n);
            for (auto& slot : st->slots) slot.process (view);

            // fold to stereo (before the fader, so sends can be taken before or after it)
            float gL = 1, gR = 1;
            if (nc == 1)
            {
                panGains (st->pan.get(), false, gL, gR);
                juce::FloatVectorOperations::copyWithMultiply (L, view.getReadPointer (0), gL, n);
                juce::FloatVectorOperations::copyWithMultiply (R, view.getReadPointer (0), gR, n);
            }
            else if (nc == 2)
            {
                // each channel of a stereo track has its own pan (e.g. left channel placed right, right channel left)
                float aL = 1, aR = 0, bL = 0, bR = 1;
                panGains (st->panL.get(), false, aL, aR);
                panGains (st->panR.get(), false, bL, bR);
                juce::FloatVectorOperations::copyWithMultiply (L, view.getReadPointer (0), aL, n);
                juce::FloatVectorOperations::addWithMultiply  (L, view.getReadPointer (1), bL, n);
                juce::FloatVectorOperations::copyWithMultiply (R, view.getReadPointer (0), aR, n);
                juce::FloatVectorOperations::addWithMultiply  (R, view.getReadPointer (1), bR, n);
            }
            else   // surround: simple monitor fold-down (even channels left, odd right) - placeholder
            {
                const float k = 1.0f / std::sqrt ((float) ((nc + 1) / 2));
                mp.pre.clear (0, n);
                for (int c = 0; c < nc; ++c)
                    juce::FloatVectorOperations::addWithMultiply ((c & 1) ? R : L, view.getReadPointer (c), k, n);
            }
            audible = ! st->mute.get() && (! anySolo || st->solo.get());
            faderGain = dbToGain (st->gainDb.get());
            lastGain = &st->lastGain; lastPre = &st->lastPre; meterL = &st->meterL; meterR = &st->meterR;
        }
        else   // Int Bus or Ext Bus: what is sent to it, through its inserts
        {
            auto* b = nd.bus;
            if (nd.receives) { mp.pre.copyFrom (0, 0, nd.incoming, 0, 0, n); mp.pre.copyFrom (1, 0, nd.incoming, 1, 0, n); }
            else mp.pre.clear (0, n);
            juce::AudioBuffer<float> view (mp.pre.getArrayOfWritePointers(), 2, n);
            for (auto& slot : b->slots) slot.process (view);
            audible = ! b->mute.get();
            faderGain = dbToGain (b->gainDb.get());
            lastGain = &b->lastGain; lastPre = &b->lastPre; meterL = &b->meterL; meterR = &b->meterR;
        }

        // the fader: post = pre * gain (smoothed)
        const float target = audible ? faderGain : 0.0f;
        const float g0 = *lastGain;
        *lastGain = target;
        for (int c = 0; c < 2; ++c) mp.post.copyFromWithRamp (c, 0, mp.pre.getReadPointer (c), n, g0, target);

        // meter (post-fader)
        for (int c = 0; c < 2; ++c)
        {
            const auto mm = mp.post.findMinMax (c, 0, n);
            const float pk = juce::jmax (std::abs (mm.getStart()), std::abs (mm.getEnd()));
            auto* meter = c == 0 ? meterL : meterR;
            if (pk > meter->get()) meter->set (pk);
        }
        {   // RMS (about 300 ms), for the meter settings
            float& ms0 = nd.kind == NodeKind::Track ? nd.strip->msL : nd.bus->msL;
            float& ms1 = nd.kind == NodeKind::Track ? nd.strip->msR : nd.bus->msR;
            const float k = 1.0f - std::exp (-(float) n / (0.3f * (float) sampleRate));
            const float a = mp.post.getRMSLevel (0, 0, n), b = mp.post.getRMSLevel (1, 0, n);
            ms0 += (a * a - ms0) * k; ms1 += (b * b - ms1) * k;
            (nd.kind == NodeKind::Track ? nd.strip->rmsL : nd.bus->rmsL).set (std::sqrt (ms0));
            (nd.kind == NodeKind::Track ? nd.strip->rmsR : nd.bus->rmsR).set (std::sqrt (ms1));
        }
        if (const void* sc = scopeSource.load (std::memory_order_relaxed))
            if (sc == (nd.kind == NodeKind::Track ? (const void*) nd.strip : (const void*) nd.bus))
            {
                auto w = scopeWrite.load (std::memory_order_relaxed);
                const float* pl = mp.post.getReadPointer (0); const float* pr = mp.post.getReadPointer (1);
                for (int i = 0; i < n; ++i)
                {
                    const auto idx = (size_t) (w % kScopeFrames);
                    scopeRing[idx * 2] = pl[i]; scopeRing[idx * 2 + 1] = pr[i];
                    ++w;
                }
                scopeWrite.store (w, std::memory_order_release);
            }

        if (nd.tapIdx >= 0 && captureBufs != nullptr && nd.tapIdx < (int) captureBufs->size())   // offline render: this channel is one of the outputs being captured
            for (int c = 0; c < 2; ++c) (*captureBufs)[(size_t) nd.tapIdx].addFrom (c, 0, mp.post, c, 0, n);

        // sends (pre-fader ones still obey mute / solo, but not the fader)
        bool needPre = false;
        for (auto& sp : nd.sends) needPre = needPre || (sp.st->pre.get() && (sp.st->active() || sp.st->lastGain > 0.0f));
        const float preTarget = audible ? 1.0f : 0.0f;
        if (needPre)
        {
            for (int c = 0; c < 2; ++c) mp.preSend.copyFromWithRamp (c, 0, mp.pre.getReadPointer (c), n, *lastPre, preTarget);
        }
        *lastPre = preTarget;
        for (auto& sp : nd.sends)
        {
            auto* sd = sp.st;
            const float st1 = sd->active() ? dbToGain (sd->gainDb.get()) : 0.0f;
            if (st1 == 0.0f && sd->lastGain == 0.0f) continue;
            const juce::AudioBuffer<float>* src = sd->pre.get() ? &mp.preSend : &mp.post;
            bool hasInserts = false;
            for (auto& slot : sd->slots) hasInserts = hasInserts || slot.mightBeLoaded();
            if (hasInserts)                                       // the send's own inserts: these never touch the channel's own signal
            {
                for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, *src, c, 0, n);
                juce::AudioBuffer<float> view (mp.sendTmp.getArrayOfWritePointers(), 2, n);
                for (auto& slot : sd->slots) slot.process (view);
                src = &mp.sendTmp;
            }
            for (int c = 0; c < 2; ++c) sp.dest->incoming.addFromWithRamp (c, 0, src->getReadPointer (c), n, sd->lastGain, st1);
            sd->lastGain = st1;
        }

        // where it goes: an Ext Bus writes to its own driver outputs (nothing else ever reaches the driver)
        if (nd.kind == NodeKind::ExtBus)
        {
            const int first = nd.bus->outFirst.load (std::memory_order_relaxed);
            if (tapL != nullptr && nd.isMainOut)                                  // talkback playback: the processing mixer's main output, for the TB pair
            {
                juce::FloatVectorOperations::copy (tapL + offset, mp.post.getReadPointer (0), n);
                juce::FloatVectorOperations::copy (tapR + offset, mp.post.getReadPointer (1), n);
            }
            for (int c = 0; c < 2; ++c)
            {
                if (first >= 0 && ! silenceOwnOutput && first + c < numOut && out[first + c] != nullptr)
                    juce::FloatVectorOperations::add (out[first + c] + offset, mp.post.getReadPointer (c), n);
                const int ac = alsoToFirst + c;                          // the audited cue mixer is also heard on the engineer's output pair
                if (alsoToFirst >= 0 && ac != first + c && ac < numOut && out[ac] != nullptr)
                    juce::FloatVectorOperations::add (out[ac] + offset, mp.post.getReadPointer (c), n);
            }
        }
    }
}

// ------------------------------------------------------------------ playback
juce::String AudioEngine::startPlayback (std::unique_ptr<PlaybackSession> s)
{
    if (isRecording()) return "Stop recording before playing back";
    if (zeros.empty()) return "The audio device has not been started";
    stopPlayback();
    if (std::abs (s->getSampleRate() - sampleRate) > 0.5)
        return "The take was recorded at " + juce::String (s->getSampleRate(), 0) + " Hz but the audio device runs at " + juce::String (sampleRate, 0) + " Hz";
    auto err = s->open();
    if (err.isNotEmpty()) return err;
    s->start();
    playSession = std::move (s);
    playback.store (playSession.get(), std::memory_order_release);
    return {};
}

void AudioEngine::stopPlayback()
{
    if (playSession == nullptr) return;
    playback.store (nullptr, std::memory_order_release);
    waitForAudioThread();
    playSession.reset();
}

// ------------------------------------------------------------------ recording
juce::String AudioEngine::startRecording (TakeWindowDef& window)
{
    if (isRecording()) return "Already recording";
    if (zeros.empty()) return "The audio device has not been started";

    std::vector<const TrackDef*> armed;
    for (auto& t : project.tracks) if (t.isArmed()) armed.push_back (&t);
    if (armed.empty()) return "No tracks are record-armed";
    for (auto* t : armed) window.unhide (t->id);          // a track you removed from this window comes back when you record it again

    auto folder = project.takeFolder (window);
    if (! folder.createDirectory()) return "Cannot create folder " + folder.getFullPathName();
    if (folder.getBytesFreeOnVolume() < 500LL * 1024 * 1024) return "Less than 500 MB free on the recording drive";

    auto rs = std::make_unique<RecordingSession>();
    auto* ringNow = sessionMode.load() ? preRollPtr.load() : nullptr;     // session mode: this take starts with the last few seconds
    rs->useRing = ringNow != nullptr; rs->ring = ringNow;
    TakeGroup group;
    group.number = window.nextNumber;
    group.sampleRate = sampleRate;
    group.recordedAt = juce::Time::getCurrentTime();
    const auto label = window.labelFor (group);
    const auto now = juce::Time::getCurrentTime();

    for (auto* t : armed)
    {
        RecTrack rt;
        rt.info.trackId = t->id;
        rt.info.trackName = t->name;
        rt.info.numChannels = t->channelCount();
        rt.route.numCh = rt.info.numChannels;
        for (int c = 0; c < rt.route.numCh; ++c) rt.route.inputs[c] = t->inputOf (c);
        rt.peaks = std::make_unique<LivePeaks>();
        rt.peaks->prepare (sampleRate, rt.info.numChannels);

        auto file = folder.getChildFile (TakeWindowDef::fileNameFor (group.number, label, t->name));
        for (int dup = 2; file.existsAsFile(); ++dup)   // never overwrite an existing recording
            file = folder.getChildFile (TakeWindowDef::fileNameFor (group.number, label, t->name + " (" + juce::String (dup) + ")"));
        rt.info.file = file;

        auto opts = juce::AudioFormatWriterOptions()
                        .withSampleRate (sampleRate)
                        .withNumChannels (rt.info.numChannels)
                        .withBitsPerSample (24)
                        .withMetadata (juce::WavAudioFormat::bwavDescription, window.name + " / " + window.displayName (group) + " / " + t->name)
                        .withMetadata (juce::WavAudioFormat::bwavOriginator, "Fermata")
                        .withMetadata (juce::WavAudioFormat::bwavOriginationDate, now.formatted ("%Y-%m-%d"))
                        .withMetadata (juce::WavAudioFormat::bwavOriginationTime, now.formatted ("%H:%M:%S"));

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) return "Cannot create " + file.getFullPathName();
        auto w = wav.createWriterFor (stream, opts);
        if (w == nullptr) return "Cannot create a 24-bit WAV writer for " + t->name;
        rt.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), writerThread, ringNow != nullptr ? 1 << 19 : 1 << 18);
        group.files.push_back (rt.info);
        rs->tracks.push_back (std::move (rt));
    }

    group.id = juce::Uuid();
    group.startSeconds = window.groups.empty() ? 0.0 : window.endSeconds() + 2.0;
    rs->windowId = window.id;
    rs->groupId = group.id;
    currentGroupId = group.id;
    window.groups.push_back (group);
    window.nextNumber++;

    session = std::move (rs);
    rec.store (session.get(), std::memory_order_release);
    project.changed();
    return {};
}

juce::Uuid AudioEngine::stopRecording()
{
    if (session == nullptr) return juce::Uuid::null();
    rec.store (nullptr, std::memory_order_release);
    waitForAudioThread();

    if (session->useRing && session->startPos >= 0)                    // hand over whatever the audio thread had not delivered yet
    {
        deliverFromRing (*session, session->delivered, session->livePos.load());
        session->delivered = session->livePos.load();
        session->samples.store (session->delivered - session->first);
    }
    const auto length = session->samples.load();
    const auto gid = session->groupId;
    session->tracks.clear();          // destroys the ThreadedWriters: flushes everything to disk
    if (auto* w = project.findTakeWindow (session->windowId))
        if (auto* g = w->findGroup (gid))
            g->lengthSamples = length;

    session.reset();
    ensurePreRoll();                                  // session mode was switched off (or the patching changed) while recording
    project.changed();
    return gid;
}

double AudioEngine::recordedSeconds() const
{
    auto* s = rec.load();
    return s != nullptr && sampleRate > 0 ? (double) s->samples.load() / sampleRate : 0.0;
}

int AudioEngine::overruns() const
{
    auto* s = rec.load();
    return s != nullptr ? s->overruns.load() : 0;
}

void AudioEngine::LivePeaks::add (const float* const* chans, int numChans, int n) noexcept
{
    int done = 0;
    const int cap = kBinsPerSecond * kMaxSeconds;
    const int nc = juce::jmin (numChans, numCh);
    while (done < n)
    {
        const int take = juce::jmin (n - done, binSamples - count);
        for (int c = 0; c < nc; ++c)
        {
            const auto mm = juce::FloatVectorOperations::findMinAndMax (chans[c] + done, take);
            if (count == 0) { curMin[c] = mm.getStart(); curMax[c] = mm.getEnd(); }
            else { curMin[c] = juce::jmin (curMin[c], mm.getStart()); curMax[c] = juce::jmax (curMax[c], mm.getEnd()); }
        }
        count += take; done += take;
        if (count >= binSamples)
        {
            const int b = bins.load (std::memory_order_relaxed);
            if (b < cap)
            {
                for (int c = 0; c < numCh; ++c)
                {
                    const auto i = ((size_t) b * (size_t) numCh + (size_t) c) * 2;
                    data[i]     = (juce::int16) juce::jlimit (-32767, 32767, (int) std::lround (curMin[c] * 32767.0f));
                    data[i + 1] = (juce::int16) juce::jlimit (-32767, 32767, (int) std::lround (curMax[c] * 32767.0f));
                }
                bins.store (b + 1, std::memory_order_release);
            }
            count = 0;
        }
    }
}

bool AudioEngine::getLivePeaks (const juce::Uuid& trackId, const juce::int16*& data, int& bins, int& binSamples, int* numChannels) const
{
    if (session == nullptr) return false;
    for (auto& rt : session->tracks)
        if (rt.info.trackId == trackId && rt.peaks != nullptr)
        {
            bins = rt.peaks->bins.load (std::memory_order_acquire);
            binSamples = rt.peaks->binSamples;
            data = rt.peaks->data.data();
            if (numChannels != nullptr) *numChannels = rt.peaks->numCh;
            return true;
        }
    return false;
}

int AudioEngine::readScope (float* l, float* r, int maxFrames) const noexcept
{
    const auto w = scopeWrite.load (std::memory_order_acquire);
    const int n = (int) juce::jmin<juce::int64> ((juce::int64) juce::jmin (maxFrames, kScopeFrames - 2048), w);
    for (int i = 0; i < n; ++i)
    {
        const auto idx = (size_t) ((w - n + i) % kScopeFrames);
        l[i] = scopeRing[idx * 2]; r[i] = scopeRing[idx * 2 + 1];
    }
    return n;
}

float AudioEngine::takeInputPeak (int inputIndex) noexcept
{
    if (inputIndex < 0 || inputIndex >= kMaxInputs) return 0.0f;
    return inputPeak[inputIndex].exchange (0.0f);
}

float AudioEngine::takeOutputPeak (int outputIndex) noexcept
{
    if (outputIndex < 0 || outputIndex >= kMaxInputs) return 0.0f;
    return outputPeak[outputIndex].exchange (0.0f);
}
} // namespace td

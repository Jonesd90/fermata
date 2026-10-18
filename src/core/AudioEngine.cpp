#include "AudioEngine.h"
#include "CoreReservation.h"
#include <algorithm>
#include <cstring>

namespace td
{
AudioEngine::AudioEngine (Project& p) : project (p)
{
    for (auto& x : inputPeak) x.store (0.0f);
    for (auto& x : outputPeak) x.store (0.0f);
    for (auto& x : stageL) x.store (1.0f);
    for (auto& x : stageR) x.store (1.0f);
    for (auto& x : stagePrevL) x = 1.0f;
    for (auto& x : stagePrevR) x = 1.0f;
    writerThread.startThread (juce::Thread::Priority::high);
    finisher.startThread (juce::Thread::Priority::high);
    // (the disk writer is NOT put on the reserved cores: they are kept for the work with a deadline every block - the audio callback and the mixer
    // helpers. The writer has seconds of buffer in hand, so it runs on the other cores, at high priority, out of their way.)
    // helpers for running the mixers side by side: one fewer than the physical cores the audio can use, leaving a core or two for everything else
    mixerWorkers.start (juce::jlimit (0, 6, juce::SystemStats::getNumPhysicalCpus() - 2));
}

AudioEngine::~AudioEngine()
{
    mixerWorkers.stop();
    if (isRecording()) stopRecording();
    waitForFinishedTakes (60000);                    // (closing the program: every file is finished first)
    finisher.stopThread (5000);
    for (auto& f : finishing) if (f->rs != nullptr) for (auto& rt : f->rs->tracks) rt.writer.reset();     // (any the finisher did not get to)
    finishing.clear();
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
    auto& cores = CoreReservation::get();
    cores.audioThreadCheck();                                  // first block on this thread, or the reserved cores just changed: move this thread to them
    // with cores reserved, no more helpers than the reserved processors can run beside this thread (two audio threads on one processor would wait for each other)
    const int reserved = cores.reservedLogicalProcessors();
    mixerWorkers.setLimit (reserved > 0 ? reserved - 1 : 1 << 20);
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = juce::Time::getHighResolutionTicks();
    process (in, numIn, out, numOut, numSamples);
    // the load meter: time spent here as a share of how long the block lasts
    const double busy = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double span = sampleRate > 0.0 ? (double) numSamples / sampleRate : 0.0;
    if (span > 0.0)
    {
        const float pct = (float) (100.0 * busy / span);
        loadBusy.store (loadBusy.load (std::memory_order_relaxed) + busy, std::memory_order_relaxed);      // (only this thread adds; the reader exchanges)
        loadSpan.store (loadSpan.load (std::memory_order_relaxed) + span, std::memory_order_relaxed);
        if (pct > loadPeakPct.load (std::memory_order_relaxed)) loadPeakPct.store (pct, std::memory_order_relaxed);
        loadBlocks.fetch_add (1, std::memory_order_relaxed);
        if (busy > span) loadLate.fetch_add (1, std::memory_order_relaxed);
    }
}

AudioEngine::LoadStats AudioEngine::takeLoadStats() noexcept
{
    LoadStats st;
    const double busy = loadBusy.exchange (0.0), span = loadSpan.exchange (0.0);
    st.average = span > 0.0 ? (float) (100.0 * busy / span) : 0.0f;
    st.peak = loadPeakPct.exchange (0.0f);
    st.blocks = loadBlocks.exchange (0); st.late = loadLate.exchange (0);
    st.recordBufferPeak = recBufPeakPct.exchange (0.0f);
    return st;
}

void AudioEngine::prepare (double sr, int block)
{
    stopPlayback();
    lastPlanSeen.store (nullptr);                  // (the audio thread is not running here: the plan it last saw may already be gone)
    sampleRate = sr;
    maxBlock = juce::jmax (16, block);
    // plug-ins that a project (re)load or a mix copy starts from now on are prepared for what the device really runs at, not 48 kHz / 512
    project.restoreSampleRate = sampleRate;
    project.restoreBlock = maxBlock;
    zeros.assign ((size_t) juce::jmax (maxBlock * 4, 8192), 0.0f);
    ltcIn.prepare (sr); ltcGen.prepare (sr); ltcSeenRestart = -1;
    tbTap.assign (zeros.size() * 2, 0.0f);   // also the silence for the pre-roll hand-over (4096 samples at a time)
    mainCap.assign (zeros.size() * 2, 0.0f);
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
        // the outputs: every Audio Track / Int Bus whose output is on goes (after its fader, at unity) to its chosen bus, or - set to 'Main' - to EVERY Ext Bus
        // ticked as a main output of this mixer (none ticked: nowhere). A main Ext Bus also plays whatever is sent to it.
        {
            std::vector<int> mains;
            for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->kind == NodeKind::ExtBus && nodes[i]->bus->isMain.get()) mains.push_back ((int) i);
            if (! mains.empty()) nodes[(size_t) mains.front()]->isMainOut = true;   // the first one is "the" main output (talkback playback, a cue mixer that follows)
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                Flag* on = nullptr; const juce::Uuid* dest = nullptr; SendState* st = nullptr;
                if (nodes[i]->kind == NodeKind::Track)       { on = &nodes[i]->strip->outOn; dest = &nodes[i]->strip->outDest; st = &nodes[i]->strip->outSend; }
                else if (nodes[i]->kind == NodeKind::IntBus) { on = &nodes[i]->bus->outOn;   dest = &nodes[i]->bus->outDest;   st = &nodes[i]->bus->outSend; }
                if (on == nullptr || ! on->get()) continue;
                const int d = dest->isNull() ? -1 : indexOfId (*dest);
                if (d >= 0) { if (d != (int) i) edges.push_back ({ (int) i, d, st }); continue; }
                for (int mi : mains) if (mi != (int) i) edges.push_back ({ (int) i, mi, st });   // 'Main' (or the chosen bus is gone): every main output
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
        if (! offlineMixerId.isNull())                     // (every mixer gets its taps: a bounce can use a different mixer for each piece)
        {
            int next = 0;
            std::vector<int> idxOf (offlineTaps.size(), -1);
            for (size_t i = 0; i < offlineTaps.size(); ++i) idxOf[i] = next++;
            for (auto& nd : mp->order)
            {
                const auto id = nd->kind == NodeKind::Track ? nd->strip->trackId : nd->bus->busId;
                const auto it = std::find (offlineTaps.begin(), offlineTaps.end(), id);
                nd->tapIdx = it != offlineTaps.end() ? idxOf[(size_t) (it - offlineTaps.begin())] : -1;
                const auto ip = std::find (offlinePreTaps.begin(), offlinePreTaps.end(), id);
                nd->tapPreIdx = ip != offlinePreTaps.end() ? (int) offlineTaps.size() + (int) (ip - offlinePreTaps.begin()) : -1;
            }
        }
        // ---- delay compensation: every path through the mixer is made equally long, so a plug-in that holds the sound back (look-ahead,
        // linear phase) on one channel does not put that channel out of time with the others.
        {
            auto chainLat = [] (const InsertSlot* sl, int count) { int t = 0; for (int i = 0; i < count; ++i) t += sl[i].latencySamples(); return t; };
            for (auto& nd : mp->order)                                       // nodes come after all the nodes that feed them
            {
                const int own = nd->kind == NodeKind::Track ? chainLat (nd->strip->slots, kNumSlots) : chainLat (nd->bus->slots, kNumSlots);
                nd->outLat = nd->inLat + own;
                for (auto& sp : nd->sends)
                    sp.dest->inLat = juce::jmax (sp.dest->inLat, nd->outLat + chainLat (sp.st->slots, kSendSlots));
            }
            int longest = 0;
            for (auto& nd : mp->order)
            {
                for (auto& sp : nd->sends)
                    sp.delay.init (sp.dest->inLat - (nd->outLat + chainLat (sp.st->slots, kSendSlots)), 2);
                if (nd->kind == NodeKind::Track && nd->inLat > 0)
                    nd->srcDelay.init (nd->inLat, juce::jlimit (1, (int) kMaxTrackChannels, p->tracks[(size_t) nd->trackIndex].numCh));
                if (nd->kind == NodeKind::ExtBus) longest = juce::jmax (longest, nd->outLat);
            }
            for (auto& nd : mp->order) if (nd->kind == NodeKind::ExtBus) nd->outDelay.init (longest - nd->outLat, 2);
            mp->latency = longest;
            int tapLongest = 0;
            for (auto& nd : mp->order) if (nd->tapIdx >= 0 || nd->tapPreIdx >= 0) tapLongest = juce::jmax (tapLongest, nd->outLat);
            for (auto& nd : mp->order)
            {
                if (nd->tapIdx >= 0)    nd->tapDelay.init (tapLongest - nd->outLat, 2);
                if (nd->tapPreIdx >= 0) nd->tapDelayPre.init (tapLongest - nd->outLat, 2);
            }
            mp->tapLatency = tapLongest;
        }
        mp->chanBuf.setSize (kMaxTrackChannels, maxBlock);
        mp->pre.setSize (2, maxBlock); mp->post.setSize (2, maxBlock); mp->preSend.setSize (2, maxBlock);
        mp->sendTmp.setSize (2, maxBlock);
        {   // for running side by side with the other mixers (see process)
            const int outs = juce::jmax (8, deviceOutputs);
            mp->outScratch.setSize (outs, maxBlock);
            mp->outPtrs.assign ((size_t) outs, nullptr);
            int ext = 0;
            for (auto& nd : mp->order) if (nd->kind == NodeKind::ExtBus) ++ext;
            mp->touched.assign ((size_t) (2 * ext + 2), -1);
        }
        p->mixers.push_back (std::move (mp));
    }
    p->jobs.resize (p->mixers.size());
    p->followers.assign (p->mixers.size(), nullptr);
    p->outChannels = juce::jmax (8, deviceOutputs);
    return p;
}

void AudioEngine::rebuildPlan()
{
    const juce::ScopedLock sl (planLock);
    // mixers' strip pools must be complete before the audio thread can see them
    project.syncMixers();
    planLatencySig.store (latencySignature (true));
    auto fresh = buildPlan();
    auto* old = plan.exchange (fresh.release());
    if (! running.load()) lastPlanSeen.store (nullptr);       // no audio thread to pick it up: nothing to carry over into the new plan
    if (waitForAudioThread()) delete old;           // (if the audio thread stalled, the old plan is left in memory rather than freed under it)
                                                    // (the audio thread has moved the sound in the old plan's delay lines into the new one by now: see carryOver)
    ensurePreRoll();                                 // a track patched to a new input starts being kept for the pre-roll
}

juce::uint64 AudioEngine::latencySignature (bool refresh) const
{
    juce::uint64 sig = 17;                                  // (unsigned: the multiplication is meant to wrap)
    auto one = [&] (InsertSlot* sl, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (refresh) sl[i].refreshLatency();
            sig = sig * 1000003u + (juce::uint64) (sl[i].latencySamples() + (sl[i].mightBeLoaded() ? 5 : 3) + (sl[i].bypass.get() ? 11 : 0));
        }
    };
    for (auto& m : project.mixers)
    {
        for (auto& st : m->stripPool) { one (st->slots, kNumSlots); for (auto& sd : st->sends.pool) one (sd->slots, kSendSlots); one (st->outSend.slots, kSendSlots); }
        for (auto& f : m->busPool)    { one (f->slots, kNumSlots);  for (auto& sd : f->sends.pool) one (sd->slots, kSendSlots); one (f->outSend.slots, kSendSlots); }
    }
    return sig;
}

void AudioEngine::checkLatency()
{
    if (sampleRate <= 0.0 || plan.load() == nullptr) return;
    if (latencySignature (true) != planLatencySig.load()) rebuildPlan();
}

int AudioEngine::mixerLatency (const MixerState* m) const
{
    if (auto* pl = plan.load (std::memory_order_acquire))
        for (auto& mp : pl->mixers) if (mp->mixer == m) return mp->latency;
    return 0;
}

int AudioEngine::offlineLatency() const
{
    if (auto* pl = plan.load (std::memory_order_acquire))
        for (auto& mp : pl->mixers) if (mp->mixer->id == offlineMixerId) return mp->tapLatency;
    return 0;
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
        if (cur != nullptr) { preRollPtr.store (nullptr); if (waitForAudioThread()) delete cur; }
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
    if (waitForAudioThread()) delete old;
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
    writerThread.notify();                          // once for all the files (not once per file per block)
}

bool AudioEngine::waitForAudioThread()
{
    if (! running.load()) return true;
    const auto c0 = blockCounter.load();
    const auto deadline = juce::Time::getMillisecondCounter() + 500;
    while (blockCounter.load() < c0 + 2 && juce::Time::getMillisecondCounter() < deadline && running.load())
        juce::Thread::sleep (2);
    return blockCounter.load() >= c0 + 2 || ! running.load();
}

void AudioEngine::setOfflineTarget (const juce::Uuid& mixerId, std::vector<juce::Uuid> tapIds, std::vector<juce::Uuid> preFaderIds)
{
    offlineMixerId = mixerId; offlineTaps = std::move (tapIds); offlinePreTaps = std::move (preFaderIds);
}

void AudioEngine::renderOffline (const std::vector<juce::AudioBuffer<float>>& trackAudio, int n, std::vector<juce::AudioBuffer<float>>& captures)
{
    auto* pl = plan.load (std::memory_order_acquire);
    for (auto& c : captures) c.clear();
    if (pl == nullptr || captures.empty()) return;
    n = juce::jmin (n, maxBlock, captures[0].getNumSamples());
    for (size_t i = 0; i < pl->disk.size(); ++i) pl->disk[i] = i < trackAudio.size() ? &trackAudio[i] : nullptr;
    captureBufs = &captures;
    transport.playing = true; transport.recording = false; transport.samplePos = offlinePos; transport.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    offlinePos += n;
    for (auto& mp : pl->mixers)
        if (mp->mixer->id == offlineMixerId)
            processMixer (*pl, *mp, nullptr, 0, 0, n, nullptr, 0, true, false, -1);
    captureBufs = nullptr;
}

// ------------------------------------------------------------------ audio thread
/** An Edit mixer runs only while its own Edit plays; the processing mixer (isFront) rests while an Edit that has a mixer of its own plays. */
bool AudioEngine::mixerHasOutput (const MixerPlan& mp) noexcept
{
    for (auto& nd : mp.order)
        if (nd->kind == NodeKind::ExtBus && nd->bus->outFirst.load (std::memory_order_relaxed) >= 0) return true;
    return false;
}

void AudioEngine::runMixerJob (void* engine, int index) noexcept
{
    auto& e = *static_cast<AudioEngine*> (engine);
    const auto& b = e.blockJobs;
    const auto& job = b.plan->jobs[(size_t) index];
    auto& mp = *job.mp;
    // the driver outputs this mixer can write in this block: its Ext buses' pairs (unless they are silenced for an audition) and the audition pair
    mp.numTouched = 0;
    auto addCh = [&] (int ch)
    {
        if (ch < 0 || ch >= b.numOut || b.out[ch] == nullptr) return;
        for (int k = 0; k < mp.numTouched; ++k) if (mp.touched[(size_t) k] == ch) return;
        if (mp.numTouched < (int) mp.touched.size()) mp.touched[(size_t) mp.numTouched++] = ch;
    };
    for (auto& nd : mp.order)
        if (nd->kind == NodeKind::ExtBus && ! job.silenceOwnOutput)
        {
            const int f = nd->bus->outFirst.load (std::memory_order_relaxed);
            if (f >= 0) { addCh (f); addCh (f + 1); }
        }
    if (job.alsoToFirst >= 0) { addCh (job.alsoToFirst); addCh (job.alsoToFirst + 1); }
    for (int k = 0; k < mp.numTouched; ++k) juce::FloatVectorOperations::clear (mp.outScratch.getWritePointer (mp.touched[(size_t) k]), b.n);
    for (int c = 0; c < b.numOut && c < (int) mp.outPtrs.size(); ++c) mp.outPtrs[(size_t) c] = b.out[c] != nullptr ? mp.outScratch.getWritePointer (c) : nullptr;
    e.processMixer (*b.plan, mp, b.in, b.numIn, 0, b.n, mp.outPtrs.data(), b.numOut, b.playing, job.silenceOwnOutput, job.alsoToFirst,
                    job.strict, job.strict ? b.tapL : nullptr, job.strict ? b.tapR : nullptr, job.capMain ? b.mainL : nullptr, job.capMain ? b.mainR : nullptr);
}

/** Moves a delay line's contents across when both plans have the same delay there (a changed delay starts empty: it cannot be helped). */
static void takeDelay (PathDelay& to, PathDelay& from) noexcept
{
    if (to.len > 0 && to.len == from.len && to.ch == from.ch && to.data.size() == from.data.size()) { std::swap (to.data, from.data); to.pos = from.pos; }
}

void AudioEngine::carryOver (Plan& from, Plan& to) noexcept
{
    for (auto& tm : to.mixers)
    {
        MixerPlan* fm = nullptr;
        for (auto& m : from.mixers) if (m->mixer == tm->mixer) { fm = m.get(); break; }
        if (fm == nullptr) continue;
        tm->wasPlaying = fm->wasPlaying;
        tm->ranLive = fm->ranLive; tm->tailLeft = fm->tailLeft; tm->quietFor = fm->quietFor;
        auto same = [] (const Node& a, const Node& b) { return a.kind == b.kind && a.strip == b.strip && a.bus == b.bus; };
        for (size_t i = 0; i < tm->order.size(); ++i)
        {
            Node& tn = *tm->order[i];
            Node* fn = (i < fm->order.size() && same (*fm->order[i], tn)) ? fm->order[i].get() : nullptr;
            if (fn == nullptr) for (auto& x : fm->order) if (same (*x, tn)) { fn = x.get(); break; }
            if (fn == nullptr) continue;
            takeDelay (tn.srcDelay, fn->srcDelay);
            takeDelay (tn.outDelay, fn->outDelay);
            for (auto& sp : tn.sends)
                for (auto& fsp : fn->sends)
                    if (fsp.st == sp.st) { takeDelay (sp.delay, fsp.delay); break; }
        }
    }
}

static bool mixerIsLive (const MixerState& m, bool isFront, const PlaybackSession* ps, bool editMixerPlays) noexcept
{
    if (! m.editId.isNull()) return ps != nullptr && ps->editId == m.editId;
    return ! (isFront && editMixerPlays);
}

void AudioEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
{
    auto* pl = plan.load (std::memory_order_acquire);
    auto* rs = rec.load (std::memory_order_acquire);
    if (auto* prev = lastPlanSeen.load (std::memory_order_relaxed); prev != pl)        // a new plan: what is on its way through the old one carries on
    {
        if (prev != nullptr && pl != nullptr) carryOver (*prev, *pl);
        lastPlanSeen.store (pl, std::memory_order_relaxed);
    }

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

    // LTC coming in on the chosen input channel (for the lamp in the top bar)
    {
        const int lc = ltcInCh.load (std::memory_order_relaxed);
        if (lc >= 0 && lc < numIn && in[lc] != nullptr) ltcIn.feed (in[lc], numSamples); else ltcIn.feedNothing (numSamples);
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
            if (! silenceOk) rt.writer->skip (numSamples);                 // (a block far bigger than the device said: its time is kept as silence)
            else if (! rt.writer->write (ptrs, numSamples))
                rs->overruns.fetch_add (1);
            if (silenceOk) rt.peaks->add (ptrs, rt.route.numCh, numSamples);
        }
        if (! silenceOk) rs->overruns.fetch_add (1);
        rs->samples.fetch_add (numSamples);
        writerThread.notify();                                              // once for all the files
    }
    if (rs != nullptr)                                                      // how full the recording buffers are (the timing log)
    {
        float fill = 0.0f;
        for (auto& rt : rs->tracks) fill = juce::jmax (fill, rt.writer->fillPercent());
        if (fill > recBufPeakPct.load (std::memory_order_relaxed)) recBufPeakPct.store (fill, std::memory_order_relaxed);
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
    // An Edit plays through its own mixer and no other Edit's. The processing mixer rests meanwhile (it would play the same thing twice on the same outputs).
    const MixerPlan* editPlan = nullptr;
    if (pl != nullptr && ps != nullptr && ! ps->editId.isNull())
        for (auto& mp : pl->mixers) if (mp->mixer->editId == ps->editId) editPlan = mp.get();
    const bool editMixerPlays = editPlan != nullptr;
    if (pl != nullptr)
        for (int offset = 0; offset < numSamples; offset += maxBlock)
        {
            const int n = juce::jmin (maxBlock, numSamples - offset);
            if (ps != nullptr && ps->automation != nullptr) ps->automation->apply (ps->getTimelinePosition());     // an edit's automation moves the faders
            // what the plug-ins are told: playing while something plays back or records; the place on the timeline (stays put when stopped)
            transport.playing = ps != nullptr || rs != nullptr;
            transport.recording = rs != nullptr;
            if (ps != nullptr) transport.samplePos = ps->getTimelinePosition();
            else if (rs != nullptr) transport.samplePos = rs->samples.load (std::memory_order_relaxed) + offset;
            transport.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
            if (ps != nullptr) ps->pull (n);          // disk audio replaces the live inputs while playing
            const bool direct = ps != nullptr && ps->directOut >= 0;      // a finished stereo mix: straight to the outputs, the mixers do not touch it
            for (size_t i = 0; i < pl->disk.size(); ++i) pl->disk[i] = (ps != nullptr && ! direct && (int) i < ps->numTracks()) ? &ps->trackBuffer ((int) i) : nullptr;
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
            // cue mixers that follow the processing mixer: their own mix rests; each of their Ext buses plays the main output of the processing mixer
            // (or of the Edit mixer that plays instead of it), copied aside as that mixer runs
            const MixerPlan* srcPlan = editMixerPlays ? editPlan : (pl->mixers.empty() ? nullptr : pl->mixers.front().get());
            int nFollow = 0;
            const bool canFollow = srcPlan != nullptr && (size_t) numSamples <= zeros.size() && mainCap.size() >= zeros.size() * 2;
            float* const mainL = mainCap.data();
            float* const mainR = mainCap.data() + zeros.size();
            if (canFollow)
                for (auto& mp : pl->mixers)
                {
                    if (mp.get() == pl->mixers.front().get() || ! mp->mixer->editId.isNull() || ! mp->mixer->followProcessing.get()) continue;
                    if (! mixerIsLive (*mp->mixer, false, ps, editMixerPlays)) continue;
                    const bool audited = audActive && mp->mixer == aud;
                    if (! audited && ! mixerHasOutput (*mp)) continue;
                    if (nFollow < (int) pl->followers.size()) pl->followers[(size_t) nFollow++] = mp.get();
                }
            if (nFollow > 0) { juce::FloatVectorOperations::clear (mainL + offset, n); juce::FloatVectorOperations::clear (mainR + offset, n); }
            // the mixers that run in this block: an Edit mixer sleeps unless its own Edit plays, and a mixer whose Ext buses go to no driver output
            // is not run at all (nothing of it could be heard), unless it is being auditioned or feeds the talkback playback
            int nJobs = 0;
            for (auto& mp : pl->mixers)
            {
                bool following = false;
                for (int f = 0; f < nFollow; ++f) following = following || pl->followers[(size_t) f] == mp.get();
                if (following) { mp->resumeClear = true; continue; }     // (its outputs are filled below from the processing mixer's)
                if (mixerIsLive (*mp->mixer, mp.get() == pl->mixers.front().get(), ps, editMixerPlays))
                {
                    mp->ranLive = true; mp->tailNow = false; mp->tailLeft = 0;
                }
                else
                {   // it has just stopped (its edit stopped playing): it goes on a while for the tail of its reverbs
                    if (mp->ranLive) { mp->tailLeft = (int) juce::jmin (30.0 * sampleRate, 1.0e9); mp->quietFor = 0; }
                    mp->ranLive = false;
                    if (mp->tailLeft <= 0) { mp->tailNow = false; continue; }
                    mp->tailNow = true;
                }
                const bool strict = tbPlayNow && mp.get() == (editMixerPlays ? editPlan : pl->mixers.front().get());       // the processing mixer (or the playing Edit's mixer)
                const bool audited = audActive && mp->mixer == aud;
                const bool capMain = nFollow > 0 && mp.get() == srcPlan;
                if (! strict && ! audited && ! capMain && ! mixerHasOutput (*mp)) continue;
                if (nJobs >= (int) pl->jobs.size()) break;
                pl->jobs[(size_t) nJobs++] = { mp.get(), audActive && mp->mixer == eng, audited ? monitorOut : -1, strict, capMain };
            }
            const bool playingNow = ps != nullptr && ! direct;
            for (int j = 0; j < nJobs; ++j) pl->jobs[(size_t) j].mp->outPeak = 0.0f;
            // Two or more mixers: they run side by side on the helper threads, each into its own outputs, which are then added up here in the usual
            // order (so the result is exactly what running them one after another gives).
            if (nJobs >= 2 && mixerWorkers.size() > 0 && offset == 0 && n == numSamples && numOut <= pl->outChannels)
            {
                blockJobs = { pl, in, numIn, out, numOut, n, playingNow, tapL, tapR, mainL, mainR };
                mixerWorkers.run (nJobs, &AudioEngine::runMixerJob, this);
                parallelBlocks.fetch_add (1, std::memory_order_relaxed);
                for (int j = 0; j < nJobs; ++j)
                {
                    const auto& mp = *pl->jobs[(size_t) j].mp;
                    for (int k = 0; k < mp.numTouched; ++k)
                    {
                        const int ch = mp.touched[(size_t) k];
                        juce::FloatVectorOperations::add (out[ch], mp.outScratch.getReadPointer (ch), n);
                    }
                }
            }
            else
                for (int j = 0; j < nJobs; ++j)
                {
                    const auto& job = pl->jobs[(size_t) j];
                    processMixer (*pl, *job.mp, in, numIn, offset, n, out, numOut, playingNow, job.silenceOwnOutput, job.alsoToFirst,
                                  job.strict, job.strict ? tapL : nullptr, job.strict ? tapR : nullptr, job.capMain ? mainL : nullptr, job.capMain ? mainR : nullptr);
                }
            for (int f = 0; f < nFollow; ++f)                    // the following cue mixers: the main output on each of their Ext buses' driver outputs
            {
                const auto& fp = *pl->followers[(size_t) f];
                const bool audited = audActive && fp.mixer == aud;
                float pk = 0.0f;
                for (int c = 0; c < 2; ++c) { const auto mm = juce::FloatVectorOperations::findMinAndMax ((c == 0 ? mainL : mainR) + offset, n); pk = juce::jmax (pk, std::abs (mm.getStart()), std::abs (mm.getEnd())); }
                auto addPair = [&] (int first)
                {
                    for (int c = 0; c < 2; ++c)
                        if (first >= 0 && first + c < numOut && out[first + c] != nullptr)
                            juce::FloatVectorOperations::add (out[first + c] + offset, (c == 0 ? mainL : mainR) + offset, n);
                };
                for (auto& nd : fp.order)
                    if (nd->kind == NodeKind::ExtBus)
                    {
                        addPair (nd->bus->outFirst.load (std::memory_order_relaxed));
                        if (pk > nd->bus->meterL.get()) nd->bus->meterL.set (pk);
                        if (pk > nd->bus->meterR.get()) nd->bus->meterR.set (pk);
                    }
                if (audited) addPair (monitorOut);              // auditioned by the engineer: heard on the engineer's pair too
            }
            for (int j = 0; j < nJobs; ++j)                     // the tails: stop once the outputs have stayed below -90 dB for half a second (or after 30 s)
            {
                auto& mp = *pl->jobs[(size_t) j].mp;
                if (! mp.tailNow) continue;
                mp.quietFor = mp.outPeak < 3.2e-5f ? mp.quietFor + n : 0;
                mp.tailLeft -= n;
                if (mp.quietFor >= (int) (0.5 * sampleRate)) mp.tailLeft = 0;
            }
            if (direct && ps->numTracks() > 0)
            {
                const auto& mix = ps->trackBuffer (0);
                for (int c = 0; c < 2 && c < mix.getNumChannels(); ++c)
                {
                    const int ch = ps->directOut + c;
                    if (ch < numOut && out[ch] != nullptr) juce::FloatVectorOperations::add (out[ch] + offset, mix.getReadPointer (c), n);
                }
            }
        }

    // Mono check: a listening aid. The mixer's driver outputs carry (L+R)/2 on both sides; dither (below) is added after it.
    if (const auto* mc = plan.load (std::memory_order_acquire))
    {
        const int nch = juce::jmin (numOut, kMaxInputs);
        for (auto& mp : mc->mixers)
        {
            if (! mp->mixer->monoCheck.load (std::memory_order_relaxed) || ! mixerIsLive (*mp->mixer, mp.get() == mc->mixers.front().get(), ps, editMixerPlays)) continue;
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
            if (! mixerIsLive (*mp->mixer, mp.get() == dp->mixers.front().get(), ps, editMixerPlays)) continue;
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
            const float g0 = crGain;
            const float pbT = stagePb.load (std::memory_order_relaxed), outT = stageOut.load (std::memory_order_relaxed);
            const float invN = 1.0f / (float) juce::jmax (1, numSamples);
            for (int o = 0; o < numOut && o < kMaxInputs; ++o)
            {
                const int mask = tbMask[(size_t) o].load (std::memory_order_relaxed);
                if (mask == 0 || out[o] == nullptr) continue;
                float* L = out[o];
                juce::FloatVectorOperations::clear (L, numSamples);
                auto addRamp = [&] (const float* src, float a0, float a1)
                {
                    if (a0 == a1) { juce::FloatVectorOperations::addWithMultiply (L, src, a0, numSamples); return; }
                    const float st = (a1 - a0) * invN;
                    for (int i = 0; i < numSamples; ++i) L[i] += src[i] * (a0 + st * (float) (i + 1));
                };
                if (tbPlayNow)                                                   // recorded audio only (see above)
                {
                    if (mask & 1) addRamp (tapL, stagePrevPb, pbT);
                    if (mask & 2) addRamp (tapR, stagePrevPb, pbT);
                }
                if (target > 0.0f || crGain > 0.0f)
                    for (int ci = 0; ci < numIn && ci < kMaxInputs; ++ci)        // every selected input is added (the fade is the same for all)
                    {
                        if (! crSel[(size_t) ci].load (std::memory_order_relaxed) || in[ci] == nullptr) continue;
                        const float tl = stageL[(size_t) ci].load (std::memory_order_relaxed), tr = stageR[(size_t) ci].load (std::memory_order_relaxed);
                        const float a0 = g0 * (((mask & 1) ? stagePrevL[ci] : 0.0f) + ((mask & 2) ? stagePrevR[ci] : 0.0f));
                        const float a1 = target * (((mask & 1) ? tl : 0.0f) + ((mask & 2) ? tr : 0.0f));
                        addRamp (in[ci], a0, a1);
                    }
                if (outT != 1.0f || stagePrevOut != 1.0f)                        // the output fader of the stage speaker mixer
                {
                    const float st = (outT - stagePrevOut) * invN;
                    for (int i = 0; i < numSamples; ++i) L[i] *= stagePrevOut + st * (float) (i + 1);
                }
                {
                    const auto mm = juce::FloatVectorOperations::findMinAndMax (L, numSamples);
                    const float pk = juce::jmax (std::abs (mm.getStart()), std::abs (mm.getEnd()));
                    if (pk > stagePeakSeen.load (std::memory_order_relaxed)) stagePeakSeen.store (pk, std::memory_order_relaxed);
                }
            }
            for (int ci = 0; ci < kMaxInputs; ++ci) { stagePrevL[ci] = stageL[(size_t) ci].load (std::memory_order_relaxed); stagePrevR[ci] = stageR[(size_t) ci].load (std::memory_order_relaxed); }
            stagePrevPb = pbT; stagePrevOut = outT;
            crGain = target;
        }
        else crGain = 0.0f;
    }

    // LTC going out on the chosen output channel (replaces whatever was on that channel)
    {
        const int oc = ltcOutCh.load (std::memory_order_relaxed);
        if (oc >= 0 && oc < numOut && out[oc] != nullptr)
        {
            const int rq = ltcOutRestart.load (std::memory_order_relaxed);
            if (rq != ltcSeenRestart || ! ltcGen.isRunning()) { ltcSeenRestart = rq; ltcGen.start (ltcOutFormat.load (std::memory_order_relaxed), ltcOutStart.load (std::memory_order_relaxed)); }
            ltcGen.fill (out[oc], numSamples);
            ltcOutRunning.store (true, std::memory_order_relaxed);
        }
        else { if (ltcGen.isRunning()) ltcGen.stop(); ltcOutRunning.store (false, std::memory_order_relaxed); }
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

void AudioEngine::processMixer (const Plan& pl, MixerPlan& mp, const float* const* in, int numIn, int offset, int n,
                                float* const* out, int numOut, bool playing, bool silenceOwnOutput, int alsoToFirst,
                                bool playbackOnly, float* tapL, float* tapR, float* mainL, float* mainR)
{
    currentHostTransport() = &transport;                    // the plug-ins in this mixer read the transport from here (on whichever thread runs it)
    bool anySolo = false;
    const juce::uint32 vcaSolo = mp.mixer != nullptr ? mp.mixer->vcaSoloMask() : 0u;      // a soloed VCA keeps every strip it controls open
    for (auto& nd : mp.order)
    {
        const juce::uint32 vm = nd->kind == NodeKind::Track ? nd->strip->vcaMask.load (std::memory_order_relaxed) : nd->kind == NodeKind::IntBus ? nd->bus->vcaMask.load (std::memory_order_relaxed) : 0u;
        const bool me = (nd->kind == NodeKind::Track && nd->strip->solo.get()) || (nd->kind == NodeKind::IntBus && nd->bus->solo.get()) || (vm & vcaSolo) != 0;
        nd->soloHeard = me;
        anySolo = anySolo || me;
    }
    if (anySolo)                                           // whatever feeds a soloed channel (tracks, buses, through faders or sends) stays open too
        for (size_t k = mp.order.size(); k-- > 0;)
        {
            auto& nd = *mp.order[k];
            for (auto& sp : nd.sends) if (sp.dest->soloHeard) { nd.soloHeard = true; break; }
        }

    for (auto& nd : mp.order) if (nd->receives) nd->incoming.clear (0, n);
    const bool playingForDelays = playing && ! mp.tailNow;
    if (playingForDelays != mp.wasPlaying || mp.resumeClear)    // playing starts (or jumps): old audio must not come out of the delay lines. When it stops they are
    {                                                      // left alone, so what is on its way through (a reverb's input, say) still comes out: the tail
        const bool starts = playingForDelays || mp.resumeClear;     // (or the mixer rested while it followed the processing mixer: what is in them is old)
        mp.wasPlaying = playingForDelays; mp.resumeClear = false;
        if (starts)
        for (auto& nd : mp.order)
        {
            nd->srcDelay.clear(); nd->outDelay.clear(); nd->tapDelay.clear(); nd->tapDelayPre.clear();
            for (auto& sp : nd->sends) sp.delay.clear();
        }
    }

    auto* plan_ = &pl;                                  // the plan that this block was started with (a rebuild during the block must not mix two plans)
    const bool recordingNow = rec.load (std::memory_order_relaxed) != nullptr;

    for (auto& ndp : mp.order)
    {
        Node& nd = *ndp;
        float* L = mp.pre.getWritePointer (0);
        float* R = mp.pre.getWritePointer (1);
        bool audible = true, soloOk = true;           // soloOk: not silenced by someone else's solo (the mute button left out)
        float faderGain = 1.0f;
        float* lastGain = nullptr; float* lastPre = nullptr;
        Param *meterL = nullptr, *meterR = nullptr;

        if (nd.kind == NodeKind::Track)
        {
            auto* st = nd.strip;
            const auto& route = plan_->tracks[(size_t) nd.trackIndex];
            const int nc = route.numCh;

            // which sources reach this strip (see Monitor): the recording played from disk, and/or the live input
            const juce::AudioBuffer<float>* diskBuf = mp.tailNow ? nullptr : plan_->disk[(size_t) nd.trackIndex];      // (a tail: silence goes in)
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
            if (playbackOnly || mp.tailNow) liveOn = false;     // talkback playback: this pass may only contain recorded audio; a tail: nothing new goes in
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
            if (nd.srcDelay.len > 0)                            // other channels send here through plug-ins that delay them: the track's own sound waits for them
            {
                float* chp[kMaxTrackChannels];
                for (int c = 0; c < nc; ++c) chp[c] = mp.chanBuf.getWritePointer (c);
                nd.srcDelay.run (chp, nc, n);
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
            {   // the strip's input gain (automatable "gain"): before the inserts and the fader
                const float gIn = dbToGain (st->inGainDb.get());
                if (gIn != 1.0f || st->lastIn != 1.0f) { view.applyGainRamp (0, n, st->lastIn, gIn); st->lastIn = gIn; }
            }
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
            soloOk = ! anySolo || st->solo.get() || nd.soloHeard;
            audible = ! st->mute.get() && soloOk;
            faderGain = dbToGain (st->gainDb.get());
            if (const auto vm = st->vcaMask.load (std::memory_order_relaxed); vm != 0 && mp.mixer != nullptr) faderGain *= mp.mixer->vcaGain (vm);      // the VCAs of this strip add their gain
            lastGain = &st->lastGain; lastPre = &st->lastPre; meterL = &st->meterL; meterR = &st->meterR;
        }
        else   // Int Bus or Ext Bus: what is sent to it, through its inserts
        {
            auto* b = nd.bus;
            if (nd.receives) { mp.pre.copyFrom (0, 0, nd.incoming, 0, 0, n); mp.pre.copyFrom (1, 0, nd.incoming, 1, 0, n); }
            else mp.pre.clear (0, n);
            juce::AudioBuffer<float> view (mp.pre.getArrayOfWritePointers(), 2, n);
            {   // the bus's input gain (automatable "gain"): before the inserts and the fader
                const float gIn = dbToGain (b->inGainDb.get());
                if (gIn != 1.0f || b->lastIn != 1.0f) { view.applyGainRamp (0, n, b->lastIn, gIn); b->lastIn = gIn; }
            }
            for (auto& slot : b->slots) slot.process (view);
            if (const float pn = juce::jlimit (-1.0f, 1.0f, b->pan.get()); pn != 0.0f)          // balance: the side away from the pan is turned down (cos), the other stays
            {
                const float k = std::cos (std::abs (pn) * juce::MathConstants<float>::halfPi);
                juce::FloatVectorOperations::multiply (pn > 0.0f ? L : R, k, n);
            }
            audible = ! b->mute.get();
            faderGain = dbToGain (b->gainDb.get());
            if (const auto vm = b->vcaMask.load (std::memory_order_relaxed); vm != 0 && mp.mixer != nullptr) faderGain *= mp.mixer->vcaGain (vm);
            lastGain = &b->lastGain; lastPre = &b->lastPre; meterL = &b->meterL; meterR = &b->meterR;
        }

        if (nd.tapPreIdx >= 0 && captureBufs != nullptr && nd.tapPreIdx < (int) captureBufs->size())   // offline render: the same channel taken before its fader
        {
            const juce::AudioBuffer<float>* capSrc = &mp.pre;
            if (nd.tapDelayPre.len > 0)
            {
                for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, mp.pre, c, 0, n);
                float* chp[2] = { mp.sendTmp.getWritePointer (0), mp.sendTmp.getWritePointer (1) };
                nd.tapDelayPre.run (chp, 2, n);
                capSrc = &mp.sendTmp;
            }
            for (int c = 0; c < 2; ++c) (*captureBufs)[(size_t) nd.tapPreIdx].addFrom (c, 0, *capSrc, c, 0, n);
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
            if (nd.kind == NodeKind::Track) { auto& lp = c == 0 ? nd.strip->livePkL : nd.strip->livePkR; if (pk > lp.get()) lp.set (pk); }
            if (nd.kind == NodeKind::ExtBus && pk > mp.outPeak) mp.outPeak = pk;
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
        {
            const juce::AudioBuffer<float>* capSrc = &mp.post;
            if (nd.tapDelay.len > 0)
            {
                for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, mp.post, c, 0, n);
                float* chp[2] = { mp.sendTmp.getWritePointer (0), mp.sendTmp.getWritePointer (1) };
                nd.tapDelay.run (chp, 2, n);
                capSrc = &mp.sendTmp;
            }
            for (int c = 0; c < 2; ++c) (*captureBufs)[(size_t) nd.tapIdx].addFrom (c, 0, *capSrc, c, 0, n);
        }

        // sends (pre-fader ones still obey mute / solo, but not the fader; a send whose "follow mute" is off ignores the mute button, still obeys solo)
        bool needPre = false;
        for (auto& sp : nd.sends) needPre = needPre || (sp.st->pre.get() && sp.st->followMute.get() && (sp.st->active() || sp.st->lastGain > 0.0f));
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
            if (! sd->followMute.get())                           // carries on when the channel is muted: its own copy, before (1) or after (the fader) the fader
            {
                const float feed = soloOk ? (sd->pre.get() ? 1.0f : faderGain) : 0.0f;
                for (int c = 0; c < 2; ++c) mp.sendTmp.copyFromWithRamp (c, 0, mp.pre.getReadPointer (c), n, sd->lastFeed, feed);
                sd->lastFeed = feed;
                src = &mp.sendTmp;
            }
            else sd->lastFeed = audible ? (sd->pre.get() ? 1.0f : faderGain) : 0.0f;     // (so switching "follow mute" off does not jump)
            bool hasInserts = false;
            for (auto& slot : sd->slots) hasInserts = hasInserts || slot.mightBeLoaded();
            if (hasInserts)                                       // the send's own inserts: these never touch the channel's own signal
            {
                if (src != &mp.sendTmp) for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, *src, c, 0, n);
                juce::AudioBuffer<float> view (mp.sendTmp.getArrayOfWritePointers(), 2, n);
                for (auto& slot : sd->slots) slot.process (view);
                src = &mp.sendTmp;
            }
            if (sp.delay.len > 0)                                 // this path is shorter than the longest one into the same channel: hold it back
            {
                if (src != &mp.sendTmp) for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, *src, c, 0, n);
                float* chp[2] = { mp.sendTmp.getWritePointer (0), mp.sendTmp.getWritePointer (1) };
                sp.delay.run (chp, 2, n);
                src = &mp.sendTmp;
            }
            for (int c = 0; c < 2; ++c) sp.dest->incoming.addFromWithRamp (c, 0, src->getReadPointer (c), n, sd->lastGain, st1);
            sd->lastGain = st1;
        }

        // where it goes: an Ext Bus writes to its own driver outputs (nothing else ever reaches the driver)
        if (nd.kind == NodeKind::ExtBus)
        {
            const int first = nd.bus->outFirst.load (std::memory_order_relaxed);
            const juce::AudioBuffer<float>* outBuf = &mp.post;                     // (all Ext Buses of the mixer come out together: the shorter ones wait)
            if (nd.outDelay.len > 0)
            {
                for (int c = 0; c < 2; ++c) mp.sendTmp.copyFrom (c, 0, mp.post, c, 0, n);
                float* chp[2] = { mp.sendTmp.getWritePointer (0), mp.sendTmp.getWritePointer (1) };
                nd.outDelay.run (chp, 2, n);
                outBuf = &mp.sendTmp;
            }
            if (tapL != nullptr && nd.isMainOut)                                  // talkback playback: the processing mixer's main output, for the TB pair
            {
                juce::FloatVectorOperations::copy (tapL + offset, outBuf->getReadPointer (0), n);
                juce::FloatVectorOperations::copy (tapR + offset, outBuf->getReadPointer (1), n);
            }
            if (mainL != nullptr && nd.isMainOut)                                 // the main output, for the cue mixers that follow this mixer
            {
                juce::FloatVectorOperations::copy (mainL + offset, outBuf->getReadPointer (0), n);
                juce::FloatVectorOperations::copy (mainR + offset, outBuf->getReadPointer (1), n);
            }
            for (int c = 0; c < 2; ++c)
            {
                if (first >= 0 && ! silenceOwnOutput && first + c < numOut && out[first + c] != nullptr)
                    juce::FloatVectorOperations::add (out[first + c] + offset, outBuf->getReadPointer (c), n);
                const int ac = alsoToFirst + c;                          // the audited cue mixer is also heard on the engineer's output pair
                if (alsoToFirst >= 0 && ac != first + c && ac < numOut && out[ac] != nullptr)
                    juce::FloatVectorOperations::add (out[ac] + offset, outBuf->getReadPointer (c), n);
            }
        }
    }
}

// ------------------------------------------------------------------ playback
juce::String AudioEngine::startPlayback (std::unique_ptr<PlaybackSession> s)
{
    if (isRecording()) return "Stop recording before playing back";
    if (zeros.empty()) return "The audio device has not been started";
    if (! waitForFinishedTakes (15000)) return "The last take's files are still being written to the disk: try again in a moment";
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
    if (waitForAudioThread()) playSession.reset();
    else (void) playSession.release();               // the audio thread stalled: do not free what it may still be reading
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
    double bytesPerSecond = 0.0;

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

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file, 1024);     // (a small buffer: see TakeWriter)
        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) return "Cannot create " + file.getFullPathName();
        auto w = wav.createWriterFor (stream, opts);
        if (w == nullptr) return "Cannot create a 24-bit WAV writer for " + t->name;
        // the buffer holds kRecordBufferSeconds of audio whatever the sample rate (the disk may stall that long); session mode hands the pre-roll over
        // faster than real time, so it gets more room. The header is written again every 10 s, so a file cut short by a crash is readable as it is.
        const int bufFrames = (int) std::lround (kRecordBufferSeconds * sampleRate * (ringNow != nullptr ? 1.5 : 1.0));
        // written to the disk in pieces of about half a second (at most 65536 frames): few, big writes keep any drive well ahead (see TakeWriter)
        rt.writer = std::make_unique<TakeWriter> (std::move (w), writerThread, bufFrames, (int) std::lround (10.0 * sampleRate), (int) std::lround (0.5 * sampleRate));
        bytesPerSecond += sampleRate * 3.0 * (double) rt.info.numChannels;
        if (backupRoot != juce::File())                    // the second drive: the same file, in a folder named after the project (copied while it records)
        {
            const auto projFolder = project.projectFolder();
            const auto rel = (projFolder != juce::File() && file.isAChildOf (projFolder)) ? file.getRelativePathFrom (projFolder) : file.getFileName();
            const auto name = projFolder != juce::File() ? projFolder.getFileName() : project.name;
            rt.backup = backupCopier.add (file, backupRoot.getChildFile (juce::File::createLegalFileName (name)).getChildFile (rel),
                                          sampleRate * (double) rt.info.numChannels * 3.0);
        }
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
    recFolder = folder;
    recBytesPerSecond.store (bytesPerSecond, std::memory_order_relaxed);
    rec.store (session.get(), std::memory_order_release);
    project.changed();
    return {};
}

juce::Uuid AudioEngine::stopRecording()
{
    if (session == nullptr) return juce::Uuid::null();
    rec.store (nullptr, std::memory_order_release);
    const double recBytesPerSecondAtStop = recBytesPerSecond.exchange (0.0, std::memory_order_relaxed);
    if (! waitForAudioThread())
    {   // the audio callback has not moved for half a second (a plug-in hung it?): it may still be inside the session, so the session is left in
        // memory rather than freed under it. The files keep their data; their headers are mended when the project is next opened.
        const auto gid = session->groupId;
        if (auto* w = project.findTakeWindow (session->windowId))
            if (auto* g = w->findGroup (gid)) g->lengthSamples = session->samples.load();
        (void) session.release();
        project.changed();
        return gid;
    }

    if (session->useRing && session->startPos >= 0)                    // hand over whatever the audio thread had not delivered yet
    {
        deliverFromRing (*session, session->delivered, session->livePos.load());
        session->delivered = session->livePos.load();
        session->samples.store (session->delivered - session->first);
    }
    const auto length = session->samples.load();
    const auto gid = session->groupId;
    auto fin = std::make_unique<Finishing>();
    int overrunTotal = session->overruns.load(); juce::int64 lost = 0; bool anyFailed = false;
    double longestWriteMs = 0.0, firstOverrunAt = -1.0; float bufPeak = 0.0f;
    for (auto& rt : session->tracks)
    {
        if (rt.backup != nullptr) fin->copies.push_back (rt.backup);
        fin->files.push_back (rt.info.file);
        lost = juce::jmax (lost, rt.writer->samplesLost()); anyFailed = anyFailed || rt.writer->failed();
        longestWriteMs = juce::jmax (longestWriteMs, rt.writer->longestWriteMs());
        bufPeak = juce::jmax (bufPeak, rt.writer->peakFillPercent());
        if (const auto at = rt.writer->firstOverrunAt(); at >= 0 && (firstOverrunAt < 0 || at < firstOverrunAt)) firstOverrunAt = (double) at;
    }
    if (auto* w = project.findTakeWindow (session->windowId))
        if (auto* g = w->findGroup (gid))
        {
            g->lengthSamples = length;
            g->overruns = overrunTotal; g->lostSamples = lost; g->writeFailed = anyFailed;
        }
    {   // the take's line in the log: where it went and how the drive kept up
        const auto folder = recFolder;
        juce::String kind = folder.isOnCDRomDrive() ? "optical" : folder.isOnRemovableDrive() ? "removable drive" : folder.isOnHardDisk() ? "fixed drive" : "network / other";
        juce::String& r = fin->report;
        r << "Take stopped: " << juce::String (sampleRate > 0 ? (double) length / sampleRate : 0.0, 1) << " s, " << (int) session->tracks.size() << " files, "
          << juce::String (recBytesPerSecondAtStop / 1.0e6, 2) << " MB/s, to " << folder.getFullPathName() << " (" << kind << ", "
          << juce::String ((double) folder.getBytesFreeOnVolume() / 1.0e9, 1) << " GB free); longest single disk write " << juce::String (longestWriteMs, 1)
          << " ms, buffer peak " << juce::String (bufPeak, 1) << "% of " << juce::String (kRecordBufferSeconds, 0) << " s";
        if (overrunTotal > 0) r << "; " << overrunTotal << " OVERRUNS, " << juce::String (sampleRate > 0 ? (double) lost / sampleRate : 0.0, 2) << " s lost, the first at "
                               << juce::String (sampleRate > 0 ? firstOverrunAt / sampleRate : 0.0, 1) << " s";
        if (anyFailed) r << "; A FILE STOPPED TAKING DATA";
        if (backupRoot != juce::File()) r << "; second drive " << backupRoot.getFullPathName();
    }
    // The files are finished on the finisher thread: whatever is still in their buffers is written there and their headers last, so Stop never waits
    // for the disk (it used to, on this thread: with the drive behind, the program hung for seconds after Stop).
    fin->rs = std::move (session);
    auto* raw = fin.get();
    finishing.push_back (std::move (fin));
    finisher.add (raw);
    ensurePreRoll();                                  // session mode was switched off (or the patching changed) while recording
    project.changed();
    return gid;
}

void AudioEngine::Finisher::run()
{
    while (! threadShouldExit())
    {
        Finishing* f = nullptr;
        { const juce::ScopedLock sl (lock); if (! queue.empty()) { f = queue.front(); queue.erase (queue.begin()); } }
        if (f == nullptr) { wait (200); continue; }
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (auto& rt : f->rs->tracks) rt.writer.reset();           // writes what is left in the buffer, then the header (the length)
        for (auto& c : f->copies) BackupCopier::finish (c);         // now the second drive's copies can take the finished files
        juce::Logger::writeToLog (f->report + "; files finished " + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms after Stop");
        f->done.store (true, std::memory_order_release);
    }
}

bool AudioEngine::isFinishing (const juce::Uuid& groupId) const
{
    for (auto& f : finishing) if (f->rs != nullptr && f->rs->groupId == groupId && ! f->done.load (std::memory_order_acquire)) return true;
    return false;
}

bool AudioEngine::anyFinishing() const
{
    for (auto& f : finishing) if (! f->done.load (std::memory_order_acquire)) return true;
    return false;
}

bool AudioEngine::waitForFinishedTakes (int maxMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, maxMs);
    while (anyFinishing())
    {
        if (! finisher.isThreadRunning() || juce::Time::getMillisecondCounter() >= deadline) return false;
        juce::Thread::sleep (2);
    }
    return true;
}

std::vector<juce::File> AudioEngine::reapFinishedTakes()
{
    std::vector<juce::File> files;
    for (size_t i = finishing.size(); i-- > 0;)
        if (finishing[i]->done.load (std::memory_order_acquire))
        {
            files.insert (files.end(), finishing[i]->files.begin(), finishing[i]->files.end());
            finishing.erase (finishing.begin() + (std::ptrdiff_t) i);
        }
    return files;
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

juce::int64 AudioEngine::samplesLost() const
{
    juce::int64 lost = 0;
    if (auto* s = rec.load())
        for (auto& rt : s->tracks) lost = juce::jmax (lost, rt.writer->samplesLost());
    return lost;
}

juce::String AudioEngine::recordingProblem() const
{
    auto* s = rec.load (std::memory_order_acquire);
    if (s == nullptr) return {};
    int bad = 0;
    for (auto& rt : s->tracks) if (rt.writer->failed()) ++bad;
    if (bad == 0) return {};
    return juce::String (bad) + (bad == 1 ? " file has" : " files have") + " stopped taking data: the recording drive is full, or has gone";
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

bool AudioEngine::getLivePeaks (const juce::Uuid& trackId, const juce::int16*& data, int& bins, int& binSamples, int* numChannels, const juce::Uuid& groupId) const
{
    const RecordingSession* rsn = groupId.isNull() || (session != nullptr && session->groupId == groupId) ? session.get() : nullptr;
    if (rsn == nullptr && ! groupId.isNull())
        for (auto& f : finishing) if (f->rs != nullptr && f->rs->groupId == groupId) rsn = f->rs.get();     // (the finisher only touches the writers, never the peaks)
    if (rsn == nullptr) return false;
    for (auto& rt : rsn->tracks)
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

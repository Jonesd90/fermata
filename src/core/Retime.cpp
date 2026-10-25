#include "Retime.h"
#include "AudioOps.h"
#include "Dither.h"
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// Signalsmith Stretch, exactly as published (third_party/signalsmith-stretch, MIT): its own warnings are not Fermata's business
#if defined (_MSC_VER)
 #pragma warning (push, 0)
#elif defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wsign-compare"
 #pragma GCC diagnostic ignored "-Wfloat-conversion"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
 #pragma GCC diagnostic ignored "-Wcast-align"
 #pragma GCC diagnostic ignored "-Wfloat-equal"
 #pragma GCC diagnostic ignored "-Wswitch-enum"
 #pragma GCC diagnostic ignored "-Wredundant-decls"
 #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
 #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
 #pragma GCC diagnostic ignored "-Wpedantic"
 #pragma GCC diagnostic ignored "-Wdeprecated-copy"
 #pragma GCC diagnostic ignored "-Wuseless-cast"
 #pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "../../third_party/signalsmith-stretch/signalsmith-stretch.h"
#include "AudioFormats.h"

namespace td { namespace retime { namespace
{
/** The library behind a plain face (and its templates made here, inside the quiet zone). A fixed seed: the same audio always comes out the same. */
class Stretcher
{
public:
    Stretcher (int channels, float sampleRate) : s (20261024L) { s.presetDefault (channels, sampleRate); }
    int inputLatency() const  { return s.inputLatency(); }
    int outputLatency() const { return s.outputLatency(); }
    int seekLength() const    { return s.seekLength(); }
    int blockSamples() const  { return s.blockSamples(); }
    void seek (float* const* in, int n, double playbackRate) { s.seek (in, n, playbackRate); }
    void process (float* const* in, int nIn, float* const* out, int nOut) { s.process (in, nIn, out, nOut); }
private:
    signalsmith::stretch::SignalsmithStretch<float> s;
};
}}} // namespace td::retime::(anonymous)

#if defined (_MSC_VER)
 #pragma warning (pop)
#elif defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace td { namespace retime
{
namespace
{
juce::int64 llr (double x) noexcept { return (juce::int64) std::llround (x); }

juce::String clockText (juce::int64 samples, double sr)
{
    const auto ms = (juce::int64) std::llround ((double) samples / sr * 1000.0);
    return juce::String (ms / 60000) + ":" + juce::String ((ms / 1000) % 60).paddedLeft ('0', 2) + "." + juce::String (ms % 1000).paddedLeft ('0', 3);
}

/** A piece's name without what the fixes added in brackets ("Take 12 (pitch +5c)" -> "Take 12"). */
juce::String baseName (const juce::String& takeName)
{
    const auto b = takeName.upToFirstOccurrenceOf (" (", false, false).trim();
    return b.isNotEmpty() ? b : juce::String ("Piece");
}

/** The audio waiting to go into the stretcher: a window of the original timeline that only moves forward, read a good stretch at a time. */
class InputWindow
{
public:
    InputWindow (int channels, juce::int64 readAhead, const ReadFn& r) : read (r), ahead (readAhead), data ((size_t) channels), ptrs ((size_t) channels) {}
    void ensure (juce::int64 lo, juce::int64 hi)
    {
        if (lo >= start && hi <= start + len) return;
        const juce::int64 newStart = lo, newLen = juce::jmax (hi, lo + ahead) - lo;
        std::vector<std::vector<float>> next (data.size(), std::vector<float> ((size_t) newLen, 0.0f));
        const juce::int64 keepLo = juce::jmax (newStart, start), keepHi = juce::jmin (newStart + newLen, start + len);
        if (keepHi > keepLo)
            for (size_t c = 0; c < data.size(); ++c)
                std::copy (data[c].begin() + (long) (keepLo - start), data[c].begin() + (long) (keepHi - start), next[c].begin() + (long) (keepLo - newStart));
        auto fill = [&] (juce::int64 a, juce::int64 b)                             // [a, b) of the new window, from the source
        {
            for (juce::int64 pos = a; pos < b;)
            {
                const int n = (int) juce::jmin ((juce::int64) (1 << 20), b - pos);
                std::vector<std::vector<float>> part (data.size(), std::vector<float> ((size_t) n, 0.0f));
                read (pos, n, part);
                for (size_t c = 0; c < data.size(); ++c) std::copy (part[c].begin(), part[c].end(), next[c].begin() + (long) (pos - newStart));
                pos += n;
            }
        };
        if (keepHi > keepLo) { fill (newStart, keepLo); fill (keepHi, newStart + newLen); }
        else fill (newStart, newStart + newLen);
        data.swap (next); start = newStart; len = newLen;
    }
    float* const* at (juce::int64 pos)
    {
        for (size_t c = 0; c < data.size(); ++c) ptrs[c] = data[c].data() + (pos - start);
        return ptrs.data();
    }
private:
    const ReadFn& read;
    juce::int64 ahead, start = 0, len = 0;
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
};
} // namespace

// ============================================================================================== the stretch itself
juce::String stretch (int C, double sr, const TimeMap& map, juce::int64 outA, juce::int64 outB, const ReadFn& read, const WriteFn& write,
                      const std::function<bool()>& cancelled, const std::function<void (double)>& progress)
{
    if (C <= 0 || outB <= outA || sr <= 0.0) return "There is nothing to make.";
    Stretcher st (C, (float) sr);
    const int inLat = st.inputLatency(), outLat = st.outputLatency(), seekLen = st.seekLength();
    // The library's clock: the input is handed over inLat samples AHEAD of where it is working, and its output comes out outLat samples BEHIND that.
    // It starts 'margin' before the first sample wanted (with real audio before that too, through seek), so that sample already has a full history.
    // Then it is driven in short steps whose ends lie exactly on the map: each step makes 'chunk' new samples from as many original samples as the map
    // says, so the speed follows the glides and every new sample lands where the map puts it (to within half a sample).
    const juce::int64 margin = juce::jmax ((juce::int64) st.blockSamples() * 2, llr (0.25 * sr));
    const juce::int64 q0 = outA - margin;                                     // the new-timeline sample it starts working at
    const juce::int64 p0 = llr (map.unmap ((double) q0));                     // ... and the original sample that is
    InputWindow in (C, juce::jmax ((juce::int64) seekLen * 2, llr (sr)), read);
    in.ensure (p0 + inLat - seekLen, p0 + inLat);
    st.seek (in.at (p0 + inLat - seekLen), seekLen, 1.0 / juce::jmax (1.0e-6, map.rate ((double) p0)));
    constexpr int chunk = 512;
    std::vector<std::vector<float>> obuf ((size_t) C, std::vector<float> ((size_t) chunk, 0.0f)), give ((size_t) C);
    std::vector<float*> optr ((size_t) C);
    for (size_t c = 0; c < obuf.size(); ++c) optr[c] = obuf[c].data();
    const juce::int64 total = (outB - q0) + outLat;                           // samples to take out of it (the first margin + outLat are only its run-up)
    juce::int64 e = 0, pPrev = p0;
    int sinceProgress = 0;
    while (e < total)
    {
        if (cancelled && cancelled()) return "Cancelled.";
        const int n = (int) juce::jmin ((juce::int64) chunk, total - e);
        const juce::int64 pNext = juce::jmax (pPrev, llr (map.unmap ((double) (q0 + e + n))));
        in.ensure (pPrev + inLat, pNext + inLat);
        st.process (in.at (pPrev + inLat), (int) (pNext - pPrev), optr.data(), n);
        const juce::int64 at = q0 + e - outLat;                                // where obuf[0] is on the new timeline
        const juce::int64 lo = juce::jmax (at, outA), hi = juce::jmin (at + n, outB);
        if (hi > lo)
        {
            const auto off = (long) (lo - at), cnt = (long) (hi - lo);
            for (size_t c = 0; c < obuf.size(); ++c) give[c].assign (obuf[c].begin() + off, obuf[c].begin() + off + cnt);
            if (! write (give, (int) cnt, lo)) return "The retimed audio could not be written (is the disk full?).";
        }
        e += n; pPrev = pNext;
        if (progress && ++sinceProgress >= 64) { sinceProgress = 0; progress ((double) e / (double) total); }
    }
    if (progress) progress (1.0);
    return {};
}

// ============================================================================================== where the marks go
std::pair<int, int> wholeRun (const EditDef& e, const juce::Uuid& piece)
{
    const int i = e.indexOf (piece);
    if (i < 0) return { -1, -1 };
    int first = i, last = i;
    while (first > 0 && e.isUnnumberedJoin (first)) --first;
    while (last + 1 < (int) e.regions.size() && e.isUnnumberedJoin (last + 1)) ++last;
    return { first, last };
}

std::pair<juce::int64, juce::int64> snapSpan (const EditDef& e, juce::int64 from, juce::int64 to, juce::String* note)
{
    const double sr = e.sampleRate > 0 ? e.sampleRate : 48000.0;
    const juce::int64 gd = llr (kGuard * sr);
    struct Edge { juce::int64 at, lo, hi; bool join; };
    std::vector<Edge> edges;
    const int n = (int) e.regions.size();
    for (int k = 0; k <= n; ++k)
    {
        const EditRegion* A = k > 0 ? &e.regions[(size_t) k - 1] : nullptr;
        const EditRegion* B = k < n ? &e.regions[(size_t) k] : nullptr;
        if (A != nullptr && B != nullptr && B->startSample <= A->endSample())
        {
            if (e.isFixJoin (k)) continue;                                     // made by an offline fix: not an edit point (see the header)
            edges.push_back ({ B->startSample, std::min ({ A->fadeOutBegin(), B->fadeInBegin(), B->startSample }),
                               std::max ({ A->fadeOutFinish(), B->fadeInFinish(), A->endSample() }), true });
            continue;
        }
        if (A != nullptr) edges.push_back ({ A->endSample(), juce::jmin (A->fadeOutBegin(), A->endSample()), juce::jmax (A->fadeOutFinish(), A->endSample()), false });
        if (B != nullptr) edges.push_back ({ B->startSample, juce::jmin (B->fadeInBegin(), B->startSample), juce::jmax (B->fadeInFinish(), B->startSample), false });
    }
    auto snap = [&] (juce::int64 t, const char* which) -> juce::int64
    {
        const Edge* best = nullptr;
        for (auto& ed : edges)
            if (t >= ed.lo - gd && t <= ed.hi + gd && (best == nullptr || std::abs (t - ed.at) < std::abs (t - best->at))) best = &ed;
        if (best == nullptr || best->at == t) return t;
        if (note != nullptr && std::abs (best->at - t) > llr (0.0005 * sr))
            *note << (note->isEmpty() ? "" : " ") << which << " was on " << (best->join ? "the crossfade of an edit point" : "the fade at the edge of a piece")
                  << ", so it is taken as being exactly on it (" << clockText (best->at, sr) << ").";
        return best->at;
    };
    const auto a = snap (from, "Mark 1"), b = snap (to, "Mark 2");
    return { a, b };
}

// ============================================================================================== the plan
Plan makePlan (const EditDef& e0, const Request& q, const juce::File& folder, const FileLength& fileLen, const IsLtc& isLtc, const Quietest& quietest)
{
    Plan p;
    p.sampleRate = e0.sampleRate > 0 ? e0.sampleRate : 48000.0;
    p.wholePiece = q.wholePiece;
    const double sr = p.sampleRate;
    auto sm = [sr] (double s) { return llr (s * sr); };
    const juce::int64 xf = sm (kFixFade), xo = sm (kOutFade), gd = sm (kGuard);
    auto ltc = [&] (const juce::Uuid& t) { return isLtc != nullptr && isLtc (t); };
    if (e0.regions.empty()) { p.error = "The edit has no pieces."; return p; }
    if (! (q.S >= kStretchMin && q.S <= kStretchMax))
    { p.error = "That is too big a change: the speed in the middle of the part has to stay between half and twice the normal speed."; return p; }
    EditDef e = e0;                                                           // a working copy: its pieces are cut where the retiming starts and ends

    // ---- the part
    juce::int64 t0 = 0, t1 = 0;
    std::pair<int, int> run { -1, -1 };
    if (q.wholePiece)
    {
        run = wholeRun (e, q.piece);
        if (run.first < 0) { p.error = "Click the piece to retime first. (An overdub is not retimed on its own: mark it with keys 1 and 2 instead.)"; return p; }
        t0 = e.regions[(size_t) run.first].startSample; t1 = e.regions[(size_t) run.second].endSample();
    }
    else
    {
        juce::int64 endAll = 0;
        for (auto& r : e.regions) endAll = juce::jmax (endAll, r.endSample());
        for (auto& o : e.overdubs) endAll = juce::jmax (endAll, o.endSample());
        const auto s = snapSpan (e, juce::jmax ((juce::int64) 0, q.from), juce::jmin (q.to, endAll), &p.note);
        t0 = s.first; t1 = s.second;
    }
    if (t1 - t0 < sm (0.25)) { p.error = q.wholePiece ? "The piece is too short to retime." : "The part between the marks is too short (at least a quarter of a second), or there is no audio in it."; return p; }
    double rIn = q.wholePiece ? 0.0 : juce::jmax (0.0, q.rampIn * sr), rOut = q.wholePiece ? 0.0 : juce::jmax (0.0, q.rampOut * sr);
    if (rIn + rOut > (double) (t1 - t0)) { const double k = (double) (t1 - t0) / (rIn + rOut); rIn *= k; rOut *= k; }
    p.t0 = t0; p.t1 = t1;
    p.map = { (double) t0, (double) t1, q.S, rIn, rOut, false };
    p.delta = llr (p.map.map ((double) t1)) - t1;
    TimeMap pieceMap = p.map; pieceMap.uniform = q.wholePiece;                // a whole piece: its own audio (handles too) evenly
    for (auto* rs : { &e.regions, &e.overdubs })
        for (auto& r : *rs)
            if (r.endSample() > t0 && r.startSample < t1 && r.waiting.active())
            { p.error = "A piece in this part is waiting for corrected audio (Export for Processing): re-link it first (right-click its 'Waiting' block)."; return p; }

    std::set<juce::Uuid> forced, openStart, openEnd;                          // retimed although outside the part / starts / ends at a join made here
    // a piece waiting for corrected audio (Export for Processing) must be neither cut nor made again: cutting clears its waiting state, so its corrected
    // files could never be re-linked. The part can reach beyond the marks (a fix join in front of mark 1, the run-on after mark 2), so this is asked
    // before every cut and of every piece that is made again.
    const juce::String waitingWhy = "A piece in or next to this part is waiting for corrected audio (Export for Processing), and the retiming would have to "
                                    "cut it or make it again: re-link it first (right-click its 'Waiting' block).";
    auto cleanStart = [&] (const EditRegion& r, juce::int64 s) { return s - xf - gd >= r.fadeInFinish() && s - r.startSample >= xf + gd && r.fadeOutBegin() - s >= gd && r.endSample() - s >= gd; };
    auto cleanEnd = [&] (const EditRegion& r, juce::int64 s) { return s - gd >= r.fadeInFinish() && s - r.startSample >= gd && r.fadeOutBegin() - s >= xo + gd && r.endSample() - s >= xo + gd; };

    if (! q.wholePiece)
    {
        // ---- mark 1, inside a take: the retimed audio takes over just BEFORE it, through a short linear crossfade (up to mark 1 both are at the normal
        // speed, the same sound, sample for sample in step). A fix join's crossfade at the mark must be retimed on both of its sides, so the take-over
        // goes in front of it.
        juce::int64 s0 = t0;
        for (int pass = 0; pass < 8; ++pass)
        {
            bool moved = false;
            for (int k = 1; k < (int) e.regions.size(); ++k)
            {
                if (! e.isFixJoin (k)) continue;
                const auto& A = e.regions[(size_t) k - 1]; const auto& B = e.regions[(size_t) k];
                const auto lo = std::min ({ A.fadeOutBegin(), B.fadeInBegin(), B.startSample }), hi = std::max ({ A.fadeOutFinish(), B.fadeInFinish(), A.endSample() });
                if (s0 > lo - gd && s0 <= hi + gd) { s0 = lo - gd; moved = true; }
            }
            if (! moved) break;
        }
        for (int i = 0; i < (int) e.regions.size(); ++i)
        {
            const auto r = e.regions[(size_t) i];
            if (! (r.startSample < s0 && r.endSample() > s0)) continue;
            if (r.waiting.active()) { p.error = waitingWhy; return p; }
            juce::int64 s = s0;
            if (! cleanStart (r, s)) s = juce::jmin (s0, r.fadeOutBegin() - gd);      // (the mark is in its crossfade out: it is cut before that)
            if (! cleanStart (r, s)) { forced.insert (r.id); continue; }             // no room for a join: retimed whole
            const int k = e.splitRegion (i, s - r.startSample);
            if (k == i) { forced.insert (r.id); continue; }
            auto& L = e.regions[(size_t) k - 1]; auto& R = e.regions[(size_t) k];
            L.outStart = -(double) xf / sr; L.outEnd = 0.0; L.setOutCurve (FadeCurve::Linear); L.fixOut = true;
            R.inStart = -(double) xf / sr; R.inEnd = 0.0; R.setInCurve (FadeCurve::Linear); R.fixIn = true;
            e.clampFades (k - 1); e.clampFades (k);
            openStart.insert (R.id); forced.insert (R.id);
            i = k;
        }

        // ---- mark 2, inside a take: the take's audio after the mark has drifted in phase while it was stretched, so a crossfade into the untouched take
        // would mix two versions of the same sound that are no longer in step. So the retimed take runs on (at the normal speed) to its next edit point
        // when that is near; otherwise it hands back at the quietest moment of the next three seconds, with an equal-power crossfade.
        int at2 = -1;
        for (int k = 0; k < (int) e.regions.size() && at2 < 0; ++k)
            if (e.regions[(size_t) k].startSample < t1 && e.regions[(size_t) k].endSample() > t1) at2 = k;
        for (int k = 0; k + 1 < (int) e.regions.size() && at2 < 0; ++k)                // (mark 2 exactly on a fix join: the take goes on through it)
            if (e.regions[(size_t) k].endSample() == t1 && e.isFixJoin (k + 1)) at2 = k;
        for (int i = juce::jmax (0, at2); at2 >= 0 && i < (int) e.regions.size(); ++i)
        {
            int last = i;
            while (last + 1 < (int) e.regions.size() && e.isFixJoin (last + 1)) ++last;
            bool split = false;
            if (e.regions[(size_t) last].endSample() - t1 > sm (kRunOn))
                for (int j = i; j <= last && ! split; ++j)
                {
                    const auto r = e.regions[(size_t) j];
                    if (r.waiting.active()) { p.error = waitingWhy; return p; }
                    const juce::int64 lo = std::max ({ t1, r.fadeInFinish() + gd, r.startSample + gd }), hi = juce::jmin (r.fadeOutBegin() - xo - gd, lo + sm (3.0));
                    juce::int64 s = lo;
                    if (hi > lo && quietest != nullptr) s = juce::jlimit (lo, hi, quietest (r, lo, hi));
                    if (hi < lo || ! cleanEnd (r, s)) { forced.insert (r.id); continue; }
                    const int k = e.splitRegion (j, s - r.startSample);
                    if (k == j) { forced.insert (r.id); continue; }
                    auto& L = e.regions[(size_t) k - 1]; auto& R = e.regions[(size_t) k];
                    L.outStart = 0.0; L.outEnd = (double) xo / sr; L.setOutCurve (FadeCurve::EqualPower); L.fixOut = true;
                    R.inStart = 0.0; R.inEnd = (double) xo / sr; R.setInCurve (FadeCurve::EqualPower); R.fixIn = true;
                    e.clampFades (k - 1); e.clampFades (k);
                    openEnd.insert (L.id); forced.insert (L.id);
                    split = true;
                    p.note << (p.note.isEmpty() ? "" : " ") << "After mark 2 the retimed take hands back to the original at "
                           << clockText (s + p.delta, sr) << " (the quietest moment of the next 3 seconds) through a short equal-power crossfade.";
                }
            if (! split)
            {
                for (int j = i; j <= last; ++j) forced.insert (e.regions[(size_t) j].id);
                p.note << (p.note.isEmpty() ? "" : " ") << "After mark 2 the retimed take runs on at the normal speed to the end of its piece ("
                       << juce::String ((double) (e.regions[(size_t) last].endSample() - t1) / sr, 1) << " s later), so it is never crossfaded with an untouched copy of itself.";
            }
            break;
        }
    }

    // ---- what happens to each piece: stays (before the part), is made again (in it), moves on (after it)
    const int n = (int) e.regions.size();
    std::vector<int> cls ((size_t) n, 0);
    for (int i = 0; i < n; ++i)
    {
        const auto& r = e.regions[(size_t) i];
        if (q.wholePiece) cls[(size_t) i] = i < run.first ? -1 : i > run.second ? 1 : 0;      // (a neighbour that overlaps the piece is still not part of it)
        else cls[(size_t) i] = forced.count (r.id) > 0 ? 0 : r.endSample() <= t0 ? -1 : r.startSample >= t1 ? 1 : 0;
        if (cls[(size_t) i] == 0 && r.waiting.active()) { p.error = waitingWhy; return p; }
    }
    if (std::find (cls.begin(), cls.end(), 0) == cls.end())
    {
        bool anyOverdub = false;
        for (auto& o : e.overdubs) anyOverdub = anyOverdub || (o.endSample() > t0 && o.startSample < t1);
        if (! anyOverdub) { p.error = "There is no audio in this part to retime."; return p; }
    }

    std::map<juce::Uuid, juce::Uuid> newTake;
    const juce::String label = q.label.isNotEmpty() ? q.label : juce::String (q.S * 100.0, 1) + "%";
    // one stream: pieces ps (consecutive, fix-joined), or one overdub
    auto makeStream = [&] (const std::vector<const EditRegion*>& ps, bool overdub) -> bool
    {
        const auto& P1 = *ps.front(); const auto& Pk = *ps.back();
        Stream s; s.audio = overdub ? p.map : pieceMap; s.what = baseName (P1.takeName);
        // the tracks, in the order of the first piece's files
        for (auto* P : ps)
            for (auto& f : P->files)
            {
                if (ltc (f.trackId)) continue;
                auto it = std::find_if (s.tracks.begin(), s.tracks.end(), [&] (const StreamTrack& t) { return t.trackId == f.trackId; });
                if (it == s.tracks.end()) { StreamTrack t; t.trackId = f.trackId; t.trackName = f.trackName; t.numChannels = juce::jmax (1, f.numChannels); s.tracks.push_back (t); it = s.tracks.end() - 1; }
                it->numChannels = juce::jmax (it->numChannels, f.numChannels);
                Source src; src.file = f.file; src.fileStart = f.fileStart; src.start = P->startSample; src.srcIn = P->srcIn;
                src.fadeIn = P != ps.front(); src.fadeOut = P != ps.back();
                src.inB = P->fadeInBegin(); src.inE = P->fadeInFinish(); src.outB = P->fadeOutBegin(); src.outE = P->fadeOutFinish();
                src.inCurve = P->curve; src.outCurve = P->outCurveValue();
                it->sources.push_back (src);
            }
        if (s.tracks.empty()) { p.error = "\"" + s.what + "\" has no audio to retime (only timecode)."; return false; }
        // what is read: the piece and its crossfades, plus handles where it meets another take (so that edit point can still be trimmed) - as far as its files go
        auto availFrom = [&] (const EditRegion& r, bool lo, juce::int64& out) -> bool
        {
            bool any = false;
            for (auto& f : r.files)
            {
                if (ltc (f.trackId)) continue;
                const auto len = fileLen != nullptr ? fileLen (f.file) : (juce::int64) -1;
                if (len < 0) { p.error = "Cannot read " + f.file.getFullPathName(); return false; }
                const auto v0 = r.startSample + (f.fileStart - r.srcIn), v1 = v0 + len;
                out = ! any ? (lo ? v0 : v1) : (lo ? juce::jmax (out, v0) : juce::jmin (out, v1));
                any = true;
            }
            if (! any) out = lo ? r.startSample : r.endSample();
            return true;
        };
        juce::int64 aLo = 0, aHi = 0;
        if (! availFrom (P1, true, aLo) || ! availFrom (Pk, false, aHi)) return false;
        const juce::int64 needLo = juce::jmin (P1.fadeInBegin(), P1.startSample), needHi = juce::jmax (Pk.fadeOutFinish(), Pk.endSample());
        const juce::int64 hLo = openStart.count (P1.id) > 0 ? sm (0.1) : sm (q.wholePiece ? kHandleWhole : kHandleInner);
        const juce::int64 hHi = openEnd.count (Pk.id) > 0 ? sm (0.1) : sm (q.wholePiece ? kHandleWhole : kHandleInner);
        s.a = juce::jmax (needLo - hLo, juce::jmin (aLo, needLo));
        s.b = juce::jmin (needHi + hHi, juce::jmax (aHi, needHi));
        s.outA = (juce::int64) std::floor (s.audio.map ((double) s.a) + 1.0e-6);           // (the tiny margins keep rounding noise from adding a sample)
        s.outB = (juce::int64) std::ceil (s.audio.map ((double) s.b) - 1.0e-6);
        if (s.outB - s.outA < 16) { p.error = "Nothing to make for " + s.what + "."; return false; }
        for (auto& t : s.tracks)
            t.dest = audioops::uniqueFile (folder.getChildFile (sanitiseForFile (s.what + " - " + t.trackName) + " (retimed " + label + ").wav"));

        // the new piece
        EditRegion r = P1;
        auto nt = newTake.find (P1.takeId);
        if (nt == newTake.end()) nt = newTake.emplace (P1.takeId, juce::Uuid()).first;
        r.takeId = nt->second;                                                 // (its samples are no longer the take's: see the development notes)
        r.takeName = s.what + " (retimed " + label + ")";
        r.files.clear();
        for (auto& t : s.tracks) { RegionFile rf; rf.trackId = t.trackId; rf.trackName = t.trackName; rf.file = t.dest; rf.numChannels = t.numChannels; rf.fileStart = 0; r.files.push_back (rf); }
        r.sourceLength = s.outB - s.outA;
        const auto ns = p.newSampleAt (P1.startSample), ne = p.newSampleAt (Pk.endSample());
        r.startSample = ns; r.srcIn = ns - s.outA; r.srcOut = ne - s.outA;
        r.inStart = P1.inStart; r.inEnd = P1.inEnd; r.curve = P1.curve; r.fixIn = P1.fixIn; r.snipIn = P1.snipIn;
        r.outStart = Pk.outStart; r.outEnd = Pk.outEnd; r.outCurveOwn = false; r.setOutCurve (Pk.outCurveValue()); r.fixOut = Pk.fixOut; r.snipOut = Pk.snipOut;
        r.barOut = Pk.barOut; r.waiting = WaitingPiece(); r.normaliseIn = P1.normaliseIn;
        // its volume changes go where their music went (each piece's own, and where a fix-joined piece took over, its level from there)
        r.gains.clear();
        for (size_t pi = 0; pi < ps.size(); ++pi)
        {
            const auto& P = *ps[pi];
            std::vector<int> idx (r.files.size(), -1);
            for (size_t j = 0; j < r.files.size(); ++j)
                for (size_t f = 0; f < P.files.size(); ++f) if (P.files[f].trackId == r.files[j].trackId) { idx[j] = (int) f; break; }
            if (pi > 0)
            {
                const auto at = p.newSampleAt (P.fadeInBegin()) - r.startSample;
                std::vector<float> d (r.files.size(), 0.0f); bool differs = false;
                for (size_t j = 0; j < d.size(); ++j)
                {
                    d[j] = idx[j] >= 0 ? P.levelDbAt ((size_t) idx[j], 0) : 0.0f;
                    differs = differs || std::abs (d[j] - r.levelDbAt (j, at)) > 0.01f;
                }
                if (differs) r.setGainChange (juce::jmax ((juce::int64) 0, at), (double) (p.newSampleAt (P.fadeInFinish()) - p.newSampleAt (P.fadeInBegin())) / sr, d);
            }
            const auto tol = (juce::int64) (0.001 * sr);
            for (auto& g : P.gains)
            {
                if (g.autoNorm) continue;
                if (pi > 0 && g.at <= tol) continue;                               // (the level a later piece starts at: the glide over its join above already reaches it -
                                                                                   //  played, it holds from the start of that piece's fade-in, so it is no step at the join)
                const juce::int64 t = P.startSample + g.at, nt2 = p.newSampleAt (t);
                std::vector<float> d (r.files.size(), 0.0f);
                for (size_t j = 0; j < d.size(); ++j) if (idx[j] >= 0 && (size_t) idx[j] < g.db.size()) d[j] = g.db[(size_t) idx[j]];
                r.setGainChange (juce::jmax ((juce::int64) 0, nt2 - r.startSample), juce::jmax (0.0, (double) (p.newSampleAt (t + llr (g.ramp * sr)) - nt2) / sr), d);
            }
        }
        (overdub ? p.overdubs : p.regions).push_back (r);
        p.retimed.push_back (r.id);
        p.streams.push_back (std::move (s));
        return true;
    };

    for (int i = 0; i < n;)
    {
        const auto& r = e.regions[(size_t) i];
        if (cls[(size_t) i] < 0) { p.regions.push_back (r); ++i; continue; }
        if (cls[(size_t) i] > 0) { auto m = r; m.startSample += p.delta; p.regions.push_back (m); ++i; continue; }
        int j = i;
        while (j + 1 < n && cls[(size_t) j + 1] == 0 && e.isFixJoin (j + 1)) ++j;
        std::vector<const EditRegion*> ps;
        for (int k = i; k <= j; ++k) ps.push_back (&e.regions[(size_t) k]);
        if (! makeStream (ps, false)) { if (p.error.isEmpty()) p.error = "The retimed audio could not be planned."; return p; }
        i = j + 1;
    }
    for (auto& o : e.overdubs)
    {
        if (o.endSample() <= t0) p.overdubs.push_back (o);
        else if (o.startSample >= t1) { auto m = o; m.startSample += p.delta; p.overdubs.push_back (m); }
        else if (! makeStream ({ &o }, true)) { if (p.error.isEmpty()) p.error = "The retimed audio could not be planned."; return p; }
    }
    if (p.streams.empty()) { p.error = "There is no audio in this part to retime."; return p; }
    return p;
}

void applyPlan (EditDef& e, const Plan& p)
{
    juce::Uuid insertBefore;
    if (e.insertIndex >= 0 && e.insertIndex < (int) e.regions.size()) insertBefore = e.regions[(size_t) e.insertIndex].id;
    e.regions = p.regions;
    e.overdubs = p.overdubs;
    // the automation: every point goes where its music went, and is pinned again to the audio now there
    for (auto& lane : e.lanes)
    {
        for (auto& pt : lane.pts)
        {
            const bool pinned = ! pt.region.isNull();
            pt.time = p.newSampleAt (pt.time);
            if (pinned) e.anchorPoint (pt, pt.time);
        }
        lane.sortPoints();
    }
    if (! insertBefore.isNull()) e.insertIndex = e.indexOf (insertBefore);
    e.clampAll();                                                             // (marks 1 - 2 are left where they are: Undo then puts back exactly what they marked)
}

// ============================================================================================== making the files
namespace
{
juce::String renderStream (juce::AudioFormatManager& fm, const Stream& s, double sr, const std::function<bool()>& cancelled, const std::function<void (double)>& progress)
{
    struct Src { const Source* s = nullptr; std::unique_ptr<juce::AudioFormatReader> r; juce::int64 v0 = 0, v1 = 0; };
    struct Trk { const StreamTrack* t = nullptr; int base = 0; std::vector<Src> srcs; std::unique_ptr<juce::AudioFormatWriter> w; std::unique_ptr<DitherOut> d; };
    std::vector<Trk> trks;
    int C = 0;
    for (auto& t : s.tracks)
    {
        Trk tk; tk.t = &t; tk.base = C; C += t.numChannels;
        for (auto& src : t.sources)
        {
            Src x; x.s = &src; x.r = audioops::openReader (fm, src.file);
            if (x.r == nullptr) return "Cannot read " + src.file.getFullPathName();
            x.v0 = src.start + (src.fileStart - src.srcIn); x.v1 = x.v0 + x.r->lengthInSamples;
            tk.srcs.push_back (std::move (x));
        }
        tk.w = audioops::makeWavWriter (t.dest, sr, t.numChannels);
        if (tk.w == nullptr) return "Cannot write " + t.dest.getFullPathName();
        tk.d = std::make_unique<DitherOut> (*tk.w, true);
        trks.push_back (std::move (tk));
    }
    juce::AudioBuffer<float> tmp;
    ReadFn read = [&] (juce::int64 start, int n, std::vector<std::vector<float>>& out)
    {
        for (auto& tk : trks)
            for (auto& x : tk.srcs)
            {
                const juce::int64 lo = std::max ({ start, x.s->from(), x.v0 }), hi = std::min ({ start + (juce::int64) n, x.s->to(), x.v1 });
                if (hi <= lo) continue;
                const int cnt = (int) (hi - lo), nc = juce::jmax (1, (int) x.r->numChannels);
                tmp.setSize (nc, cnt, false, false, true);
                if (! x.r->read (tmp.getArrayOfWritePointers(), nc, lo - x.v0, cnt)) continue;
                const bool faded = x.s->fadeIn || x.s->fadeOut;
                for (int c = 0; c < tk.t->numChannels; ++c)
                {
                    const float* src = tmp.getReadPointer (juce::jmin (c, nc - 1));
                    float* dst = out[(size_t) (tk.base + c)].data() + (lo - start);
                    if (faded) for (int k = 0; k < cnt; ++k) dst[k] += src[k] * x.s->gainAt (lo + k);
                    else       for (int k = 0; k < cnt; ++k) dst[k] += src[k];
                }
            }
    };
    juce::int64 expect = s.outA;
    std::vector<const float*> ptrs;
    WriteFn write = [&] (const std::vector<std::vector<float>>& blk, int n, juce::int64 at)
    {
        if (at != expect) return false;                                         // (always in order: anything else would be a mistake)
        for (auto& tk : trks)
        {
            ptrs.assign ((size_t) tk.t->numChannels, nullptr);
            for (int c = 0; c < tk.t->numChannels; ++c) ptrs[(size_t) c] = blk[(size_t) (tk.base + c)].data();
            if (! tk.d->write (ptrs.data(), tk.t->numChannels, n)) return false;
        }
        expect += n;
        return true;
    };
    const auto err = stretch (C, sr, s.audio, s.outA, s.outB, read, write, cancelled, progress);
    for (auto& tk : trks) { tk.d.reset(); tk.w.reset(); }                      // (closed: a file still open cannot be deleted on Windows)
    if (err.isEmpty() && expect != s.outB) return "The retimed audio came out the wrong length, so it was not used.";
    return err;
}
} // namespace

juce::int64 quietestMoment (juce::AudioFormatManager& fm, const EditRegion& r, juce::int64 from, juce::int64 to, const IsLtc& isLtc)
{
    const double sr = r.sampleRate > 0 ? r.sampleRate : 48000.0;
    const int step = juce::jmax (16, (int) std::lround (0.010 * sr)), span = juce::jmax (1, (int) std::lround (kOutFade * sr / step));
    if (to <= from) return from;
    const int blocks = (int) ((to - from) / step) + span + 1;
    std::vector<double> energy ((size_t) blocks, 0.0);
    for (auto& f : r.files)
    {
        if (isLtc != nullptr && isLtc (f.trackId)) continue;
        auto rd = audioops::openReader (fm, f.file);
        if (rd == nullptr) continue;
        std::vector<std::vector<float>> ch;
        if (! audioops::readRange (*rd, r.srcIn + (from - r.startSample) - f.fileStart, (juce::int64) blocks * step, ch)) continue;
        for (auto& c : ch)
            for (int b = 0; b < blocks; ++b)
            {
                double e = 0.0;
                for (int k = 0; k < step; ++k) { const double v = c[(size_t) (b * step + k)]; e += v * v; }
                energy[(size_t) b] += e;
            }
    }
    int best = 0; double bestE = -1.0;
    for (int b = 0; b + span <= blocks && (juce::int64) b * step <= to - from; ++b)
    {
        double e = 0.0;
        for (int k = 0; k < span; ++k) e += energy[(size_t) (b + k)];
        if (bestE < 0.0 || e < bestE * 0.999) { bestE = e; best = b; }       // (the earliest of equally quiet places)
    }
    return from + (juce::int64) best * step;
}

juce::String renderStreams (const Plan& p, const std::function<bool()>& cancelled, const std::function<void (float)>& progress, int threads)
{
    if (! p.ok() || p.streams.empty()) return p.ok() ? juce::String ("There is nothing to make.") : p.error;
    const int ns = (int) p.streams.size();
    std::vector<double> weight ((size_t) ns, 0.0);
    double total = 0.0;
    for (int i = 0; i < ns; ++i) { weight[(size_t) i] = (double) (p.streams[(size_t) i].outB - p.streams[(size_t) i].outA) * juce::jmax (1, p.streams[(size_t) i].channels()); total += weight[(size_t) i]; }
    std::unique_ptr<std::atomic<double>[]> done (new std::atomic<double>[(size_t) ns]);
    for (int i = 0; i < ns; ++i) done[i] = 0.0;
    std::atomic<int> next { 0 };
    std::atomic<bool> failed { false };
    std::mutex lock; juce::String error;
    std::mutex progressLock;
    auto report = [&]
    {
        if (! progress) return;
        double sum = 0.0;
        for (int i = 0; i < ns; ++i) sum += done[i].load() * weight[(size_t) i];
        const std::lock_guard<std::mutex> g (progressLock);
        progress ((float) (total > 0.0 ? sum / total : 0.0));
    };
    auto stop = [&] { return failed.load() || (cancelled && cancelled()); };
    auto worker = [&]
    {
        juce::AudioFormatManager fm; td::registerAudioFormats (fm);
        for (;;)
        {
            const int i = next++;
            if (i >= ns || stop()) return;
            const auto err = renderStream (fm, p.streams[(size_t) i], p.sampleRate, stop, [&, i] (double f) { done[i] = f; report(); });
            if (err.isNotEmpty())
            {
                const std::lock_guard<std::mutex> g (lock);
                if (error.isEmpty()) error = err;
                failed = true;
                return;
            }
        }
    };
    const int nt = juce::jlimit (1, juce::jmax (1, ns), threads);
    std::vector<std::thread> pool;
    for (int t = 1; t < nt; ++t) pool.emplace_back (worker);
    worker();
    for (auto& th : pool) th.join();
    if (error.isEmpty() && cancelled && cancelled()) error = "Cancelled.";
    if (error.isNotEmpty())
        for (auto& s : p.streams) for (auto& t : s.tracks) t.dest.deleteFile();
    return error;
}
}} // namespace td::retime

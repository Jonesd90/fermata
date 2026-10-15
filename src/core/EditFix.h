#pragma once
#include "AudioOps.h"
#include "Edit.h"

namespace td
{
/** Everything one region of an edit needs for a fix, gathered on the message thread. */
struct RegionJob
{
    juce::Uuid regionId; juce::int64 a = 0, b = 0;           // the piece [a,b) of the take that becomes its own region
    juce::int64 outFrom = 0, outTo = 0, fixFrom = 0, fixTo = 0;
    std::vector<juce::File> src, dest; std::vector<juce::int64> fileStart;
    std::vector<size_t> idx;                                  // which files of the region these are (all of them, or just the tracks chosen with Alt + drag)
    std::vector<audioops::FixOp> ops;                         // the changes made to this piece, in take samples (one, or several accepted in the repair window)
    std::vector<audioops::PieceResult> results;
    double curveZero = 0.0;                                   // (pitch curve) samples from the first sample fixed to curve time 0
};


inline juce::String gApplyWhy;                                   // why the last applyRegionJob said no (shown in the message and written to the log)
inline bool applyFail (const juce::String& why) { gApplyWhy = why; juce::Logger::writeToLog ("applyRegionJob refused: " + why); return false; }

/** Message thread: cuts the region at a and b and swaps the middle piece onto the new files. */
inline bool applyRegionJob (EditDef& e, const RegionJob& j, const juce::String& suffix)
{
    int i = e.indexOf (j.regionId);
    if (i < 0) return applyFail ("the region is no longer in the edit");
    {
        const auto& r = e.regions[(size_t) i];
        if (j.a < r.srcIn || j.b > r.srcOut || j.b <= j.a) return applyFail ("the piece " + juce::String (j.a) + ".." + juce::String (j.b) + " is outside the region " + juce::String (r.srcIn) + ".." + juce::String (r.srcOut));
        if (j.results.size() != j.idx.size()) return applyFail ("the corrected files are missing (" + juce::String ((int) j.results.size()) + " of " + juce::String ((int) j.idx.size()) + ")");
        for (auto& res : j.results)
        {
            if (! res.ok) return applyFail ("a corrected file was not made");
            if (! res.file.existsAsFile()) return applyFail ("the corrected file " + res.file.getFileName() + " is missing");
            if (res.from > j.a || res.to < j.b) return applyFail ("a corrected file covers " + juce::String (res.from) + ".." + juce::String (res.to) + " but " + juce::String (j.a) + ".." + juce::String (j.b) + " is needed");
        }
    }
    // A sliver is not worth a join: the crossfade of a join is 30 ms long, and a piece much shorter than that cannot hold two of them (the gains would no longer add up).
    // A marked part shorter than that is left as it was; a stretch of the region left over before or after the marked part that is shorter than that is taken into the piece
    // (when the corrected file covers it).
    juce::int64 pa = j.a, pb = j.b;
    {
        const auto& r = e.regions[(size_t) i];
        const auto minLen = (juce::int64) std::llround (0.06 * r.sampleRate);
        if (pb - pa < minLen && ! (pa <= r.srcIn && pb >= r.srcOut)) return true;
        auto covers = [&] (juce::int64 x, juce::int64 y) { for (auto& res : j.results) if (res.from > x || res.to < y) return false; return true; };
        if (pa > r.srcIn && pa - r.srcIn < minLen && covers (r.srcIn, pb)) pa = r.srcIn;
        if (pb < r.srcOut && r.srcOut - pb < minLen && covers (pa, r.srcOut)) pb = r.srcOut;
    }
    const bool cutAfter = pb < e.regions[(size_t) i].srcOut, cutBefore = pa > e.regions[(size_t) i].srcIn;
    if (cutAfter)  e.splitRegion (i, pb - e.regions[(size_t) i].srcIn);
    if (cutBefore) i = e.splitRegion (i, pa - e.regions[(size_t) i].srcIn);
    auto& mid = e.regions[(size_t) i];
    // The new piece and the old audio around it are almost the same sound, so the crossfades where they meet are LINEAR:
    // the two gains then always add up to exactly 1 and there is no bump (an equal-power fade would add about 3 dB in the middle).
    // ONLY the joins made here get the linear fade. A fade at the original edge of the region (a real edit point) keeps the curve the user chose.
    if (cutBefore) { mid.setInCurve (FadeCurve::Linear); if (i > 0) e.regions[(size_t) i - 1].setOutCurve (FadeCurve::Linear); }
    if (cutAfter)  { mid.setOutCurve (FadeCurve::Linear); if (i + 1 < (int) e.regions.size()) e.regions[(size_t) i + 1].setInCurve (FadeCurve::Linear); }
    // the joins made here are only there to put the fixed audio in: they are not edit points (no number, skipped by next / previous fade in the Trim window)
    if (cutBefore) { mid.fixIn = true; if (i > 0) e.regions[(size_t) i - 1].fixOut = true; }
    if (cutAfter)  { mid.fixOut = true; if (i + 1 < (int) e.regions.size()) e.regions[(size_t) i + 1].fixIn = true; }
    for (size_t k = 0; k < j.idx.size() && j.idx[k] < mid.files.size(); ++k) { mid.files[j.idx[k]].file = j.results[k].file; mid.files[j.idx[k]].fileStart = j.results[k].from; }
    {   // the corrected files may start / end exactly at the piece: the crossfades of the joins made here must lie where there is audio
        juce::int64 lo = (std::numeric_limits<juce::int64>::min)(), hi = (std::numeric_limits<juce::int64>::max)();
        for (auto& res : j.results) { lo = juce::jmax (lo, res.from); hi = juce::jmin (hi, res.to); }
        e.keepFixFadesInside (i, lo, hi);
    }
    mid.takeName = mid.takeName.upToFirstOccurrenceOf (" (", false, false) + " (" + suffix + ")";
    return true;
}

/** Puts the corrected pieces of 'jobs' into the edit 'e' (whose regions are 'base' when this is called) and gives every REAL edit point of the original regions back its
    fades exactly as they were: the first piece of each original region has its fade-in, the last piece its fade-out, with the curves the user chose. Only the joins
    made here (the cuts at the ends of the marked parts) are new: they are short, linear, and not counted as edit points. Returns false (and puts 'base' back) if
    anything does not fit, or if the edit would not come out exactly as long as it went in with every region where it was. */
inline bool applyRegionJobs (EditDef& e, const std::vector<EditRegion>& base, const std::vector<RegionJob>& jobs, const juce::String& suffix)
{
    e.regions = base; gApplyWhy = {};
    bool ok = true;
    for (size_t i = jobs.size(); i-- > 0;) ok = applyRegionJob (e, jobs[i], suffix) && ok;
    if (ok)
        for (size_t j = 0; j < base.size(); ++j)
        {
            const auto& ob = base[j];
            const int first = e.indexOf (ob.id);
            if (first < 0) continue;
            int last = (int) e.regions.size() - 1;
            if (j + 1 < base.size()) { const int nx = e.indexOf (base[j + 1].id); if (nx < 0) { ok = false; gApplyWhy = "a region went missing"; break; } last = nx - 1; }
            if (last < first) continue;
            auto& f = e.regions[(size_t) first]; auto& l = e.regions[(size_t) last];
            // the start of the original region: its own fade-in and the curve of it (the fade-out keeps the curve it has now)
            const auto fOut = f.outCurveValue();
            f.inStart = ob.inStart; f.inEnd = ob.inEnd; f.fixIn = ob.fixIn;
            f.curve = ob.curve; f.setOutCurve (first == last ? ob.outCurveValue() : fOut);
            // the end of the original region: its own fade-out and the curve of it
            if (last != first) { l.outStart = ob.outStart; l.outEnd = ob.outEnd; l.fixOut = ob.fixOut; l.setOutCurve (ob.outCurveValue()); }
            else { l.outStart = ob.outStart; l.outEnd = ob.outEnd; l.fixOut = ob.fixOut; }
        }
    auto span = [] (const std::vector<EditRegion>& v) { juce::int64 sum = 0, lo = (std::numeric_limits<juce::int64>::max)(), hi = 0; for (auto& r : v) { sum += r.length(); lo = juce::jmin (lo, r.startSample); hi = juce::jmax (hi, r.endSample()); } return std::array<juce::int64, 3> { sum, lo, hi }; };
    if (ok && span (e.regions) != span (base)) { ok = false; gApplyWhy = "the edit would have changed length"; juce::Logger::writeToLog ("applyRegionJobs: refused, the edit would not be the same length"); }
    if (! ok) e.regions = base;
    return ok;
}
} // namespace td

#include "AafExport.h"
#include "VideoExport.h"
#include "Ltc.h"
#include <map>
#include <algorithm>

namespace td::aafexport
{
namespace
{
juce::String channelSuffix (int channel, int channels)
{
    if (channels <= 1) return {};
    if (channels == 2) return channel == 0 ? " L" : " R";
    return " " + juce::String (channel + 1);
}
} // namespace

Plan planFor (const Project& project, const EditDef& e, const Settings& st)
{
    Plan plan;
    if (e.isEmpty()) { plan.error = "The edit is empty."; return plan; }
    if (e.sampleRate <= 0.0) { plan.error = "The edit has no sample rate."; return plan; }
    auto& doc = plan.doc;
    doc.name = e.name; doc.sampleRate = e.sampleRate; doc.timelineStartSeconds = st.timelineStartSeconds;
    const juce::int64 origin = e.audibleStart();     // the first audible sample is the timeline's start (as the AAF reader counts too)
    doc.length = e.audibleEnd() - origin;
    if (doc.length <= 0) { plan.error = "The edit has no audible length."; return plan; }

    std::vector<juce::Uuid> ltcTracks;
    for (auto& t : project.tracks) if (t.isLtc) ltcTracks.push_back (t.id);
    plan.noLtcTrack = ltcTracks.empty();
    auto isLtc = [&] (const juce::Uuid& id) { return std::find (ltcTracks.begin(), ltcTracks.end(), id) != ltcTracks.end(); };

    // the files, each once, named "<take> (<n>) <track>" (n: the file's place among the take's files, as Pyramix numbers them)
    std::map<juce::String, size_t> fileIndex;
    auto fileFor = [&] (const EditRegion& r, const RegionFile& f, int nth) -> size_t
    {
        const auto key = f.file.getFullPathName();
        auto it = fileIndex.find (key);
        if (it != fileIndex.end()) return it->second;
        aafwrite::SourceFile sf;
        sf.file = f.file; sf.name = r.takeName + " (" + juce::String (nth) + ") " + f.trackName; sf.channels = juce::jmax (1, f.numChannels); sf.sampleRate = e.sampleRate;
        Plan::FileInfo info; info.takeName = r.takeName;
        for (auto& g : r.files)
            if (isLtc (g.trackId)) { info.ltcFile = g.file; info.ltcSample = f.fileStart - g.fileStart; break; }     // the take sample at this file's start, in the LTC file's own samples
        doc.files.push_back (sf); plan.files.push_back (info);
        fileIndex[key] = doc.files.size() - 1;
        return doc.files.size() - 1;
    };

    // the tracks of the timeline: one per project track and channel, in the project's order; the overdub layer on tracks of its own
    struct Lane { juce::Uuid trackId; int channel; bool overdub; size_t track; };
    std::vector<Lane> lanes;
    auto laneFor = [&] (const juce::Uuid& trackId, const juce::String& trackName, int channel, int channels, bool overdub) -> aafwrite::Track&
    {
        for (auto& l : lanes) if (l.trackId == trackId && l.channel == channel && l.overdub == overdub) return doc.tracks[l.track];
        aafwrite::Track t; t.name = trackName + channelSuffix (channel, channels) + (overdub ? " OD" : "");
        doc.tracks.push_back (t);
        lanes.push_back ({ trackId, channel, overdub, doc.tracks.size() - 1 });
        return doc.tracks.back();
    };
    // (the lanes come out in the order the project lists its tracks: made first, empty, then filled)
    for (auto& t : project.tracks)
    {
        if (t.isLtc && ! st.includeLtcTracks) continue;
        int channels = 1;
        for (auto& r : e.regions) for (auto& f : r.files) if (f.trackId == t.id) channels = juce::jmax (channels, f.numChannels);
        for (int ch = 0; ch < channels; ++ch) laneFor (t.id, t.name, ch, channels, false);
    }

    auto addRegion = [&] (const EditRegion& r, bool overdub)
    {
        int nth = 0;
        for (auto& f : r.files)
        {
            ++nth;
            if (isLtc (f.trackId) && ! st.includeLtcTracks) continue;
            if (f.file == juce::File()) continue;
            const auto fi = fileFor (r, f, nth);
            const int channels = juce::jmax (1, f.numChannels);
            for (int ch = 0; ch < channels; ++ch)
            {
                aafwrite::Clip c;
                c.file = fi; c.channel = ch;
                c.at = r.fadeInBegin() - origin;
                c.length = juce::jmax (r.fadeOutFinish(), r.endSample()) - r.fadeInBegin();
                c.sourceStart = r.srcIn + r.samples (r.inStart) - f.fileStart;
                c.fadeIn = r.samples (r.inEnd - r.inStart);
                c.fadeOut = r.samples (r.outEnd - r.outStart);
                c.cutPoint = r.samples (-r.inStart);
                laneFor (f.trackId, f.trackName, ch, channels, overdub).clips.push_back (c);
                ++plan.clips;
            }
        }
    };
    for (auto& r : e.regions) addRegion (r, false);
    if (st.includeOverdubs) for (auto& r : e.overdubs) addRegion (r, true);
    for (auto& t : doc.tracks) std::sort (t.clips.begin(), t.clips.end(), [] (const aafwrite::Clip& a, const aafwrite::Clip& b) { return a.at < b.at; });
    if (doc.files.empty()) plan.error = "The edit has no audio files to link.";
    return plan;
}

Result exportAaf (Plan& plan, const Settings& st, juce::AudioFormatManager& fm, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    Result out;
    out.file = st.file;
    if (plan.error.isNotEmpty()) { out.error = plan.error; return out; }
    auto& doc = plan.doc;
    if (plan.noLtcTrack) out.notes.add ("The project has no LTC track (Project Designer: tick the track that records the timecode), so no file carries a source timecode.");
    std::map<int, int> rates;
    for (size_t i = 0; i < doc.files.size(); ++i)
    {
        if (cancel != nullptr && cancel->load()) { out.error = "Cancelled."; return out; }
        auto& f = doc.files[i]; auto& info = plan.files[i];
        // the file itself: length, channels and bits from its header
        if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (f.file) })
        {
            f.lengthSamples = reader->lengthInSamples; f.channels = (int) reader->numChannels; f.bits = (int) reader->bitsPerSample;
            if (f.bits <= 0) f.bits = 24;
            if (std::abs (reader->sampleRate - doc.sampleRate) > 0.5) out.notes.add (f.file.getFileName() + " is at " + juce::String (reader->sampleRate, 0) + " Hz, not the edit's " + juce::String (doc.sampleRate, 0) + ": its positions will be off in the receiving program.");
            if (! f.file.hasFileExtension ("wav;wave;bwf;rf64")) out.notes.add (f.file.getFileName() + " is not a WAV file: it is linked all the same, but not every program will take it.");
        }
        else { ++out.unreadable; out.notes.add (f.file.getFileName() + " could not be read (missing?): linked by name only, with no length."); }
        // its source timecode: the code at its first sample
        videoexport::CodeAt c;
        if (info.ltcFile == juce::File()) c.why = plan.noLtcTrack ? juce::String ("no LTC track") : juce::String ("this take has no file on the LTC track");
        else c = videoexport::timecodeAt (fm, info.ltcFile, 0, info.ltcSample);
        if (c.found) { f.startSeconds = c.seconds; ++rates[videoexport::formatCodeFor (c.fps, c.drop)]; }
        else { ++out.withoutCode; if (! plan.noLtcTrack) out.notes.add (f.name + ": " + c.why + " - written with no source timecode (00:00:00:00)."); }
        if (progress != nullptr) progress->store (0.8f * (float) (i + 1) / (float) juce::jmax ((size_t) 1, doc.files.size()));
    }
    {   // the frame rate of the timecode tracks: the one most files have
        int best = -1, bestN = 0;
        for (auto& kv : rates) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
        if (best >= 0) { double fps; int nominal; bool drop; LtcGenerator::formatInfo (best, fps, nominal, drop); doc.fps = fps; doc.drop = drop; }
        if (rates.size() > 1) out.notes.add ("The takes carry timecode at more than one frame rate; the AAF's timecode tracks use the one most files have (" + fpsName (doc.fps, doc.drop) + ").");
    }
    out.fps = doc.fps; out.drop = doc.drop;
    if (cancel != nullptr && cancel->load()) { out.error = "Cancelled."; return out; }
    if (! st.file.getParentDirectory().createDirectory()) { out.error = "Cannot create the folder " + st.file.getParentDirectory().getFullPathName(); return out; }
    out.error = aafwrite::writeAaf (doc, st.file);
    if (progress != nullptr) progress->store (1.0f);
    out.files = (int) doc.files.size(); out.clips = plan.clips; out.tracks = (int) doc.tracks.size();
    return out;
}
} // namespace td::aafexport

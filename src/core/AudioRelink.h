#pragma once
#include "Common.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include <set>
#include <algorithm>

namespace td::relink
{
/** Finding the audio files a project has lost (moved, renamed, copied elsewhere) in a folder the user points at.

    A file is found by its NAME first (anywhere in the folder or its sub-folders) - and, when the project knows its length, sample rate and
    channels, only when the file of that name has them too: Fermata's file names repeat from project to project ("001 - Piece 1 - Violin.wav"), so
    the same name in another project's folder is not the same file. A file that was renamed is found by what the project knows about it - its
    length in samples, sample rate, number of channels and (for Fermata's own recordings) bit depth - and only when ONE file in the folder fits.
    The files of one take all have the same length, rate and channels, so when several fit, the one meant is picked by what Fermata wrote into
    its header when it recorded it (the window, take and track names in the BWF description, the date and time), or failing that by the track
    name in its file name. A file that says nothing (no header: exported by another program) is never ruled out by that silence - only a file
    whose header says it is another take or track is. When that still leaves more than one, nothing is linked and the user is told which files
    were in the running. A file the project uses already (one that is there) is never linked to a different missing file. */

/** A file the project is missing, and what it knows about it. */
struct Wanted
{
    juce::File   file;                     // where the project last saw it
    double       sampleRate = 0.0;         // 0 = not known
    int          numChannels = 0;          // 0 = not known
    juce::int64  lengthSamples = 0;        // 0 = not known: then only a file of the same name is accepted
    int          bitsPerSample = 0;        // 0 = not known (24 for a file Fermata recorded)
    juce::String trackName;                // the track it belongs to (tells the files of one take apart)
    juce::String takeTag;                  // the take's number as it is written in file names and headers ("003"), or empty
    juce::String windowName;               // the take window it was recorded in (the first thing in the header Fermata writes), or empty
    juce::Time   recordedAt;               // when it was recorded, if known (a time of 0 = not known)
    bool         ownRecording = false;     // recorded by Fermata into this project: its header (take, track, date and time) is then Fermata's own and can be trusted
                                           // to say what the file is NOT (an imported file's header is another program's, or another project's take numbers)
};

/** One of the audio files in the folder, with its header read when needed. */
struct Candidate
{
    juce::File file;
    juce::int64 size = 0;
    bool uncompressed = false;                             // a WAV / AIFF: at least as big as its samples (a FLAC or MP3 can be any size)
    bool headerRead = false, headerOk = false;
    double rate = 0.0; int channels = 0, bits = 0; juce::int64 length = 0;
    juce::String description, originator, date, time;     // BWF (WAV) metadata, if any
    bool claimed = false;                                  // linked to a missing file already, or a file the project uses as it is

    void readHeader (juce::AudioFormatManager& formats)
    {
        if (headerRead) return;
        headerRead = true;
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (file));
        if (r == nullptr) return;
        headerOk = true;
        rate = r->sampleRate; channels = (int) r->numChannels; bits = (int) r->bitsPerSample; length = r->lengthInSamples;
        description = r->metadataValues.getValue (juce::WavAudioFormat::bwavDescription, {});
        originator  = r->metadataValues.getValue (juce::WavAudioFormat::bwavOriginator, {});
        date        = r->metadataValues.getValue (juce::WavAudioFormat::bwavOriginationDate, {});
        time        = r->metadataValues.getValue (juce::WavAudioFormat::bwavOriginationTime, {});
    }
    /** The smallest and largest size a WAV / AIFF holding 'w' can have (its samples; a header and its chunks are small). */
    static void sizeRange (const Wanted& w, juce::int64& least, juce::int64& most)
    {
        const juce::int64 frames = w.lengthSamples * (juce::int64) w.numChannels;
        const int lo = w.bitsPerSample > 0 ? w.bitsPerSample : 8, hi = w.bitsPerSample > 0 ? w.bitsPerSample : 64;
        least = frames * (lo / 8); most = frames * (hi / 8) + 1024 * 1024;        // (up to a megabyte of other chunks: cue lists, markers, a picture)
    }
    /** Without opening it: could a file this size hold 'w' at all? Compressed files are never ruled out this way. */
    bool sizeCouldFit (const Wanted& w) const
    {
        if (w.lengthSamples <= 0 || w.numChannels <= 0 || ! uncompressed) return true;
        juce::int64 least, most; sizeRange (w, least, most);
        return size >= least && size <= most;
    }
    /** Opens it (once) and tells whether it has the length, rate, channels and (when known) bit depth of 'w'. */
    bool fingerprintMatches (const Wanted& w, juce::AudioFormatManager& formats)
    {
        readHeader (formats);
        if (! headerOk) return false;
        if (std::abs (rate - w.sampleRate) > 0.5 || channels != w.numChannels || length != w.lengthSamples) return false;
        return w.bitsPerSample <= 0 || bits == w.bitsPerSample;
    }
};

struct Result
{
    juce::File found;                      // empty: not found
    juce::String how;                      // "the same name", "same length, rate, 24 bit, 1 channel; the take and track in its header", ...
    juce::StringArray alternatives;        // found empty and these not: several files fit and nothing told them apart (their paths in the folder)
};

inline juce::String describe (const Wanted& w)
{
    juce::StringArray s;
    if (w.lengthSamples > 0) s.add ("same length (" + juce::String (w.lengthSamples) + " samples)");
    if (w.sampleRate > 0.0) s.add (juce::String (juce::roundToInt (w.sampleRate)) + " Hz");
    if (w.bitsPerSample > 0) s.add (juce::String (w.bitsPerSample) + " bit");
    if (w.numChannels > 0) s.add (w.numChannels == 1 ? juce::String ("1 channel") : juce::String (w.numChannels) + " channels");
    return s.joinIntoString (", ");
}

/** Whether the project knows enough about 'w' to recognise it without its name. */
inline bool knowsFingerprint (const Wanted& w) { return w.lengthSamples > 0 && w.sampleRate > 0.0 && w.numChannels > 0; }

/** Every audio file in the folder and its sub-folders. */
inline std::vector<Candidate> listFolder (const juce::File& folder)
{
    std::vector<Candidate> out;
    for (auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.wav;*.wave;*.bwf;*.w64;*.rf64;*.flac;*.aif;*.aiff;*.aifc;*.caf;*.ogg;*.mp3"))
    {
        Candidate c; c.file = f; c.size = f.getSize();
        c.uncompressed = f.hasFileExtension ("wav;wave;bwf;w64;rf64;aif;aiff;aifc");          // (a CAF may hold AAC or ALAC: not ruled out by size)
        out.push_back (std::move (c));
    }
    return out;
}

/** The seconds between a BWF origination date + time and 't' (a huge number when either is not known). */
inline double secondsFrom (const juce::String& date, const juce::String& time, const juce::Time& t)
{
    if (date.length() < 10 || time.length() < 8 || t.toMilliseconds() == 0) return 1.0e12;
    const int y = date.substring (0, 4).getIntValue(), mo = date.substring (5, 7).getIntValue(), d = date.substring (8, 10).getIntValue();
    const int h = time.substring (0, 2).getIntValue(), mi = time.substring (3, 5).getIntValue(), s = time.substring (6, 8).getIntValue();
    if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 1.0e12;
    const juce::Time at (y, mo - 1, d, h, mi, s, 0, true);
    return std::abs ((double) (at.toMilliseconds() - t.toMilliseconds())) / 1000.0;
}

/** Whether a BWF description is one Fermata wrote when recording: "<window> / <take number> - <label> / <track>". Only such a header can say
    that a file is NOT the one looked for; a header another program wrote (or none) says nothing either way. */
inline bool looksLikeFermataHeader (const juce::String& d)
{
    const int i = d.indexOf (" / ");
    if (i < 0) return false;
    const auto rest = d.substring (i + 3);
    return rest.length() > 6 && rest.substring (0, 3).containsOnly ("0123456789") && rest.substring (3).startsWith (" - ") && rest.indexOf (" / ") > 0;
}

/** Whether 'word' is in 'text' as a whole word (not "Sop" inside "Soprano"), ignoring case. */
inline bool hasWholeWord (const juce::String& text, const juce::String& word)
{
    if (word.isEmpty()) return false;
    for (int i = text.indexOfIgnoreCase (word); i >= 0; i = text.indexOfIgnoreCase (i + 1, word))
    {
        const bool before = i == 0 || ! juce::CharacterFunctions::isLetterOrDigit (text[i - 1]);
        const bool after  = i + word.length() >= text.length() || ! juce::CharacterFunctions::isLetterOrDigit (text[i + word.length()]);
        if (before && after) return true;
    }
    return false;
}

/** Whether a file name (without its extension) names the track: the track's name is in it as whole words, and no longer track name of the
    project that contains it is ("003 - Kyrie - Violin 2.wav" is Violin 2's file, not Violin's; "Main L" is not "Main"). */
inline bool fileNameNamesTrack (const juce::String& nameNoExt, const juce::String& track, const juce::StringArray& allTracks)
{
    const auto t = track.trim();
    if (t.isEmpty()) return false;
    const auto san = sanitiseForFile (t);
    if (! hasWholeWord (nameNoExt, san)) return false;
    for (auto& other : allTracks)
    {
        const auto o = other.trim();
        if (o.isEmpty() || o.equalsIgnoreCase (t)) continue;
        const auto so = sanitiseForFile (o);
        if (so.length() > san.length() && so.containsIgnoreCase (san) && hasWholeWord (nameNoExt, so)) return false;
    }
    return true;
}

/** Looks for every wanted file in 'folder'. 'progress' (optional) is told what is being looked at and how far it has got (0..1).
    'inUse' are the audio files the project uses as they are (they are never linked to anything); 'trackNames' every track name the project
    knows (so "Violin" is not taken for "Violin 2"). Results are in the order of 'wanted'. Two wanted files never get the same file. */
inline std::vector<Result> find (const std::vector<Wanted>& wanted, const juce::File& folder, juce::AudioFormatManager& formats,
                                 const std::function<void (const juce::String&, double)>& progress = {},
                                 const std::set<juce::File>& inUse = {}, const juce::StringArray& trackNames = {})
{
    std::vector<Result> results (wanted.size());
    auto report = [&] (const juce::String& what, double p) { if (progress) progress (what, p); };
    report ("Listing the audio files in " + folder.getFileName(), -1.0);
    auto files = listFolder (folder);
    for (auto& c : files) if (inUse.count (c.file) > 0) c.claimed = true;             // (a file the project has is not the one it is missing)
    std::map<juce::String, std::vector<size_t>> byName;                               // lower-case file name -> the candidates called so
    std::vector<size_t> bySize, compressed;                                           // the WAVs / AIFFs sorted by size; the others (any size can fit)
    for (size_t i = 0; i < files.size(); ++i)
    {
        byName[files[i].file.getFileName().toLowerCase()].push_back (i);
        (files[i].uncompressed ? bySize : compressed).push_back (i);
    }
    std::sort (bySize.begin(), bySize.end(), [&] (size_t a, size_t b) { return files[a].size < files[b].size; });
    int headersRead = 0;
    auto readHeader = [&] (Candidate& c, double p)
    {
        if (c.headerRead) return;
        c.readHeader (formats);
        if ((++headersRead % 8) == 0) report ("Reading the files (" + juce::String (headersRead) + " of " + juce::String ((int) files.size()) + " looked into)", p);
    };
    auto relative = [&] (const Candidate& c) { return c.file.getRelativePathFrom (folder); };
    auto inOldFolder = [] (const Candidate& c, const Wanted& w) { return c.file.getParentDirectory().getFileName().equalsIgnoreCase (w.file.getParentDirectory().getFileName()); };

    // 1. by name. Several files of that name: the one in a folder named like the old one; a file whose length, rate or channels are not the
    //    project's is not it, whatever it is called. When nothing tells same-named files apart, nothing is taken (and they are listed).
    for (size_t k = 0; k < wanted.size(); ++k)
    {
        const auto& w = wanted[k];
        auto it = byName.find (w.file.getFileName().toLowerCase());
        if (it == byName.end()) continue;
        std::vector<size_t> same;
        for (auto i : it->second) if (! files[i].claimed && files[i].sizeCouldFit (w)) same.push_back (i);
        if (knowsFingerprint (w))
        {
            std::vector<size_t> fitting;
            for (auto i : same) if (files[i].fingerprintMatches (w, formats)) fitting.push_back (i);
            same = fitting;
        }
        if (same.size() > 1)
        {
            std::vector<size_t> here;
            for (auto i : same) if (inOldFolder (files[i], w)) here.push_back (i);
            if (! here.empty()) same = here;
        }
        if (same.empty()) continue;
        if (same.size() == 1)
        {
            files[same[0]].claimed = true;
            results[k].found = files[same[0]].file;
            results[k].how = knowsFingerprint (w) ? "the same name, length, rate and channels" : "the same name";
        }
        else
            for (auto i : same) results[k].alternatives.add (relative (files[i]));
    }

    // 2. renamed files: by what the project knows about them. In rounds: a file that fits two missing files is given to the one whose header
    //    (or track name) picks it, and the other missing file then has one candidate fewer - until a round links nothing more.
    size_t todo = 0;
    for (size_t k = 0; k < wanted.size(); ++k) if (results[k].found == juce::File() && knowsFingerprint (wanted[k])) ++todo;
    size_t done = 0;
    enum class Says { Yes, No, Nothing };
    for (bool again = todo > 0; again;)
    {
    again = false;
    for (size_t k = 0; k < wanted.size(); ++k)
    {
        const auto& w = wanted[k];
        if (results[k].found != juce::File() || ! knowsFingerprint (w)) continue;
        const double p = (double) juce::jmin (done, todo) / (double) juce::jmax ((size_t) 1, todo);
        if (done < todo) report ("Looking for " + w.file.getFileName() + " (" + juce::String ((int) done + 1) + " of " + juce::String ((int) todo) + ")", p);
        ++done;
        results[k].alternatives.clear();
        std::vector<size_t> fit;
        auto consider = [&] (size_t i)
        {
            auto& c = files[i];
            if (c.claimed || ! c.sizeCouldFit (w)) return;
            readHeader (c, p);
            if (c.fingerprintMatches (w, formats)) fit.push_back (i);
        };
        {   // only the files whose size can hold it (plus every compressed file)
            juce::int64 least, most; Candidate::sizeRange (w, least, most);
            auto lo = std::lower_bound (bySize.begin(), bySize.end(), least, [&] (size_t i, juce::int64 v) { return files[i].size < v; });
            auto hi = std::upper_bound (bySize.begin(), bySize.end(), most,  [&] (juce::int64 v, size_t i) { return v < files[i].size; });
            for (auto it = lo; it != hi; ++it) consider (*it);
            for (auto i : compressed) consider (i);
        }
        if (fit.empty()) continue;
        juce::String how = describe (w);
        if (fit.size() > 1)
        {
            // The files of one take all fit. What narrows it down, in turn: the header Fermata wrote (window / take / track), its date and time, the
            // track name in the file name, the take number in the file name. A file that SAYS it is the one wins; otherwise the files that say they
            // are another one drop out, and a file that says nothing (no header: exported by another program) stays in the running.
            auto narrow = [&] (const std::function<Says (const Candidate&)>& test, const juce::String& whyYes, const juce::String& whyNo)
            {
                std::vector<size_t> yes, rest;
                for (auto i : fit) { const auto s = test (files[i]); if (s == Says::Yes) yes.push_back (i); if (s != Says::No) rest.push_back (i); }
                if (! yes.empty()) { if (yes.size() < fit.size()) { fit = yes; how += "; " + whyYes; } }
                else if (! rest.empty() && rest.size() < fit.size()) { fit = rest; how += "; " + whyNo; }
            };
            // (only for Fermata's own recordings: an imported file's header is another program's, or has another project's take numbers and times)
            const auto track = w.trackName.trim(), window = w.windowName.trim();
            if (w.ownRecording && (track.isNotEmpty() || w.takeTag.isNotEmpty()))
                narrow ([&] (const Candidate& c)
                {
                    const auto d = c.description.trim();
                    if (! looksLikeFermataHeader (d)) return Says::Nothing;
                    if (w.takeTag.isNotEmpty() && ! d.containsIgnoreCase (" / " + w.takeTag + " - ")) return Says::No;
                    if (track.isNotEmpty() && ! d.endsWithIgnoreCase (" / " + track))
                        return d.length() >= 250 ? Says::Nothing : Says::No;         // (a very long description was cut short when written: it may have lost its track name)
                    // the same take number and track: the one from this window says so; one from another window - or from this window under the
                    // name it had when it recorded - neither helps nor hurts
                    return (window.isEmpty() || d.startsWithIgnoreCase (window + " / ")) ? Says::Yes : Says::Nothing;
                }, "the take and track written in its header", "the other files' headers name another take or track");
            if (fit.size() > 1 && w.ownRecording && w.recordedAt.toMilliseconds() != 0)
                narrow ([&] (const Candidate& c)
                {
                    const double s = secondsFrom (c.date, c.time, w.recordedAt);
                    if (s > 1.0e11) return Says::Nothing;                            // (no date and time in it)
                    return s < 180.0 ? Says::Yes : Says::No;
                }, "recorded at the same time", "the other files were recorded at another time");
            if (fit.size() > 1 && track.isNotEmpty())
                narrow ([&] (const Candidate& c) { return fileNameNamesTrack (c.file.getFileNameWithoutExtension(), track, trackNames) ? Says::Yes : Says::Nothing; },
                        "the track name in its file name", {});
            if (fit.size() > 1 && w.takeTag.isNotEmpty())
                narrow ([&] (const Candidate& c) { return c.file.getFileName().startsWith (w.takeTag + " ") ? Says::Yes : Says::Nothing; }, "the take number in its file name", {});
        }
        if (fit.size() == 1)
        {
            files[fit[0]].claimed = true;
            results[k].found = files[fit[0]].file; results[k].how = how;
            again = true;                                                    // (one candidate fewer for the others: look again)
        }
        else
            for (auto i : fit) results[k].alternatives.add (relative (files[i]));
    }
    }
    report ("Done", 1.0);
    return results;
}
} // namespace td::relink

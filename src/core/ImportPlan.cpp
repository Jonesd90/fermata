#include "ImportPlan.h"
#include "Dither.h"
#include <map>
#include <set>

namespace td
{
static bool isSeparator (juce::juce_wchar c) { return c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']' || c == '+' || c == ','; }

static juce::StringArray tokensOf (const juce::String& stem)
{
    juce::StringArray t; juce::String cur;
    for (auto c : stem) { if (isSeparator (c)) { if (cur.isNotEmpty()) t.add (cur); cur.clear(); } else cur << c; }
    if (cur.isNotEmpty()) t.add (cur);
    return t;
}
static bool hasDigit (const juce::String& s) { for (auto c : s) if (juce::CharacterFunctions::isDigit (c)) return true; return false; }
static int numberIn (const juce::String& s)
{
    juce::String d; bool started = false;
    for (auto c : s) { if (juce::CharacterFunctions::isDigit (c)) { d << c; started = true; } else if (started) break; }
    return d.isEmpty() ? 0 : d.substring (0, 6).getIntValue();
}

juce::StringArray readIXmlTrackNames (const juce::File& f, int channels)
{
    juce::StringArray names;
    std::unique_ptr<juce::FileInputStream> in (f.createInputStream());
    if (in == nullptr || in->getTotalLength() < 44) return names;
    char hdr[12]; if (in->read (hdr, 12) != 12 || (std::memcmp (hdr, "RIFF", 4) != 0 && std::memcmp (hdr, "RF64", 4) != 0) || std::memcmp (hdr + 8, "WAVE", 4) != 0) return names;
    while (! in->isExhausted())
    {
        char id[4]; if (in->read (id, 4) != 4) break;
        const auto size = (juce::uint32) in->readInt();
        if (std::memcmp (id, "iXML", 4) == 0 && size < 4u * 1024u * 1024u)
        {
            juce::MemoryBlock mb; in->readIntoMemoryBlock (mb, (juce::int64) size);
            auto xml = juce::XmlDocument::parse (juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize()));
            if (xml != nullptr)
                if (auto* tl = xml->getChildByName ("TRACK_LIST"))
                {
                    for (int c = 0; c < channels; ++c) names.add ({});
                    for (auto* t : tl->getChildWithTagNameIterator ("TRACK"))
                    {
                        const int idx = t->getChildElementAllSubText ("INTERLEAVE_INDEX", "0").getIntValue() - 1;
                        const auto nm = t->getChildElementAllSubText ("NAME", {}).trim();
                        if (juce::isPositiveAndBelow (idx, channels) && nm.isNotEmpty()) names.set (idx, nm);
                    }
                }
            return names;
        }
        const juce::int64 next = in->getPosition() + (juce::int64) size + (size & 1u);
        if (next <= in->getPosition() || next > in->getTotalLength()) break;
        in->setPosition (next);
    }
    return names;
}

std::vector<ImportSource> inspectFiles (juce::AudioFormatManager& fm, const juce::Array<juce::File>& files, juce::StringArray& problems)
{
    std::vector<ImportSource> out;
    for (auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->numChannels < 1 || r->lengthInSamples < 1) { problems.add (f.getFileName() + " could not be read"); continue; }
        ImportSource s; s.file = f; s.channels = (int) r->numChannels; s.length = r->lengthInSamples; s.sampleRate = r->sampleRate;
        s.floatData = r->usesFloatingPointData; s.modified = f.getLastModificationTime();
        if (s.channels > 2 && f.hasFileExtension ("wav;wave;bwf")) s.channelNames = readIXmlTrackNames (f, s.channels);
        out.push_back (std::move (s));
    }
    return out;
}

ImportPlan buildImportPlan (std::vector<ImportSource> sources)
{
    ImportPlan plan; plan.sources = std::move (sources);
    auto& S = plan.sources;
    struct Item { int src; juce::StringArray tok; juce::String stem; };
    std::vector<Item> flat;                                            // mono / stereo files
    // ---- polyphonic files: each is a take on its own
    for (int i = 0; i < (int) S.size(); ++i)
    {
        if (S[(size_t) i].channels > 2)
        {
            ImportTake t; t.length = S[(size_t) i].length; t.sampleRate = S[(size_t) i].sampleRate; t.modified = S[(size_t) i].modified;
            t.number = numberIn (S[(size_t) i].file.getFileNameWithoutExtension());
            for (int c = 0; c < S[(size_t) i].channels; ++c)
            {
                ImportPart p; p.source = i; p.channel = c;
                const auto nm = c < S[(size_t) i].channelNames.size() ? S[(size_t) i].channelNames[c] : juce::String();
                p.trackName = nm.isNotEmpty() ? nm : "Ch " + juce::String (c + 1);
                p.trackKey = p.trackName.toLowerCase(); p.trackChannels = 1;
                t.parts.push_back (p);
            }
            plan.takes.push_back (std::move (t));
        }
        else flat.push_back ({ i, tokensOf (S[(size_t) i].file.getFileNameWithoutExtension()), S[(size_t) i].file.getFileNameWithoutExtension() });
    }

    // ---- mono / stereo files: the ones with the same length (and rate) are one take
    std::sort (flat.begin(), flat.end(), [&] (const Item& a, const Item& b)
    {
        const auto &x = S[(size_t) a.src], &y = S[(size_t) b.src];
        if (x.sampleRate != y.sampleRate) return x.sampleRate < y.sampleRate;
        if (x.length != y.length) return x.length < y.length;
        return a.stem.compareNatural (b.stem) < 0;
    });
    std::vector<std::vector<Item>> clusters;
    for (auto& it : flat)
    {
        const auto& s = S[(size_t) it.src];
        if (! clusters.empty())
        {
            const auto& last = S[(size_t) clusters.back().back().src];
            if (last.sampleRate == s.sampleRate && std::abs (last.length - s.length) <= 64) { clusters.back().push_back (it); continue; }
        }
        clusters.push_back ({ it });
    }

    // ---- which word in the file names is the take number? (the same in every file of a take, different from take to take)
    int tokPos = 0; bool haveTok = false;                              // >0: from the left (1-based), <0: from the right
    if (clusters.size() > 1)
    {
        double bestScore = 0.0;
        for (int sign : { 1, -1 })
            for (int pos = 1; pos <= 8; ++pos)
            {
                std::set<juce::String> values; int agree = 0, usable = 0; bool anyDigit = false;
                for (auto& c : clusters)
                {
                    juce::String v; bool same = true, have = true;
                    for (auto& it : c)
                    {
                        const int n = it.tok.size(), idx = sign > 0 ? pos - 1 : n - pos;
                        if (idx < 0 || idx >= n) { have = false; break; }
                        if (v.isEmpty()) v = it.tok[idx]; else if (v != it.tok[idx]) same = false;
                    }
                    if (! have) continue;
                    ++usable; if (same) { ++agree; values.insert (v); anyDigit = anyDigit || hasDigit (v); }
                }
                if (usable < (int) clusters.size() || agree < (int) clusters.size()) continue;
                if ((int) values.size() < (int) clusters.size()) continue;      // must differ from take to take
                const double score = (anyDigit ? 2.0 : 1.0) + (sign > 0 ? 0.01 : 0.0) - 0.001 * pos;
                if (score > bestScore) { bestScore = score; tokPos = sign * pos; haveTok = true; }
            }
    }
    // one single cluster (every file the same length): maybe it is several takes of identical length. A word that splits it into
    // equal groups with the same set of track names is the take number.
    if (! haveTok && clusters.size() == 1 && clusters[0].size() >= 2)
    {
        double bestScore = 0.0;
        for (int sign : { 1, -1 })
            for (int pos = 1; pos <= 8; ++pos)
            {
                std::map<juce::String, std::set<juce::String>> groups; bool ok = true, anyDigit = false;
                for (auto& it : clusters[0])
                {
                    const int n = it.tok.size(), idx = sign > 0 ? pos - 1 : n - pos;
                    if (idx < 0 || idx >= n) { ok = false; break; }
                    juce::StringArray t = it.tok; const auto v = t[idx]; t.remove (idx);
                    if (! groups[v].insert (t.joinIntoString (" ").trim().toLowerCase()).second) { ok = false; break; }   // same track twice in a group
                    anyDigit = anyDigit || hasDigit (v);
                }
                if (! ok || ! anyDigit || groups.size() < 2) continue;
                for (auto& kv : groups) if (kv.second != groups.begin()->second || kv.second.size() < 2) ok = false;   // 'Mic 1', 'Mic 2' alone stay one take
                if (! ok) continue;
                const double score = (anyDigit ? 2.0 : 1.0) + (sign > 0 ? 0.01 : 0.0) - 0.001 * pos;
                if (score > bestScore) { bestScore = score; tokPos = sign * pos; haveTok = true; }
            }
    }
    auto tokenOf = [&] (const Item& it) -> juce::String
    {
        if (! haveTok) return {};
        const int n = it.tok.size(), idx = tokPos > 0 ? tokPos - 1 : n + tokPos;
        return (idx >= 0 && idx < n) ? it.tok[idx] : juce::String();
    };
    auto keyOf = [&] (const Item& it) -> juce::String
    {
        juce::StringArray t = it.tok;
        if (haveTok) { const int n = t.size(), idx = tokPos > 0 ? tokPos - 1 : n + tokPos; if (idx >= 0 && idx < n) t.remove (idx); }
        return t.joinIntoString (" ").trim();
    };

    // ---- clusters that hold the same track twice are really several takes of the same length: split them by the take word
    std::vector<std::vector<Item>> takesFlat;
    for (auto& c : clusters)
    {
        std::set<juce::String> seen; bool dup = false;
        for (auto& it : c) { const auto k = keyOf (it).toLowerCase(); if (seen.count (k)) dup = true; seen.insert (k); }
        if (! dup || ! haveTok) { takesFlat.push_back (c); continue; }
        std::map<juce::String, std::vector<Item>> byTok;
        for (auto& it : c) byTok[tokenOf (it)].push_back (it);
        for (auto& kv : byTok) takesFlat.push_back (kv.second);
        plan.notes.add ("Several takes have exactly the same length; they were told apart by the number in the file names.");
    }

    for (auto& c : takesFlat)
    {
        ImportTake t; const auto& first = S[(size_t) c.front().src];
        t.length = first.length; t.sampleRate = first.sampleRate; t.modified = first.modified;
        t.number = haveTok ? numberIn (tokenOf (c.front())) : 0;
        for (auto& it : c)
        {
            ImportPart p; p.source = it.src; p.channel = -1;
            p.trackName = keyOf (it); if (p.trackName.isEmpty()) p.trackName = it.stem;
            p.trackKey = p.trackName.toLowerCase();
            p.trackChannels = S[(size_t) it.src].channels >= 2 ? 2 : 1;
            t.parts.push_back (p);
            if (S[(size_t) it.src].modified < t.modified) t.modified = S[(size_t) it.src].modified;
        }
        plan.takes.push_back (std::move (t));
    }

    // ---- order: by the take numbers if every take has its own, else by the time the files were made, else by name
    bool numbersOk = ! plan.takes.empty();
    { std::set<int> nums; for (auto& t : plan.takes) { if (t.number <= 0 || nums.count (t.number)) numbersOk = false; nums.insert (t.number); } }
    std::stable_sort (plan.takes.begin(), plan.takes.end(), [&] (const ImportTake& a, const ImportTake& b)
    {
        if (numbersOk) return a.number < b.number;
        if (a.modified != b.modified) return a.modified < b.modified;
        return S[(size_t) a.parts.front().source].file.getFileName().compareNatural (S[(size_t) b.parts.front().source].file.getFileName()) < 0;
    });
    if (! numbersOk && plan.takes.size() > 1) plan.notes.add ("No take numbers were found in the names, so the takes are in the order the files were made.");

    // ---- tracks: the same key in different takes is the same track
    std::set<juce::String> seenKeys;
    for (auto& t : plan.takes)
    {
        std::stable_sort (t.parts.begin(), t.parts.end(), [&] (const ImportPart& a, const ImportPart& b)
        {
            if (a.channel >= 0 && b.channel >= 0 && a.source == b.source) return a.channel < b.channel;
            return S[(size_t) a.source].file.getFileName().compareNatural (S[(size_t) b.source].file.getFileName()) < 0;
        });
        for (auto& p : t.parts)
            if (! seenKeys.count (p.trackKey)) { seenKeys.insert (p.trackKey); plan.tracks.push_back ({ p.trackKey, p.trackName, p.trackChannels }); }
    }
    std::set<double> rates; for (auto& t : plan.takes) rates.insert (t.sampleRate);
    if (rates.size() > 1) plan.notes.add ("The files have different sample rates; they are imported as they are.");
    // a take that lacks tracks the others have
    for (auto& t : plan.takes)
        if (t.parts.size() < plan.tracks.size() && plan.takes.size() > 1) { plan.notes.add ("Some takes have fewer tracks than others."); break; }
    return plan;
}

juce::String copyImportFiles (juce::AudioFormatManager& fm, const ImportPlan& plan, const juce::File& folder, const juce::String& label,
                              const std::vector<int>& takeNumbers, std::vector<std::vector<juce::File>>& files, AudioJob* job)
{
    files.assign (plan.takes.size(), {});
    folder.createDirectory();
    juce::int64 totalSamples = 0, doneSamples = 0;
    for (auto& t : plan.takes) totalSamples += t.length * (juce::int64) t.parts.size();
    std::vector<juce::File> made;
    auto fail = [&] (const juce::String& m) { for (auto& f : made) f.deleteFile(); files.clear(); return m; };
    for (size_t ti = 0; ti < plan.takes.size(); ++ti)
    {
        const auto& take = plan.takes[ti];
        files[ti].assign (take.parts.size(), juce::File());
        // parts that come from the same polyphonic file are read once, together
        std::map<int, std::vector<size_t>> bySource;
        for (size_t pi = 0; pi < take.parts.size(); ++pi) bySource[take.parts[pi].source].push_back (pi);
        for (auto& kv : bySource)
        {
            const auto& src = plan.sources[(size_t) kv.first];
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src.file));
            if (reader == nullptr) return fail ("Cannot read " + src.file.getFileName());
            const int nc = (int) reader->numChannels;
            std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers; std::vector<std::unique_ptr<DitherOut>> douts; std::vector<std::vector<int>> chans;
            // 16 and 24 bit sources come across bit for bit; only a source wider than 24 bit (32-bit integer) loses word length and is dithered
            const bool reduces = ! src.floatData && reader->bitsPerSample > 24;
            for (auto pi : kv.second)
            {
                const auto& part = take.parts[pi];
                const auto dest = audioops::uniqueFile (folder.getChildFile (TakeWindowDef::fileNameFor (takeNumbers[ti], label, part.trackName)));
                std::vector<int> cs; if (part.channel >= 0) cs = { part.channel }; else for (int c = 0; c < part.trackChannels && c < nc; ++c) cs.push_back (c);
                auto w = audioops::makeWavWriter (dest, src.sampleRate, (int) cs.size(), src.floatData);
                if (w == nullptr) return fail ("Cannot create " + dest.getFileName());
                writers.push_back (std::move (w)); douts.push_back (std::make_unique<DitherOut> (*writers.back(), reduces)); chans.push_back (cs); files[ti][pi] = dest; made.push_back (dest);
            }
            std::vector<std::vector<float>> blk;
            constexpr juce::int64 block = 1 << 16;
            for (juce::int64 pos = 0; pos < src.length; pos += block)
            {
                if (job != nullptr) { if (job->cancelled()) return fail ("Cancelled."); job->setProgress ((float) ((double) doneSamples / (double) juce::jmax ((juce::int64) 1, totalSamples))); }
                const auto cnt = juce::jmin (block, src.length - pos);
                if (! audioops::readRange (*reader, pos, cnt, blk)) return fail ("Cannot read " + src.file.getFileName());
                for (size_t w = 0; w < writers.size(); ++w)
                {
                    std::vector<const float*> ptrs; for (int c : chans[w]) ptrs.push_back (blk[(size_t) c].data());
                    if (! douts[w]->write (ptrs.data(), (int) ptrs.size(), (int) cnt)) return fail ("Disk full or write error.");
                }
                doneSamples += cnt * (juce::int64) writers.size();
            }
            writers.clear();
        }
    }
    return {};
}
} // namespace td

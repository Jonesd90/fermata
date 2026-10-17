#include "ImportPlan.h"
#include "Dither.h"
#include <map>
#include <set>
#include <regex>

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

std::vector<ImportSource> inspectFiles (juce::AudioFormatManager& fm, const juce::Array<juce::File>& files, juce::StringArray& problems,
                                        const std::function<bool (int, int)>& progress)
{
    std::vector<ImportSource> out;
    int done = 0;
    for (auto& f : files)
    {
        if (progress && ! progress (done, files.size())) break;
        ++done;
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

    // ---- names like "A Winter Ride 015_piano-L.wav": <piece> <take number>_<instrument or mic>[-L|-R|-C]. Every file that follows this pattern
    //      is placed by what its name says: all takes of one piece share the same tracks (one per instrument / mic name).
    {
        static const std::regex pat (R"(^(.*?)[ ]+(\d{2,4})_(.+)$)");
        static const std::regex patNoNumber (R"(^(.*?)_(.+)$)");          // "LV1 - Gravity_Omni-L": Pyramix leaves the number off the first take
        struct Hit { int idx; juce::String piece, inst; int num; };
        // the same piece may be spelt a little differently ("LV01 - Gravity" / "LV1 - Gravity"): numbers are compared without their leading zeros
        auto pieceKey = [] (const juce::String& piece)
        {
            juce::String k; bool inNum = false, lead = false;
            for (auto c : piece.toLowerCase().trim())
            {
                if (juce::CharacterFunctions::isDigit (c)) { if (! inNum) { inNum = true; lead = true; } if (lead && c == '0') continue; lead = false; k << c; }
                else { if (inNum && lead) k << '0'; inNum = false; lead = false; k << c; }
            }
            if (inNum && lead) k << '0';
            return k;
        };
        std::vector<Hit> hits;
        std::set<juce::String> numberedPieces;
        for (int k = 0; k < (int) flat.size(); ++k)
        {
            std::smatch m; const std::string stem = flat[(size_t) k].stem.toStdString();
            if (std::regex_match (stem, m, pat) && m[1].length() > 0)
            {
                hits.push_back ({ k, juce::String (m[1].str()).trim(), juce::String (m[3].str()).trim(), juce::String (m[2].str()).getIntValue() });
                numberedPieces.insert (pieceKey (hits.back().piece));
            }
        }
        if (! hits.empty())
            for (int k = 0; k < (int) flat.size(); ++k)
            {
                bool already = false; for (auto& h : hits) if (h.idx == k) { already = true; break; }
                if (already) continue;
                std::smatch m; const std::string stem = flat[(size_t) k].stem.toStdString();
                if (std::regex_match (stem, m, patNoNumber) && m[1].length() > 0 && numberedPieces.count (pieceKey (juce::String (m[1].str()).trim())) != 0)
                    hits.push_back ({ k, juce::String (m[1].str()).trim(), juce::String (m[2].str()).trim(), 0 });       // the take without a number comes first
            }
        if (hits.size() >= 2 && hits.size() * 5 >= flat.size() * 4)       // at least 80 % of the files follow the pattern
        {
            std::map<juce::String, std::map<juce::String, int>> spellings;      // piece key -> spelling -> how often
            for (auto& h : hits) spellings[pieceKey (h.piece)][h.piece]++;
            std::map<juce::String, juce::String> pieceName;
            for (auto& kv : spellings) { int best = -1; for (auto& sp : kv.second) if (sp.second > best) { best = sp.second; pieceName[kv.first] = sp.first; } }
            std::map<std::pair<juce::String, int>, std::vector<Hit>> groups;
            for (auto& h : hits) groups[{ pieceKey (h.piece), h.num }].push_back (h);
            for (auto& kv : groups)
            {
                ImportTake t; const auto& first = S[(size_t) flat[(size_t) kv.second.front().idx].src];
                t.length = first.length; t.sampleRate = first.sampleRate; t.modified = first.modified; t.number = kv.first.second;
                t.piece = pieceName[kv.first.first];
                std::set<juce::String> used;
                for (auto& h : kv.second)
                {
                    const auto& src = S[(size_t) flat[(size_t) h.idx].src];
                    ImportPart p; p.source = flat[(size_t) h.idx].src; p.channel = -1;
                    juce::String inst = h.inst;
                    if (inst.endsWithIgnoreCase ("-C") || inst.endsWithIgnoreCase ("_C")) inst = inst.dropLastCharacters (2).trim();   // "C" = a mono track
                    if (inst.isEmpty()) inst = h.inst;
                    p.trackName = inst;                                    // every piece shares the same tracks (the same instrument / mic names)
                    p.trackKey = p.trackName.toLowerCase();
                    if (used.count (p.trackKey)) continue;
                    used.insert (p.trackKey);
                    p.trackChannels = src.channels >= 2 ? 2 : 1;
                    t.length = juce::jmax (t.length, src.length);
                    if (src.modified < t.modified) t.modified = src.modified;
                    t.parts.push_back (p);
                }
                plan.takes.push_back (std::move (t));
            }
            plan.notes.add ("The file names (piece, take number, instrument) were used to put every take of a piece on the same tracks.");
            std::set<int> taken; for (auto& h : hits) taken.insert (h.idx);
            std::vector<Item> rest; for (int k = 0; k < (int) flat.size(); ++k) if (! taken.count (k)) rest.push_back (flat[(size_t) k]);
            flat = std::move(rest);
        }
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
    std::vector<int> takeWords;                                        // EVERY word position that is a take number (names like "tk.01_01" carry it twice)
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
                if (anyDigit) takeWords.push_back (sign * pos);
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
    if (haveTok && std::find (takeWords.begin(), takeWords.end(), tokPos) == takeWords.end()) takeWords.push_back (tokPos);
    auto keyOf = [&] (const Item& it) -> juce::String
    {
        juce::StringArray t = it.tok;
        if (haveTok)
        {
            std::set<int> drop;                                                     // all the take-number words of this name (a word position from the left or from the right)
            const int n = t.size();
            for (int pos : takeWords) { const int idx = pos > 0 ? pos - 1 : n + pos; if (idx >= 0 && idx < n) drop.insert (idx); }
            for (auto i = drop.rbegin(); i != drop.rend(); ++i) t.remove (*i);
        }
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
    { std::set<std::pair<juce::String, int>> nums; for (auto& t : plan.takes) { if (t.number < 0 || nums.count ({ t.piece.toLowerCase(), t.number })) numbersOk = false; nums.insert ({ t.piece.toLowerCase(), t.number }); } }
    std::stable_sort (plan.takes.begin(), plan.takes.end(), [&] (const ImportTake& a, const ImportTake& b)
    {
        if (numbersOk) { const int c = a.piece.compareNatural (b.piece); return c != 0 ? c < 0 : a.number < b.number; }
        if (a.modified != b.modified) return a.modified < b.modified;
        return S[(size_t) a.parts.front().source].file.getFileName().compareNatural (S[(size_t) b.parts.front().source].file.getFileName()) < 0;
    });
    if (! numbersOk && plan.takes.size() > 1) plan.notes.add ("No take numbers were found in the names, so the takes are in the order the files were made.");

    // ---- tidy the track names: "0105 L tk" -> "L" (a leading track number that every name has, and a last word "tk" / "take" that every name has)
    {
        std::vector<ImportPart*> all; for (auto& t : plan.takes) for (auto& p : t.parts) all.push_back (&p);
        auto tryTidy = [&] (const std::function<juce::String (const juce::StringArray&)>& fn)
        {
            if (all.empty()) return;
            std::map<juce::String, juce::String> keyToNew, newToKey;                  // old key -> new name; a tidy that would make two tracks the same is not done
            for (auto* p : all)
            {
                const auto nn = fn (tokensOf (p->trackName));
                if (nn.isEmpty()) return;
                const auto ok = p->trackKey, nk = nn.toLowerCase();
                if (keyToNew.count (ok) && keyToNew[ok] != nn) return;
                if (newToKey.count (nk) && newToKey[nk] != ok) return;
                keyToNew[ok] = nn; newToKey[nk] = ok;
            }
            for (auto* p : all) { p->trackName = keyToNew[p->trackKey]; p->trackKey = p->trackName.toLowerCase(); }
        };
        auto allNames = [&] (const std::function<bool (const juce::StringArray&)>& test) { for (auto* p : all) if (! test (tokensOf (p->trackName))) return false; return ! all.empty(); };
        auto isNumberWord = [] (const juce::String& w) { return w.length() >= 2 && w.containsOnly ("0123456789"); };
        auto isTakeWord = [] (const juce::String& w) { return w.equalsIgnoreCase ("tk") || w.equalsIgnoreCase ("take") || w.equalsIgnoreCase ("tke"); };
        if (allNames ([&] (const juce::StringArray& t) { return t.size() >= 2 && isNumberWord (t[0]); }))
            tryTidy ([] (const juce::StringArray& t) { juce::StringArray r = t; r.remove (0); return r.joinIntoString (" "); });
        if (allNames ([&] (const juce::StringArray& t) { return t.size() >= 2 && isTakeWord (t[t.size() - 1]); }))
            tryTidy ([] (const juce::StringArray& t) { juce::StringArray r = t; r.remove (r.size() - 1); return r.joinIntoString (" "); });
    }

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

/** 'piano-L' -> base 'piano', side L (0).  Understands: a last word L, R, Left, Right after '-', '_' or a space ("piano-L", "Piano R"); the letter stuck to the
    name ("Piano4099L", "PianoR"); and a name that is only L or R (base empty). With 'numbers' also a last 1 / 2 ("Mic 1", "Mic2": 1 = left, 2 = right). */
static bool splitSide (const juce::String& name, juce::String& base, int& side, bool numbers)
{
    const auto n = name.trim();
    auto word = [&] (const juce::String& w, int& sd)
    {
        const auto l = w.trim().toLowerCase();
        if (l == "l" || l == "left") { sd = 0; return true; }
        if (l == "r" || l == "right") { sd = 1; return true; }
        if (numbers && l == "1") { sd = 0; return true; }
        if (numbers && l == "2") { sd = 1; return true; }
        return false;
    };
    for (auto sep : { '-', '_', ' ' })
    {
        const int i = n.lastIndexOfChar ((juce::juce_wchar) sep);
        if (i <= 0) continue;
        if (! word (n.substring (i + 1), side)) continue;
        base = n.substring (0, i).trim();
        return base.isNotEmpty();
    }
    if (word (n, side) && ! n.containsOnly ("12")) { base = {}; return true; }               // just "L" or "R" (a bare 1 / 2 is not a pair)
    if (n.length() >= 2)
    {
        const auto c = n[n.length() - 1], prev = n[n.length() - 2];
        const bool prevOk = juce::CharacterFunctions::isDigit (prev) || juce::CharacterFunctions::isLowerCase (prev);
        if ((c == 'L' || c == 'R') && prevOk) { side = c == 'L' ? 0 : 1; base = n.dropLastCharacters (1).trim(); return base.isNotEmpty(); }
        if (numbers && (c == '1' || c == '2') && juce::CharacterFunctions::isLetter (prev)) { side = c == '1' ? 0 : 1; base = n.dropLastCharacters (1).trim(); return base.isNotEmpty(); }
    }
    return false;
}

bool hasStereoPairs (const ImportPlan& plan, bool numbers)
{
    for (auto& t : plan.takes)
    {
        std::map<juce::String, int> seen;
        for (auto& p : t.parts)
        {
            juce::String base; int side = 0;
            if (p.channel < 0 && p.trackChannels == 1 && p.source2 < 0 && plan.sources[(size_t) p.source].channels == 1 && splitSide (p.trackName, base, side, numbers))
                if ((seen[base.toLowerCase()] |= (1 << side)) == 3) return true;
        }
    }
    return false;
}

std::vector<StereoPairCandidate> stereoPairCandidates (const ImportPlan& plan, bool numbers)
{
    std::vector<StereoPairCandidate> out; std::set<juce::String> done;
    for (auto& t : plan.takes)
    {
        struct Seen { int mask = 0; juce::String base, l, r; };
        std::map<juce::String, Seen> seen; std::vector<juce::String> order;
        for (auto& p : t.parts)
        {
            juce::String base; int side = 0;
            if (p.channel < 0 && p.trackChannels == 1 && p.source2 < 0 && plan.sources[(size_t) p.source].channels == 1 && splitSide (p.trackName, base, side, numbers))
            {
                const auto k = base.toLowerCase();
                if (seen.find (k) == seen.end()) { order.push_back (k); seen[k].base = base; }
                auto& sn = seen[k]; sn.mask |= 1 << side; (side == 0 ? sn.l : sn.r) = p.trackName;
            }
        }
        for (auto& k : order)
            if (seen[k].mask == 3 && ! done.count (k)) { done.insert (k); out.push_back ({ k, seen[k].base.isEmpty() ? juce::String ("L-R") : seen[k].base, seen[k].l, seen[k].r }); }
    }
    return out;
}

ImportPlan joinStereoPairs (const ImportPlan& in, bool numbers, const std::set<juce::String>* joinOnly)
{
    ImportPlan plan = in;
    for (auto& t : plan.takes)
    {
        std::map<juce::String, std::pair<int, int>> halves;                   // base -> (left part index, right part index)
        for (int i = 0; i < (int) t.parts.size(); ++i)
        {
            const auto& p = t.parts[(size_t) i];
            juce::String base; int side = 0;
            if (p.channel < 0 && p.trackChannels == 1 && p.source2 < 0 && plan.sources[(size_t) p.source].channels == 1 && splitSide (p.trackName, base, side, numbers))
            {
                auto it = halves.emplace (base.toLowerCase(), std::make_pair (-1, -1)).first;
                (side == 0 ? it->second.first : it->second.second) = i;
            }
        }
        std::vector<ImportPart> out; std::set<int> used;
        for (int i = 0; i < (int) t.parts.size(); ++i)
        {
            if (used.count (i)) continue;
            const auto& p = t.parts[(size_t) i];
            juce::String base; int side = 0;
            if (p.channel < 0 && p.trackChannels == 1 && p.source2 < 0 && plan.sources[(size_t) p.source].channels == 1 && splitSide (p.trackName, base, side, numbers))
            {
                auto h = halves[base.toLowerCase()];
                if (h.first >= 0 && h.second >= 0 && (joinOnly == nullptr || joinOnly->count (base.toLowerCase()) != 0))
                {
                    if (used.count (h.first) || used.count (h.second)) continue;
                    used.insert (h.first); used.insert (h.second);
                    ImportPart j = t.parts[(size_t) h.first];
                    j.source2 = t.parts[(size_t) h.second].source;
                    j.trackName = base.isEmpty() ? juce::String ("L-R") : base; j.trackKey = j.trackName.toLowerCase(); j.trackChannels = 2;
                    out.push_back (j);
                    continue;
                }
            }
            out.push_back (p);
        }
        t.parts = std::move (out);
    }
    plan.tracks.clear(); std::set<juce::String> seenKeys;
    for (auto& t : plan.takes)
        for (auto& p : t.parts)
            if (! seenKeys.count (p.trackKey)) { seenKeys.insert (p.trackKey); plan.tracks.push_back ({ p.trackKey, p.trackName, p.trackChannels }); }
    if (joinOnly == nullptr || ! joinOnly->empty()) plan.notes.add ("Left / right pairs were joined into stereo tracks.");
    return plan;
}

juce::String copyImportFiles (juce::AudioFormatManager& fm, const ImportPlan& plan, const juce::File& folder, const juce::String& label,
                              const std::vector<int>& takeNumbers, std::vector<std::vector<juce::File>>& files, AudioJob* job, bool linkOriginals)
{
    files.assign (plan.takes.size(), {});
    folder.createDirectory();
    juce::int64 totalSamples = 0, doneSamples = 0;
    for (auto& t : plan.takes) totalSamples += t.length * (juce::int64) t.parts.size();
    totalSamples = juce::jmax ((juce::int64) 1, totalSamples);
    std::vector<juce::File> made;
    auto fail = [&] (const juce::String& m) { for (auto& f : made) f.deleteFile(); files.clear(); return m; };
    for (size_t ti = 0; ti < plan.takes.size(); ++ti)
    {
        const auto& take = plan.takes[ti];
        files[ti].assign (take.parts.size(), juce::File());
        // a stereo track made from a left and a right mono file: one new stereo file
        for (size_t pi = 0; pi < take.parts.size(); ++pi)
        {
            const auto& part = take.parts[pi];
            if (part.source2 < 0) continue;
            const auto& sl = plan.sources[(size_t) part.source]; const auto& sr = plan.sources[(size_t) part.source2];
            std::unique_ptr<juce::AudioFormatReader> rl (fm.createReaderFor (sl.file)), rr (fm.createReaderFor (sr.file));
            if (rl == nullptr || rr == nullptr) return fail ("Cannot read " + (rl == nullptr ? sl : sr).file.getFileName());
            const auto dest = audioops::uniqueFile (folder.getChildFile (TakeWindowDef::fileNameFor (takeNumbers[ti], label, part.trackName)));
            auto w = audioops::makeWavWriter (dest, sl.sampleRate, 2, sl.floatData || sr.floatData);
            if (w == nullptr) return fail ("Cannot create " + dest.getFileName());
            const bool reduces = (! sl.floatData && rl->bitsPerSample > 24) || (! sr.floatData && rr->bitsPerSample > 24);
            DitherOut dout (*w, reduces);
            made.push_back (dest); files[ti][pi] = dest;
            std::vector<std::vector<float>> bl, br;
            const juce::int64 total = juce::jmax (sl.length, sr.length);
            constexpr juce::int64 block = 1 << 16;
            for (juce::int64 pos = 0; pos < total; pos += block)
            {
                if (job != nullptr) { if (job->cancelled()) return fail ("Cancelled."); job->setProgress ((float) ((double) doneSamples / (double) juce::jmax ((juce::int64) 1, totalSamples))); }
                const auto cnt = (int) juce::jmin (block, total - pos);
                std::vector<float> zero ((size_t) cnt, 0.0f), left = zero, right = zero;
                if (pos < sl.length) { const auto c = juce::jmin ((juce::int64) cnt, sl.length - pos); if (! audioops::readRange (*rl, pos, c, bl)) return fail ("Cannot read " + sl.file.getFileName()); std::copy (bl[0].begin(), bl[0].begin() + c, left.begin()); }
                if (pos < sr.length) { const auto c = juce::jmin ((juce::int64) cnt, sr.length - pos); if (! audioops::readRange (*rr, pos, c, br)) return fail ("Cannot read " + sr.file.getFileName()); std::copy (br[0].begin(), br[0].begin() + c, right.begin()); }
                const float* ptrs[2] = { left.data(), right.data() };
                if (! dout.write (ptrs, 2, cnt)) return fail ("Disk full or write error.");
                doneSamples += (juce::int64) cnt * 2;
            }
        }
        // parts that come from the same polyphonic file are read once, together
        std::map<int, std::vector<size_t>> bySource;
        for (size_t pi = 0; pi < take.parts.size(); ++pi)
        {
            const auto& part = take.parts[pi];
            if (part.source2 >= 0) continue;
            if (linkOriginals && part.channel < 0 && plan.sources[(size_t) part.source].channels == part.trackChannels)   // a whole mono / stereo file: used where it is, nothing is copied
            {
                files[ti][pi] = plan.sources[(size_t) part.source].file;
                continue;
            }
            bySource[part.source].push_back (pi);
        }
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

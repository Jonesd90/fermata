#include "MasterDef.h"
#include <set>

namespace td
{
using juce::var;

namespace
{
var newObj() { return var (new juce::DynamicObject()); }
void put (var& o, const char* k, const var& v) { o.getDynamicObject()->setProperty (k, v); }

var tagsToVar (const MasterTags& t)
{
    var o = newObj();
    put (o, "title", t.title); put (o, "artist", t.artist); put (o, "album", t.album); put (o, "albumArtist", t.albumArtist); put (o, "composer", t.composer);
    put (o, "genre", t.genre); put (o, "year", t.year); put (o, "comment", t.comment); put (o, "copyright", t.copyright); put (o, "isrc", t.isrc);
    return o;
}
MasterTags tagsFromVar (const var& v)
{
    MasterTags t;
    t.title = v["title"].toString(); t.artist = v["artist"].toString(); t.album = v["album"].toString(); t.albumArtist = v["albumArtist"].toString();
    t.composer = v["composer"].toString(); t.genre = v["genre"].toString(); t.year = v["year"].toString(); t.comment = v["comment"].toString();
    t.copyright = v["copyright"].toString(); t.isrc = v["isrc"].toString();
    return t;
}
juce::Uuid idOf (const var& v) { const auto s = v.toString(); return s.isEmpty() ? juce::Uuid::null() : juce::Uuid (s); }
juce::String idStr (const juce::Uuid& u) { return u.isNull() ? juce::String() : u.toString(); }
} // namespace

void MasteringDef::sync (const std::vector<std::unique_ptr<EditDef>>& edits)
{
    std::set<juce::Uuid> have;
    for (auto& e : edits) if (e != nullptr) have.insert (e->id);
    items.erase (std::remove_if (items.begin(), items.end(), [&] (const MasterItem& i) { return have.count (i.editId) == 0; }), items.end());
    ddp.clips.erase (std::remove_if (ddp.clips.begin(), ddp.clips.end(), [&] (const DdpClip& c) { return have.count (c.editId) == 0; }), ddp.clips.end());
    for (auto& e : edits)
    {
        if (e == nullptr) continue;
        if (findItem (e->id) == nullptr) { MasterItem i; i.editId = e->id; i.include = ! e->isEmpty(); items.push_back (i); }
        if (findClip (e->id) == nullptr) { DdpClip c; c.editId = e->id; c.include = ! e->isEmpty(); c.title = e->name; ddp.clips.push_back (c); }
    }
    if (findItem (selectedItem) == nullptr) selectedItem = items.empty() ? juce::Uuid::null() : items.front().editId;
    if (findClip (selectedClip) == nullptr) selectedClip = ddp.clips.empty() ? juce::Uuid::null() : ddp.clips.front().editId;
}

var MasteringDef::toVar() const
{
    var root = newObj();
    put (root, "mixer", idStr (mixerId)); put (root, "source", idStr (sourceId)); put (root, "tail", tailSeconds); put (root, "view", view);
    {
        var s = newObj();
        put (s, "format", (int) vm.format); put (s, "rate", vm.sampleRate); put (s, "bits", vm.bitDepth); put (s, "dither", (int) vm.dither);
        put (s, "mp3Kbps", vm.mp3Kbps); put (s, "mp3Vbr", vm.mp3Vbr); put (s, "oggQuality", vm.oggQuality); put (s, "flacLevel", vm.flacLevel); put (s, "extraFormats", vm.extraFormats); put (s, "normalise", vm.normalise);
        put (s, "peak", (double) vm.peakDb); put (s, "together", vm.peakTogether); put (s, "number", vm.numberFiles); put (s, "folder", vm.folder);
        put (s, "album", tagsToVar (vm.album));
        put (root, "vm", s);
    }
    juce::Array<var> its;
    for (auto& i : items)
    {
        var o = newObj();
        put (o, "edit", i.editId.toString()); put (o, "include", i.include); put (o, "fileName", i.fileName); put (o, "tags", tagsToVar (i.tags));
        its.add (o);
    }
    put (root, "items", its);
    {
        var d = newObj();
        put (d, "upc", ddp.upc); put (d, "title", ddp.title); put (d, "performer", ddp.performer); put (d, "songwriter", ddp.songwriter);
        put (d, "composer", ddp.composer); put (d, "arranger", ddp.arranger); put (d, "language", ddp.languageCode);
        put (d, "normalise", ddp.normalise); put (d, "peak", (double) ddp.peakDb); put (d, "together", ddp.peakTogether);
        put (d, "zip", ddp.makeZip); put (d, "cue", ddp.makeCue); put (d, "folder", ddp.folder); put (d, "endPad", ddp.endPadSectors);
        juce::Array<var> cs;
        for (auto& c : ddp.clips)
        {
            var o = newObj();
            put (o, "edit", c.editId.toString()); put (o, "include", c.include); put (o, "gap", c.gapSectors); put (o, "i01", c.index01Shift); if (c.index00Shift != kIndexAuto) put (o, "i00s", c.index00Shift);
            put (o, "title", c.title); put (o, "performer", c.performer); put (o, "songwriter", c.songwriter); put (o, "composer", c.composer);
            put (o, "arranger", c.arranger); put (o, "isrc", c.isrc); put (o, "pre", c.preEmphasis); put (o, "copy", c.copyPermitted);
            cs.add (o);
        }
        put (d, "clips", cs);
        put (root, "ddp", d);
    }
    return root;
}

void MasteringDef::fromVar (const var& root)
{
    *this = MasteringDef();
    if (! root.isObject()) return;
    mixerId = idOf (root["mixer"]); sourceId = idOf (root["source"]); tailSeconds = juce::jlimit (0.0, 30.0, (double) root["tail"]); view = juce::jlimit (0, 1, (int) root["view"]);
    const auto s = root["vm"];
    if (s.isObject())
    {
        vm.format = (MasterFormat) juce::jlimit (0, 4, (int) s["format"]); vm.sampleRate = (double) s["rate"]; vm.bitDepth = (int) s["bits"];
        if (vm.bitDepth != 16 && vm.bitDepth != 24 && vm.bitDepth != 32) vm.bitDepth = 24;
        vm.dither = (DitherMode) juce::jlimit (0, 1, (int) s["dither"]);
        vm.mp3Kbps = (int) s["mp3Kbps"]; vm.mp3Vbr = juce::jlimit (0, 9, (int) s["mp3Vbr"]); vm.oggQuality = juce::jmax (0, (int) s["oggQuality"]);
        vm.flacLevel = s["flacLevel"].isVoid() ? 5 : juce::jlimit (0, 8, (int) s["flacLevel"]); vm.extraFormats = juce::jlimit (0, 31, (int) s["extraFormats"]);
        vm.normalise = s["normalise"].isVoid() ? true : (bool) s["normalise"]; vm.peakDb = s["peak"].isVoid() ? -1.0f : (float) (double) s["peak"];
        vm.peakTogether = (bool) s["together"]; vm.numberFiles = s["number"].isVoid() ? true : (bool) s["number"]; vm.folder = s["folder"].toString();
        vm.album = tagsFromVar (s["album"]);
    }
    if (auto* a = root["items"].getArray())
        for (auto& v : *a)
        {
            MasterItem i; i.editId = idOf (v["edit"]); i.include = v["include"].isVoid() ? true : (bool) v["include"];
            i.fileName = v["fileName"].toString(); i.tags = tagsFromVar (v["tags"]);
            if (! i.editId.isNull()) items.push_back (i);
        }
    const auto d = root["ddp"];
    if (d.isObject())
    {
        ddp.upc = d["upc"].toString(); ddp.title = d["title"].toString(); ddp.performer = d["performer"].toString(); ddp.songwriter = d["songwriter"].toString();
        ddp.composer = d["composer"].toString(); ddp.arranger = d["arranger"].toString(); ddp.languageCode = d["language"].isVoid() ? 9 : (int) d["language"];
        ddp.normalise = (bool) d["normalise"]; ddp.peakDb = d["peak"].isVoid() ? -0.3f : (float) (double) d["peak"]; ddp.peakTogether = d["together"].isVoid() ? true : (bool) d["together"];
        ddp.makeZip = d["zip"].isVoid() ? true : (bool) d["zip"]; ddp.makeCue = d["cue"].isVoid() ? true : (bool) d["cue"]; ddp.folder = d["folder"].toString(); ddp.endPadSectors = juce::jmax (0, (int) d["endPad"]);
        if (auto* a = d["clips"].getArray())
            for (auto& v : *a)
            {
                DdpClip c; c.editId = idOf (v["edit"]); c.include = v["include"].isVoid() ? true : (bool) v["include"];
                c.gapSectors = juce::jmax (0, v["gap"].isVoid() ? 150 : (int) v["gap"]);
                c.title = v["title"].toString(); c.performer = v["performer"].toString(); c.songwriter = v["songwriter"].toString();
                c.composer = v["composer"].toString(); c.arranger = v["arranger"].toString(); c.isrc = v["isrc"].toString();
                c.preEmphasis = (bool) v["pre"]; c.copyPermitted = (bool) v["copy"];
                c.index01Shift = (int) v["i01"]; c.index00Shift = v["i00s"].isVoid() ? kIndexAuto : (int) v["i00s"];
                if (! c.editId.isNull()) ddp.clips.push_back (c);
            }
    }
}

// ----------------------------------------------------------------------------- lengths
juce::int64 masterLengthSamples (const EditDef& e, double tailSeconds)
{
    if (e.isEmpty() || e.sampleRate <= 0.0) return 0;
    const juce::int64 n = e.lengthSamples() - e.firstSample();
    return juce::jmax ((juce::int64) 0, n) + (juce::int64) std::llround (tailSeconds * e.sampleRate);
}

juce::int64 convertedLength (juce::int64 samples, double fromRate, double toRate)
{
    if (std::abs (fromRate - toRate) < 0.5 || fromRate <= 0.0 || toRate <= 0.0) return samples;
    return (juce::int64) std::floor ((double) samples * toRate / fromRate + 0.5);       // the same rounding as SincResampler::outputLength
}

// ----------------------------------------------------------------------------- Auto PQ
PqLayout computePq (const DdpDisc& disc, const std::vector<juce::int64>& frames)
{
    PqLayout L;
    int pos = 0, number = 0, prevIndex01 = 0;
    for (size_t i = 0; i < disc.clips.size(); ++i)
    {
        const auto& c = disc.clips[i];
        if (! c.include) continue;
        const juce::int64 fr = i < frames.size() ? frames[i] : 0;
        if (fr <= 0) { L.warnings.add (c.title.isNotEmpty() ? "'" + c.title + "' is empty and is left out." : "An empty edit is left out."); continue; }
        ++number;
        PqTrack t;
        t.clipIndex = (int) i; t.editId = c.editId; t.number = number; t.frames = fr;
        t.gapSectors = number == 1 ? juce::jmax (150, c.gapSectors) : juce::jmax (0, c.gapSectors);
        t.audioStart = pos + t.gapSectors;
        t.lengthSectors = (int) ((fr + kSamplesPerSector - 1) / kSamplesPerSector);
        t.endSector = t.audioStart + t.lengthSectors;
        // INDEX 01 normally sits where the audio starts; it can be moved (earlier = the player starts in the pause, later = it skips the first bit)
        const int lo01 = number == 1 ? 150 : prevIndex01 + 1;
        t.index01 = juce::jlimit (lo01, juce::jmax (lo01, t.endSector - 1), t.audioStart + c.index01Shift);
        // INDEX 00 is the end of the previous track (the countdown to this one starts there). It is placed independently of INDEX 01 but never after it;
        // when they are together there is no countdown and the tracks run on seamlessly.
        if (number == 1) t.index00 = 0;
        else if (c.index00Shift == kIndexAuto) t.index00 = (t.gapSectors > 0 && pos < t.index01) ? pos : -1;
        else
        {
            const int i0 = juce::jlimit (prevIndex01 + 1, juce::jmax (prevIndex01 + 1, t.index01), t.audioStart + c.index00Shift);
            t.index00 = i0 < t.index01 ? i0 : -1;
        }
        prevIndex01 = t.index01;
        pos = t.endSector;
        L.tracks.push_back (t);
    }
    L.leadOut = L.tracks.empty() ? 150 : pos + juce::jmax (0, disc.endPadSectors);

    if (L.tracks.size() > 99) L.warnings.add ("A CD holds 99 tracks at most (there are " + juce::String ((int) L.tracks.size()) + ").");
    if (L.leadOut > kMaxCdSectors) L.warnings.add ("The disc is " + sectorsToMsf (L.leadOut) + " long: more than the 79:57 an 80 minute CD holds.");
    for (auto& t : L.tracks)
    {
        const auto& c = disc.clips[(size_t) t.clipIndex];
        const auto nm = "Track " + juce::String (t.number) + (c.title.isNotEmpty() ? " (" + c.title + ")" : juce::String());
        if (t.lengthSectors < 300) L.warnings.add (nm + " is shorter than 4 seconds (the Red Book minimum).");
        if (! isValidIsrc (c.isrc)) L.warnings.add (nm + ": the ISRC is not in the form CC-XXX-YY-NNNNN.");
    }
    if (! isValidUpc (disc.upc)) L.warnings.add ("The UPC/EAN must be 13 digits with a correct check digit.");
    return L;
}

bool isValidUpc (const juce::String& upc)
{
    if (upc.isEmpty()) return true;
    if (upc.length() != 13) return false;
    int sum = 0;
    for (int i = 0; i < 13; ++i)
    {
        if (! juce::CharacterFunctions::isDigit (upc[i])) return false;
        const int d = upc[i] - '0';
        if (i < 12) sum += (i % 2 == 0) ? d : 3 * d;
        else return d == (10 - sum % 10) % 10;
    }
    return false;
}

juce::String compactIsrc (const juce::String& isrc) { return isrc.removeCharacters ("- ").toUpperCase(); }

bool isValidIsrc (const juce::String& isrc)
{
    const auto s = compactIsrc (isrc);
    if (s.isEmpty()) return true;
    if (s.length() != 12) return false;
    for (int i = 0; i < 12; ++i)
    {
        const auto c = s[i];
        const bool letter = c >= 'A' && c <= 'Z', digit = c >= '0' && c <= '9';
        if (i < 2 && ! letter) return false;
        if (i >= 2 && i < 5 && ! (letter || digit)) return false;
        if (i >= 5 && ! digit) return false;
    }
    return true;
}

juce::String toLatin1Text (const juce::String& s)
{
    juce::String out;
    for (auto c : s)
    {
        if (c < 32) out << ' ';
        else if (c > 255) out << '?';
        else out << c;
    }
    return out;
}
} // namespace td

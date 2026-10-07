#include "Ddp.h"
#include <map>

namespace td
{
namespace ddp
{
namespace
{
constexpr int kDdpIdLen = 128, kDdpMsLen = 128, kPqLen = 64, kCtPackLen = 18, kCtTextLen = 12, kCtMaxPacks = 255;
constexpr int kChunkFrames = 16384;
const char* const kImageName = "IMAGE.DAT";
const char* const kCdTextName = "CDTEXT.BIN";
const char* const kPqName = "PQDESCR";

void setField (juce::MemoryBlock& rec, int offset, int width, const juce::String& text)
{
    const auto t = toLatin1Text (text);
    for (int i = 0; i < width; ++i) rec[(size_t) (offset + i)] = i < t.length() ? (char) (juce::uint8) t[i] : ' ';
}
juce::MemoryBlock spaces (int n) { juce::MemoryBlock b ((size_t) n); b.fillWith (' '); return b; }

juce::String sectorDigits (int sector)             // MMSSFF
{
    const int m = sector / (75 * 60), s = (sector / 75) % 60, f = sector % 75;
    return juce::String (m).paddedLeft ('0', 2) + juce::String (s).paddedLeft ('0', 2) + juce::String (f).paddedLeft ('0', 2);
}
int sectorFromDigits (const juce::String& d) { return d.length() < 6 ? -1 : d.substring (0, 2).getIntValue() * 75 * 60 + d.substring (2, 4).getIntValue() * 75 + d.substring (4, 6).getIntValue(); }

int controlOf (const DdpClip& c) { return (c.preEmphasis ? 1 : 0) | (c.copyPermitted ? 2 : 0); }

juce::MemoryBlock pqRecord (const juce::String& track, const juce::String& index, int sector, int control, const juce::String& isrc, const juce::String& catalog)
{
    auto rec = spaces (kPqLen);
    setField (rec, 0, 4, "VVVS"); setField (rec, 4, 2, track); setField (rec, 6, 2, index); setField (rec, 10, 6, sectorDigits (sector));
    setField (rec, 16, 2, juce::String::toHexString ((control << 4) | 1).toUpperCase().paddedLeft ('0', 2));
    if (isrc.isNotEmpty()) setField (rec, 20, 12, compactIsrc (isrc));
    if (catalog.isNotEmpty()) setField (rec, 32, 13, catalog);
    return rec;
}

juce::MemoryBlock msRecord (const juce::String& kind, int size, const juce::String& name, const juce::String& fileName, int offset = -1,
                            const juce::String& dataType = {}, int pregap = -1)
{
    auto rec = spaces (kDdpMsLen);
    setField (rec, 0, 6, kind);
    setField (rec, 14, 8, juce::String (size).paddedLeft ('0', 8));
    if (offset >= 0) setField (rec, 22, 8, juce::String (offset).paddedLeft ('0', 8));
    setField (rec, 30, 8, name);
    if (dataType.isNotEmpty()) setField (rec, 38, 4, dataType);
    if (pregap >= 0) setField (rec, 47, 3, juce::String (pregap));
    setField (rec, 71, 3, "017");
    setField (rec, 74, 54, fileName);
    return rec;
}

/** Streams the 16-bit stereo programme of the disc (silence, clips, padding), starting at absolute sector 'fromSector'. */
juce::String streamProgram (const PqLayout& L, const std::vector<juce::File>& files, int fromSector,
                            const std::function<bool (const juce::int16*, int)>& sink, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    if (files.size() != L.tracks.size()) return "The number of audio files does not match the number of tracks.";
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    const double totalSectors = juce::jmax (1, L.leadOut - fromSector);
    juce::int64 skip = (juce::int64) fromSector * kSamplesPerSector;
    int doneSectors = 0;
    std::vector<juce::int16> zeros ((size_t) kSamplesPerSector * 64 * 2, 0);
    bool failed = false;
    auto emit = [&] (const juce::int16* d, int n)
    {
        if (skip >= n) { skip -= n; return true; }
        const int off = (int) skip; skip = 0;
        if (! sink (d + off * 2, n - off)) failed = true;
        return ! failed;
    };
    auto silence = [&] (juce::int64 frames)
    {
        while (frames > 0 && ! failed)
        {
            const int n = (int) juce::jmin (frames, (juce::int64) kSamplesPerSector * 64);
            if (! emit (zeros.data(), n)) return false;
            frames -= n;
        }
        return ! failed;
    };
    auto report = [&] { if (progress != nullptr) progress->store ((float) juce::jlimit (0.0, 1.0, doneSectors / totalSectors)); };
    int pos = 0;
    for (size_t i = 0; i < L.tracks.size(); ++i)
    {
        const auto& t = L.tracks[i];
        if (! silence ((juce::int64) (t.audioStart - pos) * kSamplesPerSector)) return "Cannot write the output.";
        doneSectors += juce::jmax (0, t.audioStart - pos);
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (files[i]));
        if (r == nullptr) return "Cannot read " + files[i].getFullPathName();
        if (std::abs (r->sampleRate - 44100.0) > 0.5 || r->numChannels != 2) return files[i].getFileName() + " is not 44.1 kHz stereo.";
        if (r->bitsPerSample != 16 || r->usesFloatingPointData) return files[i].getFileName() + " is not 16 bit (a disc master is always 44.1 kHz, 16 bit).";
        if (r->lengthInSamples != t.frames) return files[i].getFileName() + " has a different length than the layout expects.";
        juce::AudioBuffer<float> buf (2, kChunkFrames);
        std::vector<juce::int16> pcm ((size_t) kChunkFrames * 2);
        for (juce::int64 p = 0; p < t.frames; p += kChunkFrames)
        {
            if (cancel != nullptr && cancel->load()) return "Cancelled.";
            const int n = (int) juce::jmin ((juce::int64) kChunkFrames, t.frames - p);
            r->read (&buf, 0, n, p, true, true);
            const float* l = buf.getReadPointer (0); const float* rr = buf.getReadPointer (1);
            for (int k = 0; k < n; ++k)
            {
                pcm[(size_t) k * 2]     = (juce::int16) juce::jlimit (-32768, 32767, (int) std::lrintf (l[k] * 32768.0f));
                pcm[(size_t) k * 2 + 1] = (juce::int16) juce::jlimit (-32768, 32767, (int) std::lrintf (rr[k] * 32768.0f));
            }
            if (! emit (pcm.data(), n)) return "Cannot write the output.";
            doneSectors = t.audioStart + (int) juce::jmin ((juce::int64) t.lengthSectors, (p + n) / kSamplesPerSector);
            report();
        }
        if (! silence ((juce::int64) t.lengthSectors * kSamplesPerSector - t.frames)) return "Cannot write the output.";
        pos = t.endSector; doneSectors = pos; report();
    }
    if (L.leadOut > pos && ! silence ((juce::int64) (L.leadOut - pos) * kSamplesPerSector)) return "Cannot write the output.";      // the End of CD flag was dragged later: silence up to it
    return {};
}


/** MD5 of a file (RFC 1321), for CHECKSUM.MD5. */
juce::String md5OfFile (const juce::File& file)
{
    static const juce::uint32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    juce::uint32 a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    juce::FileInputStream in (file);
    if (in.failedToOpen()) return {};
    juce::uint64 total = 0;
    juce::uint8 block[64]; int fill = 0;
    auto process = [&] (const juce::uint8* m)
    {
        juce::uint32 w[16];
        for (int i = 0; i < 16; ++i) w[i] = (juce::uint32) m[i * 4] | ((juce::uint32) m[i * 4 + 1] << 8) | ((juce::uint32) m[i * 4 + 2] << 16) | ((juce::uint32) m[i * 4 + 3] << 24);
        juce::uint32 a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i)
        {
            juce::uint32 f; int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            f = f + a + K[i] + w[g];
            a = d; d = c; c = b; b = b + ((f << R[i]) | (f >> (32 - R[i])));
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    };
    std::vector<juce::uint8> buf (1 << 20);
    for (;;)
    {
        const int n = in.read (buf.data(), (int) buf.size());
        if (n <= 0) break;
        total += (juce::uint64) n;
        for (int i = 0; i < n; ++i) { block[fill++] = buf[(size_t) i]; if (fill == 64) { process (block); fill = 0; } }
    }
    block[fill++] = 0x80;
    if (fill > 56) { while (fill < 64) block[fill++] = 0; process (block); fill = 0; }
    while (fill < 56) block[fill++] = 0;
    const juce::uint64 bits = total * 8;
    for (int i = 0; i < 8; ++i) block[56 + i] = (juce::uint8) (bits >> (8 * i));
    process (block);
    juce::String hex;
    for (auto v : { a0, b0, c0, d0 }) for (int i = 0; i < 4; ++i) hex << juce::String::toHexString ((int) ((v >> (8 * i)) & 0xff)).paddedLeft ('0', 2);
    return hex;
}

juce::String quoteCue (const juce::String& s) { return "\"" + toLatin1Text (s).replaceCharacter ('"', '\'') + "\""; }
} // namespace

juce::uint16 crc16 (const juce::uint8* data, size_t size)
{
    juce::uint32 crc = 0;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= (juce::uint32) data[i] << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
        crc &= 0xffffu;
    }
    return (juce::uint16) (crc ^ 0xffffu);
}

juce::MemoryBlock buildDdpId (const DdpDisc& d)
{
    auto rec = spaces (kDdpIdLen);
    setField (rec, 0, 8, "DDP 2.00");
    if (d.upc.isNotEmpty()) setField (rec, 8, 13, d.upc);
    setField (rec, 87, 2, "CD");
    return rec;
}

juce::MemoryBlock buildDdpMs (int pqBytes, int imageSectors, int cdTextBytes)
{
    juce::MemoryBlock out;
    auto add = [&] (const juce::MemoryBlock& b) { out.append (b.getData(), b.getSize()); };
    add (msRecord ("VVVMS0", pqBytes, "PQ DESCR", kPqName));
    add (msRecord ("VVVMD0", imageSectors, "", kImageName, 0, "DA70", 150));
    if (cdTextBytes > 0) add (msRecord ("VVVMS0", cdTextBytes, "CDTEXT", kCdTextName));
    return out;
}

juce::MemoryBlock buildPqDescr (const DdpDisc& d, const PqLayout& L)
{
    juce::MemoryBlock out;
    auto add = [&] (const juce::MemoryBlock& b) { out.append (b.getData(), b.getSize()); };
    if (L.tracks.empty()) return out;
    const auto cat = d.upc;
    add (pqRecord ("00", "00", 0, controlOf (d.clips[(size_t) L.tracks.front().clipIndex]), {}, cat));
    for (auto& t : L.tracks)
    {
        const auto& c = d.clips[(size_t) t.clipIndex];
        const auto num = juce::String (t.number).paddedLeft ('0', 2);
        if (t.index00 >= 0) add (pqRecord (num, "00", t.index00, controlOf (c), t.number == 1 ? c.isrc : juce::String(), {}));
        add (pqRecord (num, "01", t.index01, controlOf (c), t.number == 1 && t.index00 >= 0 ? juce::String() : c.isrc, {}));
    }
    const auto lastCtrl = controlOf (d.clips[(size_t) L.tracks.back().clipIndex]);
    const auto lead = pqRecord ("AA", "01", L.leadOut, lastCtrl, {}, cat);
    add (lead); add (lead);
    return out;
}

namespace
{
struct PackList
{
    juce::MemoryBlock data; int seq = 0;
    void addPack (int type, int number, const juce::uint8* text12, int charPos)
    {
        juce::uint8 p[kCtPackLen] = {};
        p[0] = (juce::uint8) type; p[1] = (juce::uint8) number; p[2] = (juce::uint8) (seq & 0xff); p[3] = (juce::uint8) (juce::jmin (charPos, 15));
        std::memcpy (p + 4, text12, (size_t) kCtTextLen);
        const auto crc = crc16 (p, 16);
        p[16] = (juce::uint8) (crc >> 8); p[17] = (juce::uint8) (crc & 0xff);
        data.append (p, kCtPackLen);
        ++seq;
    }
    /** items: (track number, text). The texts follow each other (each ends with a 0 byte) and flow over the packs. */
    int addText (int type, const std::vector<std::pair<int, juce::String>>& items)
    {
        std::vector<juce::uint8> stream; std::vector<std::pair<int, int>> owners;
        for (auto& it : items)
        {
            const auto t = toLatin1Text (it.second);
            for (int i = 0; i <= t.length(); ++i) owners.push_back ({ it.first, i });
            for (int i = 0; i < t.length(); ++i) stream.push_back ((juce::uint8) t[i]);
            stream.push_back (0);
        }
        int made = 0;
        for (size_t off = 0; off < stream.size(); off += (size_t) kCtTextLen)
        {
            juce::uint8 chunk[kCtTextLen] = {};
            const size_t n = juce::jmin ((size_t) kCtTextLen, stream.size() - off);
            std::memcpy (chunk, stream.data() + off, n);
            addPack (type, owners[off].first, chunk, owners[off].second);
            ++made;
        }
        return made;
    }
};
} // namespace

juce::MemoryBlock buildCdText (const DdpDisc& d, const PqLayout& L, juce::String& error)
{
    error = {};
    PackList packs;
    int counts[16] = {};
    struct Field { int type; juce::String album; std::function<juce::String (const DdpClip&)> of; };
    const std::vector<Field> fields {
        { 0x80, d.title,      [] (const DdpClip& c) { return c.title; } },
        { 0x81, d.performer,  [&] (const DdpClip& c) { return c.performer.isNotEmpty() ? c.performer : d.performer; } },
        { 0x82, d.songwriter, [] (const DdpClip& c) { return c.songwriter; } },
        { 0x83, d.composer,   [] (const DdpClip& c) { return c.composer; } },
        { 0x84, d.arranger,   [] (const DdpClip& c) { return c.arranger; } } };
    for (auto& f : fields)
    {
        std::vector<std::pair<int, juce::String>> items { { 0, f.album } };
        bool any = f.album.isNotEmpty();
        for (auto& t : L.tracks) { auto s = f.of (d.clips[(size_t) t.clipIndex]); any = any || s.isNotEmpty(); items.push_back ({ t.number, s }); }
        if (! any) continue;
        counts[f.type - 0x80] = packs.addText (f.type, items);
    }
    if (packs.data.getSize() == 0 || L.tracks.empty()) return {};
    const int total = (int) (packs.data.getSize() / kCtPackLen) + 3;
    if (total > kCtMaxPacks) { error = "The CD-Text needs " + juce::String (total) + " packs but at most " + juce::String (kCtMaxPacks) + " fit. Shorten the longest titles or names."; return {}; }
    counts[15] = 3;
    juce::uint8 info[36] = {};
    info[0] = 0x00; info[1] = (juce::uint8) L.tracks.front().number; info[2] = (juce::uint8) L.tracks.back().number; info[3] = 0;
    for (int i = 0; i < 16; ++i) info[4 + i] = (juce::uint8) juce::jmin (counts[i], 255);
    info[20] = (juce::uint8) ((total - 1) & 0xff);
    info[28] = (juce::uint8) (d.languageCode & 0xff);
    for (int i = 0; i < 3; ++i) packs.addPack (0x8f, i, info + i * 12, 0);
    return packs.data;
}

juce::String writeFileset (const juce::File& folder, const DdpDisc& d, const PqLayout& L, const std::vector<juce::File>& files,
                           const juce::File& zipFile, std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    if (L.tracks.empty()) return "There are no tracks on the disc.";
    if (! folder.createDirectory()) return "Cannot create " + folder.getFullPathName();
    juce::String err;
    const auto cdText = buildCdText (d, L, err);
    if (err.isNotEmpty()) return err;
    const auto pq = buildPqDescr (d, L);
    for (auto n : { "DDPID", "DDPMS", kPqName, kCdTextName, kImageName, "CHECKSUM.MD5" }) folder.getChildFile (n).deleteFile();

    auto writeBlock = [&] (const char* name, const juce::MemoryBlock& b) -> bool
    {
        auto f = folder.getChildFile (name);
        return f.replaceWithData (b.getData(), b.getSize());
    };
    if (! writeBlock ("DDPID", buildDdpId (d))) return "Cannot write DDPID.";
    if (! writeBlock (kPqName, pq)) return "Cannot write PQDESCR.";
    if (cdText.getSize() > 0 && ! writeBlock (kCdTextName, cdText)) return "Cannot write CDTEXT.BIN.";
    if (! writeBlock ("DDPMS", buildDdpMs ((int) pq.getSize(), L.leadOut, (int) cdText.getSize()))) return "Cannot write DDPMS.";

    {
        juce::FileOutputStream out (folder.getChildFile (kImageName));
        if (out.failedToOpen()) return "Cannot write IMAGE.DAT.";
        out.setPosition (0); out.truncate();
        const auto e = streamProgram (L, files, 0, [&] (const juce::int16* data, int frames)
        {
#if JUCE_BIG_ENDIAN
            std::vector<juce::int16> sw ((size_t) frames * 2);
            for (size_t i = 0; i < sw.size(); ++i) sw[i] = (juce::int16) juce::ByteOrder::swap ((juce::uint16) data[i]);
            return out.write (sw.data(), (size_t) frames * 4);
#else
            return out.write (data, (size_t) frames * 4);
#endif
        }, progress, cancel);
        if (e.isNotEmpty()) { out.flush(); folder.getChildFile (kImageName).deleteFile(); return e; }
        out.flush();
    }

    // CHECKSUM.MD5: the usual md5sum format
    {
        juce::String sums;
        for (auto n : { "DDPID", "DDPMS", kPqName, kCdTextName, kImageName })
        {
            auto f = folder.getChildFile (n);
            if (f.existsAsFile()) sums << md5OfFile (f) << " *" << n << "\n";
        }
        if (! folder.getChildFile ("CHECKSUM.MD5").replaceWithText (sums)) return "Cannot write CHECKSUM.MD5.";
    }
    if (zipFile != juce::File())
    {
        juce::ZipFile::Builder b;
        for (auto n : { "DDPID", "DDPMS", kPqName, kCdTextName, kImageName, "CHECKSUM.MD5" })
        {
            auto f = folder.getChildFile (n);
            if (f.existsAsFile()) b.addFile (f, 0, n);
        }
        zipFile.deleteFile();
        juce::FileOutputStream zo (zipFile);
        if (zo.failedToOpen()) return "Cannot write " + zipFile.getFullPathName();
        if (! b.writeToStream (zo, nullptr)) return "Cannot write the zip file.";
    }
    return {};
}

juce::String cueSheetText (const juce::String& wavName, const DdpDisc& d, const PqLayout& L)
{
    juce::String s;
    s << "REM GENERATOR Fermata\r\n";
    if (d.upc.isNotEmpty()) s << "CATALOG " << d.upc << "\r\n";
    if (d.title.isNotEmpty()) s << "TITLE " << quoteCue (d.title) << "\r\n";
    if (d.performer.isNotEmpty()) s << "PERFORMER " << quoteCue (d.performer) << "\r\n";
    if (d.songwriter.isNotEmpty()) s << "SONGWRITER " << quoteCue (d.songwriter) << "\r\n";
    s << "FILE " << quoteCue (wavName) << " WAVE\r\n";
    for (auto& t : L.tracks)
    {
        const auto& c = d.clips[(size_t) t.clipIndex];
        s << "  TRACK " << juce::String (t.number).paddedLeft ('0', 2) << " AUDIO\r\n";
        if (c.title.isNotEmpty()) s << "    TITLE " << quoteCue (c.title) << "\r\n";
        const auto perf = c.performer.isNotEmpty() ? c.performer : d.performer;
        if (perf.isNotEmpty()) s << "    PERFORMER " << quoteCue (perf) << "\r\n";
        if (c.songwriter.isNotEmpty()) s << "    SONGWRITER " << quoteCue (c.songwriter) << "\r\n";
        if (compactIsrc (c.isrc).isNotEmpty()) s << "    ISRC " << compactIsrc (c.isrc) << "\r\n";
        if (c.preEmphasis || c.copyPermitted) s << "    FLAGS" << (c.copyPermitted ? " DCP" : "") << (c.preEmphasis ? " PRE" : "") << "\r\n";
        if (t.number > 1 && t.index00 >= 0) s << "    INDEX 00 " << sectorsToMsf (t.index00 - 150) << "\r\n";
        s << "    INDEX 01 " << sectorsToMsf (t.index01 - 150) << "\r\n";
    }
    return s;
}

juce::String writeWavAndCue (const juce::File& wavFile, const juce::File& cueFile, const DdpDisc& d, const PqLayout& L, const std::vector<juce::File>& files,
                             std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    if (L.tracks.empty()) return "There are no tracks on the disc.";
    wavFile.getParentDirectory().createDirectory();
    juce::FileOutputStream out (wavFile);
    if (out.failedToOpen()) return "Cannot write " + wavFile.getFullPathName();
    out.setPosition (0); out.truncate();
    auto w32 = [&] (juce::uint32 v) { out.writeInt ((int) v); };
    auto w16 = [&] (juce::uint16 v) { out.writeShort ((short) v); };
    out.write ("RIFF", 4); w32 (0); out.write ("WAVE", 4); out.write ("fmt ", 4); w32 (16); w16 (1); w16 (2); w32 (44100); w32 (44100 * 4); w16 (4); w16 (16);
    out.write ("data", 4); w32 (0);
    juce::int64 bytes = 0;
    const auto e = streamProgram (L, files, 150, [&] (const juce::int16* data, int frames)
    {
#if JUCE_BIG_ENDIAN
        std::vector<juce::int16> sw ((size_t) frames * 2);
        for (size_t i = 0; i < sw.size(); ++i) sw[i] = (juce::int16) juce::ByteOrder::swap ((juce::uint16) data[i]);
        bytes += (juce::int64) frames * 4; return out.write (sw.data(), (size_t) frames * 4);
#else
        bytes += (juce::int64) frames * 4; return out.write (data, (size_t) frames * 4);
#endif
    }, progress, cancel);
    if (e.isNotEmpty()) { out.flush(); wavFile.deleteFile(); return e; }
    out.setPosition (4); w32 ((juce::uint32) (36 + bytes));
    out.setPosition (40); w32 ((juce::uint32) bytes);
    out.flush();
    if (! cueFile.replaceWithText (cueSheetText (wavFile.getFileName(), d, L), false, false, "\r\n")) return "Cannot write " + cueFile.getFullPathName();
    return {};
}

bool verifyFileset (const juce::File& folder, juce::String& report)
{
    juce::StringArray problems, info;
    auto load = [&] (const char* name, juce::MemoryBlock& mb) { return folder.getChildFile (name).loadFileAsData (mb); };
    juce::MemoryBlock id, ms;
    if (! load ("DDPID", id) || id.getSize() != (size_t) kDdpIdLen) problems.add ("DDPID is missing or not 128 bytes.");
    else if (std::memcmp (id.getData(), "DDP 2.00", 8) != 0) problems.add ("DDPID does not start with 'DDP 2.00'.");
    if (! load ("DDPMS", ms) || ms.getSize() == 0 || ms.getSize() % (size_t) kDdpMsLen != 0) { problems.add ("DDPMS is missing or its size is not a multiple of 128 bytes."); report = problems.joinIntoString ("\n"); return false; }

    int pqBytes = 0, imageSectors = 0, textBytes = 0;
    juce::String pqName, imageName, textName;
    for (size_t r = 0; r + kDdpMsLen <= ms.getSize(); r += (size_t) kDdpMsLen)
    {
        const auto rec = juce::String::fromUTF8 (static_cast<const char*> (ms.getData()) + r, kDdpMsLen);
        const auto kind = rec.substring (0, 6), name = rec.substring (74, 128).trim();
        const int size = rec.substring (14, 22).getIntValue();
        if (kind == "VVVMD0") { imageSectors = size; imageName = name; }
        else if (name == kPqName) { pqBytes = size; pqName = name; }
        else if (name == kCdTextName) { textBytes = size; textName = name; }
    }
    if (imageName.isEmpty() || pqName.isEmpty()) problems.add ("DDPMS does not list both the audio image and the PQ descriptor.");

    juce::MemoryBlock pq;
    int leadOut = -1, trackCount = 0, firstStart = -1, lastIndex01 = -1;
    if (pqName.isNotEmpty())
    {
        if (! load (pqName.toRawUTF8(), pq) || (int) pq.getSize() != pqBytes || pqBytes % kPqLen != 0) problems.add ("PQDESCR has the wrong size.");
        else
            for (int r = 0; r < pqBytes / kPqLen; ++r)
            {
                const auto rec = juce::String::fromUTF8 (static_cast<const char*> (pq.getData()) + r * kPqLen, kPqLen);
                if (rec.substring (0, 4) != "VVVS") { problems.add ("PQ record " + juce::String (r + 1) + " is damaged."); continue; }
                const auto trk = rec.substring (4, 6), idx = rec.substring (6, 8); const int sec = sectorFromDigits (rec.substring (10, 16));
                if (trk == "AA") { leadOut = sec; continue; }
                if (idx == "01" && trk != "00")
                {
                    ++trackCount;
                    if (firstStart < 0) firstStart = sec;
                    if (sec <= lastIndex01) problems.add ("Track " + trk + " does not start after the previous one.");
                    lastIndex01 = sec;
                    info.add ("Track " + trk + " starts at " + sectorsToMsf (sec));
                }
            }
        if (firstStart >= 0 && firstStart < 150) problems.add ("The first track starts before 00:02:00.");
        if (leadOut < 0) problems.add ("There is no lead-out entry.");
        else if (leadOut != imageSectors) problems.add ("The lead-out (" + sectorsToMsf (leadOut) + ") is not at the end of the audio image.");
        if (trackCount > 99) problems.add ("More than 99 tracks.");
    }
    if (imageName.isNotEmpty())
    {
        const auto f = folder.getChildFile (imageName);
        if (! f.existsAsFile() || f.getSize() != (juce::int64) imageSectors * kBytesPerSector) problems.add ("IMAGE.DAT is missing or its size does not match the map.");
    }
    if (textName.isNotEmpty())
    {
        juce::MemoryBlock tx;
        if (! load (textName.toRawUTF8(), tx) || (int) tx.getSize() != textBytes || textBytes % kCtPackLen != 0) problems.add ("CDTEXT.BIN has the wrong size.");
        else
        {
            int bad = 0;
            for (int p = 0; p < textBytes / kCtPackLen; ++p)
            {
                const auto* b = static_cast<const juce::uint8*> (tx.getData()) + p * kCtPackLen;
                if (crc16 (b, 16) != (juce::uint16) ((b[16] << 8) | b[17])) ++bad;
            }
            if (bad > 0) problems.add (juce::String (bad) + " CD-Text pack(s) fail their checksum.");
            info.add ("CD-Text: " + juce::String (textBytes / kCtPackLen) + " packs");
        }
    }
    else info.add ("No CD-Text");
    {
        juce::StringArray lines; lines.addLines (folder.getChildFile ("CHECKSUM.MD5").loadFileAsString());
        int checked = 0;
        for (auto& l : lines)
        {
            const auto hash = l.upToFirstOccurrenceOf (" *", false, false).trim(), name = l.fromFirstOccurrenceOf (" *", false, false).trim();
            if (hash.isEmpty() || name.isEmpty()) continue;
            const auto f = folder.getChildFile (name);
            if (! f.existsAsFile() || md5OfFile (f) != hash) problems.add ("The checksum of " + name + " does not match.");
            ++checked;
        }
        if (checked == 0) problems.add ("CHECKSUM.MD5 is missing.");
    }
    juce::String out = "Tracks: " + juce::String (trackCount) + ", length " + (leadOut >= 0 ? sectorsToMsf (leadOut) : juce::String ("?")) + "\n" + info.joinIntoString ("\n");
    if (problems.isEmpty()) out << "\nThe fileset is consistent.";
    else out << "\nPROBLEMS:\n" << problems.joinIntoString ("\n");
    report = out;
    return problems.isEmpty();
}
} // namespace ddp
} // namespace td

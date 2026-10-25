#pragma once
#include "Common.h"

namespace td
{
/** Mends WAV files that were being written when the program (or the computer) stopped.
    A file that is still being recorded has a header that says "no audio yet" (the real length is only written when the recording is finished), so a
    program that does not know better sees an empty file. This looks at the file itself: the audio is all there, so the header is simply put right.
    A file of more than 4 GB (two hours of stereo at 96 kHz, 41 minutes of a six-channel track) cannot say its length in a plain WAV header: it is
    given the RF64 header (a 'ds64' chunk with 64-bit sizes) in the space the writer keeps free for exactly that, which every modern program reads. */
namespace wavrepair
{
struct Result
{
    bool wasBroken = false;      // the header did not match the audio that is in the file (and has now been put right)
    bool ok = false;             // a readable PCM / float WAV, whole or mended
    bool rf64 = false;           // the mended file has (or needed) an RF64 header
    juce::int64 frames = 0;      // sample frames of audio in the file
    double sampleRate = 0.0;
    int channels = 0;
};

inline juce::uint32 rd32 (const unsigned char* p) { return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24); }
inline juce::uint16 rd16 (const unsigned char* p) { return (juce::uint16) (p[0] | (p[1] << 8)); }
inline void wr32 (juce::OutputStream& o, juce::uint32 v) { o.writeInt ((int) v); }
inline void wr64 (juce::OutputStream& o, juce::int64 v) { o.writeInt64 (v); }

/** 'repair' false: only looks. 'extendToFileEnd': the file was being recorded (by Fermata) when the program stopped, so a header that says LESS than
    what the file holds is stale too (the header is refreshed every few seconds while recording) and the whole file is kept. Never for a file from
    elsewhere: a WAV with chunks after its audio would be read as audio. */
inline Result inspect (const juce::File& f, bool repair, bool extendToFileEnd = false)
{
    Result r;
    juce::FileInputStream in (f);
    if (in.failedToOpen()) return r;
    const juce::int64 len = in.getTotalLength();
    unsigned char hdr[12];
    if (len < 44 || in.read (hdr, 12) != 12 || std::memcmp (hdr + 8, "WAVE", 4) != 0) return r;
    const bool isRf64 = std::memcmp (hdr, "RF64", 4) == 0;
    if (! isRf64 && std::memcmp (hdr, "RIFF", 4) != 0) return r;
    juce::int64 pos = 12, dataOffset = -1, junkOffset = -1, ds64Offset = -1; juce::uint32 declared = 0, junkSize = 0, ds64Size = 0; int blockAlign = 0;
    juce::int64 ds64Data = -1;
    while (pos + 8 <= len)
    {
        unsigned char ch[8];
        in.setPosition (pos);
        if (in.read (ch, 8) != 8) break;
        const juce::uint32 size = rd32 (ch + 4);
        if (std::memcmp (ch, "fmt ", 4) == 0 && size >= 16)
        {
            unsigned char fm[16];
            if (in.read (fm, 16) != 16) break;
            r.channels = rd16 (fm + 2); r.sampleRate = (double) rd32 (fm + 4); blockAlign = rd16 (fm + 12);
        }
        else if (std::memcmp (ch, "JUNK", 4) == 0 && junkOffset < 0) { junkOffset = pos; junkSize = size; }
        else if (std::memcmp (ch, "ds64", 4) == 0 && size >= 28)
        {
            unsigned char d[28];
            if (in.read (d, 28) != 28) break;
            ds64Offset = pos; ds64Size = size; ds64Data = (juce::int64) ((juce::uint64) rd32 (d + 8) | ((juce::uint64) rd32 (d + 12) << 32));
        }
        else if (std::memcmp (ch, "data", 4) == 0) { dataOffset = pos + 8; declared = size; break; }
        pos += 8 + (juce::int64) size + (size & 1);
    }
    if (dataOffset < 0 || blockAlign <= 0 || r.channels <= 0) return r;
    const juce::int64 avail = len - dataOffset;
    juce::int64 good = isRf64 && declared == 0xffffffffu && ds64Data >= 0 ? ds64Data : (juce::int64) declared;
    const bool stale = extendToFileEnd && good > 0 && good <= avail - blockAlign;       // (a pad byte at the end is not audio; written so that a nonsense 64-bit size cannot overflow: 3.0.0)
    // An RF64 file whose 32-bit data size is a real number but whose 64-bit one says something else: the 32-bit one is right (it is only meant to be
    // read when it is 0xffffffff), but most readers - JUCE's among them - go by the 64-bit one regardless, so the two are made to agree.
    const bool ds64Disagrees = isRf64 && ds64Offset == 12 && declared != 0xffffffffu && ds64Data != (juce::int64) declared;
    const bool sizeWrong = good == 0 || (declared == 0xffffffffu && ! isRf64) || good > avail || stale;
    const bool broken = sizeWrong || ds64Disagrees;
    if (sizeWrong) good = avail;                                        // (the sizes only disagreeing: the 32-bit one stands)
    good -= good % blockAlign;                                          // never half a frame
    r.ok = true; r.wasBroken = broken && good > 0; r.frames = good / blockAlign;
    const bool needsRf64 = good >= 0xffffffffLL || len - 8 >= 0xffffffffLL;
    r.rf64 = isRf64 || needsRf64;
    if (broken && repair && good > 0)
    {
        juce::FileOutputStream out (f);
        if (out.failedToOpen()) { r.ok = false; return r; }
        if (isRf64 && ds64Offset == 12 && ds64Size >= 28)
        {
            // Already RF64 (a recording that had passed 4 GB, or a recorder that writes RF64 whatever the size): readers go by its 64-bit sizes, so
            // those are what is put right, and the 32-bit ones say "see the 64-bit ones". The chunk keeps its size and any table it has. (Until 3.0
            // a small RF64 file was given 32-bit sizes only, which readers ignore: the file still read as empty, or - with a nonsense 64-bit size -
            // could crash the reader.)
            out.setPosition (4);              wr32 (out, 0xffffffffu);
            out.setPosition (20);             wr64 (out, len - 8); wr64 (out, good); wr64 (out, good / blockAlign);
            out.setPosition (dataOffset - 4); wr32 (out, 0xffffffffu);
        }
        else if (! needsRf64)
        {
            if (isRf64) { out.setPosition (0); out.write ("RIFF", 4); }      // marked RF64 but without the 64-bit sizes: under 4 GB a plain header says it all
            out.setPosition (4);              wr32 (out, (juce::uint32) (len - 8));
            out.setPosition (dataOffset - 4); wr32 (out, (juce::uint32) good);
        }
        else
        {
            // 64-bit sizes go in a 'ds64' chunk right after "WAVE". The writer leaves a 'JUNK' chunk there for the purpose (28 bytes, or 52 when the
            // format chunk is the short one); a file that already has a ds64 chunk (it was already RF64 when the recording stopped) keeps it.
            juce::int64 at = ds64Offset;
            juce::uint32 room = 28;
            if (at < 0 && junkOffset == 12 && junkSize >= 28) { at = junkOffset; room = junkSize; }
            if (at != 12)
            {   // no room for 64-bit sizes: say as much as a plain header can (the first 4 GB), rather than a wrapped-round number
                const juce::int64 capped = (0xfffffff0LL / blockAlign) * blockAlign;
                r.frames = capped / blockAlign;
                out.setPosition (4);              wr32 (out, 0xffffffffu);
                out.setPosition (dataOffset - 4); wr32 (out, (juce::uint32) capped);
                out.flush();
                return r;
            }
            out.setPosition (0);
            out.write ("RF64", 4); wr32 (out, 0xffffffffu); out.write ("WAVE", 4);
            out.write ("ds64", 4); wr32 (out, 28);
            wr64 (out, len - 8);                                      // the RIFF size
            wr64 (out, good);                                         // the data size
            wr64 (out, good / blockAlign);                            // the sample count
            wr32 (out, 0);                                            // no table
            if (room >= 36) { out.write ("JUNK", 4); wr32 (out, room - 28 - 8); }    // what is left of the placeholder stays a (smaller) JUNK chunk
            out.setPosition (dataOffset - 4); wr32 (out, 0xffffffffu);
        }
        out.flush();
    }
    return r;
}
} // namespace wavrepair
} // namespace td

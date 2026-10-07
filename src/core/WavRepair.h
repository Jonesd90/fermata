#pragma once
#include "Common.h"

namespace td
{
/** Mends WAV files that were being written when the program (or the computer) stopped.
    A file that is still being recorded has a header that says "no audio yet" (the real length is only written when the recording is finished), so a
    program that does not know better sees an empty file. This looks at the file itself: the audio is all there, so the header is simply put right. */
namespace wavrepair
{
struct Result
{
    bool wasBroken = false;      // the header did not match the audio that is in the file (and has now been put right)
    bool ok = false;             // a readable PCM / float WAV, whole or mended
    juce::int64 frames = 0;      // sample frames of audio in the file
    double sampleRate = 0.0;
    int channels = 0;
};

inline juce::uint32 rd32 (const unsigned char* p) { return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24); }
inline juce::uint16 rd16 (const unsigned char* p) { return (juce::uint16) (p[0] | (p[1] << 8)); }

/** 'repair' false: only looks. */
inline Result inspect (const juce::File& f, bool repair)
{
    Result r;
    juce::FileInputStream in (f);
    if (in.failedToOpen()) return r;
    const juce::int64 len = in.getTotalLength();
    unsigned char hdr[12];
    if (len < 44 || in.read (hdr, 12) != 12 || std::memcmp (hdr, "RIFF", 4) != 0 || std::memcmp (hdr + 8, "WAVE", 4) != 0) return r;
    juce::int64 pos = 12, dataOffset = -1; juce::uint32 declared = 0; int blockAlign = 0;
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
        else if (std::memcmp (ch, "data", 4) == 0) { dataOffset = pos + 8; declared = size; break; }
        pos += 8 + (juce::int64) size + (size & 1);
    }
    if (dataOffset < 0 || blockAlign <= 0 || r.channels <= 0) return r;
    const juce::int64 avail = len - dataOffset;
    juce::int64 good = (juce::int64) declared;
    const bool broken = declared == 0 || declared == 0xffffffffu || (juce::int64) declared > avail;
    if (broken) good = avail;
    good -= good % blockAlign;                                          // never half a frame
    r.ok = true; r.wasBroken = broken && good > 0; r.frames = good / blockAlign;
    if (broken && repair && good > 0)
    {
        juce::FileOutputStream out (f);
        if (out.failedToOpen()) { r.ok = false; return r; }
        out.setPosition (4);              out.writeInt ((int) juce::jmin ((juce::int64) 0xffffffff, len - 8));
        out.setPosition (dataOffset - 4); out.writeInt ((int) good);
        out.flush();
    }
    return r;
}
} // namespace wavrepair
} // namespace td

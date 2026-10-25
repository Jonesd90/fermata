#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace td
{
/** Text from bytes that came from outside (a file someone sent, a field recorder's chunk, a network message): UTF-16 with its byte-order mark,
    UTF-8 with or without one - and when the bytes are not valid UTF-8, the Windows code page (1252) a spreadsheet or an older program saves in.

    Why not JUCE's own: juce::String::fromUTF8 takes the bytes to be valid UTF-8 without looking, and with a character cut off at the end JUCE's
    UTF-8 reader then reads past the end of the text (the XML reader, String::operator[] and the tokenizer all did - a crash, found by the 3.0.0
    fuzz test). juce::String::createStringFromData looks first, but its check (JUCE 8.0.9) does not test that the bytes after a lead byte are
    continuation bytes, so "Caf\xe9 </NAME>" in the Windows code page passes as UTF-8 and the e-acute swallows the next two characters. This one checks
    UTF-8 properly (continuation bytes, no overlong forms, no surrogates, nothing past U+10FFFF). */
inline bool isStrictUtf8 (const juce::uint8* p, size_t n) noexcept
{
    size_t i = 0;
    while (i < n)
    {
        const juce::uint8 b = p[i];
        if (b < 0x80) { ++i; continue; }
        int extra = 0; juce::uint32 cp = 0, least = 0;
        if (b >= 0xC2 && b <= 0xDF)      { extra = 1; cp = b & 0x1Fu; least = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { extra = 2; cp = b & 0x0Fu; least = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { extra = 3; cp = b & 0x07u; least = 0x10000; }
        else return false;                                                  // a continuation byte on its own, C0 / C1, F5 and up
        if (i + (size_t) extra >= n) return false;                          // cut off at the end
        for (int k = 1; k <= extra; ++k)
        {
            const juce::uint8 c = p[i + (size_t) k];
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t) extra + 1;
    }
    return true;
}

inline juce::String textFromBytes (const void* data, size_t size)
{
    if (data == nullptr || size == 0) return {};
    const auto* p = static_cast<const juce::uint8*> (data);
    if (size >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
        return juce::String::createStringFromData (data, (int) juce::jmin (size, (size_t) 0x7fffffff));          // UTF-16 with its mark
    if (size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; size -= 3; }                   // the UTF-8 mark
    if (isStrictUtf8 (p, size))
        return juce::String::fromUTF8 (reinterpret_cast<const char*> (p), (int) juce::jmin (size, (size_t) 0x7fffffff));
    // Windows-1252: Latin-1, with these in 0x80 - 0x9F (the five it leaves out are kept as the control characters they are in Latin-1)
    static const juce::juce_wchar w1252[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                                0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
    std::vector<juce::juce_wchar> chars; chars.reserve (size + 1);
    for (size_t i = 0; i < size; ++i)
    {
        const juce::uint8 b = p[i];
        if (b == 0) break;                                                  // (a juce::String ends at the first nul anyway)
        chars.push_back (b >= 0x80 && b < 0xA0 ? w1252[b - 0x80] : (juce::juce_wchar) b);
    }
    chars.push_back (0);
    return juce::String (juce::CharPointer_UTF32 (reinterpret_cast<const juce::CharPointer_UTF32::CharType*> (chars.data())));
}
} // namespace td

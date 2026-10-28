#pragma once
#include "Common.h"
#include "FileSync.h"
#include <cstdio>
#include <string>
#include <vector>

namespace td
{
/** A small PDF writer for Fermata's printed sheets (the take sheet): A4 pages of text, lines and filled boxes in the three standard Helvetica faces
    (every PDF reader has them, so nothing is embedded). Text is written in the Windows character set (WinAnsi: every West-European letter,
    curly quotes, dashes, the euro); anything else becomes "?". Coordinates are points (1/72 inch) from the TOP-left corner of the page; a text's y
    is its baseline. Pure: no files until save(). */
namespace pdf
{
enum Font : int { Regular = 0, Bold, Italic };
struct Colour { float r = 0.0f, g = 0.0f, b = 0.0f; };
inline Colour grey (float v) { return { v, v, v }; }

/** The character as a WinAnsi byte (0 = not representable). */
inline int winAnsi (juce::juce_wchar c)
{
    if (c >= 32 && c < 127) return (int) c;
    if (c >= 0xA0 && c <= 0xFF) return (int) c;
    switch (c)
    {
        case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83; case 0x201E: return 0x84; case 0x2026: return 0x85;
        case 0x2020: return 0x86; case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89; case 0x0160: return 0x8A;
        case 0x2039: return 0x8B; case 0x0152: return 0x8C; case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
        case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95; case 0x2013: return 0x96; case 0x2014: return 0x97;
        case 0x02DC: return 0x98; case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B; case 0x0153: return 0x9C;
        case 0x017E: return 0x9E; case 0x0178: return 0x9F;
        case 0x266F: return '#';                 // a sharp sign
        case 0x266D: return 'b';                 // a flat sign
        case 0x2212: return '-';                 // a minus sign
        case '\t':   return ' ';
        default: return 0;
    }
}
inline std::string encode (const juce::String& s)
{
    std::string out;
    for (auto p = s.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        const int b = winAnsi (c);
        out.push_back ((char) (b > 0 ? b : '?'));
    }
    return out;
}

/** The width of a text (points) in a face at a size. */
inline float width (const juce::String& text, Font f, float size)
{
    static const unsigned short kHelv[224] = {
        278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
        556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
        1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
        667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
        333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
        556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584, 761,
        556, 556, 222, 556, 333, 1000, 556, 556, 333, 1000, 667, 333, 1000, 556, 611, 556,
        556, 222, 222, 333, 333, 350, 556, 1000, 333, 1000, 500, 333, 944, 556, 500, 667,
        278, 333, 556, 556, 556, 556, 260, 556, 333, 737, 370, 556, 584, 333, 737, 333,
        400, 584, 333, 333, 333, 556, 537, 278, 333, 333, 365, 556, 834, 834, 834, 611,
        667, 667, 667, 667, 667, 667, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
        722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
        556, 556, 556, 556, 556, 556, 889, 500, 556, 556, 556, 556, 278, 278, 278, 278,
        556, 556, 556, 556, 556, 556, 556, 584, 611, 556, 556, 556, 556, 500, 556, 500,
    };
    static const unsigned short kHelvBold[224] = {
        278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
        556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
        975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
        667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
        333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
        611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584, 761,
        556, 611, 278, 556, 500, 1000, 556, 556, 333, 1000, 667, 333, 1000, 611, 611, 611,
        611, 278, 278, 500, 500, 350, 556, 1000, 333, 1000, 556, 333, 944, 611, 500, 667,
        278, 333, 556, 556, 556, 556, 280, 556, 333, 737, 370, 556, 584, 333, 737, 333,
        400, 584, 333, 333, 333, 611, 556, 278, 333, 333, 365, 556, 834, 834, 834, 611,
        722, 722, 722, 722, 722, 722, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
        722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
        556, 556, 556, 556, 556, 556, 889, 556, 556, 556, 556, 556, 278, 278, 278, 278,
        611, 611, 611, 611, 611, 611, 611, 584, 611, 611, 611, 611, 611, 556, 611, 556,
    };
    const auto* w = f == Bold ? kHelvBold : kHelv;            // (Helvetica-Oblique has Helvetica's widths)
    double sum = 0.0;
    for (unsigned char b : encode (text)) sum += b >= 32 ? w[b - 32] : 0;
    return (float) (sum * size / 1000.0);
}

/** The text broken into lines no wider than maxWidth (at spaces; a word longer than a line is cut). */
inline juce::StringArray wrap (const juce::String& text, Font f, float size, float maxWidth)
{
    juce::StringArray lines;
    for (auto& para : juce::StringArray::fromLines (text))
    {
        auto words = juce::StringArray::fromTokens (para, " ", "");
        words.removeEmptyStrings();
        juce::String cur;
        for (auto w : words)
        {
            const auto tryLine = cur.isEmpty() ? w : cur + " " + w;
            if (width (tryLine, f, size) <= maxWidth) { cur = tryLine; continue; }
            if (cur.isNotEmpty()) { lines.add (cur); cur.clear(); }
            while (width (w, f, size) > maxWidth && w.length() > 1)          // a single word wider than the line: cut it
            {
                int k = w.length() - 1;
                while (k > 1 && width (w.substring (0, k), f, size) > maxWidth) --k;
                lines.add (w.substring (0, k)); w = w.substring (k);
            }
            cur = w;
        }
        if (cur.isNotEmpty() || para.isEmpty()) lines.add (cur);
    }
    return lines;
}

class Document
{
public:
    static constexpr float kWidth = 595.28f, kHeight = 841.89f;          // A4

    void setTitle (const juce::String& t) { title = t; }
    void setAuthor (const juce::String& a) { author = a; }
    /** Starts a new page and draws on it from now on. */
    void newPage() { pages.emplace_back(); current = (int) pages.size() - 1; }
    /** Draws on an earlier page (for "page 2 of 5", written once the number of pages is known). */
    void setPage (int i) { if (i >= 0 && i < (int) pages.size()) current = i; }
    int pageCount() const noexcept { return (int) pages.size(); }

    void text (float x, float y, const juce::String& s, Font f, float size, Colour c = {})
    {
        if (current < 0) newPage();
        auto& p = pages[(size_t) current];
        p += "BT /F" + std::to_string ((int) f + 1) + " " + num (size) + " Tf " + rgb (c) + " rg 1 0 0 1 " + num (x) + " " + num (kHeight - y) + " Tm (";
        for (char ch : encode (s))
        {
            if (ch == '(' || ch == ')' || ch == '\\') p.push_back ('\\');
            p.push_back (ch);
        }
        p += ") Tj ET\n";
    }
    void textRight (float right, float y, const juce::String& s, Font f, float size, Colour c = {}) { text (right - width (s, f, size), y, s, f, size, c); }
    /** The text cut to fit maxWidth, with "..." when it was cut. */
    static juce::String fit (const juce::String& s, Font f, float size, float maxWidth)
    {
        if (width (s, f, size) <= maxWidth) return s;
        juce::String t = s;
        while (t.length() > 1 && width (t + "...", f, size) > maxWidth) t = t.dropLastCharacters (1);
        return t.trimEnd() + "...";
    }
    void line (float x1, float y1, float x2, float y2, float w, Colour c = {})
    {
        if (current < 0) newPage();
        pages[(size_t) current] += rgb (c) + " RG " + num (w) + " w " + num (x1) + " " + num (kHeight - y1) + " m " + num (x2) + " " + num (kHeight - y2) + " l S\n";
    }
    void rect (float x, float y, float w, float h, Colour fill)
    {
        if (current < 0) newPage();
        pages[(size_t) current] += rgb (fill) + " rg " + num (x) + " " + num (kHeight - y - h) + " " + num (w) + " " + num (h) + " re f\n";
    }
    /** Everything drawn until endClip() is cut to this box (3.12.0: the take chart's no-take pinstripe). Pairs must not cross a newPage(). */
    void beginClip (float x, float y, float w, float h)
    {
        if (current < 0) newPage();
        pages[(size_t) current] += "q " + num (x) + " " + num (kHeight - y - h) + " " + num (w) + " " + num (h) + " re W n\n";
    }
    void endClip() { if (current >= 0) pages[(size_t) current] += "Q\n"; }

    /** The whole file. */
    juce::MemoryBlock toPdf() const
    {
        std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        std::vector<size_t> offsets;
        auto obj = [&] (const std::string& body) { offsets.push_back (out.size()); out += std::to_string (offsets.size()) + " 0 obj\n" + body + "\nendobj\n"; };
        const int n = juce::jmax (1, (int) pages.size());
        // 1 catalog, 2 pages, 3-5 fonts, 6 info, then a page and its contents for each page
        std::string kids;
        for (int i = 0; i < n; ++i) kids += std::to_string (7 + 2 * i) + " 0 R ";
        obj ("<< /Type /Catalog /Pages 2 0 R >>");
        obj ("<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string (n) + " >>");
        obj ("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
        obj ("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
        obj ("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique /Encoding /WinAnsiEncoding >>");
        obj ("<< /Title (" + escaped (title) + ") /Author (" + escaped (author) + ") /Producer (Fermata) /CreationDate (D:"
             + juce::Time::getCurrentTime().formatted ("%Y%m%d%H%M%S").toStdString() + ") >>");
        for (int i = 0; i < n; ++i)
        {
            const std::string content = i < (int) pages.size() ? pages[(size_t) i] : std::string();
            obj ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num (kWidth) + " " + num (kHeight) + "] /Contents " + std::to_string (8 + 2 * i)
                 + " 0 R /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> >>");
            obj ("<< /Length " + std::to_string (content.size()) + " >>\nstream\n" + content + "endstream");
        }
        const size_t xref = out.size();
        out += "xref\n0 " + std::to_string (offsets.size() + 1) + "\n0000000000 65535 f \n";
        for (auto o : offsets) { char b[24]; std::snprintf (b, sizeof (b), "%010zu 00000 n \n", o); out += b; }
        out += "trailer\n<< /Size " + std::to_string (offsets.size() + 1) + " /Root 1 0 R /Info 6 0 R >>\nstartxref\n" + std::to_string (xref) + "\n%%EOF\n";
        return juce::MemoryBlock (out.data(), out.size());
    }
    bool save (const juce::File& f) const
    {
        const auto mb = toPdf();
        return filesync::writeDataChecked (f, mb.getData(), mb.getSize());      // (3.1.3: checked, so a full drive is reported instead of leaving a short file)
    }

private:
    static std::string num (float v)
    {
        char b[32]; std::snprintf (b, sizeof (b), "%.2f", (double) v);
        std::string s (b);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (! s.empty() && s.back() == '.') s.pop_back();
        return s.empty() || s == "-0" ? std::string ("0") : s;
    }
    static std::string rgb (Colour c) { return num (c.r) + " " + num (c.g) + " " + num (c.b); }
    static std::string escaped (const juce::String& s)
    {
        std::string out;
        for (char ch : encode (s)) { if (ch == '(' || ch == ')' || ch == '\\') out.push_back ('\\'); out.push_back (ch); }
        return out;
    }
    std::vector<std::string> pages;
    int current = -1;
    juce::String title, author;
};
} // namespace pdf
} // namespace td

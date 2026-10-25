#include "ReviewPage.h"

namespace td
{
namespace reviewpage
{
namespace
{
#include "ReviewPageTemplate.inc"
#include "AudioFormats.h"

juce::String htmlEscape (const juce::String& s) { return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;"); }

/** JSON that is safe inside a <script> block: no "</", no U+2028 / U+2029. */
juce::String scriptJson (const juce::var& v)
{
    auto s = juce::JSON::toString (v, true);
    s = s.replace ("</", "<\\/").replace ("<!--", "<\\!--");
    s = s.replace (juce::String::charToString (0x2028), "\\u2028").replace (juce::String::charToString (0x2029), "\\u2029");
    return s;
}
} // namespace

juce::String base64 (const void* data, size_t size)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const unsigned char*> (data);
    std::string out; out.reserve ((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3)
    {
        const unsigned v = ((unsigned) p[i] << 16) | ((unsigned) p[i + 1] << 8) | p[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == size) { const unsigned v = (unsigned) p[i] << 16; out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "=="; }
    else if (i + 2 == size) { const unsigned v = ((unsigned) p[i] << 16) | ((unsigned) p[i + 1] << 8); out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '='; }
    return juce::String (out.c_str());
}

int peakColumnsFor (double d) { return juce::jlimit (3000, 40000, (int) std::ceil (d * 25.0)); }

std::vector<int> computePeaks (const juce::File& file, float gain, int columns, const std::atomic<bool>* cancel)
{
    std::vector<int> out ((size_t) columns * 2, 0);
    juce::AudioFormatManager fm; td::registerAudioFormats (fm);
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr || r->lengthInSamples <= 0) return out;
    const juce::int64 n = r->lengthInSamples;
    std::vector<float> mn ((size_t) columns, 0.0f), mx ((size_t) columns, 0.0f);
    juce::AudioBuffer<float> buf (2, 65536);
    for (juce::int64 pos = 0; pos < n; pos += 65536)
    {
        if (cancel != nullptr && cancel->load()) break;
        const int m = (int) juce::jmin ((juce::int64) 65536, n - pos);
        r->read (&buf, 0, m, pos, true, true);
        const int chans = juce::jmax (1, (int) r->numChannels);
        for (int i = 0; i < m; ++i)
        {
            const auto col = (size_t) juce::jmin ((juce::int64) columns - 1, (pos + i) * (juce::int64) columns / n);
            for (int c = 0; c < juce::jmin (2, chans); ++c)
            {
                const float x = buf.getSample (c, i) * gain;
                if (x < mn[col]) mn[col] = x;
                if (x > mx[col]) mx[col] = x;
            }
        }
    }
    float top = 0.0f; for (int c = 0; c < columns; ++c) top = juce::jmax (top, -mn[(size_t) c], mx[(size_t) c]);
    const float scale = top > 1.0e-6f ? 100.0f / juce::jmax (top, 0.05f) : 0.0f;      // quiet pieces are drawn bigger (but never raised more than 26 dB)
    for (int c = 0; c < columns; ++c)
    {
        out[(size_t) c * 2]     = juce::jlimit (-100, 0, (int) std::floor (mn[(size_t) c] * scale));
        out[(size_t) c * 2 + 1] = juce::jlimit (0, 100, (int) std::ceil (mx[(size_t) c] * scale));
    }
    return out;
}

juce::String write (const juce::File& html, const Spec& s, const std::atomic<bool>* cancel)
{
    return writeMany (html, s.piece, s.project, { s }, cancel);
}

juce::String writeMany (const juce::File& html, const juce::String& title, const juce::String& project, const std::vector<Spec>& pieces, const std::atomic<bool>* cancel)
{
    if (pieces.empty()) return "There is nothing to put in the listening copy.";
    juce::String tpl;
    for (auto* p : kReviewTemplate) tpl += p;

    auto meta = new juce::DynamicObject();
    juce::var m (meta);
    meta->setProperty ("id", juce::Uuid().toString().retainCharacters ("0123456789abcdef").substring (0, 16));
    meta->setProperty ("title", title); meta->setProperty ("project", project);
    meta->setProperty ("name", html.getFileNameWithoutExtension());          // the returned notes are named after this ("... listening copy" -> "... listening notes")
    juce::Array<juce::var> tracks;
    for (auto& s : pieces)
    {
        auto t = new juce::DynamicObject();
        t->setProperty ("piece", s.piece); t->setProperty ("edit", s.editId);
        t->setProperty ("offset", s.offsetSeconds); t->setProperty ("duration", s.durationSeconds); t->setProperty ("mime", s.mime);
        juce::Array<juce::var> pk; for (int v : s.peaks) pk.add (v);
        t->setProperty ("peaks", pk);
        tracks.add (juce::var (t));
    }
    meta->setProperty ("tracks", tracks);

    const int at = tpl.indexOf ("__CHUNKS__");
    if (at < 0) return "Internal error: the page template is damaged.";
    auto head = tpl.substring (0, at), tail = tpl.substring (at + 10);
    head = head.replace ("__TITLE__", htmlEscape (title)).replace ("__META__", scriptJson (m));

    html.getParentDirectory().createDirectory();
    html.deleteFile();                                           // (a FileOutputStream would append to an old file)
    juce::FileOutputStream out (html);
    if (out.failedToOpen()) return "Cannot write " + html.getFullPathName();
    out.write (head.toRawUTF8(), head.getNumBytesAsUTF8());

    constexpr size_t kRaw = 3000000;                             // a multiple of 3, so every piece is whole base64 with no padding inside
    std::vector<char> buf (kRaw);
    for (size_t k = 0; k < pieces.size(); ++k)
    {
        juce::FileInputStream src (pieces[k].audio);
        if (src.failedToOpen()) { out.flush(); html.deleteFile(); return "Cannot read " + pieces[k].audio.getFullPathName(); }
        const juce::String open = "<script type=\"text/plain\" data-k=\"" + juce::String ((int) k) + "\" class=\"ch\">";
        for (;;)
        {
            if (cancel != nullptr && cancel->load()) { out.flush(); html.deleteFile(); return "Cancelled."; }
            const int got = src.read (buf.data(), (int) kRaw);
            if (got <= 0) break;
            const auto b64 = base64 (buf.data(), (size_t) got);
            out.writeText (open, false, false, nullptr);
            out.write (b64.toRawUTF8(), b64.getNumBytesAsUTF8());
            out.writeText ("</script>\n", false, false, nullptr);
        }
    }
    out.write (tail.toRawUTF8(), tail.getNumBytesAsUTF8());
    out.flush();
    if (out.getStatus().failed()) return "Cannot write " + html.getFullPathName() + ": " + out.getStatus().getErrorMessage();
    return {};
}
} // namespace reviewpage
} // namespace td

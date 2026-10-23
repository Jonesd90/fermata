#pragma once
#include "Project.h"
#include "Pdf.h"

namespace td
{
/** The take sheet: every take of a take window (or of the whole project), with its time, length, bars, markers and anything that went wrong while
    it was recorded, for the producer, the label or the next session. As a PDF to print or send, and as a CSV for a spreadsheet. */
namespace takesheet
{
struct Row
{
    int number = 0;
    juce::String name;                 // "003 - Kyrie"
    juce::Time at;                     // when it was recorded
    double lengthSeconds = 0.0;
    juce::String bars;                 // "Bar 12 - 40" (empty: not entered)
    int barIn = 0, barOut = 0;
    bool dud = false, roomTone = false;
    juce::StringArray markers;         // "Clarinet in (0:42)"
    juce::StringArray problems;        // "LOST 0.3 s", "CUT SHORT"
};
struct Section
{
    juce::String piece;                // the take window's name
    juce::StringArray tracks;          // the tracks its takes recorded, in project order
    std::vector<Row> rows;
    int duds() const { int n = 0; for (auto& r : rows) n += r.dud ? 1 : 0; return n; }
    double seconds() const { double s = 0.0; for (auto& r : rows) s += r.lengthSeconds; return s; }
};

/** "0:42", "4:05", "1:02:03". */
inline juce::String clockText (double seconds)
{
    const int s = (int) std::lround (juce::jmax (0.0, seconds));
    if (s >= 3600) return juce::String (s / 3600) + ":" + juce::String ((s / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}
/** "1 h 04 min", "12 min", "45 s". */
inline juce::String durationText (double seconds)
{
    const int s = (int) std::lround (juce::jmax (0.0, seconds));
    if (s >= 3600) return juce::String (s / 3600) + " h " + juce::String ((s / 60) % 60).paddedLeft ('0', 2) + " min";
    if (s >= 60) return juce::String (s / 60) + " min";
    return juce::String (s) + " s";
}

/** The sheet's contents: one section per take window with takes in it (only 'onlyWindow' when it is not null), the takes in number order. */
inline std::vector<Section> collect (const Project& p, const juce::Uuid& onlyWindow = juce::Uuid::null())
{
    std::vector<Section> out;
    for (auto& wp : p.takeWindows)
    {
        const auto& w = *wp;
        if (! onlyWindow.isNull() && w.id != onlyWindow) continue;
        if (onlyWindow.isNull() && w.groups.empty()) continue;                 // (the whole project: a piece with nothing recorded is left out)
        Section sec;
        sec.piece = w.name;
        // the tracks: every track any take of this window recorded, in the project's order (then any no longer in the project)
        juce::Array<juce::Uuid> seen;
        for (auto& t : p.tracks)
            for (auto& g : w.groups)
            {
                bool has = false;
                for (auto& f : g.files) has = has || f.trackId == t.id;
                if (has) { if (! t.isLtc) sec.tracks.add (t.name); seen.add (t.id); break; }
            }
        for (auto& g : w.groups) for (auto& f : g.files) if (! seen.contains (f.trackId)) { seen.add (f.trackId); sec.tracks.addIfNotAlreadyThere (f.trackName); }
        std::vector<const TakeGroup*> takes;
        for (auto& g : w.groups) takes.push_back (&g);
        std::sort (takes.begin(), takes.end(), [] (const TakeGroup* a, const TakeGroup* b) { return a->number != b->number ? a->number < b->number : a->startSeconds < b->startSeconds; });
        for (auto* g : takes)
        {
            Row r;
            r.number = g->number;
            r.name = w.displayName (*g);
            r.at = g->recordedAt;
            r.lengthSeconds = g->lengthSeconds();
            r.bars = barsLabel (g->barIn, g->barOut).replace ("Bar ", "");
            r.barIn = g->barIn; r.barOut = g->barOut;
            r.dud = g->dud; r.roomTone = g->roomTone;
            std::vector<const Marker*> ms;
            for (auto& m : w.markers) if (m.seconds >= g->startSeconds && m.seconds < g->startSeconds + juce::jmax (0.001, r.lengthSeconds)) ms.push_back (&m);
            std::sort (ms.begin(), ms.end(), [] (const Marker* a, const Marker* b) { return a->seconds < b->seconds; });
            for (auto* m : ms) r.markers.add ((m->name.trim().isNotEmpty() ? m->name.trim() : juce::String ("Marker")) + " (" + clockText (m->seconds - g->startSeconds) + ")");
            if (g->lostSamples > 0 || g->overruns > 0) r.problems.add ("LOST " + lostText (g->lostSamples, g->sampleRate));
            if (g->writeFailed) r.problems.add ("CUT SHORT");
            if (! g->waiting.empty()) r.problems.add ("waiting for corrected audio");
            sec.rows.push_back (r);
        }
        out.push_back (sec);
    }
    return out;
}

/** CSV (UTF-8, comma-separated, a heading row), for Excel, Numbers or Google Sheets. */
inline juce::String csv (const std::vector<Section>& sections)
{
    auto q = [] (const juce::String& s)
    {
        if (s.containsAnyOf (",\"\r\n")) return "\"" + s.replace ("\"", "\"\"") + "\"";
        return s;
    };
    juce::String out = "Piece,Take,Name,Date,Time,Length,Bar in,Bar out,Dud,Room tone,Markers,Problems,Tracks\r\n";
    for (auto& sec : sections)
        for (auto& r : sec.rows)
        {
            juce::StringArray f;
            f.add (q (sec.piece)); f.add (juce::String (r.number)); f.add (q (r.name));
            f.add (r.at.toMilliseconds() != 0 ? r.at.formatted ("%Y-%m-%d") : juce::String());
            f.add (r.at.toMilliseconds() != 0 ? r.at.formatted ("%H:%M:%S") : juce::String());
            f.add (clockText (r.lengthSeconds));
            f.add (r.barIn > 0 ? juce::String (r.barIn) : juce::String()); f.add (r.barIn > 0 ? juce::String (r.barOut > r.barIn ? r.barOut : r.barIn) : juce::String());
            f.add (r.dud ? "yes" : ""); f.add (r.roomTone ? "yes" : "");
            f.add (q (r.markers.joinIntoString ("; "))); f.add (q (r.problems.joinIntoString ("; ")));
            f.add (q (sec.tracks.joinIntoString ("; ")));
            out << f.joinIntoString (",") << "\r\n";
        }
    return out;
}

/** The printed sheet: A4 portrait, a heading on every page, one section per piece (its tracks, then a table of its takes), page numbers. */
inline pdf::Document makePdf (const std::vector<Section>& sections, const juce::String& projectName, const juce::String& version, juce::Time printed)
{
    using namespace pdf;
    Document d;
    d.setTitle ("Take sheet - " + projectName);
    d.setAuthor ("Fermata");
    const float L = 40.0f, R = Document::kWidth - 40.0f, top = 40.0f, bottom = Document::kHeight - 46.0f;
    const Colour ink = grey (0.11f), dim = grey (0.42f), rule = grey (0.80f), band { 0.16f, 0.20f, 0.24f }, accent { 0.11f, 0.45f, 0.60f }, red { 0.75f, 0.16f, 0.14f };
    // the table's columns: take, name, recorded, length, bars, notes
    const float cNo = L, cName = L + 32.0f, cTime = L + 196.0f, cLen = L + 250.0f, cBars = L + 292.0f, cNotes = L + 350.0f;
    const float notesW = R - cNotes - 2.0f;
    float y = 0.0f;
    auto pageHead = [&]
    {
        d.newPage();
        d.text (L, top + 10.0f, "TAKE SHEET", Bold, 8.5f, accent);
        d.textRight (R, top + 10.0f, "Fermata", Bold, 8.5f, dim);
        d.text (L, top + 34.0f, Document::fit (projectName, Bold, 20.0f, R - L), Bold, 20.0f, ink);
        d.line (L, top + 44.0f, R, top + 44.0f, 0.8f, ink);
        y = top + 62.0f;
    };
    auto tableHead = [&]
    {
        d.rect (L, y - 11.0f, R - L, 16.0f, band);
        const Colour w { 1.0f, 1.0f, 1.0f };
        d.text (cNo + 4.0f, y, "TAKE", Bold, 7.0f, w); d.text (cName, y, "NAME", Bold, 7.0f, w); d.text (cTime, y, "RECORDED", Bold, 7.0f, w);
        d.text (cLen, y, "LENGTH", Bold, 7.0f, w); d.text (cBars, y, "BARS", Bold, 7.0f, w); d.text (cNotes, y, "MARKERS AND NOTES", Bold, 7.0f, w);
        y += 16.0f;
    };
    pageHead();
    if (sections.empty() || [&] { for (auto& s : sections) if (! s.rows.empty()) return false; return true; }())
    {
        d.text (L, y + 10.0f, "No takes have been recorded yet.", Italic, 11.0f, dim);
    }
    for (size_t si = 0; si < sections.size(); ++si)
    {
        const auto& sec = sections[si];
        // the piece: its name, a summary, its tracks (kept together with at least two rows of the table)
        const auto trackLines = wrap ("Tracks: " + (sec.tracks.isEmpty() ? juce::String ("-") : sec.tracks.joinIntoString (", ")), Regular, 8.0f, R - L);
        const float headH = 22.0f + 11.0f * (float) trackLines.size() + 22.0f + 2.0f * 16.0f;
        if (y + headH > bottom) pageHead();
        else if (si > 0) y += 10.0f;
        d.text (L, y + 6.0f, Document::fit (sec.piece, Bold, 14.0f, R - L - 160.0f), Bold, 14.0f, ink);
        juce::String summary = juce::String ((int) sec.rows.size()) + (sec.rows.size() == 1 ? " take" : " takes");
        if (sec.duds() > 0) summary << "  /  " << sec.duds() << (sec.duds() == 1 ? " dud" : " duds");
        summary << "  /  " << durationText (sec.seconds()) << " recorded";
        d.textRight (R, y + 6.0f, summary, Regular, 8.5f, dim);
        y += 20.0f;
        for (auto& tl : trackLines) { d.text (L, y, tl, Regular, 8.0f, dim); y += 11.0f; }
        y += 12.0f;
        tableHead();
        juce::String lastDate;
        bool shade = false;
        for (auto& r : sec.rows)
        {
            const auto date = r.at.toMilliseconds() != 0 ? r.at.formatted ("%A %d %B %Y") : juce::String();
            juce::StringArray notes;
            if (r.dud) notes.add ("DUD");
            if (r.roomTone) notes.add ("Room tone");
            notes.addArray (r.problems);
            notes.addArray (r.markers);
            const auto lines = wrap (notes.joinIntoString ("  /  "), Regular, 8.0f, notesW);
            const float rowH = juce::jmax (15.0f, 4.0f + 10.5f * (float) juce::jmax (1, lines.size()));
            const bool newDate = date.isNotEmpty() && date != lastDate;
            if (y + rowH + (newDate ? 15.0f : 0.0f) > bottom) { pageHead(); d.text (L, y + 2.0f, Document::fit (sec.piece, Bold, 10.0f, R - L) + "  (continued)", Bold, 10.0f, ink); y += 16.0f; tableHead(); lastDate.clear(); }
            if (newDate)
            {
                d.text (L + 4.0f, y + 1.0f, date, Bold, 7.5f, accent);
                y += 13.0f; lastDate = date;
            }
            if (shade) d.rect (L, y - 10.5f, R - L, rowH, grey (0.955f));
            shade = ! shade;
            const Colour c = r.dud ? grey (0.55f) : ink;
            d.text (cNo + 4.0f, y, juce::String (r.number), Bold, 9.0f, c);
            d.text (cName, y, Document::fit (r.name, Regular, 9.0f, cTime - cName - 8.0f), Regular, 9.0f, c);
            d.text (cTime, y, r.at.toMilliseconds() != 0 ? r.at.formatted ("%H:%M:%S") : juce::String ("-"), Regular, 9.0f, c);
            d.text (cLen, y, clockText (r.lengthSeconds), Regular, 9.0f, c);
            d.text (cBars, y, r.bars.isNotEmpty() ? r.bars : juce::String ("-"), Regular, 9.0f, r.bars.isNotEmpty() ? c : grey (0.7f));
            float ly = y;
            for (int i = 0; i < lines.size(); ++i)
            {
                auto t = lines[i];
                if (i == 0 && r.dud && t.startsWith ("DUD"))
                {
                    d.text (cNotes, ly, "DUD", Bold, 8.0f, red);
                    t = t.substring (3);
                    d.text (cNotes + width ("DUD", Bold, 8.0f), ly, t, Regular, 8.0f, c);
                }
                else d.text (cNotes, ly, t, Regular, 8.0f, c);
                ly += 10.5f;
            }
            y += rowH;
            d.line (L, y - 10.5f, R, y - 10.5f, 0.4f, rule);
        }
    }
    // page numbers and the footer on every page
    const int n = d.pageCount();
    for (int i = 0; i < n; ++i)
    {
        d.setPage (i);
        d.line (L, Document::kHeight - 34.0f, R, Document::kHeight - 34.0f, 0.5f, rule);
        d.text (L, Document::kHeight - 22.0f, "Fermata " + version + "  /  printed " + printed.formatted ("%d %b %Y %H:%M"), Regular, 7.5f, dim);
        d.textRight (R, Document::kHeight - 22.0f, "Page " + juce::String (i + 1) + " of " + juce::String (n), Bold, 7.5f, ink);
    }
    return d;
}
} // namespace takesheet
} // namespace td

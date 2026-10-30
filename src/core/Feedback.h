#pragma once
#include "Common.h"
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <vector>
#include "TextBytes.h"

namespace td
{
/** One listener's comment, pinned to a time on an edit's timeline. They come from the review page (Include Feedback Form) or from a marker list exported by a review website.
    Several notes at the same time form a thread (the first is the comment, the rest are replies). Not part of undo. */
struct FeedbackNote
{
    juce::String key;                // identity, so importing the same file twice does not double the notes
    juce::String who, bar, text;     // the person, the bar as they typed it ("bar 14", "19 - 24"), the comment
    juce::String colour;             // "Red", "Green", "Orange" ... as the source gave it (empty = none)
    juce::String source;             // the file it was imported from
    double       seconds = 0.0;      // on the edit's timeline
    bool         done = false;       // ticked off by the producer
};

namespace feedback
{
/** What an import file held. 'seconds' of each note is time in the exported file; add 'offset' (when 'hasOffset') or the edit's first sample to get edit time. */
struct Parsed
{
    std::vector<FeedbackNote> notes;
    juce::String editId, piece, format;
    double offset = 0.0;
    bool hasOffset = false;
    std::vector<Parsed> parts;       // a listening copy that held several pieces: one part per piece that has comments (each with its own edit and offset); 'notes' is then empty
};

inline juce::uint32 colourArgb (const juce::String& name)
{
    const auto n = name.trim().toLowerCase();
    if (n == "red") return 0xffe8433au;
    if (n == "green") return 0xff3cc75au;
    if (n == "orange") return 0xffee9a2eu;
    if (n == "yellow") return 0xffe9d23au;
    if (n == "blue") return 0xff4a8fe8u;
    if (n == "purple" || n == "violet") return 0xff9a6ae0u;
    if (n == "pink" || n == "magenta") return 0xffe36ab5u;
    if (n == "cyan" || n == "teal") return 0xff35c5c5u;
    if (n == "white") return 0xfff0f0f0u;
    return 0xff35b7c9u;
}

/** A time as written by a person or a program: 83.5, 1:23.5, 0:01:23, 00:01:23:12 (hours:minutes:seconds:frames; ';' also allowed), "1m 23s". Returns false if it is not a time. */
inline bool parseTime (juce::String s, double frameRate, double& seconds)
{
    s = s.trim().replaceCharacter (';', ':');
    if (s.isEmpty()) return false;
    if (s.containsOnly ("0123456789.-")) { if (s.containsChar ('-') && ! s.startsWithChar ('-')) return false; seconds = s.getDoubleValue(); return true; }
    if (s.containsOnly ("0123456789:.,"))
    {
        s = s.replaceCharacter (',', '.');
        juce::StringArray p; p.addTokens (s, ":", "");
        if (p.size() < 2 || p.size() > 4) return false;
        double v[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < p.size(); ++i) { if (p[i].isEmpty()) return false; v[i] = p[i].getDoubleValue(); }
        if (p.size() == 2) seconds = v[0] * 60.0 + v[1];
        else if (p.size() == 3) seconds = v[0] * 3600.0 + v[1] * 60.0 + v[2];
        else seconds = v[0] * 3600.0 + v[1] * 60.0 + v[2] + v[3] / juce::jmax (1.0, frameRate);
        return true;
    }
    // "1m 23s", "1h2m3.5s"
    double total = 0.0, num = 0.0; bool any = false, haveNum = false; juce::String cur;
    for (auto c : s + " ")
    {
        if (juce::CharacterFunctions::isDigit (c) || c == '.') { cur += juce::String::charToString (c); continue; }
        if (cur.isNotEmpty()) { num = cur.getDoubleValue(); haveNum = true; cur.clear(); }
        const auto lc = juce::CharacterFunctions::toLowerCase (c);
        if (haveNum && (lc == 'h' || lc == 'm' || lc == 's')) { total += num * (lc == 'h' ? 3600.0 : lc == 'm' ? 60.0 : 1.0); any = true; haveNum = false; }
        else if (c != ' ' && c != ':') return false;
    }
    if (! any) return false;
    seconds = total; return true;
}

/** Splits delimited text (tab, comma or semicolon; the one used most in the first line) into rows of fields. Quotes may hold the delimiter, a line break, or "" for a quote. */
inline std::vector<std::vector<juce::String>> splitTable (const juce::String& text)
{
    std::vector<std::vector<juce::String>> rows;
    const auto firstLine = text.upToFirstOccurrenceOf ("\n", false, false);
    const int nTab = firstLine.retainCharacters ("\t").length(), nComma = firstLine.retainCharacters (",").length(), nSemi = firstLine.retainCharacters (";").length();
    const juce::juce_wchar delim = (nTab >= nComma && nTab >= nSemi && nTab > 0) ? '\t' : (nSemi > nComma ? ';' : ',');
    // The characters are taken out once and each field is gathered as characters: text[i] walks the text from its start every time, and adding
    // to a juce::String one character at a time copies it every time, so a long list (or one stray quote that made the rest of it one field) took
    // minutes. (3.0.0)
    std::vector<juce::juce_wchar> chars; chars.reserve ((size_t) text.getNumBytesAsUTF8());
    for (auto p = text.getCharPointer(); ! p.isEmpty();) chars.push_back (p.getAndAdvance());
    std::vector<juce::String> row; std::vector<juce::juce_wchar> field; bool quoted = false, wasQuoted = false;
    auto fieldText = [&field]
    {
        field.push_back (0);
        juce::String t (juce::CharPointer_UTF32 (reinterpret_cast<const juce::CharPointer_UTF32::CharType*> (field.data())));
        field.pop_back();
        return t;
    };
    auto fieldBlank = [&field] { for (auto ch : field) if (! juce::CharacterFunctions::isWhitespace (ch)) return false; return true; };
    auto endField = [&] { row.push_back (wasQuoted ? fieldText() : fieldText().trim()); field.clear(); wasQuoted = false; };
    auto endRow = [&] { endField(); bool blank = true; for (auto& f : row) if (f.isNotEmpty()) blank = false; if (! blank) rows.push_back (row); row.clear(); };
    const int n = (int) chars.size();
    for (int i = 0; i < n; ++i)
    {
        const auto c = chars[(size_t) i];
        if (quoted)
        {
            if (c == '"') { if (i + 1 < n && chars[(size_t) i + 1] == '"') { field.push_back ('"'); ++i; } else quoted = false; }
            else field.push_back (c);
        }
        else if (c == '"' && fieldBlank()) { quoted = true; wasQuoted = true; field.clear(); }
        else if (c == delim) endField();
        else if (c == '\r') { if (i + 1 < n && chars[(size_t) i + 1] == '\n') ++i; endRow(); }
        else if (c == '\n') endRow();
        else field.push_back (c);
    }
    if (! field.empty() || ! row.empty()) endRow();
    return rows;
}

inline juce::String makeKey (const juce::String& source, double seconds, const juce::String& who, const juce::String& text)
{
    return juce::String::toHexString (juce::String (source + "|" + juce::String (seconds, 3) + "|" + who + "|" + text).hashCode64());
}

namespace detail
{
inline int findColumn (const std::vector<juce::String>& header, std::initializer_list<const char*> names)
{
    for (auto* nm : names)
        for (size_t i = 0; i < header.size(); ++i)
            if (header[i].trim().equalsIgnoreCase (nm)) return (int) i;
    return -1;
}
inline juce::String at (const std::vector<juce::String>& row, int i) { return i >= 0 && i < (int) row.size() ? row[(size_t) i].trim() : juce::String(); }

/** A marker list as a review website exports it (a header row, then one row per comment). Handles the Premiere-style layout where the comment text is in "Marker Name" and the person in "Comment". */
inline juce::String parseTable (const juce::String& text, double frameRate, const juce::String& source, Parsed& out)
{
    const auto rows = splitTable (text);
    if (rows.size() < 2) return "The file has no comments in it.";
    const auto& h = rows[0];
    const int cTime = findColumn (h, { "start", "time", "timecode", "timestamp", "position", "seconds", "t", "in", "at" });
    if (cTime < 0) return "Cannot find a time column (looked for Start, Time, Timecode, Timestamp, Position, Seconds).";
    const int cMarkerName = findColumn (h, { "marker name" }), cComment = findColumn (h, { "comment" });
    int cText = findColumn (h, { "text", "message", "note", "notes", "body", "comment text", "description" });
    int cWho = findColumn (h, { "author", "user", "username", "who", "by", "person", "commenter", "from", "name" });
    if (cText < 0 && cMarkerName >= 0 && cComment >= 0) { cText = cMarkerName; cWho = cComment; out.format = "marker list"; }
    else if (cText < 0) cText = cComment >= 0 ? cComment : cMarkerName;
    if (cText < 0) return "Cannot find a comment column (looked for Marker Name, Comment, Text, Message, Note).";
    const int cBar = findColumn (h, { "bar", "bars", "location" }), cColour = findColumn (h, { "color", "colour" }), cId = findColumn (h, { "id", "uuid", "key" }), cDone = findColumn (h, { "done", "resolved" });
    if (out.format.isEmpty()) out.format = "table";
    for (size_t r = 1; r < rows.size(); ++r)
    {
        const auto& row = rows[r];
        double s = 0.0;
        if (! parseTime (at (row, cTime), frameRate, s)) continue;
        FeedbackNote n; n.seconds = juce::jmax (0.0, s);
        n.text = at (row, cText);
        if (n.text.isEmpty() && cMarkerName >= 0 && cComment >= 0) n.text = at (row, cComment);
        if (n.text.isEmpty()) continue;
        n.who = at (row, cWho); if (cWho == cText) n.who = {};
        n.bar = at (row, cBar); n.colour = at (row, cColour); n.source = source;
        const auto dn = at (row, cDone).toLowerCase(); n.done = dn == "true" || dn == "yes" || dn == "1" || dn == "done";
        const auto id = at (row, cId);
        n.key = id.isNotEmpty() ? id : makeKey (source, n.seconds, n.who, n.text);
        out.notes.push_back (n);
    }
    if (out.notes.empty()) return "No rows with a readable time and a comment were found.";
    return {};
}

inline void readNotes (const juce::var& list, double frameRate, const juce::String& source, const juce::String& defaultWho, std::vector<FeedbackNote>& into)
{
    auto* a = list.getArray();
    if (a == nullptr) return;
    for (auto& v : *a)
    {
        FeedbackNote n; double s = 0.0;
        juce::var t = v["t"]; if (t.isVoid()) t = v["seconds"]; if (t.isVoid()) t = v["time"]; if (t.isVoid()) t = v["timestamp"];
        if (t.isDouble() || t.isInt() || t.isInt64()) s = (double) t; else if (! parseTime (t.toString(), frameRate, s)) continue;
        n.seconds = juce::jmax (0.0, s);
        n.text = v["text"].toString(); if (n.text.isEmpty()) n.text = v["comment"].toString(); if (n.text.isEmpty()) n.text = v["message"].toString();
        if (n.text.trim().isEmpty()) continue;
        n.who = v["who"].toString(); if (n.who.isEmpty()) n.who = v["author"].toString(); if (n.who.isEmpty()) n.who = v["user"].toString(); if (n.who.isEmpty()) n.who = defaultWho;
        n.bar = v["bar"].toString(); n.colour = v["colour"].toString(); if (n.colour.isEmpty()) n.colour = v["color"].toString();
        n.source = source; n.done = (bool) v["done"];
        const auto id = v["id"].toString();
        n.key = id.isNotEmpty() ? id : makeKey (source, n.seconds, n.who, n.text);
        into.push_back (n);
    }
}

inline juce::String parseJson (const juce::var& root, double frameRate, const juce::String& source, Parsed& out)
{
    if (root.isObject() && root["pieces"].isArray())
    {   // a listening copy that held all the pieces: one part per piece
        out.format = "Fermata review"; out.piece = root["title"].toString();
        const juce::var pieces = root["pieces"];
        for (auto& pv : *pieces.getArray())
        {
            Parsed part;
            part.editId = pv["edit"].toString(); part.piece = pv["piece"].toString(); part.format = out.format;
            if (! pv["offset"].isVoid()) { part.offset = (double) pv["offset"]; part.hasOffset = true; }
            readNotes (pv["notes"], frameRate, source, root["who"].toString(), part.notes);
            if (! part.notes.empty()) out.parts.push_back (std::move (part));
        }
        if (out.parts.empty()) return "The file holds no comments.";
        return {};
    }
    const juce::var* list = &root; juce::var notesVar;
    if (root.isObject())
    {
        out.editId = root["edit"].toString(); out.piece = root["piece"].toString();
        if (! root["offset"].isVoid()) { out.offset = (double) root["offset"]; out.hasOffset = true; }
        out.format = root["fermata"].toString().isNotEmpty() ? "Fermata review" : "json";
        notesVar = root["notes"]; if (notesVar.isVoid()) notesVar = root["comments"];
        list = &notesVar;
    }
    if (list->getArray() == nullptr) return "The file is JSON, but it holds no list of comments.";
    readNotes (*list, frameRate, source, root.isObject() ? root["who"].toString() : juce::String(), out.notes);
    if (out.notes.empty()) return "The file holds no comments.";
    return {};
}
} // namespace detail

/** Reads an import file from its raw bytes. UTF-8 (with or without a byte-order mark) or UTF-16 with a mark. Returns an error text, or "" when it worked. */
inline juce::String parse (const void* data, size_t size, double frameRate, const juce::String& source, Parsed& out)
{
    out = {};
    if (size == 0) return "The file is empty.";
    if (size > (size_t) 256 * 1024 * 1024) return "The file is too big to be a list of comments.";
    // UTF-16 with its mark, UTF-8 with or without one - and when the bytes are not UTF-8 at all, the Windows code page a spreadsheet saves in
    // (see TextBytes.h: bytes that are not UTF-8 used to be taken as UTF-8 anyway, and the text then read past its own end - 3.0.0 fuzz test)
    juce::String text = textFromBytes (data, size);
    text = text.trim();
    if (text.startsWithChar ('{') || text.startsWithChar ('['))
    {
        juce::var v;
        if (juce::JSON::parse (text, v).failed()) return "The file looks like JSON but cannot be read.";
        return detail::parseJson (v, frameRate, source, out);
    }
    return detail::parseTable (text, frameRate, source, out);
}

/** Adds the notes whose key is new; a note already there keeps its 'done' tick and gets the new text. Returns how many were added. */
inline int merge (std::vector<FeedbackNote>& into, const std::vector<FeedbackNote>& incoming, int* updated = nullptr)
{
    int added = 0, upd = 0;
    // each key's first place in the list, looked up rather than searched for (walking the list for every note made a long list take seconds: 3.0.0)
    struct KeyHash { size_t operator() (const juce::String& k) const noexcept { return (size_t) k.hashCode64(); } };
    std::unordered_map<juce::String, size_t, KeyHash> at;
    at.reserve (into.size() + incoming.size());
    for (size_t i = 0; i < into.size(); ++i) at.emplace (into[i].key, i);
    for (auto& n : incoming)
    {
        const auto found = at.find (n.key);
        if (found == at.end()) { at.emplace (n.key, into.size()); into.push_back (n); ++added; }
        else { auto& o = into[found->second]; const bool d = o.done; if (o.text != n.text || o.bar != n.bar || o.who != n.who) ++upd; o = n; o.done = d || n.done; }
    }
    std::stable_sort (into.begin(), into.end(), [] (const FeedbackNote& a, const FeedbackNote& b) { return a.seconds < b.seconds; });
    if (updated) *updated = upd;
    return added;
}

/** Every note within 'window' seconds of the first one at a position, as one thread (indices into the list, which is sorted by time). */
inline std::vector<std::vector<int>> threads (const std::vector<FeedbackNote>& list, double window = 0.05)
{
    std::vector<std::vector<int>> out;
    for (int i = 0; i < (int) list.size(); ++i)
    {
        if (! out.empty() && std::abs (list[(size_t) i].seconds - list[(size_t) out.back().front()].seconds) <= window) out.back().push_back (i);
        else out.push_back ({ i });
    }
    return out;
}

} // namespace feedback
} // namespace td

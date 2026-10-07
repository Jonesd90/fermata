#pragma once
#include "Edit.h"

namespace td
{
/** The Mastering window's data: which edits go out, in what order, how, and the layout of the CD (DDP). Saved with the project. */

enum class MasterFormat { Wav = 0, Aiff, Flac, Ogg, Mp3 };
enum class DitherMode { Off = 0, Tpdf };

/** The tags of one file (or the album-wide defaults, for the fields that make sense there). */
struct MasterTags
{
    juce::String title, artist, album, albumArtist, composer, genre, year, comment, copyright, isrc;
    bool operator== (const MasterTags& o) const
    { return title == o.title && artist == o.artist && album == o.album && albumArtist == o.albumArtist && composer == o.composer
          && genre == o.genre && year == o.year && comment == o.comment && copyright == o.copyright && isrc == o.isrc; }
};

/** One edit in the Virtual Master list (the order of the list is the order of the files). */
struct MasterItem
{
    juce::Uuid   editId;
    bool         include = true;
    juce::String fileName;            // empty = the edit's name
    MasterTags   tags;                // per-file tags (empty fields fall back to the album tags)
};

struct MasterExportSettings
{
    MasterFormat format = MasterFormat::Wav;
    double       sampleRate = 0.0;    // 0 = the sample rate of the edits
    int          bitDepth = 24;       // 16, 24, or 32 (= 32-bit float, WAV only)
    DitherMode   dither = DitherMode::Tpdf;     // used for 16 and 24 bit
    int          mp3Kbps = 320;       // constant bit rate; 0 = variable (see mp3Vbr)
    int          mp3Vbr = 2;          // LAME's V0 (best) .. V9; used when mp3Kbps == 0
    int          oggQuality = 6;      // index into the Ogg encoder's quality list
    int          flacLevel = 5;       // FLAC compression 0 (fastest, biggest) .. 8 (slowest, smallest). Always lossless
    int          extraFormats = 0;    // bit f set = also export every file as MasterFormat f in the same run (the chosen "format" is always made)
    bool         normalise = true;
    float        peakDb = -1.0f;
    bool         peakTogether = false;          // false: every file reaches the peak level on its own. true: one gain for all (the loudest reaches it)
    bool         numberFiles = true;            // "01 - name"
    juce::String folder;
    MasterTags   album;               // album, albumArtist, genre, year, comment, copyright: the defaults for every file
};

// ----------------------------------------------------------------------------- the CD
constexpr int kIndexAuto = -2147483647 - 1;

struct DdpClip
{
    juce::Uuid   editId;
    bool         include = true;
    int          gapSectors = 150;    // silence BEFORE this track in 1/75 s (the track's pause / INDEX 00). The first track's is the lead-in, never less than 150 (2 s)
    juce::String title, performer, songwriter, composer, arranger, isrc;
    bool         preEmphasis = false, copyPermitted = false;
    int          index01Shift = 0;    // where the track's INDEX 01 (its PQ start) sits compared with where its audio starts, in sectors (negative = before the audio). Dragged on the timeline
    int          index00Shift = kIndexAuto;   // where its INDEX 00 sits (= the END of the previous track, SADiE style) compared with where its audio starts, in sectors; kIndexAuto = at the end of the previous audio. Never later than INDEX 01 (equal = no countdown, a continuous join). Dragged on the timeline
};

struct DdpDisc
{
    juce::String upc;                 // 13-digit UPC/EAN (the catalogue number on the disc)
    juce::String title, performer, songwriter, composer, arranger;
    int          languageCode = 9;    // CD-Text language code (9 = English)
    std::vector<DdpClip> clips;       // the order of the list is the order on the disc
    bool         normalise = false;
    float        peakDb = -0.3f;
    bool         peakTogether = true; // one gain for the whole disc keeps the balance between the tracks (what a mastering engineer normally wants)
    bool         makeZip = true, makeCue = true;
    juce::String folder;
    int          endPadSectors = 0;   // silence after the last track before the lead-out (the End of CD flag, dragged on the timeline)
};

struct MasteringDef
{
    std::vector<MasterItem> items;
    MasterExportSettings    vm;
    DdpDisc                 ddp;
    juce::Uuid              mixerId = juce::Uuid::null();     // what is rendered: null = the processing mixer ...
    juce::Uuid              sourceId = juce::Uuid::null();    // ... and its first Ext bus
    double                  tailSeconds = 0.0;                // extra time after the last audio (reverb tails)
    int                     view = 0;                         // 0 = Virtual Master, 1 = DDP builder
    juce::Uuid              selectedItem = juce::Uuid::null(), selectedClip = juce::Uuid::null();

    /** Adds edits that are new, drops the ones that are gone (keeps order and settings of the rest). */
    void sync (const std::vector<std::unique_ptr<EditDef>>& edits);
    MasterItem* findItem (const juce::Uuid& id) { for (auto& i : items) if (i.editId == id) return &i; return nullptr; }
    DdpClip* findClip (const juce::Uuid& id) { for (auto& c : ddp.clips) if (c.editId == id) return &c; return nullptr; }

    juce::var toVar() const;
    void fromVar (const juce::var&);
};

// ----------------------------------------------------------------------------- helpers
constexpr int kSamplesPerSector = 588;             // 44100 / 75
constexpr int kBytesPerSector = 2352;
constexpr int kMaxCdSectors = 359775;               // 79:57, what an 80-minute disc holds

/** The length of the file the Mastering window makes of this edit, in samples at the edit's own rate: first audio to last audio, plus the tail. */
juce::int64 masterLengthSamples (const EditDef&, double tailSeconds);
/** The same at a new sample rate (what the resampler produces). */
juce::int64 convertedLength (juce::int64 samples, double fromRate, double toRate);

// ----------------------------------------------------------------------------- the PQ list
struct PqTrack
{
    int          clipIndex = 0;       // index into DdpDisc::clips
    juce::Uuid   editId;
    int          number = 1;          // 1..99
    juce::int64  frames = 0;          // the audio, in 44.1 kHz samples
    int          lengthSectors = 0;   // the audio rounded UP to whole sectors
    int          index00 = -1;        // absolute sector of INDEX 00 = where the previous track ENDS and the countdown to this one begins (-1 = none: INDEX 00 and 01 are at the same place)
    int          index01 = 0;         // absolute sector where the track starts as the PQ code says (INDEX 01); 150 = 00:02:00
    int          audioStart = 0;      // absolute sector where its audio really begins (the same as index01 unless the flag was moved)
    int          endSector = 0;       // first sector after the audio
    int          gapSectors = 0;      // the pause before it
};
struct PqLayout
{
    std::vector<PqTrack> tracks;
    int leadOut = 150;                // absolute sector of the lead-out
    juce::StringArray warnings;       // things that break the Red Book rules or will not fit
    int totalSeconds() const { return (leadOut - 150) / 75; }
};

/** Auto PQ: where every track starts and where its pause begins, from the order and the gaps. 'frames' = the 44.1 kHz length of each clip of disc.clips
    (a clip that is not included is ignored). Because the layout is built from gaps and lengths, changing the length of an edit moves everything after it
    and every gap stays as it was. */
PqLayout computePq (const DdpDisc& disc, const std::vector<juce::int64>& frames);

inline juce::String sectorsToMsf (int sectors)
{
    const int m = sectors / (75 * 60), s = (sectors / 75) % 60, f = sectors % 75;
    return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2) + ":" + juce::String (f).paddedLeft ('0', 2);
}
inline int sectorsFromSeconds (double seconds) { return juce::jmax (0, (int) std::llround (seconds * 75.0)); }

bool isValidUpc (const juce::String& upc);          // 13 digits with a correct EAN check digit (empty = fine, there is none)
bool isValidIsrc (const juce::String& isrc);        // CC-XXX-YY-NNNNN, dashes optional (empty = fine)
juce::String compactIsrc (const juce::String& isrc);   // no dashes, upper case
juce::String toLatin1Text (const juce::String&);    // what CD-Text can hold: ISO 8859-1, anything else becomes '?'

} // namespace td

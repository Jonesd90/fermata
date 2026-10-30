#pragma once
#include "Bounce.h"
#include "MasterDef.h"
#include "Ddp.h"
#include "MasterRender.h"

namespace td
{
/** One file of an export: which edit, and its name (without the extension). */
struct MasterJobItem
{
    juce::Uuid   editId;
    juce::String baseName;
    MasterTags   tags;               // already merged with the album tags
    int          trackNo = 0;
    juce::String editName, projectName;   // for the listening copy
    double       editOffsetSeconds = 0.0;   // where the exported file's time 0 sits on the edit's timeline
};

struct MasterJobSpec
{
    enum class Kind { Files, Disc } kind = Kind::Files;
    MasterExportSettings out;        // format, rate, bit depth, levels (for a disc: 44.1 kHz / 16 bit WAV with the disc's level settings)
    std::vector<MasterJobItem> items;
    std::vector<int> clipIndex;      // Disc: for each item, its index in disc.clips
    juce::Uuid   mixerId, sourceId;
    bool         ownMixers = true;   // every piece goes through its own Edit's mixer (mixerId is only the fallback)
    double       tailSeconds = 0.0;
    juce::File   destFolder;         // Files: the audio files go here. Disc: the folder that gets the DDP folder, the zip and the WAV + CUE
    juce::File   workFolder;         // intermediates (deleted at the end)
    DdpDisc      disc;              // Disc only. disc.clips holds every clip; 'items' are the included ones, in order
    bool         formsOnly = false; // Files: make only the listening copies (one .html per piece, with its audio inside), no audio files
    bool         formSingle = false;// the listening copies: all the pieces in ONE .html (named after formTitle) instead of one per piece
    juce::String formTitle;         // the name of that one page (the album title, or the project's name)
    juce::String discName;
    /** The answer to the loudness check (a piece that cannot reach the target under the ceiling): 0 = ask (the job pauses and calls onLoudnessCheck),
        1 = retain the peak (those pieces stop at the ceiling, below the target), 2 = ignore (the full gain; they clip), 3 = cancel. */
    int          loudnessDecision = 0;
};

/** What the loudness check found: every piece as measured, and the ones that cannot reach the target without going over the ceiling. */
struct LoudnessCheck
{
    struct Piece
    {
        juce::String name;
        double lufs = -100.0, truePeakDb = -100.0;   // as rendered
        double gainDb = 0.0;                         // the gain the target asks for (per album: the same for all)
        double peakAtTargetDb = -100.0;              // the true peak with that gain
        double excessDb = 0.0;                       // how far over the ceiling that is (> 0 = this piece is a problem: it needs this much limiting / compression)
    };
    std::vector<Piece> pieces;
    double targetLufs = -23.0, ceilingDbTp = -1.0;
    bool perAlbum = false;
    double albumLufs = -100.0;                       // perAlbum: the album measured as one programme
    int problems() const { int n = 0; for (auto& p : pieces) if (p.excessDb > 0.0) ++n; return n; }
    double worstExcessDb() const { double w = 0.0; for (auto& p : pieces) w = juce::jmax (w, p.excessDb); return w; }
    /** The lines of the report. */
    juce::String text() const;
};

/** The gain (dB) each piece gets for a loudness target, and the problems: the heart of the check (pure, tested). */
LoudnessCheck planLoudness (const std::vector<juce::String>& names, const std::vector<double>& lufs, const std::vector<double>& truePeakDb, double albumLufs,
                            double targetLufs, double ceilingDbTp, bool perAlbum);
/** The gains after a decision (1 retain the peak, 2 ignore). */
std::vector<double> loudnessGains (const LoudnessCheck&, int decision);

struct MasterExportResult
{
    juce::String error;              // empty = fine
    juce::StringArray files;         // what was written (Disc: the DDP folder, the zip and the WAV / CUE)
    float peakDb = -100.0f;          // loudest sample written
    bool clipped = false, cancelled = false;
    juce::String report;             // Disc: what the checker found in the finished DDP. Files with a loudness target: every piece's loudness, true peak and gain
    std::vector<juce::int64> frames; // the length of each item's audio at the target rate
    LoudnessCheck loudness;          // what was measured (a loudness target only)
};

/** Builds the list of files for the Virtual Master (or says why it cannot): included edits, in list order, named and tagged. */
juce::String makeFilesSpec (const Project&, const MasteringDef&, MasterJobSpec& spec);
/** The listening copies on their own (the Listening copy tab): the same pieces as the Virtual Master, at the Virtual Master's levels, with the audio
    inside each page as chosen on that tab (WAV / FLAC / MP3, its own sample rate and bit depth). No audio files are made. */
juce::String makeFormsSpec (const Project&, const MasteringDef&, MasterJobSpec& spec);
/** The same for the DDP: the included clips, in order, as 44.1 kHz / 16 bit files. */
juce::String makeDiscSpec (const Project&, const MasteringDef&, MasterJobSpec& spec);

/** The mixer and output used for rendering (the defaults: processing mixer, its first Ext bus). Returns an error text or "". */
juce::String resolveRenderSource (const Project&, const MasteringDef&, juce::Uuid& mixerId, juce::Uuid& sourceId);

double masterTargetRate (const MasterExportSettings&, double editRate);
juce::String masterFileExtension (MasterFormat);

/** The command that runs LAME (full path, or just "lame" when it is on the PATH); empty if it cannot be found. Looks next to the program first. */
juce::String findLame();

/** Splices a Vorbis-comment block (the tags of a FLAC file) into a finished FLAC file. */
bool addFlacTags (const juce::File& flac, const MasterTags&, int trackNo);

/** Renders, converts, levels and writes everything on a background thread. Create and destroy on the message thread; 'done' is called on the message thread. */
class MasterExportJob : private juce::Thread
{
public:
    /** startNow = false: nothing runs in the background; call execute() yourself (the tests do). */
    MasterExportJob (const Project& live, const MasterJobSpec&, std::function<void (MasterExportResult)> done, bool startNow = true);
    ~MasterExportJob() override;
    float getProgress() const;
    juce::String getStatus() const;
    /** Starts the work in the background (for a job made with startNow = false, once its callbacks are set). */
    void start() { if (! isThreadRunning()) startThread (juce::Thread::Priority::normal); }
    void cancel() noexcept { cancelFlag = true; decide (3); }
    bool isRunning() const { return isThreadRunning(); }
    /** The loudness check: when a piece cannot reach the target under the ceiling and the spec says ask, the job stops here (nothing written yet) and
        calls this on the message thread; it goes on when decide() is called (1 retain the peak, 2 ignore, 3 cancel). */
    std::function<void (const LoudnessCheck&)> onLoudnessCheck;
    void decide (int decision) { decision_.store (decision); decided.signal(); }
    bool isWaitingForDecision() const noexcept { return waiting.load(); }
    /** Does the whole job on the calling thread (and removes the intermediate files). */
    MasterExportResult execute();

private:
    void run() override;
    MasterExportResult executeInner();
    juce::String finalise (const juce::File& src, const juce::File& dest, double rate, float gain, const MasterJobItem& item, float& peakOut, MasterFormat fmt);

    MasterJobSpec spec;
    std::unique_ptr<MasterRenderBatch> batch;       // the renders to export (up to date ones are reused, the others are made first)
    std::function<void (MasterExportResult)> onDone;
    std::atomic<float> sub { 0.0f };
    std::atomic<int> stage { 0 };
    std::atomic<bool> cancelFlag { false };
    mutable juce::CriticalSection lock;
    juce::String status, earlyError;
    juce::WaitableEvent decided; std::atomic<int> decision_ { 0 }; std::atomic<bool> waiting { false };
};
} // namespace td

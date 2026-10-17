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
};

struct MasterExportResult
{
    juce::String error;              // empty = fine
    juce::StringArray files;         // what was written (Disc: the DDP folder, the zip and the WAV / CUE)
    float peakDb = -100.0f;          // loudest sample written
    bool clipped = false, cancelled = false;
    juce::String report;             // Disc: what the checker found in the finished DDP
    std::vector<juce::int64> frames; // the length of each item's audio at the target rate
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
    void cancel() noexcept { cancelFlag = true; }
    bool isRunning() const { return isThreadRunning(); }
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
    std::vector<double> editRates;
};
} // namespace td

#pragma once
#include "Bounce.h"
#include "MasterDef.h"

namespace td
{
/** What decides how an Edit sounds when the Mastering window renders it. */
struct RenderParams
{
    juce::Uuid sourceId;            // the output that is rendered (an Ext bus, an Int bus or a track)
    juce::Uuid mixerId;             // the mixer used when ownMixers is false; otherwise only the fallback
    bool       ownMixers = true;    // every Edit goes through its own mixer
    double     tailSeconds = 0.0;
};

/** The render settings of the Mastering window (an error text, or "" when it is fine). */
juce::String makeRenderParams (const Project&, const MasteringDef&, RenderParams&);

/** A short text that changes whenever the rendered sound of this Edit would change: its pieces, fades and automation, the audio files they use,
    the mixer(s) it goes through (levels, sends, plug-in states), the routing and the render settings. Message thread. */
juce::String masterRenderFingerprint (const Project&, const EditDef&, const RenderParams&);

/** The renders of the Mastering window, kept in the project's "Mastering renders" folder: at most TWO per Edit (slots A and B).
    The newest one is the one that is used; a new render always replaces the older of the two, so the one before the newest stays as a backup.
    Everything here is safe to call from any thread (one lock, the list is read from / written to disk every time). */
class MasterRenders
{
public:
    explicit MasterRenders (const juce::File& folder) : dir (folder) {}
    static juce::File folderFor (const Project&);

    enum class State { None, Ready, Stale, Previous };
    struct Info { State state = State::None; juce::File file; double rate = 0.0; juce::int64 frames = 0; bool hasBackup = false; };

    /** Is there a render of this Edit that matches 'fingerprint'? Ready = yes (its file is in Info). If the OTHER slot matches (the Edit was changed back),
        that slot is made the current one. Previous = the user chose the earlier version (it is used whatever the Edit looks like now). */
    Info check (const juce::Uuid& editId, const juce::String& fingerprint);
    /** Where a render in progress is written. */
    juce::File tempFile (const juce::Uuid& editId) const { return dir.getChildFile ("mrtmp-" + editId.toString() + ".wav"); }
    /** A finished render: it takes the place of the older of the two, and becomes the current one. */
    void commit (const juce::Uuid& editId, const juce::String& fingerprint, double rate, juce::int64 frames, const juce::File& produced);
    /** Previous on: the earlier render is used instead of the newest. Off: back to the newest. False if there is no earlier one. */
    bool setUsePrevious (const juce::Uuid& editId, bool on);
    /** The next check says Stale, so the Edit is rendered again (into the older slot, as always). */
    void markStale (const juce::Uuid& editId);
    /** Deletes every render. */
    void clearAll();
    void removeTemps() const;
    const juce::File& folder() const { return dir; }

private:
    struct Slot { juce::String fp; double rate = 0.0; juce::int64 frames = 0; };
    struct Entry { Slot s[2]; int active = -1; bool pinned = false, stale = false; };
    std::map<juce::String, Entry> load() const;
    void save (const std::map<juce::String, Entry>&) const;
    juce::File slotFile (const juce::Uuid& id, int i) const { return dir.getChildFile (id.toString() + (i == 0 ? "-A.wav" : "-B.wav")); }
    bool valid (const juce::Uuid& id, const Entry& e, int i) const { return e.s[i].fp.isNotEmpty() && slotFile (id, i).existsAsFile(); }
    juce::File dir;
};

/** Renders the Edits that have no up-to-date render (each through its own mixer, with its automation, 32-bit float, tail included) and hands back the file to use
    for every Edit. Create it on the message thread (it copies the project for the pieces that need rendering); run() can be called from any thread. */
class MasterRenderBatch
{
public:
    MasterRenderBatch (const Project& live, const std::vector<juce::Uuid>& editIds, const RenderParams&, bool force = false);
    ~MasterRenderBatch();
    const juce::String& getPrepareError() const { return prepareError; }
    int total() const { return (int) items.size(); }
    int toRender() const;
    /** Renders what is missing and stores it. "" = fine, "Cancelled." = stopped, anything else = the problem. */
    juce::String run (std::atomic<float>* progress, const std::atomic<bool>* cancel);
    /** After run(): the render to use for the i-th Edit (a file that does not exist if that Edit has nothing to render). */
    juce::File fileFor (int i) const { return items[(size_t) i].file; }
    juce::Uuid editAt (int i) const { return items[(size_t) i].editId; }
    double rateFor (int i) const { return items[(size_t) i].rate; }
    juce::int64 framesFor (int i) const { return items[(size_t) i].frames; }       // the length of the render, tail included
    bool needsRender (int i) const { return items[(size_t) i].stale; }
    bool isPrevious (int i) const { return items[(size_t) i].previous; }           // the earlier version was chosen for this Edit
    bool hasBackup (int i) const { return items[(size_t) i].backup; }

private:
    struct Item { juce::Uuid editId; juce::String fp; juce::File file; bool stale = false, previous = false, backup = false; double rate = 0.0; juce::int64 frames = 0; };
    struct Group { double rate = 0.0; std::unique_ptr<Bouncer> bouncer; std::vector<int> idx; };
    std::vector<Item> items;
    std::vector<Group> groups;
    juce::File folder;
    juce::String prepareError;
};

/** A MasterRenderBatch on a background thread. Create and destroy on the message thread; 'done' is called on the message thread (error "", "Cancelled." or a problem). */
class MasterRenderJob : private juce::Thread
{
public:
    MasterRenderJob (std::unique_ptr<MasterRenderBatch>, std::function<void (juce::String)> done);
    ~MasterRenderJob() override;
    float getProgress() const { return progress.load(); }
    void cancel() noexcept { cancelFlag = true; }
    bool isRunning() const { return isThreadRunning(); }
    const MasterRenderBatch& batchRef() const { return *batch; }

private:
    void run() override;
    std::unique_ptr<MasterRenderBatch> batch;
    std::function<void (juce::String)> onDone;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancelFlag { false };
};
} // namespace td

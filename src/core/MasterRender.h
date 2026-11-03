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
/** Looks at a piece's description several times over a fraction of a second; whatever changes by itself (not through anything the person did) is left out of the fingerprint from then on. Logged. */
juce::StringArray learnMasterRenderVolatile (const Project&, const EditDef&, const RenderParams&);
/** The description behind the fingerprint, part by part (path -> value). Message thread. */
using MasterRenderParts = std::map<juce::String, juce::String>;
MasterRenderParts masterRenderParts (const Project&, const EditDef&, const RenderParams&);
/** 3.27.1: true when the only parts that differ between two descriptions of this Edit are ones known to change by themselves (learnMasterRenderVolatile).
    A render made from 'before' may then be stamped with the description 'now'; if anything else differs, the person changed the piece while it was
    rendered, and the render is out of date. */
bool masterRenderOnlyVolatileDiffers (const MasterRenderParts& before, const MasterRenderParts& now, const juce::Uuid& editId);

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
    /** A finished render: it takes the place of the older of the two, and becomes the current one. False when the file could not be moved into its
        slot (the list is then left as it was, and 'produced' stays where it is for removeTemps). */
    bool commit (const juce::Uuid& editId, const juce::String& fingerprint, double rate, juce::int64 frames, const juce::File& produced);
    /** 3.27.1: forgets the renders of Edits that are not in 'liveEditIds' any more (deleted edits), files and all; the folder used to only grow. */
    void prune (const std::vector<juce::Uuid>& liveEditIds);
    /** A render just finished for this Edit: its sound is described by 'fingerprint' as the project stands now (the project can describe itself slightly differently
        after a render, e.g. normalised gains settle), so the newest render takes that description. */
    void restamp (const juce::Uuid& editId, const juce::String& fingerprint);
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
    static bool changed (const std::map<juce::String, Entry>& a, const std::map<juce::String, Entry>& b);
    juce::File slotFile (const juce::Uuid& id, int i) const { return dir.getChildFile (id.toString() + (i == 0 ? "-A.wav" : "-B.wav")); }
    bool valid (const juce::Uuid& id, const Entry& e, int i) const { return e.s[i].fp.isNotEmpty() && slotFile (id, i).existsAsFile(); }
    juce::File dir;
};

/** Renders the Edits that have no up-to-date render (each through its own mixer, with its automation, 32-bit float, tail included) and hands back the file to use
    for every Edit. Create it on the message thread (it copies the project for the pieces that need rendering); run() can be called from any thread. */
class MasterRenderBatch
{
public:
    /** 3.26.0 stems: every Edit is also rendered through each bus in 'stems' (in the same pass as its master render when that is out of date, else in a
        pass of their own: an up-to-date master render and its backup are never touched). The stem renders are not kept with the master renders: they go
        to 'stemFolder' (stemFileFor), for the caller to use and delete. */
    MasterRenderBatch (const Project& live, const std::vector<juce::Uuid>& editIds, const RenderParams&, bool force = false,
                       const std::vector<juce::Uuid>& stems = {}, const juce::File& stemFolder = {});
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
    /** After run(): the render of the i-th Edit through stem bus s (32-bit float, the Edit's rate, as long as its master render). */
    juce::File stemFileFor (int i, int s) const { const auto& v = items[(size_t) i].stemFiles; return juce::isPositiveAndBelow (s, (int) v.size()) ? v[(size_t) s] : juce::File(); }
    int numStems() const { return (int) stems.size(); }

private:
    struct Item { juce::Uuid editId; juce::String fp, name; juce::File file; bool stale = false, previous = false, backup = false; double rate = 0.0; juce::int64 frames = 0;
                  std::vector<juce::File> stemFiles; };
    struct Group { double rate = 0.0; bool master = true; std::unique_ptr<Bouncer> bouncer; std::vector<int> idx; };     // master: this pass makes the master renders too (else only the stems)
    std::vector<Item> items;
    std::vector<Group> groups;
    juce::File folder;
    juce::String prepareError;
    std::vector<juce::Uuid> stems;
    juce::File stemDir;
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

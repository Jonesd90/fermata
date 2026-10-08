#pragma once
#include <map>
#include "Common.h"
#include "Preamp.h"
#include "Mixer.h"
#include "Takes.h"
#include "Edit.h"
#include "MasterDef.h"
#include <array>

namespace td
{
enum class TrackFormat { Mono, Stereo, Surround };

struct InputChannel
{
    juce::String   name;              // the name you gave it (shown everywhere)
    juce::String   driverName;        // the audio driver's own name for this channel
    PreampSettings preamp;
};

struct TrackDef
{
    juce::Uuid   id;
    juce::String name;
    TrackFormat  format = TrackFormat::Mono;
    int          surroundChannels = 6;   // used when format == Surround
    juce::uint32 colour = 0;             // the channel's colour as ARGB (0 = automatic); shown on the mixer strip and on the track in the take / edit windows
    std::array<int, kMaxTrackChannels> inputs;   // 0-based physical input of each channel of the track (-1 = none)
    std::shared_ptr<TrackLive> live = std::make_shared<TrackLive>();

    TrackDef() { inputs.fill (-1); }

    bool isArmed() const noexcept        { return live->armed.get(); }
    void setArmed (bool b) noexcept      { live->armed.set (b); }
    Monitor monitor() const noexcept     { return live->getMonitor(); }
    void setMonitor (Monitor m) noexcept { live->monitor.store ((int) m); }

    int channelCount() const
    {
        switch (format) { case TrackFormat::Mono: return 1; case TrackFormat::Stereo: return 2; default: break; }
        return juce::jlimit (3, kMaxTrackChannels, surroundChannels);
    }
    int inputOf (int c) const noexcept { return juce::isPositiveAndBelow (c, kMaxTrackChannels) ? inputs[(size_t) c] : -1; }
};

/** An Int Bus (fed by audio tracks / other Int Buses, goes to tracks and buses) or an Ext Bus
    (fed by tracks / Int Buses, MUST go to driver outputs or nowhere). */
struct BusDef { juce::Uuid id; juce::String name; bool external = false; juce::uint32 colour = 0; };

enum class NodeKind { None, Track, IntBus, ExtBus };
enum class SendResult { Refused, Changed, RoutingChanged };
struct OutputDef { juce::String name; juce::String driverName; };

/** Builds a processor from a saved description + state (the app supplies a VST3 implementation). */
using InsertFactory = std::function<std::unique_ptr<InsertProcessor> (const juce::String& description,
                                                                      const juce::String& stateBase64)>;

/** The whole session: design (inputs / tracks / effects channels / outputs), mixers and take windows. */
class Project : public juce::ChangeBroadcaster
{
public:
    Project();

    // --- design (message thread) ---
    std::vector<InputChannel> inputs;
    std::vector<TrackDef>     tracks;
    std::vector<BusDef>        buses;
    std::vector<OutputDef>    outputs;
    std::vector<int> crMicInputs;  // the driver inputs that make up the Control Room mic: any number of them (empty = none)
    /** The stage speaker mixer (main window, top right): what the TB pair carries. Indexed by driver input (grown as needed); 0 dB and centre = the old behaviour. */
    std::vector<float> stageGainDb, stagePan;
    float stagePlaybackDb = 0.0f, stageOutputDb = 0.0f;
    float stageGainOf (int input) const { return input >= 0 && input < (int) stageGainDb.size() ? stageGainDb[(size_t) input] : 0.0f; }
    float stagePanOf (int input) const  { return input >= 0 && input < (int) stagePan.size() ? stagePan[(size_t) input] : 0.0f; }
    bool isCrMic (int input) const { return std::find (crMicInputs.begin(), crMicInputs.end(), input) != crMicInputs.end(); }
    /** The mixer the Stream Deck's second Mixer key opens (null = the first mixer after the processing mixer). */
    juce::Uuid altMixer = juce::Uuid::null();
    /** Index in 'mixers' of the Alt Mixer, or -1 when there is only the processing mixer. */
    int altMixerIndex() const
    {
        const int ce = cueEnd();
        for (int i = 1; i < ce; ++i) if (mixers[(size_t) i]->id == altMixer) return i;
        return ce > 1 ? 1 : -1;
    }
    /** The mixers are kept in this order: the processing mixer, the cue mixers, then the Edit mixers (one per Edit). This is the index of the first Edit mixer (= the number of the others). */
    int cueEnd() const
    {
        int n = 0;
        for (auto& m : mixers) { if (! m->editId.isNull()) break; ++n; }
        return n;
    }
    /** The mixer that belongs to this Edit (nullptr if it has none). */
    MixerState* mixerOfEdit (const juce::Uuid& editId) const
    {
        if (editId.isNull()) return nullptr;
        for (auto& m : mixers) if (m->editId == editId) return m.get();
        return nullptr;
    }
    /** Gives every Edit its own mixer (a copy of the processing mixer's settings and output routing when it is made), names them after their Edits and removes the ones whose Edit is gone.
        Returns true if anything changed (the caller announces that with structureChanged()). */
    bool syncEditMixers();
    std::vector<int> tbOutputs;   // the talkback pairs: each is the first driver output of a stereo pair (that output and the next); any number; the last output alone is mono
    std::vector<std::unique_ptr<MixerState>>   mixers;
    std::vector<std::unique_ptr<TakeWindowDef>> takeWindows;
    std::vector<std::unique_ptr<EditDef>>      edits;
    MasteringDef mastering;                                     // the Mastering window: Virtual Master export list and the CD (DDP) layout
    std::vector<std::unique_ptr<MixerState>>   retiredMixers;   // removed mixers stay alive until the app closes
    void retireAllMixers();
    juce::String name { "Untitled" };

    /** Makes the input / output lists match the audio driver (names are kept for channels that already existed). */
    void syncWithDevice (const juce::StringArray& inputNames, const juce::StringArray& outputNames);
    /** The name to show for input i: your name for it, falling back to the driver's. */
    juce::String inputLabel (int i) const;
    /** Sets the optional name of an input; mono tracks on it that still have an automatic name follow it (so it shows at the top of the mixer strip). */
    void setInputName (int i, const juce::String& name);
    juce::String outputLabel (int i) const;
    void setInputCount (int n);
    void setOutputCount (int n);
    TrackDef& addTrack (const juce::String& trackName, TrackFormat f, int firstInput);
    void moveTrack (int from, int to);
    /** Gives the track its inputs: channel 0 gets 'first', channel 1 gets first+1, ... (clipped to the inputs that exist). */
    void assignInputsFrom (TrackDef&, int first) const;
    void removeTrack (int index);
    BusDef& addBus (const juce::String& busName, bool external = false);
    void removeBus (int index);
    /** Moves a bus up (-1) or down (+1) among the buses of its own kind (Int or Ext). */
    void moveBus (int index, int delta);
    /** Global indices (into 'buses') of the Int Buses / Ext Buses, in the order they were added or arranged. */
    std::vector<int> busIndices (bool external) const;
    BusDef* findBus (const juce::Uuid&);
    NodeKind kindOf (const juce::Uuid&) const;
    /** Gives an audio track or a bus its own colour (ARGB; 0 = back to the automatic colour). Everything that shows the channel repaints. */
    void setChannelColour (const juce::Uuid&, juce::uint32 argb);
    /** The colour you gave the channel (0 = none, the interface picks one). */
    juce::uint32 channelColour (const juce::Uuid&) const;
    juce::String nodeName (const juce::Uuid&) const;
    /** Which Ext buses (of which mixers) feed driver output 'out' (0-based), e.g. "Processing mixer: Out"; empty if none. */
    juce::String outputFedBy (int out) const;
    /** The driver outputs the Ext buses of one mixer go to, e.g. "Out -> 1-2, Foldback -> 3-4"; "(no outputs)" if none. */
    juce::String mixerOutputsText (const MixerState&) const;
    /** The sends of a track or bus in a mixer (nullptr if the id is neither, or an Ext Bus). */
    SendList* sendsOf (MixerState&, const juce::Uuid& nodeId) const;
    /** Could 'src' send to 'dest' in this mixer? Tracks and Int Buses can send to tracks, Int Buses and Ext Buses; never to themselves or in a loop. */
    bool sendAllowed (const MixerState&, const juce::Uuid& src, const juce::Uuid& dest) const;
    /** Sets a send level (<= -60 dB = off). RoutingChanged means a send was switched on: the engine must re-plan. */
    SendResult setSendLevel (MixerState&, const juce::Uuid& src, const juce::Uuid& dest, float db);
    MixerState& addMixer (const juce::String& mixerName);
    void removeMixer (int index);
    TakeWindowDef& addTakeWindow (const juce::String& windowName);
    void removeTakeWindow (int index);
    EditDef& addEdit (const juce::String& editName, const juce::Uuid& windowId);
    EditDef* findEdit (const juce::Uuid&);
    EditDef* editForWindow (const juce::Uuid& windowId);   // the first edit made from this take window
    TakeGroup* findTake (const juce::Uuid& windowId, const juce::Uuid& takeId);
    TrackDef* findTrack (const juce::Uuid&);
    /** The tracks an edit shows, in its own order (all project tracks when the edit has no list of its own). Ids that no longer exist are skipped. */
    std::vector<const TrackDef*> editTracks (const EditDef&) const;
    /** Where each window was last (window key -> "x y w h"), saved with the project so a window opens again where and how big it was left. Not an undo step, not a change. */
    std::map<juce::String, juce::String> windowBounds;
    /** Track ids the edit's audio uses that are not in the project any more (so no mixer strip exists for them): id -> (name, channels). */
    std::vector<std::pair<juce::Uuid, std::pair<juce::String, int>>> missingTracksOf (const EditDef&) const;
    /** Puts a deleted track back under its old id (the mixers get a strip for it). */
    void restoreTrack (const juce::Uuid& id, const juce::String& trackName, int channels);
    /** Gives the edit its own track list (the current project order) if it has none yet. */
    void materialiseEditTracks (EditDef&) const;
    TakeWindowDef* findTakeWindow (const juce::Uuid&);
    juce::Array<int> inputsOfTrack (const TrackDef&) const;

    /** The balance of a mixer (levels, pans, mutes, solos, sends, effects returns - and optionally the plug-in inserts) as data. */
    juce::var mixSnapshot (const MixerState&, bool withInserts) const;
    /** Puts such a balance onto a mixer (its master level and output pair are left alone). */
    void applyMix (MixerState& to, const juce::var& snapshot, double sampleRate, int maxBlock);

    /** Makes sure every mixer has a strip per track and a return per effects channel. */
    void syncMixers();
private:
    MixerState& makeEditMixer (const EditDef&);
public:
    /** The program sets this: strips created from now on have their output button on (so a new track is heard). Saved projects keep what they saved. */
    bool newStripsToMain = false;

    // --- preamps ---
    void setPreamp (int inputIndex, const PreampSettings&);
    /** Records what the hardware reports, without sending anything back to it. */
    void adoptPreamp (int inputIndex, const PreampSettings&);
    std::vector<PreampDeviceCfg> preampDevices;       // the Merging devices whose preamps Fermata controls (see Ravenna.h)
    std::shared_ptr<PreampDriver> preampDriver = std::make_shared<NullPreampDriver>();

    // --- plugins ---
    InsertFactory insertFactory;

    // --- files ---
    juce::File projectFile;
    juce::File readingFrom;                          // while a project file is being read: its folder (to find the files that live inside it)
    juce::File loadBase() const { return readingFrom != juce::File() ? readingFrom : projectFile.getParentDirectory(); }
    /** The project's own folder (where the .fermata file is) and its three working folders:
          Recorded Media            every recording made by the program
          Bounced Media             what is bounced from the Take / Edit windows
          Bounced Media/Mastered Audio   what is bounced from the Mastering window */
    juce::File projectFolder() const;
    juce::File audioFolder() const;                  // Recorded Media
    juce::File processingFolder() const;            // Processing Media: audio sent out with "Export for Processing"
    juce::File bouncedFolder() const;                // Bounced Media
    juce::File masteredFolder() const;               // Bounced Media/Mastered Audio
    void createFolders() const;
    /** After a crash or power cut: recordings that were still being made have a header that says 'empty'. Puts the headers right and the takes' lengths,
        so everything that was recorded up to the moment it stopped is there. Returns a line for each take that was mended. */
    juce::StringArray repairInterruptedRecordings();
    juce::File takeFolder (const TakeWindowDef&) const;
    bool save (juce::String& error);
    bool saveAs (const juce::File&, juce::String& error);
    bool load (const juce::File&, juce::String& error);
    void createDefaultDesign();

    /** A private, independent copy of the whole project (plug-ins re-created from their state) for offline rendering. */
    std::unique_ptr<Project> cloneForRender (double sampleRate, int maxBlock) const;
    double restoreSampleRate = 48000.0; int restoreBlock = 512;    // used when plug-ins are re-created by fromVar()

    juce::var toVar (bool editorialOnly = false) const;
    juce::var mixerToVar (const MixerState&) const;      // one mixer, as saved (levels, sends, plug-in states)
    juce::var editToVar (const EditDef&) const;          // one edit, as saved
    bool fromVar (const juce::var&);

    // ---- undo: the last 30 states of the takes' details (bars, duds, names, positions, removed takes), the edits and the mastering ----
    static constexpr int kUndoLevels = 30;
    std::vector<juce::String> undoStack, redoStack;      // older / newer states (JSON)
    juce::String undoCurrent;                            // the state as of the last quiet moment
    bool undoPending = false; juce::uint32 undoDirtyAt = 0;
    juce::String undoState() const;                      // what the history looks at, as text
    void undoReset() { undoStack.clear(); redoStack.clear(); undoCurrent = undoState(); undoPending = false; }
    /** Call a few times a second: once changes have stopped for half a second the previous state is kept as an undo level (if anything undoable changed). */
    void undoTick (bool recording, bool force = false);
    bool undo (const juce::Uuid& liveTake = {});         // false if there is nothing to undo; a take being recorded is never touched
    bool redo (const juce::Uuid& liveTake = {});
    int undoDepth() const { return (int) undoStack.size(); }
    int redoDepth() const { return (int) redoStack.size(); }

    /** Anything edited: take moved/renamed, preamp changed, recording finished... (listen on the Project itself) */
    /** Re-places every automation point on the audio it belongs to (the edits' pieces may have moved). Cheap; safe to call at any time. */
    void resolveAllAutomation() { for (auto& e : edits) if (e != nullptr && ! e->lanes.empty()) e->resolveAutomation(); }
    void changed()          { resolveAllAutomation(); dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); sendChangeMessage(); }
    /** Tracks / effects channels / mixers / inputs / outputs / names changed: UI must rebuild, engine must re-plan. */
    void structureChanged() { resolveAllAutomation(); dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); structure.sendChangeMessage(); sendChangeMessage(); }
    void markDirty() noexcept { dirty = true; }          // value tweaks (faders etc.) that need saving but no UI refresh
    juce::ChangeBroadcaster structure;
    std::atomic<bool> dirty { false };
};
} // namespace td

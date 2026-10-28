#pragma once
#include <set>
#include <map>
#include "Common.h"
#include "Preamp.h"
#include "Mixer.h"
#include "Takes.h"
#include "Edit.h"
#include "MasterDef.h"
#include "Denoise.h"
#include "ScreenLayout.h"
#include "ConsonantData.h"
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
    bool         isLtc = false;          // the track that records the LTC (timecode) signal; it is shown in the take / edit windows only when LTC is switched on there
    std::array<int, kMaxTrackChannels> inputs;   // 0-based physical input of each channel of the track (-1 = none)
    std::shared_ptr<TrackLive> live = std::make_shared<TrackLive>();
    /** Spot-mic time alignment (see timealign::analyse): the track's role - 0 not used, 1 main, 2 spot, 3 distance. The role belongs to the track
        (where the mic is in the set-up); the delays and measurements belong to each piece (Project::alignSets). */
    int          alignRole = 0;

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

/** One track's last spot-mic measurement (see timealign::analyse), kept so the results can be looked at later - in post - and a spot's delay can
    follow a new pan without measuring again. */
struct AlignResult
{
    bool   measured = false;
    double leftMs = 0.0, rightMs = 0.0; bool hasLeft = false, hasRight = false;     // a spot's lead over the left / right main channels
    double arrivalMs = 0.0;                                                           // a distance mic: how much later than the main pair
    double spreadMs = 0.0;
    float  clarity = 0.0f, suggestedPan = 0.0f;
    float  suggestedPanR = 0.0f; bool suggestedStereo = false;                         // a stereo pair: suggestedPan for its left channel, this for its right
    bool   inverted = false;
    int    severity = 0;
    juce::String message;
};
/** One track in a piece's alignment: its delay (on or off) and the measurement it came from. */
struct AlignTrack
{
    double ms = 0.0;
    bool   on = false;
    AlignResult result;
};
/** A piece's spot-mic alignment: a take window's (measured on one of its takes), or an edit's own (measured on the edit, in post). People, mics,
    distances and the temperature change from piece to piece, so every piece keeps its own. The mixers use the set of the piece being recorded,
    played or worked on (Project::alignActive). */
struct AlignSet
{
    std::map<juce::String, AlignTrack> tracks;       // track id -> its delay and measurement
    juce::String measuredOn;                          // "Take 004 - Kyrie (the whole take, 6:12), 22 Oct 2026 21:30"
    bool hasDelays() const { for (auto& kv : tracks) if (kv.second.on && kv.second.ms > 0.0) return true; return false; }
    bool measured() const  { for (auto& kv : tracks) if (kv.second.result.measured) return true; return measuredOn.isNotEmpty(); }
    AlignTrack* find (const juce::Uuid& track) { auto it = tracks.find (track.toString()); return it != tracks.end() ? &it->second : nullptr; }
    const AlignTrack* find (const juce::Uuid& track) const { auto it = tracks.find (track.toString()); return it != tracks.end() ? &it->second : nullptr; }
};

/** The pitch pipe (its window, from the menu bar): kept with the project, as the outputs it plays to belong to this project's set-up. */
struct PitchPipeDef
{
    double refA = 440.0;                  // the reference: A4 in Hz
    int    temperament = 0;               // tuning::Temperament
    int    key = 0;                       // the key the tuning is built on (pitch class, 0 = C)
    bool   chordMode = false;             // the Chord tick box: notes are chosen first, then held together with Play
    int    chordTuning = 0;               // tuning::ChordTuning (chord mode only)
    float  levelDb = -14.0f;
    std::vector<int> outputs;             // the driver outputs it plays to (0-based)
    int    lowestC = 48;                  // the keyboard's lowest key (MIDI: 48 = C3); it shows two octaves from there
    float  tone = 0.35f;                  // 0 = soft ... 1 = bright (a low-pass that follows each note's pitch)
    bool   keyboard = false;              // the Keyboard sound tick box: a simple keyboard note instead of the reed
    bool   toTalkback = false;            // its To talkback button: also to the talkback pair, at the stage speaker mixer's Pitch pipe level (3.1.0)
};

/** The click (its window, from the menu bar): the tempo, the bar, the sound and where it goes. Switched on and off by hand (never saved as on). */
struct ClickDef
{
    double bpm = 100.0;
    int    beatsPerBar = 4, beatUnit = 4;
    bool   accent = true;
    int    sound = 2;                     // ClickTrack::Sound (2 = Soft)
    float  levelDb = -14.0f;
    bool   quaverTicks = false;           // a very quiet hi-hat on every quaver, whatever the sound
    float  tickDb = -20.0f;               // how far below the beat those ticks sit
    std::vector<int> outputs;
    bool   toTalkback = false;            // its To talkback button: also to the talkback pair, at the stage speaker mixer's Click level (3.1.0)
};

/** An Int Bus (fed by audio tracks / other Int Buses, goes to tracks and buses) or an Ext Bus
    (fed by tracks / Int Buses, MUST go to driver outputs or nowhere). */
struct BusDef { juce::Uuid id; juce::String name; bool external = false; juce::uint32 colour = 0; };

/** A VCA fader: one of up to 16, numbered 1 .. 16 (the number is kept when others are removed). It carries no audio: in each mixer it controls the level of the audio tracks and
    Int Buses chosen for it there (see VcaState). Written "4 - Strings" when it has a name. */
struct VcaDef
{
    juce::Uuid id; int number = 1; juce::String name; juce::uint32 colour = 0;
    juce::String label() const { return juce::String (number) + (name.trim().isNotEmpty() ? " - " + name.trim() : juce::String()); }
};

enum class NodeKind { None, Track, IntBus, ExtBus, Vca };
enum class SendResult { Refused, Changed, RoutingChanged };
struct OutputDef { juce::String name; juce::String driverName; };

/** Builds a processor from a saved description + state (the app supplies a VST3 implementation). */
using InsertFactory = std::function<std::unique_ptr<InsertProcessor> (const juce::String& description,
                                                                      const juce::String& stateBase64)>;

/** The whole session: design (inputs / tracks / effects channels / outputs), mixers and take windows. */
/** Which parts of a mixer a copy takes: faders (levels, mutes, solos, output buttons), sends, pans, plug-in inserts. */
struct MixParts { bool faders = true, sends = true, pan = true, inserts = true; };

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
    float stageClickDb = 0.0f, stagePipeDb = 0.0f;     // the Click and Pitch pipe boxes: those two, when their To talkback button is on (3.1.0; one box each since 3.1.1)
    float stageGainOf (int input) const { return input >= 0 && input < (int) stageGainDb.size() ? stageGainDb[(size_t) input] : 0.0f; }
    float stagePanOf (int input) const  { return input >= 0 && input < (int) stagePan.size() ? stagePan[(size_t) input] : 0.0f; }
    bool isCrMic (int input) const { return std::find (crMicInputs.begin(), crMicInputs.end(), input) != crMicInputs.end(); }
    // LTC (timecode as audio): the driver input it arrives on, the driver output Fermata sends its own on, and what that one is.
    int    ltcInput = -1, ltcOutput = -1;
    bool   ltcOutOn = false;
    int    ltcOutFormat = 1;                  // see LtcGenerator::formatInfo (1 = 25 fps)
    int    ltcOutStartMode = 0;               // 0 = starts at the time of day on this computer's clock, 1 = starts at ltcOutStartSeconds
    double ltcOutStartSeconds = 0.0;
    // ---- spot-mic alignment (the Align window): every piece keeps its own set (a take window's id -> its set; an edit measured in post: the edit's id)
    std::map<juce::String, AlignSet> alignSets;
    juce::String alignActive;                 // whose set the mixers use now: the piece being recorded, played or worked on (set by the program)
    bool         alignEnabled = true;         // the A / B switch in the Align window: off, no track is delayed
    /** The set an edit plays with: its own (measured on the edit), else that of the take window most of its audio comes from. A take window: its own. */
    juce::String alignOwnerFor (bool isEdit, const juce::Uuid& id) const;
    AlignSet* alignSetOf (const juce::String& owner) { auto it = alignSets.find (owner); return it != alignSets.end() ? &it->second : nullptr; }
    const AlignSet* alignSetOf (const juce::String& owner) const { auto it = alignSets.find (owner); return it != alignSets.end() ? &it->second : nullptr; }
    /** How much later this track is heard now (samples at this rate): the active set's delay, if it is on and alignment is on. */
    double alignSamplesFor (const TrackDef& t, double sampleRate) const
    {
        if (! alignEnabled) return 0.0;
        const auto* set = alignSetOf (alignActive);
        const auto* at = set != nullptr ? set->find (t.id) : nullptr;
        return at != nullptr && at->on && at->ms > 0.0 ? at->ms * sampleRate / 1000.0 : 0.0;
    }
    /** The delays a piece plays with, as text (for knowing when a render is out of date). */
    juce::String alignSignature (const juce::String& owner) const
    {
        juce::String s = alignEnabled ? "on" : "off";
        if (const auto* set = alignSetOf (owner)) for (auto& kv : set->tracks) if (kv.second.on && kv.second.ms > 0.0) s << ";" << kv.first << "=" << juce::String (kv.second.ms, 3);
        return s;
    }
    PitchPipeDef pitchPipe;                   // the pitch pipe's settings (not an undo step)
    ClickDef     click;                       // the click's settings (not an undo step)
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
    /** The room tone of the recording, learned once (the Room tone button of an edit window: from a take marked as the room tone, or from marks 1 - 2
        of the edit) and kept with the project: every track's noise profile, per channel of its file and for the mono mix the pictures show. The ReTouch
        and Noise mode windows denoise with it. */
    struct RoomToneTrack { juce::Uuid trackId; juce::String trackName; std::vector<denoise::Profile> perChannel; denoise::Profile mono; };
    struct RoomTone
    {
        juce::String source;                                    // "00:34 - 00:51 of take 8 of New World" (for the windows' labels)
        std::vector<RoomToneTrack> tracks;
        bool valid() const { return ! tracks.empty(); }
        const RoomToneTrack* forTrack (const juce::Uuid& id) const { for (auto& t : tracks) if (t.trackId == id) return &t; return nullptr; }
    };
    RoomTone roomTone;
    /** The consonants kept for moving and re-use (3.6.0, the ReTouch window's Consonants tab): their audio is in consonantsFolder(), one float WAV per track.
        Saved with the project; not an undo step (Ctrl+Z never takes one off the list). */
    std::vector<ConsonantDef> consonants;
    const ConsonantDef* findConsonant (const juce::Uuid& id) const { for (auto& c : consonants) if (c.id == id) return &c; return nullptr; }
    juce::File consonantsFolder() const { return projectFolder().getChildFile ("Consonants"); }
    /** The project's sample rate (3.9.0): chosen when the project is made (44.1, 48, 96 or 192 kHz: see kProjectRates), saved with it. The engine
        runs at it (behind the live converter when the audio device runs at another) and every take is recorded at it - Record is refused while the
        engine is at another rate. 0 = not chosen: a project from before 3.9.0, whose rate audioRate() then takes from its takes (and saves); one with no
        takes is asked for it. Changed only while the project has no takes. Saved in the project file; not an undo step. */
    double sampleRate = 0.0;
    static constexpr double kProjectRates[4] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    /** At least one take in any take window (the rate can then no longer be changed). */
    bool hasTakes() const { for (auto& w : takeWindows) if (! w->groups.empty()) return true; return false; }
    static juce::var roomToneToVar (const RoomTone&);
    static RoomTone roomToneFromVar (const juce::var&);
    std::vector<std::unique_ptr<MixerState>>   retiredMixers;   // removed mixers (and those of a project that was open before) stay alive until the audio thread has certainly left them: see freeRetiredMixers
    void retireAllMixers();
    /** Frees the retired mixers and their plug-ins. Only once the engine has swapped to a plan that no longer knows them (AudioEngine::rebuildPlan
        with the old plan freed) and their plug-ins' windows are closed: the program calls this (AppContext), not the project. */
    void freeRetiredMixers (size_t count) { retiredMixers.erase (retiredMixers.begin(), retiredMixers.begin() + (std::ptrdiff_t) juce::jmin (count, retiredMixers.size())); }
    juce::String name { "Untitled" };

    /** Makes the input / output lists match the audio driver (names are kept for channels that already existed). */
    void syncWithDevice (const juce::StringArray& inputNames, const juce::StringArray& outputNames);
    /** The name to show for input i: your name for it, falling back to the driver's. */
    juce::String inputLabel (int i) const;
    /** The device behind a driver input and its channel on it, "THAP-12", "Anubis-2": from the preamp driver when the device is connected, else from
        the Preamps setup (ANEMAN's patching, or the device's first input and the ones that follow it up to the next device). "" when no device covers it. */
    juce::String inputDeviceLabel (int i) const;
    juce::String inputTag (int i) const;                       // "36-BHAP-12": the ASIO number, the device, the channel in the device (two digits each); else "36-" and the driver's name
    /** The short name of a driver input for the strips and the lists: the device label, or else the driver's own name without the part every input's
        name begins with ("MAD Input 12" -> "Input 12" when they all start "MAD "). */
    juce::String inputShortName (int i) const;
    juce::String driverNamePrefix() const;                       // (remembered for the driver names it was worked out for; message thread)
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
    // --- VCAs (message thread)
    std::vector<VcaDef> vcas;                   // sorted by number
    /** Adds a VCA with the lowest free number (nullptr when all 16 are in use). */
    VcaDef* addVca (const juce::String& vcaName = {}, bool inProcessingAndEditMixers = false);
    void showVca (MixerState& mixer, const VcaDef& vca, bool shown);
    void removeVca (const juce::Uuid& id);
    VcaDef* findVca (const juce::Uuid&);
    const VcaDef* findVca (const juce::Uuid&) const;
    VcaDef* vcaByNumber (int number);
    /** Puts the audio track or Int Bus 'node' under (or takes it from) the VCA of 'mixer'. */
    void setVcaMember (MixerState& mixer, const VcaDef& vca, const juce::Uuid& node, bool on);
    bool isVcaMember (const MixerState& mixer, const VcaDef& vca, const juce::Uuid& node) const;
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
    /** A project file's backup put back on purpose: its text with the lists of take window, edit and mixer files (and the windows / mixers kept in
        the file itself) taken from the project file there now ('currentText'), because those files are not put back - the windows and mixers
        open as they are now. Returns 'backupText' as it is when the current text cannot be read. */
    static juce::String backupWithCurrentWindows (const juce::String& backupText, const juce::String& currentText);
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
    /** The Display Organiser's saved layouts (the project's own; the program's built-in ones are not kept here). Saved with the project, not an undo step. */
    std::vector<LayoutDef> layouts;
    /** Track ids the edit's audio uses that are not in the project any more (so no mixer strip exists for them): id -> (name, channels). */
    std::vector<std::pair<juce::Uuid, std::pair<juce::String, int>>> missingTracksOf (const EditDef&) const;
    /** Puts a deleted track back under its old id (the mixers get a strip for it). */
    void restoreTrack (const juce::Uuid& id, const juce::String& trackName, int channels);
    /** Gives the edit its own track list (the current project order) if it has none yet. */
    void materialiseEditTracks (EditDef&) const;
    TakeWindowDef* findTakeWindow (const juce::Uuid&);
    juce::Array<int> inputsOfTrack (const TrackDef&) const;

    /** Moves clips of a take window from one track to another, the takes staying where they are in time (an import that put them on new tracks of
        their own): the clip each of 'takes' has on 'fromTrack' goes onto 'toTrack' (a take with none there is passed over). The pieces of edits made
        from those takes go with them (an edit with a track list of its own gets 'toTrack' beside 'fromTrack'), and 'toTrack' is shown in the window
        again if it had been removed from it. Nothing moves if one of the takes already has audio on 'toTrack', if the clips' channels are not
        'toTrack's, or if either is the LTC track: canMoveTakeClips says why ("" = it can). Returns how many clips moved; 'editPieces' (optional)
        how many pieces of edits went with them. One change, so one undo step. */
    int moveTakeClips (const juce::Uuid& windowId, const juce::Uuid& fromTrack, const juce::Uuid& toTrack, const std::vector<juce::Uuid>& takes,
                       juce::String& why, int* editPieces = nullptr);
    juce::String canMoveTakeClips (const juce::Uuid& windowId, const juce::Uuid& fromTrack, const juce::Uuid& toTrack, const std::vector<juce::Uuid>& takes);
    /** The track whose name this track's name ends with ("VivienneSPOT1" ends with "SPOT1": an import that put the same word in front of every
        name), the longest such name; capitals and spaces at the ends do not count; number words at the end are taken off if that finds nothing
        (trackNamedIn). Not the LTC track. Null if there is none. */
    const TrackDef* trackNamedAtEndOf (const juce::Uuid& trackId) const;
    /** The track a name refers to: the longest track name it ends with (as trackNamedAtEndOf), else the same with the number words at its end taken
        off one at a time - "Rorate Caeli SPOT4 19 18" -> "SPOT4", "Rorate CaeliROOM 414 01" -> "ROOM 414" (an import that added the piece and take
        numbers). Never the LTC track nor 'except'; with 'channels' > 0 only a track with that many channels. Null if there is none. */
    const TrackDef* trackNamedIn (const juce::String& name, int channels = 0, const TrackDef* except = nullptr) const;
    /** The name without the number words at its end ("Rorate Caeli AB 20 19" -> "Rorate Caeli AB"; "SPOT4" stays as it is). */
    static juce::String withoutEndNumbers (const juce::String& name);
    /** Is there audio on this track anywhere in the project: a clip in a take window, or a piece (or overdub) of an edit? */
    bool trackHasAudio (const juce::Uuid& trackId) const;
    /** Deletes these tracks from the project at once (one structure change), passing over the LTC track and any track with audio on it somewhere
        (trackHasAudio), so nothing is lost. The audio files on the disk are not touched. Returns how many tracks went. */
    int removeEmptyTracks (const std::vector<juce::Uuid>& ids);

    /** Which parts of a mixer a copy takes: faders (levels, mutes, solos, output buttons), sends, pans, plug-in inserts. */
    using MixParts = td::MixParts;
    /** The balance of a mixer (levels, pans, mutes, solos, sends, effects returns - and optionally the plug-in inserts) as data. */
    juce::var mixSnapshot (const MixerState&, bool withInserts) const;
    /** Puts such a balance onto a mixer (its master level and output pair are left alone). */
    void applyMix (MixerState& to, const juce::var& snapshot, double sampleRate, int maxBlock, const MixParts& parts = {});

    /** Makes sure every mixer has a strip per track and a return per effects channel. */
    void syncMixers();
private:
    MixerState& makeEditMixer (const EditDef&);
    juce::String findDriverNamePrefix() const;
    mutable juce::String prefixCache; mutable juce::uint64 prefixKey = 0; mutable bool prefixKnown = false;     // (driverNamePrefix: the last answer, and for which names)
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
    bool filesMoved = false;                         // set while reading: a recording was found by its path inside the project folder, not where it was saved (the project was moved or copied: every take is checked again)
    juce::File loadBase() const { return readingFrom != juce::File() ? readingFrom : projectFile.getParentDirectory(); }
    /** The project's own folder (where the .fermata file is) and its three working folders:
          Recorded Media            every recording made by the program
          Bounced Media             what is bounced from the Take / Edit windows
          Bounced Media/Mastered Audio   what is bounced from the Mastering window
          Analysis                  the takes' fingerprints for "the same place in another take" (3.4.0; made again if missing)
          Waveforms                 the coloured waveforms' cache (made again if missing) */
    juce::File projectFolder() const;
    juce::File audioFolder() const;                  // Recorded Media
    /** The saved copy of the comments imported on an edit: <project folder>/comments/<edit name> [id]/ holds the files that were imported and comments.json (the notes, ready to reload).
        A new import REPLACES the copy. Folders of edits that are gone are left alone. */
    juce::File commentsFolderFor (const EditDef& e) const;
    void saveCommentsCopy (const EditDef& e, const std::vector<juce::File>& importedFiles) const;
    void restoreSavedComments();                    // after a project is opened: an edit with no comments of its own gets its saved copy back
    juce::File processingFolder() const;            // Processing Media: audio sent out with "Export for Processing"
    juce::File bouncedFolder() const;                // Bounced Media
    juce::File masteredFolder() const;               // Bounced Media/Mastered Audio
    void createFolders() const;
    /** After a crash or power cut: recordings that were still being made have a header that says 'empty'. Puts the headers right and the takes' lengths,
        so everything that was recorded up to the moment it stopped is there. Returns a line for each take that was mended.
        Only takes that can have been cut off are opened (saved while still recording, or a file shorter than the take): the others are passed by
        their file size alone, so a project with thousands of recordings opens quickly. */
    juce::StringArray repairInterruptedRecordings();
    juce::File takeFolder (const TakeWindowDef&) const;
    /** quick: an automatic save (plug-ins hand over their last state unless it may have changed: see InsertProcessor::getStateBase64Quick).
        Writes everything before it returns (prepareSave + writeSave on this thread). */
    bool save (juce::String& error, bool quick = false);

    /** A window file that a save no longer needs: renamed (the new file has the same content: delete) or the window is gone (keep it in _removed). */
    struct OldWindowFile { juce::File file; bool renamed; };
    /** A save in two halves (2.102): the snapshot, made on the message thread by prepareSave (the project file's var, and plain copies of the windows
        whose files must be written again), and the writing, done by writeSave on any thread - the text, the files, the flush to the disk, tidying
        old window files away. The program writes its automatic saves on a thread of its own (BackgroundSaver), so they no longer hold it up. */
    struct SaveJob
    {
        juce::File projectFile, folder;                       // the project file and its folder (audio paths inside it are saved relative to it too)
        juce::var root;                                       // the project file
        juce::var tracks; std::set<juce::String> trackIds;    // root's tracks (each window file lists the ones it uses)
        struct Window { juce::File file; juce::String id; std::shared_ptr<const TakeWindowDef> take; std::shared_ptr<const EditDef> edit; juce::var mixer; juce::var mixerFile; };
        std::vector<Window> windows;                          // the window files to write (a take window, an edit with its mixer, or a mixer file)
        std::vector<OldWindowFile> tidy;                      // window files to tidy away once the project file is written
        double prepareMs = 0.0, writeMs = 0.0;
    };
    /** Message thread: the snapshot of a save (nullptr without a project file). The project counts as saved from now on (dirty is cleared). */
    std::unique_ptr<SaveJob> prepareSave (bool quick);
    /** Any thread: writes a prepared save. 'windowsFailed' (optional): it stopped at a window file (the project file was not written). */
    static bool writeSave (SaveJob&, juce::String& error, bool* windowsFailed = nullptr);
    /** Message thread: a save that could not be written - the project is unsaved again, its windows' files are written again next time, the old files
        it was to tidy wait for the next save. */
    void saveFailed (const SaveJob&);
    /** The program sets this: waits for the saves being written on the saver's thread (and takes in their results) before anything else writes,
        reads or moves the project's files (save, load, deleting a window's file). */
    std::function<void()> settleBackgroundSaves;
    bool saveAs (const juce::File&, juce::String& error);
    bool load (const juce::File&, juce::String& error);

    // ---- window files: every Edit is saved in its own .fmedit file (Edits folder) and every Take window in its own .fmtake file (Take Windows folder).
    //      The project file only lists them. The files are what is read when the project opens, and they are rewritten whenever the project is saved.
    struct WindowFile { juce::String rel; juce::String hash; };
    std::map<juce::String, WindowFile> windowFiles;     // window id -> its file (path from the project folder)
    std::vector<juce::var> missingWindowRefs;            // listed by the project file but not readable when it was opened: kept in the list so nothing is forgotten
    std::function<void()> onNewWindow;                   // the program sets this: called when an edit, take window or mixer is made, so its file is written at once
    juce::StringArray loadNotes;                         // things to tell the person after the project was opened
    /** The program sets this while a project opens: called now and then with what is being read and how far it has got (0..1, < 0 = not known),
        so a "please wait" window can show it. Nothing else depends on it. */
    std::function<void (const juce::String& what, double progress)> loadProgress;
    void reportLoad (const juce::String& what, double progress) const { if (loadProgress) loadProgress (what, progress); }
    /** Reads a .fmedit / .fmtake file (from another project, or a copy) and adds it. Same-named windows get a number, ids that clash are renewed, tracks are matched by name.
        'missingFiles' (optional) is told how many of the audio files the imported window uses cannot be found. */
    bool importWindowFile (const juce::File&, juce::String& message, juce::Uuid* newId = nullptr, int* missingFiles = nullptr);
    /** Reads a .fmmix mixer file and adds it as a new mixer (the strips are matched with this project's tracks and buses by name). */
    bool importMixerFile (const juce::File&, juce::String& message);
    /** How many audio files used by the edits and take windows cannot be found (each file counted once, however many places use it). With 'quick',
        the files of takes found whole at the last open (verified) are not asked about - unless the project has moved since - so a big project is not
        slowed down: a file that went missing since then is found out when it is played, or by missingAudioCount() in full. */
    int missingAudioCount (bool quick = false) const;
    /** The project's rate: the one chosen for it (sampleRate, 3.9.0), or for a project from before that, the rate most of its takes and edits are at (a
        tie: the first found); 0 when neither is known (3.8.0: the engine runs at this rate, behind a live converter when the audio device runs at another
        - see AudioEngine::setProjectRate). Quick: no disk. */
    double audioRate() const;
    /** The missing audio files, each once, with where it is used ("Takes - Mass in B minor, 003 - Kyrie, ORTF L"). With 'quick', as missingAudioCount (true):
        the files of verified takes are not asked about (opening a big project with one file missing used to ask about every file of it again). */
    juce::StringArray missingAudioList (int atMost = 40, bool quick = false) const;
    /** What "Find missing audio files" did. */
    struct RelinkReport
    {
        int byName = 0, renamed = 0, unsure = 0, stillMissing = 0;     // files found by name; found renamed (by their length, rate, depth, channels ...); fitting several files; not found
        juce::StringArray found;                                        // "old name  ->  new name  (how)"
        juce::StringArray unsureLines;                                  // "old name: fits 3 files (a.wav, b.wav, c.wav)"
        juce::StringArray missing;                                      // the names still missing
        juce::String summary() const;
    };
    /** Looks for the missing audio files inside a folder (and its sub-folders): first by name (a same-named file with another length, rate or channel count
        is not it), then - for a file that was renamed - by what the project knows about it (length in samples, sample rate, channels, and bit depth for
        Fermata's own recordings), taking it only when exactly one file fits (the files of one take are told apart by the names Fermata wrote in their
        headers, their recording time, or the track name in the file name). A file the project uses as it is is never taken for a missing one.
        'progress' (optional) is told what is being looked at. Every place that used a file is pointed at the file found. The undo history is cleared
        (the relink is not an undo step). */
    RelinkReport relinkAudio (const juce::File& searchFolder, const std::function<void (const juce::String&, double)>& progress = {});
    /** Removes the edit / take window from the project AND moves its file to the recycle bin. The audio recordings are not touched. The caller closes its windows first. */
    bool deleteEditAndFile (const juce::Uuid&);
    bool deleteTakeWindowAndFile (const juce::Uuid&);
private:
    /** Decides which window files a save writes (copies of those windows go into the job) and takes the windows out of root, leaving the list of
        files. 'next' is what windowFiles becomes and 'old' the files to tidy away once the project file is written, so a stop between the two leaves
        a folder that still opens. */
    void planWindowFiles (juce::var& root, SaveJob& job, std::map<juce::String, WindowFile>& next, std::vector<OldWindowFile>& old);
    static void tidyWindowFiles (std::vector<OldWindowFile>&, const juce::File& folder);     // deletes renamed windows' old files, keeps gone windows' in _removed
    std::vector<OldWindowFile> oldWindowFiles;             // waiting to be tidied away by the next save that gets as far as writing the project file
    juce::String takeFileHash (const TakeWindowDef&, const juce::String& tracksHash) const;              // what WindowFile::hash holds
    juce::String editFileHash (const EditDef&, const juce::String& tracksHash, const juce::var& rootMixers, juce::var* mixerOut) const;  // (the edit's mixer var, from root, comes back too)
    void refreshWindowFileHashes();                        // after load: the hashes of what was read, so the first save writes only what changes
    void mergeWindowFiles (juce::var& root);
    void forgetWindowFile (const juce::String& id);
public:
    void createDefaultDesign();
    /** What a NEW project starts with besides that (a new project or the blank one at start-up, not a project being read): one Ext Bus, "Main",
        ticked as the processing mixer's main output and sent to the driver's outputs 1-2. Returns it. */
    BusDef& addDefaultMainBus();

    /** A private, independent copy of the whole project (plug-ins re-created from their state) for offline rendering. */
    std::unique_ptr<Project> cloneForRender (double sampleRate, int maxBlock) const;
    /** The plug-ins that are running (not bypassed) in this project's mixer 'mixerId' but could not be started in 'copy' (a render copy made by
        cloneForRender): "<plug-in> on <strip>", one per slot. Empty: the copy has every one of them, and a render through it sounds like the mix. */
    juce::StringArray pluginsMissingFrom (const Project& copy, const juce::Uuid& mixerId) const;
    double restoreSampleRate = 48000.0; int restoreBlock = 512;    // used when plug-ins are re-created by fromVar()

    /** The whole project as saved. withWindows false: without the take windows and edits (a save adds them as their own files: see planWindowFiles). */
    juce::var toVar (bool withWindows = true) const;
    void addWindowsToVar (juce::var& root) const;        // the "takeWindows" and "edits" arrays
    juce::var mixerToVar (const MixerState&) const;      // one mixer, as saved (levels, sends, plug-in states)
    juce::var editToVar (const EditDef&) const;          // one edit, as saved
    juce::var takeWindowToVar (const TakeWindowDef&) const;   // one take window, as saved
    static juce::var editToVarIn (const EditDef&, const juce::File& base);              // the same, with audio paths relative to 'base' (any thread)
    static juce::var takeWindowToVarIn (const TakeWindowDef&, const juce::File& base);
    bool fromVar (const juce::var&);

    // ---- undo: the last 30 states of the takes' details (bars, duds, names, positions, removed takes), the edits and the mastering ----
    //      Kept per window: every take window, every edit and the mastering list has its own snapshot (a hash of what the history looks at, and
    //      a plain copy of the window). After a change only the windows whose hash moved are copied, so the cost of a change is the size of the
    //      window it was made in, not of the project. An undo step holds the previous copies of the windows that changed together. (2.102: copies,
    //      not text - turning a big edit into text twice took a second, half a second after every change.)
    static constexpr int kUndoLevels = 30;
    enum class UndoKind { takeWindow, edit, mastering };
    struct UndoPiece
    {
        UndoKind kind; juce::Uuid id; juce::String text;                      // text: the mastering list (and a window whose copy is not there)
        std::shared_ptr<const TakeWindowDef> take; std::shared_ptr<const EditDef> edit;     // the window as it was
    };
    struct UndoStep { std::vector<UndoPiece> pieces; };
    /** A window's state as of the last quiet moment: its hash and a plain copy of the window (the mastering list: its text). */
    struct UndoSnap
    {
        juce::uint64 hash = 0; juce::String text;
        std::shared_ptr<TakeWindowDef> takeCopy; std::shared_ptr<EditDef> editCopy;
    };
    static UndoSnap snapOf (const TakeWindowDef&);       // the hash and a copy (about a millisecond for a big window)
    static UndoSnap snapOf (const EditDef&);
    static UndoPiece pieceOf (UndoKind, const juce::Uuid&, const UndoSnap&);
    UndoSnap masteringSnap() const;                      // the mastering list's hash and text
    std::vector<UndoStep> undoStack, redoStack;          // older / newer states
    std::map<juce::Uuid, UndoSnap> undoTakes, undoEdits; // the state of each window as of the last quiet moment
    UndoSnap undoMastering;
    bool undoPending = false; juce::uint32 undoDirtyAt = 0;
    /** What the history looks at, as a hash (cheap: no text is made) and as text. The hashes cover exactly the fields the texts hold: see the
        "undo hash" test, which changes every one of them. View state (playheads, cursors, marks, markers, hidden tracks) is in neither. */
    static juce::uint64 undoHash (const TakeWindowDef&);
    static juce::uint64 undoHash (const EditDef&);
    static juce::uint64 undoHash (const MasteringDef&);
    juce::String undoText (const TakeWindowDef&) const;
    juce::String undoText (const EditDef&) const;
    juce::String undoText (const MasteringDef&) const;
    /** The undo hash plus the view state that is saved with the window: what decides whether a window's file is written again (see planWindowFiles). */
    static juce::uint64 saveHash (const TakeWindowDef&);
    static juce::uint64 saveHash (const EditDef&);
    void undoReset();
    /** Call a few times a second: once changes have stopped for half a second the previous state is kept as an undo level (if anything undoable changed). */
    void undoTick (bool recording, bool force = false);
    /** The take window is snapshotted as it is now, without an undo step: for a change the person did not make and cannot undo - a take starting
        (AudioEngine::startRecording, which first ticks the pending changes into a step of their own). The next tick then sees no change in it (3.1.4). */
    void undoAbsorb (const juce::Uuid& takeWindowId);
    bool undo (const juce::Uuid& liveTake = {});         // false if there is nothing to undo; a take being recorded is never touched
    bool redo (const juce::Uuid& liveTake = {});
    int undoDepth() const { return (int) undoStack.size(); }
    int redoDepth() const { return (int) redoStack.size(); }

    /** Anything edited: take moved/renamed, preamp changed, recording finished... (listen on the Project itself) */
    /** Re-places every automation point on the audio it belongs to (the edits' pieces may have moved). Cheap; safe to call at any time. */
    void resolveAllAutomation() { for (auto& e : edits) if (e != nullptr && ! e->lanes.empty()) e->resolveAutomation(); }
    void refreshNormalisedGains() { for (auto& e : edits) if (e != nullptr) e->refreshNormalised(); }
    void commitAllJoins()   { for (auto& e : edits) if (e != nullptr) e->commitAllJoins(); }          // the pieces' edges sit at the edit points (see EditDef::commitJoin)
    /** Like changed(), for edits that come in quick succession (a slider being dragged, a nudge button clicked repeatedly): the data is brought up to date at once, but the
        windows (which redraw everything) are told at most about every 100 ms, and always once more after the last change, so the display never stays out of date. */
    void changedLive()
    {
        commitAllJoins(); resolveAllAutomation(); refreshNormalisedGains(); dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter();
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastLiveSend >= 100u) { lastLiveSend = now; trail.stopTimer(); sendChangeMessage(); }
        else if (! trail.isTimerRunning()) trail.startTimer (100);
    }
    void changed()          { commitAllJoins(); resolveAllAutomation(); refreshNormalisedGains(); dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); sendChangeMessage(); }
    /** Tracks / effects channels / mixers / inputs / outputs / names changed: UI must rebuild, engine must re-plan. */
    void structureChanged() { resolveAllAutomation(); dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); structure.sendChangeMessage(); sendChangeMessage(); }
    void markDirty() noexcept { dirty = true; }          // value tweaks (faders etc.) that need saving but no UI refresh
    /** A change that needs saving and is an undo step (the mastering list, which has its own hash), but tells no window: as markDirty, plus the undo
        tick (3.1.4: the mastering window's edits only called markDirty, so Ctrl+Z straight after one undid the step before it instead). */
    void markDirtyForUndo() noexcept { dirty = true; undoPending = true; undoDirtyAt = juce::Time::getMillisecondCounter(); }
    struct Trail : juce::Timer { Project& p; explicit Trail (Project& o) : p (o) {} void timerCallback() override { stopTimer(); p.lastLiveSend = juce::Time::getMillisecondCounter(); p.sendChangeMessage(); } };
    Trail trail { *this };
    juce::uint32 lastLiveSend = 0;
    juce::ChangeBroadcaster structure;
    std::atomic<bool> dirty { false };
};
} // namespace td

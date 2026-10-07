#pragma once
#include "../core/TalkbackKey.h"
#include "../core/PreampMemory.h"
#include "Uikit.h"
#include "PluginHost.h"
#include "../core/SessionClock.h"

namespace td
{
class TakeDisplayHub;
class TalkbackOverlay;

/** What the Bounce Out window is asked to bounce. */
struct BounceContext
{
    enum class Kind { Edit, Take } kind = Kind::Edit;
    juce::Uuid id;                          // the edit, or the take window
    std::vector<juce::Uuid> selected;       // Edit: the pieces selected in the edit window
    juce::String defaultName;
};

/** Windows with level meters get told (about 30 times a second) when fresh peaks are available in AppContext::inPeaks / outPeaks. */
struct PeakListener { virtual ~PeakListener() = default; virtual void peaksUpdated() = 0; };

/** Everything the windows share. Owned by the application object. */
struct AppContext : private juce::Timer, private juce::ChangeListener
{
    AppContext();
    ~AppContext() override;

    /** Peak of every driver input (before any fader) and driver output (after the faders) since the previous update. Read once, then shared by every meter. */
    std::vector<float> inPeaks, outPeaks;
    juce::ListenerList<PeakListener> peakListeners;

    /** How much the drawn waveforms are enlarged (the . and , keys in the take, edit and trim windows). Display only: the audio is never touched. */
    float waveZoom = 1.0f;
    void changeWaveZoom (float factor) { waveZoom = juce::jlimit (0.25f, 40.0f, waveZoom * factor); project.sendChangeMessage(); }

    Project project;
    juce::AudioDeviceManager devices;
    AudioEngine engine { project };
    PluginHost plugins;
    juce::ApplicationProperties props;

    // windows (set by the application)
    std::function<void (const juce::Uuid&)> showTakeWindow, showMixer, showEdit;
    /** Makes sure the edit's window exists WITHOUT taking the focus: a new one opens behind 'stayInFront' (a window of the caller); an open one is left alone. */
    std::function<void (const juce::Uuid& editId, juce::Component* stayInFront)> showEditBehind;
    std::function<void (const juce::Uuid& editId, const juce::Uuid& regionId, bool atEnd)> showTrim;   // trim the join BEFORE this region (atEnd: the END of the edit; the region is then the last one)
    std::function<void (const juce::Uuid& editId)> closeTrim;                              // closes the trim window of that edit
    std::function<void()> showDesign, showAudioSettings, refreshTitles, showBridge, showMedia;
    std::function<void (const juce::Uuid&)> showScope;
    std::function<void()> showOrganiser;
    std::function<void()> showTakeDisplay;
    std::function<void()> showMastering;                // the Mastering window (Virtual Master export, DDP builder)

    // ---- the Take Display (take number, time of day and time left, on other screens) ----
    struct TakeDisplaySettings
    {
        juce::String line;                              // the project: usually an artist name or an album title
        int logoMode = 0;                               // 0 = the Fermata mark, 1 = no logo, 2 = an image file
        juce::String logoPath;
        int timerMode = 0;                              // 0 = the session ends at a time of day, 1 = the session has a length
        juce::String endAtText = "17:00", lengthText = "03:00";
        juce::int64 endMs = 0;                          // when the session ends (ms since 1970); 0 = no countdown
    } takeDisplay;
    void saveTakeDisplay();
    juce::String setSessionEndAt (const juce::String& hhmm);      // returns an error message or ""
    juce::String setSessionLength (const juce::String& hhmm);
    void clearSessionTimer();
    /** Seconds left (negative when over time). False if no countdown is set. */
    bool sessionSecondsLeft (juce::int64& seconds) const;
    std::unique_ptr<TakeDisplayHub> takeDisplayHub;

    // ---- preamp memory: the full preamp state is written next to the project every second; opening the project later offers to restore it ----
    struct PreampMemoryTicker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } preampTicker;
    std::shared_ptr<bool> anemanAlive = std::make_shared<bool> (true);
    bool preampChecked = true, preampAsking = false, haveWrittenPreamps = false;
    int preampWaitTicks = 0;
    PreampMemory lastWrittenPreamps;
    juce::File preampMemoryFile() const;
    void preampMemoryTick();
    void askRestorePreamps (const PreampMemory& stored, const std::vector<int>& differing);

    // ---- talkback: numpad + opens the CR mic (tap = latch, hold = momentary), numpad - switches playback to the TB speakers on / off ----
    void setCrMicInput (int inputIndex, bool on);        // adds or removes one input from the CR mic (any number can be on)
    void setTbPair (int firstOutput, bool on);           // a talkback pair starting at this output (it and the next; mono on the last output); any number of pairs
    bool talkbackReady() const { return ! project.crMicInputs.empty() && ! project.tbOutputs.empty(); }
    bool crMicOpen() const { return crWanted && talkbackReady(); }              // what the screens show (only when it really is routed)
    bool tbPlaybackOn() const { return tbWanted && ! project.tbOutputs.empty(); }
    std::function<juce::Rectangle<int>()> mainWindowArea;                      // set by the application: where the main window is
    void applyTalkback();
    TalkbackKey crKey;
    // ---- remote control (Stream Deck): the same talkback logic with a 1 second hold, and the transport / mixer actions ----
    TalkbackKey remoteCrKey;
    bool remoteCrDown = false;
    void remoteTalk (bool down);                         // the Talk key went down / up
    void remotePlayback() { if (! project.tbOutputs.empty()) tbWanted = ! tbWanted; }
    void remoteRecord();                                 // start (green) / stop (red) a take in the take window used last
    void remotePlayPause();                              // plays in the window you used last (take or edit), or stops
    void remoteMixer (int index) { if (toggleMixerAt) toggleMixerAt (index); }
    std::function<void (int)> toggleMixerAt;             // set by the application: opens / closes the mixer with this index (0 = the processing mixer)
    std::function<bool (int)> mixerOpenAt;
    /** The take or edit window you last worked in registers how its Play button behaves; the remote's play / pause uses it. */
    void setTransportHandler (const void* owner, std::function<void()> fn) { transportOwner = owner; transportHandler = std::move (fn); }
    void clearTransportHandler (const void* owner) { if (transportOwner == owner) { transportOwner = nullptr; transportHandler = nullptr; } }
    std::function<void()> transportHandler; const void* transportOwner = nullptr;
    bool crWanted = false, tbWanted = false, minusWasDown = false;
    std::unique_ptr<TalkbackOverlay> talkbackOverlay;
    struct TalkbackPoller : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } talkbackPoller;
    /** Display Organiser: the open windows (key, title), and putting windows into parts of the screen (key, fraction of the display). Set by Main.cpp. */
    std::function<std::vector<std::pair<juce::String, juce::String>>()> listWindows;
    std::function<void (const std::vector<std::pair<juce::String, juce::Rectangle<float>>>&)> placeWindows;
    /** Highlights this file in the Media window (if it is open; otherwise it is remembered for when it opens). */
    juce::File mediaSelection;
    std::function<void (const juce::File&)> mediaReveal;
    void revealInMedia (const juce::File& f) { mediaSelection = f; if (mediaReveal) mediaReveal (f); }
    std::function<void (const BounceContext&)> showBounce;

    /** The last pitch correction / repair, so it can be undone (the new audio files stay on the disk; the originals were never touched). */
    int undoHold = 0;      // > 0 while a dialog is trying something out (pitch curve audition): the undo history is not written to meanwhile
    std::function<void()> fixUndo;
    juce::String fixUndoLabel;

    /** Session mode: every patched input is kept for the last 6 seconds, so a take starts 6 s before Record was pressed. Remembered for next time. */
    void setSessionMode (bool on);
    bool sessionMode() const { return engine.isSessionMode(); }

    /** Start recording into the given take window, or stop if already recording. */
    void toggleRecord (const juce::Uuid& takeWindowId);
    /** Deletes a cue mixer (never the processing mixer, which is the first one). */
    bool deleteMixer (const juce::Uuid& mixerId);
    juce::Uuid recordTarget;                         // take window used by the main Record button (the one recorded into last)
    /** For the "Next Take / This Take" box: true while recording (number = the take being recorded), otherwise number = the next take of the most recently used take window. */
    bool takeBoxInfo (int& number);

    // ---- playback (everything plays through the mixers) ----
    struct PlayInfo
    {
        enum class Kind { None, Take, Edit, Other } kind = Kind::None;
        juce::Uuid windowId, id;          // take window + take, or the edit
        double startSeconds = 0.0;        // where on the take / edit timeline playback began
        bool noFollow = false;            // the playhead stays put when this stops (the trim window's audition)
        double loopLen = 0.0;             // > 0: the range [startSeconds, startSeconds + loopLen) is playing in a loop
        bool windowTimeline = false;      // Take: startSeconds is a time on the take WINDOW's timeline (all its takes play where they stand), not inside one take
    };
    PlayInfo playInfo;
    /** Plays a take (all its tracks) from fromSec to toSec (-1 = to the end), seconds from the start of the take. */
    /** Plays the whole take window from 'fromSec' on its timeline: every take sounds where it stands, the gaps and everything after the last take are silent,
        and it keeps going (about 6 hours) until you stop it. */
    juce::String playTakeWindow (const juce::Uuid& windowId, double fromSec);
    juce::String playTake (const juce::Uuid& windowId, const juce::Uuid& takeId, double fromSec, double toSec = -1.0, bool loop = false);
    /** Plays the edit, crossfades included, from fromSec to toSec (-1 = to the end), on the edit timeline. */
    juce::String playEdit (const juce::Uuid& editId, double fromSec, double toSec = -1.0, bool loop = false);
    /** One clip of a disc audition: the edit and where it starts on the disc's timeline. */
    struct DiscPlayItem { juce::Uuid editId; double startSeconds = 0.0; };
    /** Plays the clips one after the other, each at its place on the disc timeline (the pauses between them are silence), through the mixers with the
        edits' automation, from fromSec to toSec (-1 = to the end of the last clip). 'token' goes into playInfo.id so the caller knows it is its own playback. */
    juce::String playDisc (const std::vector<DiscPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token);
    /** Plays one region's raw files (no fades, no neighbours) on the timeline of the file's own samples:
        the trim window's 'play original A / B'. */
    juce::String playRegionOriginal (const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 fromSample, juce::int64 toSample);
    /** Playhead behaviour (all windows): on = after Stop the playhead stays where playback stopped, so the next Play goes on from there;
        off = it goes back to where playback started. Remembered between runs. */
    bool playheadFollows = true;
    void setPlayheadFollows (bool on);
    void stopPlayback();
    /** Seconds played since the playback began, wrapped to the loop length while a loop plays. */
    double playedSeconds() const;
    /** Cheap notification while dragging in the trim window (marks the project dirty and repaints windows). */
    void markDirtyAndRepaintTrim() { project.markDirty(); project.sendChangeMessage(); }
    bool isPlaying() const { return engine.isPlaying(); }
    /** Time on the timeline of the thing being played (take or edit), or -1 if nothing plays. */
    double playheadSeconds() const;

    /** Sends the EDIT-marked part (keys 1 and 2) of the marked take to the edit. Without 'atEditPlayhead' it goes after the last piece (key 3);
        with it, at the edit window's playhead, cutting whatever is there (key 4). With the window's Overdub box ticked it is laid over the edit
        at the playhead instead and plays together with what is under it. Returns an error or "". */
    juce::String copyMarkedToEdit (const juce::Uuid& takeWindowId, bool atEditPlayhead = false, juce::Uuid* editIdOut = nullptr);
    /** A new, empty edit (not tied to any take window). */
    EditDef& makeEdit (const juce::String& name);

    /** Opens every channel of the audio driver and makes the project's input / output lists match it. */
    void syncWithDevice();
    /** The Audio settings window's Apply button: remember the device setup and re-plan the engine. */
    void applyAudioSettings();

    // ---- engineer audition and copying mixes (the FIRST mixer is the engineer's) ----
    juce::Uuid auditionId;                                         // null = the engineer hears their own mixer
    bool isAuditioning (const juce::Uuid& mixerId) const { return auditionId == mixerId && ! mixerId.isNull(); }
    /** Listen to another mixer on the engineer's outputs until switched off (pass a null id, or the same id again, to stop). */
    void setAudition (const juce::Uuid& mixerId);
    /** Copies the balance of one mixer onto another (toId null = onto every other mixer). Returns a sentence for the status line. */
    juce::String copyMix (const juce::Uuid& fromId, const juce::Uuid& toId, bool withInserts);
    bool canUndoCopyMix() const { return ! mixUndo.empty(); }
    juce::String undoCopyMix();
    MixerState* findMixer (const juce::Uuid&) const;
    /** Sets one send of a track / Int Bus (<= -60 dB = off). Refuses sends that would loop; re-plans the engine when a send is switched on. */
    SendResult setSendLevel (MixerState&, const juce::Uuid& src, const juce::Uuid& dest, float db);
    bool isEngineerMixer (const juce::Uuid& id) const { return ! project.mixers.empty() && project.mixers.front()->id == id; }

    /** The Merging devices whose preamps Fermata controls. Kept with the project and also remembered for new projects. */
    void setPreampDevices (const std::vector<PreampDeviceCfg>&, bool chosenByUser = true);
    /** Asks ANEMAN (in the background) which mic inputs are patched to which ASIO inputs, and follows it if that differs from what is set up. Quietly does nothing if ANEMAN is not there. */
    void refreshPreampsFromAneman();
    void rebuildPreampDriver();
    bool newProject (const juce::File& projectFile);
    bool openProject (const juce::File& projectFile);
    void saveNow();
    void restoreLastProject();
    void shutdown();

private:
    std::vector<std::pair<juce::Uuid, juce::var>> mixUndo;
    struct PeakPoller : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } peakPoller;
    struct UndoTicker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } undoTicker;
public:
    /** Ctrl+Z / Ctrl+Y: the last 30 steps of the takes' details (bars, duds, names, positions, removed takes), the edits and the mastering. Says what happened in the status line. */
    void undoStep();
    void redoStep();
    juce::String notice; juce::uint32 noticeUntil = 0;
    void setNotice (const juce::String& t) { notice = t; noticeUntil = juce::Time::getMillisecondCounter() + 3500; }
private:
    struct PlaybackWatcher : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } watcher;
    void timerCallback() override;                   // autosave
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // re-plan the engine after structural edits
    void afterProjectReplaced();
};
} // namespace td

#pragma once
#include "../core/TalkbackKey.h"
#include "../core/PreampMemory.h"
#include "Uikit.h"
#include "WaveColor.h"
#include "PluginHost.h"
#include "../core/SessionClock.h"
#include "../core/SaveWriter.h"
#include "StallWatch.h"

namespace td
{
class TakeDisplayHub;
class TalkbackOverlay;
class SurfaceController;

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

    /** WaveColour: the waveforms are coloured by what is in them (pitch = hue, tonal = vivid, a thump below 100 Hz = black). Off by default; the rainbow button in the take and edit windows. */
    bool waveColour = false;
    WaveColorCache waveColors;
    void setWaveColour (bool on) { waveColour = on; project.sendChangeMessage(); }
    /** The colours of this file's waveform, or nothing while it is not wanted or not worked out yet (it is being worked out in the background). */
    std::shared_ptr<const WaveColorData> waveColourOf (const juce::File& f) { return waveColour ? waveColors.get (f) : nullptr; }

    Project project;
    juce::AudioDeviceManager devices;
    AudioEngine engine { project };
    PluginHost plugins;
    juce::ApplicationProperties props;

    // windows (set by the application)
    std::function<void (const juce::Uuid&)> showTakeWindow, showMixer, showEdit;
    /** The T / E / M squares of the menu bar: bring the take / edit / mixer window used last ("take", "edit", "mixer") to the front (opening it if need be); and which kind the person is working in now ("" = none). */
    std::function<void (const juce::String& kind)> focusRecent;
    std::function<juce::String()> activeKind;
    /** The light after Take Window / Edit Window / Mixer in the menu bar, for "take", "edit" or "mixer": 0 none open, 1 open, 2 the one in use, 3 minimised. */
    std::function<int (const juce::String& kind)> windowLight;
    /** Makes sure the edit's window exists WITHOUT taking the focus: a new one opens behind 'stayInFront' (a window of the caller); an open one is left alone. */
    std::function<void (const juce::Uuid& editId, juce::Component* stayInFront)> showEditBehind;
    std::function<void (const juce::Uuid& editId, const juce::Uuid& regionId, bool atEnd)> showTrim;   // trim the join BEFORE this region (atEnd: the END of the edit; the region is then the last one)
    std::function<void (const juce::String& key)> closeWindowByKey;                        // closes a window by its key ("edit:<id>", "take:<id>", ...)
    /** Deletes the edit / take window AND its .fmedit / .fmtake file (the caller has already asked the person). Its windows are closed first; recordings are never touched. */
    void deleteWindowAndFile (bool isEdit, const juce::Uuid& id);
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
    /** LTC (timecode as audio): the input it arrives on / the output it is sent on, from the project's settings. */
    void applyLtc (bool restartOutput = false);
    /** Chooses the driver input that carries LTC (-1 = none). The LTC track (recorded with every take) is made if there is none, and gets that input. */
    void setLtcInput (int inputChannel);
    /** The LTC track of the project (nullptr if there is none yet). */
    TrackDef* ltcTrack();
    TalkbackKey crKey;
    // ---- remote control (Stream Deck): the same talkback logic with a 1 second hold, and the transport / mixer actions ----
    TalkbackKey remoteCrKey;
    bool remoteCrDown = false;
    void remoteTalk (bool down);                         // the Talk key went down / up
    void remotePlayback() { if (! project.tbOutputs.empty()) tbWanted = ! tbWanted; }
    void remoteRecord();                                 // start (green) / stop (red) a take in the take window used last
    /** Space and R, watched straight from the keyboard 60 times a second (like the talkback keys) so that they work from every window of the program,
        whichever window or button has the keyboard: Space stops the recording, R starts one (in the take window used last). Not while a text box is
        being typed in, not with Shift / Ctrl / Alt held, not in a window whose R has its own job (the edit window, the process windows), and only
        while Fermata is the program in front. */
    void pollTransportKeys();
    bool spaceWasDown = false, rWasDown = false;
    bool spaceHeldAfterStop = false;                     // the Space that stopped a recording is still held: no window acts on it
    static bool typingInTextBox();
    static bool rKeyHasOwnJobHere();
    void remotePlayPause();                              // plays in the window you used last (take or edit), or stops
    void remoteMixer (int index) { if (toggleMixerAt) toggleMixerAt (index); }
    std::function<void (int)> toggleMixerAt;             // set by the application: opens / closes the mixer with this index (0 = the processing mixer)
    std::function<bool (int)> mixerOpenAt;
    // ---- the Stream Deck + XL (see core/RemoteDesk.h) ----
    /** Windows by name: "bridge", "pitchpipe", "click", "take", and (2.97) "editwin", "takedisplay", "organiser", "media", "mastering". State: 0 not open, 1 open and shown,
        3 open but minimised. Toggle: opens it, or closes it (a minimised one is brought back; nothing is closed while a menu or message is up), or for "take" minimises / brings
        back the take window used last (never closes it). "minimiseall" minimises every window but the main one; pressed again it brings those back. Set by the application. */
    std::function<int (const juce::String&)> deskWindowState;
    std::function<void (const juce::String&)> deskWindowToggle;
    /** The Loop button of the take or edit window used last: -1 there is none, 0 off, 1 on; and the same as pressing it (key L). Set by the application. */
    std::function<int()> deskLoopState;
    std::function<void()> deskLoopToggle;
    /** The window keys of the take or edit window used last (mark1 mark2 align search trim markers feedback) and the slip keys (slipl slipr, the setting every window shares).
        State: -1 not available now, 0 off, 1 lit; Act: the same as pressing that button. Set by the application. */
    std::function<int (const juce::String&)> deskActState;
    std::function<void (const juce::String&)> deskAct;
    /** Arm All / Arm None in one key: 0 no track is armed, 1 some are, 2 all are; the toggle arms all, or disarms all when they all are armed (not while recording). */
    int armState() const;
    void remoteArmToggle();
    void remoteWaveColour() { setWaveColour (! waveColour); }
    /** The levels of the stage speaker panel were changed from outside (the Stream Deck): the engine takes them. */
    void applyStageMix() { engine.setStageMix (project.stageGainDb, project.stagePan, project.stagePlaybackDb, project.stageOutputDb); }
    // ---- the control surface (an Avid S1 or any other Mackie Control surface, over MIDI): see Surface.h. Owned by the application. ----
    SurfaceController* surface = nullptr;
    /** The mixer whose window is in front, or was in front most recently (the first mixer when none has been opened yet); null only when there are no mixers
        (at the start screen it is the blank project's mixer: callers check hasProject() themselves). Set by the application. */
    std::function<juce::Uuid()> frontMixerId;
    /** Sends a key to the take or edit window that was last in front, as if it had been typed there (the surface's zoom keys). False if there is no such window. */
    std::function<bool (const juce::KeyPress&)> keyToWorkWindow;
    /** The strip of this audio track in this mixer's window: selected / unselected (as a click on its name plate), and whether it is selected now. */
    std::function<void (const juce::Uuid& mixerId, const juce::Uuid& trackId)> selectStripInMixer;
    std::function<bool (const juce::Uuid& mixerId, const juce::Uuid& trackId)> isStripSelectedInMixer;
    std::function<void()> showSurfacePanel;              // the "Mixer controller" window (MIDI ports, what the buttons do, the MIDI monitor)
    std::function<void()> showPitchPipe, showClick;      // the Pitch pipe and Click windows (menu bar)
    /** The Align window (spot-mic time alignment), measuring on a take window (isEdit false) or an edit. */
    std::function<void (bool isEdit, const juce::Uuid& id)> showAlign;
    /** Gives the engine the pitch pipe's and the click's settings from the project (outputs, level, tempo ...). */
    void applyAux();
    /** Spot-mic alignment: the mixers use this piece's set from now on (a take window's id, or an edit's: see Project::alignOwnerFor). */
    void setAlignOwner (const juce::String& owner);
    /** The piece the mixers should align for now: the one being recorded, played, or worked on (the take / edit window used last). */
    juce::String currentAlignOwner();
    /** The take or edit window you last worked in registers how its Play button behaves; the remote's play / pause uses it. */
    void setTransportHandler (const void* owner, std::function<void()> fn) { transportOwner = owner; transportHandler = std::move (fn); }
    void clearTransportHandler (const void* owner) { if (transportOwner == owner) { transportOwner = nullptr; transportHandler = nullptr; } }
    std::function<void()> transportHandler; const void* transportOwner = nullptr;
    bool crWanted = false, tbWanted = false, minusWasDown = false, crClickLatch = false;     // crClickLatch: the C-R square in the menu bar was clicked (it stays on until clicked again)
    std::unique_ptr<TalkbackOverlay> talkbackOverlay;
    struct TalkbackPoller : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } talkbackPoller;
    /** Display Organiser (set by Main.cpp): the windows it can arrange - the main window, every take window, edit and mixer of the project (open or
        not: a closed one is opened when it is arranged) and the other windows open now; 'recency' orders them by when they were last in front
        (higher = later; -1 = not open). placeWindows puts windows into parts of the screen the main window is on (key, fraction of that screen).
        windowPlacesNow gives the windows on that screen as they are (for "save how the windows are now"): the screen area, and each window's bounds. */
    struct WindowEntry { juce::String key, title, kind; bool open = false; int recency = -1; };
    std::function<std::vector<WindowEntry>()> listWindows;
    std::function<void (const std::vector<std::pair<juce::String, juce::Rectangle<float>>>&)> placeWindows;
    struct WindowPlaces { juce::Rectangle<int> area; std::vector<std::pair<juce::String, juce::Rectangle<int>>> windows; };
    std::function<WindowPlaces()> windowPlacesNow;
    /** Highlights this file in the Media window (if it is open; otherwise it is remembered for when it opens). */
    juce::File mediaSelection;
    std::function<void (const juce::File&)> mediaReveal;
    void revealInMedia (const juce::File& f) { mediaSelection = f; if (mediaReveal) mediaReveal (f); }
    std::function<void (const BounceContext&)> showBounce;
    std::function<bool (const juce::String& key)> windowIsOpen;       // is the program window with this key open ("trim:<edit id>", "bounce:<edit id>")? The edit window lights its Trim and Bounce Out buttons from it
    /** "Find missing audio files": the folder chooser and the search (the main window). Set by the main window. */
    std::function<void()> findMissingAudio;

    /** The last pitch correction / repair, so it can be undone (the new audio files stay on the disk; the originals were never touched). */
    int undoHold = 0;      // > 0 while a dialog is trying something out (pitch curve audition): the undo history is not written to meanwhile
    int saveHold = 0;      // > 0 while a fix's preview files are in a take or an edit (FixTools.cpp: PreviewApplied): the automatic save waits, so a crash can never leave the project pointing at them
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
    int alignTicks = 0;                              // (the watcher looks at which piece to align for three times a second)
    struct DiscAlign { double end = 0.0; juce::String owner; };
    std::vector<DiscAlign> discAlign;                // while a disc plays through the mixers: where each piece ends on the disc and the spot delays it has
    juce::Uuid discToken;
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
    /** Plays the take window's timeline from fromSec. With loopToSec > fromSec it goes round [fromSec, loopToSec) until the loop is let go of
        (Loop switched off: it then carries straight on), at the sample rate of rateOfTake if given. */
    juce::String playTakeWindow (const juce::Uuid& windowId, double fromSec, double loopToSec = -1.0, const juce::Uuid& rateOfTake = juce::Uuid::null());
    juce::String playTake (const juce::Uuid& windowId, const juce::Uuid& takeId, double fromSec, double toSec = -1.0, bool loop = false);
    /** Plays the edit, crossfades included, from fromSec to toSec (-1 = to the end), on the edit timeline. goOnAfterLoop: a loop that is let go of
        (the edit window's Loop switched off) carries straight on past toSec; without it the pass finishes and it stops (the process windows). */
    juce::String playEdit (const juce::Uuid& editId, double fromSec, double toSec = -1.0, bool loop = false, bool goOnAfterLoop = false);
    /** The edit window's visual guide on its own (its S button, 2.102): plays only the guide, where it stands on the edit's timeline, through the
        edit's own mixer on the guide's track - the same arguments and rules as playEdit. */
    juce::String playEditGuide (const juce::Uuid& editId, double fromSec, double toSec = -1.0, bool loop = false, bool goOnAfterLoop = false);
    /** Right-click on a clip in a take window: that track's clip of the take - between marks 1 and 2 if they are set in this take, otherwise the
        whole take - becomes the visual guide of the take window's edit (the one 3 and 4 send to; else an edit made from that take window, the
        edit window used last, or the only edit; made if there is none), starting at 0:00. It replaces the edit's guide, if it had one (Undo puts
        that back). Returns an error or ""; 'editOut' is told which edit it went to. */
    juce::String setEditGuide (const juce::Uuid& takeWindowId, const juce::Uuid& takeId, const juce::Uuid& trackId, juce::Uuid* editOut = nullptr);
    /** One clip of a disc audition: the edit and where it starts on the disc's timeline. */
    struct DiscPlayItem { juce::Uuid editId; double startSeconds = 0.0; };
    /** Plays the clips one after the other, each at its place on the disc timeline (the pauses between them are silence), through the mixers with the
        edits' automation, from fromSec to toSec (-1 = to the end of the last clip). 'token' goes into playInfo.id so the caller knows it is its own playback. */
    juce::String playDisc (const std::vector<DiscPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token);
    /** One finished render (a stereo file made by the Mastering window) and where it starts on the disc's timeline. */
    struct RenderPlayItem { juce::File file; double startSeconds = 0.0; double rate = 0.0; juce::int64 frames = 0; };
    /** Plays the renders where they sit on the timeline (the pauses are silence; a tail that runs into the next piece is added to it), straight to the
        driver outputs outFirst and outFirst + 1 at unity: no mixer, no gain change. Same position / token rules as playDisc. */
    juce::String playRenders (const std::vector<RenderPlayItem>& items, double fromSec, double toSec, const juce::Uuid& token, int outFirst);
    /** Plays one region's raw files (no fades, no neighbours) on the timeline of the file's own samples:
        the trim window's 'play original A / B'. */
    juce::String playRegionOriginal (const juce::Uuid& editId, const juce::Uuid& regionId, juce::int64 fromSample, juce::int64 toSample);
    /** Playhead behaviour (all windows): on = after Stop the playhead stays where playback stopped, so the next Play goes on from there;
        off = it goes back to where playback started. Remembered between runs. */
    bool playheadFollows = true;
    /** The trim window's Slip Left / Slip Right boxes: on by default, and they stay as the user last set them (all trim windows, remembered between runs). */
    bool trimSlipLeft = true, trimSlipRight = true;
    /** The INSERT flag in the edit window can be hidden (while only fades are being worked on). Remembered between runs. */
    bool showInsertFlag = true;
    /** Re-draw waveforms: forgets the saved waveform outline and colours of these audio files (in the Waveforms folder and in memory) so they are worked out again from the audio.
        The windows notice through redrawLog / redrawStamp. */
    void redrawWaveforms (const std::vector<juce::File>& files);
    /** The same, quietly and only in memory: a take whose files have just been finished (a window may have read them half written). */
    void forgetWaveforms (const std::vector<juce::File>& files);
    std::vector<std::pair<int, juce::String>> redrawLog; int redrawStamp = 0;
    /** The edit or take window that was last in front ("edit:<id>" / "take:<id>"; "" = none), set by the application. */
    std::function<juce::String()> lastWorkWindowKey;
    /** The take window that is in front, or was in front last (whatever has been in front since): where R records. Null if no take window has been
        open yet. Set by the application. */
    std::function<juce::Uuid()> lastTakeWindowId;
    std::vector<juce::File> audioFilesOfWindow (const juce::String& key) const;
    void setShowInsertFlag (bool on);
    void setTrimSlip (bool left, bool right);
    void setPlayheadFollows (bool on);
    /** quick: a press of Space or Stop (the play's tidying up is handed to a thread of its own, so it returns at once). Without it (everything
        else: closing, a process window's Stop or Revert ...) the files that were playing are closed when it returns, so they can be deleted. */
    void stopPlayback (bool quick = false);
    /** Seconds played since the playback began, wrapped to the loop length while a loop plays. */
    double playedSeconds() const;
    /** playheadSeconds, moving on smoothly between the audio blocks: what the windows draw the moving playhead with. */
    double playheadSecondsSmooth() const;
    bool isPlaying() const { return engine.isPlaying(); }
    /** Time on the timeline of the thing being played (take or edit), or -1 if nothing plays. */
    double playheadSeconds() const;

    /** Sends the EDIT-marked part (keys 1 and 2) of the marked take to the edit. Without 'atEditPlayhead' it goes after the last piece (key 3);
        with it, at the edit window's playhead (key 4): it REPLACES the same length of what is there and nothing else moves (EditDef::overwriteRegionAt),
        or with 'insert' (Shift + 4) it cuts what is there in two and pushes everything after it later (EditDef::insertRegionAt). With the window's
        Overdub box ticked it is laid over the edit at the playhead instead and plays together with what is under it. Returns an error or "". */
    juce::String copyMarkedToEdit (const juce::Uuid& takeWindowId, bool atEditPlayhead = false, juce::Uuid* editIdOut = nullptr, bool insert = false);
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
    /** Copy the chosen parts of a mixer onto another mixer (toId), or every other one (null id). It is also kept, to paste in other mixers later. */
    juce::String copyMix (const juce::Uuid& fromId, const juce::Uuid& toId, const Project::MixParts& parts);
    /** Paste the last copied mixer (those parts) onto a mixer. */
    juce::String pasteMix (const juce::Uuid& toId);
    /** Keep the chosen parts of a mixer to paste into other mixers later (nothing changes yet). */
    juce::String copyMixToClipboard (const juce::Uuid& fromId, const Project::MixParts& parts);
    bool canPasteMix() const { return mixClipName.isNotEmpty(); }
    juce::String mixClipLabel() const { return mixClipName; }
    Project::MixParts mixClipParts;
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
    /** A project file's backup (Name.fermata.bak: the project as it was saved the time before) put back as the project file and opened. The file it
        replaces is kept beside it as "Name (replaced <date> <time>).fermata.bak"; the take and edit windows' own files are left as they are. */
    bool openBackupProject (const juce::File& bakFile);
    /** Project > Close: the project's windows close and Fermata goes back to its start panel (New / Open). save: write it first; otherwise nothing more is
        written (what was saved automatically as you worked stays). False if it cannot close now (recording). */
    bool closeProject (bool save);
    std::function<void()> closeAllWindows;            // (set by the application) closes every project window
    AudioEngine::LoadStats lastLoad;                  // the load meter: the audio thread's last second (see tickLoad)
    /** quick: an automatic save - a snapshot here, written on the saver's thread (see BackgroundSaver). Save from the menu, closing, Save and
        quit: a full one, written there and then (after the saver has finished what it was given). */
    void saveNow (bool quick = false);
    /** Waits until the automatic saves handed to the saver's thread are on the disk, and takes in what they did (a failed one is saved again). */
    void waitForSaves();
    /** Play / Stop just happened (the automatic save keeps out of the way for a few seconds: the next press may be coming). */
    void noteTransport() noexcept { lastTransportMs = juce::Time::getMillisecondCounter(); }
    juce::uint32 lastTransportMs = 0;
    double lastPlayStartMs = 0.0;                     // how long the last Play took on the message thread (for the log)
    std::unique_ptr<StallWatch> stallWatch;           // logs every moment the message thread is held up (see StallWatch)
    bool skipFinalSave = false;                       // "Quit without saving": the last save when closing is left out
    void restoreLastProject();
    /** After a plan rebuild: the mixers retired before it (a removed mixer, the mixers of a project that was open before) are freed with their
        plug-ins, a little later on the message thread (once every window has let go of them), if the rebuild freed the old plan. */
    void freeRetiredMixersSoon();
    /** Has a project been chosen (new or opened)? Until then nothing is saved or recorded. */
    bool hasProject() const { return project.projectFile != juce::File(); }
    /** The start-up state: an empty in-memory project that is not saved anywhere, until the user picks where the project goes. */
    void startBlankProject();
    juce::File lastProjectFile();
    void shutdown();

private:
    std::vector<std::pair<juce::Uuid, juce::var>> mixUndo;
    juce::var mixClipboard; juce::String mixClipName; juce::Uuid mixClipFrom;
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
    /** The load meter (once a second) and the timing log (a line in fermata-log.txt every minute while the audio runs, and at the end of each take). */
    struct LoadTicker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } loadTicker;
    AudioEngine::LoadStats minuteLoad;                // gathered for the next log line
    int minuteSeconds = 0;
    bool loadWasRecording = false;
    void tickLoad();
    void timerCallback() override;                   // autosave
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // re-plan the engine after structural edits
    // ---- saving (2.102). An automatic save is a snapshot taken here (a few tens of milliseconds) and written on the saver's thread, and it only
    //      happens when nothing is going on: not while playing or recording, not within a few seconds of Play / Stop or of a change, never with
    //      a mouse button held, and at most every 30 seconds (every 5 minutes at the least, if the project keeps changing). Saving from the menu,
    //      opening another project and closing wait for the saver, then write everything there and then.
    static constexpr juce::uint32 kAutosaveGapMs = 30000, kAutosaveLatestMs = 300000;
    void autosaveIfIdle();
    void savedInBackground (BackgroundSaver::Result&, int generation);
    void showSaveError (const juce::String& err, bool automatic);
    void retryFailedSave();                          // closing without saving: the automatic save that failed (if the last one did) is written again
    std::unique_ptr<Project::SaveJob> lastFailedJob; // the automatic save that failed, until a save gets through
    bool quickSaveWanted = false;                    // an automatic save was asked for while one was being written: taken once that is done
    bool closingProgram = false;
    std::unique_ptr<BackgroundSaver> saver;
    std::shared_ptr<bool> saverAlive = std::make_shared<bool> (true);
    int saveGeneration = 0;                          // another project since a save was handed over: what it did is not this project's
    juce::uint32 lastAutosaveMs = 0, lastSaveErrorMs = 0;
    bool wholeSaveNext = false;                      // a window file could not be written: the next save is a full one, there and then
    void afterProjectReplaced();
public:
    /** The Trim window's own waveform size (its . and , keys): independent of the take and edit windows, remembered between sessions, and every Trim window opens at it. */
    float trimWaveZoom = 1.0f;
    juce::ChangeBroadcaster trimWaveZoomChanged;
    void changeTrimWaveZoom (float factor);
};
} // namespace td

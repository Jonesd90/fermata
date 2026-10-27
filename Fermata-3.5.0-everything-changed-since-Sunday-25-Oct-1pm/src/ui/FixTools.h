#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td { namespace fixtools
{
// ---- Take window ----
/** Opens the "Import takes" dialog for these files (an empty list asks which files / folder to use first). */
void importTakes (AppContext&, const juce::Uuid& takeWindowId, const juce::Array<juce::File>& files, juce::Component* parent, const juce::Uuid& intoEdit = juce::Uuid::null());
/** Pass an edit id as intoEdit to bring the audio into that edit (one piece per take, each file on its own track) instead of into a take window. */
void showImportMenu (AppContext&, const juce::Uuid& takeWindowId, juce::Component* source, const juce::Uuid& intoEdit = juce::Uuid::null());
void pitchTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);
/** Export for Processing: sends the marked part of all tracks out to the Processing Media folder (to be corrected in other software) and shows it as 'Waiting for corrected audio'. */
void exportForProcessingEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void exportForProcessingTake (AppContext&, const juce::Uuid& windowId, juce::Component* parent);
/** Brings the corrected files back (checks that they have the same channels, rate and length) and shows their waveform again. */
void relinkEdit (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionId, juce::Component* parent);
void relinkTake (AppContext&, const juce::Uuid& windowId, const juce::Uuid& takeId, juce::int64 waitingFrom, juce::Component* parent);
/** The ReTouch window (spectral repair): rebuilds a box of the picture from the sound around it, finds and mends clicks (De-click), denoises a box. */
void repairTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);
/** Draw a line of pitch against time over the marked part, audition it, then accept or revert. */
void pitchCurveTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);
/** The Pitch button: one window, Pitch curve on the first tab and Pitch process (semitones / cents) on the second. */
void pitchTabsTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);

// ---- Edit window ----
void pitchEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
/** The same for marks 1 - 2 of an edit (each piece repaired in its own place). */
void repairEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void pitchCurveEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void pitchTabsEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
/** Re-HarmoniSer: retunes individual out-of-tune notes in the marked part of all tracks, every track in the same way. */
void reharmoniserEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
/** Noise mode: the whole edit in one spectral window (built once at several zoom levels) with Gain, ReTouch, De-click and the denoiser all to hand; each fix
    goes into the edit at once (new piece files), Undo takes it off, Write back to clip keeps them and the window stays open, Finished closes it. */
void noiseModeEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
/** The Room tone button: learns the project's room tone (every track's noise, per channel and for the mono mix) from a take marked as the room tone in a take
    window, or from marks 1 - 2 of this edit; kept with the project, used by the ReTouch and Noise mode windows' Denoise. */
void roomToneEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);

/** Undoes the last pitch correction / repair (any window). */
void undoLastFix (AppContext&);

/** Project menu: 'Save as...' (a new folder; then the project continues there) or 'Create copy of entire project...' (a perfect, checked copy of every file, to take elsewhere; the project stays where it is). */
void copyProject (AppContext&, juce::Component* parent, bool saveAs);

/** Project menu: reads an AAF (from Pyramix etc.), splits its timeline into pieces and makes one edit window per piece from the audio already imported. */
void importAaf (AppContext&, juce::Component* parent);

bool isAudioFile (const juce::File&);

// ---- plumbing the process windows share (3.1.7: the ReTimEr had copies of its own) ----
/** A message box with an OK button. */
void say (const juce::String& title, const juce::String& text);
/** Opens a process window as a dialog round 'content', centred on 'around' (native title bar, Escape closes it). resizable: its size and place are
    remembered under the title. mayClose: asked before the window closes (false keeps it open: a job still running, say). */
void launchDialog (std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around, bool resizable = false, std::function<bool()> mayClose = {});
/** The 'applied' flag of a fix window's state: while it is true the fix's preview files are in a take or an edit (to be played, or one stream of an
    edit point waiting for the other), and the automatic save waits (AppContext::saveHold) - a crash can then never leave the project file pointing
    at files that were only being listened to. Written like a bool; it raises and drops the hold itself (and drops it if the state goes while on). */
struct PreviewApplied
{
    AppContext* app = nullptr;                                         // (set when the state is made)
    PreviewApplied() = default;
    explicit PreviewApplied (AppContext& a) : app (&a) {}
    PreviewApplied (const PreviewApplied&) = delete;
    PreviewApplied& operator= (const PreviewApplied&) = delete;
    ~PreviewApplied() { if (on && app != nullptr) app->saveHold -= 1; }
    PreviewApplied& operator= (bool b) { if (b != on) { on = b; if (app != nullptr) app->saveHold += b ? 1 : -1; } return *this; }
    operator bool() const noexcept { return on; }
private:
    bool on = false;
};
}} // namespace td::fixtools

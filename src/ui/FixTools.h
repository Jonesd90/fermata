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
}} // namespace td::fixtools

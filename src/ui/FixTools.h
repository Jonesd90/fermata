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
/** Spectral Repair (rebuilds a box of the picture from the sound around it), or - with declick true - the De-Click window (finds and mends clicks). */
/** Export for Processing: sends the marked part of all tracks out to the Processing Media folder (to be corrected in other software) and shows it as 'Waiting for corrected audio'. */
void exportForProcessingEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void exportForProcessingTake (AppContext&, const juce::Uuid& windowId, juce::Component* parent);
/** Brings the corrected files back (checks that they have the same channels, rate and length) and shows their waveform again. */
void relinkEdit (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionId, juce::Component* parent);
void relinkTake (AppContext&, const juce::Uuid& windowId, const juce::Uuid& takeId, juce::int64 waitingFrom, juce::Component* parent);
void repairTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent, bool declick = false);
/** Draw a line of pitch against time over the marked part, audition it, then accept or revert. */
void pitchCurveTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);

// ---- Edit window ----
void pitchEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void repairEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent, bool declick = false);
void pitchCurveEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
/** Re-HarmoniSer: retunes individual out-of-tune notes in the marked part of all tracks, every track in the same way. */
void reharmoniserEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);

/** Undoes the last pitch correction / repair (any window). */
void undoLastFix (AppContext&);

/** Project menu: 'Save as...' (a new folder; then the project continues there) or 'Create copy of entire project...' (a perfect, checked copy of every file, to take elsewhere; the project stays where it is). */
void copyProject (AppContext&, juce::Component* parent, bool saveAs);

bool isAudioFile (const juce::File&);
}} // namespace td::fixtools

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
void repairTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);
/** Draw a line of pitch against time over the marked part, audition it, then accept or revert. */
void pitchCurveTake (AppContext&, const juce::Uuid& takeWindowId, juce::Component* parent);

// ---- Edit window ----
void pitchEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void repairEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);
void pitchCurveEdit (AppContext&, const juce::Uuid& editId, juce::Component* parent);

/** Undoes the last pitch correction / repair (any window). */
void undoLastFix (AppContext&);

/** Project menu: 'Save as...' (a new folder; then the project continues there) or 'Create copy of entire project...' (a perfect, checked copy of every file, to take elsewhere; the project stays where it is). */
void copyProject (AppContext&, juce::Component* parent, bool saveAs);

bool isAudioFile (const juce::File&);
}} // namespace td::fixtools

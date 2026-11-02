#pragma once
// Export for Processing, sending to a program (3.22.0 "Edit in RX"; 3.24.0 any program the person links - iZotope RX, Melodyne ...): the edit window's
// Export for Processing button opens a menu. "Create files to edit" is Export for Processing as before (files in Processing Media, re-linked by hand).
// "Open in <program>" sends the part between marks 1 and 2 to that program - one WAV per track with 2 s of handle each side and markers where the part
// starts and ends - and each file saved there comes back into the edit by itself (only the part is replaced; Ctrl+Z takes it out). Until then the part
// shows as a hatched "In ..." block and plays as it was. (The names here still say rx: RX was the first program.)
#include "AppContext.h"

namespace td { namespace rxlink
{
/** A program the person has linked: what the menu calls it, and where it is. Remembered on this computer (its settings), not in the project. */
struct Program { juce::String name; juce::File file; };
std::vector<Program> programs (AppContext&);
void setPrograms (AppContext&, const std::vector<Program>&);

/** The edit window's Export for Processing button: Create files to edit, Open in <each linked program>, Add a program..., Remove a program. */
void showExportMenu (AppContext&, const juce::Uuid& editId, juce::Component* button);
/** Sends the part between marks 1 and 2 to this program (asks first while Rec mode is on, like every fix). */
void sendToProgram (AppContext&, const juce::Uuid& editId, const Program&, juce::Component* parent);
/** Project menu: the linked programs, Add a program..., Remove a program. */
void showProgramsMenu (AppContext&, juce::Component* target);
/** A file box for the program, then its name; done(true) once it is linked (done may be empty). */
void addProgram (AppContext&, juce::Component* parent, std::function<void (bool)> done);

/** The iZotope RX audio editor / Melodyne in the usual places (the newest version found), or an empty File. */
juce::File findRx();
juce::File findMelodyne();
/** At start-up, once: offers to link the editing programs found on this computer (RX, Melodyne), or to choose one (a few seconds after the start). */
void askAtFirstStart (AppContext&);

/** The once-a-second look at the files that are out in programs: whatever has been saved there comes into the edits. Kept by the application while it runs. */
std::unique_ptr<juce::Timer> makeWatcher (AppContext&);

/** The "In ..." block's right-click menu: bring in every file of piece 'regionId' as the program has it now (even if not saved since), open its files in
    the program again, or show them in Explorer. */
void bringInNow (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionId);
void openAgain (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionId);
void showFiles (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionId);
}} // namespace td::rxlink

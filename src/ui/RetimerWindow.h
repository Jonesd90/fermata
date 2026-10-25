#pragma once
#include "AppContext.h"

namespace td
{
/** The ReTimEr window (2.103), from the edit window's ReTimEr button: slows down or speeds up a whole piece (the one clicked, 'selectedPiece', when marks
    1 and 2 are not set) or the part between marks 1 and 2 (with ramps in and out), every track together and without changing the pitch; everything after
    it moves along. Listen to the original and the retimed version, then Write back to clip (Undo fix / Ctrl+Z take it off again). See core/Retime.h. */
void openRetimer (AppContext& app, const juce::Uuid& editId, const juce::Uuid& selectedPiece, juce::Component* parent);
} // namespace td

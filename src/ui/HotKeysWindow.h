#pragma once
#include "Uikit.h"
#include <vector>

namespace td
{
/** Project > HotKeys: every key of the program and what it does, window by window, with the keys drawn as keys. Only a list to read: nothing in it is
    assignable. Whenever a key is added or changed anywhere in the program, its line here is changed with it (DEVELOPMENT-NOTES, 3.0.1). */
namespace hotkeys
{
/** One line of the list.
    keys: as they are pressed - "Ctrl+Z", "Shift+4". "or" stands between keys that do the same job; "/" between a pair whose two jobs the description
    gives in the same order ("Home / End" - "the start / the end"). click, drag, double-click, right-click and wheel are the mouse.
    An empty keys is a note: it is written across the whole line, under the heading. */
struct Line { const char* section; const char* keys; const char* what; };

inline const std::vector<Line>& lines()
{
    static const std::vector<Line> v
    {
        // ---------------------------------------------------------------------------------------------------- everywhere
        { "Everywhere", "", "In the main window and every window on the desk (take, edit, Trim, mixer, meter bridge ...), except while you are typing in a box: the keys then type. The repair, pitch, ReHarmoniSer and ReTimEr windows, plug-in windows and the Take Display screens have keys of their own, further down." },
        { "Everywhere", "Space", "Play or stop the take or edit window you used last, from any window that has no play of its own." },
        { "Everywhere", "Space", "While a take is recording: stop the recording, from every window." },
        { "Everywhere", "R", "Start recording into the take window you used last, if it is open (with no take window open, R does nothing). Recording needs Rec mode (the Rec half of the Rec | Pre button): without it, a warning offers to switch it on. In the edit window R butts pieces together, in ReTouch and Noise mode it does the ReTouch, on the pitch curve it previews; in ReTimEr, ReHarmoniSer and plug-in windows it does nothing." },
        { "Everywhere", "M", "Open or close the mixer: the edit's own mixer while you work in an edit or its Trim window, otherwise the processing mixer. If it is open but behind other windows, M brings it forward." },
        { "Everywhere", "Ctrl+B", "Open or close the meter bridge." },
        { "Everywhere", "U", "Open or close the pitch pipe (a minimised one comes back)." },
        { "Everywhere", "I", "Open or close the click (a minimised one comes back)." },
        { "Everywhere", "Ctrl+Z", "Undo: the edits, the mastering and the takes' details (bars, duds, names, positions, removed takes), up to 30 steps back." },
        { "Everywhere", "Ctrl+Y or Ctrl+Shift+Z", "Redo." },
        { "Everywhere", "", "Keys pressed in the mixer, phase scope, meter bridge, pitch pipe, click, Media, Take Display, Display Organiser, Align and HotKeys windows go on to the take or edit window you used last, so Space, 1, 2, L and the rest work from there too." },

        // ---------------------------------------------------------------------------------------------------- main window
        { "Main window", "click", "The Rec | Pre button in the menu bar: Rec (left half) switches Rec mode on or off - recording / tracking, nothing heavy in the background; Pre (right half) switches the lead-in on or off. Right-click: how many seconds Pre keeps (1 to 10)." },
        { "Main window", "click", "The MATCHING box (top row): how far the take matching has got, Pause / Resume, and its switch for every take window." },
        { "Main window", "Enter", "In an input or output name box: done, the name is kept." },
        { "Main window", "", "Project > Save this window: the take window, edit window or mixer you used last is kept as it is now. Fermata saves everything automatically as you work; this marks what you want to keep. When you quit, the windows that changed since you last saved them (Save, or Save this window) are listed with a tick each: untick one and it goes back to your last save of it (new recordings are always kept)." },
        { "Main window", "Esc", "In a name box: back to the name it had." },
        { "Main window", "Tab", "In a name box: on to the next one, to name the inputs one after another." },
        { "Main window", "double-click or Alt+click", "An input's gain knob: back to 20 dB (as the readout shows it)." },
        { "Main window", "double-click", "An input's gain readout: type a gain." },
        { "Main window", "double-click / Ctrl+click", "A level knob of the stage speaker boxes (top right): type a level / back to 0 dB." },
        { "Main window", "double-click / Ctrl+click", "A pan knob of the stage speaker boxes: type a position / back to the centre." },

        // ---------------------------------------------------------------------------------------------------- take window
        { "Take window", "Space", "Play or stop from the playhead. With Loop on and marks 1 and 2 set: round and round the marked part." },
        { "Take window", "R", "Start recording into this take window (in Rec mode: without it, a warning offers to switch it on)." },
        { "Take window", "1 / 2", "Mark 1 / mark 2 at the playhead, in the take under it." },
        { "Take window", "P", "Marks 1 and 2 round the whole of the selected take." },
        { "Take window", "5", "Take marks 1 and 2 away." },
        { "Take window", "3", "Send the marked part to the edit, after its last piece." },
        { "Take window", "4", "Put the marked part at the edit window's playhead, over the same length of what is there (nothing else moves)." },
        { "Take window", "Shift+4", "Insert the marked part at the edit window's playhead: what is there is cut in two and everything after it moves later. (Shift + click on To playhead does the same.)" },
        { "Take window", "D", "Mark the selected takes as duds, or take the mark off again (while recording: the take being recorded)." },
        { "Take window", "L", "Loop on / off." },
        { "Take window", "C", "Scroll so the playhead is in the middle of the window (the zoom stays)." },
        { "Take window", "J / Shift+J", "The same place in the music in the next / the previous take (found by matching the takes' harmony; duds are passed over; while playing, play goes on from there). Needs take matching switched on for the window (the MATCHING lamp in its marks row)." },
        { "Take window", "click / right-click", "The MATCHING lamp at the right of the marks row: switch take matching on or off for this window / how far it has got, Pause, every window's switch." },
        { "Take window", "Home / End", "Go to the start / to the end of the last take." },
        { "Take window", "Left / Right", "Zoom out / in, round the playhead." },
        { "Take window", "Shift+Left / Shift+Right", "Move the playhead 10 ms earlier / later." },
        { "Take window", "Up / Down", "All tracks smaller / bigger." },
        { "Take window", ". / ,", "Bigger / smaller waveforms (the take and edit windows share the size)." },
        { "Take window", "B", "The bars and notes box of the take clicked last - also the take being recorded: type its IN Bar, Tab, its OUT Bar, Enter." },
        { "Take window", "Shift+B", "Open or close the take chart: every take as a line along the piece's bars, the bars with no take / only one / two / three or more lit up and listed." },
        { "Take window", "Ctrl+A", "Select every take." },

        // ---------------------------------------------------------------------------------------------------- take window: a take's bars and notes box
        { "Take window: a take's bars and notes box", "", "Opened by B (the take clicked last), double-clicking a take's audio or right-clicking the strip above its tracks (also while it records)." },
        { "Take window: a take's bars and notes box", "Enter", "In IN Bar: on to OUT Bar. In OUT Bar: save." },
        { "Take window: a take's bars and notes box", "Tab", "On to the next box (IN, OUT, Notes, the buttons)." },
        { "Take window: a take's bars and notes box", "Ctrl+Enter", "In Notes: save (Enter there starts a new line)." },
        { "Take window: a take's bars and notes box", "Esc", "Close without saving." },
        // ---------------------------------------------------------------------------------------------------- the project's sample rate (3.9.0)
        { "Project sample rate box", "", "Comes up when a project is made (after its folder and name), before the first take of a project that has no rate yet, and from Project > Project sample rate." },
        { "Project sample rate box", "Enter", "OK, once a rate is picked (nothing is picked to begin with)." },
        { "Project sample rate box", "Esc", "Cancel: no project is made / nothing changes." },
        // ---------------------------------------------------------------------------------------------------- the take chart (3.8.0)
        { "Take chart", "", "Drops down from BARS at the left of a take window's bar strip (click it), or Shift+B. No take / Only 1 / 2 takes / 3 takes / 4+ light those bars up and list them (crimson, bright red, red-orange, yellow, light to dark green)." },
        { "Take chart", "BARS / Shift+B / Esc", "Put it away (also its Close button)." },
        { "Take chart", "click", "A take's line: that bar in that take. The ALL line, or a line of the list: that bar (the take window goes there)." },
        { "Take chart", "drag", "Its bottom-right corner: bigger or smaller (it keeps the size)." },

        // ---------------------------------------------------------------------------------------------------- take window: with the mouse
        { "Take window: with the mouse", "click / drag", "The time ruler: the playhead goes there; dragged along the ruler, it follows the mouse (scrubbing). While playing, play carries on from the spot clicked, or from where the drag let go." },
        { "Take window: with the mouse", "Ctrl+click", "A take: add it to the selected takes, or take it out (the selection Bounce Out's \"Selected takes\" uses). Ctrl + Shift + click does the same." },
        { "Take window: with the mouse", "Ctrl+click", "An empty part of the timeline: the selected takes stay selected (a plain click there lets go of them)." },
        { "Take window: with the mouse", "Alt+drag", "Over the tracks: a lasso. Every take it touches is selected." },
        { "Take window: with the mouse", "Alt+click", "Select only the take under the mouse (or none)." },
        { "Take window: with the mouse", "Ctrl+Alt+drag", "Across a take: marks 1 and 2 on only the tracks the drag covers." },
        { "Take window: with the mouse", "Ctrl+Shift+drag", "A take's clip up or down: move the clips on that track to another track (those of all the selected takes, if this take is one of them)." },
        { "Take window: with the mouse", "click", "A bar on the bar strip (under the head): the takes that have it are framed and the playhead goes to it. BARS at its left: the take chart." },
        { "Take window: with the mouse", "right-click", "The bar strip: how many bars the piece has (the strip is red until you enter it)." },
        { "Take window: with the mouse", "double-click", "A take's audio: its bars and notes box." },
        { "Take window: with the mouse", "double-click", "The strip above a take's tracks: Take settings (rename the take and its files)." },
        { "Take window: with the mouse", "right-click", "A take's audio: re-draw the waveform, bars and notes, room tone, or use it as the edit window's visual guide." },
        { "Take window: with the mouse", "right-click", "A \"Waiting for corrected audio\" block: re-link the corrected audio, or stop waiting and keep the original." },
        { "Take window: with the mouse", "Shift+click / Ctrl+click", "A track name: every track from the last one clicked to this one / this track in or out of the selection." },
        { "Take window: with the mouse", "right-click", "A track name: colour, move its clips onto another track, remove or delete tracks." },

        // ---------------------------------------------------------------------------------------------------- take and edit windows: markers and the clock
        { "Take and edit windows: markers and the clock", "right-click", "The marker line (with Markers shown): add a marker, or clear them all." },
        { "Take and edit windows: markers and the clock", "right-click / double-click", "A marker: rename or delete it / rename it." },
        { "Take and edit windows: markers and the clock", "right-click", "The big clock: show the position as time, minutes:seconds, timecode or samples; the timecode frame rate and start; copy; go to a time." },
        { "Take and edit windows: markers and the clock", "double-click", "The big clock: type a time and go there." },

        // ---------------------------------------------------------------------------------------------------- edit window
        { "Edit window", "Space", "Play or stop." },
        { "Edit window", "1 / 2", "Mark 1 / mark 2 at the playhead (where you clicked, when nothing plays): the part the pitch, repair and ReHarmoniSer tools work on, and Loop plays." },
        { "Edit window", "P", "Marks 1 and 2 round the selected pieces, exactly at their start and end." },
        { "Edit window", "5", "Take marks 1 and 2 away." },
        { "Edit window", "Enter", "Open ReTouch, DeClick & Denoise for the part between marks 1 and 2." },
        { "Edit window", "H", "Open the ReHarmoniSer for the part between marks 1 and 2." },
        { "Edit window", "T", "Open the selected join (a numbered flag) in the Trim window; with a piece selected, the join before it." },
        { "Edit window", "F1", "Snip at the playhead: the piece there is cut with a short, normal crossfade (an \"sn\" flag, not a numbered edit point). Playback carries on." },
        { "Edit window", "R", "Butt the selected piece up against the piece before it." },
        { "Edit window", "4", "Lay the part marked in a take window down at this edit's playhead, over the same length of what is there." },
        { "Edit window", "Shift+4", "Insert the part marked in a take window at this edit's playhead: what is there is cut in two and everything after it moves later." },
        { "Edit window", "Delete or Backspace", "Delete the selected piece (or overdub)." },
        { "Edit window", "L", "Loop on / off (round marks 1 and 2)." },
        { "Edit window", "C", "Scroll so the playhead is in the middle of the window." },
        { "Edit window", "Home / End", "Go to the start / to the end of the last piece." },
        { "Edit window", "Left / Right", "Zoom out / in." },
        { "Edit window", "Up / Down", "Shorter / taller tracks." },
        { "Edit window", ". / ,", "Bigger / smaller waveforms." },

        // ---------------------------------------------------------------------------------------------------- edit window: with the mouse
        { "Edit window: with the mouse", "Ctrl+click", "A piece: select the whole piece, ready to move. Ctrl + click more pieces to add them; Ctrl + click a selected one to take it out." },
        { "Edit window: with the mouse", "Ctrl+drag", "A piece (or drag it once it is selected): slide it sideways. An overdub moves freely." },
        { "Edit window: with the mouse", "Ctrl+click", "Outside the audio: the selected pieces stay selected (a plain click there lets go of them)." },
        { "Edit window: with the mouse", "Alt+drag", "Over the audio: a lasso. Every piece it touches is selected, overdubs too; the playhead stays where it is." },
        { "Edit window: with the mouse", "Alt+click", "Select only the piece under the mouse (or none)." },
        { "Edit window: with the mouse", "Ctrl+Alt+drag", "Over the audio: marks 1 and 2 on only the tracks the drag covers." },
        { "Edit window: with the mouse", "Shift+drag", "A track's clip up or down: move that clip to another track (it swaps with a clip already there; mono to mono, stereo to stereo)." },
        { "Edit window: with the mouse", "Shift+click / Ctrl+click", "A track name: every track from the last one clicked to this one / this track in or out of the selection." },
        { "Edit window: with the mouse", "double-click", "A piece: the Trim window, on the join before it." },
        { "Edit window: with the mouse", "double-click", "A numbered flag: the Trim window, on that join." },
        { "Edit window: with the mouse", "double-click", "A volume change's teal line (or its shaded glide): change it again - the amount for each track and the fade in / fade out, heard as you change them (Play in the box). Right-click it: the same, as \"Change this volume change...\"." },
        { "Edit window: with the mouse", "right-click", "A piece: volume changes (to the next edit, between marks 1 and 2, between the fades), remove them, delete an overdub, re-draw the waveform." },
        { "Edit window: with the mouse", "right-click", "A piece, with marks 1 and 2 inside it: Find other versions - every place in the take window's takes where the marked music was played (\"possible\" ones: a false start, or the same notes elsewhere); Audition plays each in place, Use this one puts it in (everything after moves to fit; Ctrl+Z takes it out). Needs take matching switched on for that take window." },
        { "Edit window: with the mouse", "right-click", "A track name: colour, remove tracks from this edit." },
        { "Edit window: with the mouse", "right-click", "The visual guide: move its start (to 0:00 or the playhead), hide it, remove it." },
        { "Edit window: with the mouse", "right-click or Shift+click or Ctrl+click", "An automation point (Automation on): delete it." },
        { "Edit window: with the mouse", "Alt+drag", "An automation point: move it ten times finer." },

        // ---------------------------------------------------------------------------------------------------- trim window
        { "Trim window", "F2", "Audition the join (press again to stop)." },
        { "Trim window", "Space", "Stop what is playing." },
        { "Trim window", "Enter", "Keep the changes and go back to the edit window." },
        { "Trim window", "[ / ]", "Keep this fade and go to the join on the left / on the right." },
        { "Trim window", "A", "Copy A's fade-out onto B's fade-in." },
        { "Trim window", "Z", "Copy B's fade-in onto A's fade-out." },
        { "Trim window", "Left / Right", "Zoom out / in." },
        { "Trim window", ". / ,", "Bigger / smaller waveforms (the Trim window has its own size)." },
        { "Trim window", "wheel", "Over the waveforms: zoom in (wheel up) / out (wheel down)." },
        { "Trim window", "Shift+wheel", "Over the waveforms: move the view left / right (a sideways swipe or a tilt of the wheel does the same). The scroll bar under the waveforms reaches all of both takes." },
        { "Trim window", "drag", "A fade, the crossfade or a take dragged to the left or right edge: the view moves along (faster further out), so the join can be taken far away while zoomed in." },
        { "Trim window", "double-click or Alt+click", "\"Level of A (out)\" or \"Level of B (in)\": back to 0 dB." },

        // ---------------------------------------------------------------------------------------------------- mixer
        { "Mixer", "Enter", "In \"Mixer name\": rename the mixer (an edit's mixer renames the edit too)." },
        { "Mixer", "Enter / Esc", "In a VCA strip's name box: keep the name / back to the old one." },
        { "Mixer", "Shift+click / Ctrl+click", "A strip's name plate: every strip from the last one clicked to this one / this strip in or out of the selection." },
        { "Mixer", "Shift+Alt", "Held, with two or more strips selected: they are linked while you hold them (bold orange outlines). Faders, sends and pans move together; M, S and the sends' mute and pre are set the same; plug-ins go on, come off or are bypassed on every one." },
        { "Mixer", "right-click", "A strip's name plate: colour, hide strips from this mixer, select all or none." },
        { "Mixer", "click", "An edit's own mixer, in the METERS strip: Match edit hides the strips of tracks that are not in the edit and shows those that are (once, now); Show all strips brings every hidden strip back." },
        { "Mixer", "double-click or Ctrl+click", "A fader cap: back to 0 dB." },
        { "Mixer", "double-click", "The dB number under a fader: type a level." },
        { "Mixer", "click", "Press and hold the small arrows beside the fader number: 1 dB steps that keep repeating." },
        { "Mixer", "double-click or Ctrl+click", "A PAN slider: back to the centre (a stereo track's PAN L hard left, PAN R hard right)." },
        { "Mixer", "double-click / Ctrl+double-click", "A send dial: 0 dB / type a level." },
        { "Mixer", "double-click or Alt+click", "The fader in a send's panel: 0 dB." },
        { "Mixer", "Alt+drag or Shift+drag", "An insert's name onto another slot, in any mixer: an exact copy with all its settings (a plain drag moves the plug-in; two plug-ins swap places)." },
        { "Mixer", "right-click", "An insert slot: open, bypass, remove or replace the plug-in, scan for plug-ins." },
        { "Mixer", "right-click", "The LC key (or press and hold it): the low-cut panel. A click switches the low cut on or off." },
        { "Mixer", "double-click or Alt+click / double-click", "The low-cut panel's slider: back to 80 Hz / its value: type a frequency." },
        { "Mixer", "wheel / Shift+wheel", "Over the strips: scroll left and right / up and down (when the window is shorter than the strips)." },

        // ---------------------------------------------------------------------------------------------------- retouch, declick & denoise (spectral repair)
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "Space", "Play the part of the picture in view (from the red playhead if one is placed), as \"Listen to\" says, or stop." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "R", "The ReTouch tab, and ReTouch the box." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "G", "The Gain tab, and the gain of the box (silence to +18 dB)." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "D", "The De-click tab, and de-click the box's time (everything, with no box)." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "L", "The Denoise tab, and the box becomes the room tone (at least half a second)." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "N", "The Denoise tab, and denoise the box with the room tone." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "C", "The Consonants tab: Copy consonant / Copy and remove keep the consonant in the box (every track and channel) with the project." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "P", "Paste the consonant chosen in the Consonants list at the red playhead (click the time ruler to place it)." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", ", / .", "Nudge the consonant just pasted 1 ms earlier / later." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "Shift+, / Shift+.", "Nudge the consonant just pasted 5 ms earlier / later." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "Left / Right", "Zoom out / in, in time." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "Up / Down", "Zoom out / in, in pitch." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "Esc", "Close, as Cancel (it asks first when fixes have not been written back)." },
        { "ReTouch, DeClick & Denoise (Spectral Repair)", "", "In the OUT STREAM and IN STREAM windows (a marked part with an edit point in it) the keys are the same, but Esc does nothing: close them with the X or Cancel. The same goes for the pitch and ReHarmoniSer stream windows." },

        // ---------------------------------------------------------------------------------------------------- noise mode
        { "Noise mode", "Space", "Play from the red playhead (or the left of the page) to the end of the edit, turning the pages (Loop repeats the page), or stop." },
        { "Noise mode", "R / G / D / L / N", "As in Spectral Repair, but each fix goes straight into the edit; D with no box covers the whole edit." },
        { "Noise mode", "Home / End", "Playhead to the start / the end of the edit." },
        { "Noise mode", "Left / Right", "Zoom out / in, in time." },
        { "Noise mode", "Up / Down", "Zoom out / in, in pitch." },
        { "Noise mode", "Esc", "Close, as \"Finished de-noising\" (asks Keep / Throw away / Cancel when fixes have not been written back)." },

        // ---------------------------------------------------------------------------------------------------- spectral repair and noise mode: with the mouse
        { "Spectral Repair and Noise mode: with the mouse", "click / Shift+click or right-click", "The colour roll (bottom left of the picture): the next / the previous colour look." },
        { "Spectral Repair and Noise mode: with the mouse", "wheel", "Over the colour roll: roll through the colour looks." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click", "The colour roll: back to the standard colours." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click", "The time ruler above the picture: show everything again." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click", "The picture: clear the box." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click or Alt+click", "Denoise Amount / Residual: back to 0 dB / -12 dB." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click or Alt+click", "The slider of the box's gain: back to 0 dB." },
        { "Spectral Repair and Noise mode: with the mouse", "double-click or Alt+click", "Spectral Repair, Consonants tab: the Level slider back to 0 dB." },
        { "Spectral Repair and Noise mode: with the mouse", "right-click / double-click", "Noise mode's clock: as the take and edit windows' clock." },

        // ---------------------------------------------------------------------------------------------------- pitch
        { "Pitch", "Space", "Pitch curve: play the part of the line in view, from the playhead placed on the ruler (round and round with Loop lit), or stop." },
        { "Pitch", "R", "Pitch curve: Preview, work out the corrected sound and play it." },
        { "Pitch", "Left / Right", "Pitch curve: zoom out / in, in time." },
        { "Pitch", "Up / Down", "Pitch curve: zoom out / in, in cents." },
        { "Pitch", "Delete or Backspace", "Pitch curve: remove the selected point." },
        { "Pitch", "Esc", "Close the window (a preview that was not accepted is put back)." },
        { "Pitch", "right-click or double-click", "A point on the pitch curve: delete it." },
        { "Pitch", "double-click", "The pitch curve's time ruler: the whole part again." },
        { "Pitch", "double-click or Alt+click", "Pitch process: Semitones or Cents back to 0." },

        // ---------------------------------------------------------------------------------------------------- reharmoniser
        { "ReHarmoniSer", "Space", "Play from the playhead, or stop (between the loop markers with Loop on; with Isolate on, just the picked note)." },
        { "ReHarmoniSer", "Enter", "Apply correction: the picked note goes into Corrected Notes." },
        { "ReHarmoniSer", "Ctrl+Z / Ctrl+Shift+Z", "Undo / redo: with a note picked, its setting changes; with none, the last corrected note comes out / goes back." },
        { "ReHarmoniSer", "1 / 2", "Loop start / loop end marker at the playhead." },
        { "ReHarmoniSer", "5", "Clear both loop markers." },
        { "ReHarmoniSer", "L", "Loop on / off." },
        { "ReHarmoniSer", "Left / Right", "Zoom out / in, in time." },
        { "ReHarmoniSer", "Up / Down", "Zoom out / in, in pitch." },
        { "ReHarmoniSer", "Esc", "With the \"Pitch over time\" chart open: close the chart." },
        { "ReHarmoniSer", "Esc", "Otherwise: close the window. With corrected notes not yet written back it asks first (Write back to clip keeps them); so do Cancel and the X." },
        { "ReHarmoniSer", "Enter", "In the \"meant to be\" box: the target note (F5, Ab4 ...). In \"Reference A4\": the A (400 - 480 Hz)." },
        { "ReHarmoniSer", "wheel / Ctrl+wheel / Shift+wheel", "Over the picture: zoom in time round the mouse / zoom the pitch range / scroll in time." },
        { "ReHarmoniSer", "double-click", "The time ruler: the whole length." },
        { "ReHarmoniSer", "double-click", "The picture: clear the selection and put the playhead there." },
        { "ReHarmoniSer", "double-click", "The Colour wheel (View): back to the original amber." },
        { "ReHarmoniSer", "double-click or Alt+click", "A channel's brightness slider (See mixer): back to 0 dB." },

        // ---------------------------------------------------------------------------------------------------- retimer
        { "ReTimEr", "Space", "Stop, if anything plays; otherwise Play retimed (it makes the retimed audio first if it needs to)." },
        { "ReTimEr", "O", "Play original: the edit as it is, from the pre-roll to 2 s past the part." },
        { "ReTimEr", "Enter", "In a number box (new length, %, ramps): tidy the number and give the keyboard back." },
        { "ReTimEr", "Esc", "Close, as Cancel (the edit is left as it was)." },

        // ---------------------------------------------------------------------------------------------------- mastering
        { "Mastering", "", "These are the DDP builder tab's. On the other tabs, Space plays the take or edit window you used last." },
        { "Mastering", "Space", "Play the disc from the playhead, or stop." },
        { "Mastering", "Left / Right", "Zoom out / in, on the playhead." },
        { "Mastering", "C", "Scroll so the playhead is in the middle." },
        { "Mastering", "Home", "Playhead to the start of the disc." },
        { "Mastering", "Enter", "In \"Pause before this track\": that pause for the selected track." },
        { "Mastering", "wheel / Ctrl+wheel", "Over the disc: scroll / zoom round the playhead." },
        { "Mastering", "double-click", "A PQ flag: back to its automatic place." },

        // ---------------------------------------------------------------------------------------------------- display organiser
        { "Display Organiser", "Enter", "Arrange: the windows go where the picture shows them." },
        { "Display Organiser", "double-click", "A layout in the list: load it and arrange the windows at once." },

        // ---------------------------------------------------------------------------------------------------- take display
        { "Take Display", "Esc", "On a Take Display screen: close that screen." },
        { "Take Display", "double-click", "A Take Display screen: close it." },
        { "Take Display", "Space", "On a screen: play or stop the take or edit window you used last." },
        { "Take Display", "Enter", "In the session timer's box (the Take Display window): set the timer, as Start." },

        // ---------------------------------------------------------------------------------------------------- plug-in windows
        { "Plug-in windows", "Space", "Play or stop the take or edit window you used last, even while the plug-in has the keyboard." },
        { "Plug-in windows", "R", "Left to the plug-in (for typing a preset name): it never starts a recording." },

        // ---------------------------------------------------------------------------------------------------- project designer
        { "Project Designer", "Delete or Backspace", "Delete the selected track, bus, VCA or mixer (it asks first; not while recording)." },
        { "Project Designer", "right-click", "A row: move it up or down, delete it; on Mixers, open or delete the mixer." },
        { "Project Designer", "double-click", "A Name cell: rename it there." },

        // ---------------------------------------------------------------------------------------------------- pitch pipe, click and media
        { "Pitch pipe, Click and Media", "click", "Pitch pipe: press and hold a key: it sounds while you hold it. In chord mode, press and hold Play: the lit keys sound together." },
        { "Pitch pipe, Click and Media", "double-click or Alt+click", "Pitch pipe: the Tone slider back to its usual setting." },
        { "Pitch pipe, Click and Media", "double-click or Alt+click", "Click: the quaver tick's level back to -20 dB." },
        { "Pitch pipe, Click and Media", "double-click", "Media: a file is shown in Explorer; a folder opens or closes." },
        { "Pitch pipe, Click and Media", "right-click", "Media: show the file or folder in Explorer, or copy its path." },

        // ---------------------------------------------------------------------------------------------------- boxes for typing a value
        { "Boxes for typing a value", "Enter / Esc", "The box that opens when you double-click a fader number, a pan, the low cut, a preamp gain, a stage knob or a clock: apply / leave the value alone." },
        { "Boxes for typing a value", "Enter / Esc", "A marker's name box: keep the name / the old name." },
        { "Boxes for typing a value", "Enter", "Align spot mics, a spot's delay box: that delay for this piece (0 - 200 ms)." },
        { "Boxes for typing a value", "Enter", "Import AAF as edit windows, the silence-gap box: read the AAF again with that gap." },
        { "Boxes for typing a value", "Esc", "The Plug-ins window's search box: clear the search." },
        { "Boxes for typing a value", "double-click or Alt+click", "The faders of the Change volume windows (right-click a piece in the edit window): back to 0 dB." },

        // ---------------------------------------------------------------------------------------------------- messages and questions
        { "Messages and questions", "Enter", "OK: the main button." },
        { "Messages and questions", "Esc", "Close or cancel. Esc also closes the Preamps, Plug-ins, CPU cores, Change volume, Loudness report and Import windows, and the repair tools' settings boxes." },
        { "Messages and questions", "Tab", "On to the next box or button." },
        { "Messages and questions", "", "A question with two buttons: Enter or the first button's first letter presses the first, Esc or the second's first letter the second. With three: the first letter of each of the first two, Esc for the third." },
        { "Messages and questions", "O / I / B", "Export for Processing, when the marked part has an edit point in it: send the Out stream / the In stream / Both." },
    };
    return v;
}

inline bool isMouseWord (const juce::String& s)
{
    return s == "click" || s == "drag" || s == "double-click" || s == "right-click" || s == "wheel";
}

/** The whole list as plain text (the Copy button): a heading per window, the keys in a column, then what they do. */
inline juce::String asText (const juce::String& version)
{
    juce::String out ("FERMATA " + version + " - HOTKEYS\n");
    juce::String section;
    for (auto& l : lines())
    {
        if (section != l.section) { section = l.section; out << "\n" << section.toUpperCase() << "\n"; }
        const juce::String keys (l.keys);
        if (keys.isEmpty()) out << "  (" << l.what << ")\n";
        else out << "  " << keys.paddedRight (' ', 26) << "  " << l.what << "\n";
    }
    return out;
}
} // namespace hotkeys

/** The list itself: headings, and a line per key with the keys drawn as keycaps on the left and what they do on the right. Sits in a viewport. */
class HotKeysList : public juce::Component
{
public:
    HotKeysList() { setWantsKeyboardFocus (true); }      // (with the keyboard here, Up / Down / Page Up / Page Down / Home / End scroll the list; every other key goes on)
    void setFilter (const juce::String& f) { filter = f.trim(); relayout(); }
    void layoutFor (int width, int height) { if (width > 0) { listW = width; minH = height; relayout(); } }      // (at least the viewport's height: the list's background fills it)
    int count() const { int n = 0; for (auto& r : rows) if (r.kind == Row::Line && juce::String (hotkeys::lines()[r.index].keys).isNotEmpty()) ++n; return n; }     // (the notes are not counted)

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::row);
        const auto clip = g.getClipBounds();
        if (rows.empty())
        {
            g.setColour (theme::dimText); g.setFont (juce::FontOptions (14.0f, juce::Font::italic));
            g.drawText ("No key matches \"" + filter + "\".", getLocalBounds().reduced (16, 0).removeFromTop (60), juce::Justification::centredLeft, true);
            return;
        }
        int alt = 0;
        for (auto& r : rows)
        {
            if (r.kind == Row::Head) alt = 0;
            if (r.y > clip.getBottom() || r.y + r.h < clip.getY()) { if (r.kind == Row::Line) ++alt; continue; }
            const auto& ln = hotkeys::lines()[r.index];
            const auto area = juce::Rectangle<int> (0, r.y, getWidth(), r.h);
            if (r.kind == Row::Head)
            {
                g.setColour (theme::text); g.setFont (juce::FontOptions (15.5f, juce::Font::bold));
                g.drawText (ln.section, area.reduced (kPad, 0).withTrimmedTop (8), juce::Justification::centredLeft, true);
                g.setColour (theme::accent); g.fillRect (kPad, r.y + r.h - 3, getWidth() - 2 * kPad, 2);
                continue;
            }
            if (alt++ % 2 == 1) { g.setColour (theme::rowAlt); g.fillRect (area); }
            if (juce::String (ln.keys).isEmpty())
            {
                drawText (g, ln.what, juce::Rectangle<int> (kPad + 4, r.y + kRowPad, getWidth() - 2 * kPad - 8, r.h - 2 * kRowPad), true);
                continue;
            }
            for (auto& p : keyPieces (ln.keys))
            {
                auto b = p.box.translated (kPad, r.y + kRowPad);
                if (p.kind == Piece::Cap)
                {
                    g.setColour (theme::button); g.fillRoundedRectangle (b.toFloat(), 4.0f);
                    g.setColour (theme::button.darker (0.45f)); g.fillRect (b.getX() + 3, b.getBottom() - 2, b.getWidth() - 6, 2);           // (a key's lower edge)
                    g.setColour (theme::border); g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 4.0f, 1.0f);
                    g.setColour (theme::inkOn (theme::button)); g.setFont (capFont());
                    g.drawText (p.text, b.withTrimmedBottom (1), juce::Justification::centred, false);
                }
                else
                {
                    g.setColour (theme::dimText); g.setFont (p.kind == Piece::Mouse ? juce::FontOptions (13.0f, juce::Font::italic) : juce::FontOptions (12.5f));
                    g.drawText (p.text, b, juce::Justification::centred, false);
                }
            }
            drawText (g, ln.what, juce::Rectangle<int> (kPad + kKeysW, r.y + kRowPad, getWidth() - kPad * 2 - kKeysW, r.h - 2 * kRowPad), false);
        }
    }

private:
    static constexpr int kPad = 14, kKeysW = 270, kCapH = 22, kRowPad = 6, kHeadH = 40;
    struct Row { enum Kind { Head, Line } kind; size_t index; int y, h; };
    struct Piece { enum Kind { Cap, Plus, Word, Mouse } kind; juce::String text; juce::Rectangle<int> box; };
    std::vector<Row> rows;
    juce::String filter;
    int listW = 600, minH = 0;

    static juce::Font capFont() { return juce::Font (juce::FontOptions (12.5f, juce::Font::bold)); }
    static juce::Font textFont() { return juce::Font (juce::FontOptions (14.0f)); }
    static juce::Font noteFont() { return juce::Font (juce::FontOptions (13.5f, juce::Font::italic)); }

    /** The keys of a line laid out left to right in the keys column, relative to its top left corner. A combination (Ctrl + Shift + Z) is never split:
        when it does not fit after what is already on the line, it starts the next one. */
    static std::vector<Piece> keyPieces (const juce::String& keys)
    {
        std::vector<Piece> out;
        int x = 0, y = 0;
        const auto smallW = [] (const juce::String& s) { return juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (12.5f)), s); };
        const auto mouseW = [] (const juce::String& s) { return juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (13.0f, juce::Font::italic)), s) + 6; };
        const auto capW   = [] (const juce::String& s) { return juce::jmax (kCapH + 2, juce::GlyphArrangement::getStringWidthInt (capFont(), s) + 16); };
        for (auto& token : juce::StringArray::fromTokens (keys, " ", ""))
        {
            if (token == "or" || token == "/")
            {
                const int w = smallW (token) + 8;
                out.push_back ({ Piece::Word, token, juce::Rectangle<int> (x + 3, y, w, kCapH) });
                x += w + 6;
                continue;
            }
            auto parts = juce::StringArray::fromTokens (token, "+", "");
            int comboW = 0;
            for (int i = 0; i < parts.size(); ++i) comboW += (i > 0 ? 13 : 0) + (hotkeys::isMouseWord (parts[i]) ? mouseW (parts[i]) : capW (parts[i]));
            if (x > 0 && x + comboW > kKeysW - 8) { x = 0; y += kCapH + 4; }                     // (the whole combination onto the next line)
            for (int i = 0; i < parts.size(); ++i)
            {
                if (i > 0) { out.push_back ({ Piece::Plus, "+", juce::Rectangle<int> (x, y, 13, kCapH) }); x += 13; }
                const auto& p = parts[i];
                const bool mouse = hotkeys::isMouseWord (p);
                const int w = mouse ? mouseW (p) : capW (p);
                out.push_back ({ mouse ? Piece::Mouse : Piece::Cap, p, juce::Rectangle<int> (x, y, w, kCapH) });
                x += w;
            }
        }
        return out;
    }
    static int keysHeight (const juce::String& keys)
    {
        int h = 0;
        for (auto& p : keyPieces (keys)) h = juce::jmax (h, p.box.getBottom());
        return h;
    }
    static juce::TextLayout layoutOf (const juce::String& text, int width, bool note)
    {
        juce::AttributedString as; as.setJustification (juce::Justification::topLeft);
        as.append (text, note ? noteFont() : textFont(), note ? theme::dimText : theme::text);
        juce::TextLayout tl; tl.createLayout (as, (float) juce::jmax (40, width));
        return tl;
    }
    static void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, bool note)
    {
        auto tl = layoutOf (text, r.getWidth(), note);
        const float top = note ? (float) r.getY() : (float) r.getY() + 2.0f;             // (level with the keys' writing)
        tl.draw (g, juce::Rectangle<float> ((float) r.getX(), top, (float) r.getWidth(), tl.getHeight() + 2.0f));
    }
    bool matches (const hotkeys::Line& l) const
    {
        if (filter.isEmpty()) return true;
        for (auto& word : juce::StringArray::fromTokens (filter, " ", ""))                // every word typed must be found somewhere in the line
            if (! juce::String (l.section).containsIgnoreCase (word) && ! juce::String (l.keys).containsIgnoreCase (word) && ! juce::String (l.what).containsIgnoreCase (word))
                return false;
        return true;
    }
    void relayout()
    {
        rows.clear();
        int y = 4;
        juce::String section;
        const auto& L = hotkeys::lines();
        for (size_t i = 0; i < L.size(); ++i)
        {
            const auto& l = L[i];
            if (! matches (l)) continue;
            if (section != l.section) { section = l.section; rows.push_back ({ Row::Head, i, y, kHeadH }); y += kHeadH + 4; }
            const bool note = juce::String (l.keys).isEmpty();
            const int textW = note ? listW - 2 * kPad - 8 : listW - 2 * kPad - kKeysW;
            const int h = juce::jmax (note ? 0 : keysHeight (l.keys), (int) std::ceil (layoutOf (l.what, textW, note).getHeight()) + 2) + 2 * kRowPad;
            rows.push_back ({ Row::Line, i, y, h }); y += h;
        }
        setSize (listW, juce::jmax (minH, 80, y + 12));
        repaint();
    }
};

/** The window's contents: a box to find a key or a word, the Copy button, and the list. */
class HotKeysComponent : public juce::Component
{
public:
    HotKeysComponent()
    {
        find.setTextToShowWhenEmpty ("Find a key or a word: zoom, mark, Enter, F2 ...", theme::dimText.withAlpha (0.7f));
        find.onTextChange = [this] { list.setFilter (find.getText()); viewport.setViewPosition (0, 0); showCount(); };
        find.onEscapeKey = [this] { find.clear(); list.setFilter ({}); showCount(); list.grabKeyboardFocus(); };
        find.onReturnKey = [this] { list.grabKeyboardFocus(); };
        addAndMakeVisible (find);
        copyButton.setTooltip ("Copies the whole list as text, to paste into a document or an e-mail");
        copyButton.onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (hotkeys::asText (juce::JUCEApplication::getInstance() != nullptr ? juce::JUCEApplication::getInstance()->getApplicationVersion() : juce::String()));
            copyButton.setButtonText ("Copied");
            juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<juce::TextButton> (&copyButton)] { if (safe != nullptr) safe->setButtonText ("Copy the list"); });
        };
        addAndMakeVisible (copyButton);
        countLabel.setColour (juce::Label::textColourId, theme::dimText);
        countLabel.setFont (juce::FontOptions (12.5f));
        countLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (countLabel);
        viewport.setViewedComponent (&list, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (12);
        addAndMakeVisible (viewport);
        setSize (860, 760);
        showCount();
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (30);
        top.removeFromRight (52);                                     // (the window's pin and sun / moon buttons sit in the top right corner)
        copyButton.setBounds (top.removeFromRight (128));
        top.removeFromRight (10);
        countLabel.setBounds (top.removeFromRight (110));
        top.removeFromRight (6);
        find.setBounds (top);
        r.removeFromTop (10);
        viewport.setBounds (r);
        list.layoutFor (viewport.getMaximumVisibleWidth(), viewport.getMaximumVisibleHeight());
        showCount();
    }
    /** The keyboard goes to the list (not the Find box): the keys then work as everywhere else, and the arrows scroll. */
    void focusGained (FocusChangeType) override { list.grabKeyboardFocus(); }

private:
    void showCount()
    {
        const int n = list.count();
        countLabel.setText (find.getText().trim().isEmpty() ? juce::String (n) + " shortcuts" : juce::String (n) + (n == 1 ? " match" : " matches"), juce::dontSendNotification);
    }
    juce::TextEditor find;
    juce::TextButton copyButton { "Copy the list" };
    juce::Label countLabel;
    juce::Viewport viewport;
    HotKeysList list;
};
} // namespace td

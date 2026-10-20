# Fermata - version history

Version numbers are MAJOR.MINOR.PATCH (for example 2.1.3). The number is set in one place, the first lines of `CMakeLists.txt`, and appears on the splash screen, in the first line of `fermata-log.txt` and in the properties of `Fermata.exe`.

- **PATCH** (2.0.1, 2.0.2 ...): bug fixes and small tweaks only.
- **MINOR** (2.1.0, 2.2.0 ...): a new feature or a visible change.
- **MAJOR** (3.0.0 ...): a large change in how the program works.

## 2.77.3
- **Pyramix-style imports ("001_1-C.wav", "001_OMNI-L.wav", the first take "0_...") put every take on one set of tracks again, even when one file of a take is not the same length as the others.** Fermata first groups a take's files by length; one take whose Room pair (say) ran a second longer split in two, and that one odd take was enough for the take number not to be recognised at all - so every track name kept its take number ("001 1 C", "002 1 C" ...) and every take got tracks of its own. Now the take number in the names groups the files whatever their lengths, the "-C" that only says "mono" is treated like "-L" / "-R" (it no longer gets mistaken for part of the name, and comes off the track names: "1", "2" ... "8", OMNI, ORTF, Room, Wides as stereo tracks), and of the possible readings of a name the one where every take holds (nearly) all the tracks wins.

## 2.77.2
- **The Pro Tools import really is one set of tracks now.** The session's files are named "Ave Maria 8 ORTF.27_27.wav": the take number after the dot, and after the "_" Pro Tools' own clip number, which mostly matches the take but falls one behind from take 32 ("ORTF.32_31", "REVERB.44_43"). Fermata found the take number but left the clip number in every track name, so each take still got tracks of its own (and as the clip numbers only go to 43: 860 tracks). 2.77.1 did not catch this. Now a second number that goes with the take (the same in every file of a take, different from take to take) comes off the track names too, the take number may be anywhere in the name (start, middle or end), and the piece name every track starts with comes off for every kind of import (two words or more, never leaving a bare number). The result for this session: AB, MAIN PRINTS, ORTF, REVERB, ROOM 414, SAX SPOTS, SPOT PRINT, SPOT1 - SPOT12, WIDE ROOM, with takes 1 - 44 in order.

## 2.77.1
- **Importing a Pro Tools session: one set of tracks, every take on it.** Files named like "Ave Maria 8 AB 01.L.wav", "Ave Maria 8 SPOT1 01.wav", "Ave Maria 8 ROOM 414 01.R.wav" (piece, track, take number, and L / R for split stereo) used to give every take its own set of tracks (44 takes x 20 tracks = 860 tracks), because the import grouped the files of a take by their length and Pro Tools' files of one take are often not all the same length, and the take number was not the same word from the end in a stereo file (".L") and a mono one. Now the take number is found as the last whole number in the name (before an L / R), the files are grouped by it whatever their lengths, the takes share one set of tracks (AB, MAIN PRINTS, ORTF ... SPOT1 - SPOT12, WIDE ROOM; the L / R files become stereo tracks as before), the piece name every file starts with ("Ave Maria 8") comes off the track names, and the takes are laid out in take-number order. Names with Pro Tools' "-01" after the take number ("AB 01-01.L") work too. Every other kind of import is unchanged (this is used only when the usual grouping would leave each take with tracks of its own).

## 2.77.0
- **Denoise (take window): a background denoiser of Fermata's own.** Mark a part of one take that holds nothing but the room (mark 1 / mark 2 - at least half a second, better two or more) and press the new Denoise button: every track of the take learns its own room tone from that part (a close mic and a distant mic each in its own way) and is denoised whole, into new files beside the originals ("name (denoise).wav"; the originals are never touched, Undo fix brings them back; edits made from the take follow, unless you untick it). It is built for what the usual denoisers get wrong on classical recordings:
  - **reverberation tails are never cut off.** There is no threshold anywhere: in each band the denoiser measures how fast the room itself dies away (from the take, band by band - or set the time yourself, up to 10 s) and never lets a tail fall faster than that, so a chord dying away into a church is followed down smoothly through the noise;
  - **the top stays.** A long analysis window (85 ms) sees a sustained partial as one narrow line far above the per-bin noise, so weak upper harmonics are kept rather than taken for noise; a narrow peak that persists is protected outright; and a ceiling on how far the highs may be turned down can be set if ever needed;
  - **a little room tone is left in** (the Residual, -12 dB by default: total silence sounds unnatural), and where nothing but room tone is heard, exactly that much is taken out;
  - **the tracks help each other.** A stereo-file denoiser only ever hears the mix; this one has the whole multitrack. Every track works out, for each moment and frequency, how likely it is to hold music rather than noise, and each track is then given what the *other* tracks hear: where a spot mic clearly has a quiet partial or a soft entry, the distant mics keep that sound instead of taking it for noise. "Other tracks: help keep the music" is the default; "... and decide what is noise" also treats a sound no other track hears (a rumble, the audience) as noise - opt in, because a source only one mic hears (an organ without a spot mic) would go with it.
  - **Preview before anything is written:** a stretch (10 - 60 s, after the room tone or from the take window's playhead) is made ready and you switch, while it plays, between the original, the denoised sound and *what was removed* (it carries on from where it is) - if the music is in what was removed, it is taking too much. Amount (how much louder than measured the room tone is taken to be), Residual, Tail and the other-tracks setting are remembered. The room's decay is measured once, when the room tone is learned, so the preview and the whole take agree on the tails.
  The engine's reference implementation and its test bench are in `tools/denoise_reference/` (Python); the C++ engine is checked against it by the core tests.
- The 2.76.1 zip still said 2.76.0 in CMakeLists.txt (the program's version string); nothing else was wrong with it. 2.77.0 says 2.77.0.

## 2.76.1
- **Builds again on Windows.** 2.76.0 did not compile: the main window had a variable called `small`, and since 2.75.0 it includes the file-sync header, whose `windows.h` brings in a Windows type macro of that name ("'int' followed by 'char' is illegal"). The header now includes a lean `windows.h` and drops the macros (`small`, `near`, `far`, `hyper`, `min`, `max`) that are ordinary words elsewhere; the few variables with such names in Fermata are renamed; and the development check (`tools/check-windows-code.sh`) now flags any identifier that is a Windows macro name, so this cannot come back. No change in what the program does.

## 2.76.0
- **Missing audio files are found again.** When a project opens with audio files it cannot find, Fermata says which (takes, tracks, edits) and offers to look for them; "Find missing audio files..." (Takes and Edits menus) does the same at any time (it checks every file first and says so when nothing is missing). Point it at a folder: it searches that folder and every folder inside it, first by file name - a file of the same name whose length, sample rate or channels are not the project's is not taken (Fermata's file names repeat from project to project) - then, for a file that was **renamed**, by what the project knows about it: its length in samples, sample rate, number of channels and, for Fermata's own recordings, 24-bit depth. A renamed file is taken only when exactly one file in the folder fits; as the files of one take are all alike, the one meant is picked by the window, take and track names Fermata writes into its recordings' headers, by their recording time, or by the track name in the file name ("Violin 2" is never taken for "Violin"); a file with no header (exported by another program) is never ruled out by that silence, only a file whose header says it is another take or track is. A file the project already has is never taken for a missing one. When that still leaves several, nothing is guessed: the report names them and says to give the right one its old name. Every place that used a file (takes, edit pieces, overdubs) is pointed at the file found, the project is saved, and the result lists what was found, how, what fits several files and what is still missing (every file is in the log). Ctrl+Z does not undo a relink (the save before it is the project's .bak). A "please wait" window shows the search. A take window imported from another project is checked afresh at the next open, and the import says how many of its own files are missing.
- **The keys never go dead again.** After Enter in a name box (the take window's, the edit's, a VCA's or a mixer's), or after a pop-up closed, nothing in the window held the keyboard, and the keys then went to the window frame, which dropped them: Space to play, the arrows, 1 / 2 / 3 and the rest did nothing until something in the window was clicked - and clicking its title bar did not help, because the window was active already. R and Space-to-stop still worked (they are watched separately), which made it look like a record / play lockup. Now the window's contents take the keyboard back at once, the keys reach them even while nothing has it, and Enter in the take and edit windows' name boxes hands the keyboard straight back to the timeline. Every window's contents also take the keyboard themselves now, so activating a window (a click on its title bar) never gives the keys to its first text box instead - in the mixer that was the mixer's name box, and in the main window an input's name: R and Space were "typing" from then on. Tab pressed with no box in use no longer moves the keyboard into the window's first text box either. None of this touches a plug-in's own window or a driver's panel: while one of those has the keyboard, Fermata's windows leave it there.
- **Shift + Left / Right nudges the take window's playhead by 10 ms,** as it always said it would: the key never arrived (a key with Shift held was taken for a different key).
- **Mixer window width:** dragging its edge can make it as wide as all its strips (across two displays, say); it keeps that width until it is closed, and opens within one display next time. (2.75.0 limited it to the display altogether.)

## 2.75.0
From the list sent after testing 2.74.
- **Take window head:** the marks line ("EDIT 002 - Set A Seal: Mark 1 ... Mark 2 ... (176.42 s)", or the playhead) now sits on the same line as the "To edit goes into" box, under Next Take, REC and the readouts; the line it had to itself is timeline again. A recording problem shows in the same place while there is one.
- **TAKE LENGTH** is a readout of its own beside the time readout: the same fixed digit cells and smooth 30-times-a-second count as the main readout (it no longer jiggles), in a grey box with softer digits (a soft red while recording) and dashes while nothing records in that window.
- **Rulers and marks read MM:SS under an hour and HH:MM:SS from an hour on** (1 min 15 s = 01:15; 1 h 1 min 15 s = 01:01:15) in the take, edit and trim windows, the process windows and the volume dialog. They used to start again from 00:00 after an hour.
- **Space plays the take or edit window you were last in, from any window that has no player of its own:** the main window, a mixer, the Project Designer, a plug-in window, the Take Display... even a window that is down in the bar. If that window has been closed, the one before it. A take or edit window in front plays itself; the Mastering, Trim and process windows keep their own Space.
- **Main window header:** project name, rate and depth on the top row with the free disk space under them; the ASIO line on two rows (alerts and driver, then buffer, ins / outs, xruns and DSP); DRIVER INPUTS directly under the Fermata logo; a thin line between the logo and the project boxes, and between those and the ASIO line.
- **Take Display:** the project line is centred on the screen; when it would reach the logo it starts just right of the logo instead.
- **The edit window's Reference waveform box and the take window's "To edit goes into" box change only when clicked:** they no longer take the arrow keys, which stay with the window (zoom, track heights).
- **Mixer window:**
  - never opens wider than its display, nor so tall that its bottom edge (and scroll bar) is below the screen: strips that do not fit scroll, with the scroll bar there from the moment it opens. It used to grow to the full width of its strips by itself, off the edge of the screen.
  - remembers its size and place (it used to open at the size that shows every strip each time). A window that shows every strip keeps doing so as strips are added.
  - the mouse wheel scrolls the strips left and right (Shift + wheel: up and down, when the window is shorter than the strips; when every strip fits across, the plain wheel scrolls up and down). No fader, pan or send dial in a mixer moves with the wheel any more.
  - changing the strip width with - / + keeps the METERS strip where it is: the window keeps its right edge and the view no longer jumps back to channel 1.
- **Sends:**
  - a **MUTE** button in the send's panel (click a send dial): lit red when the send is muted (its level is kept), with a red outline when the channel's own MUTE is silencing it (Follow mute ticked). On the strip a muted send's dial goes grey and its read-out says MUTE; a send silenced by the channel's mute has a red outline round its read-out. A muted send keeps nothing open under a solo. Saved with the project (written so that an older Fermata opens the send silent, not at its level) and copied with a mix's sends.
  - a send taken before the fader is shown by a yellow square round its dial; the "(pre)" after its name is gone.
- **Alt + drag a plug-in onto another insert slot to drop an exact copy** with all its settings (Shift does the same); a plain drag still moves it.
- **Plug-ins window** (Audio settings > Plug-ins..., or the menu of any insert slot): every VST3 plug-in file Fermata has found, with the plug-ins in it, and a search box. **Ignore** one and it is never scanned, loaded or offered again (a project that uses it opens with that slot empty and its settings kept; a copy already loaded when you ignore it keeps working, bounces included, until the project is opened again); **Use again** brings it back (its file is scanned again). A file marked **CRASHED** - it crashed Fermata while it was being scanned, so it was left out (shown as soon as Fermata starts again) - can be **tried again** (the project is saved first; if it crashes again it is left out again). Ignoring works on files: a file that holds several plug-ins (a shell) is ignored with all of them. The list is kept on this computer (`plugins-ignored.txt` beside `plugins.xml`).
- **A project's .bak can be opened.** Open project also lists `Name.fermata.bak` (the project as saved the time before). Fermata asks first, keeps the project file that is there now beside it as "Name (replaced <date> <time>).fermata.bak" (nothing is deleted; opening that copy the same way goes back), puts the .bak back as the project file and opens it. The take windows, edits and mixers have files of their own, which are not changed: they open as they are now. A window file's own .bak (`.fmtake.bak`, `.fmedit.bak`) can be imported like the window file (Takes / Edits menu).
- **Lasso: Alt + drag** in the take window or the edit window draws a box, and every take (or piece, overdubs too) it touches is selected, whole. Alt + click selects what is under the mouse. **Marks 1 - 2 on chosen tracks only is now Ctrl + Alt + drag** (it was Alt + drag). Alt while dragging an automation point still gives fine steps, and with automation showing in the edit window, a drag works on the automation as before.
- **Modifier hints:** while Ctrl, Alt, Shift or a mix of them is held, the box in the top left corner of the take and edit windows (above the first track's name) says what they do there (in the edit window, what they do to the automation while it is showing).

## 2.74.1
- **A new project starts with one Ext Bus, "Main",** ticked as the processing mixer's main output and sent to the audio driver's outputs 1-2, so it is heard straight away (it used to start with no buses at all). Projects that already exist are not changed.

## 2.74.0
- **The Neumann MT48 gets its own preamp controls** (it was treated as an Anubis). Inputs 1-2: MIC / LINE, 48V, polarity, a **three-position PAD** and a **four-position low cut**; inputs 3-4: LINE / INSTR, polarity and the same low cut. Click **PAD** to step OFF -> -12 -> -24 -> OFF: as on the MT48's own page it moves the gain's scale (OFF 12 to 78 dB, -12 dB 0 to 66 dB, -24 dB -12 to 54 dB) and the gain knob stays where it is; the readout and a typed gain follow the scale. In Line mode the Pad is not shown. Click **LC** to step OFF -> 40 Hz -> 60 Hz -> 80 Hz -> OFF. No BOOST or Z HI key on the MT48 (its "lift" switch is part of the Pad). The low-cut frequency is kept in the project and the preamp memory. The Anubis and the Hapis are unchanged.
- `tools/watch_device.py` also saves the device's full settings and every change to files you can send back (`<address>-settings.json`, `<address>-changes.txt`).

## 2.73.0
- **Switching Loop off while it plays carries straight on past mark 2** (take window and edit window). Play never stops at mark 2: Loop sends the playback back to mark 1 each time round, and switching it off lets the sound run on through mark 2 and keep playing, as Play would - with no gap and no jump back, even in the last moment before mark 2. (It used to finish the pass and stop at mark 2.) The take window's loop now plays on the window's own timeline, like Play, so after the loop is let go of it carries on through the window. The process windows' auditions keep their Loop as it was (switched off, the pass finishes and the audition stops).

## 2.72.0
- **Automation lands on the music under it, also behind plug-ins that delay the sound.** A plug-in with look-ahead or linear phase holds the sound back (Fermata lines every path up again at the outputs), so a fader, pan, send or plug-in move drawn on a strip after it used to happen that much early: 50 ms of look-ahead, 50 ms early. Each lane is now read as late as the sound is at the point where that control acts - input gain before the strip's plug-ins, fader and pan after them, a plug-in's own parameter after the plug-ins before it, a send's level where it joins its bus - in playback, bounces and masters alike, following loops. A VCA lane is read for each channel it controls at that channel's own lateness, so it lands on time on all of them even when only some sit behind a delaying plug-in; adding a channel to a VCA or taking it out is taken into account at once.
- **A send faded up from off by its automation is now heard.** A send was only connected when it was on at the moment the mixer was last set up, so a send-level lane that brought a send in from off did nothing. Sends driven by the edit's automation are now kept connected while it plays (silent while their level is off; they keep nothing open under a solo).
- Documentation brought up to date (the light look is the starting look; known limits; the R key; the name of "Create copy of entire project...").

## 2.71.0
From a review of the plug-in hosting and of playback, bounce and mastering. Nothing changes in how the program is used.
- **A bypassed plug-in keeps hearing the music.** It is run on a copy of the sound and the copy thrown away, so its reverb tail, its look-ahead and its meters stay in step, and switching Bypass off again is clean. Until now a bypassed plug-in was left out altogether, so the first moment after Bypass was switched off played whatever had been sitting in it (a burst of old reverb).
- **A plug-in that gets stuck cannot hang the program.** Loading, removing or moving a plug-in waits for the audio to let go of its slot; if a plug-in is stuck inside its process call that wait used to be for ever (the program froze with the audio). Now it gives up after two seconds, leaves the slot as it was and writes a line to the log. The audio itself never waits for a slot.
- **Plug-ins and mixers that are no longer used are freed.** A deleted cue mixer, the mixer of a deleted edit, and all the mixers of a project that was closed used to stay in memory with every plug-in they held until the program was closed (a long session opening project after project kept every one of their plug-ins alive). They are now freed, with their windows closed first, once the audio thread has certainly moved on from them and nothing is playing. A stall of the audio thread (the callback not coming round for half a second while something is taken away from it) keeps everything from then on in memory, as before - safe over tidy.
- **A plug-in that crashes the scan is left out next time.** The scan keeps a note of the plug-in it is looking at; one that takes the program down is found there at the next scan and skipped, with a line in the log (delete `plugins.xml` to try it again). The scan also logs the plug-ins it could not load. Plug-in windows are closed before a project is switched, and a plug-in window whose close button is pressed just as the program closes it for its own reasons (a project being switched, its mixer deleted) can no longer touch a window that is already gone.
- **Playback and bounces open each audio file as they reach it and close it behind them.** Pressing Play on an edit used to open every file of every piece first, each with a read buffer (an edit of thousands of pieces: thousands of open files before the first sound). A missing file is still reported before anything plays; a file at the wrong sample rate is refused from what the project knows about it, without opening it, and one the project got wrong is found out when it is reached (that piece is silent and the log says so; a bounce stops and names it).
- **A bounce or master of a whole edit covers its first fade-in and last fade-out even where they reach outside the first and last piece.** When the first piece's fade-in had been dragged to before the piece (or the last one's fade-out to after it) the rendered file began at the piece, cutting the start of the fade off; the mastering length, the CD track offsets and the listening copies' time scale now match what playing the edit plays.

## 2.70.0
The screen and opening, kept quick on a big project (measured on a test project of 50 pieces x 60 takes x 20 stereo tracks, 3000 takes, 50 edits of 40 pieces). Nothing changes in how the program is used.
- **The take window and the edit window draw only what is on the screen.** Takes and pieces to the left or right of the view, and rows above or below it, are skipped. Before, every take of the window was worked through for every repaint - and the input meters in the name column repaint 30 times a second, so a window with 60 takes of 20 tracks kept a whole processor core busy doing nothing visible (each of those repaints took 30-45 ms; now 2 ms). A full redraw of the window went from ~64 ms to ~18 ms.
- **While recording, only the growing take's columns are redrawn,** not the whole window, 30 times a second.
- **A change to the project no longer lays out and repaints the whole main window.** Every click that changed anything cost ~12 ms of layout plus a full repaint of the 40 input strips; now the main window is laid out again only when the stage speaker panel changed. The change handlers of the open windows went from ~14 ms per change to under 1 ms.
- **Input and mixer meters that show the same picture are not repainted** (an idle input used to be repainted 30 times a second; forty of them on a big rig).
- **The waveform outline is left off bands under 10 px tall** (it was the whole picture there, and two thirds of the drawing work of a window full of small rows).
- **Opening a big project:** the undo snapshots at open are copies of the windows (18 ms) instead of their text (600 ms); a take whose files were found whole is marked **verified** and saved so, and the next open does not ask the disk about it (3000 takes of 20 files were 60,000 questions, half a second on a quick drive and more on Windows) - unless the project has been moved or copied since, when every take is looked at again; the take and piece data is read with ready-made keys (a third off the "set up" time). The "Opened ... in N ms" log line says where the time went: files read, recordings checked, audio and undo ready.
- Developer: `FERMATA_PAINT_PROFILE=1` in the environment makes the take / edit / main windows write a breakdown of their paint and change-handler times to the log (`src/ui/PaintProfile.h`); the render harness has a `bench.sh` that opens a window on a project and times its paints.

## 2.69.0
Undo and saving made to stay quick on a big project (many pieces, thousands of takes). Nothing changes in how they are used.
- **Undo keeps a snapshot per window, not of the whole project.** Half a second after a change, only the take window, edit or mastering list that changed is written out again (a cheap hash of each window says which); it used to be the entire project as one text after every change, which on 3000 takes took about a third of a second each time and kept 30 copies of it in memory. Ctrl+Z / Ctrl+Y behave exactly as before: one step back across the whole project, changes made together undone together.
- **The automatic save only serialises the windows that changed.** The same hashes decide which window files need writing; an unchanged window costs nothing. On the 3000-take test project a save went from about 300 ms to about 25 ms. Opening a project no longer makes the first save rewrite every window file either.
- (2.68.1's `.bak` copies apply to these files as before.)

## 2.68.1
Saving and opening, made to survive a stop at the wrong moment (the program crashing or the computer losing power while a project is being written). Nothing changes in how the program is used.
- **Every save keeps the one before it:** the project file and each take-window, edit and mixer file are written beside a `.bak` of the version they replace (`Session.fermata.bak`, `Take Windows/Piece 1.fmtake.bak` ...). The bytes are pushed to the drive before the new file takes the old one's place, so a power cut right after a save cannot leave an empty or half-written file.
- **A damaged or missing file is mended from its `.bak` when the project opens:** the save before is used, the unreadable file is kept beside it as `.damaged`, and a note says what happened and what may be missing (anything changed after that save). This applies to the project file and to every window file; a window whose file and `.bak` are both gone is still listed as missing, as before.
- **A save that is interrupted leaves a folder that still opens:** the old window files are tidied away (renamed ones deleted, removed ones moved to `_removed`) only after the project file that stops listing them has been written. Until 2.68.0 they went first, so a stop in between made the project say a window was "missing its file".
- **A new project no longer inherits the missing-files list of the project that was open before it** (it could show a "Files missing" message about files that were never its own).
- **"Create copy of entire project..." and "Save as..." leave the `.bak` and `.damaged` files behind** (the copy is written afresh).
- The "Files missing" message is now "Project files" and explains the `.bak` case too.

## 2.68.0
- **Space stops the recording and R starts one from any Fermata window,** whatever has the keyboard: a mixer, a plug-in window, the main window, a dialog, even with a button just clicked. Not while you are typing in a text box, with Shift / Ctrl / Alt held, or when another program is in front. R records into the take window that is in front, or the take window that was in front last. In the edit window, the process windows and plug-in windows R keeps its own job (butt / audition / do the fix / the plug-in's); Space still stops a recording there.
- **VCA ghost line on the faders:** a strip controlled by a VCA shows, beside its fader, a line in the VCA's colour from the fader cap to where the level really is, with a faint ghost cap there (VCA down 6 dB: the line runs 6 dB down the scale). A muted VCA draws it to the bottom. Int Buses controlled by a VCA show it too.
- **Enter confirms a VCA's name and leaves the box** (it used to need a click outside). The same for the mixer's name, the gap boxes and the take display's timer, so Space and R work again straight away. Esc in the VCA name puts the old name back.
- **The VCA strip lines up with the other strips:** M / S, the fader, its meter and the level number are at the same height as on the track and bus strips.
- **Changing the strip width keeps the meter strip (with - / +) where it is:** the mixer window grows or shrinks to the left, and a scrolled mixer keeps the same distance from its right end, so - / + can be pressed again without scrolling. The window is never made wider than the screen.
- **The strips' rounded bottoms are no longer hidden under the mixer's scroll bar:** when a scroll bar is needed the strips stop above it.
- **The take window's Play marked button is gone.** Play never stops at mark 2; Loop (L) plays the marked area round and round.
- **The take window's name column moved up a row:** "Piece / Take Window Name:" starts right at the top of the window, so the name box sits level with the Next Take box and the whole head is a little shorter (more room for the tracks).
- **Switching Pre-Rec on no longer holds the program up.** The memory that keeps the last seconds of every input (about 120 MB for 40 inputs at 96 kHz) is now made on a background thread and put in place a moment later; it used to be made on the screen's thread, which stalled the program the first time Pre-Rec was switched on (and whenever a track was patched to a new input while it was on).
- From the first rig test of 2.67.1 (20 stereo tracks at 96 kHz, 4 takes): no overruns, the longest single disk write 3.4 ms, the recording buffer never more than 5.3% full, files finished 15-17 ms after Stop.

## 2.67.1
Fixes from the first test on the real rig (40 mono tracks at 24-bit / 96 kHz on the internal NVMe): a disk overrun about 40 s into the first take, and the program hanging for about 10 s after Stop.

- **Recordings are written to the disk in big pieces.** Each file is now written in pieces of about half a second of audio (at most 65536 frames), not every few milliseconds. The amount written is tiny for any modern drive (40 tracks at 96 kHz is 11.5 MB/s); the number of writes was the problem: about 2500 small writes a second, each making a file longer, more than one disk thread could keep up with on Windows. Now it is about 80 a second. (2.66 had made it worse by turning off a small buffer that used to join some of them.)
- **Stop is instant.** The files of a stopped take are finished (what is left in their buffers written, then their headers) on a background thread, so the program answers at once and the next take can be recorded straight away. While its files are being finished the take keeps its live waveform; then the real waveform is drawn. Play on a take that is still being finished waits only until it is done. The copies on the second recording drive are finished once the main files are closed.
- **Redrawing a window no longer opens audio files** unless it is zoomed in far enough to need the samples themselves (opening a file that has only just been recorded can wait on Windows' virus scan).
- **A line per take in `fermata-log.txt`:** length, number of files, MB/s, the folder and the kind of drive, free space, the longest single disk write, how full the recording buffer got (about 6% is normal: half a second of 8), any overruns and when the first one happened, the second drive, and how long after Stop the files were finished.
- Tests: the writer waits for a big piece, Stop returns before the files are finished and the take's live waveform is still there until they are.

## 2.67.0
The speed and stability items left from the 2.66 review, and a load meter so they can be measured on the real PC.

- **Load meter.** The main window's status line shows how hard the audio thread works: **DSP 23% (peak 61%)** = the share of each block's time it needed over the last second (100% = the PC only just keeps up). Blocks that took longer than they last are shown as **LATE BLOCKS: n** (a dropout is then likely).
- **Timing log.** Every minute while the audio runs, and at the end of every take, `fermata-log.txt` gets one line: average and peak load, late blocks, buffer size and sample rate, the driver's xrun count, the number of mixer helpers, how full the recording buffers got, playback underruns and the reserved processors. Send these lines after a test session.
- **Playback reads in big pieces.** Every file played (takes, edits, the trim window) is read through a buffer of up to 1 MB, instead of small reads every few thousand samples: far fewer calls into Windows and much kinder to a hard drive. An edit with thousands of pieces shares 96 MB between them (never less than 32 KB each).
- **The disk writer and the playback reader no longer run on the reserved CPU cores.** Those cores are now kept for the work with a deadline every block (the audio callback and the mixer helpers); the disk threads work seconds ahead, so they run on the other cores at high priority, out of the way.
- **Automatic saves no longer ask every plug-in for its whole state each time.** A plug-in hands over the state it gave last time unless it has reported a change since, its window is open, or a minute has passed. Saving from the menu, closing and Save and quit still ask every plug-in afresh. (A big convolution reverb can take a noticeable moment to answer; the automatic save asked every few seconds.) Recording start and stop use the quick save too, so pressing Record never waits on the plug-ins.
- Tests: the read-buffer budget, the load meter, quick versus full saves.

## 2.66.0
The audio-thread and recording review of 2.65 (every finding checked against the code, the engine run under the thread, address and undefined-behaviour sanitizers, plus a new stress test), and its fixes. Nothing new to learn: the program records and plays as before, it just tells you when something is wrong and loses nothing it can keep.

- **A full or failing recording drive is no longer silent.** The engine's own recording writer (`TakeWriter`) replaces the JUCE one, which threw away the result of every disk write: when the drive filled up or dropped out, the file simply stopped growing while "RECORDING" stayed on. Now the main window's status line and the take window say **RECORDING HAS STOPPED REACHING THE DISK**, flashing, with a notice; the file keeps a good header for what did reach the disk, and the take is marked **CUT SHORT** in red.
- **How long the drive will last.** While recording, the DISK FREE box shows the time left at the current rate ("812 GB  4 h 12 min left"), red under 15 minutes, and the status line says **DISK NEARLY FULL** under 5 minutes.
- **A disk overrun no longer moves the take in time.** When the disk falls more than the buffer behind, the lost block is written later as **silence, in its place**, so the file keeps its length and stays in time with the other tracks and the timeline (it used to be left out, shifting everything after it earlier, on some tracks and not others). The status line says how much was lost; the take carries **LOST 0.3 s** in orange afterwards, and the count is saved with the project.
- **The recording buffers hold 8 seconds whatever the sample rate** (they were a fixed number of samples: 5.5 s at 48 kHz but only 1.4 s at 192 kHz), so the disk may stall that long before anything is lost.
- **Recordings are bit-exact.** JUCE's float-to-24-bit step wrote positive samples above -6 dBFS one step low (a quarter of all possible words); the words now go to the file exactly as the converter made them. (Inaudible at -138 dBFS, but a null test against the source passes now.)
- **The header is refreshed every 10 seconds while recording**, so a file cut short by a crash or a power cut is readable at once; opening the project still mends it to its full length.
- **Crash repair of files over 4 GB** (two hours of stereo at 96 kHz, 41 minutes of a six-channel track) wrote a wrapped-round size, so the take read back as if cut to a fraction of its length. It is now given an RF64 header (64-bit sizes) in the space the writer keeps free for it, and reads back whole.
- **Mixer helper threads are safer.** They now join Windows' "Pro Audio" scheduling class whether or not CPU cores are reserved (outside it, any busy program could pre-empt a helper in the middle of a block while the audio thread waited for it), and with cores reserved no more helpers take part than the reserved processors can run beside the audio thread.
- **The mixer window no longer locks a plug-in slot to read its name.** It did so three times a second for every slot; each time the audio thread met that lock, the plug-in was skipped for one block and the sound went past it dry.
- Small: the latency hash used a signed multiply that overflowed (undefined behaviour in principle); Stop while the audio callback has hung no longer frees the recording under it; a block far bigger than the device announced is counted as an overrun instead of vanishing; the playback ring no longer shares a juce::AudioBuffer flag between its two threads; the helper-thread count can no longer be changed while the device runs.
- **Tests:** the new writer (every 24-bit word round-trips, a dropped block becomes silence in its place, a failing drive is reported and the header stays good, the header refresh), the RF64 repair (with a real 4.4 GB sparse file), the helper limit, and a concurrency stress test (`tests/stress_test.cpp`, a pretend audio thread against a pretend user) that `tools/sandbox-check.sh` now runs.

## 2.65.0
- **Mixer: "Main out" is now a tick box on every Ext Bus, and several can be ticked at once.** A strip whose output is set to **Main** goes to every Ext Bus ticked as Main out, so the main mix can feed several outputs. A ticked Ext Bus also plays whatever is sent to it (a send, or a strip's output set to that bus): for example a performer's feed that is the main mix plus more of themselves, without making a cue mix for them.
  - **Nothing ticked means Main goes nowhere.** Until now an unticked mixer still sent Main to the first Ext Bus, which was confusing. (Part of that was a display fault: the Main out box on an Ext Bus strip was never updated, so it always looked unticked even where Main was going.)
  - **A new mixer starts with its first Ext Bus ticked**, and so does a mixer that gets its first Ext Bus. An Ext Bus added later starts unticked.
  - **Projects made before 2.65** open with the bus that was their main output ticked (or their first Ext Bus, which is where Main went).
  - The strip's output box tooltip names the buses "Main" goes to.
- **Cue mixers: "Follow Processing Mixer".** A tick box at the top of every cue mixer. Ticked, every Ext Bus output of that mixer plays exactly what the processing mixer sends to its main output (plug-ins, automation and all; while an edit plays through its own mixer, that mixer's main output). The cue mixer's own mix is not changed: it rests, kept as it was, under a cover saying it is following, and comes back the moment you untick the box. Saved with the project. (If you audition a following cue mixer, you hear the main mix on your own outputs too.)

## 2.64.4
- **Take window, the column at the top left moved up and closed up:** "Piece / Take Window Name:" now starts at the very top (level with the top of the Next Take box), with the name box right under it, then the line, "To edit goes into:" and its box, all as close together as they can be.
- **The take's length (--:--:--.---) is only as wide as its digits and sits right against the time readout**, so the readout and the buttons beside it move left and the buttons get more room on the top line.

## 2.64.3
- **Fixed: the mixer strip width (- / + under STRIP WIDTH) now changes the strips at once**, in every open mixer, and the mixer window takes the new width straight away. The strips were rebuilt, but each kept the width of the strip it replaced, so the change only showed after closing and reopening the mixer.
- **Take window top bar, rearranged:** on the left, one column: "Piece / Take Window Name:", the name box, a thin line, "To edit goes into:" and its box (exactly as wide as the name box). Then Next Take, REC, the take's length and the time readout. **All the picture buttons sit to the right of the time readout**, and when they don't fit on one line the rest goes on a second line under them (the two lines are as tall as the readout), never under the name or the readout.
- **Edit window top bar, the same way:** the name, the time readout, and the buttons to the right of the readout on as many lines as they need.
- The buttons of both windows stop short of the pin and sun / moon buttons in the top-right corner.

## 2.64.2
- **Take and edit windows, the name of the window:** the caption now reads **Piece / Take Window Name:** (in the edit window **Piece / Edit Window Name:**) and sits above the name box, so the two together are as tall as the Next Take box (and the edit window's top bar gained the room the old "Edit:" caption took).
- **The record button is the same size and shape as the Next Take box** beside it: "REC [R]" across the top and a big red dot under it (a square while recording, with "STOP [Space]").

## 2.64.1
- **The time readout moved from the middle of the top bar to the left:** in the take window it sits just after the take's length (the "--:--:--.---" beside the record button), in the edit window just after the edit's name. All the room to its right is for the buttons again, so on a wide screen they fit on one line (or two in the take window). When the bar does spill onto another line, a group of buttons (the transport, say) now moves down together instead of being split.

## 2.64.0
- **A big time readout in the middle of the top bar of the take and edit windows**, as in other DAWs. It shows where that window's playhead is, and moves with the music while the window plays (in the take window, while it records too). **White** when stopped, **light blue** while anything is playing, **red** while anything is recording.
  - **How the time is written:** click the small label above the digits to go through the choices, or right-click for the menu: hours : minutes : seconds . ms, minutes : seconds . ms (handy for the length of a piece), **timecode** (25, 24, 29.97 drop-frame, 29.97, 30 or 23.976 fps, with the timecode at the start of the timeline, e.g. 01:00:00:00, set in the same menu) or **samples**. Every readout follows the same choice, and it is remembered on this computer.
  - **Double-click it to go to a time:** type 1:30, 1:02:03.5, 2m10s or 90 (in Timecode also 01:02:03:04, in Samples a sample number) and press Enter; the playhead goes there (and carries on playing from there if it was playing). Something it can't read makes the outline flash red. The right-click menu can also copy the time.
  - The digits stay still as they change (each has its own space), and hours and minutes that haven't started yet are faint.
  - (2.64.1: moved to the left of the bar, see above.)
  - (The small "--:--:--.---" next to the record button is unchanged: it is how long the take being recorded in that window is so far, and shows dashes when nothing is recording there.)
- **Fixed: a plug-in loaded with a project did nothing** (audio went past it: no reverb, nothing on the plug-in's meters, while the strip itself passed the sound). When a project was opened (or reloaded) while the audio interface was already running, its plug-ins were prepared for 48 kHz and blocks of 512 samples whatever the interface really ran at, and since 2.62 a plug-in handed a bigger block than it was prepared for was skipped. Plug-ins are now prepared for what the interface really runs at, and a bigger block than expected goes through the plug-in in pieces instead of past it. (At a sample rate other than 48 kHz this also puts right a plug-in that was running at the wrong rate after opening a project, such as a reverb sounding shorter or longer than it should.)
- **Sends: Follow mute.** Click a send dial: under PRE / POST there is now a **Follow mute** tick box. Ticked (as every send has been until now, and still is by default): muting the channel mutes this send too, pre or post fader. Not ticked: the send carries on when the channel is muted (a post-fader one still follows the fader; a solo on another channel still silences it). A send that does not follow mute has a * after its name on the strip. Saved with the project.
- **Space works while a plug-in's window is in front.** It plays / stops the take or edit window you last used, so you no longer have to click back into that window (and lose the plug-in behind it). Held down it acts once. (A plug-in that has its own text box, a preset name say, will still see the space too.)
- **Take window: In flags are drawn over Out flags**, so the start of a piece is never hidden under the end of the one before. **The crossfade-only flags are small and grey**: "I fx" / "O fx" in a grey tag just big enough for the text.
- **Mixer strips have rounded corners at the bottom too** (the coloured name plate at the bottom used to cover them).
- **Main window: naming an input or an output** - Enter, or a click anywhere outside the box, now finishes the typing (it used to keep the box in editing). Escape puts the old name back.
- **Re-HarmoniSer: "One voice only".** A tick box in Note ReShape (beside Advanced). Tick it when only one singer or instrument is on the note (a solo): nothing is then looked for beside the note, so it can never be taken for two voices with half of it left where it was, and the whole note, all its overtones and the room's ring-out move together. It stays ticked for the next note until you untick it; ticking or unticking it with a note boxed measures the note again. Leave it off for a choir or ensemble.
- **Re-HarmoniSer: a note on its own no longer leaves its upper overtones behind** (the main cause of the "two singers a little apart" sound). Only the first ten overtones were moved, so everything above them (for an Eb above middle C: above about 3.1 kHz, which is where a voice's ring is) stayed at the old pitch and sounded like a second singer close to the first. With "One voice only", every overtone up to 8 kHz now moves (on a test voice, what is left of overtones 12-22 at the old pitch went from all of it to 45 dB down). A note found to be alone without the tick box has its overtones moved up to where their bands would start to fill the space between overtones (so another voice's or a piano's upper overtones sounding there are not moved with it); the bands of the overtones never overlap.

## 2.63.1
- **Re-HarmoniSer: no glitch at the end of a legato note.** After the box the correction used to hold for 0.25 s and then fade over 0.35 s, so when the singer went straight on to the next note (legato) the glide between the notes passed through the band and part of it was moved: a glitch where the notes join (worse in 2.63.0 than before, because its bands are wider). The engine now notices the voice leaving the note's pitch while it goes on singing, and fades the correction out as the voice leaves (within about 50-80 ms). When the note is followed by a rest the room's ring-out is still followed as in 2.63.0.

## 2.63.0
- **Re-HarmoniSer: a voice on its own is retuned cleanly.** Tuned on your solo vocal clip (the sharp Eb, the flat F, the wavering Eb before 9 s) on all three mics. When no other voice is found beside the boxed note, the band taken round each overtone is now flat and as wide as the note's own wobble (plus a margin), scaled with the overtone, instead of a narrow bell that let a third of the sound through: the room's ring-out, which is spread round the OLD pitch (it rings what the singer sang a moment ago), now moves with the note instead of being left behind as a ghost under it. On your clip what was left at the old pitch fell from 8-30 dB down (worst on the room mics) to 35-60 dB down. **The ring-out after the note is corrected too:** the correction now follows the room's decay after the box (up to 1.5 s, stopping early when it has died away or something new starts on that pitch), so the tail no longer rings at the old pitch. A note that is NOT alone (other voices found beside it) is handled exactly as in 2.56. A backstop: before the wide band is used, the sound round the note is looked at; where something else sits close by that the finder did not take for a voice, the band stays close to the note. The note info now says "On its own" or "n voices beside it".
- **Re-HarmoniSer: "Keep wobble" no longer puts the note out of tune.** The wobble was measured from a plain smoothing of the sung pitch, which lagged at the ends of the note and was pulled down by the scoop a note often starts with; the wobble that was left had an offset in it, and keeping part of it kept part of the offset (on your sharp Eb, Snap 100 % with Keep wobble 40 % came out +15 cents). The pitch is now a trend fitted through the note (the scoop hardly counts), so the kept wobble is only the wobble: the same setting now lands within 3 cents.
- **Re-HarmoniSer: the box has handles again.** With a note measured, drag a side of the box to make the selection longer or shorter (or the top / bottom for its pitch range, a corner for both): it re-measures. The mouse pointer shows which edge it is on.
- **Re-HarmoniSer, Section ReFinement: the voices are moved with the joint fit** (the one the note correction uses when voices are found beside the note): all the found voices are followed and fitted together, window by window, and only the ticked ones are moved. A narrow band per voice (as before) could not hold a voice's vibrato without catching the voice beside it. On the bench the old pitch left behind a moved voice fell by 2-5 dB and the voice beside it is touched less. Known limit (unchanged): Find voices still sees a voice with a BIG vibrato as two or three voices (one at each end of its swing).

## 2.62.0
- **Plug-ins are told what the transport is doing.** Every plug-in in every mixer now knows whether Fermata is playing or recording, where on the timeline it is, and the sample rate (as in other hosts). Plug-ins that listen while the host plays and work when it stops, such as **Auto-Align 2** (the plain VST3 version), can now capture and analyse: press Align, play the passage (30 seconds or more), press Stop. A bounce or mastering render counts as playing too, so a fast offline pass captures the whole piece. When nothing plays (live input only) the plug-ins are told the transport is stopped. It costs next to nothing (a few numbers per block).
- **Plug-ins are set up as stereo the proper VST3 way** (instead of being told the channel count informally). A plug-in with a side-chain input gets a silent side-chain; a plug-in that only works in mono has its output put on both sides.
- **Mixer faders and pans, quicker to reset and to type:**
  - double-click a fader cap: back to 0 dB;
  - double-click the level under a fader (e.g. "-3.0"): type a level;
  - double-click a pan slider: back to C (on a stereo track its two pans go back to hard left / hard right);
  - click the pan read-out beside it (e.g. "L30"): type a position.
  - (Ctrl + click still resets, as before.)
- **Reverb tails ring on after Stop**, like outboard gear fed from a tape machine. When playback stops, the mixer that was playing (an edit's own mixer too) carries on running with silence going in, so its reverbs and delays die away naturally instead of being cut dead. It stops by itself once its outputs have been below -90 dB for half a second (at most 30 seconds). The delay-compensation lines are no longer emptied at Stop, only when playback starts again.
- **Mixer strips:**
  - **Six strip widths:** - and + under STRIP WIDTH in the METERS strip (below + Strip...) make every strip narrower or wider, from a little thinner than before to a little wider. All the mixers follow; the choice is remembered on this computer.
  - **Rounded corners** on every strip.
  - **The send dials' rings are easier to see:** the unlit part of the ring is as dark in the light look as in the dark look, and the coloured part is a little thicker.
  - **The output box at the bottom of a strip shows the bus's name first** ("Strings (Int)", "Main L-R (Ext)"), so a narrow box shows the name instead of a row of "Int".
- **A maximised window stops just below the main window's menu bar**, so the menu bar can always be seen.
- **A reference waveform behind the bus / VCA automation:** in the edit window's Buses / VCAs view, "Reference waveform" (at the right, above the timeline) picks one of the edit's audio tracks (the first one to start with); its waveform is drawn faintly behind every bus and VCA row while you write their automation. "No reference" switches it off.
- **The talkback boxes get all the room they can:** DRIVER INPUTS now sits just after the driver line (device, buffer, ins / outs), and the stage speaker boxes use all the space to its right, so several talkback mics fit before a scroll bar is needed.

## 2.61.1
- **The top row of the main window no longer moves** when a talkback mic or a talkback output is chosen (or taken away). The place for the stage speaker boxes at the top right is always kept, at the same height and width, whether they are shown or not (room for a talkback mic, Playback and Output; with more talkback mics the boxes scroll sideways inside that place).

## 2.61.0
- **Fermata opens maximised every time** (filling the screen, with the title bar and the taskbar still showing). "Restore" on the title bar goes back to the usual size.
- **Listening copies: all the pieces in one file.** The Listening copy tab in Mastering has a new choice, Files: "One file for each piece" (as before) or "All the pieces in one file". The single file is named after the album title on the Virtual Master tab (or the project when there is none), for example "Brahms Requiem - listening copy.html". The listener sees a list of the pieces with their lengths (and how many comments they have made on each), clicks one to listen, and it plays straight on into the next piece like a CD; the previous / next buttons sit either side of the play controls. Their comments are listed piece by piece, and when you import them each piece's comments go onto that piece's own edit. The tab shows roughly how big the file will be.
- **The listening page no longer warns "Changes you made may not be saved"** when the listener closes it. Their comments are kept in their browser as they go, so they can close the page and carry on later: the comments come back when they open the same file again on that computer.
- **Save and Export are now two different buttons on the listening page:**
  - **Save my comments** keeps them in the browser on that computer (this also happens on every change; the button confirms it, and warns if the browser will not keep them, as in a private window);
  - **Export comments for sending back to the sender** makes the file they send back to you.
  - **Open exported comments...** (a small link under them) loads an exported file back into the page, so a listener can carry on from it on another computer.
- **The returned file is called after the listening copy, with "listening notes" in place of "listening copy", and ends in .fmnotes** (for example "01 - Kyrie - listening notes.fmnotes"). Import it as before: Feedback button in the edit window, Import comments from a file. Older .json files still import.

## 2.60.0
- **Record to a second drive at the same time.** Project > Second recording drive... chooses a folder, normally on another drive. Every take is then copied there while it records, a moment behind the main drive, into a folder named after the project (with the same folders inside it as the project). The main drive always comes first:
  - the audio and the main recording know nothing about the copy (no extra buffer, no extra work in the audio callback, no waiting at Stop or Record);
  - the copy runs at Windows' background priority (for the processor and the disk);
  - if the second drive falls more than 20 seconds behind, or cannot be written (full, removed, unplugged), that take's copy is thrown away, a notice says so ("The main recording is not affected"), and the next take tries again;
  - after Stop, the last moment and the finished header of each file are copied in the background, so you can record again straight away.
  Project > Stop recording to the second drive switches it off. The setting belongs to the computer, not to the project.
- **No gap in the sound when recording starts or stops.** Starting or stopping a recording rebuilds the audio paths, and until now that emptied the delay lines (delay compensation, plug-in latency), so the processing and cue mixers had a tiny hole in them at Record and at Stop. The delay lines now carry their contents across, so what is coming out of the mixers carries on without a break.
- **The mixers run side by side.** On a computer with more than two physical cores, the processing mixer, the cue mixers and a playing edit's mixer are worked out at the same time on helper threads (on the reserved cores when CPU cores are reserved) instead of one after another. The result is exactly the same as before, sample for sample. A mixer with no outputs going anywhere is no longer worked out at all.
- **Little lights in the menu bar for the Take, Edit and Mixer windows:** green = one is open and in use (in front), orange = open but not in front, red = open but minimised, no light = none open.
- **The header of the main window:** a line now separates the Fermata name and the project from the driver inputs, and there are boxes for the project name, the sample rate, the bit depth and the disk space free on the recording drive (red under 10 GB).
- **The LTC boxes are smaller:** just the light and LTC IN / LTC OUT. Point at one (or click it) for the details.
- **Media and Mastering are the last two entries of the menu**, straight after Display Organiser, with the spare space after them.
- **Listening copies have their own tab in the Mastering window.** "Make listening copies" makes one .html page per piece (with the audio inside it) and nothing else: no audio files have to be exported with them. Choose the audio inside (WAV, FLAC or MP3), its sample rate (the edits' own, 44.1 or 48 kHz) and bit depth, and the folder (normally Bounced Media / Mastered Audio / Listening copies). The pieces, their order and their levels are the ones on the Virtual Master tab. The Virtual Master export no longer makes listening copies.
- **The sun / moon button now switches between the desk colours (light) and the same desk colours a shade darker (dark).** The wood, the colour theme and the lighter keys stay in both. (The old light-grey look is gone.) Fermata starts in the light look.

## 2.59.1
- **The main window fits a 16:9 screen.** It now opens exactly as tall as the full layout needs (752 pixels), leaving room for the window's title bar, so on a 1920 x 1080 screen at 125 % everything shows at full size with no scroll bar.
- **On a shorter screen the input strips shrink to fit** (for example 1920 x 1080 at 150 %): the gain knob, the keys, the meters and the gaps get smaller evenly, and the wooden rail gets thinner, just enough for the strips to fit the height. On a taller screen nothing changes. (The strips still scroll sideways when there are more than fit across.)

## 2.59.0
- **The desk look in every window.** The Take, Edit, Trim and Mixer windows (and the other windows: Project Designer, Mastering, Media, Audio settings ...) now have the same blue desk as the main window: blue-grey panels, dark waveform lanes and displays, and keys lighter than the panel with dark writing and pictures, so they stand out. Every window has wooden side panels and a wooden strip along the bottom. The mixers have blue strips with light keys. The sun / moon button still switches to the light look (without the wood).
- **The colour you choose follows everywhere.** Project > Colour theme now changes every window at once (and is used from the start the next time Fermata opens), not only the main window.
- **The main window fits a laptop screen without scrolling** (1080 lines at 125 %: the window opens 1440 x 790):
  - **One line along the top:** the Fermata mark and name, the project name, the driver and recording state, and DRIVER INPUTS, with thin lines between them. The "E / S / S-L / T = monitoring" note is gone.
  - **The stage speaker boxes (top right) are small squares with knobs** instead of faders: a level knob (and a pan knob for each talkback mic) ringed with lit segments. Drag up / down, double-click to type a value, Ctrl + click for 0 dB / centre.
  - **The talkback key at the bottom of each input strip is smaller and plain:** a red outline round a light key with TALK in black; when the input is a talkback mic it goes all red with TALK in white.
  - **The preamp keys show their names under the pictures** (MIC / LINE, 48V, LC, PAD, BOOST; Ø and Z HI stay as words).
  - **The output strips are shorter:** which mixer feeds an output is now written inside its black display, under its name, and the meter sits straight under the driver name and the TB key.
  - **The audition buttons** (with the headphones) sit on the DRIVER OUTPUTS line.
- **The menu bar always stays on one line.** The entries sit closer together with thin lines between them; on a narrow screen the writing gets smaller instead of the bar wrapping onto a second line.
- **The wooden rail along the bottom of the main window is thinner** (44 pixels), square along the top and rounded at the bottom corners, with "fermata" and the mark pressed into its right-hand end.

## 2.58.0
- **A new look for the main window: the blue desk.** The Driver Inputs are now a row of vertical strips along the top and the Driver Outputs a row of strips below (each row scrolls sideways when there are more than fit). A strip has a coloured bar top and bottom, a black display with your own second name and the driver's name, the gain as a knob ringed with 21 lit segments (drag up or down; double-click for 20 dB; double-click the number under it to type a gain; the mouse wheel still never changes it), the preamp keys, a live meter with its dB scale, the E / S / S-L / T monitor keys and the talkback mic key. The panel is light blue-grey with wooden side panels and a thick wooden rail along the bottom (the minimised windows sit on it as cream tabs). Everything else about the controls works as before.
- **Little pictures on the preamp keys:** a microphone (MIC) or a jack plug (LINE), a lightning bolt (48V), a ramp (LC), a pillow (PAD) and a rocket (BOOST), each over a small caption; phase and Z HI stay as words. The TB key has a megaphone, the audition buttons a pair of headphones, and the talkback mic key is a large-diaphragm studio microphone with a red TALK under it. The keys have 10 pixel rounded corners and are lighter than the panel so they stand out; a lit key goes dark with a coloured line along its bottom (red for 48V, amber for the rest, green / orange for monitoring, purple for talkback).
- **Six colours, chosen once in the Project menu** (Project > Colour theme): blue (the default), red, orange, yellow, green and purple. The choice is remembered. Only the panel changes colour; meters and channel colours do not.
- **The menu bar squares are renamed:** C-R is now **Talkback** and P-B is **Playback**. They are clickable (as before) as well as being switched by the + and - keys, and on the Stream Deck.
- The main window opens taller (940 pixels) to fit the strips; on a smaller screen the inputs scroll up and down.
- Not changed yet: the Take, Edit, Trim and Mixer windows keep their present look.

## 2.57.1
- **The menu bar stays visible on a small screen.** An edit or take window (or any other) that opened maximised, or at a place remembered from a bigger screen, could cover the menu bar along the top of the main window, for example on a laptop. Windows are now checked as soon as they open and whenever they come to the front: one that would cover the bar is moved down below it (a maximised one is fitted to the screen under the bar instead), made shorter if its bottom would go off the screen, and the bar is brought back in front.

## 2.57.0
- **A "please wait" window while a project opens.** It appears straight away over the main window and says what is being read: the project file, each take window, edit and mixer (with a bar showing how far it has got), each plug-in as it starts (a plug-in can take a few seconds), and the check of the recordings. A big project no longer looks as if the program has frozen.
- **The same while files are imported.** After you choose the files (or a folder) for Import takes, every file is opened to read its length, sample rate and channels. That now happens in the background, with the window saying "File 212 of 480" and a Cancel button; the import window follows when it is done. When the copying has finished, the window shows again while the takes are put into the take window or edit.
- **Projects open faster, above all big ones (thousands of recordings):**
  - The check for recordings cut off by a crash or a power cut used to open every recording in the project, one by one. Now only takes that can have been cut off are opened: one saved while it was still recording, or one whose file is shorter than the take. The rest are passed by their file size alone, which is far quicker, especially on a hard disk or with a virus checker running.
  - When the project is still in the folder it was saved in, the audio files are no longer each looked up on the disk while the project is read.
- **Saving is quicker.** The automatic save (a few seconds after any change) used to write out every take window and edit as text just to see whether it had changed. It now works that out from the data itself, and only writes a window's file when that window really changed. On a test project with 250 takes of 32 tracks a save went from about 0.23 s to 0.06 s.
- **The moving playhead is smooth.** In the Edit and Take windows it now moves with every refresh of the screen, and only the thin strip it leaves and the one it moves to are redrawn. Before, every waveform in the window was redrawn 30 times a second, which is what made it stutter with many tracks. Between audio blocks it now glides with the clock instead of jumping a block at a time. The audio itself is not affected.
- **Fewer hitches while playing:**
  - The automatic save waits while the music plays (for up to a minute) and is done as soon as you stop.
  - The Media window looks for new files every 3 seconds instead of every second, not while playing or recording, and takes the file sizes from the folder listing instead of asking each file twice.
  - Waveforms being read (after an import, or the first time a window shows them) are redrawn at most about 6 times a second. The take window also no longer asks the disk about every file it has shown each time one of those waveforms moves on, which could keep the program busy for a long while after a big import.
- The start-up picture is drawn at once, instead of when the program has finished starting.
- New tests: a cut-off finished take is still mended while a healthy one is left alone, a window's file is only rewritten when that window changed, and the playhead clock never runs more than one audio block ahead.

## 2.56.0
- **Re-HarmoniSer: better with a voice that is out of tune among others.** Tuned on your three examples (the flat sop D and the flat repeated F, the sops' C that is not together, the alto Eb / Db / C), and on a bench of eight made-up cases built from your own recordings (a known note plus known neighbours mixed into the real choir). The aim: move the boxed voice, and nothing else.
  - **The TimeCode track is ignored.** A TimeCode (LTC) channel is recognised (a loud, very steady buzz that music never is). It is hidden from the picture and from measuring (its Mute box is ticked, and you can untick it), so it can no longer confuse the pitch search, and it comes out of a correction exactly as it went in.
  - **It follows the boxed voice, vibrato and all.** The pitch search now starts wide and narrows step by step, so a singer with a big vibrato (+-20 cents) is followed to about 1 cent (on the bench the line was typically 14 cents off before, up to 37, so the correction landed beside the real note). A louder voice nearby no longer takes the line over: the voice nearest the middle of your box is the one that is kept.
  - **The voices beside it are found and kept apart.** Up to two other voices within about 110 cents are found (each one has to stand clear of its surroundings all through the note, so ordinary background is not taken for a voice). With neighbours found, the correction fits the note and the neighbours together, a short window at a time, and moves only the note's own sound. On the bench what was done to the neighbours fell from as loud as the neighbour itself (+0.2 to +6 dB) to well below it (-1.5 to -15 dB), and the corrected note still lands within about 1 to 2 dB of where it should, with the old pitch gone. With no neighbour found the correction works exactly as it did.
  - **Measuring takes longer: about 5 to 15 seconds for a two second note** (it used to be a second or two). Nothing changes in how the corrections sound beyond the above.
  - **Honest limit:** three voices all within about 45 cents of each other, every one with a strong vibrato, can still be mixed up for a moment, and then the boxed line is 12 to 16 cents off for part of the note (two of the eight bench cases: 15.7 and 12.5 cents; the other six are 0.7 to 2.6 cents). A narrower box round the note is the way round it.
- A new check, `tests/reharmoniser_voices_test.cpp` (run by `tools/reharmoniser-check.sh`), covers the TimeCode detector, and a boxed note with a louder neighbour: its line, the neighbour's line, where the note lands, that the old pitch is left, and that the neighbour is not moved.

## 2.55.1
- **Waveforms open as plain waveforms.** WaveColour (the rainbow button) is now off when Fermata starts. The colours are only worked out once you switch it on, so take and edit windows open faster.

## 2.55.0
- **Take window: picture buttons.** Mark 1 (a blue M1), Mark 2 (an orange M2), To edit (an arrow into a stave), To playhead (an arrow and a play sign), Overdub (a blue band with a narrower one inside), Markers (a flag), Edit window (a stave with a treble clef), Arm all (a red flexed arm), Arm none (a skinny green arm), Import takes (a shipping container and a clipboard), Export for Processing (the Edit window's multitrack import picture) and Search for bar (a magnifying glass). Hover over any of them for its name and key. Overdub and Markers light up when on. The bar search result now shows in the status line and in the magnifying glass's tooltip.
- **Project > Close.** Closes the project without closing Fermata. It asks "Save and close", "Close without saving" or "Cancel"; then every project window closes and the start panel (New project / Open / Open the last project) comes back.
- **Repair / ReTouch / De-click window: Gain.** Draw a box round a resonance, a ring or anything in one spot, press **G** (or the Gain button) and choose from silence (-inf) to +18 dB. Only the sound inside the box changes, fading in and out smoothly at its edges. It is a fix like any other: Play to listen, Undo to take it off, Write back to keep it. The last value used is remembered.

## 2.54.1
- **Attenuate a stray consonant: the edges now ease out instead of cutting off.** Before, the sound stopped dead where the box started (most clearly after a long main consonant, e.g. the late s in "Multiple s"), and could step back up where it ended. Now:
  - From the start of the box, whatever was sounding just before it (the main consonant, its echo in the room) **dies away** to the background in a straight line in dB, like a natural decay. This takes 40 % of the box (12 to 80 ms).
  - Towards the end of the box the level **rises to meet** the sound that actually follows it, over 25 % of the box (10 to 40 ms), so there is no step back up.
  - Only the shape at the two ends changed: the middle of the box is matched to the clean side as before. A stray copy sitting very close to the main consonant may keep a little of its start; the rest still comes off.
- Tip: if the stray consonant (or its echo in the room mics) carries on past where you end the box, the box now rises to meet it rather than leaving a step. So draw the box over all of the stray copy and a little past it on the clean side: that side is left alone anyway.

## 2.54.0
- **ReTouch: Attenuate a stray consonant.** A new method in the ReTouch window for tidying doubled or messy consonants (an early "sh", a late "s", a second "d", a breathy "t") to one. Draw the box round the early or late copy only, from about 2 kHz (1.5 kHz for sh) to the top, and press ReTouch.
  - Plain Attenuate takes the sound next to the box as "normal". Next to a stray consonant that is the main consonant, so only about half of the stray copy came off. The new method matches every track to the side AWAY from the main consonant: the vowel before an early copy, the sound after a late one.
  - Which side holds the main consonant is worked out once on all the tracks together (shown in the status line). Move Before / After off the middle to choose the side yourself.
  - The background on that side is taken from its quieter frames (up to 150 ms), so a consonant there does not count. Only what stands above it is turned down, by a smooth gain: vowels that carry on through the box keep their level, a box with nothing in it is left alone, and the box fades into the main consonant over 8 to 30 ms.
  - Tuned on five choir recordings (early sh, messy t, multiple s, two double-d examples). Across their 15 tracks it left less than half as much of the stray consonant as plain Attenuate, and pulled far less below the surrounding level.
- Plain Attenuate and Replace are unchanged.

## 2.53.0
- **The I and O flags are gone.** The Edit window no longer has the green IN / red OUT flags or the I and O keys. Everything they did (Bounce Out "between the flags", volume change between the marks) now uses marks 1 and 2 only, as the take window already did. Old projects' I / O points are simply not loaded.
- **P puts marks 1 and 2 round a selection, sample-exact:** in the take window round the selected take, in the Edit window round the selected piece(s) (before, P set the I / O flags). Key 5 takes marks 1 and 2 away.
- The take window calls its flags "Mark 1" and "Mark 2" (buttons, flags, status line, messages) instead of "Edit IN / OUT".

## 2.52.0
- **Bounce Out: "before fader".** Every audio track and Int bus in the list now has a "before fader" tick box next to it. A ticked one makes an extra stereo file of that signal taken after its plug-ins and pan but ahead of its fader, and not affected by mute or solo. The file name ends with "(before fader)". A track can be ticked both ways. New quick button "Tracks before fader"; Clear unticks everything.
- **Export for Processing: Out stream / In stream choice.** The question for an edit point inside the marks showed no buttons (JUCE message boxes only take three). It is now a proper window with Out stream, In stream, Both and Cancel.

## 2.51.0
- De-click and ReTouch now mend **digital dropouts where samples went missing** (a dropped recorder or network buffer, not filled with silence). This is what the clicks in "0115" (1.334 s) and "Click01" (1.927 s) are: 128 samples missing at the same sample on every track, so the music after the jump simply carries on from 1.3 ms later. The jump is found on all the tracks together; the number of missing samples is measured from all of them (prediction of the sound across the gap); the samples are rebuilt from both sides, with the fine texture (air, hiss) of the recording kept; and the second around the click is squeezed back to its original length (about 2 cents for half a second each side: inaudible). Files keep their length, every track is mended in exactly the same way, and nothing outside that second changes.
- The old "step with a fading tail" mend is gone: it took off a tail that was not there and so ADDED a low thump. That is why the low half of these clicks stayed.
- A ReTouch box round such a click puts the missing samples back and then leaves the box alone (it is clean). In the Edit window the time squeeze stays inside the piece that is put in.
- The status line of the repair window says what was found ("Digital dropout at 1.334 s: 130 samples were missing on every track; put back").
- Acoustic clicks (a noise in the room, with its reverb) are not affected: De-click still takes the click itself, ReTouch / Attenuate over the tail takes the reverb down.

## 2.50.0
- The Edit window's top row now uses pictures for the last nine word buttons: Export for Processing (container, arrow, clipboard), Multitrack import (lines, arrow, container), Markers (yellow highlighter), Feedback markers (highlighter and the shouting head), Slip left / Slip right (arrow and an upturned banana skin), Shuffle (two playing cards), Insert flag (arrow on a flag) and Next goes at end (arrow to a double bar line). Hover any of them for its name and explanation; the on/off ones light up when on.

## 2.49.0
- **De-click and ReTouch now take off the click in Click01 (Alto 1, 1.927 s), and clicks like it.** It is the same kind of click as before (a one-sample jump with a fading low thump behind it), but three things hid it from the finder: it jumps DOWN (only upward jumps were ever fixed), in some microphones the thump is faint and short, and in others it is long and hidden under the bass. Jumps in either direction are now found, and a faint or short thump is accepted when what is left is no more than the music itself would leave. Stricter checks keep ordinary sound from being touched (no false hits on the click-free takes, at any sensitivity). In both click files (Click01 and the earlier click!) every microphone that has the click now gets exactly one repair, at the click; Solo, which has none, is left alone.
- **The whole thump comes off even when the marked part or box ends before it does.** The fading tail of a step is followed for up to 25 ms after the jump, past the end of the area or box (the Take window writes that stretch too).
- **ReTouch round a click takes the jump and the thump off first, in both methods.** A box of 250 ms or less that reaches below 1 kHz now finds a step-type click inside it and removes it before the box is turned down (Attenuate, the default) or rebuilt (Replace). Before, Attenuate took the jump down by less than half and left the thump behind. Click sensitivity now sets how readily it is found in ReTouch as well, and a short De-click area also mends what a step leaves behind (a spike on top of it) the usual way.
- **Cautious when the length of the thump is uncertain.** Loud bass can hide how long a thump lasts, so the tail taken off is an average over all the lengths the sound allows, leaning towards the short ones. Where the sound shows the answer it is as good as before; where it does not, it leaves a little thump rather than putting in one of its own. On about 1,800 artificial clicks laid into the real recordings, about three times fewer repairs came out worse than leaving the click.
- The two edges of a dropout (a run of digital silence) are never taken for a level step.

## 2.48.0
- **Picture buttons in the Edit and Take windows.** The row of grey rectangles is replaced by small symbols: the classic transport set (return to start, rewind, play / stop, forward, go to end, record), Trim Window (a T with scissors and a needle and thread), Bounce Out (a bouncing ball), Pitch (a straight line with a wavy red one through it), Repair & DeClick (a plaster), ReHarmoniSer (three lines converging to one), Tracks (a plus), Feedback (a shouting head), Edit Mixer (faders), Move earlier / Move later (an alarm clock with a left / right arrow), Delete piece (a red "no" sign), Undo fix (a backwards arrow), Automation (a yellow line with dots) and Buses / VCAs (a bus). Resting the mouse on any of them for about a second shows its name (and the longer explanation under it). All tooltips now wait about a second.
- **One Pitch button.** It opens one window with two tabs: Pitch curve first, then Pitch process (semitones / cents). (When the marked part contains an edit point, so the two streams are open, the Pitch process tab says it is not offered there; the curve works in each stream as before.)
- **Spectral Repair and De-click are one button, "Repair & DeClick".** It opens the same combined window as before.
- **Edit window keys:** with marks 1 and 2 set, Enter opens Repair & DeClick and H opens the ReHarmoniSer for that part.
- **Main window: preamp controls only where they work.** The gain slider and the MIC / 48V / polarity / low cut / pad / Z HI / BOOST buttons appear only on inputs fed by a Merging Hapi, MT48 or Anubis that Fermata is connected to (through the MADPanel driver). Inputs on any other ASIO driver or preamp show just the driver name, second name, meter, monitor and CR.

## 2.47.0
- **De-click now removes the low-frequency part of a step-type click.** Some clicks are a sudden level jump followed by a fading offset (a thump of a few hundred hertz lasting a millisecond or two, which is what a high-pass in the chain makes of a step). De-click used to repair the jump and the bright part but left that thump, which is where the click lived below 2 kHz. It now finds the jump, fits the fading tail against a model of the music just before it, and takes the whole thing off. It only acts when the result is clearly smoother and the tail matches the size of the jump, so normal material is left alone.
- Take-window automation lanes were dropped (Edit window only).

## 2.46.1
- **ReHarmoniSer Out / In windows: an untouched stream can be written back.** Pressing Write back to clip on a stream where nothing was changed now simply means "leave this stream as it is": its window closes and the other stream goes into the edit. (It used to say "Nothing has been changed yet. Press Cancel to close.", and there was no way to finish without changing both streams.)

## 2.46.0
- **De-click now repairs dropouts.** A run of digital silence in the middle of sound (like the 128-sample hole, 1.3 ms, at 4.05 s in the close mics of the test files) used to stay: only the jump at each end of it was smoothed. The hole is now rebuilt from the sound before and after it (and brought up to the level around it). Real silence and very quiet passages are left alone. On clean piano with holes cut into it the error drops by 12 dB at 128 samples and the filled level is within 1 dB of the original.
- **Bus / VCA view in the Edit window.** A new **Buses / VCAs** button swaps the rows from audio tracks to the Int Buses and VCAs, each with its own automation lane (labels, M / S, colour and lane choice work as on a track; no audio is shown). Int Buses can automate Gain, Fader, Pan, sends and plug-in parameters like an audio track; VCAs automate their Fader. Automation plays, is part of Bounce Out and the Mastering render.
- **Int Buses have a Pan and an input Gain.** Pan is a balance of the bus's stereo pair (the opposite side is turned down); the new slider is on the Int Bus strip where the track strips have their pans. Input gain (before the inserts and the fader) is automatable. Both are saved and copied with the mix.

## 2.45.0
- **VCA faders.** A new kind of mixer strip that carries no audio: its fader moves the level of the audio tracks and Int Buses assigned to it by the same amount (a -4 dB move lowers each by 4 dB), while every strip keeps its own fader position. A strip can be under several VCAs (the gains add). The VCA strip has M and S (mute / solo everything it controls), an **Assign...** button that opens a list of tracks and Int Buses to tick, a name box (shown as `4 - Strings`) and a **Remove strip** button. Up to 16 VCAs, numbered 1 to 16; the number stays when others are removed. Every controlled strip gets a glowing number for each of its VCAs on its coloured bar. Ext Buses cannot be controlled (they are where sound leaves).
- **Project designer: new "5. VCAs" tab** to add, name and delete VCAs; they appear in the processing mixer (and the Edit mixers). Mixers is now tab 6.
- **+ Strip... button** under the Phase scope / Mono buttons of every mixer: add an Int Bus, an Ext Bus or a VCA (a new one, or one that exists in the project but is not in this mixer yet).
- The VCA levels, mutes, solos, assignments and which mixers show them are saved with the project and the mixer files, and are copied with the mix. Bounces and the Mastering window hear the VCAs like the live mixer. A VCA's fader is the only thing that can be automated (the playback side is in; the lane row in the Edit window comes next).

## 2.44.0
- **ReTouch and De-click in one window.** The Spectral Repair and De-Click windows are now the same window with two buttons, **ReTouch** (key R) and **De-click** (key D), sharing the picture, the box, Undo and the list of fixes. De-click uses only the time span of the box (or the whole picture if there is no box) and ignores its pitch range; the Click sensitivity slider sits next to the buttons.
- **ReTouch (Attenuate) keeps the harmonics' decay.** It now measures the level of each pitch just before and just after the box and slides smoothly between the two, so a harmonic that is fading out keeps fading at the same rate through the fix, with no bump. (Measured against RX: harmonic ridge change -0.73 dB, was -2.76 dB.)
- **Better de-clicker.** Clicks are found against a model learned from a de-spiked copy, so loud clicks no longer hide each other, clicks that were left alone are now found, and note onsets and dense crackle are no longer mistaken for clicks. Mending is guarded against making a hole (a mend that is too quiet is replaced by a prediction from both sides).
- **Bug fix:** dragging on the spectrogram made a note selection but moved the whole window (caused by the window-raise fix of 2.42.1).
- **Bug fix:** resizing the selection box by an edge or corner could make it perfectly narrow or flat; a move that would collapse it is now ignored.

## 2.43.0
- **Take window meters show playback.** While a take plays back, the small horizontal meters of each track show the level of the mixer strip (a stereo track shows left over right), as in the edit window. While recording they show the input as before.
- **M and S buttons at the top left of each track, above the name** (take window and edit window), side by side, so it is clear which track they belong to. The take window now has them too (the same mute / solo flags as the mixer strip). The name sits below them; in low rows it sits to their right.
- **The track-name column of the take window can be resized** by dragging its right edge (like the edit window). The width is shared by all take windows and the buttons, meters and ARM switch follow it.

## 2.42.1
- **OUT / IN stream windows no longer disappear when Stop (or Play) is clicked.** They are now owned by the main window (so Windows keeps them above it) and are put back on top after every click inside them.

## 2.42.0
- **Dragging the first fade-in earlier / the last fade-out later now grows the piece.** In the Trim window at the very start or end of the edit, moving the outer end of the fade outward makes the piece (the blue region box, its waveform and the edit point) grow to cover the whole fade, using more of the take. (Before, only the orange overlay moved.)
- **Edit point flags at the start and end of the edit stand in the middle of their fade,** like the flags of joins, wherever the fade has been moved to.
- **Imported comments are saved in a `comments` folder** in the project folder (one sub-folder per edit: the imported files and `comments.json`). They reload with the project. Importing a new set of comments for an edit replaces the old set and its saved copy (a "done" tick on a comment that is still in the new set is kept; several files imported together are added to each other).

## 2.41.2
- **Trim window feels snappier.** Quick successions of changes (dragging the level sliders, clicking the Slide A / Slide B / Move the join buttons repeatedly) now bring the edit up to date at once but redraw the other windows at most about every 100 ms, with one last redraw after the final change (`Project::changedLive`). Accept, Revert, fade drags and shape changes still redraw immediately.

## 2.41.1
- **Crossfades into externally processed / corrected audio no longer fade in from silence.** The corrected file often starts exactly where the piece starts, but the join's crossfade was centred on the join, so its first half faded in nothing (a dip in level). A fix join's crossfade now slides inside the corrected audio (same length), with the matching fade of the neighbouring piece. Applies to re-linking audio from Export for Processing and to offline fixes (Spectral Repair, De-Click, ReHarmoniSer, Pitch curve).

## 2.41.0
- **Export for Processing over an edit point asks which take to send.** If the 1/2 marks contain an edit point you can send the **Out stream**, the **In stream**, or **Both** (two separate sets of files, named "<name> - Out Stream" and "<name> - In Stream"). Each is sent at full volume with no fade applied. Out stream: from mark 1 to mark 2 or to where it has faded out completely, whichever comes first. In stream: from where its fade-in begins (or mark 1) to mark 2. The edit point and its crossfade stay in place; re-linking the corrected files puts each take back under its own piece.

## 2.40.2
- **OUT STREAM / IN STREAM windows: clearer and independent.** Each window now has a big coloured banner at the top (orange "OUT STREAM", blue "IN STREAM") saying which take it is. Each can be processed, played, stopped and auditioned on its own, in any order, with the mouse. The keyboard (Space, R, arrows ...) starts with the OUT stream. When you press Write back (Accept in Pitch curve) on a stream, that window goes away and the keyboard moves to the other one; when both have been written back they go into the edit together (one undo step). The X on the remaining window cancels everything, including a stream already written back.
- **Key 5 in the edit window now clears the I / O points as well as the 1 / 2 flags.**

## 2.40.1
- **Trimming the start or end of the edit now removes the audio it leaves out.** In the Trim window, when you move the fade-in at the very start of the edit later (say from 0:00 to 0:04), the piece now begins where the fade begins and the unwanted audio before it is cut off, instead of staying there as silence. With Slip right on, everything after it moves up to close the space (the first piece stays at the start of the edit); with it off, the rest stays where it is. The same at the end of the edit: moving the fade-out earlier shortens the last piece. The fade keeps its shape and the sound from the fade on does not change. (The audio you cut stays in the take: you can slide it back later.)

## 2.40.0
- **Edit points inside an offline correction: OUT STREAM and IN STREAM.** When marks 1 and 2 contain an edit point and you open Spectral Repair, De-Click, Pitch curve or ReHarmoniSer, two windows open side by side over the same marked part. The OUT STREAM (orange) is the take that plays up to the edit, shown on its own and carried on past the edit through its handle; the IN STREAM (blue) is the take that plays from the edit, shown on its own and started before it. No more mixed picture with a false click line at the join. Correct each stream in its own window.
- **Write back** in each window only gets that stream ready (press it on an untouched stream to leave that stream as it is). When both are ready they go into the edit together, as ONE undo step, and the edit point and its fades stay exactly where they are: no rendered piece over the join. The X or Cancel on either window closes both and changes nothing.
- The crossfade at the edit now uses corrected audio on both sides (each take's handle is corrected too).
- One edit point at a time: if the marks contain more than one, Fermata asks you to mark a shorter part. With no edit point between the marks everything works as before.

## 2.39.0
- **The edit point is now where the pieces really change.** When you move an edit point in the Trim window, the boundary between the two pieces now moves to the middle of the crossfade (where the numbered edit flag is). The fades stay at exactly the same times, so the sound is sample-for-sample unchanged. The result: the old blue line is gone, the piece name banner starts at the edit, the spectrogram and the offline fixes use the take you actually hear, and the false click line at the old position disappears. Edit points you moved in older projects are tidied the same way when the project opens.
- **The join line in the Trim window is a free reference.** Drag its grip (the little triangle in the ruler) anywhere; it changes nothing in the edit. When you let go, the view re-centres on it, so zooming stays on the new line, and the millisecond readings count from it. "Centre" puts it back on the join.
- Export for Processing: if the marked part runs past the next edit point, only the part up to it is sent (a processing piece is always one take), and the message now says so.

## 2.38.1
- **Fixed: dragging the pitch scale in Spectral Repair and De-Click zoomed instead of moving.** Click-dragging the frequency scale at the left of the picture now slides the visible part up and down, following the mouse, and never changes how much is shown. It works the same on every pitch scale (Mel, even Hz, octaves) and stops at the lowest and highest pitch. (Both windows use the same picture, so both are fixed.)

## 2.38.0
- **The top band stays put when you scroll down.** In the edit window and the take window, when there are more tracks than fit and you scroll down, the markers, the flags, the ruler (timeline), the piece / take names and the edit points now stay fixed at the top, like the buttons, and the track names stay at the left. You can click, drag and right-click in the band wherever you have scrolled to.
- **Markers and Feedback markers each have their own line and their own tick box.** The edit window's top band now has a line for your markers (amber flags, as before) and, under it, a line for the listeners' comments (teal flags with a thin stripe of the colour the website gave them). The new **Markers** and **Feedback markers** tick boxes in the toolbar show or hide each one (remembered with the project); the left of each line says which it is. The take window has the **Markers** tick box too.

## 2.37.0
- **Feedback from listeners, flagged on your timeline.** New **Feedback...** button in the edit window: *Import comments from a file...* reads a comments file and puts each comment on the timeline as a flag along the top (next to the markers). Click a flag to read the thread (who wrote it, the bar they typed, the text); tick a comment **Done** (the flag goes grey), delete it, or **Go here**. Several comments at one moment are one flag ("+2" = two replies). Importing the same file again adds nothing twice. The comments are saved with the project and are not part of undo. Menu: show / hide the flags, mark all done, take away the done ones, take away all.
- **Imports your colleague's website export.** The tab-separated marker list (UTF-8; columns Name, Type, Marker Name, Comment, Start, Duration, Color) is read directly: the comment is in *Marker Name*, the person in *Comment*, the time is timecode (hours:minutes:seconds:FRAMES; Fermata asks the frame rate once and remembers it), and the marker colour colours the flag. The times are taken as time in the exported file, so they are placed from the start of the edit's first audio. Other layouts (CSV, JSON, times as 83.5 / 1:23 / 00:01:23:12) are understood too, with columns found by name.
- **Make your own: Include Feedback Form (Virtual Master tab).** Tick it and every exported track also gets ONE `.html` file ("name - listening copy.html") with the full-quality recording inside. The listener double-clicks it; it opens in their browser (Windows, Mac, iPhone / iPad, Android) with nothing to install. It shows the waveform (zoom with + / - or Ctrl + wheel), has play / pause / stop / back 10 s / forward 10 s / volume, and an **Add a comment here** button that takes the time the moment it is pressed (the box pauses the music while they type, and carries on afterwards): the time can be adjusted, the bar is free text ("Bar 14", "Bar 19 -24"), then the comment. Each comment becomes a flag on the waveform and an entry in the list on the right; flags can be dragged, comments edited or deleted. **Save my comments** makes a small `.json` file to send back. Comments are kept in the browser too, so closing the page does not lose them. The audio inside can be WAV (the export's rate and bit depth, e.g. 24 / 96), FLAC (lossless, about half the size) or MP3. The file is about a third bigger than the audio inside it; Fermata shows the size per minute. Very large files (over about 150 MB) may be too much for a phone; FLAC halves it with no loss.
- Imported listener files carry the edit they were made from and where the file's time 0 sat on the timeline, so the flags land on the right place even if you have since edited more at the end.

## 2.36.0
- **The offline windows remember their size and place.** Spectral Repair, De-Click, ReHarmoniSer, the pitch curve and Import AAF open the way you left them (the size and the screen position; if the screens have changed they are kept on a screen).
- **And the way they look and their settings.** Spectral Repair / De-Click remember the method, Bands, Multi-resolution, Link stereo, direction, strength, surrounding length, before / after weighting, click sensitivity, the pitch scale (Mel...), the bass detail and the colour roll, as well as the Spectrogram settings (colours, levels, detail, reassignment) that were already remembered. ReHarmoniSer remembers the View block (spectrogram opacity, colour contrast, colour wheel, lines, gain, detail), the Isolate lead-in / tail and the Key level.

## 2.35.1
- **Fixed: processing across an edit point (a join made by an earlier fix) could leave the sound after the join silent.** The corrected files of the pieces on either side of the join were given the SAME name, so the second one was written on to the end of the first and the piece read the wrong audio (it stopped after a second or two). Every new file now gets a name of its own, and a new WAV always replaces an old file of the same name instead of being added to it. This affects Re-HarmoniSer, Spectral Repair, De-Click, Pitch correction and the pitch curve in an edit window.
- All of those now put the corrected pieces into the edit in one shared way, and give every real edit point its own fades and curves back exactly (before, only Re-HarmoniSer did, and it could turn the equal-power crossfade after the marked area into a linear one). Marks that only just touch a piece (less than 60 ms of it) no longer cut a sliver out of it: the 30 ms crossfades of a join cannot fit in a piece that short, and the sound dipped there. A test crosses the marks over the joins in many ways and checks that the edit sounds exactly the same and the fades are as they were.

## 2.35.0
- **Spectral Repair: Link stereo (on by default, Attenuate).** A stereo file is now turned down by the same amount on both sides (the amount found for the louder side) with each side's own phase kept, so the time differences between the microphones and the level difference between them, which make the stereo image, are not changed. Untick it to treat each channel on its own. It applies to stereo files (pairs joined at import); two separate mono files are still processed one by one.
- The Pattern and Partials + Noise tabs of RX will not be built.

## 2.34.1
- **Attenuate tuned against real examples** (a double consonant and a click in a held chord, each with the iZotope RX result): the patch now settles to the same level as RX's within about 1 dB, and sound outside the noise is moved by less than half a dB. Draw the box tight round the noise for the best match.

## 2.34.0
- **Spectral Repair: new Attenuate method (now the default), set up like iZotope RX.** Draw a box round the noise (a pencil drop, a stray consonant, a click over a held chord) and Attenuate turns down only what sticks out above the sound around the box, keeping the music underneath, so the patch matches its surroundings. Controls: Bands (128 - 4096, default 1024), Multi-resolution (long window for the bass, short for the top), Surrounding length (default 100 %), Strength (default 80 %), Before / After weighting, and the direction Horizontal / Vertical / 2D. The old rebuild-from-the-surroundings method stays as **Replace**.
- **A sharper spectrogram.** The picture of what is in view is now drawn with a Gaussian window and reassignment (the energy is moved to where the sound really is), so a held note is a thin line and a click a thin column. It is made again after you zoom or move (a moment after you stop), on every track at once. New default pitch scale **Mel**, and the **cyan-to-orange** colours with the RX levels (-134.3 to -5.3 dB). **Spectrogram settings...** changes the colours, levels, detail and reassignment, and is remembered.
- Pattern and Partials + Noise (the other tabs of RX) are not built yet.

## 2.33.0
- **Import takes: one take window per piece.** When the file names name several pieces ("LV01 - Gravity", "LV02 - I Wake" ...), a tick (on by default) puts each piece in its own take window, named after the piece, with its takes numbered from 1. A window of that name that already exists is used. All the pieces share the same tracks, so the tracks are made once and your stereo / mono choices apply to every window. The tracks are no longer given the piece name as a prefix.
- **Import takes: choose the track order.** The new **Track order...** button lists the tracks (alphabetical to begin with); Move up / Move down puts them in the order you want, and the new tracks are made in that order. Fermata remembers the order for the next import (any track not in the list goes after the others).

## 2.32.0
- **Import takes: choose stereo or mono for each left / right pair.** The single "join pairs" tick box is now a list with one tick per pair (Omni, ORTF, Room, Solo ...). Tick = one stereo track, untick = keep the two as mono tracks. Keep the piano's three mics mono by unticking "Pn". Fermata remembers what you left mono for the next import.
- **Import takes: slightly different spellings of a piece name are one piece.** "LV1 - Gravity" and "LV01 - Gravity" no longer create a second set of tracks, and the first take of a Pyramix session (whose file names have no number, "LV1 - Gravity_Omni-L") now joins the others as take 0 instead of being handled separately.
- **Import AAF:** works with stereo tracks joined at import (the left and right names both find the one stereo file), and uses each take file's own track.

## 2.31.0
- **Project > Import AAF as edit windows...** Reads an AAF written by Pyramix (or another editor), splits its one long timeline into separate pieces wherever nothing plays on any track for more than 10 seconds (you can change the 10), and makes one edit window per piece: the takes laid out where they sat, the track names kept, and the **cross-fades kept** (their length and position). Nothing is copied: the edits use the audio you already imported into take windows (matched by the files' original names, which Fermata now remembers when it imports), or files found in an extra folder you choose (used where they are). Anything not found is listed.
- Takes imported before this version do not remember their original names; for those, choose the Pyramix audio folder in the AAF dialog.

## 2.30.2
- **Mastering previews that stayed orange after Render previews:** some part of a piece's description (most likely a plug-in's saved state) keeps changing by itself, so its render never matched. After a render, if the description still differs a moment later, Fermata now looks at it several times over a fraction of a second, finds the parts that change by themselves, leaves just those out of the up-to-date check from then on, and writes them to `fermata-log.txt` ("keeps changing by itself at ..."). Everything you actually change still makes the preview orange.
- The previews are all re-checked once after this update (they were described a different way before), so press Render previews once.

## 2.30.1
- **Mastering previews stay "up to date" after Render previews:** after a render the project can describe a piece slightly differently from when the render started, so two of the four pieces still showed orange. The new render now takes the project's description as it stands when it finishes. (If it still happens, `fermata-log.txt` says which piece's description changed while it rendered.)

## 2.30.0
- **Mastering: nothing renders by itself any more.** The DDP builder only *looks* at the preview renders (it never starts one). A light shows how they stand: **red** "Preview not rendered", **orange** "Preview renders are out of date" (after an edit or mixer change; it names which pieces), **green** "Preview renders up to date". The new **Render previews** button makes the missing / out-of-date ones, once. This ends the endless re-rendering and waveform redrawing.
- Play in the DDP builder no longer renders: if the previews are not up to date it tells you to press Render previews first. Export still makes any renders it needs.

## 2.29.1
- **Virtual Master settings:** "File format" and its drop-down are now on one line, like Sample rate, Bit depth and Dither.
- **Tab key:** it now goes down the left column (format ... folder) and then down the right column (Album, Album artist, Artist ... the selected file's tags), instead of jumping back to the left.

## 2.29.0
- **Mixer copy button, new take:** it opens a panel with tick boxes for Faders (levels, mutes, solos, output buttons), Sends, Pan and Inserts. Press "Copy" to keep it and then press the new **Paste** button that appears in any other mixer, or send it straight to any one mixer (or all the others) from the same panel. "Undo last copy / paste" is still there.
- **Pin and sun / moon buttons:** the Display Organiser, Audio settings, Take Display and Meter bridge windows now have a clear strip along the top for them, so they no longer cover other controls.

## 2.28.0
- **Edits made from a take window are now named "take name - EDIT"** (capital EDIT).
- **Re-draw waveform:** right-click the audio in an edit window (new menu item) or a take window (the right-click on the audio now opens a small menu; the bars option is still there, and right-clicking the banner still opens it directly) to throw away the saved waveform shape and colours of that file and work them out again from the audio.
- **Project menu > Re-draw all waveforms in current window** does the same for every file in the edit or take window you were last working in.

## 2.27.2
- **Trim window:** the Level of A / Level of B faders now hold through the whole fade. Before, the level only began where the piece starts, so the fade rose to 0 dB and then dropped to the new level (a swell at the end of the fade); now the fade only reaches the level you set.

## 2.27.1
- **Mastering window:** "Render from", the source and the Tail moved down into the row under the tabs, so the pin and moon buttons no longer cover the Tail box.
- **Waveforms:** the colour-by-frequency waveforms (WaveColour) are back exactly as they were; the Pyramix-style pale look made them lose their colours, so it is switched off.

## 2.27.0
- **Mastering window:** the pieces are now only rendered while the **DDP builder** tab is open (the Virtual master tab does not need them, so its render line is hidden and leaving the DDP tab cancels any rendering). The edits are no longer rendered over and over: the renders are now only looked at when the window opens, when you change which pieces are ticked or the render settings, and when something that a ticked piece's sound depends on has really changed (an edit, an audio file, its mixer) while the Mastering window is open. Bringing the window to the front, or a render finishing, no longer starts another look.

## 2.26.0
- **Waveforms folder:** the waveform outlines and the multicoloured waveform colours are now saved in a `Waveforms` folder next to the project file, so opening a project no longer reads every audio file through again to redraw them. The first time a file is seen it is worked out as before; after that it is loaded from the folder. A saved waveform is used only while the audio file keeps the same name, size and modified date, so edited or replaced audio is simply redone. The folder can be deleted at any time and is rebuilt as needed.

## 2.25.0
- **Mixer inserts:** clicking the name of an insert slot now opens the plug-in straight away (an empty slot opens the plug-in list). A little dot on the right of the slot opens the menu (load, bypass, remove, replace). Right-click also opens the menu.
- **Mixer inserts:** drag a plug-in by its name onto another insert slot, in the same mixer or another one, to move it there with all its settings (if the slot is occupied the two swap places). Hold **Shift** while dropping to put a copy there instead, with the current values of all its parameters. The slot you are dropping on lights up (yellow = move, green = copy).

## 2.24.0
- **Edit window:** a new **Insert flag** tick box shows or hides the INSERT flag (the strip, the dotted line and the "next take goes here" note). Hidden, it also cannot be dragged by accident. Remembered between runs.
- **Trim window:** "Play original A" and "Play original B" now show a playhead moving through that take's own stream, across the edit point, while they play.

## 2.23.2
- **Edit window:** Slip Left and Slip Right are the same setting as in the trim window: on by default, remembered between runs, and a change in one window shows in the other.

## 2.23.1
- **Trim window:** Slip Left and Slip Right are now on by default, and they stay as you last set them, in every trim window and between runs.

## 2.23.0
- **Trim window:** A / Z (copy the fade across) now copy the fade to the same place IN TIME. Before, they copied its distance from the piece's own edge, which landed somewhere else whenever A's end and B's start were not at the same spot (after pieces had been moved in the edit window).
- **Trim window:** a line beside the copy buttons says how much silence (or overlap) there is between where A ends and where B starts, and a new **Close gap** button moves B (and everything after it) so it starts exactly where A ends, with the normal crossfade. This is the fix for A and B sitting out of line.
- **Edit window:** a new **Shuffle** tick box beside Slip Left / Slip Right. Ticked, dragging a piece by its banner past a neighbour swaps them like cards; unticked (the default), the piece simply goes where you drop it.
- **Edit window:** key **R** moves the selected piece so it butts up against the piece before it (the pieces after it move with it), with the normal crossfade, ready to be trimmed.

## 2.21.0
- **Mixer:** double-clicking a send knob now sets it to 0 dB (Ctrl + double-click types a level instead).
- **Mixer:** Int Buses now have a Solo button. Soloing a bus keeps open everything that feeds it (tracks and other buses, through their faders or sends) and silences the rest; soloing a track still keeps the buses it goes to open. The solo is saved with the project.
- **Look button:** the dark-look symbol is now a proper crescent moon (it had a stray blob beside it).

## 2.20.0
- **Edit window:** a piece can now be dragged by its top banner to wherever you like and it stays there, leaving a stretch of silence if you want one (the last piece too). Dragging past a neighbour's middle still swaps them like cards. With Slip Left / Slip Right on, the pieces on that side move with it. Where silence is left, the pieces get short edge fades instead of a crossfade into the gap.

## 2.19.1
- **Trim window:** the bottom row of buttons was squeezed out of sight by the new level faders; the control area is taller again (and the window opens a little taller).
- **Trim window:** the slide buttons (Slide A, Slide B, Move the join) now also have -100 ms and +100 ms, for sliding a take by a third of a second or more in a few clicks.
- **Trim window:** when a slide (a button or dragging the waveform) cannot go any further because the take runs out of audio, or the piece would become too short, a notice now says so and how far it can go, instead of nothing happening.

## 2.19.0
- **T, E and M squares in the menu bar**, just to the right of Take Window, Edit Window and Mixer. A click on a square does not open the drop-down: it brings the take / edit / mixer window you used last to the front (restoring it if it was minimised, opening it if it was closed). A square lights up while the window you are working in is of that kind.
- **Faster meters in the edit and take windows.** The edit window's track meters showed a 300 ms average, so they trailed the mixer's; they now show the peak (read 30 times a second, falling away smoothly) just like the mixer's meters. The take window's meters are also read 30 times a second now.

## 2.18.1
- **The 1 / 2 marks in the take window stay after "To edit" (key 3).** They used to be cleared when the take was sent, so after undoing an insertion you had to mark the part again. They now stay until you make new 1 / 2 marks (or press 5 to take them away).

## 2.18.0
- **Edit window: shuffle.** Drag a piece by the banner at its top (the strip with its take name) and it trades places with its neighbours like cards in a deck as soon as its middle passes theirs. The pieces after them stay where they were, the joins around the swapped pieces get the default crossfade, and the whole shuffle is one undo step. No key is needed: the banner is only for shuffling (Ctrl + click is still how you slide a piece, Alt + drag how you mark one track).
- **Edit window: the insert flag has its own strip.** A new strip under the markers holds an orange INSERT flag for "next take goes here". Drag it along; it sticks to the start or end of a piece when it gets close and settles on the nearest one when you let go. Clicking the time ruler still places it too.

## 2.17.0
- **Trim window: level faders for the two takes.** "Level of A (out)" and "Level of B (in)" raise or lower the take going out of the edit or the take coming into it (up to 12 dB either way, double-click for 0) so both sides match. Each fader sets the level of that whole take, which is the same volume change that "Change volume between fades" shows and edits, so you see and change it in both places.
- **Trim window: "Normalise volume over next take".** With it ticked, a change made to the level of B at the edit glides back to 0 dB in a straight line from the end of this crossfade to the start of the next crossfade (for example +4 dB falling to 0 dB over 15 s). The glide follows the crossfades whenever either of them is moved.
- **Right-click on a piece in the edit window:** the volume choices are now "Change volume from here to next edit", "Change volume between the 1/2 marks" (all tracks, or just those chosen with Alt + drag) and "Change volume between fades (this take only)".

## 2.16.3
- **Delay compensation in the mixer.** Plug-ins that hold the sound back (look-ahead, linear-phase filters, some reverbs and de-essers) used to leave their channel late against the others. Fermata now asks every plug-in how late it is and holds all the shorter paths back by the difference, so every track, bus and send arrives together: sends into a channel, buses into buses, and all the Ext Buses of a mixer come out of the driver outputs at the same moment. It follows loading, replacing, removing and bypassing a plug-in by itself, and plug-ins that change their own delay while working.
- **Strip display:** at the bottom of every audio track and bus strip, above the name, a number shows how many samples the plug-ins on that channel delay the engine by (0 when there is none or they are bypassed). Hover the strip for the milliseconds and the longest path in the mixer.
- **Bounce Out is compensated too:** the first samples (the lead-in of the longest delay) are thrown away and the same amount is rendered at the end, so bounced files line up with the timeline and with each other.
- With any compensation in use, everything the mixer plays is that many samples late against the transport (that is how compensation works); the delay is the longest plug-in path, shown in the strip tooltip.
- **Meter bridge and big take clock:** both stay in front of everything on the desktop; the topmost state is re-asserted every second so Windows cannot quietly drop it.

## 2.16.2
- **Trim window: click the time ruler to move the centre line.** Click (or drag) along the ruler above the two waveforms and the view re-centres on that spot; zooming in and out then keeps that spot in the middle, as in the take and edit windows.

## 2.16.1
- **Sending a selection to the edit window no longer brings it up.** "To edit" from the take window now leaves the edit window exactly as it is: if it is closed it stays closed, if it is minimised it stays minimised, if it is behind it stays behind. The material is added to the edit and appears when you open the edit window yourself.

## 2.16.0
- **Mixer: double-click any value and type it.** Faders (channel and bus), pan sliders and send dials open a small box next to the control: type a level in dB (or -inf / off), a pan position (L30, C, R45, or -30 / 30) and press Enter; Escape or a click elsewhere leaves it alone. The stage (talkback) mixer's faders and pans work the same way. Ctrl + click puts a control back to its default (0 dB / centre); the click on a send dial that opens its panel now waits a moment so a double-click is not mistaken for it.
- **Mixer fader:** after the mixer was made taller or shorter, the cap no longer followed the mouse (it moved as if the fader were still its old size). The mouse now moves the cap exactly as far as the cap travels, at any window size.
- **Pin button:** a pinned window now stays in front of the other Fermata windows only, not of other programs on the desktop (the same as the menu bar strip).

## 2.15.1
- Mixer: the "copy mix to another mixer" button moved left so the pin button no longer covers it.

## 2.15.0
- **Import: all takes on the same tracks.** File names like "0105 L tk.03_03.wav" (a track number first, the take number written twice at the end) used to leave the second take number in the track name, so every take got its own set of tracks. Every take-number word is now removed, a leading track number and a trailing "tk" are tidied away ("0100 Live2TChoir tk.01_01" becomes the track "Live2TChoir"), and 27 takes of 14 mics give 14 tracks.
- Import: the L / R option now also recognises "Piano4099L" / "Piano4099R" and bare "L" / "R" names (a bare pair becomes the stereo track "L-R"); a second tick-box also joins names ending in 1 and 2 (off unless ticked). Untick the L / R box to keep every file a mono track.
- Import: one choice for the whole import - copy the audio into the project's folder (default) or use the files where they are (nothing copied). Stereo joins and multichannel files still have to be written as new files.
- Edit window: the little track meters now fall smoothly back to zero when playback stops or pauses, instead of staying at the last level.
- ReHarmoniSer: a colour wheel in the View block changes the colour of the spectrogram smoothly (any hue; double-click for the original amber).

## 2.14.0
- **Quick markers** in both the take window and the edit window. A thin strip along the very top (above the time ruler): right-click it for "Add Marker" or "Clear All Markers". A new marker asks for a name (Enter keeps what you type; click away and it keeps its sequential number 1, 2, 3 ...). Drag a marker to move it; right-click it to rename or delete it (double-click also renames). Markers are saved with the window and are not undo steps.

## 2.13.0
- **LTC (timecode as audio).** Two labelled lamps in the always-visible top bar: "LTC in" is red when no timecode is detected and green with the frame rate (24, 25, 29.97, 29.97 DF, 30...) when it is. The source is any audio input - including the Hapi TC input routed in Aneman, which is just another driver channel. "LTC out" sends generated timecode through an output channel of your choice (routed in Aneman to a Hapi output) with its own lamp. Click either lamp to choose the channel / format / start.
- Recorded LTC is an ordinary mono recorded track marked LTC, linked to each take like any other audio. A "LTC" triangle in the take window and the edit window shows or hides its waveform.
- On playback and bounce the LTC of each edit piece starts hard at the first sample of that piece's fade-in, has no fades and no crossfades, and stops one sample before the next piece's LTC starts, so LTC pieces never overlap.

## 2.12.1
- Take window: the "In n" / "Out n" flags that show where a take is used in the edit now carry exactly the same numbers as the edit points in the edit window (so "In 2" can no longer appear before "Out 2" by mistake). Joins made by an offline fix (pitch, repair, de-click, export for processing) show "fx", as in the edit window, and are not counted.

## 2.12.0
- Import: new tick-box "Join the -L / -R files into stereo tracks". A pair such as "piano-L" and "piano-R" in the same take becomes ONE stereo track (one stereo mixer strip, one stereo take track, one stereo edit track). The two mono files are written as a single new 24-bit stereo WAV in the project folder; your originals are not touched. It is on by default when pairs are found; untick it to keep them as separate mono tracks.

## 2.11.0
- ReHarmoniSer: a separate movable playhead (click the picture or ruler to place it; playback starts there), a progress bar while it reads, corrects and writes back, ONE Audition button beside the transport, drawn symbols on the transport buttons, each piano key plays its own note, and the explanation box is tall enough for its text.
- Edit window: track names wrap onto two lines, the right edge of the name column can be dragged wider, and each track has a small level meter plus Mute and Solo buttons that are the same switches as on the edit's mixer.
- Import: file names like "A Winter Ride 015_piano-L.wav" (piece, take number, instrument / mic) are now read, so every take of a piece goes on the same tracks as separate takes instead of new tracks for each take.
- Mixer: the strips fit the window height (taller window = more empty insert slots and a taller fader; shorter = a shorter fader), and the window can be maximised.
- Pin button (next to the sun / moon) on every window: a pinned window stays in front; only one window is pinned at a time.
- Top row: the Pre-Rec, C-R and P-B squares are now buttons; the separate Pre-Rec button between Take Window and Edit Window is gone.

## 2.10.0
- ReHarmoniSer and crossfades. The marked section is now loaded the way it sounds, with the crossfades between the takes applied, so the picture and the pitch measurements see an edit point as you hear it.
- On Write back to clip, each piece of the edit is corrected from its OWN audio (every track of that piece, with up to 1 s of the take either side that its crossfades use), so the two takes on either side of an edit point each get the pitch change for their own sound, including the tail that hangs over the join. Edit points keep their numbers, fade lengths, fade positions and fade curves exactly as they were, so you can still change a crossfade later (the audio under it is corrected as far as the extra second either side).
- The joins made at the start and end of the marked area (not edit points) are linear fades; the existing crossfades are never changed to linear any more. For this a region can now have a different curve for its fade-in and its fade-out (shown correctly in the Trim window and saved in the project).
- A piece of the edit that the corrections cannot reach is left exactly as it is (no new files, no new joins). Outside the marks, the audio next to a new join is the original, sample for sample.
- The sample-accurate length checks of 2.9.1 still apply.

## 2.9.1
- ReHarmoniSer: the corrected audio is now checked to be exactly the same length as the original, to the sample. If the audio handed back is not the same length, or putting it into the edit would change the edit's length or move any clip, it is refused with a message and the edit is left untouched.

## 2.9.0
- ReHarmoniSer: **Write back to clip now works** when a recording file ends a few samples before the region it belongs to (the error "a corrected file covers ... but ... is needed"). The corrected piece simply ends where the shortest file ends.
- ReHarmoniSer: the window is redesigned to match the original: sage-green right panel with bold titles and thin lines, Note Selection with the note box, down / up arrows, "meant to be" and from / to / Set, collapsible section headers (all closed when the window opens), the View block at the bottom (Loop markers, Zoom time / pitch, Note lines, Spectrogram opacity, Colour gain, Colour contrast, Detail), amber LCD readouts, rounded light buttons, dark slider tracks with a light grip, transport row with |<, Play, Stop, >|, Loop, Original and Isolate, and a dark status bar.
- ReHarmoniSer: new **Isolate** (plays just the picked note with a lead-in and tail), **Colour contrast**, **Spectrogram** opacity and **Detail** (Pitch / Balanced / Time) controls.

## 2.8.1
- ReHarmoniSer: when the corrected audio cannot be put into the edit, the message now says exactly why (and it is written to fermata-log.txt).

## 2.8.0
- ReHarmoniSer: Section ReFinement no longer leaves a note selected after Apply correction, and Write back to clip uses the corrected audio that is already prepared, so the edit stays untouched until Write back is pressed.
- ReHarmoniSer: new look closer to the original: sage-green panels, dark picture, amber LCD readouts, light buttons, dark slider tracks.

## 2.7.1
- Fixed the Windows build error in 2.7.0 (the Pitch over time chart's Close button). No other change.

## 2.7.0
- ReHarmoniSer: **Pitch over time...** button in Ensemble ReCentre. It measures the whole recording and shows a chart of how far the choir's pitch sits from A=440 (green line, 0 c = A=440), with the reference A4 now in the box as an amber dashed line and the whole-recording A4 underneath. Esc or Close goes back.

## 2.6.0
- ReHarmoniSer: new **Section ReFinement** (several singers on one note, some out of tune). Draw a box round the note, open Section ReFinement and press Find voices: each separate voice is drawn on the picture as a coloured line labelled with how many cents it is from the written note, and listed with a tick (voices already within about 6 cents start unticked) and a bar for how strong it is. Choose "Move them to the written note" or "to the strongest voice", press Preview move (then Space to listen) and Apply correction. Only the ticked voices are moved, in every channel the same way. Each move appears in Corrected Notes (switch off, Edit, X) and is written back with the rest.
- It is now listed in the same order as the Python tool: Ensemble ReCentre, Note ReShape, Section ReFinement, Erase ReBrush, See mixer, Corrected Notes.

## 2.5.1
- The window files now live in three folders named `Take Windows`, `Edit Windows` and `Mixer Windows`. Projects saved with 2.3.0 - 2.5.0 move their files into the new folders on the next save (the old files are removed once the new ones are written).
- A new edit, take window or mixer gets its file straight away, not on the next automatic save.
- ReHarmoniSer: Apply correction / Erase (apply) no longer changes the edit. The corrected audio is put into the edit only while you play it inside ReHarmoniSer (so it goes through the edit's mixer) and taken out again when playback stops. It stays in the edit only when you press Write back to clip. Cancel or closing leaves the edit exactly as it was.

## 2.5.0
- Every mixer (the processing mixer and the cue mixers) is now saved in its own file too: `Mixers/<name>.fmmix` in the project folder. With the edit and take window files, the whole project except the recordings is now in separate files. The Edit mixers stay inside their edit's `.fmedit` file. If the processing mixer's file is missing when a project opens, you are told and a new empty one is made (the missing file is not deleted).
- Mixers menu: "Import a mixer file (.fmmix) as a new mixer..." (strips are matched with this project's tracks and buses by name).
- ReHarmoniSer: new **Erase ReBrush** section. Hold the mouse down and drag along a sound on the picture (a cough, a squeak, a stray voice): a red stroke with its reach shows. Set Time radius, Pitch radius and Amount, Audition, then Erase (apply). Erasures appear in Corrected Notes (switch off, Edit, X) and are written back with the notes, the same on every channel.

## 2.4.0
- ReHarmoniSer: new **See mixer** section in the right-hand panel. One row per audio channel (named after its track): M hides the channel from the picture, S shows only the soloed channels, and the slider makes that channel brighter or darker in the picture. It only changes what you SEE (and which channels are used to measure a note); what you hear still goes through the edit's own mixer. The picture updates a moment after you stop moving a slider.
- ReHarmoniSer: clicking a piano key on the left of the picture now plays that note. New **Key level** slider in the View block sets how loud the key notes are. The tone goes to the same outputs as the edit's mixer.
- Not in this version yet: Section ReFinement (nudging single voices), Erase ReBrush, the pitch chart, the standalone program.

## 2.3.0
- Every edit is now saved in its own file (`Edits/<name>.fmedit`) and every take window in its own file (`Take Windows/<name>.fmtake`), inside the project folder. The project file only lists them. The files are updated every time the project is saved, and they are what Fermata reads when it opens the project.
- Old projects convert on their first save. The old project file is kept once as `<project>.fermata.before-window-files`.
- New in the Edits and Takes menus: "Import an edit file..." / "Import a take window file...", so an edit or take window can be copied into another project (tracks are matched by name; if the audio is not found, "Find the audio files..." looks for it by file name in a folder you choose), and "Delete an edit" / "Delete a take window". Delete asks first ("Yes, I do want to delete it"), removes the window and moves its file to the recycle bin. Audio recordings are never deleted.
- If a window's file is missing when a project opens, you are told, the window is left out, and it stays in the project's list so the file can be put back.

## 2.2.0
- New ReHarmoniSer button in the edit window (next to De-Click). Mark the stretch with keys 1 and 2, press ReHarmoniSer: all tracks between the marks open in one window with a spectrogram. Drag a box round a note, the note is measured, set Move / Snap, Audition (it gets ready; Space plays), Apply correction, then Write back to clip. Every track is changed in exactly the same way. Corrected notes can be switched off, edited or removed; Original plays without the changes. Undo fix brings the original audio back.
- Not in this version yet: Section ReFinement (nudging single voices), Erase ReBrush, the choir-pitch chart, the project file.

## 2.1.0
- Re-HarmoniSer groundwork: the pitch-correction engine (a plain C++ port of the Python tool, no screens yet) is added to the build. Nothing visible changes in Fermata. It was checked against the Python results on a test choir signal (27 checks, all matching).

## 2.0.6
- Anubis jack inputs: the Line / Instrument switch now works (Line = inputMode 1, Instrument = inputMode 2, as read from the device), and the gain shown and changed is the Instrument gain while in Instrument mode.

## 2.0.5
- Fixed: pressing LINE / INSTR on an Anubis jack input wrote a true / false into the device's instrument GAIN, which made the Anubis restart. Nothing is sent for that button now (it stays at LINE) until the real Line / Instrument setting has been read from the device.

## 2.0.4
- Fixed: the start-up picture and the log file always said "1.0"; they now show the real version number.

## 2.0.3
- Preamps: for the Anubis jack inputs the Line / Instrument switch is now the first button of the row, where MIC / LINE is on the other inputs: LINE (plain) or INSTR (blue).

## 2.0.2
- Preamps: fixed line inputs with their own gain (the Anubis jack inputs 3 / 4) are now found and controlled: gain 0-66 dB, polarity, low cut and the Line / Instrument switch (shown as INSTR). No MIC, 48V, PAD or BOOST buttons for them.
- The preamp modules are ordered by the numbers in their names (Combo 1/2, Jack 3/4) so the channels line up with ANEMAN's.
- fermata-log.txt now also lists each module's capabilities and first channel (lines with MODULE), to check the device's field names.

## 2.0.1
- Fixed: in the Preamps window the found devices covered the column titles (Name, IP address, First input, Status).
- Preamps: when ANEMAN sends more inputs of a device to ASIO than the device itself offers preamp controls for (for example the Anubis's jack inputs 3 / 4), the window now says so, and fermata-log.txt lists every module the device reports (lines with MODULE) and whether it is controlled, so it can be seen exactly why.

## 2.0.0
- New: **CPU cores** window (Audio settings > CPU cores...). Keeps chosen CPU cores for the audio engine only, with a measurement that finds the quietest cores, and an option to keep other programs off them. Off until switched on.
- Fixed: the audio callback could mix two different mixer plans during a routing change; a stalled audio thread could have old audio data freed under it; normalised bounces could clip; automation moved the wrong mixer when everything was rendered through one chosen mixer; the take window's Alt-drag track choice was not saved.
- Tidied: old take-window I / O mark fields and a number of unused functions and variables removed.

## 1.0.0 (the stable base)
Everything up to and including: M / the Stream Deck mixer tile open the mixer of the Edit you are working in; Auto PQ "Keep positions" snaps the PQ flags to the tracks; the ISRC tool; overlapping tracks in the PQ editor; take-window Edit IN / OUT flags, P sets them round a whole take; spectral-window playhead and zoom; the Stage Speaker mixer; Loop and rainbow buttons.

# Fermata - version history

Version numbers are MAJOR.MINOR.PATCH (for example 2.1.3). The number is set in one place, the first lines of `CMakeLists.txt`, and appears on the splash screen, in the first line of `fermata-log.txt` and in the properties of `Fermata.exe`.

- **PATCH** (2.0.1, 2.0.2 ...): bug fixes and small tweaks only.
- **MINOR** (2.1.0, 2.2.0 ...): a new feature or a visible change.
- **MAJOR** (3.0.0 ...): a large change in how the program works.

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

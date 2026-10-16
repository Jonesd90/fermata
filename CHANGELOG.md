# Fermata - version history

Version numbers are MAJOR.MINOR.PATCH (for example 2.1.3). The number is set in one place, the first lines of `CMakeLists.txt`, and appears on the splash screen, in the first line of `fermata-log.txt` and in the properties of `Fermata.exe`.

- **PATCH** (2.0.1, 2.0.2 ...): bug fixes and small tweaks only.
- **MINOR** (2.1.0, 2.2.0 ...): a new feature or a visible change.
- **MAJOR** (3.0.0 ...): a large change in how the program works.

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

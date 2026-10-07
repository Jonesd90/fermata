# Fermata - development notes (internal: change history, build steps, details of every feature)

The look is a dark console look (dark greys, near-white ink) with the Fermata logo teal as the accent and square 1-pixel-outlined controls. It applies to every window. The typeface is Inter (bundled, SIL Open Font License, see assets/Inter-LICENSE.txt). Buttons share one size and one column grid.

## PART 1 - Get the program (no installing anything, about 20 minutes, one time)

GitHub builds the program for you on its own computers. You only click buttons.

1. Go to https://github.com and create a free account (or sign in).
2. Click the **+** at the top right, then **New repository**. Name it `Fermata`. Choose **Private**. Click **Create repository**.
3. Unzip the `Fermata.zip` I gave you on your computer. You get a folder called `Fermata`.
4. On your new repository page click **uploading an existing file**.
5. Open the unzipped folder, select EVERYTHING inside it (including the hidden `.github` folder - if you can't see it, in File Explorer click View > Show > Hidden items) and drag it all into the browser window. Wait until every file has uploaded.
   *The `.github` folder is essential: it contains the build recipe.*
6. Scroll down and click **Commit changes**.
7. Click the **Actions** tab at the top. If GitHub asks, click **I understand my workflows, enable them**.
8. The build starts by itself (or click **Build Windows app** on the left, then **Run workflow**). It takes 10-20 minutes. A yellow dot means working, a green tick means done, a red cross means failed.
9. When it is green, click the run, scroll down to **Artifacts**, and click **Fermata-windows**. A zip downloads. Unzip it: inside is `Fermata.exe`.
10. Double-click `Fermata.exe`. If Windows says "protected your PC", click **More info** then **Run anyway** (it is your own program, not signed).

**If the build shows a red cross:** click the failed run, click the red step, copy the last 50 lines of the red text and send them to me. I will fix it. This is the first-ever Windows build, so one round of fixes is likely.

## PART 2 - First start

1. Press **Audio settings...** Choose the **ASIO** type and your Merging device. Set the sample rate. Press **Apply**, then **Close**. All driver inputs and outputs (e.g. 40 and 40) are switched on automatically.
2. The main window's **Driver Inputs** page lists every driver input (numbered in the order the driver shows them) with a live meter, a box for your own second name (e.g. ORTF L), and the preamp controls: gain, MIC/LINE, 48V, polarity, low cut. These are the only preamp controls; every mixer shares them. Each input row also has an **ARM** button next to the preamp controls, and the monitor buttons **E / S / S-L / T**. They apply to every track that is fed by that input (they are greyed out until a track uses the input). The tracks themselves are listed in the Project Designer, **2. Audio tracks** tab. (On a small laptop screen the main window shrinks to fit.)
3. Press **Project Designer**. The **I/O** tab lists every driver input and output (you can name them). In **Tracks**, add tracks (mono / stereo / surround); the list fills as you add. Rename, drag to reorder, right-click to delete. Choose each track's input (a stereo track on input 5 auto-fills 5 & 6; change the second if you need to).
4. Each driver input row in the main window has monitor buttons for the tracks it feeds: **E** (live only when armed), **S** (always live, muted while a take plays back), **S-L** (live even during playback), **T** (talkback: on between takes, completely off while recording).
5. Project > Save. Recordings go in `Audio/<Take window name>/`.

**A new project is completely empty:** one mixer (the processing mixer), no tracks, no buses. There is **no Main / master output and no effects channel**. Sound reaches the outputs only through an **Ext bus**: add one in the Project Designer (tab 4), send your tracks to it with the dial on each strip, and pick its pair of driver outputs on its own strip in the mixer.

## Main window layout

Driver inputs are always listed on the left and driver outputs on the right (drag the bar between them to give one more room), Outputs have live meters too, and show which Ext bus feeds them.

The top row of buttons: **Project**, **Audio settings**, **Project Designer**, **Take Window**, **Edit Window**, **Mixer** (and **Meter bridge**). There is no Record button here: recording is always started with **REC** in a take window. A faint Fermata mark sits behind the window.

## Mixers

- **Look.** Strips are drawn fine and dense: a coloured bar on top says what kind of channel it is (AUDIO, INT BUS, EXT BUS), a coloured name plate sits at the bottom, with recessed insert and send wells, a finely divided fader scale and a segmented meter that shares the fader's scale. The **dark console look is the default**; **Light look** (top right of the mixer window) switches to the light Fermata look and is remembered. The background of each strip is tinted with the channel's colour (click the coloured bar or name plate to change it), and the waveforms in the Take and Edit windows use the same colour.
- **Channel colours.** Click the coloured bar at the top or the name plate at the bottom of any strip, pick a colour (or *Other...* for any colour, or *Automatic*). The colour goes on the fader cap, the send dials that feed it, the pan line, and on that track in the **Take and Edit windows** (name plate and clips). You can also right-click a track name in a take or edit window and choose *Colour...*. Audio tracks are blue, Int buses violet and Ext buses red until you pick one. Colours are saved with the project.
- Under the coloured bar is the ASIO input selector for that track (a stereo track has two). It is the same setting as in the main window and the Project Designer.

- Type a new name in the **Mixer name** box at the top and press Enter.
- Every pan slider is as wide as the strip and its exact position is written above it: **L100 ... C ... R100**. A stereo track has two (Pan L, Pan R): put the left channel on the right and the right channel on the left if you like. Mono tracks have one. Double-click a slider to reset it.
- **Delete this mixer** (top of a cue mixer's window, or **Mixer > Delete a cue mixer** in the main window, or the Project Designer's Mixers tab). The processing mixer (the first one) can never be deleted.
- Every channel strip has dials near the top for its sends (see "Track types and routing" below).
- Meters have a dB scale. Only the top changes colour (orange from -9 dB, red from -2 dB). The peak marker holds 10 seconds, but a peak above -2 dBFS stays until you press **Reset peaks** (the last strip).

## Keys that work in every window

- **M** opens or closes the **processing mixer** (the first mixer, the one you run the session from). The other mixers are called **cue mixers**.
- In the **take window and the edit window**, **Left / Right arrows** zoom the time axis out / in and **Up / Down arrows** make the tracks smaller / bigger. They work on whichever window you are in.
- **B** opens or closes the **meter bridge**: a floating window (it stays on top) with a tall vertical meter for every driver input (before any fader, as they arrive from the preamps) and every driver output (after the faders). Peak hold works as in the mixers; **Reset peaks** clears it. The same window opens from the **Meter bridge** button in the main window.
- (Typing in a text box, such as a mixer's name, never triggers these keys.)

## Track types and routing

There are three kinds of mixer channel. Add and arrange them in the **Project Designer** (tabs 2, 3 and 4). Each list shows the items in the order you added them; **Up / Down** rearrange them and **Delete** removes them.

- **Audio track** - no input, or an input from the audio driver; mono, stereo (or surround). It can be sent to other audio tracks, Int Buses and Ext Buses.
- **Int bus** - fed by audio tracks and other Int Buses. It can be sent on to audio tracks, other Int Buses and Ext Buses.
- **Ext bus** - fed by any audio track or Int Bus. Its only way out is a pair of driver outputs (chosen on its own strip, in each mixer) or nowhere. **Ext buses are the only things that reach the driver outputs** (there is no Main or master output).

Sending is done with a **rotary dial** near the top of the channel's strip, one dial for each Int Bus and Ext Bus:
- Click, hold and move the mouse **up** to send more, **down** to send less (bottom = off).
- **Click once** (without dragging) for a small panel: the send level on a small fader, **PRE / POST** fader, and **one or two VST3 inserts** that sit between the signal and the send. These inserts only affect that send, never the audio going through the channel's own fader.
- To send to another **audio track**, press **+ send to track...** under the dials; a dial for it then appears.
- A send that would make a loop (A feeds B feeds A) is refused ("no loops!").
- Pre-fader sends ignore the fader but still obey Mute and Solo.
- Routing lives in each mixer, so every mixer can have its own routing. Old projects open with their effects channels as Int Buses.

## Cue mixer audition and copying mixes

The FIRST mixer in the list is the processing mixer (the engineer's). Every other mixer is a cue mixer (producer, composer...) and gets an **<its name> Audition** button, both at the top of its own mixer window and in the row under the main window's status line. The name on the button is the mixer's name, so rename the mixer and the button follows.

- Press it: whatever that mixer sends to its Ext buses is also played on the first output pair of the processing mixer's first Ext bus, and your own Ext buses go quiet, its window opens so you can edit it while you listen, and it stays that way until you press the (orange) button again. The other person keeps hearing their own mix on their own outputs the whole time.
- **Copy this mix to...** (top of the processing mixer's window) copies your levels, pans, mutes, solos, sends (with their pre/post setting) and bus levels onto one other mixer, or onto ALL of them. On a cue mixer the same button reads **Copy processing mixer's mix here...**. The Ext buses' output pairs are never copied. Tick "Also copy plug-in inserts" if you want those too.
- **Undo last copy** is in the same menu (one level).

## PART 3 - Recording

- Press ARM on the driver input(s) you want to record (main window), then press **R** (or the REC button) in a take window. **Space** stops the recording. Every armed track makes its own file; takes recorded together are grouped. The waveform appears live as you record.
- While recording (and while playing) in a take window, a red playhead moves left to right. When it reaches the right edge the window turns a whole page, so the playhead is at the left again, instead of scrolling continuously. If you scroll away yourself, it will not pull you back until it reaches the edge of the view again.
- Files are named like `005 - Symphony 2 - violin.wav`.
- Right-click a track name in a take window to remove that track from that window only (other take windows, the mixer strip, the edits and the audio files are untouched).
- **Take Window** (main window) opens a take window (one per piece, unlimited). Takes and Edits are independent: deleting a take leaves it in the edit.

## PART 4 - Editing (SADiE style)

1. In a take window, click the **ruler** at the top (the tall strip with the times) to place the **playhead**; clicking a take only selects it, and clicking an empty patch deselects every take. Press **1** / **2** to mark the edit IN / OUT: you can press them at the playhead, or *while the take is playing or recording* to mark by ear. (**I** / **O** are only for Bounce Out marks.) Press **3** to send the marked part to the edit (it goes after the last piece), or **4** to place it at the edit window's playhead (what is there is cut and the rest moves later). **Space** plays from the playhead (and stops again); during a recording Space stops it. When a recording ends the playhead goes to the start of that take, so Space plays it back. **Right / Left arrows** zoom the time axis in / out (they zoom around the playhead; Shift + arrow nudges the playhead); **Up / Down arrows** make all tracks smaller / bigger (8 steps, from 32 tracks per page to one stereo track per page). Send the piece to an edit with **To edit [3]** or **At edit playhead [4]**.
   - **Overdub box** (take window): when ticked, 3 and 4 lay the piece *over* the edit at the edit's playhead, and it plays at the same time as what is already there. Overdubs are drawn as narrower strips in the middle of each track row; click one to select it, drag it sideways (nothing else moves), Delete removes it. The edit's playhead is the purple marker on its ruler: click in the edit to move it.
   - **Volume inside a piece:** right-click a piece (or overdub) in the Edit window, **Volume change here...**. A small mixer opens with a fader for every file of the grouped piece and an **All** fader. Tick **Fade the change** to make it a glide of 0.1 - 1.0 s instead of an instant step. **Whole piece** sets one level from the start. The change is shown as a teal line (the glide shaded). **Remove this change** or **Remove all volume changes** undo it. Changes made while the edit is playing are heard the next time you press play.
2. **Edit Window** (main window) opens an Edit window. Drag a piece left/right to slide it. **Slip Left / Slip Right** decide whether the pieces to the left / right move along.
3. Click a join and press **T** (or double-click it) to open the **Trim window**:
   - **F2** auditions (the yellow playhead moves). **Enter** accepts, closes the Trim window and returns you to the Edit window.
   - Drag the top (A, out) or bottom (B, in) waveform sideways to slide that audio. Slip Left / Right toggles in the Trim window are separate from the Edit window's.
   - Crossfade: drag the corners in the top half of A for the out-fade only, the bottom half of B for the in-fade only, the middle band for both. The white line stays where the join originally was.
   - **A** copies the out-fade onto the in-fade; **Z** copies in to out.
   - **Right / Left arrows** zoom in / out (mouse wheel too).
   - In the Edit window the same arrows work: Left / Right zoom time, Up / Down change the track height.
   - **Accept**, **Next edit >>**, **<< Previous edit** accept this join and move on.

## PART 5 - Bounce Out (making the master)

**Bounce Out...** is a button in the Edit window and in the take window. It plays the audio **faster than real time** through the settings of the **processing mixer** (the first mixer) - or any other mixer you pick - and writes 24-bit WAV files. Your live mixers, plug-ins and audio device are not disturbed: the bounce works on a private copy.

**What to bounce**
- **Full Track** (Edit window) - from the first bit of audio to the last bit of audio used in the edit.
- **Use I/O points** - in the Edit window press **I** to set the IN and **O** to set the OUT at the playhead (or at the cursor if nothing is playing). They are drawn in green / red. In a take window the take's own IN / OUT marks are used.
- **Selected pieces** (Edit window) - click a piece of the edit, **Ctrl + click** more of them. Each selected piece becomes its own file (name - 01, name - 02 ...).
- **Selected takes** (take window) - click a take (it goes a little darker), **Ctrl + click** more. Each selected take becomes its own file (name - take 005 ...). They must all be at the same sample rate.
- **Whole take** (take window) - the take you clicked or marked.
- **Add before / Add after (0 - 9 seconds)** works with every range above: the file becomes exactly that much longer. Nothing but silence goes into the mixer during the extra time, so plug-ins settle and reverb tails ring out in it.

**Options**
- **Outputs to bounce**: tick any Ext buses, Int buses and / or audio tracks (their signal after their faders). Ext buses are ticked to begin with. **Every ticked output becomes its own stereo file**, so you can print a stereo mix and stems in one go. The file name ends with the output's name, for example `Symphony 2 - Out.wav`, `Symphony 2 - violin.wav`. (With only one output ticked, the "Add the output's name" box decides.)
- **Normalise**: the loudest peak ends up at the level you type (for example -0.1 or -1 dBFS). Normally every file is normalised on its own. Tick **Normalise all the files together** to apply one gain to all the files of a piece (the loudest reaches the level), so stems keep their balance with each other. If you do not normalise and something goes over 0 dBFS, the window tells you it clipped.
- **File name** and **Folder** (the last folder is remembered).
- **Mixer**: which mixer's faders, pans, sends and plug-ins are used (the processing mixer by default).

Bounced files are never overwritten: if the name exists you get "name (2).wav".

## Known limits (honest list)

- **Preamp control** (Hapi / Anubis / MT48 over Ravenna): the screens are there, but the link to the hardware is NOT connected yet. I need a capture of the control traffic from your rig. Use MADPanel for gains meanwhile.
- LTC timecode chase: not built yet.
- Each recorded file must stay under 4 GB (about 58 minutes of stereo at 192 kHz).
- VST3 plug-in scanning happens inside the program; a badly-behaved plug-in can crash it.
- Surround tracks are folded to stereo for monitoring (placeholder).
- Insert plug-ins run as stereo effects.
- Windows/ASIO behaviour has not yet been tested on a real machine.

## Preamp control (Anubis, Hapi, MT48)

Fermata talks to each device through the same web interface that its own web page uses (a WebSocket on port 80 of the device). Press **Preamps...** in the main window:

1. **Add my usual four** fills in Anubis .20, MT48 .30, THAP .40 and BHAP .50 (change any address that differs), or **Add device** for your own.
2. Press **Apply and connect**. The status column shows each device connecting and how many preamp channels it has.
3. **First input** says which driver input the device's first preamp feeds; its other channels follow in order. Press **Stack in order** to fill these in from the channel counts, then **Apply and connect** again. Change them if your Ravenna routing differs.
4. The gain slider, MIC/LINE, 48V, polarity and low-cut buttons on each input in the main window now control the real preamp. Only the setting you change is sent.

Safety: when Fermata connects it only *reads* the device (the controls show what the hardware has); it never sends the project's stored settings. The device list is saved with the project and remembered for new projects.


## Mixer outputs, main bus, fader (latest)
- Every Audio Track and Int Bus strip has an **OUT** button and a drop-down under its fader: this is the strip's output *after* the big fader. "Main" goes to the mixer's main Ext Bus; or pick any Int / Ext Bus.
- On an Ext Bus strip, tick **Main out** to make it the mixer's main output (only one can be ticked).
- Mute is now a stronger red; the fader is longer and its cap taller and rounded.
- The main window has no time read-out and shows the project and driver info on one line.

## This build
- **Piece:** the Take window has one name box. Every take is named after it ("001 - Piece - violin.wav"); a take can still be given its own name (double-click it).
- **ARM** now lives in the Take window: click ARM on a track's name to arm it, or use **Arm all / Arm none**. The main window no longer has ARM.
- **Crossfade shapes** (Trim window, "Fade shape"): Equal power, Linear, Cosine, Smooth S, Logarithmic, Exponential, Fast, Slow.
- **Minimise** sends a window to a title bar along the bottom of the main window; click it to bring the window back. New windows always open in front.
- **I / O** (bounce in / out) work while a take is playing (live position). **P** marks a whole selected take (Take window) or the selected pieces (Edit window) as the bounce range.
- **. and ,** make the waveforms bigger / smaller in the Take, Edit and Trim windows (display only).
- **Splash screen** with the build number for about 4 seconds; the logo is the program icon, and saved projects (.fermata) get a paper-and-logo icon (set up automatically the first time the program runs).
- **Media** button: a folder tree of where every file is saved. Clicking a take or a piece in the Take / Edit window highlights its file there (even if the Media window is behind). Double-click a file to open its folder.
- **Session mode** (button in the main window): the inputs are kept for the last 6 seconds all the time, so a take starts 6 seconds BEFORE Record was pressed. A thin red line shows round the main window while it is on. Pressing Record or Stop never interrupts what you hear.

### Latest changes
- Stereo (and wider) tracks show every channel's waveform while recording.
- Edit flags read "Edit IN" / "Edit OUT".
- Mixer windows cannot be made bigger than the strips need; strips are slimmer (90 px).
- "Next Take / This Take" box in the Take window and on the main page.

### This build (5 Oct)
- Mixer: slimmer top bar (copy-mix icon, sun / moon look icon; Scan plug-ins moved to Audio settings), one insert slot until one is used (then every strip shows one more), black border below the strips.
- Meters strip: Peak / RMS / Peak + RMS, rise, fall, peak hold, and a Phase scope window for any track, Int Bus or Ext Bus.
- Bounce Out: "All takes as individual files"; normalise default -1 dBFS.
- Display Organiser (main window): 1 to 4 windows tiled on the screen.
- Take and Edit window toolbars flow onto as few lines as fit. Sending to the edit keeps you in the take window. Key 4 works in the Edit window.
- Playhead follows the time after Stop (button with a triangle and a line toggles it).
- Trim window: double-click a piece in the Edit window; Accept + next fade / previous fade (keys ] and [).

## Edge fades (5 Oct, later)
Every piece of audio now starts and ends with a 1 ms fade, so nothing clicks when it starts or stops. The recorded files are never changed; the fade is applied when playing and bouncing.
- Take window: each take has its own fade-in and fade-out. Drag the little white triangle at the top-left corner of a take to the right for a longer fade-in, and the top-right corner to the left for a longer fade-out.
- Edit window: the start of the first piece and the end of the last piece (and both ends of an overdub) have the same corner handles. Joins between pieces are crossfades, edited in the Trim window.
- Play from the middle / stop at a mark also uses a 1 ms fade. Drag a fade to nothing for a hard cut.
- Older projects get the 1 ms fades on the ends of their edits when opened.

## Sharper waveforms, mixer height (5 Oct, later)
- Waveforms in the Take and Edit windows: the overview is now 4x finer (one value per 128 samples), and once you zoom in far enough that fewer than 128 samples share a pixel, the window reads the audio itself, so the peaks are exact; zoomed right in it draws the real sample curve (with a dot on each sample when very close). Drawn from `src/ui/WaveDraw.h`.
- Mixer window: when the strips are wider than the window, the horizontal scroll bar was taking 12 px from the height, so the window could never be tall enough to hide the vertical scroll bar. The maximum height (and width) now allows for the scroll bars.

- Trim window: drag in the dark band between the two waveforms to move the view left and right (the edit itself does not change). The Centre button brings the join back to the middle.

## Windows belong to the main window; naming the edit (5 Oct, later)
- Every Fermata window (take, edit, mixer, trim, scope, bounce, media, organiser, audio settings) is now OWNED by the main window (Windows). None can disappear behind it when you click it, they have no taskbar buttons of their own, and minimising the main window minimises all of them (the taskbar button of the main window brings them all back). `src/ui/WinOwner.cpp`. On other systems the old "raise on click" is used.
- The first time you send a piece from a take window with no edit yet (keys 3 / 4, or the buttons), you are asked to name the edit. After that, sending pieces never raises or focuses the edit window: it opens behind the take window the first time, and stays where it is afterwards.
- Plug-in windows (VST3 editors) now belong to the main window too, like every other window. Where windows cannot be owned, the fallback now puts the windows back in front of the main window in the order they were last used (the one you were working in stays on top), and plug-in windows stay above the rest.

## Take Display (5 Oct, later)
Main window > "Take Display" opens its settings. It fills other screens (or any screen you tick) with: the project line (an artist name or album title), a logo (the Fermata mark, none, or your own PNG / JPG / GIF), the piece name, the take number (green "NEXT TAKE", red "THIS TAKE" while recording), the time of day (24 hour HH:MM:SS) and the session countdown. Everything scales to the size of the screen.
- Session timer: "Ends at (time of day)" with HH:MM, or "Length of the session" with HH:MM, then press Start (the length counts from that moment). At 00:00:00 it goes on as -00:04:53 in red, labelled OVER TIME. A time of day that passed less than 12 hours ago counts as today (so you are over time); an older one means tomorrow. "No countdown" hides it.
- A screen is closed with a double-click or Esc on it, or its button in the settings. The settings window has a preview, so you can try it with one screen.
- The settings are remembered for next time (they belong to the computer, not to the project).

## Menu bar, mixer scroll bars, Display Organiser (5 Oct, later)
- Main window: all the buttons are now a menu bar along the top (Project, Audio settings, Preamps, Project Designer, Take Window, Edit Window, Mixer, Meter bridge, Media, Session mode, Take Display, Display Organiser), with no "..." in the names. The window cannot be made narrower than the bar (it would only wrap onto a second line on a very narrow screen).
- Display Organiser: unless the main window is given a part of the screen itself, it goes to the top-left corner of the display and the other windows are arranged below its menu bar, so the bar is never covered.
- Mixer window: the biggest it can be is exactly what the strips need (no scroll bars at all at that size). Previously the limit depended on whether a scroll bar happened to be showing, so the window could never reach a size without them. Scroll bars appear only when the window is smaller than the strips (or the strips are bigger than the screen).

## Talkback (CR mic and TB speakers)

Set-up (main window): press **CR** on the input that is the Control Room microphone, and **TB** on the first of the two outputs that feed the talkback speakers (the pair is shown as TB L / TB R). Both are saved with the project.

* **Numeric keypad +** opens the CR mic to the TB pair. A tap (under 1/3 s) latches it on, the next tap latches it off. Holding it longer keeps it open only while it is held. The main-keyboard + does nothing.
* **Numeric keypad -** switches playback to the TB pair on, and off again on the next press. The main-keyboard - and the one beside 0 do nothing.
* While the CR mic is open a thin, slowly pulsing **purple** border shows on the main screen and on every Take Display screen; while TB playback is on a slowly pulsing **yellow** glow shows just inside it.
* **Safety, built into the audio engine:** the two TB outputs are cleared every audio block, whatever the mixers have routed to them. They then receive only (1) the CR mic, while open, and (2) while something is playing back and TB playback is on, the processing mixer's main output computed with *every live input forced off*, so it can only contain recorded audio. A live microphone going out of an Ext bus therefore can never reach the TB pair, whatever the monitor modes or routing. Nothing is muted or warned about; the path simply does not exist. Covered by tests.
* The keys are read while Fermata is the front program, from any of its windows.

## Automation (Edit window)

Press **Automation** in the Edit window. Each track row then shows an automation lane over its waveform, with a small **parameter label** (click it to choose Fader, Pan, a Bus send or a Plug-in parameter, and which mixer the lane drives: the Processing mixer by default) and a **padlock** (click to lock the lane: a locked lane cannot be changed).

* Click on a lane to add a point, drag a point to move it, right-click (or Shift/Ctrl-click) a point to delete it. Between points the value ramps in a straight line (in dB for faders).
* **Points follow the audio, not the clock.** Every point remembers which piece of which take it sits on, so if the edit ripples, slides, is re-ordered, split or trimmed, the point stays on the same musical moment. If the audio under a point is removed, the point moves by a guess (like its nearest surviving neighbours) and is drawn as an orange ring so you can check it; drag it to put it right.
* While an edit plays from its window, the strip's fader / pan follows the lane (the mixer faders move by themselves). Bounce Out of the edit includes the automation. Restart playback to hear changes made to points. Stereo tracks can be automated on the fader; pan lanes are for mono tracks.
* **Bus sends:** the label menu lists the sends the track already has in the chosen mixer (set a send up on the mixer first). A send lane uses the same dB scale as the fader; the very bottom is off.
* **Plug-in parameters:** the label menu lists every automatable parameter of the VST3s in the track's four insert slots (grouped 25 to a page). The lane runs 0 to 1 (bottom to top, the plug-in's own scale) and the parameter is only sent to the plug-in when its value changes. Load the plug-in before choosing it; if the plug-in is replaced, the old lane is ignored.
* Lanes are kept per track and per mixer, saved with the project, and included in Bounce Out.

## Preamp Boost

Each input row has a **BOOST** button next to LC. It is sent to the device as its "lift" switch (the Hapi manual lists the mic preamp's Mic / Boost setting; the device's own settings call the field "lift").

## Preamp: PAD, Z HI, scroll wheel, memory

* Each input row now has **PAD** (`pad`) and **Z HI** (`z_in`) buttons as well as BOOST (`lift`). All of them follow the hardware, and the web page follows Fermata.
* The mouse wheel no longer changes a preamp gain.
* **Preamp memory.** While the devices are connected, the full state of every preamp Fermata can read (gain, MIC/LINE, 48V, polarity, low cut, PAD, Z HI, BOOST) is written, overwriting the last one, once a second (and only when something changed) to "<project name> - preamp memory.json" next to the project file. When you open the project again, Fermata waits (up to about 12 s) for the devices to answer; if the hardware differs from the memory it asks "Keep preamp settings or load stored preamp settings from last time this project was used?" with a choice box. Nothing is written until you have chosen, so the stored state is never lost. An input whose device is not connected keeps its old stored entry.

## Smaller changes (this round)

* **Take window:** each track's name plate has a little horizontal level meter (live input signal, with a short peak mark) so you can see signal before recording.
* **Zoom (Take and Edit windows):** left / right arrows zoom around the middle of the screen (in the Take window, the playhead is moved to the middle if it is on screen). The scroll bars never take the keyboard any more, so the up / down arrows always change the track heights, even after using a scroll bar.
* **Main window menu bar:** it is now a strip of its own that always stays on top of every Fermata window, laid over the top of the main window and following it when it moves. The rest of the main window is a normal window. It is only shown while Fermata is the front program (so it does not float over other programs).
* **Seeking while playing:** in the Take and Edit windows, clicking the ruler while music plays moves the playhead and carries on playing from there (dragging along the ruler in the Take window carries on from where you let go).
* **Trim window:** Space stops an audition (or the original) that is playing.

## Dark / light look (whole program)

A small sun / moon button sits in the top-right corner of every window (and the mixer's own button does the same). It switches the whole program, all windows and all mixers, between the dark console look and a light look, and remembers the choice. Anything drawn with a fixed colour of its own (for example channel colours) stays as it is.

## Importing takes, pitch correction and noise repair

**Import takes** (Take window; or drag audio files onto the window). Pick or drop all the files of a session made in another DAW.
PolyWAV files become one take each (track names are read from the file when it has them). Mono/stereo files with the same
length are grouped as one take; the take number is found in the file names, otherwise the order the files were made is used.
Files are copied into the project (24-bit WAV; 32-bit float stays float). Tracks are matched to existing ones by name.

**Pitch...** (Take or Edit window). Mark a range with keys 1 and 2, click Pitch..., choose semitones (+/-12) and/or cents.
The pitch moves but the length and timing never change (no "slower playback"). All tracks of the take are processed.
In the Edit window you can also process 0-9 s extra either side (1 s steps) so the edit point can still be moved.

**Repair...** (Take or Edit window). Mark an area a little bigger than the noise with 1 and 2. A merged spectrogram of all
tracks is shown; drag a box round the noise. Patch rebuilds that part of the spectrum from the clean audio to the left, right or both
sides; Declick mends short clicks. Keys 1/2 now also work in the Edit window.

Nothing is destructive: the results are new files in the project's `Fixes` folder, placed over the originals. **Undo fix** puts the
originals back (one level). In the Take window each fix writes full-length copies of the files, so it uses disk space.
Edit window repair needs both marks inside one piece. Overdubs are not processed.

## Take counter in the menu bar
The green (next take) / red (recording take) box with the take number now sits at the left end of the menu bar.

## Mastering window (menu bar > Mastering)
Pick where the sound comes from at the top right (normally the processing mixer and its Ext bus, plus a tail of 0-10 s for reverb to ring out).

**Virtual Master** - one file per Edit window. Tick the edits to include, drag the grip on the left to reorder, type a new file name in the list.
Formats: WAV (16/24/32-bit float), AIFF, FLAC, Ogg Vorbis, MP3 (needs the free `lame.exe` placed next to Fermata.exe - it cannot be shipped with the program).
Sample rate, bit depth, TPDF dither, peak level (one gain for all files, or each file on its own), numbering, tags (album-wide and per file).

**DDP builder** - the edits become tracks of a CD. Drag a track left/right on the timeline (or type the pause) to set the gap; dragging it back over the previous track swaps them.
"Auto PQ" sets every pause (first track starts at 2 s). The PQ list is calculated live from the positions and the pauses, so if an edit changes length the pauses are kept.
Disc and track CD-Text, UPC/EAN, ISRC, pre-emphasis and copy flags. "Make DDP" writes the DDP folder (+ optional zip and WAV + CUE sheet); "Check DDP" re-reads it.
**Important:** the DDP layout follows the published format but has not been tested with a real DDP player - open the result in a free DDP player before sending to a plant.

## Noise repair: preview, commit, revert
The repair window now stays open. Draw a box, set the controls and press **Preview**: the spectrogram redraws with the repaired sound (nothing is written yet).
**Show original / Show repaired** flips the picture; **Revert** goes back; **Commit** writes the new files (the originals stay; Undo fix brings them back). Changing any setting asks for a new Preview before Commit.
Controls (like iZotope RX Spectral Repair): **Strength**, **Direction** (left and right = sound before/after; up and down = pitches above/below; both), **Surrounding length** (% of the box) and **Before <-> After** weighting.

## Windows and the menu bar
A tool window whose title bar would end up under the floating menu bar is moved down below it (a maximised one is fitted below it).

## DDP builder timeline
Flags mark the PQ points: grey = CD start (0:00), green = INDEX 01 (track start), orange outline = INDEX 00 (start of the pause), purple (pointing back) = end of each track, red = end of CD (lead-out).

## DDP builder: playhead, audition, zoom
The timeline now runs the full width of the Mastering window. Click the ruler (or anywhere in the picture) to put the **playhead** there; drag on the ruler to scrub.
**Play** (or Space) plays the disc from the playhead with the real pauses between the tracks, through the processing mixer (automation included). Clicking while it plays jumps there.
**Play the gap** plays the pause before the selected track: 3 s before, the silence, 3 s of the track. **Track start** moves the playhead to the selected track's INDEX 01.
**Right arrow** zooms in on the playhead, **Left arrow** zooms out (also the + / - buttons; Ctrl + mouse wheel too); the wheel scrolls; **Fit** shows the whole disc; Home goes to the start.
(The arrow keys work when the cursor is not in a text box: click the timeline first.)

## Auto PQ, C, drag-to-mark, repair surround box
- **Auto PQ** (DDP builder) moved to the transport row so it never scrolls away. Its box starts on **Keep positions**: Auto PQ then makes the PQ points from where the tracks are and moves nothing (only a first track earlier than 2 s is moved to 2 s). The other choices first set every pause to 0 / 1 / 1.5 / 2 / 3 / 4 / 5 s. The selected-track panel scrolls when the window is short.
- **C** centres the view on the playhead without changing the zoom (DDP builder, Edit window, Take window).
- **Drag to mark:** in the Take window, dragging over a take's tracks sets Edit IN (1) where the mouse went down and Edit OUT (2) where it is let go. In the Edit window the same sets marks 1 and 2, when the piece under the mouse was not already selected (a selected piece still slides when dragged; the first drag over audio deselects it, so the next drag marks again).
- **Noise repair:** the dotted blue box(es) show the surrounding sound the repair learns from (before/after for left-right, above/below for up-down, all four for both), sized by the Surrounding length %.

## Automatic preamp set-up (ANEMAN), window size, keyboard focus
**Preamps > Find automatically** asks ANEMAN (the Merging network manager, port 6167 on this PC) for the devices and the patch, and sets each device's preamp channels to the ASIO inputs they are actually patched to, channel by channel, in any order. The same is done quietly when Fermata starts or a project opens (if ANEMAN is not running, your saved list is kept). Typing a number in "First input" goes back to "consecutive from here". The MT48 only appears if it is patched in ANEMAN.
Mastering opens at 1320 x 720 (windows never open bigger than the screen); the DDP bottom block and the Virtual Master settings use the width. Windows now take the keyboard as soon as they are in front, so the arrow keys, C and Space work without clicking first.

## Bars: IN Bar / OUT Bar and Search for bar
Right-click a take in a take window: two boxes, **IN Bar** and **OUT Bar** (the bars the take starts and ends in; it contains those bars and every bar between). They are saved with the project and written along the bottom of each track's waveform for that take, e.g. "Bar 17 - 39". (The old rename / remove menu is behind "More...".) Pieces sent to an edit carry the bars with them.
In the Edit window, **Search for bar** asks for a bar number, lists every take that contains it (and how many pieces of it are in this edit) and outlines those pieces in blue-green. Click the button again to clear the list and the outlines.

## Bars box while recording, Dud, draggable PQ flags, Undo
- Right-click a take (also one still being recorded): a small box with IN Bar and OUT Bar opens with the cursor already in IN. It does not block anything, so R / Space still work. Enter in IN goes to OUT, Enter in OUT saves, Esc closes. "More..." leads to rename / remove.
- **D** marks the selected take (or, while recording, the take being recorded) as a DUD: a red "DUD" is written before the bars along the bottom of the take. Press D again to clear.
- DDP builder, the PQ flags: click a flag to select that track and put the playhead exactly on it; drag **INDEX 01** (T1, T2 ...) to start the track earlier or later than its audio, drag **INDEX 00** (n.00) to choose where the pause code begins, drag **End of CD** later to add silence before the lead-out. Double-click a flag to put it back to automatic. "Set every pause to N s" also puts all flags back. Moving a track's box still changes its pause; moving flags never moves audio.
- **Undo / Redo** (Ctrl+Z / Ctrl+Y, or Ctrl+Shift+Z): the last 30 steps of the takes' details (bars, duds, names, positions, removed takes), the edits (pieces, fades, automation, repairs) and the Mastering / DDP settings. Marks, playheads, mixer faders and preamps are not undo steps, and a take that is being recorded is never touched. The status line says what happened.

## PQ flags the SADiE way
As in SADiE, **the end of a track is the INDEX 00 of the next track** (the player counts down from there to the next INDEX 01). So the purple **End Tn** flag is draggable: it moves that INDEX 00. It can go anywhere after the previous track's INDEX 01 and up to the next INDEX 01; putting it on top of INDEX 01 means no countdown, a continuous join. The green INDEX 01 flags are draggable independently (INDEX 00 is never left after INDEX 01), the first track's INDEX 01 can be dragged later to hide a track in the pregap, and the last track's End flag (and End of CD) move the lead-out, adding silence at the end. Double-click a flag to put it back to automatic. The audio never moves when a flag does.

## Dither (every new audio file, and the mixer output)

* **Every audio file Fermata writes is dithered whenever it loses word length.** 24-bit and 16-bit integer files are made with TPDF dither (triangular, ±1 LSB) added before rounding, so the quantisation error is noise and not distortion. This covers Bounce (24-bit files, and the final pass when "Normalise" is on), the pitch / noise-repair files, take import when a source is wider than 24 bit, and every Virtual Master / DDP export (16 or 24 bit, with the Virtual Master "Dither" box still able to switch it off). 32-bit float intermediates are not dithered, because nothing is lost in them. Recordings are written exactly as the converters deliver them (24-bit in, 24-bit out), so there is nothing to dither.
* **Output dither on every mixer.** In the METERS column of each mixer, under "Reset peaks", the **OUTPUT DITHER** box offers Off / 24 bit / 16 bit (default 24 bit). It is applied to the mixer's driver outputs just before the audio goes to the interface, is saved with the project, and is separate for each mixer. Digital silence stays silent, and anything at or above full scale is passed on untouched. Bounces and exports never use it (they are dithered once, when written).
* **Disc master (DDP, WAV + CUE) is always 44.1 kHz, 16 bit, TPDF-dithered**, whatever the Virtual Master is set to: the programme is rendered in 32-bit float, sample-rate converted to 44.1 kHz, dithered to 16 bit, and the DDP builder refuses any file that is not 44.1 kHz / 16 bit.

## Pitch curve (draw the pitch against time)

For a note or a whole passage that drifts flat or sharp. In the **Edit window** mark the part with keys 1 and 2, then press **Pitch curve...** (the same button exists in the Take window, using that window's Edit IN / OUT marks).

* The lane shows the marked audio faintly behind a level line. **Click** to add a point, **drag** to move it (up = sharper, down = flatter; +100 cents at the top, -100 at the bottom, 0 in the middle), **double-click or Delete** removes one, **Reset line** clears all. Any number of points; straight lines join them, and the line stays level before the first and after the last point. Your example: a point at 0 cents at 5 s and one at +15 cents at the end raises the pitch gradually over those 10 seconds; add more points for a slow drift followed by a sudden fall.
* **Audition** calculates the corrected sound (it cannot run live; a few seconds per take) on every track and plays it through your mixers, from 2 s before the marked part to 2 s after. **Accept** keeps it, **Revert** puts the original back; closing the window without Accept also reverts. If you change the line after auditioning, press Audition again (Accept waits for that).
* The audio is replaced by new files (dithered 24-bit, in the project's Fixes folder or next to the take's files); the originals are never touched, and **Undo fix** brings them back after an Accept. In the Edit window "Process extra, each end" can correct more audio each side (the line stays level there) so the edit points can be moved outward later.
* Method: the same phase-vocoder pitch shifter as the fixed Pitch tool, with its analysis hop and play-back speed following the line, so the length never changes. In the tests a 440 Hz note that sinks 30 cents is brought back within 2 cents all the way along, and a jump of 40 cents is reproduced to within 2 cents.

## Loop, and listening in the repair and pitch-curve windows

* **Loop** (button next to Play, or key **L**) in the Edit and Take windows: when lit, Play repeats the marked area over and over, with no gap, until you press Stop. Edit window: the area between marks 1 and 2. Take window: Edit IN / OUT [1] [2] if set, otherwise Bounce IN / OUT [I] [O] (the same marks "Play marked" uses); both Play and Play marked loop. Switching Loop on while playing starts the loop at once; switching it off lets the pass finish and stop. With no area marked it plays normally and says so in the status line.
* **Pitch curve window:** Audition now shows a progress bar (full width, with a percentage) while the corrected sound is calculated. While it plays, a red playhead runs across the line with a dot on the line itself and the line's value in cents shown below. **Play** replays the current sound (the original before an Audition), **Loop** goes round the marked part, **Stop** stops.
* **Noise repair window:** it now has a transport. **Play** plays the selected box (with a second of sound either side), or everything if nothing is selected, through your mixers with a playhead moving across the spectrogram - no repair needed, so you can listen to find what has to be mended. **Listen to** chooses Original or Repaired. **Preview and listen** shows the repaired spectrogram and then plays the repaired sound (it makes the repaired audio ready first, with a progress bar), so you hear it "through" the repair; Play with Loop lit goes round it as often as you like, and changing the box or any setting brings back the original until you Preview again. Commit keeps what you heard (no second calculation); closing the window without Commit puts everything back.

## Status squares (Pre-Rec, C-R, P-B)
The coloured borders that used to flash round the screen are gone. Three squares now sit at the right end of the top menu bar and are always visible: dark when off, lit when on.
- **Pre-Rec** (red): session mode is on.
- **C-R** (purple): the control-room mic is open (talkback).
- **P-B** (yellow): playback of recorded material to the talkback output.
Hover a square for a reminder. Take Display screens keep their own optional talkback glow.

## Update of 6 October (take / edit / repair / export)
**Take window:** stereo tracks show two meters (L and R). Enter after typing the piece name confirms and leaves the box. Takes cannot be dragged; they stay in time order. Waveforms are drawn as one filled shape, so no vertical stripes at any track height. Key **5** removes the 1/2 flags; with no flags, a clicked take plus Pitch / Pitch curve / Repair loads the whole take. Small **In n / Out n** flags show which edit number each take's start and end went to.

**C-R talkback:** any number of mic inputs can be the C-R mic (the C-R button toggles each input; they are summed at unity).

**Edit window:** numbered edit points: 1 = start of the first piece, 2 = join of piece 1 and 2 ... and the final fade-out. Where fades do not overlap the flag sits midway between the in-start and out-finish. Double-click a flag to open the Trim window, which shows the number and can also load a fade at the very start or end (a one-sided fade). **Ctrl+click** selects a whole piece, which can then be dragged sideways (release Ctrl once selected); a plain drag marks a portion. Key **5** clears the 1/2 flags; with no flags a selected piece is processed whole.

**Noise repair:** crossfades are linear. The whole loaded area is processed. **Accept** keeps the change and leaves the window open (several changes can be made); **Finish: write it back** renders everything between In and Out at once. **Space** plays/stops here and in both pitch-curve windows; playback covers only the loaded part.

**Pitch curve:** Preview button, a **Hear corrected** switch (on = changed audio, off = original, carried on from the same place), new points start at 0 cents, whole cents, within 1 cent snaps to 0.

**Windows and menu:** a new window always opens on top, even over a full-screen one. The menu bar is always shown. Order: ... Take display, Organiser, Media, Mastering (right). The session button is called Pre-Rec.

**Virtual Master export:** FLAC **Compression** 0-8 (lossless, only size and speed change); MP3 quality box as before (CBR / VBR). A **File name** box for each file (separate from the edit name). **Also export every file as** WAV / AIFF / FLAC / Ogg / MP3: tick any combination and one Export makes them all. MP3 still needs lame.exe beside Fermata.exe.

**Mixer:** a **Mono** button under Phase scope in the METERS column. Lit amber = the mixer's outputs carry (L+R)/2 on both sides, for checking mono compatibility. Listening only, never in files, off when the project opens.

Not done yet: Shift+drag of a clip to another track (placeholder only).

## Tracks of an edit (Edit window, "Tracks..." button)
* Every edit can have **its own list of tracks**. By default it shows every project track; the first time you use **Tracks...** it gets a list of its own (saved with the project).
* **Tracks...**: add a new mono / stereo track, add a track the project already has, remove a track (its audio leaves this edit; **Ctrl+Z** brings it back), or show every project track again. A new track has a mixer strip in every mixer at once, and no microphone input.
* **Drag a track's name up or down** to change the order (a yellow line shows where it will land). The order belongs to that edit only.
* **Strips and tracks that do not match:** when an edit is opened (and before Play), if its audio belongs to tracks that are no longer in the project, a window asks "There are more audio tracks than mixer strips. Would you like to add the missing strips?" Yes puts the tracks back (same identity, no input) with a strip in every mixer; No leaves that audio silent. A mixer with more strips than the edit has tracks is fine: the extra strips just stay quiet.
* Not yet: the mixer windows still list strips in the project's order (they will follow an edit's own order when each edit gets its own mixer), and the Take window has no track list of its own.

## Move a clip to another track (Shift + drag, Edit window)
Hold **Shift**, press on one track's clip in a piece (or overdub) and drag up or down: a yellow frame shows the track it will land on. Let go and the clip is on that track, at exactly the same time. If the target track already has a clip in the same piece, the two swap tracks. Both tracks must have the same number of channels (mono to mono, stereo to stereo); otherwise a notice appears and nothing changes. The audio files are not touched, volume changes and fades stay with the clip, and Ctrl+Z undoes it. This is also how audio gets onto a track you added with "Tracks...".
* **Audio always goes to its own track.** A piece sent from a take window lands on each track by the track's identity, not by the row it had in the take: if "vln1" is the 3rd track in the take window and the 10th in this edit, vln1's audio appears in the 10th row (and plays through its own mixer strip). A track the edit does not list yet is added at the bottom of the edit.
* **Shift + drag** a clip (one track's part of a piece) up or down to another track: it keeps its place on the timeline. If the other track already has a clip in that piece the two swap. Both tracks must have the same number of channels.

## Import into the Edit window
The **Import...** button in the Edit window (or just drag audio files / a folder onto the window) brings a multitrack recording straight into that edit: a multichannel / polyphonic WAV, or one mono / stereo file per track (WAV, AIFF, FLAC). The same file-reading as the Take window's import is used: tracks are matched to your existing tracks by name (new ones are created), every track goes on its own row, and each take found is placed in the edit one after the other, ready for mixing. The files are copied into the project's audio folder (in a folder named after the edit) as 24-bit WAV; the originals are not touched. The edit's sample rate must match the audio.
(Importing a whole recording day into a **Take window** has been there already: its Import button, or drag the files onto it.)

## Control port (Stream Deck)
Fermata listens on 127.0.0.1 port 7788 (this computer only) so a Stream Deck can press its main buttons and show their state. One line of JSON per message. Commands: `record`, `playpause`, `talk` (with `down` true / false: a tap under 1 second latches the C-R mic, a longer hold is momentary), `playback` (the P-B speaker), `mixer` (with `index`: 0 = processing mixer, 1 = second mixer), `prerec`. Fermata sends back a `state` message whenever something changes (recording, next / this take number, playing, C-R open, P-B on, Pre-Rec, mixers open, the time of day and the session time left). Put `remotePort` in the settings file to use another port (0 = off). The plugin is in the separate Fermata Stream Deck package.

## Transport everywhere, zoomable repair windows, bar search in the take window (latest)
* **Full transport in the Take and Edit windows:** `|<` `<<` Play `>>` `>|` (Home / End also go to the start / end). You can play with no take or region at all, and past the end of what is there (silence); bouncing and mastering still use the real length. The timelines follow the playhead and grow as it moves on.
* **Noise repair and pitch-curve windows:** left / right arrows zoom in time (right = in), up / down zoom in pitch (repair) or cents (pitch curve). A time ruler sits above the picture: drag it to move left / right, double-click it to see everything. **Play (and Space) plays only what you can see**: zoom in to hear a short piece, out to hear more. The existing transport buttons are used; there is no extra Play button.
* **Pitch scale box (repair window):** linear (even Hz) through three blends to musical (every octave the same height). Drag the pitch axis up / down to move the pitch view.
* **Round colour control (bottom left of the picture):** drag it up / down, click it, or use the wheel to step through eight colour maps and level ranges (standard, strong sounds only, quiet sounds, very quiet noise, greys, rainbow) so faint noises show. Double-click returns to standard. (Modelled on the colour roll of spectral editors such as CEDAR Retouch; its exact behaviour is not publicly documented, so this is our own interpretation.) Zoomed-in detail is limited by the resolution the area was scanned at.
* **Pitch curve points:** the first point anchors at 0 cents; later points go where you click, snapping to 0 near the middle line. Whole cents only.
* **Search for bar** moved from the Edit window to the Take window: type a bar and every take that contains it is framed in blue-green; the button shows how many.

## Windows remember their size and place; the mixer opens tall enough (latest)
* **Every window** (take, edit, mixer, mastering, scope, bridge, media, ...) reopens where and how big you left it. This is kept **in the project file** (so each project has its own), not in the program. It is saved with the project; it does not count as a change to the project.
* **The mixer** opens tall and wide enough to show every strip completely (names at the top and bottom, no scroll bars), even if that is taller than the screen: the title bar stays on screen and the rest hangs off the bottom. It can be made smaller (scroll bars appear) but never needs to be bigger than that. Its size and place are remembered like the other windows.

## Window fighting and the menu bar (latest)
* **Take and Edit windows no longer fight for the front.** Cause: a scroll bar that moved by itself (a timeline following the playhead while something plays) gave its window the keyboard, which pulled that window forward; with two windows scrolling, each pulled the other in turn. A scroll bar now takes the keyboard only in the window you are working in, and no window is pulled forward just to receive the keyboard.
* **The menu bar strip (Project, Audio settings, Pre-amps ...)** is no longer on top of every program. It belongs to the Fermata main window: it stays in front of the other Fermata windows, goes behind a program you put on top of Fermata, can be covered by one, and minimises and restores with the main window.

## Any outputs can be talkback (latest)
* **TB on the Driver Outputs list:** click TB on any output and that output and the next become a stereo talkback pair (to the studio). You can make as many pairs as you like. On the last output the pair is mono (the missing right side is simply ignored, no warning). Click again (on either output of a pair) to turn it off. Like the CR mic, which can be any input(s), the talkback outputs can be any output(s).
* The safety is unchanged: a talkback output carries only the CR mic and recorded playback, whatever the mixers send there. Older projects with one pair open as before.

## Alt Mixer for the Stream Deck's second Mixer key (latest)
* Every mixer except the processing mixer has a small key-pad button in its top bar (right of Copy mix). Lit (glowing) = **this is the Alt Mixer**: the one the second Mixer key opens and closes. Only one mixer can be the Alt Mixer at a time. If none is chosen, the first cue mixer is used; with no cue mixer the key reads "Alt-Mixer" and does nothing. The key shows the Alt Mixer's own name, and follows it when it is renamed.
* The Stream Deck keys have new pictures: Record (red dot with the take number on green; all red "TAKE" while recording), Play / Pause (white, with the fermata sign), and a little mixing-desk drawing with the name for both Mixer keys.

* **Mixer window size, second fix:** the window's limits were worked out from the window's own size at the moment it was first fitted to the strips, when that size was still a placeholder, so it could shrink itself to a tiny size and open that way. The limits now use the window's frame, and a mixer window that has never been fitted opens showing every strip completely (a size you set yourself afterwards is still remembered).

## Project folders, Save as, Create copy, crash recovery (latest)
**Folder layout.** A new project asks where to keep it and what to call it, and makes a folder with that name:
`Projects 101/` holds the project file, `Recorded Media/` (all recorded audio), `Bounced Media/` (the default place for exports from the Take and Edit windows) and `Bounced Media/Mastered Audio/` (the default for the Mastering tab). Paths inside the project are stored relative to the project folder as well as absolute, so copy-pasting the whole folder by hand (to a drive, another computer) is a working copy.
**Project menu.** *Save as...* asks "Copy everything" or "Project file only", then a name and place, then carries on in the new project. *Create copy of entire project...* copies every file (also audio kept elsewhere, into `Recorded Media/From elsewhere`), reads each back and compares it, and stays in the original project.
**Crash recovery.** The take is saved the moment recording starts, and WAV headers only get their length when a file is finished, so after a crash the files look empty. When a project is opened, every take file is checked and its header put right from the real size of the audio; the take's length is restored and a "Recording recovered" message lists what was kept. Limits: WAV files over 4 GB; what survives a power cut depends on what Windows had written to the disk.

## The program is now called Fermata everywhere (latest)
The program, its .exe, its settings folder, its log (`Documents\\Fermata\\fermata-log.txt`), its default project folder and the project files (`.fermata`) all use the new name. Projects made under the old name (`.takedaw`) still open, and the old settings are carried over once.

## Zoom follows the playhead everywhere (latest)
As in SADiE: the arrow-key (and +/-) zoom is centred on the playhead. Zooming in moves the playhead toward the middle of the window and keeps it there; near the start of the timeline it can't go further left than time 0, so it stays toward the left and slides back there as you zoom out. This now applies to the Take window, the Edit window, the Mastering timeline, and the spectrogram / pitch-curve windows (while the playhead is running; otherwise they zoom around the middle). The Trim window always zooms around the edit point.
The Take and Edit timelines always have empty room after the end of the material (as much again as the material, and at least 10 minutes; 24 hours at most) for dragging more audio onto, and zooming out goes as far as that whole length fits on screen, so a 2.5 hour concert can be seen end to end.

## Mixer window: opens at the full size, Fit button, log (latest)
A mixer window now always opens at exactly the size that shows every strip (only its position is remembered per project, not a size that may have been saved while it was too small), and there is a **Fit** button in its top bar that does the same at any time. The numbers (strip size, window frame, screen area, resulting window size) are written to `Documents\Fermata\fermata-log.txt` each time, so a window that still comes out wrong can be diagnosed from the log.

## Start-up asks where the project goes (latest)
Fermata no longer creates a project by itself. When it starts, a screen asks "Where do you want to save your project?": **New project...** (pick the place, type a name; a folder with that name is created holding the project file, Recorded Media and Bounced Media/Mastered Audio), **Open an existing project...**, **Open the last project: ...**, or **Quit**. Until a project is chosen nothing is saved and Record does nothing. Double-clicking a project file in Explorer still opens it directly.

Mixer window size, found from the log: Windows applied the window's biggest-size limit to the whole window (frame and title bar included), so a mixer set to exactly the strips' size was cut short by about 14 x 37 points, which made scroll bars appear. The limit now has some slack. The start screen shows only its buttons.

## Export for Processing (Oct 2026)
- `WaitingPiece` (core/Edit.h) lives on `EditRegion::waiting` and `TakeGroup::waiting`; persisted by Project.cpp (file + rel paths; "channels" key so project copy includes the files).
- `audioops::exportRange / checkReplacement / spliceFile` (core/AudioOps.cpp), tested in tests/core_test.cpp.
- Edit window: the region keeps pointing at the ORIGINAL files while waiting; re-link swaps in the processing files (fileStart = srcIn). Take window: re-link splices into new full-length files (5 ms crossfade).
- UI: fixtools::exportForProcessingEdit/Take, relinkEdit/Take (FixTools.cpp); `drawWaitingBlock` in WaveDraw.h.

* **Repair / De-Click workflow (latest):** **Fix** does the repair at once (no auto-play) and keeps it as a temporary step; **Play** (Listen to: Fixed) auditions it; **Undo** takes the last fix off (stackable); more boxes can be drawn and Fixed in the same window, earlier fixes stay; **Write back to file** renders all fixes between the marks. The round colour control now turns smoothly (the colour maps and level ranges blend continuously). De-Click: multi-pass detection with hysteresis, longer gaps (to 1500 samples, predictor order up to 128), and a box of 30 ms or less is mended as a whole.

* **Low-end detail (Spectral Repair / De-Click window):** a "Low-end detail" box (normal 2048, fine 8192, very fine 16384, finest 32768 point analysis) redraws the picture with a longer window: low notes separate, sudden sounds smear. **Waveforms:** solid body in a slightly darker shade with a thin bright line round the outer limit (SADiE / Pyramix style), drawn in WaveDraw.h fillEnvelope.

* **WaveColour (new):** rainbow button next to the playhead button in the Take and Edit windows (same setting in both; default off; lit = on). Each file is analysed once in the background (ui/WaveColor.h: 4096-sample blocks, FFT): hue = spectral centroid (100 Hz red, 1 kHz green, 8 kHz+ blue), saturation = tonality (spectral flatness), BLACK = more than half the energy below 100 Hz (a kicked mic stand), grey = silence. A pixel column shows the colour of the loudest block under it. Drawn as a horizontal gradient through fillEnvelope.

* **Info buttons:** explanatory sentences that took up room (Design window pages, Ext Bus note in the mixer strip, Media window, Organiser hint, Take Display hint, Preamp setup, the pitch / pitch-curve / repair / de-click dialogs) now sit behind a small blue "i" (`InfoNote` in Uikit.h; a drop-in for the Label that showed them). Clicking it opens the text in a call-out box. The File menu's "Save now" is now "Save".

* **Automation playback position (bug fix):** the engine read the lanes at "samples played since Play" instead of the timeline position, so playing from the middle or looping gave wrong fader values. `PlaybackSession::getTimelinePosition()` (start + played, wrapping with the loop) is now used.
* **Automation value box:** while a point is set or dragged a box beside it shows its value (dB / L-C-R / plug-in value) and time. Values snap to 0.1 dB; hold **Alt** while dragging for ten times finer movement.
* **Spectrogram:** two analysis layers (2048-point window at full rate, and the same size after 8x decimation = 8x longer window, 3 Hz rows) blended between 250 Hz and 1.5 kHz (`MergedSpectrogram::level`). The "Bass detail" box: blended / off / extra fine (16x, 150-800 Hz).
* **Spectral Repair / De-Click in the Edit window across edit points:** the marks may now cover several pieces. The picture is the whole marked area, piece after piece; each piece is repaired from its own source file (the part of the box that falls in it) and put back in its own place, so edit points and fades are kept.

* **Quit asks first:** closing Fermata (window close or Quit) always asks "Save and quit / Quit without saving / Cancel" (Main.cpp systemRequestedQuit); Cancel keeps working.

* **Fix joins are not edits:** the joins made by Pitch, Pitch curve, Spectral Repair, De-Click and Export for Processing carry `fixIn` / `fixOut` flags on the pieces (EditDef::isFixJoin). They are drawn as a small grey "fx" flag with no number, are left out of the edit count (also in the Trim window caption), and "Accept, next / previous fade" skips them; on a fix join those two buttons are hidden. They can still be opened and trimmed (click + T, double-click, or the **Trim Window** button, formerly "Trim join").

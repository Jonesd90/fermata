# Fermata

Recording, taking and editing for classical music, on Windows. Fermata records every armed input to its own file, keeps every take, and lets you assemble, trim, repair, bounce and master the result in one program (ASIO audio, VST3 plug-ins).

## Installing

Fermata is a single program: `Fermata.exe`. Put it anywhere (a folder on your desktop is fine) and double-click it. The first time, Windows may say it "protected your PC" because the program is not signed: click **More info**, then **Run anyway**.

## Starting

When Fermata starts it asks where to keep your project: **New project...** (choose a drive or folder and give the project a name), **Open an existing project...**, or **Open the last project**.

Fermata makes one folder with the name you give, and everything for the project lives inside it:

- the project file (`.fermata`)
- **Recorded Media**: every recorded audio file
- **Bounced Media**: files bounced from the Take and Edit windows
  - **Mastered Audio**: files made in the Mastering window

Copying that one folder to another drive or computer is a complete copy of the project. **Project > Save as...** saves under a new name; **Project > Create copy of entire project...** makes a checked copy of everything (for example onto an external drive) and stays in the project you are in.

If the program or the computer stops during a recording, nothing recorded is lost: when you open the project again the recordings are put right and appear as takes.

## First set-up

1. **Audio settings**: choose the ASIO driver and your device, set the sample rate, press Apply.
2. **Project Designer**: name your inputs and outputs, add tracks (mono, stereo, surround) and choose the input of each. Sound reaches the outputs through an **Ext bus**: add one, send your tracks to it, and choose its driver outputs on its strip in the mixer.

## Keeping CPU cores for the audio (Audio settings > CPU cores...)
For recording on a laptop or a busy PC. Tick one or two CPU cores and Fermata keeps its audio (the driver callback, the disk writing and the playback reading) on those cores only and everything else on the others; with the second box ticked it also asks the other programs to stay off them (what Process Lasso does). Press "Find the quietest cores" to let Fermata measure which cores Windows is using for driver interrupts and tick the calm ones. It is off until you switch it on, and closing Fermata puts everything back.
Windows' own driver work (DPCs) cannot be moved by any program: if you still get clicks, run LatencyMon to see which driver is the cause.

## Recording

Arm the inputs you want (ARM next to each input), open a **Take Window** (one per piece), and press **R** (or REC). **Space** stops. Each armed track makes its own file, named like `005 - Symphony 2 - violin.wav` (with BWF metadata: the piece, take and track, and the time of day of its first sample).

**Pitch pipe** and **Click** (on the menu bar) play to the outputs you tick - the stage speakers, the performers' headphones, or only your control-room speakers - and are never recorded. The pitch pipe plays single notes or held chords on any reference A in seven tunings (equal, just, Pythagorean, meantone, Werckmeister III, Vallotti, Kirnberger III) built on the key you choose, a chord pure major or pure minor if wanted; the click runs on its own (start / stop by hand, not with Play or Record) with an accented first beat, five sounds (a soft pitchless "fum" and a little drum kit among them) and, if wanted, a very quiet hi-hat on every quaver. The pitch pipe's keyboard shows two octaves and its Tone slider sets how soft the reed is.

**Spot mics lined up with the main pair** (**Align**, in the take and edit windows). Mark the tracks Main, Spot, Distance or Not used, record the run-through and press Analyse: each spot is delayed by exactly as much as it hears its instrument earlier than the main pair, in every mixer (the files are not changed), with the pans taken into account. Every piece keeps its own measurements; an edit can be measured in post.

**Take sheet** (take window, or the Project menu for every take window): every take with its time, length, bars, markers and problems, as a PDF and a CSV in the project's Take Sheets folder.

## Editing

In a Take Window, click the ruler to place the playhead, mark the part you want with **1** and **2**, and send it to an **Edit Window** with **3** (added after the last piece) or **4** (placed at the edit's playhead); **5** clears the marks. In the Edit Window drag pieces, open a join with **T** to trim and crossfade it, and use **Spectral Repair**, **De-Click**, **Pitch** and **Pitch curve** on marked parts. Everything is undoable with **Ctrl+Z** / **Ctrl+Y**.

**Spectral Repair and De-Click.** Mark a part (keys **1** and **2**) and open either window: all tracks are shown as one spectrogram. In Spectral Repair draw a box round a noise and drag its edges or corners to adjust it; the dotted areas beside it show which surrounding sound the repair learns from (shared out by the *Before <-> After* slider). De-Click is the same window, but it finds and mends clicks (a box limits the search to a stretch of time). To tidy a doubled or messy consonant to one, choose the method *Attenuate a stray consonant* and draw the box round the early or late copy only (from about 2 kHz up): every track is matched to the side away from the main consonant, so the stray copy comes right down and the main one stays. To make one spot louder or quieter (a resonance, a ring), draw the box round it and press **G**: a slider from silence to +18 dB changes just that box.

**Export for Processing.** To correct audio in other software (e.g. iZotope RX): mark the part, press **Export for Processing...**, and name the job. The part of every track is saved in the recorded format to *Processing Media/<name>* in the project folder. In Fermata the part shows as *Waiting for corrected audio* (the original still plays). Open the files in the other program, fix them and save **over the same files** (same length, sample rate and channels; Fermata keeps nothing open). Then right-click the block and choose **Re-link corrected audio**; the waveform is redrawn. In a Take Window the corrected part is written into new copies of the take's files (the old ones stay on disk).

## Bouncing and mastering

**Bounce Out...** (Take and Edit windows) renders faster than real time through the mixer you choose and writes 24-bit WAV files. The **Mastering** window builds the programme (PQ, DDP, exports); its output goes into Bounced Media / Mastered Audio by default.

## Useful keys

| Key | What it does |
| --- | --- |
| R / Space | Record / stop (play in a window) |
| 1, 2 | Mark the edit IN / OUT |
| 3 / 4 | Send the marked part to the end of the edit / to its playhead |
| 5 | Clear the edit marks |
| M | Open / close the processing mixer |
| B | Open / close the meter bridge |
| C | Centre the window on the playhead |
| P | Put the 1 / 2 flags round the whole selected take (sample-exact) |
| D | Mark a take as a dud |
| L | Loop (the circular-arrows button) |
| Home / End | Go to the start / end |
| Left / Right arrows | Zoom out / in (around the playhead) |
| Up / Down arrows | Make the tracks smaller / bigger |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+A | Select every take (take window) |
| F1 | Snip at the playhead (edit window): an unnumbered join ("sn") to roll a moment out in the Trim window |
| Ctrl+Shift + drag | Move the selected takes' clips on a track onto another track (take window) |

## If something goes wrong

Fermata keeps a log in `Documents\Fermata\fermata-log.txt`. Send that file with a description of what happened. If the program ever seems to freeze for a moment, the log has a line starting "Stall watch" saying how long and what it was doing.

**Display Organiser:** a picture of your screen: pick a shape, click each part to choose its window (any take window, edit or mixer of the project: a closed one is opened), drag the lines to change the split, press Arrange. Layouts can be saved with the project; the program's own (Recording, Editing ...) take the windows you used last.
**Fades:** the start and end of an edit (and of each take) fade for 25 ms by default; drag the top corners to change it.
**Bounce Out:** *Between marks 1 and 2* uses the 1 and 2 flags. P puts them round a selected take (take window) or the selected pieces (edit window).

**Gain and Fader automation.** In the Edit Window, the automation lane menu has both: *Gain* changes the level going INTO the mixer strip (before the inserts and the fader), *Fader* changes it after the inserts. Use Gain to even out a level before a compressor, and Fader for the final balance.

**Choosing tracks (Alt + drag).** Dragging over the audio marks 1 and 2 on every track. Hold **Alt** while you drag and only the track(s) the mouse covers are marked (start on one track and drag up or down to include more). Pitch, Pitch curve, Spectral Repair, De-Click, Export for Processing and the volume change below then act on just those tracks. **5** clears the marks and the track choice.

**Moving clips onto another track (Ctrl + Shift + drag).** In a Take Window, select the takes (**Ctrl+A** for all of them, or Alt + drag), hold **Ctrl + Shift** and drag one of their clips up or down onto another track: the clips those takes have on that track go onto the other track and stay where they are in time (the pieces of edits made from them go along). For an import that put everything on new tracks with a word in front of the names ("VivienneSPOT1" instead of "SPOT1"), right-click the track's name: *Move its clips onto SPOT1*, or *Move the clips of all ... tracks named like this*, which does them all at once (numbers at the end of the names are ignored: "Rorate Caeli SPOT4 19 18" goes onto SPOT4) and then offers to delete the emptied tracks. **Ctrl+Z** puts the clips back. To avoid this when importing, tick **One set of tracks** in the import window: each file then goes on the track named in its file name.

**Visual guide track (edit window).** Right-click a track's clip in a take window - for example the ORTF or omni pair of the run-through you recorded first - and choose *Use as edit window visual guide track*: that clip (the part between 1 and 2, if they are set in that take) appears in a grey **GUIDE** lane above the edit's tracks. Drag it sideways to line it up with the edit (it can start before 0:00) and compare the gaps between the phrases. Its **S** button makes Space play only the guide; the **Guide** button at the top of the edit window shows or hides it; right-click it to move its start to 0:00 or the playhead, or to remove it. It never plays with the edit and is never bounced.

**Volume change between the marks.** Right-click on the audio with marks 1 and 2 set and choose *Volume change between the marks...*: each marked track gets a fader from +30 dB (top) through 0 (no change) to -inf (bottom). The level glides to the new value over the fade length at the start and glides back to where it was at the end. The old *Volume change from here on* is still there.

**Colour:** in the colour panel, the strip under the swatches slides smoothly through every hue.

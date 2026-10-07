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

## Recording

Arm the inputs you want (ARM next to each input), open a **Take Window** (one per piece), and press **R** (or REC). **Space** stops. Each armed track makes its own file, named like `005 - Symphony 2 - violin.wav`. There is no click track.

## Editing

In a Take Window, click the ruler to place the playhead, mark the part you want with **1** and **2**, and send it to an **Edit Window** with **3** (added after the last piece) or **4** (placed at the edit's playhead); **5** clears the marks. In the Edit Window drag pieces, open a join with **T** to trim and crossfade it, and use **Spectral Repair**, **De-Click**, **Pitch** and **Pitch curve** on marked parts. Everything is undoable with **Ctrl+Z** / **Ctrl+Y**.

**Spectral Repair and De-Click.** Mark a part (keys **1** and **2**) and open either window: all tracks are shown as one spectrogram. In Spectral Repair draw a box round a noise and drag its edges or corners to adjust it; the dotted areas beside it show which surrounding sound the repair learns from (shared out by the *Before <-> After* slider). De-Click is the same window, but it finds and mends clicks (a box limits the search to a stretch of time).

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
| I, O | Bounce IN / OUT marks |
| M | Open / close the processing mixer |
| B | Open / close the meter bridge |
| C | Centre the window on the playhead |
| P | Mark the whole selected take |
| D | Mark a take as a dud |
| L | Loop |
| Home / End | Go to the start / end |
| Left / Right arrows | Zoom out / in (around the playhead) |
| Up / Down arrows | Make the tracks smaller / bigger |
| Ctrl+Z / Ctrl+Y | Undo / redo |

## If something goes wrong

Fermata keeps a log in `Documents\Fermata\fermata-log.txt`. Send that file with a description of what happened.

**Window organiser:** you can choose any take window, edit window or mixer of the project (a closed one is opened); the main window is left as it is.
**Fades:** the start and end of an edit (and of each take) fade for 25 ms by default; drag the top corners to change it.
**Bounce Out:** *Between the I/O flags* uses the I and O flags, or the 1 and 2 flags if those are the ones you set.

# Fermata - version history

Version numbers are MAJOR.MINOR.PATCH (for example 2.1.3). The number is set in one place, the first lines of `CMakeLists.txt`, and appears on the splash screen, in the first line of `fermata-log.txt` and in the properties of `Fermata.exe`.

- **PATCH** (2.0.1, 2.0.2 ...): bug fixes and small tweaks only.
- **MINOR** (2.1.0, 2.2.0 ...): a new feature or a visible change.
- **MAJOR** (3.0.0 ...): a large change in how the program works.

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

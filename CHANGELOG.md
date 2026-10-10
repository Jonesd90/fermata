# Fermata - version history

Version numbers are MAJOR.MINOR.PATCH (for example 2.1.3). The number is set in one place, the first lines of `CMakeLists.txt`, and appears on the splash screen, in the first line of `fermata-log.txt` and in the properties of `Fermata.exe`.

- **PATCH** (2.0.1, 2.0.2 ...): bug fixes and small tweaks only.
- **MINOR** (2.1.0, 2.2.0 ...): a new feature or a visible change.
- **MAJOR** (3.0.0 ...): a large change in how the program works.

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

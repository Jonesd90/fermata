# Fermata - the third deep code sweep: the whole program (brief for a Fable 5.1 session)

**How it starts.** The person switches this conversation to Fable 5.1 and says **"start the sweep"**. Then:
- Read this file, then the sections of `docs/DEVELOPMENT-NOTES.md` and `CHANGELOG.md` for the areas you touch.
- Do **one area per build**, in the order below, starting with area A.
- Before each build, tell the person in plain words what you found and what you plan to change, and wait for "Build it" / "Yes". Their standing rule: explain first, no code until asked.
- After a build has been tested they say "carry on" (or "next area"): go on to the next area.

**Effort.** Areas A, B and C (the audio thread and threads, recording and saving, files that leave Fermata and come back) at **xhigh**; every other area at **high**.

---

## What this is

Fermata is a classical-music recording and editing DAW for Windows: JUCE 8.0.9, C++17, MSVC, built by GitHub Actions, namespace `td`.

**This sweep checks EVERYTHING - the person asked for the whole program, not only what is new.** Every source file in `src/` (and `src/reharmoniser/`) belongs to one of the areas below (see "Which files belong to which area"); read each one, old or new. Where an earlier sweep already checked old code and it has not changed since, a lighter read is fine, but nothing is skipped.

The history:
- First sweep: areas A-F, versions 3.1.2 to 3.1.7 (26 October 2026), everything up to 3.1.1.
- Second sweep: areas A-E, 3.18.2 / 3.18.3, everything up to 3.18.1; then the clean-up 3.18.5 and 3.18.6.
- Third sweep: all nine areas at once (3.27.1, 3 November 2026), everything up to 3.27.0 - the findings and what was checked are in the 3.27.1 section of `docs/DEVELOPMENT-NOTES.md`. A fourth sweep starts from there (and from the "Not fuzzed" list in that section).
- 3.19.0 to 3.27.0 were first swept in 3.27.1 - **the closest look inside their areas** when the next sweep comes round again:
  - 3.19.0 save this window / the quit list; 3.20.0 the Trim window scroll bar and Match edit; 3.21.x N and V bar keys, the quit list's mixer comparison;
  - 3.22.0 / 3.24.0 sending to linked programs (RX, Melodyne): `RxLink.*`, `RxWatch.h`, `WaitingPiece::rx` / `program`, `RegionFile::rxFrom`, `Project::rxSeen`, the once-a-second watcher, Export for Processing's menu;
  - 3.23.0 stereo track from two mono tracks (`StereoJoin.h`, `audioops::joinToStereo`, `keepWindow`, `undoReset`); send dials that do not double the fader (`Project::sendDialBuses` / `faderFeeds`, `MixerComponent::routeSignature`); the ruler far out (`timefmt::rulerSteps`);
  - 3.25.0 dragging marks 1 / 2 and Ctrl + Alt + click tracks;
  - 3.26.0 automation heard while it plays (`PlaybackSession::autoNow`, `AudioEngine::replaceAutomation` / `serviceReplacedAutomation` / `handBackAutomation`, `AppContext::followAutomation`, `lanesSignature`); Virtual Master stems (`MasteringDef::stemIds`, `MasterRenderBatch` passes, `resampleFile`); the Export complete box; the mixer's fader line and scrolling top section (`StripLayout`, `fitStrip`, `StripBase::layoutFrame`, `topPane` / `topContent`, `FaderLine`, `MixerComponent::layoutView`);
  - 3.27.0 automated controls going back (`EditDef::autoBefore`, `holdAutomatedControls`); compressed waveforms (`WaveShape.h`, `CompressWaveButton`).

**Why sweeps matter.** 3.17.2 fixed a bug no feature test saw: a background thread that span for ever when idle, because **a default-constructed `juce::Uuid` is a new random id, not a null one**. A "no job" check of `job.takeId.isNull()` was never true; a map grew to 22 GB. Bugs of that kind live between features and only show in a long real session.

**Who you work for.** The person records and edits classical sessions with this program. They are a recording engineer, not a programmer: explain findings briefly and in plain words. Their top values:
- **Nothing they recorded or edited is ever lost.**
- **The program stays snappy:** no stalls on Space, Play or Stop; it opens and closes quickly.

## Working setup (this sandbox)

- **Paths:**
  - Repository: `/home/claude/jonesd90/fermata` (branch `fermata-3.22.0-edit-in-rx`; commits stay local - the person uploads the zips to GitHub themselves; never push and never ask about it).
  - JUCE: `$S/juce`, where `S=/tmp/claude-0/-home-claude/feb329c0-2bf0-58d6-9292-835ade6decf9/scratchpad`.
  - Downloads: `/home/claude/out`.
  - The sweep starts from 3.27.0 (commit `6d417bc`): the first sweep build's zip holds the files changed since that commit.
- **The machine has 2 cores and 7 GB:** never run two compiles at the same time (out of memory: "Killed signal").
- **Gates.** One after another, never at the same time (they share a temporary folder; run them with `bash`, in the background, and wait for them):
  - `bash tools/syntax-check-ui.sh $S/juce` (only the known BounceWindow / PreampSetup warnings are allowed)
  - `bash tools/check-windows-code.sh $S/juce`
  - `bash tools/sandbox-check.sh $S/juce`
  - Do not change a source file while a gate runs; if you must, stop the gates and start them all again.
  - Windows-only code (`#if JUCE_WINDOWS`) is syntax-checked by copying the file with that line forced to `#if 1` into `$S/wincheck/`.
- **Quick core test:** `bash $S/rt/par.sh` (compiles the core and `core_test` two at a time, links, runs).
- **Tool checks.** `bash tools/tool-checks.sh $S/juce <mode>`: asan, tsan, fuzz, warnings, tidy, query, valgrind, coverage, rtcheck, longsession (the 3.0.0 and 3.1.x notes explain each). Run `asan` and `fuzz` after changes to code that reads files or messages, `tsan` / `rtcheck` after threading or audio-path changes.
- **Delivery, for every build:**
  - Bump `set(FERMATA_VERSION x.y.z)` in `CMakeLists.txt` (PATCH for fix-only builds: 3.27.1, 3.27.2 ...).
  - Zip **only the files changed since the previous zip** (never `.github`) into `/home/claude/out`, send it, and give a one-line test reminder.
  - A `CHANGELOG.md` entry in the person's language and a section in `docs/DEVELOPMENT-NOTES.md`.
  - A section with tick-box checks at the end of the master test log doc (project `b62e847a-862d-41ec-b531-6d37a613c1e7`, prose node `37c1509e-f944`), and the zip added to the upload order in its paragraph `mfcnnwc3rqw.319` (read the doc since its last revision first).
  - A line in the memory file `/areas/fermata-test-list.md`.
  - A local commit.
- **Style:** all sources ASCII only; comments in plain English that say why; no renaming sprees or reformatting; diffs a person could review; any key or mouse action added, changed or removed also changes `src/ui/HotKeysWindow.h` in the same build.

## The cross-cutting checks (in every area, for that area's files)

1. **"A default `juce::Uuid` means none."** It does not: `juce::Uuid x;` and `juce::Uuid()` are random ids; only `juce::Uuid::null()` is null. For every `juce::Uuid` member or local declared without `= juce::Uuid::null()`, find out whether anything treats its unset value as "none". If so, it is a bug (3.17.2). `grep -rnE "juce::Uuid\s+\w+\s*;" src` lists them. Also the reverse: an id loaded from a file or message that is empty or garbled becomes what?
2. **Every background loop rests when idle and stops promptly.** Every `juce::Thread::run()`, `std::thread` body and timer that starts work: does it block with nothing to do, check `threadShouldExit` / a cancel flag inside long work, and stop **before** what it uses is destroyed? Whether there is work is told by the queue, never by a field of a default-made job.
3. **Containers that only grow.** Maps, vectors and caches keyed by ids or files: are entries removed when the take, edit, window or project goes? Is there a ceiling? Memory after a ten-hour session with hundreds of takes? (New ones to look at: `AudioEngine::replacedPlans`, `Project::rxSeen`, `RxSaveWatch::pending` / `tries`, `EditDef::autoBefore`, the stems' temporary renders, the master renders folder.)
4. **Idle cost.** With a big project open and nothing happening: about 0 % CPU and no disk. Timers doing real work every tick count against it - including the newer ones: `AppContext::liveAutoTicker` (20 Hz, loops over every edit's lanes), the linked-programs watcher (1 s, looks at files on disk), the mixer's 30 Hz / 3 Hz passes (`routeSignature`, `slotsNeeded`).
5. **The audio callback never locks, allocates or waits** (rtcheck covers part of it).
6. **Asynchronous gaps.** `this` and pointers from `findEdit` / `findTakeWindow` / `mixerOfEdit` kept across `callAsync`, `showMenuAsync`, `launchAsynchronously`, `enterModalState` callbacks: use `SafePointer` or look things up again by id.

## The sweep, in order (each area = one build)

### A. The audio thread, playback and threads; starting and closing (xhigh)

- **The audio callback** (`AudioEngine::process` and everything it calls): the mixer plan swap and `carryOver`, `MixerWorkers`, the rate bridge, playback sessions, the pre-roll ring, talkback, LTC, the click / pitch pipe, the scope tap, the meters. No lock, allocation or wait anywhere on it.
- **Live automation (3.26.0 / 3.27.0), closely:** `autoNow` publication and the `blockCounter + 2` rule; `replaceAutomation` (held sends, `rebuildPlan` before publishing); `serviceReplacedAutomation` / `handBackAutomation` (sends and VCA bits handed back only for what the new plan does not drive; several replacements in a row; a stop with replaced plans waiting; a stalled audio thread); `stopPlayback`'s hand-off rule; `followAutomation`'s order (remember controls -> replace the plan) and `holdAutomatedControls`' `canRestore`. Can a control be put back while something still drives it, or never be put back? Any click or stuck fader possible?
- **Plans and offline renders:** `rebuildPlan` while playing, `AutomationPlan::begin / end`, the Bouncer's own engine and automation.
- **Start-up and closing order:** `Main.cpp`, `AppContext` construction / destruction (the `closing::Mark`s), `shutdown()`, opening a project, closing it to open another. For each service (TakeMatchService, the waveform readers and colour cache, Playback and its Retirer, MixerWorkers, PluginHost, RemoteHub / RemoteControl, Ravenna, the control surface, CoreReservation, StallWatch, SaveWriter, the linked-programs watcher, the master render / export jobs, the bounce jobs): who starts and stops it, is it stopped before what it reads is destroyed, is the wait bounded, what if the project is replaced while it works.
- **Safety nets:** the closing watchdog (never before the save is written), `crashreport::describeThreads`, the helper process (`writeViaHelper` / `helperMain`).

### B. Recording and saving (xhigh)

- **Recording:** `TakeWriter` (the disk thread, overruns, a full or vanished drive), the second drive, the recording monitor, the device stopping or changing rate under a take, Pre-Rec, Rec mode and what it holds back, N / V bar keys during a take.
- **Saving and loading:** every field in `Project.cpp` save / load - old and new (`autoBefore`, `stemIds`, `rxSeen`, `WaitingPiece::rx` / `program` / `fileFrom` / `fileLen`, `RegionFile::rxFrom`, the stereo-joined tracks, the external programs list in the settings) - with defaults for older files and survival of a damaged or partial file. Is every field in the undo hash and the save hash where it should be, and out where it should not be?
- `SaveWriter` / the autosave (during closing, a project switch, a crash), "save this window" and the quit list (3.19.0 / 3.21.1), `ProjectCopy`, missing files (relink, ignore permanently, look again), `WavRepair` crash recovery.
- **For every file write:** full disk, vanished drive, crash half way, read-only folder, long or non-ASCII path, OneDrive folder. Is the person told? Is the previous good file still there?

### C. Files that leave Fermata and come back (xhigh)

The data-safety heart of the newer features:
- **Linked programs (`RxLink.*`, `RxWatch.h`):** the files sent (handles, markers, float / 24-bit kept), the watcher (skips while recording / undo / save holds / modal boxes; a file half written; a file replaced by a program that writes a new file and renames; a file deleted; a save while Fermata was closed; `rxSeen` growing), bringing a save in (only the marked part replaced, crossfades, Ctrl+Z), a wrong length / rate / channel count refused, "bring in now", "open again", Cancel. Can a save ever overwrite or lose the person's audio, or land on the wrong piece or track?
- **Export for Processing and re-linking** (`FixTools*`: `processingJobFolder`, `planSendOut`, `runSimpleJob`, `relinkEdit`), "waiting for corrected audio" blocks, Stop waiting.
- **The stereo join (`StereoJoin.h`):** plan / write / apply, a take changed between plan and apply, the files it writes, mixers and lanes moved over, `undoReset` / `keepWindow`. It cannot be undone: is every failure path clean?
- **Imports:** multitrack import (`ImportPlan`), take import, AAF import (`Aaf.*`).

### D. Edit operations and undo, across the whole program (high)

Every operation that changes takes or edits, old and new: pieces (move, slide, slip, shuffle, delete, insert / replace with 3 / 4 / Shift+4, Find other versions "Use this one", J), joins and the Trim window (fades, crossfades, levels, normalise over next take, close gap, the 3.20.0 scroll), `anchorStart` / `anchorEdits`, marks 1 / 2 (3.25.0 dragging, Ctrl + Alt + click, P), volume changes, automation lanes and points (`resolveAutomation`, rows), snips, consonants, Noise mode / denoise, the fix tools (pitch, repair, ReHarmoniSer, ReTimEr), overdubs, the visual guide, markers and feedback flags, takes' details (bars, duds, names), the take chart.

For each: one clean undo step, exact redo, a failure leaves the edit as it was, no view state inside undo state (or the reverse).

### E. The mixers and routing (high)

`Mixer.h`, `MixerWindow.*`, `MixerLook.h`, `StageMixer.h`, `MeterBridge.h`, `ScopeWindow.*`, `PluginHost.*` / `PluginManager.h`, `LowCut*`, VCAs, gangs, sends and `sendDialBuses`, cue mixers and Follow Processing Mixer, audition, Copy / Paste mix, Match edit, hidden strips, the control surface (`Surface.*`, `Mcu.h`, `SurfacePanel.h`) and the remote desk (`Remote*`).
- **The 3.26.0 layout, closely:** `fitStrip` at every window size (tiny, huge, very many buses, no buses, stereo / mono only), the fader line drag (strips rebuilt mid-drag, extra slots held), `topPane` / `topContent` lifetime and clipping, the shared scroll bar and wheel, child components that look for their parent strip, the METERS strip never cut off.
- Plug-in states saved exactly; a plug-in's delay change re-planned; nothing in the window on the audio path.

### F. Mastering, bouncing and exports (high)

`MasterDef.*`, `MasterRender.*` (the two-slot render cache, fingerprints, `learnMasterRenderVolatile`, the stems passes - an up-to-date master and its backup never touched), `MasterExport.*` (rates, loudness check, levels, dither, formats, tags, LAME, stems at the master's gain, `resampleFile`, clipped stems), `Ddp.*`, `ReviewPage.*` (listening copies), `Loudness.h`, `MasteringWindow.*` (the Stems list, the Export complete box), `Bounce.*` / `BounceWindow.*`, `AafExport*` / `AafWrite*`, `VideoExport*`, `TakeSheet*`, the work folders and temporary files (always removed, never left half written in the person's folder).

### G. Files and messages from outside (high)

Every reader and writer, plus a fuzz run for each reader (`tests/fuzz_readers.cpp`): the project file and window files, the take-match fingerprint cache, consonant files, feedback / comment files, iXML names, AAF, the remote protocol, the Ravenna discovery, MCU, the settings XML (the external programs list), crash-report files. Rules: bytes from outside become text only through `td::textFromBytes`; never index a `juce::var` with a number without checking `getArray()` and its size.

### H. Snappiness and a long session (high)

Message-thread cost on a big project (14 take windows x 26 takes x 40 tracks, edits of 400 pieces x 40 files, 367 takes): opening, the waveform drawing (now also through `waveshape::apply` - `std::pow` per column), the edit and take window repaints, the mixer rebuilds, the take chart, VersionsWindow searches, the undo / save snapshots, the 20 Hz `followAutomation` loop over all edits. Use `longsession` with the services running and look for growing memory and drifting timing. Measure before and after; only change what measurably helps.

### I. Clean-up (last; only where tests cover it; no behaviour change at all)

Dead code (the `rx` names that now mean "linked program" may stay as they are - renaming is not wanted), duplicated helpers between windows, code left from removed features. Splitting `FixTools.cpp` only if the person agrees first.

## Which files belong to which area

Every file is read in one area (header and source together). Shared headers are read in the first area that lists them.
- **A:** `Main.cpp`, `AudioEngine.*`, `Playback.*`, `Automation.h`, `MixerWorkers.h`, `RateBridge.h`, `Resampler.h`, `HostTransport.h`, `Ltc.*`, `PitchPipe.h`, `Dither.h`, `Dsp.h`, `CoreReservation.*`, `CrashReport.*`, `SessionClock.h`, `TalkbackKey.h`, `ui/AppContext.*`, `ui/ClosingLog.h`, `ui/StallWatch.h`, `ui/WinOwner.*`, `ui/Splash.h`, `ui/HelpMenu.h`, `ui/CpuCoresPanel.h`, `ui/TalkbackGlow.h`, `ui/ClickWindow.h`, `ui/PitchPipeWindow.*`.
- **B:** `Project.*`, `Takes.h`, `TakeWriter.h`, `SaveWriter.h`, `FileSync.h`, `WavRepair.h`, `LostSamples.h`, `BackupCopier.h`, `ProjectCopy.h`, `AudioRelink.h`, `Preamp.h`, `PreampMemory.h`, `Ravenna.*`, `Markers.h`, `ui/RecordMonitor.h`, `ui/RecPrePanel.h`, `ui/PreampSetup.h`, `ui/ProjectRateDialog.h`, `ui/MediaWindow.*`, `ui/OrganiserWindow.*`, `ui/DesignWindow.*`, `ui/MainWindow.*`.
- **C:** `RxWatch.h`, `StereoJoin.h`, `AudioOps.*`, `ImportPlan.*`, `Aaf.*`, `AafModel.h`, `ui/RxLink.*`, `ui/FixTools.*`, `ui/FixToolsCommon.h`, `ui/FixToolsDialogs.h`, `ui/FixToolsEdits.cpp`.
- **D:** `Edit.h`, `EditFix.h`, `Retime.*`, `Consonants.h`, `ConsonantData.h`, `Denoise.h`, `DenoiseFft.cpp`, `SpectralRepair.h`, `SpectralPicture.h`, `SpectralPyramid.h`, `PitchShift.h`, `Tuning.h`, `TimeAlign.h`, `TakeMatch.*`, `Feedback.h`, `src/reharmoniser/`, `ui/EditWindow.*`, `ui/TakeWindow.*`, `ui/TrimWindow.*`, `ui/TakeDisplay.*`, `ui/TakeChart.h`, `ui/BarCoverage.h`, `ui/VersionsWindow.h`, `ui/RetimerWindow.*`, `ui/AlignWindow.*`, `ui/TakeMatchService.h`, `ui/TakeMatchPanel.h`, `ui/FeedbackUi.h`, `ui/MarkerStrip.h`, `ui/VolumeDialog.h`, `ui/GridEditor.h`.
- **E:** `Mixer.h`, `LowCut.h`, `Mcu.h`, `RemoteDesk.h`, `RemoteHub.h`, `RemoteProtocol.h`, `ui/MixerWindow.*`, `ui/MixerLook.h`, `ui/StageMixer.h`, `ui/MeterBridge.h`, `ui/ScopeWindow.*`, `ui/PluginHost.*`, `ui/PluginManager.h`, `ui/LowCutPanel.h`, `ui/OutputPicker.h`, `ui/Surface.*`, `ui/SurfacePanel.h`, `ui/RemoteControl.*`, `ui/DeskLook.h`.
- **F:** `MasterDef.*`, `MasterRender.*`, `MasterExport.*`, `Ddp.*`, `ReviewPage.*` (and `ReviewPageTemplate.inc`), `Loudness.h`, `Bounce.*`, `AafExport.*`, `AafWrite.*`, `VideoExport.*`, `TakeSheet.h`, `Pdf.h`, `ui/MasteringWindow.*`, `ui/BounceWindow.*`, `ui/AafExportWindow.h`, `ui/VideoExportWindow.h`, `ui/TakeSheetExport.h`.
- **G:** `TextBytes.h`, `AudioFormats.h`, `TimeFormat.h`, `ScreenLayout.h`, `Common.h`, and every reader found in the other areas (fuzz each).
- **H:** `ui/WaveDraw.h`, `ui/WaveThumb.h`, `ui/WaveStore.h`, `ui/WaveColor.h`, `WaveShape.h`, `ui/PaintProfile.h`, `ui/TimeReadout.h`, `ui/Uikit.h`, `ui/FermataLookAndFeel.h`, `ui/ThemeSwitch.h`, `ui/ColourPicker.h`, `ui/BusyWindow.h`, `ui/ValueEntry.h`, `ui/HotKeysWindow.h`, `ui/FontData*.cpp`, `BoxEdges.h`.
- **Tests and tools** (`tests/`, `tools/`): kept working in every area; a new test goes with each fix.

If you find a file not in this list, read it in the area it fits best and say so.

## Ground rules

1. **No silent behaviour changes.** Fix bugs and make things robust; do not redesign features. Anything the person would notice goes in `CHANGELOG.md`. If a fix would change how a feature behaves, ask first.
2. **A test first where it can be done:** a section in `tests/core_test.cpp` (or `stress_test.cpp`) that fails without the fix. For UI-only fixes, say how you checked them.
3. **Data safety over everything.** Never make a change that could lose, overwrite or orphan audio, edits or project files. Recorded files are never modified in place (the one exception: crash recovery mending a WAV header, `WavRepair.h`).
4. **Builds in big chunks:** one build per area, not one per fix.
5. **Ask** before anything risky or ambiguous: file-format changes, removing anything, or behaviour you are not sure was intended.

## Decisions already made by the person (do not undo)

- No separate background recorder process and no once-a-second disk flush: the existing crash recovery is what they want.
- Plug-in crash isolation stays as it is (but a user save must capture every plug-in's actual state).
- No preamp "Cut" feature anywhere.
- Play never stops at mark 2; only Loop sends playback back to mark 1.
- No assignable key commands: the keys are listed in Project > HotKeys. B = the bars box of the take clicked last; Ctrl+B = the meter bridge; Shift+B = the take chart.
- Markers, listeners' comment flags and the edit window's visual guide are never moved by ripple edits (the 0:00 anchoring moves everything together on purpose).
- The edit always begins at 0:00 (or at the "leave space" time).
- The closing safety net stays as a backstop.
- Find other versions matching quality waits for the person's written examples: do not retune it.
- Stereo only for now.
- Linked programs: files sent to a program come back by themselves when saved; files made with "Create files to edit" never do (they wait to be re-linked). The 2 s handles stay.
- Automation follows edits live while playing; a control whose lane loses its points goes back where it was.
- Stems come out at the master's gain, in the master's main format; their working copies are not kept.
- The mixer's fader line: double-click = the normal size (it never snaps back by itself); the line's setting never makes windows grow.

## The earlier sweeps (for reference)

The first sweep (3.1.2-3.1.7, the tool pass in 3.0.0) and the second (3.18.2-3.18.6) are in those sections of `docs/DEVELOPMENT-NOTES.md`, with what was checked and found sound. Use them to save time on old code that has not changed - but this sweep still reads every file.

## What to hand back each time

For each area:
- a short findings list, most serious first: what could go wrong for the person, in plain words, and whether it is fixed;
- then, after "Build it", the zip, the changelog entry, the development-notes section, the test log section and the memory line.

If an area turns up nothing worth changing, say so and move on. That is a good result.

# Fermata - the second deep code sweep (brief for a Fable 5.1 session)

**How it starts.** The person switches this conversation to Fable 5.1 and says **"start the sweep"**. Then:
- Read this file, then the sections of `docs/DEVELOPMENT-NOTES.md` and `CHANGELOG.md` for the areas you touch.
- Do **one area per build**, in the order below, starting with area A.
- Before each build, tell the person in plain words what you found and what you plan to change, and wait for "Build it" / "Yes" (their standing rule: explain first, no code until asked).
- After a build has been tested they say "carry on" (or "next area"): go on to the next area.

**Effort.** Areas A and B (threads / starting / closing, and recording / saving) at **xhigh**; every other area at **high**.

---

## What this is

Fermata is a classical-music recording and editing DAW for Windows: JUCE 8, C++17, MSVC, built by GitHub Actions, namespace `td`.

The first sweep (areas A-F, versions 3.1.2 to 3.1.7, 26 October 2026) covered everything up to 3.1.1. Since then about 30 builds (3.2.0 to 3.17.2) have added a lot, each tested on its own:
- the recording monitor, video export, AAF export;
- J and Find other versions, with take matching (`TakeMatch`, `TakeMatchService`);
- consonants, Noise mode / denoise, and the live sample-rate converter;
- the project sample rate, the take chart and coverage colours;
- CPU core reservation on hybrid CPUs, the Help menu, crash reports and the helper-process freeze report;
- the closing log and closing safety net, the opening log, and "ignore missing files permanently";
- "the edit always begins at 0:00" (`EditDef::anchorStart`, `AppContext::anchorEdits`) with "leave space before the first edit";
- the B / Ctrl+B swap.

This sweep reads across all of it, and back into the older code it leans on.

**Why now.** 3.17.2 fixed a bug that the feature-by-feature tests never saw: a background thread that span for ever when idle. The cause was that **a default-constructed `juce::Uuid` is a new random id, not a null one**. A "no job" check of `job.takeId.isNull()` was therefore never true. The thread "analysed" a made-up take thousands of times a second and filed each one in a map, which ran to 22 GB, kept a core at 100 %, and meant minutes of freeing memory on closing and a crawl on the next opening. Bugs of that kind are what this sweep is for: they live between features, and only show in a long real session.

**Who you work for.** The person records and edits classical sessions with this program. They are a recording engineer, not a programmer, so explain findings briefly and in plain words. Their top values:
- **Nothing they recorded or edited is ever lost.**
- **The program stays snappy:** no stalls on Space, Play or Stop, and it opens and closes quickly.

## Working setup (this sandbox)

- **Paths:**
  - Repository: `/home/claude/TakeDAW`.
  - JUCE: `$S/juce`, where `S=/tmp/claude-0/-home-claude/feb329c0-2bf0-58d6-9292-835ade6decf9/scratchpad`.
  - Downloads: `/home/claude/out`.
- **Gates.** Run them **one after another, never at the same time** (they share a temporary folder):
  - `tools/syntax-check-ui.sh $S/juce`
  - `tools/check-windows-code.sh $S/juce`
  - `tools/sandbox-check.sh $S/juce`
- **Quick core test:**
  - `REBUILD="<names>" bash $S/rt/par.sh`.
  - When `Project.h` or `Edit.h` change, rebuild **all** core objects, or the test crashes from mismatched objects: `Project Playback AudioEngine Bounce AudioOps DenoiseFft ImportPlan Aaf AafWrite AafExport TakeMatch Ltc MasterDef Ddp MasterExport ReviewPage MasterRender Ravenna CoreReservation CrashReport Retime VideoExport`.
- **Tool checks.** `tools/tool-checks.sh $S/juce <mode>` provides asan, tsan, fuzz, warnings, tidy, query, valgrind, coverage, rtcheck and longsession; the first sweep's notes (3.0.0, 3.1.x) explain each. Run `asan` and `fuzz` after any change to code that reads files or messages, and `tsan` / `rtcheck` after threading or audio-path changes.
- **Delivery, for every build:**
  - Bump `set(FERMATA_VERSION x.y.z)` in `CMakeLists.txt` (PATCH for fix-only builds).
  - Zip **only the files changed since the previous zip** (never `.github`) into `/home/claude/out` and send it.
  - Give a one-line test reminder.
  - Add a `CHANGELOG.md` entry in the person's language and a section in `docs/DEVELOPMENT-NOTES.md`.
  - Add a section with tick-box checks to the master test log doc (project `b62e847a-862d-41ec-b531-6d37a613c1e7`, prose node `37c1509e-f944`). Also add the zip to the zip order in its "Before you start" paragraph `mfcnnwc3rqw.319`.
  - Add a line to the memory file `/areas/fermata-test-list.md`.
- **Style:**
  - All sources ASCII only.
  - Comments in plain English that say why.
  - No renaming sprees or reformatting.
  - Diffs a person could review.
  - Any key added, changed or removed also changes `src/ui/HotKeysWindow.h` in the same build.

## The cross-cutting checks (do these in every area, for the files of that area)

1. **"A default `juce::Uuid` means none."** It does not: `juce::Uuid x;` and `juce::Uuid()` make a random id; only `juce::Uuid::null()` is null. For every `juce::Uuid` member or local declared without `= juce::Uuid::null()`, find out whether anything treats its unset value as "none" (`isNull()`, `== juce::Uuid()`, "not found", "not chosen yet", "no mixer"). If anything does, it is a bug, as in 3.17.2. If nothing does, leave it alone, but add `= juce::Uuid::null()` where "unset" is clearly the intent. Known leads (the full list: `grep -rnE "juce::Uuid\s+\w+\s*;" src`, about 90 places):
   - `src/core/Playback.h:42`: `automationPlanFor (..., const juce::Uuid& unassignedMixer = juce::Uuid())`. The default is a random id. Check what callers that leave it out get.
   - Members:
     - ScopeWindow, MixerWindow and Mixer: `mixerId`, `trackId`, `busId`.
     - AafExportWindow, VideoExportWindow, VolumeDialog and EditWindow: `editId`.
     - TakeWindow, BarCoverage and TakeChart: `windowId` (TakeChart also `take`).
     - Surface: `pin`, `shownMixer`. AlignWindow: `sourceId`.
     - AppContext: `discToken`, `DiscPlayItem::editId`, `mixClipFrom`.
     - MasteringWindow: `playToken`. VersionsWindow: `Row::takeId`.
     - AudioEngine: `currentGroupId`.
     - `trackId` in ConsonantData, Retime `StreamTrack` and Project `RoomToneTrack`.
     - MasterRender: `Item::editId`.
   - `juce::Uuid()` passed where "none" might be meant: look at `Retime.cpp:423` and `FixTools.cpp:6591` and decide whether each is a deliberate new id.
   - And the reverse: an id loaded from a file or a message that is empty or garbled becomes... what? (`juce::Uuid (juce::String)` on bad text.)
2. **Every background loop must rest when idle.** For every `run()` (all `juce::Thread` subclasses, `std::thread` bodies, timers that kick off work):
   - With nothing to do, does it block (wait on an event, or a timeout of a second or so), or can it spin?
   - Does it stop promptly when asked (`threadShouldExit`, a cancel flag checked inside long work)?
   - Is it stopped **before** what it uses is destroyed?
   - Keep the 3.17.2 rule: whether there is work is told by the queue, never by a field of a default-made job.
3. **Containers that only grow.** Maps, vectors and caches keyed by ids or files:
   - Are entries removed when the take, edit, window or project goes (closing a project, opening another, deleting a take)?
   - Is there a ceiling?
   - In a ten-hour session with hundreds of takes, what is the memory at the end?
   - Examples to check: `TakeMatchService::known` / `alignments`, the waveform caches, the take chart / coverage data, consonant data, stall-watch / log buffers, the recording monitor's event list, the undo stack.
4. **Idle cost.** With a big project open and nothing happening, Fermata should use about 0 % CPU and no disk. Timers that do real work every tick (repaints of unchanged things, sweeps over every take, file existence checks) count against it. The minute timing lines in `fermata-log.txt` name the busiest threads: read the 3.15.0 / 3.17.0 notes for what they show.

## The sweep, in order (each area = one build)

### A. Starting, closing and threads (xhigh)

**Start-up and closing order:**
- `Main.cpp` (start-up, the crash handler, `--fermata-dump` helper start), `AppContext` construction and destruction order (the `closing::Mark` members show it), `shutdown()`, `afterProjectReplaced`, opening a project (the "Opening:" steps), and closing a project to open another.
- For each service (TakeMatchService, the waveform readers, Playback, MixerWorkers, PluginHost, RemoteHub / RemoteControl, Ravenna, the control surface, CoreReservation, StallWatch, the autosave / SaveWriter), check:
  - who starts it, and who stops it;
  - is it stopped before what it reads is destroyed;
  - does stopping wait for a bounded time;
  - what happens if the project is replaced while it works.

**Safety nets:**
- The closing safety net (`ClosingWatchdog` in `ClosingLog.h`):
  - can it fire while a save is still being written?
  - is `endThisProcess` ever reachable before the save?
- `crashreport::describeThreads`: it suspends threads; can it suspend one that holds the heap lock or the loader lock while the caller allocates?
- The helper process (`writeViaHelper` / `helperMain`): every handle closed, every wait bounded.

**Plus the first sweep's area-A checks, for code added since 3.1.1:**
- `SafePointer` and `findEdit` / `findTakeWindow` results kept across an asynchronous gap.
- `this` captured in `callAsync` / `showMenuAsync` / `confirmAsync` / `launchAsync` lambdas in windows added since then: VersionsWindow, TakeChart, the consonants tab, Noise mode, the recording monitor, HelpMenu, CpuCoresPanel, the ignore-missing dialogs, the EditWindow lead-in boxes.
- Detached threads.
- JUCE message-thread calls from other threads.

**The audio callback still never locks, allocates or waits.** Check the rate converter, the consonant paste, the denoise preview and anything else added to the audio path; `rtcheck` covers part of it.

### B. Recording and saving (xhigh)

**Recording:**
- `TakeWriter` (disk thread, overruns, a full or vanished drive), the second recording drive, the recording monitor's events, and the device stopping or changing rate under a take.
- The rate converter's "recording refused while converting" path, and the project sample rate checks before Record.

**Saving and loading:**
- `Project.cpp` save / load of everything added since 3.1.1, with defaults for older files and survival of a damaged or partial file: `ignoredMissing`, `leadIn` / `leadInSeconds`, the project sample rate, consonants, room tone, take-match settings, the "waiting for corrected audio" marks.
- Is every new field in the undo hash and the save hash where it should be (and kept out where it should not be)?
- `SaveWriter.h` / the autosave: a save in progress during closing, during a project switch, or during a crash.
- `ProjectCopy` (does it take the Consonants and Analysis folders?).
- Missing files: relink, "Stop waiting for exported files", "Ignore missing files permanently" and "Look for them again".

**For every file write:**
- What happens on a full disk, a vanished drive, a crash half way, a read-only folder, a long or non-ASCII path, or a OneDrive folder?
- Is the person told?
- Is the previous good file still there?

### C. Edit operations and undo since 3.1.1 (high)

- **Edit start and alignment:**
  - `EditDef::anchorStart` and its first-region extension (3.17.1);
  - `AppContext::anchorEdits` and its skip conditions;
  - with Find other versions "Use this one" (its ripple and "Undo fix");
  - with J;
  - with the Trim window.
- **Consonants:** copy, remove, paste and nudge.
- **Noise mode / denoise:** write back to the clip.
- **ReTimEr changes.**

For each operation, check that it is one clean undo step, that redo is exact, and that a failure leaves the edit as it was. Look for operations that change view state inside undo state, or the reverse.

### D. Files and messages from outside (high)

Every reader and writer added since 3.1.1, plus a fuzz run for each new reader (add them to `tests/fuzz_readers.cpp`):
- the take-match fingerprint cache (`.fpr`: a truncated or old file must be refused and re-made, never crash);
- the consonant files;
- the take sheet PDF / CSV;
- AAF export (`AafWrite`: Resolve and Pro Tools are the real readers);
- video export with LTC;
- crash-report file names and pruning;
- `cpu-fence.txt`.

The two rules from the first sweep still hold:
- bytes from outside become text only through `td::textFromBytes`;
- never index a `juce::var` with a number without checking `getArray()` and its size.

### E. Snappiness and a long session (high)

Message-thread cost on a big project: 14 take windows x 26 takes x 40 tracks, edits of 400 pieces x 40 files, 367 takes. Look at:
- the opening steps (the "Opening:" lines);
- the take chart and coverage colours, VersionsWindow searches, the sweep timers, repaints and the undo / save snapshots.

Use `longsession` with the new services running (take matching, the recording monitor, the stall watch) and look for growing memory and drifting timing. Measure before and after. Only change what measurably helps.

### F. Clean-up (last, only where tests cover it; no behaviour change at all)

- Code left behind by the B / Ctrl+B swap, the removed Trim nudges and older experiments.
- Duplicated helpers between the newer windows.
- Dead code.
- **Splitting `FixTools.cpp`** (left from the first sweep) only if the person can build and run the tools the same day. Ask first.

## Ground rules

1. **No silent behaviour changes.** Fix bugs and make things robust; do not redesign features. Anything the person would notice goes in `CHANGELOG.md`. If a fix would change how a feature behaves, ask first.
2. **A test first where it can be done:** a section in `tests/core_test.cpp` (or `stress_test.cpp`) that fails without the fix. For UI-only fixes, say how you checked them.
3. **Data safety over everything.** Never make a change that could lose, overwrite or orphan audio, edits or project files. Recorded files are never modified in place. The one exception is crash recovery mending a WAV header (`WavRepair.h`).
4. **Builds in big chunks:** one build per area, not one per fix.
5. **Ask** before anything risky or ambiguous: file-format changes, removing anything, or behaviour you are not sure was intended.

## Decisions already made by the person (do not undo)

- No separate background recorder process and no once-a-second disk flush: the existing crash recovery is what they want.
- Plug-in crash isolation stays as it is (but a user save must capture every plug-in's actual state).
- No preamp "Cut" feature anywhere.
- Play never stops at mark 2; only Loop sends playback back to mark 1.
- No assignable key commands: the keys are listed in Project > HotKeys. B = the bars box of the take clicked last; Ctrl+B = the meter bridge; Shift+B = the take chart.
- Markers, listeners' comment flags and the edit window's visual guide are never moved by ripple edits. (The 0:00 anchoring in 3.16.0 moves everything together on purpose: that is a shift of the whole edit, not a ripple.)
- The edit always begins at 0:00 (or at the "leave space" time). Edit 1's opening fade defines the start.
- The closing safety net stays as a backstop even though 3.17.2 removed the cause it was added for.
- Find other versions matching quality waits for the person's written examples: do not retune it in this sweep.
- Stereo only for now.

## The first sweep (for reference)

The first sweep covered areas A to F in versions 3.1.2 to 3.1.7. Its findings, and what was checked and found sound, are in those sections of `docs/DEVELOPMENT-NOTES.md`; the tool pass behind it is in the 3.0.0 section. Start from those notes rather than repeating that work, except where code has changed since.

## What to hand back each time

For each area:
- a short findings list, most serious first: what could go wrong for the person, in plain words, and whether it is fixed;
- then, after "Build it", the zip, the changelog entry, the development-notes section, the test log section and the memory line.

If an area turns up nothing worth changing, say so and move on. That is a good result.

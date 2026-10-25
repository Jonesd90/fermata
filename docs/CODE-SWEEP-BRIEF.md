# Fermata - the deep code sweep (brief for a Fable 5.1 session)

**To start it, paste this to Claude (Fable 5.1), with the Fermata repository attached (GitHub: Jonesd90/fermata):**

> Fermata code sweep: read `docs/CODE-SWEEP-BRIEF.md` in the Fermata repository and carry it out, one area per build, starting with area A. Before each build, tell me in plain words what you found and what you are changing.

**After each build has been uploaded and tested, paste this to go on to the next area:**

> Carry on with the Fermata code sweep: the next area in `docs/CODE-SWEEP-BRIEF.md`.

---

## What this is

Fermata is a classical-music recording and editing DAW for Windows (JUCE 8, C++17, MSVC, built by GitHub Actions; namespace `td`). It was built feature by feature over many builds, each tested on its own. This sweep reads across the whole program for what feature-by-feature work misses: races between threads, objects used after they are gone, error paths that lose work, inconsistent undo, and duplicated plumbing.

The person you are working for records and edits classical sessions with this program. They are a recording engineer, not a programmer: explain findings briefly and in plain words. Their top values: **nothing they recorded or edited is ever lost**, and **the program stays snappy** (no stalls on Space, Play, Stop).

## Read first

- `docs/DEVELOPMENT-NOTES.md`: the design record, one section per version (from the 3.0.0 section on, newest first; the oldest ones come before it). Before you change an area, read the sections that mention it. Many odd-looking choices are deliberate and explained there.
- `CHANGELOG.md`: what the person was told, in their language. Every visible change you make goes in here, in the same plain style.
- `src/core` (engine, project model, file formats, DSP: testable without UI), `src/ui` (windows; `FixTools.cpp` holds most process windows), `src/reharmoniser`, `tests/core_test.cpp` (the main test program: sections named after the version that added them), `tests/stress_test.cpp` (a pretend audio thread against a pretend user), `tools/` (the Linux check scripts), `third_party/` (vendored libraries, never edited).

## Already done by tools - do not repeat

Version 3.0.0 ran every check where the tools, not the reasoning, find the problems. `tools/tool-checks.sh <JUCE> <mode>` runs each one again; the results, and what each finding turned out to be, are in the 3.0.0 section of `docs/DEVELOPMENT-NOTES.md`:

- `asan`: AddressSanitizer + UndefinedBehaviorSanitizer + the leak check on the core tests, the stress test, the time-format and control-port tests and the Re-HarmoniSer's tests;
- `tsan`: ThreadSanitizer on the stress test, the control-port test and the core tests;
- `fuzz`: the readers' fuzz test (`tests/fuzz_readers.cpp`): damaged and random input, tens of thousands of times, for every reader of outside data (WAV headers and their mending, iXML, AAF, listeners' comments, MIDI from the control surface, Stream Deck lines, saved projects and settings, the Merging devices' and ANEMAN's JSON, typed times, the names of files being imported, LTC audio);
- `warnings`: every source through GCC with the bug-finding warnings on;
- `tidy`: clang-tidy's bugprone and static-analyzer checks on every source;
- `query`: clang-query for every `juce::var` indexed with a number (unchecked by JUCE in a release build);
- `valgrind`: memcheck (memory read before it was written) on the stress test and the small tests;
- `coverage`: which lines of `src/core` the tests run;
- and the Windows-header check (`tools/check-windows-code.sh`).

Their real findings were fixed in 3.0.0. Re-run them only to check your own changes (`fuzz` and `asan` after any change to code that reads files or messages). Never run two core test programs at the same time: they share a temporary folder. Spend your effort on what needs reading and reasoning across files.

Two rules came out of it - keep to them in anything you write:
- **Bytes from outside become text only through `td::textFromBytes`** (`src/core/TextBytes.h`). `juce::String::fromUTF8` trusts the bytes, and JUCE's UTF-8 reader then runs past the end of the text; JUCE's own validity check lets some Windows-code-page text through as UTF-8.
- **Never index a `juce::var` with a number without checking `getArray()` and the size first.** In a release build it is a crash, not an empty value.

## Leads from the tool pass (they need reasoning, which is why they are yours)

- **Area A - 40 places where a window is used again after a call** (GCC's null warnings, listed in the 3.0.0 notes): a `SafePointer` (or `def()` / `findTakeWindow`) is checked at the top of a callback, then used again after calls into the program. Each is checked first; the question is whether any call in between can close or delete that window (a project change, a mixer copy, an edit deleted), so that the next `safe->` is null. Also every asynchronous callback that captures `this` itself rather than a `SafePointer` (`confirmAsync (..., [this ...`, `showMenuAsync`, `callAsync`, `enterModalState`: about 40; `grep -nE "(confirmAsync|showAsync|showMenuAsync|callAsync|callAfterDelay|ModalCallbackFunction::create)[^;]*\[[^]]*\bthis\b" src/ui/*`): can its window close while it waits (the Stream Deck can close windows)? And the detached `std::thread`s (`grep -n "detach()" src`): one may still be running when its window - or the program - closes.
- **Area D - the Merging device:** `RavennaDevice::handleMessage` does not make the channel list again when a message under `.custom.ins.channels` changes how many channels a module has (`readChannel` no longer crashes on that, but the list stays stale until the next full settings message). `WebSocketClient::receive` caps one frame at 64 MB but not a message made of many continuation frames.
- **Area E - text handled a character at a time:** the comments import took hours on a long list (`juce::String::operator[]` walks the text from its start; adding to a `juce::String` one character at a time copies it every time). Look for the same pattern anywhere a file's or a message's text is walked.
- **Areas B and C - code no test runs:** the coverage list in the 3.0.0 notes, least covered first (Ravenna.cpp, the device conversation, at about a quarter; MasterRender, Bounce and MasterExport have untested error paths). `src/ui` has no automated test at all: everything there is reasoning (or the render harness).

## Ground rules

1. **No silent behaviour changes.** Fix bugs and make things robust; do not redesign features. Anything the person would notice goes in `CHANGELOG.md`. If a fix would change how a feature behaves, ask first.
2. **A test first where it can be done:** a section in `tests/core_test.cpp` (or `stress_test.cpp`) that fails without the fix. UI-only fixes: explain how you checked them.
3. **Data safety over everything.** Never make a change that could lose, overwrite or orphan audio, edits or project files. Recorded files are never modified in place (the one exception is crash recovery, which mends a WAV's header - never its audio - when a project opens: `WavRepair.h`).
4. **Builds in big chunks:** one build per area below, not one per fix. Bump the version for every build in `CMakeLists.txt` (`set(FERMATA_VERSION x.y.z)`, the one place): PATCH for fix-only builds.
5. **Delivery:** a zip of only the files changed since the previous zip (never `.github`), plus a one-line reminder of what to test. The person uploads the files to GitHub; the build there is MSVC.
6. **Before every zip:** `tools/sandbox-check.sh <JUCE checkout>` (core build, all tests, stress test) and `tools/check-windows-code.sh <JUCE>` must pass; every `src/ui` and `src/core` file must compile. Write a section for the build in `docs/DEVELOPMENT-NOTES.md`.
7. **Keep the style:** comments in plain English that say why; no renaming sprees or reformatting; diffs a person could review.
8. **Ask** before anything risky or ambiguous (file-format changes, removing anything, behaviour you are not sure was intended).
9. **Keys:** a key you add, change or remove anywhere is changed in the HotKeys list too (`src/ui/HotKeysWindow.h`, shown by Project > HotKeys), in the same build.

## Decisions already made by the person (do not undo)

- No separate background recorder process and no once-a-second disk flush: the existing crash recovery (files mended on reopen) is what they want.
- Plug-in crash isolation stays as it is (but a user save must capture every plug-in's actual state).
- No preamp "Cut" feature anywhere.
- Play never stops at mark 2; only Loop sends playback back to mark 1.
- No assignable key commands: the keys stay as they are, and Project > HotKeys lists them (3.0.1).
- Markers, listeners' comment flags and the edit window's visual guide are never moved by ripple edits.
- Stereo only for now (surround plug-in processing later).

## The sweep, in order (each area = one build)

### A. Threads and lifetimes (crashes and hangs)
Where threads meet: `AudioEngine` (the audio callback must never lock, allocate, wait or touch the project; the session hand-off and `Retirer`), `Playback` (reader thread and ring), `MixerWorkers`, plug-in hosting (`PluginHost`), recording (`TakeWriter`: disk thread, buffer overruns), `SaveWriter.h` / the automatic save, every `AudioJob` user (each process window: callbacks arriving after the window closed, files deleted while still open on Windows), the control port and Stream Deck (`RemoteHub`, `RemoteControl`), `Ravenna`, `CoreReservation`, `StallWatch`.
Look for: data races and lock order; long work on the message thread; `this` or references captured in `std::function`s that outlive their object; `findEdit` / `findTakeWindow` / `findTrack` results kept across an asynchronous gap; JUCE message-thread APIs called from other threads; static initialisation order.

### B. Data safety
`Project.cpp` (prepare / write saves, window files, tidying, loading damaged or partial projects), crash recovery of recordings (`WavRepair`, `TakeWriter`), `ProjectCopy` (Save as / Create copy), relinking and missing files, AAF import, Export for Processing / re-link, the mastering exports (DDP, WAV, review pages).
For every file write: what happens on a full disk, a drive that vanishes, a crash half way, a read-only folder, a long or non-ASCII Windows path, a OneDrive or network folder? Is the user told? Is the previous good file still there?

### C. Error paths and undo
Every operation that can fail (file, device, plug-in, network) says so clearly and leaves the project consistent - no early `return` that quietly drops work. Undo / redo for every edit operation: what is undo state and what is view state (`Project::undoHash` / `saveHash`, `restorePiece`), and is every operation one clean step?

### D. Hardware-facing code (reason about it: it cannot be run here)
ASIO device changes and loss mid-session, sample-rate changes, Ravenna / MADI channel counts (64+), the Mackie Control (MCU) parser (`Mcu.h`), the Stream Deck protocol (`RemoteProtocol.h`), the Merging preamp remote (`Preamp.h`, `PreampMemory.h`). Malformed or unexpected input must never crash or hang the program.

### E. Snappiness
Message-thread cost on a big project (14 take windows x 26 takes x 40 tracks; edits of 400 pieces x 40 files): repaints, waveform caching, change handlers, undo snapshots, save snapshots. Measure before and after (the stall watch logs to `fermata-log.txt`; the 2.102 notes describe how the save / undo / play-start timings were benched). Only change what measurably helps.

### F. Clean-up (last, and only where tests cover it)
Duplicated plumbing (the process windows' preview / save-hold / dialog / delete-later helpers exist in `FixTools.cpp` and again in `RetimerWindow.cpp`), splitting `FixTools.cpp` (6,000+ lines) into a file per tool, dead code. No behaviour change at all in this build.

## What to hand back each time

For each area: a short findings list (most serious first: what could go wrong for the person, in plain words, and whether it is fixed), the zip, the changelog entry and the development-notes section. If an area turns up nothing worth changing, say so and move on - that is a good result.

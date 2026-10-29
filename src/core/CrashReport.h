#pragma once
#include <juce_core/juce_core.h>

namespace td
{
/** Crash reports (3.14.0). When Fermata crashes, or its main thread is stuck for a long time, Windows writes a "minidump" of the program into
    Documents\Fermata\Crash reports: a snapshot of every thread at that moment (where each one was, its stack, the modules loaded). With the
    Fermata.pdb of the same build it shows exactly which line of Fermata was running - far more than the stack trace in the log, which loses its
    way without the .pdb. The log says when a report was written; the next start tells the user once and offers to show the folder.
    A report is a few megabytes; only the newest few are kept. Nothing is ever sent anywhere: the user sends a report on purpose.
    Windows only: elsewhere nothing is written and the calls do nothing. */
namespace crashreport
{
    /** Message thread, at start-up: where the reports go, and the version for their names. Loads Windows' dbghelp.dll now (not during a crash). */
    void install (const juce::File& folder, const juce::String& version);
    juce::File folder();

    /** Writes a report. 'exceptionPointers' = what Windows passed to the crash handler (EXCEPTION_POINTERS*), or nullptr for a report of the
        program as it is (a stuck main thread). 'why' goes into the file name: "crash" or "freeze". Returns the file, or File() if none was written. */
    juce::File write (void* exceptionPointers, const char* why);

    /** "access violation at Fermata.exe+0x1a2b3c" - which kind of crash, and where (the module and the offset in it), worked out without any
        symbols. Empty when unknown. For the crash line in the log. */
    juce::String describeException (void* exceptionPointers);

    /** The reports written after 'since' (newest first). */
    juce::Array<juce::File> reportsSince (juce::Time since);
    /** All reports, newest first. */
    juce::Array<juce::File> allReports();
    /** Deletes all but the newest 'keep' reports. */
    void prune (int keep = 5);

    /** (3.17.0) Where every thread of Fermata is right now, as text for the log: each thread by name ("main thread", "Take matching", "Mixer helper 2",
        or its number), with the functions it is in, innermost first - with their source file and line when Fermata.pdb is next to Fermata.exe, else the
        module and offset ("Fermata.exe+0x1a2b3c"). Each thread is held for a moment while its place is read, then let go; nothing else of the program is
        touched. 'mainThreadId' (Windows' id of the message thread) is listed first, with up to 'mainFrames' functions; the others with up to
        'otherFrames'. Windows on x64 only; elsewhere empty. Call from a thread of its own (the stall watch), never from the one that is stuck. */
    juce::String describeThreads (unsigned long mainThreadId, int mainFrames = 48, int otherFrames = 14, unsigned long onlyThread = 0);
    /** ('onlyThread', when not 0: only the main thread and that one - a thread that keeps a core busy.) */

    /** (3.17.0) A freeze report written by a separate little process - Fermata.exe started again with "--fermata-dump <process id> <file>" - instead of
        from inside this one: a program that is stuck can hold the very locks that writing a report of itself needs (a 0-byte report was left behind like
        that). Waits up to 'waitMs' for it. Returns the file written, or File() (then 'error' says why). Windows only. */
    juce::File writeViaHelper (const char* why, juce::String& error, int waitMs = 60000);
    /** The helper's side: the arguments of "--fermata-dump <pid> <file>" -> 0 when the report was written. Called by the application before anything
        else starts when it sees that argument. */
    int helperMain (const juce::StringArray& args);
    /** (3.17.0) Gives the calling thread its name in Windows ("Take matching", "Mixer helper 2" ...), so the log's "busiest" lines and the thread
        listing can say which part of Fermata a thread is (JUCE names its threads only in debug builds). Does nothing elsewhere. */
    void nameThisThread (const juce::String& name);
    /** The same without making a juce::String (for the audio callback: nothing is allocated). */
    void nameThisThreadW (const wchar_t* name) noexcept;
    /** (3.17.0) Ends this program at once (the closing safety net: only once everything is saved). */
    [[noreturn]] void endThisProcess() noexcept;
    /** True when these are the helper's arguments. */
    inline bool isHelperCall (const juce::StringArray& args) { return args.contains ("--fermata-dump"); }
}
} // namespace td

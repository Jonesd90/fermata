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
}
} // namespace td

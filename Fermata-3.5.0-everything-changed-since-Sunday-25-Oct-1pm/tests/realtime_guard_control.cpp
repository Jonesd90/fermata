// The realtime guard's own check (3.1.2, run by tools/tool-checks.sh rtcheck before the real tests): a pretend td::AudioEngine::process that does
// the four things the audio thread must never do - allocate, free, take a lock, sleep - and one thing it may (wake another thread). The guard
// must report the four and let the fifth through; otherwise a clean run of the real tests would prove nothing.
//
// It is compiled twice, into two objects: -DRTGUARD_CONTROL_ENGINE gives the pretend engine, -DRTGUARD_CONTROL_MAIN the program that calls it.
// They have to be separate objects because the linker's --wrap only catches calls that cross from one object into another (as the tests' calls
// into the real engine do).
#include <cstdio>
#include <mutex>
#include <vector>
#include <juce_core/juce_core.h>

namespace td { struct AudioEngine { void process (const float* const* in, int numIn, float* const* out, int numOut, int n); }; }

#if defined (RTGUARD_CONTROL_ENGINE)
static std::mutex gLock;
static juce::WaitableEvent gWake;
static std::vector<char> gKeep;
void td::AudioEngine::process (const float* const*, int, float* const*, int, int)
{
    std::vector<char> big (100000);                     // a real allocation (big enough that the compiler cannot do without it), freed on the way out
    gKeep.swap (big);
    { const std::lock_guard<std::mutex> l (gLock); }    // a lock
    gWake.signal();                                     // allowed: how the engine wakes the disk writer and the mixer helpers
    juce::Thread::sleep (1);                            // a sleep
}
#elif defined (RTGUARD_CONTROL_MAIN)
namespace td::rtguard { int reportCount(); }
int main()
{
    td::AudioEngine e;
    e.process (nullptr, 0, nullptr, 0, 0);
    e.process (nullptr, 0, nullptr, 0, 0);              // (the same faults again: counted, reported once per place)
    std::printf ("control: %d report(s)\n", td::rtguard::reportCount());
    return td::rtguard::reportCount() >= 4 ? 0 : 1;     // (the shell script checks that each kind was reported)
}
#else
 #error "compile with -DRTGUARD_CONTROL_ENGINE or -DRTGUARD_CONTROL_MAIN"
#endif

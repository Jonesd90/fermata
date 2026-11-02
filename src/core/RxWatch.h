#pragma once
// 3.22.0 "Edit in RX" (3.24.0: any linked program): deciding when a file that was sent to a program has been saved there and can be brought back into the edit.
// Plain logic with no disk access of its own (the caller looks at the files), so the tests can drive it.
#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include <vector>

namespace td
{
struct RxSaveWatch
{
    /** What the disk says about one watched file right now. */
    struct Look { juce::String path; bool exists = false; juce::int64 modified = 0, size = 0; };

    /** Called about once a second with every watched file. Returns the files saved since they were last dealt with that have not changed since the
        previous call (so RX has finished writing them). 'seen' is Project::rxSeen: a file met for the first time is taken as it is now (nothing to
        bring in), and a file whose time is the one already dealt with is left alone. */
    std::vector<juce::String> ready (const std::vector<Look>& now, std::map<juce::String, juce::int64>& seen)
    {
        std::vector<juce::String> out;
        std::set<juce::String> present;
        for (auto& l : now)
        {
            present.insert (l.path);
            if (! l.exists) { pending.erase (l.path); continue; }                 // (while RX puts the new file in place the old one can be missing for a moment)
            const auto it = seen.find (l.path);
            if (it == seen.end()) { seen[l.path] = l.modified; pending.erase (l.path); continue; }
            if (it->second == l.modified) { pending.erase (l.path); tries.erase (l.path); continue; }
            const auto p = pending.find (l.path);
            if (p == pending.end() || p->second.first != l.modified || p->second.second != l.size)
            {
                pending[l.path] = { l.modified, l.size };                        // just saved, or still being written: looked at again next time
                continue;
            }
            out.push_back (l.path);
        }
        for (auto it = pending.begin(); it != pending.end();) it = present.count (it->first) != 0 ? std::next (it) : pending.erase (it);
        return out;
    }

    /** Dealt with - brought in, or turned down with a message: this save is not looked at again (a later save is). */
    void done (const juce::String& path, juce::int64 modified, std::map<juce::String, juce::int64>& seen)
    {
        seen[path] = modified; pending.erase (path); tries.erase (path);
    }

    /** Could not be read yet (RX may still have it open): looked at again in a moment. True once it has failed so often that the caller should say so
        and call done() instead. */
    bool later (const juce::String& path, int maxTries = 10)
    {
        pending.erase (path);
        return ++tries[path] >= maxTries;
    }

    std::map<juce::String, std::pair<juce::int64, juce::int64>> pending;      // changed files waiting to be the same twice running: (time, size)
    std::map<juce::String, int> tries;                                         // failed reads of a saved file
};
} // namespace td

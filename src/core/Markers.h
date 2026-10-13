#pragma once
#include "Common.h"
#include <algorithm>
#include <vector>

namespace td
{
/** A quick marker on a take window's or an edit window's timeline (seconds on that window's own timeline). */
struct Marker
{
    juce::Uuid   id;
    juce::String name;
    double       seconds = 0.0;
};

/** The number a new marker gets when it is not named: one more than the biggest all-digit name already there (1, 2, 3 ...). */
inline int nextMarkerNumber (const std::vector<Marker>& list)
{
    int top = 0;
    for (auto& m : list)
        if (m.name.isNotEmpty() && m.name.containsOnly ("0123456789") && m.name.length() < 9) top = std::max (top, m.name.getIntValue());
    return top + 1;
}

/** Adds a marker at 'seconds' called by the next sequential number; returns its id. */
inline juce::Uuid addMarker (std::vector<Marker>& list, double seconds)
{
    Marker m; m.id = juce::Uuid(); m.seconds = std::max (0.0, seconds); m.name = juce::String (nextMarkerNumber (list));
    list.push_back (m);
    return m.id;
}

inline Marker* findMarker (std::vector<Marker>& list, const juce::Uuid& id)
{
    for (auto& m : list) if (m.id == id) return &m;
    return nullptr;
}

inline void removeMarker (std::vector<Marker>& list, const juce::Uuid& id)
{
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Marker& m) { return m.id == id; }), list.end());
}
} // namespace td

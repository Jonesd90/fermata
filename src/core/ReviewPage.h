#pragma once
#include "Common.h"
#include <atomic>
#include <vector>

namespace td
{
/** The listening copy: ONE .html file that holds the recording itself, a waveform, play / pause / stop, and a form where a listener adds comments
    (each pinned to the time they pressed the button, with an optional bar written as text). It opens with a double-click in any current browser
    (Windows, Mac, iPhone / iPad, Android) with nothing to install. The listener saves a small .json file and sends it back; Fermata imports it. */
namespace reviewpage
{
struct Spec
{
    juce::File   audio;              // the finished WAV / FLAC / MP3 that goes inside the page
    juce::String mime;               // "audio/wav", "audio/flac" or "audio/mpeg"
    juce::String piece;              // name shown at the top
    juce::String project;            // optional second line
    juce::String editId;             // so the comments find their way back to the right edit
    double       offsetSeconds = 0;  // where the file's time 0 sits on the edit's timeline
    double       durationSeconds = 0;
    std::vector<int> peaks;          // min, max pairs, -100..100 (see computePeaks)
};

/** Min / max of the louder of the two channels in 'columns' slices of 'file' (times 'gain'), as pairs in -100..100. */
std::vector<int> computePeaks (const juce::File& file, float gain, int columns, const std::atomic<bool>* cancel = nullptr);
int peakColumnsFor (double durationSeconds);

/** Writes the page. Returns an error text, or "" when it worked. */
juce::String write (const juce::File& html, const Spec&, const std::atomic<bool>* cancel = nullptr);

juce::String base64 (const void* data, size_t size);
} // namespace reviewpage
} // namespace td

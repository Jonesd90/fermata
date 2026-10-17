#pragma once
#include "Common.h"
#include <atomic>
#include <vector>

namespace td
{
/** The listening copy: ONE .html file that holds the recording itself, a waveform, play / pause / stop, and a form where a listener adds comments
    (each pinned to the time they pressed the button, with an optional bar written as text). It opens with a double-click in any current browser
    (Windows, Mac, iPhone / iPad, Android) with nothing to install. The listener's comments are kept in their browser as they go ("Save my comments");
    "Export comments for sending back to the sender" makes a small .fmnotes file (JSON inside), named like the page with "listening notes" in place
    of "listening copy", which Fermata imports. One page can also hold all the pieces (a list of pieces, played one after another like a CD). */
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

/** Writes the page for one piece. Returns an error text, or "" when it worked. */
juce::String write (const juce::File& html, const Spec&, const std::atomic<bool>* cancel = nullptr);

/** Writes ONE page that holds several pieces (in this order), with 'title' at the top. */
juce::String writeMany (const juce::File& html, const juce::String& title, const juce::String& project, const std::vector<Spec>& pieces, const std::atomic<bool>* cancel = nullptr);

juce::String base64 (const void* data, size_t size);
} // namespace reviewpage
} // namespace td

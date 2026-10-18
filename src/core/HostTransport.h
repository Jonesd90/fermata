#pragma once
#include "Common.h"

namespace td
{
/** What the plug-ins are told about the transport (VST3 ProcessContext): playing or not, recording or not, and where on the timeline the block
    starts. Plug-ins that "listen" while the host plays (Auto-Align 2, analysers ...) need it; most others ignore it.
    The engine fills one per block before running the mixers, and points the running thread at it while a mixer is processed
    (currentHostTransport), so the plug-ins' play head can read it on whichever thread that mixer runs. Nothing here allocates or locks. */
struct HostTransport
{
    bool playing = false, recording = false;
    juce::int64 samplePos = 0;           // timeline position of the block's first sample
    double sampleRate = 48000.0;
};

inline const HostTransport*& currentHostTransport() noexcept
{
    thread_local const HostTransport* t = nullptr;
    return t;
}
} // namespace td

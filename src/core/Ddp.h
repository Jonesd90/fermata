#pragma once
#include "MasterDef.h"

namespace td
{
/** Writing a CD master: the DDP 2.00 fileset (DDPID, DDPMS, PQDESCR, CDTEXT.BIN, IMAGE.DAT, CHECKSUM.MD5), a WAV + CUE pair, and a checker that reads a fileset back.
    The audio is always 16-bit / 44.1 kHz stereo. 'files' holds one such WAV file per track of the PqLayout (same order). */
namespace ddp
{
juce::uint16 crc16 (const juce::uint8* data, size_t size);          // the CD-Text CRC (CCITT, inverted)

juce::MemoryBlock buildDdpId (const DdpDisc&);
juce::MemoryBlock buildDdpMs (int pqBytes, int imageSectors, int cdTextBytes);
juce::MemoryBlock buildPqDescr (const DdpDisc&, const PqLayout&);
/** Empty block with 'error' empty = no CD-Text at all (nothing was typed). */
juce::MemoryBlock buildCdText (const DdpDisc&, const PqLayout&, juce::String& error);

/** Writes the fileset into 'folder' (created; old DDP files there are replaced). Returns an error text, or "" when it worked. If 'zipFile' is not a null File,
    the fileset is also zipped there. */
juce::String writeFileset (const juce::File& folder, const DdpDisc&, const PqLayout&, const std::vector<juce::File>& files,
                           const juce::File& zipFile, std::atomic<float>* progress, const std::atomic<bool>* cancel);

/** One WAV file with the whole programme (lead-in silence left out) and a CUE sheet that points into it. */
juce::String writeWavAndCue (const juce::File& wavFile, const juce::File& cueFile, const DdpDisc&, const PqLayout&, const std::vector<juce::File>& files,
                             std::atomic<float>* progress, const std::atomic<bool>* cancel);
juce::String cueSheetText (const juce::String& wavName, const DdpDisc&, const PqLayout&);

/** Reads a fileset back and checks it: sizes, track order, the lead-out, CD-Text CRCs, the checksums. 'report' is a readable summary (also lists problems). */
bool verifyFileset (const juce::File& folder, juce::String& report);
} // namespace ddp
} // namespace td

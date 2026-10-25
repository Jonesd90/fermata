#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

namespace td
{
/** JUCE's WAV reader believes the sizes in a file's header. A damaged header can make it report a length the file cannot hold: an RF64 file whose
    64-bit data size is nonsense comes out with a negative length, and reading it then writes outside the buffer being filled (a crash, found by the
    3.0.0 fuzz test). This reader keeps the length within what the file's bytes can actually hold. A whole file is never changed by it: its data is
    never larger than the file, so the check only ever catches a header that is wrong. (The file's size is only asked for when the header claims
    more than a plain WAV header can - over 4 GB - so opening the thousands of files of a big edit costs nothing more.) */
class CheckedWavAudioFormat : public juce::WavAudioFormat
{
public:
    juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
    {
        auto* r = juce::WavAudioFormat::createReaderFor (in, deleteStreamIfOpeningFails);
        if (r != nullptr) keepWithin (*r, [r] { return r->input != nullptr ? r->input->getTotalLength() : (juce::int64) -1; });
        return r;
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (const juce::File& f) override
    {
        auto* r = juce::WavAudioFormat::createMemoryMappedReader (f);
        if (r != nullptr) keepWithin (*r, [&f] { return f.getSize(); });
        return r;
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (juce::FileInputStream* fin) override
    {
        auto* r = juce::WavAudioFormat::createMemoryMappedReader (fin);          // (the stream is gone after this: it is read and freed inside)
        if (r != nullptr) keepWithin (*r, [r] { return r->getFile().getSize(); });
        return r;
    }

    /** A negative length becomes 0; a length past what the file can hold ('fileBytes' says how big it is) becomes what it can hold. */
    template <typename FileBytes>
    static void keepWithin (juce::AudioFormatReader& r, FileBytes&& fileBytes)
    {
        if (r.lengthInSamples < 0) { r.lengthInSamples = 0; return; }
        const juce::int64 frameBytes = (juce::int64) r.numChannels * (juce::int64) juce::jmax (1, (int) r.bitsPerSample / 8);
        if (frameBytes <= 0 || r.lengthInSamples <= 0xffffffffLL / frameBytes) return;      // no more than a plain header can say: nothing to ask
        const juce::int64 total = fileBytes();
        const juce::int64 most = total > 0 ? total / frameBytes : 0;
        if (r.lengthInSamples > most) r.lengthInSamples = most;
    }
};

/** What every AudioFormatManager in the program is set up with: JUCE's basic formats, with the checked WAV reader in place of the plain one. */
inline void registerAudioFormats (juce::AudioFormatManager& fm)
{
    fm.registerFormat (new CheckedWavAudioFormat(), true);
    fm.registerFormat (new juce::AiffAudioFormat(), false);
   #if JUCE_USE_FLAC
    fm.registerFormat (new juce::FlacAudioFormat(), false);
   #endif
   #if JUCE_USE_OGGVORBIS
    fm.registerFormat (new juce::OggVorbisAudioFormat(), false);
   #endif
   #if JUCE_MAC || JUCE_IOS
    fm.registerFormat (new juce::CoreAudioFormat(), false);
   #endif
   #if JUCE_USE_MP3AUDIOFORMAT
    fm.registerFormat (new juce::MP3AudioFormat(), false);
   #endif
   #if JUCE_USE_WINDOWS_MEDIA_FORMAT
    fm.registerFormat (new juce::WindowsMediaAudioFormat(), false);
   #endif
}
} // namespace td

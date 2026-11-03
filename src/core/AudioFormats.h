#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

namespace td
{
/** JUCE's WAV reader believes the sizes in a file's header. A damaged header can make it report a length the file cannot hold: an RF64 file whose
    64-bit data size is nonsense comes out with a negative length, and reading it then writes outside the buffer being filled (a crash, found by the
    3.0.0 fuzz test). This reader keeps the length within what the file's bytes can actually hold. A whole file is never changed by it: its data is
    never larger than the file, so the check only ever catches a header that is wrong. (The file's size is only asked for when the header claims
    more than a plain WAV header can - over 4 GB - so opening the thousands of files of a big edit costs nothing more.) */
namespace checkedformat
{
    /** 3.27.1: the header's shape itself. JUCE's WAV reader reads through a 5760-byte buffer in whole frames: a header saying more channels than fit in it
        (over 1920 at 24 bit) makes every read ask for 0 frames and loop for ever; JUCE's AIFF reader divides by the frame size, which a header saying
        fewer than 8 bits makes 0. Neither header can come from a real recording (nothing Fermata does uses more channels than kMaxChannels), so such a
        reader is refused as "could not be read" rather than handed out. */
    constexpr int kMaxChannels = 256;
    inline bool sane (const juce::AudioFormatReader& r) noexcept
    {
        // (unsigned throughout: JUCE reads the WAV channel count as a signed 16-bit number into an unsigned int, so 0xFFFF channels arrive as about four billion)
        return r.numChannels >= 1u && r.numChannels <= (unsigned int) kMaxChannels && r.bitsPerSample >= 8u && r.bitsPerSample <= 32u && r.sampleRate > 0.0;
    }
    /** A negative length becomes 0; a length past what the file can hold ('fileBytes' says how big it is) becomes what it can hold. */
    template <typename FileBytes>
    inline void keepWithin (juce::AudioFormatReader& r, FileBytes&& fileBytes)
    {
        if (r.lengthInSamples < 0) { r.lengthInSamples = 0; return; }
        const juce::int64 frameBytes = (juce::int64) r.numChannels * (juce::int64) juce::jmax (1, (int) r.bitsPerSample / 8);
        if (frameBytes <= 0 || r.lengthInSamples <= 0xffffffffLL / frameBytes) return;      // no more than a plain header can say: nothing to ask
        const juce::int64 total = fileBytes();
        const juce::int64 most = total > 0 ? total / frameBytes : 0;
        if (r.lengthInSamples > most) r.lengthInSamples = most;
    }
    /** The reader checked (shape, then length), or deleted and null. */
    template <typename Reader, typename FileBytes>
    inline Reader* checked (Reader* r, FileBytes&& fileBytes)
    {
        if (r == nullptr) return nullptr;
        if (! sane (*r)) { delete r; return nullptr; }
        keepWithin (*r, fileBytes);
        return r;
    }
}

class CheckedWavAudioFormat : public juce::WavAudioFormat
{
public:
    juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
    {
        auto* r = juce::WavAudioFormat::createReaderFor (in, deleteStreamIfOpeningFails);
        return checkedformat::checked (r, [r] { return r->input != nullptr ? r->input->getTotalLength() : (juce::int64) -1; });
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (const juce::File& f) override
    {
        auto* r = juce::WavAudioFormat::createMemoryMappedReader (f);
        return checkedformat::checked (r, [&f] { return f.getSize(); });
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (juce::FileInputStream* fin) override
    {
        auto* r = juce::WavAudioFormat::createMemoryMappedReader (fin);          // (the stream is gone after this: it is read and freed inside)
        return checkedformat::checked (r, [r] { return r->getFile().getSize(); });
    }

    template <typename FileBytes>
    static void keepWithin (juce::AudioFormatReader& r, FileBytes&& fileBytes) { checkedformat::keepWithin (r, fileBytes); }
};

/** 3.27.1: the same checks over JUCE's AIFF reader (an AIFF is accepted everywhere a WAV is: imports, relinks, AAF-linked files). */
class CheckedAiffAudioFormat : public juce::AiffAudioFormat
{
public:
    juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
    {
        auto* r = juce::AiffAudioFormat::createReaderFor (in, deleteStreamIfOpeningFails);
        return checkedformat::checked (r, [r] { return r->input != nullptr ? r->input->getTotalLength() : (juce::int64) -1; });
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (const juce::File& f) override
    {
        auto* r = juce::AiffAudioFormat::createMemoryMappedReader (f);
        return checkedformat::checked (r, [&f] { return f.getSize(); });
    }
    juce::MemoryMappedAudioFormatReader* createMemoryMappedReader (juce::FileInputStream* fin) override
    {
        auto* r = juce::AiffAudioFormat::createMemoryMappedReader (fin);
        return checkedformat::checked (r, [r] { return r->getFile().getSize(); });
    }
};

/** What every AudioFormatManager in the program is set up with: JUCE's basic formats, with the checked WAV and AIFF readers in place of the plain ones. */
inline void registerAudioFormats (juce::AudioFormatManager& fm)
{
    fm.registerFormat (new CheckedWavAudioFormat(), true);
    fm.registerFormat (new CheckedAiffAudioFormat(), false);
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

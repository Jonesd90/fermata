#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <set>

namespace td
{
/** The 'Waveforms' folder: what Fermata has worked out about the look of each audio file (the waveform outline and the colours) is kept there,
    so opening a project again does not read every file through once more. A saved file is only used if the audio file still has the same name,
    size and modified date, so editing or replacing audio just makes a new one. The folder can be deleted at any time: it is rebuilt as needed. */
namespace wavestore
{
constexpr juce::int32 kMagicThumb = 0x46575448, kMagicColour = 0x46574331;       // 'FWTH', 'FWC1'

inline juce::File fileFor (const juce::File& folder, const juce::File& audio, const char* ext)
{
    const auto id = audio.getFileName() + "|" + juce::String (audio.getSize()) + "|" + juce::String (audio.getLastModificationTime().toMilliseconds());
    return folder.getChildFile (audio.getFileNameWithoutExtension().substring (0, 50) + "_" + juce::String::toHexString (id.hashCode64()) + ext);
}
inline void writeHeader (juce::OutputStream& o, juce::int32 magic, const juce::File& audio)
{
    o.writeInt (magic); o.writeInt64 (audio.getSize()); o.writeInt64 (audio.getLastModificationTime().toMilliseconds());
}
inline bool readHeader (juce::InputStream& i, juce::int32 magic, const juce::File& audio)
{
    return i.readInt() == magic && i.readInt64() == audio.getSize() && i.readInt64() == audio.getLastModificationTime().toMilliseconds();
}

/** Fills a thumbnail from the Waveforms folder. False if there is no (valid) saved one: the caller then reads the audio as usual. */
inline bool loadThumbnail (juce::AudioThumbnail& t, const juce::File& folder, const juce::File& audio)
{
    const auto f = fileFor (folder, audio, ".fwave");
    if (! f.existsAsFile()) return false;
    juce::FileInputStream in (f);
    if (! in.openedOk() || ! readHeader (in, kMagicThumb, audio)) return false;
    return t.loadFrom (in);
}
inline void saveThumbnail (juce::AudioThumbnail& t, const juce::File& folder, const juce::File& audio)
{
    if (! t.isFullyLoaded() || t.getTotalLength() <= 0.0 || ! audio.existsAsFile()) return;
    if (! folder.createDirectory().wasOk()) return;
    const auto f = fileFor (folder, audio, ".fwave");
    juce::TemporaryFile tmp (f);
    {
        juce::FileOutputStream out (tmp.getFile());
        if (! out.openedOk()) return;
        writeHeader (out, kMagicThumb, audio);
        t.saveTo (out);
    }
    tmp.overwriteTargetFileWithTemporary();
}

/** The thumbnails of one window: asks the Waveforms folder first, and saves each one once it has been read from the audio. */
struct ThumbSaver
{
    std::set<juce::String> done;                                  // keys already on disk (or loaded from there)
    void markDone (const juce::String& key) { done.insert (key); }
    template <typename Map>
    void saveFinished (Map& thumbs, const juce::File& folder)
    {
        for (auto& kv : thumbs)
            if (done.count (kv.first) == 0 && kv.second->isFullyLoaded())
            {
                done.insert (kv.first);
                saveThumbnail (*kv.second, folder, juce::File (kv.first));
            }
    }
};
} // namespace wavestore
} // namespace td

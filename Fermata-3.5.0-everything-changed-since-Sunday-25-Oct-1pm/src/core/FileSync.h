#pragma once
#include <juce_core/juce_core.h>

namespace td::filesync
{
/** Asks the drive to take the file's bytes now. The system otherwise keeps what was just written in memory for a while and writes it when it
    pleases, so a power cut right after a save could leave the file empty or half written although the program had been told it was saved.
    Best effort: returns false if the file could not be opened (a drive that will not do it is still written). (3.1.3: through JUCE's stream, whose
    flush() is FlushFileBuffers on Windows and fsync elsewhere, so this header no longer pulls windows.h into every file that includes it.) */
inline bool syncToDisk (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    juce::FileOutputStream out (f);                          // (opened for writing, nothing written: flush() only asks the drive to take what it has)
    if (out.failedToOpen()) return false;
    out.flush();
    return out.getStatus().wasOk();
}

/** Where the previous version of a saved file is kept: <name>.bak beside it. */
inline juce::File backupOf (const juce::File& f) { return f.getSiblingFile (f.getFileName() + ".bak"); }

/** Where a file that could not be read is put aside when its backup is used instead: <name>.damaged beside it. */
inline juce::File damagedCopyOf (const juce::File& f) { return f.getSiblingFile (f.getFileName() + ".damaged"); }

/** Writes 'bytes' to a new file 'f' and checks that all of it got there: every write's result, the stream's state after the flush, and the size of the
    file on the disk against what was written. (3.1.3. JUCE's File::replaceWithText / replaceWithData do not look at the result of their write, and a
    FileOutputStream only writes its last buffer in its destructor, where nothing can report a failure: on a full drive they left a short file and
    said it had gone well. Every file Fermata writes whole goes through here, or through replaceTextSafely below.) */
namespace detail
{
/** 'put' writes the file's contents to the stream and returns false if any write was refused; the file must then be exactly as long as the stream says.
    A file that failed is left as it is (short) for the caller to deal with: the temporary files this is used on are removed by their owners, and a file
    written in place is reported, never quietly removed. */
template <typename Put> bool writeChecked (const juce::File& f, Put&& put, juce::String* error)
{
    auto fail = [&] (const juce::String& why) { if (error != nullptr) *error = "Could not write " + f.getFullPathName() + ": " + why; return false; };
    juce::int64 written = 0;
    {
        juce::FileOutputStream out (f);
        if (out.failedToOpen()) return fail (out.getStatus().getErrorMessage().isNotEmpty() ? out.getStatus().getErrorMessage() : juce::String ("the file could not be made"));
        if (! out.setPosition (0) || ! out.truncate().wasOk()) return fail ("the file could not be started afresh");
        const bool ok = put (out);
        out.flush();
        written = out.getPosition();
        if (! ok || out.getStatus().failed()) return fail (out.getStatus().getErrorMessage().isNotEmpty() ? out.getStatus().getErrorMessage() : juce::String ("the drive did not take all of it (is it full?)"));
    }
    if (f.getSize() != written) return fail ("the drive took only part of it (is it full?)");
    return true;
}
}

inline bool writeDataChecked (const juce::File& f, const void* bytes, size_t size, juce::String* error = nullptr)
{
    return detail::writeChecked (f, [&] (juce::FileOutputStream& out) { return size == 0 || out.write (bytes, size); }, error);
}

/** The text form of writeDataChecked: UTF-8, with Windows line breaks, as JUCE's File::replaceWithText wrote it (so the files come out byte for byte as before). */
inline bool writeTextChecked (const juce::File& f, const juce::String& text, juce::String* error = nullptr)
{
    return detail::writeChecked (f, [&] (juce::FileOutputStream& out) { return out.writeText (text, false, false, "\r\n"); }, error);
}

/** Writes text to a file so that what is on the disk is always a complete file: either the old one or the new one, never half of either.
    The text goes to a temporary file beside the target (checked: writeTextChecked), the drive is asked to take it (syncToDisk), and only then
    does it take the target's place. With keepBackup the file that was there before becomes <name>.bak, so a save that is interrupted (or a file
    that goes wrong later) still leaves the version from the save before it. */
inline bool replaceTextSafely (const juce::File& target, const juce::String& text, bool keepBackup, juce::String* error = nullptr)
{
    target.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (target);
    if (! writeTextChecked (tmp.getFile(), text, error))
    {
        if (error != nullptr && error->isEmpty()) *error = "Could not write " + target.getFullPathName();
        return false;
    }
    syncToDisk (tmp.getFile());
    juce::File bak;
    if (keepBackup && target.existsAsFile())
    {
        bak = backupOf (target);
        bak.deleteFile();
        if (! target.moveFileTo (bak)) bak = juce::File();            // (could not be kept: the file is replaced below all the same)
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        if (bak != juce::File()) bak.moveFileTo (target);           // put the old file back rather than leave none
        if (error != nullptr) *error = "Could not write " + target.getFullPathName();
        return false;
    }
    return true;
}

/** The project file a project backup belongs to: "Name.fermata.bak" -> "Name.fermata", and a copy kept aside by restoreBackupText,
    "Name (replaced 2026-10-20 00.10.00).fermata.bak", -> "Name.fermata" too (so going back is opening that copy the same way).
    An empty File when 'bak' is not the backup of a project file. */
inline juce::File projectFileForBackup (const juce::File& bak)
{
    if (! bak.hasFileExtension ("bak")) return {};
    const auto inner = bak.getSiblingFile (bak.getFileNameWithoutExtension());           // Name.fermata, or Name (replaced ...).fermata
    if (! (inner.hasFileExtension ("fermata") || inner.hasFileExtension ("takedaw"))) return {};
    auto base = inner.getFileNameWithoutExtension();
    if (base.endsWithChar (')') && base.contains (" (replaced ")) base = base.upToLastOccurrenceOf (" (replaced ", false, false);
    if (base.isEmpty()) return {};
    return bak.getSiblingFile (base + inner.getFileExtension());
}

/** Opening a file's backup on purpose ("the save before"): 'target' - if it is there - is first copied aside as "<name> (replaced <stamp>)<ext>.bak"
    (nothing is ever deleted; a .bak so it is not opened as a second project by mistake, and opening it the same way goes back), then 'text' (what
    was in the backup, read before anything was saved) takes its place, written safely; the file that was there becomes <name>.bak, as at every
    save. 'aside' is set to the copy kept (empty if there was no file). */
inline bool restoreBackupText (const juce::File& target, const juce::String& text, const juce::String& stamp, juce::File& aside, juce::String& error)
{
    aside = juce::File();
    if (target.existsAsFile())
    {
        for (int n = 1; n < 1000; ++n)
        {
            aside = target.getSiblingFile (target.getFileNameWithoutExtension() + " (replaced " + stamp + (n > 1 ? " " + juce::String (n) : juce::String()) + ")"
                                           + target.getFileExtension() + ".bak");
            if (! aside.exists()) break;
        }
        if (aside.exists() || ! target.copyFileTo (aside)) { error = "A copy of " + target.getFileName() + " could not be kept, so nothing was changed."; aside = juce::File(); return false; }
    }
    return replaceTextSafely (target, text, true, &error);
}

/** Reads a JSON file into v. If the file is missing or cannot be parsed but its .bak can, the .bak is used: the broken file is put aside as
    <name>.damaged and the .bak copied into its place, so the folder is whole again. Returns 0 when nothing could be read, 1 when the file was
    read, 2 when the backup had to be used. 'accept' says whether the parsed value is what the file should hold (the right format, say). */
template <typename Accept>
inline int readJsonWithBackup (const juce::File& f, juce::var& v, Accept&& accept)
{
    v = juce::var();
    if (f.existsAsFile() && ! juce::JSON::parse (f.loadFileAsString(), v).failed() && accept (v)) return 1;
    const auto bak = backupOf (f);
    v = juce::var();
    if (! bak.existsAsFile() || juce::JSON::parse (bak.loadFileAsString(), v).failed() || ! accept (v)) { v = juce::var(); return 0; }
    if (f.existsAsFile())
    {
        const auto aside = damagedCopyOf (f);
        aside.deleteFile();
        if (! f.moveFileTo (aside)) f.deleteFile();
    }
    bak.copyFileTo (f);
    return 2;
}
} // namespace td::filesync

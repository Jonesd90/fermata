#pragma once
#include "Project.h"

namespace td
{
/** Makes a complete, checked copy of a project in a new folder: the project file, every recording and everything bounced, so the folder can be
    taken to another computer (on an external drive, say) and opened there. Runs on a background thread: nothing here touches the live project. */
namespace projectcopy
{
struct Report { int files = 0; int missing = 0; juce::int64 bytes = 0; juce::File projectFile; };

namespace detail
{
inline juce::uint32 crcUpdate (juce::uint32 crc, const unsigned char* p, size_t n)
{
    static juce::uint32 table[256]; static bool made = false;
    if (! made) { for (juce::uint32 i = 0; i < 256; ++i) { juce::uint32 c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; table[i] = c; } made = true; }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}
struct Item { juce::File src, dst; juce::int64 size = 0; juce::uint32 crc = 0; };

/** Every object in the saved project that names an audio file (it has a "file" and "channels"). */
inline void collect (const juce::var& v, std::vector<juce::var>& out)
{
    if (auto* a = v.getArray()) { for (auto& e : *a) collect (e, out); return; }
    if (auto* o = v.getDynamicObject())
    {
        if (o->hasProperty ("file") && o->hasProperty ("channels")) out.push_back (v);
        for (auto& pr : o->getProperties()) collect (pr.value, out);
    }
}
}

/** 'progress' gets 0..1 and a short text; return false from it to cancel. 'root' is a snapshot of the project (Project::toVar()). Returns "" when all went well. */
inline juce::String run (juce::var root, const juce::File& srcFolder, const juce::File& destFolder, const juce::String& newName, bool copyAudio,
                         const std::function<bool (float, const juce::String&)>& progress, Report& rep)
{
    using namespace detail;
    if (newName.trim().isEmpty()) return "Give the copy a name.";
    if (destFolder == srcFolder || destFolder.isAChildOf (srcFolder) || srcFolder.isAChildOf (destFolder))
        return "The copy cannot go inside the project's own folder (or contain it). Choose another place, such as another drive.";
    if (destFolder.exists() && destFolder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0)
        return "The folder '" + destFolder.getFullPathName() + "' already exists and has things in it. Choose a new name.";
    if (! destFolder.createDirectory()) return "Cannot create " + destFolder.getFullPathName();

    std::vector<Item> items;
    std::map<juce::String, juce::File> mapped;                 // original full path -> where it is in the copy
    auto add = [&] (const juce::File& s, const juce::File& d)
    {
        if (mapped.count (s.getFullPathName()) != 0) return;
        Item it; it.src = s; it.dst = d; it.size = s.getSize(); items.push_back (it); mapped[s.getFullPathName()] = d;
    };
    if (copyAudio)
    {
        for (auto& f : srcFolder.findChildFiles (juce::File::findFiles, true, "*"))
        {
            if (f.hasFileExtension ("takedaw") || f.hasFileExtension ("fermata") || f.hasFileExtension ("fmedit") || f.hasFileExtension ("fmtake") || f.hasFileExtension ("fmmix") || f.getFileName().endsWith ("~")) continue;
            add (f, destFolder.getChildFile (f.getRelativePathFrom (srcFolder)));
        }
        std::vector<juce::var> objs; collect (root, objs);
        for (auto& o : objs)                                         // audio that lives somewhere else is brought in too
        {
            const juce::File f (o["file"].toString());
            if (mapped.count (f.getFullPathName()) != 0) continue;
            if (! f.existsAsFile()) { ++rep.missing; continue; }
            auto d = destFolder.getChildFile ("Recorded Media").getChildFile ("From elsewhere").getChildFile (f.getFileName());
            for (int k = 2; d.exists() || std::any_of (items.begin(), items.end(), [&] (const Item& i) { return i.dst == d; }); ++k)
                d = d.getSiblingFile (f.getFileNameWithoutExtension() + " (" + juce::String (k) + ")" + f.getFileExtension());
            add (f, d);
        }
    }
    juce::int64 total = 0; for (auto& i : items) total += i.size;
    if (destFolder.getBytesFreeOnVolume() > 0 && destFolder.getBytesFreeOnVolume() < total + 200LL * 1024 * 1024)
        return "Not enough room: the copy needs " + juce::File::descriptionOfSizeInBytes (total) + " and the drive has " + juce::File::descriptionOfSizeInBytes (destFolder.getBytesFreeOnVolume()) + " free.";

    // copy (first half of the progress), then read every copied file back and compare (second half)
    juce::int64 done = 0; const double all = (double) juce::jmax ((juce::int64) 1, total) * 2.0;
    std::vector<unsigned char> buf (4 * 1024 * 1024);
    for (auto& it : items)
    {
        if (! progress ((float) ((double) done / all), "Copying " + it.src.getFileName())) return "Cancelled.";
        it.dst.getParentDirectory().createDirectory();
        juce::FileInputStream in (it.src);
        if (in.failedToOpen()) return "Cannot read " + it.src.getFullPathName();
        {
            juce::FileOutputStream out (it.dst);
            if (out.failedToOpen()) return "Cannot write " + it.dst.getFullPathName();
            out.setPosition (0); out.truncate();
            juce::uint32 crc = 0xffffffffu;
            for (;;)
            {
                const auto n = in.read (buf.data(), (int) buf.size());
                if (n <= 0) break;
                crc = crcUpdate (crc, buf.data(), (size_t) n);
                if (! out.write (buf.data(), (size_t) n)) return "The drive is full or not writable: " + it.dst.getFullPathName();
                done += n;
                if (! progress ((float) ((double) done / all), "Copying " + it.src.getFileName())) { out.flush(); return "Cancelled."; }
            }
            out.flush();
            it.crc = ~crc;
        }
        it.dst.setLastModificationTime (it.src.getLastModificationTime());
        rep.bytes += it.size; ++rep.files;
    }
    juce::int64 checked = 0;
    for (auto& it : items)
    {
        if (! progress ((float) ((double) (total + checked) / all), "Checking " + it.dst.getFileName())) return "Cancelled.";
        if (it.dst.getSize() != it.size) return "The copy of " + it.src.getFileName() + " is not the same size as the original.";
        juce::FileInputStream in (it.dst);
        if (in.failedToOpen()) return "Cannot read back " + it.dst.getFullPathName();
        juce::uint32 crc = 0xffffffffu;
        for (;;)
        {
            const auto n = in.read (buf.data(), (int) buf.size());
            if (n <= 0) break;
            crc = crcUpdate (crc, buf.data(), (size_t) n);
            checked += n;
            if (! progress ((float) ((double) (total + checked) / all), "Checking " + it.dst.getFileName())) return "Cancelled.";
        }
        if (~crc != it.crc) return "The copy of " + it.src.getFileName() + " does not match the original (the drive may be faulty). Nothing was lost: the original is untouched.";
    }

    // the project file of the copy: its audio paths point into the copy
    {
        std::vector<juce::var> objs; collect (root, objs);
        for (auto& o : objs)
        {
            auto* dyn = o.getDynamicObject();
            auto m = mapped.find (o["file"].toString());
            if (m == mapped.end()) continue;
            dyn->setProperty ("file", m->second.getFullPathName());
            dyn->setProperty ("rel", m->second.getRelativePathFrom (destFolder).replaceCharacter ('\\', '/'));
        }
        if (auto* ro = root.getDynamicObject()) ro->setProperty ("name", newName);
    }
    for (const char* sub : { "Recorded Media", "Bounced Media", "Bounced Media/Mastered Audio" }) destFolder.getChildFile (sub).createDirectory();
    rep.projectFile = destFolder.getChildFile (juce::File::createLegalFileName (newName) + ".fermata");
    juce::TemporaryFile tmp (rep.projectFile);
    if (! tmp.getFile().replaceWithText (juce::JSON::toString (root, false)) || ! tmp.overwriteTargetFileWithTemporary())
        return "Could not write the project file " + rep.projectFile.getFullPathName();
    progress (1.0f, "Done");
    return {};
}
} // namespace projectcopy
} // namespace td

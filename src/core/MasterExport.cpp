#include "MasterExport.h"
#include "Playback.h"
#include "AudioOps.h"
#include "Resampler.h"
#include "Dither.h"

namespace td
{
namespace
{
constexpr int kChunk = 32768;

MasterTags mergeTags (MasterTags t, const MasterTags& album, const juce::String& defaultTitle)
{
    if (t.title.isEmpty()) t.title = defaultTitle;
    if (t.albumArtist.isEmpty()) t.albumArtist = album.albumArtist.isNotEmpty() ? album.albumArtist : album.artist;
    if (t.artist.isEmpty()) t.artist = album.artist.isNotEmpty() ? album.artist : album.albumArtist;
    if (t.album.isEmpty()) t.album = album.album;
    if (t.composer.isEmpty()) t.composer = album.composer;
    if (t.genre.isEmpty()) t.genre = album.genre;
    if (t.year.isEmpty()) t.year = album.year;
    if (t.comment.isEmpty()) t.comment = album.comment;
    if (t.copyright.isEmpty()) t.copyright = album.copyright;
    return t;
}

void appendLe32 (juce::MemoryBlock& b, juce::uint32 v) { for (int i = 0; i < 4; ++i) { const juce::uint8 x = (juce::uint8) (v >> (8 * i)); b.append (&x, 1); } }

bool isRateOkForMp3 (double r) { return std::abs (r - 32000) < 1 || std::abs (r - 44100) < 1 || std::abs (r - 48000) < 1; }
} // namespace

double masterTargetRate (const MasterExportSettings& o, double editRate)
{
    if (o.format == MasterFormat::Mp3)
    {
        if (o.sampleRate > 0.0) return o.sampleRate;
        if (isRateOkForMp3 (editRate)) return editRate;
        return std::fmod (editRate, 11025.0) < 1.0 ? 44100.0 : 48000.0;
    }
    return o.sampleRate > 0.0 ? o.sampleRate : editRate;
}

juce::String masterFileExtension (MasterFormat f)
{
    switch (f) { case MasterFormat::Wav: return ".wav"; case MasterFormat::Aiff: return ".aif"; case MasterFormat::Flac: return ".flac"; case MasterFormat::Ogg: return ".ogg"; case MasterFormat::Mp3: return ".mp3"; }
    return ".wav";
}

juce::String findLame()
{
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (auto name : { "lame.exe", "lame" })
        for (auto& dir : { exeDir, exeDir.getChildFile ("lame"), exeDir.getChildFile ("tools") })
        {
            auto f = dir.getChildFile (name);
            if (f.existsAsFile()) return f.getFullPathName();
        }
    juce::ChildProcess p;                                                    // or one that is installed on the PATH
    if (p.start (juce::StringArray { "lame", "--version" }))
        if (p.waitForProcessToFinish (4000) && p.getExitCode() == 0) return "lame";
    return {};
}

bool addFlacTags (const juce::File& flac, const MasterTags& t, int trackNo)
{
    juce::MemoryBlock data;
    if (! flac.loadFileAsData (data) || data.getSize() < 8 || std::memcmp (data.getData(), "fLaC", 4) != 0) return false;
    auto* b = static_cast<juce::uint8*> (data.getData());
    size_t pos = 4, lastHeader = 0; bool done = false;
    while (! done)
    {
        if (pos + 4 > data.getSize()) return false;
        lastHeader = pos;
        done = (b[pos] & 0x80) != 0;
        const size_t len = ((size_t) b[pos + 1] << 16) | ((size_t) b[pos + 2] << 8) | b[pos + 3];
        pos += 4 + len;
        if (pos > data.getSize()) return false;
    }
    std::vector<std::pair<juce::String, juce::String>> kv {
        { "TITLE", t.title }, { "ARTIST", t.artist }, { "ALBUM", t.album }, { "ALBUMARTIST", t.albumArtist }, { "COMPOSER", t.composer }, { "GENRE", t.genre },
        { "DATE", t.year }, { "COMMENT", t.comment }, { "COPYRIGHT", t.copyright }, { "ISRC", compactIsrc (t.isrc) }, { "TRACKNUMBER", trackNo > 0 ? juce::String (trackNo) : juce::String() } };
    juce::MemoryBlock vc;
    const juce::String vendor = "Fermata";
    appendLe32 (vc, (juce::uint32) vendor.getNumBytesAsUTF8()); vc.append (vendor.toRawUTF8(), vendor.getNumBytesAsUTF8());
    juce::uint32 count = 0; for (auto& p : kv) if (p.second.isNotEmpty()) ++count;
    appendLe32 (vc, count);
    for (auto& p : kv)
        if (p.second.isNotEmpty())
        {
            const auto line = p.first + "=" + p.second;
            appendLe32 (vc, (juce::uint32) line.getNumBytesAsUTF8()); vc.append (line.toRawUTF8(), line.getNumBytesAsUTF8());
        }
    juce::MemoryBlock out;
    b[lastHeader] &= 0x7f;                                                   // the old last block is not the last any more
    out.append (data.getData(), pos);
    const juce::uint8 hdr[4] = { 0x84, (juce::uint8) (vc.getSize() >> 16), (juce::uint8) (vc.getSize() >> 8), (juce::uint8) vc.getSize() };   // VORBIS_COMMENT, last
    out.append (hdr, 4); out.append (vc.getData(), vc.getSize());
    out.append (static_cast<const char*> (data.getData()) + pos, data.getSize() - pos);
    return flac.replaceWithData (out.getData(), out.getSize());
}

// ----------------------------------------------------------------------------- building the lists
juce::String resolveRenderSource (const Project& p, const MasteringDef& def, juce::Uuid& mixerId, juce::Uuid& sourceId)
{
    mixerId = juce::Uuid::null(); sourceId = juce::Uuid::null();
    if (p.mixers.empty()) return "The project has no mixer.";
    mixerId = p.mixers.front()->id;
    if (! def.mixerId.isNull()) for (auto& m : p.mixers) if (m->id == def.mixerId) mixerId = m->id;
    if (! def.sourceId.isNull() && p.kindOf (def.sourceId) != NodeKind::None) sourceId = def.sourceId;
    else for (auto& b : p.buses) if (b.external) { sourceId = b.id; break; }
    if (sourceId.isNull()) return "There is nothing to render yet: add an Ext bus (and route the tracks to it) in the Project Designer, or choose an output in the Mastering window.";
    return {};
}

namespace
{
juce::String commonChecks (const Project& p, const MasteringDef& def, MasterJobSpec& spec, std::vector<EditDef*>& edits)
{
    juce::String err = resolveRenderSource (p, def, spec.mixerId, spec.sourceId);
    if (err.isNotEmpty()) return err;
    spec.tailSeconds = def.tailSeconds;
    if (edits.empty()) return "Tick at least one edit that has audio in it.";
    for (auto* e : edits) if (std::abs (e->sampleRate - edits.front()->sampleRate) > 0.5)
        return "The ticked edits have different sample rates (" + juce::String (edits.front()->sampleRate, 0) + " and " + juce::String (e->sampleRate, 0) + " Hz). Tick edits with the same rate and export the others separately.";
    return {};
}
} // namespace

juce::String makeFilesSpec (const Project& p, const MasteringDef& def, MasterJobSpec& spec)
{
    spec = MasterJobSpec();
    spec.kind = MasterJobSpec::Kind::Files; spec.out = def.vm;
    if (def.vm.folder.trim().isEmpty()) return "Choose the folder the files go into.";
    spec.destFolder = juce::File (def.vm.folder.trim());
    spec.workFolder = spec.destFolder.getChildFile ("_fermata_work");
    std::vector<EditDef*> edits; std::vector<const MasterItem*> its;
    for (auto& it : def.items)
    {
        if (! it.include) continue;
        auto* e = const_cast<Project&> (p).findEdit (it.editId);
        if (e == nullptr || e->isEmpty()) continue;
        edits.push_back (e); its.push_back (&it);
    }
    auto err = commonChecks (p, def, spec, edits);
    if (err.isNotEmpty()) return err;
    if (def.vm.format == MasterFormat::Mp3 || (def.vm.extraFormats & (1 << (int) MasterFormat::Mp3)) != 0)
    {
        if (findLame().isEmpty()) return "MP3 files need the free LAME encoder. Put lame.exe in the same folder as Fermata, then try again. (LAME is not included because it is a separate program.)";
        const double r = def.vm.sampleRate > 0.0 ? def.vm.sampleRate : edits.front()->sampleRate;
        if (! isRateOkForMp3 (masterTargetRate (def.vm, r))) return "MP3 supports 32, 44.1 and 48 kHz only. Choose one of those sample rates.";
    }
    int n = 0;
    for (size_t i = 0; i < edits.size(); ++i)
    {
        ++n;
        MasterJobItem j; j.editId = edits[i]->id; j.trackNo = n;
        const auto base = its[i]->fileName.trim().isNotEmpty() ? its[i]->fileName.trim() : edits[i]->name;
        j.baseName = def.vm.numberFiles ? juce::String (n).paddedLeft ('0', 2) + " - " + base : base;
        j.tags = mergeTags (its[i]->tags, def.vm.album, its[i]->tags.title.isNotEmpty() ? its[i]->tags.title : base);
        spec.items.push_back (j);
    }
    return {};
}

juce::String makeDiscSpec (const Project& p, const MasteringDef& def, MasterJobSpec& spec)
{
    spec = MasterJobSpec();
    spec.kind = MasterJobSpec::Kind::Disc; spec.disc = def.ddp;
    if (def.ddp.folder.trim().isEmpty()) return "Choose the folder the DDP goes into.";
    spec.destFolder = juce::File (def.ddp.folder.trim());
    spec.workFolder = spec.destFolder.getChildFile ("_fermata_work");
    spec.out = MasterExportSettings();
    spec.out.format = MasterFormat::Wav; spec.out.sampleRate = 44100.0; spec.out.bitDepth = 16; spec.out.dither = DitherMode::Tpdf;
    spec.out.normalise = def.ddp.normalise; spec.out.peakDb = def.ddp.peakDb; spec.out.peakTogether = def.ddp.peakTogether;
    spec.discName = def.ddp.title.trim().isNotEmpty() ? def.ddp.title.trim() : juce::String ("CD master");
    std::vector<EditDef*> edits;
    for (size_t i = 0; i < def.ddp.clips.size(); ++i)
    {
        auto& c = def.ddp.clips[i];
        if (! c.include) continue;
        auto* e = const_cast<Project&> (p).findEdit (c.editId);
        if (e == nullptr || e->isEmpty()) continue;
        edits.push_back (e); spec.clipIndex.push_back ((int) i);
    }
    auto err = commonChecks (p, def, spec, edits);
    if (err.isNotEmpty()) return err;
    int n = 0;
    for (auto* e : edits) { MasterJobItem j; j.editId = e->id; j.trackNo = ++n; j.baseName = "clip-" + juce::String (n).paddedLeft ('0', 2); spec.items.push_back (j); }
    return {};
}

// ----------------------------------------------------------------------------- the job
MasterExportJob::MasterExportJob (const Project& live, const MasterJobSpec& s, std::function<void (MasterExportResult)> done, bool startNow)
    : juce::Thread ("Mastering export"), spec (s), onDone (std::move (done))
{
    BounceSettings bs;
    bs.automationEdit = juce::Uuid::null();
    bs.folder = spec.workFolder; bs.mixerId = spec.mixerId; bs.sources = { spec.sourceId };
    bs.addOutputName = false; bs.floatFiles = true; bs.normalise = false; bs.tailSeconds = spec.tailSeconds;
    for (size_t i = 0; i < spec.items.size(); ++i)
    {
        auto* e = const_cast<Project&> (live).findEdit (spec.items[i].editId);
        if (e == nullptr || e->isEmpty()) { earlyError = "An edit no longer exists or is empty."; break; }
        if (i == 0) bs.sampleRate = e->sampleRate;
        editRates.push_back (e->sampleRate);
        BounceItem bi;
        bi.name = "item-" + juce::String ((int) i + 1).paddedLeft ('0', 2);
        bi.start = e->firstSample(); bi.end = e->lengthSamples();
        bi.segments = segmentsForEdit (live, *e);
        bi.automationEdit = e->automationOn ? e->id : juce::Uuid::null();
        bs.items.push_back (std::move (bi));
    }
    if (earlyError.isEmpty())
    {
        bouncer = std::make_unique<Bouncer> (live, bs);
        if (bouncer->getPrepareError().isNotEmpty()) earlyError = bouncer->getPrepareError();
    }
    if (startNow) startThread (juce::Thread::Priority::normal);
}

MasterExportJob::~MasterExportJob()
{
    cancelFlag = true;
    stopThread (15000);
}

float MasterExportJob::getProgress() const
{
    const float p = sub.load();
    switch (stage.load()) { case 0: return 0.55f * p; case 1: return 0.55f + 0.20f * p; default: return 0.75f + 0.25f * p; }
}
juce::String MasterExportJob::getStatus() const { const juce::ScopedLock sl (lock); return status; }

void MasterExportJob::run()
{
    auto result = execute();
    auto fn = onDone;
    juce::MessageManager::callAsync ([fn, result] { if (fn) fn (result); });
}

juce::String MasterExportJob::finalise (const juce::File& src, const juce::File& dest, double rate, float gain, const MasterJobItem& item, float& peakOut, MasterFormat fmt)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (src));
    if (r == nullptr) return "Cannot read " + src.getFileName();
    const auto& o = spec.out;
    const bool mp3 = fmt == MasterFormat::Mp3;
    const auto lameIn = spec.workFolder.getChildFile ("lame-in.wav"), lameOut = spec.workFolder.getChildFile ("lame-out.mp3");
    const auto target = mp3 ? lameIn : dest;

    int bits = 24;
    switch (fmt) { case MasterFormat::Wav: bits = o.bitDepth; break; case MasterFormat::Aiff: case MasterFormat::Flac: bits = o.bitDepth == 16 ? 16 : 24; break; case MasterFormat::Ogg: bits = 16; break; case MasterFormat::Mp3: bits = 16; break; }
    const bool floatOut = fmt == MasterFormat::Wav && bits == 32;
    const bool lossy = fmt == MasterFormat::Ogg;
    const bool quantise = ! floatOut && ! lossy;
    const float scale = bits == 16 ? 32768.0f : 8388608.0f;
    const bool dither = quantise && o.dither == DitherMode::Tpdf;

    std::unordered_map<juce::String, juce::String> md;
    const auto& t = item.tags;
    auto add = [&] (const char* k, const juce::String& v) { if (v.isNotEmpty()) md[juce::String (k)] = v; };
    if (fmt == MasterFormat::Wav)
    {
        add (juce::WavAudioFormat::riffInfoTitle, t.title); add (juce::WavAudioFormat::riffInfoArtist, t.artist); add (juce::WavAudioFormat::riffInfoProductName, t.album);
        add (juce::WavAudioFormat::riffInfoGenre, t.genre); add (juce::WavAudioFormat::riffInfoDateCreated, t.year); add (juce::WavAudioFormat::riffInfoComment, t.comment);
        add (juce::WavAudioFormat::riffInfoCopyright, t.copyright); add (juce::WavAudioFormat::riffInfoMusicBy, t.composer);
        if (item.trackNo > 0) add (juce::WavAudioFormat::riffInfoTrackNumber, juce::String (item.trackNo));
    }
    else if (fmt == MasterFormat::Ogg)
    {
        add (juce::OggVorbisAudioFormat::id3title, t.title); add (juce::OggVorbisAudioFormat::id3artist, t.artist); add (juce::OggVorbisAudioFormat::id3album, t.album);
        add (juce::OggVorbisAudioFormat::id3comment, t.comment); add (juce::OggVorbisAudioFormat::id3date, t.year); add (juce::OggVorbisAudioFormat::id3genre, t.genre);
        if (item.trackNo > 0) add (juce::OggVorbisAudioFormat::id3trackNumber, juce::String (item.trackNo));
    }

    std::unique_ptr<juce::AudioFormat> af;
    switch (fmt)
    {
        case MasterFormat::Wav: case MasterFormat::Mp3: af = std::make_unique<juce::WavAudioFormat>(); break;
        case MasterFormat::Aiff: af = std::make_unique<juce::AiffAudioFormat>(); break;
        case MasterFormat::Flac: af = std::make_unique<juce::FlacAudioFormat>(); break;
        case MasterFormat::Ogg:  af = std::make_unique<juce::OggVorbisAudioFormat>(); break;
    }
    target.getParentDirectory().createDirectory();
    target.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) return "Cannot write " + target.getFullPathName();
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (bits);
    if (floatOut) opts = opts.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    if (fmt == MasterFormat::Ogg) opts = opts.withQualityOptionIndex (juce::jlimit (0, 10, o.oggQuality));
    if (fmt == MasterFormat::Flac) opts = opts.withQualityOptionIndex (juce::jlimit (0, 8, o.flacLevel));
    if (! md.empty()) opts = opts.withMetadataValues (md);
    auto w = af->createWriterFor (stream, opts);
    if (w == nullptr) return "This file format cannot be written with these settings (" + juce::String (bits) + " bit, " + juce::String (rate, 0) + " Hz).";

    juce::AudioBuffer<float> buf (2, kChunk);
    DitherOut dout (*w, dither, 20260101u);          // TPDF dither, then the integer words are made exactly (not by the writer's own rounding)
    float peak = 0.0f;
    for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += kChunk)
    {
        if (cancelFlag.load()) { w.reset(); target.deleteFile(); return "Cancelled."; }
        const int n = (int) juce::jmin ((juce::int64) kChunk, r->lengthInSamples - pos);
        r->read (&buf, 0, n, pos, true, true);
        for (int c = 0; c < 2; ++c)
        {
            float* d = buf.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i] * gain;
                peak = juce::jmax (peak, std::abs (x));
                if (quantise && ! dither)                      // dither Off: plain rounding to the word length
                    x = juce::jlimit (-scale, scale - 1.0f, std::nearbyint (x * scale)) / scale;
                d[i] = x;
            }
        }
        dout.write (buf, 0, n);
        sub = (float) ((double) (pos + n) / (double) juce::jmax ((juce::int64) 1, r->lengthInSamples));
    }
    w.reset();
    peakOut = peak;
    if (fmt == MasterFormat::Flac) addFlacTags (target, t, item.trackNo);
    if (mp3)
    {
        const auto lame = findLame();
        if (lame.isEmpty()) return "LAME was not found.";
        juce::StringArray args; args.add (lame);
        if (o.mp3Kbps > 0) { args.add ("-b"); args.add (juce::String (o.mp3Kbps)); args.add ("--cbr"); } else { args.add ("-V"); args.add (juce::String (juce::jlimit (0, 9, o.mp3Vbr))); }
        args.add ("--quiet");
        auto tag = [&] (const char* opt, const juce::String& v) { if (v.isNotEmpty()) { args.add (opt); args.add (v); } };
        tag ("--tt", t.title); tag ("--ta", t.artist); tag ("--tl", t.album); tag ("--ty", t.year); tag ("--tc", t.comment); tag ("--tg", t.genre);
        if (item.trackNo > 0) tag ("--tn", juce::String (item.trackNo));
        if (t.composer.isNotEmpty()) { args.add ("--tv"); args.add ("TCOM=" + t.composer); }
        if (t.albumArtist.isNotEmpty()) { args.add ("--tv"); args.add ("TPE2=" + t.albumArtist); }
        if (t.copyright.isNotEmpty()) { args.add ("--tv"); args.add ("TCOP=" + t.copyright); }
        if (compactIsrc (t.isrc).isNotEmpty()) { args.add ("--tv"); args.add ("TSRC=" + compactIsrc (t.isrc)); }
        args.add (lameIn.getFullPathName()); args.add (lameOut.getFullPathName());
        lameOut.deleteFile();
        juce::ChildProcess cp;
        if (! cp.start (args)) return "Could not start LAME.";
        while (cp.isRunning())
        {
            if (cancelFlag.load()) { cp.kill(); lameIn.deleteFile(); return "Cancelled."; }
            juce::Thread::sleep (80);
        }
        const auto output = cp.readAllProcessOutput();
        if (cp.getExitCode() != 0 || ! lameOut.existsAsFile()) { lameIn.deleteFile(); return "LAME failed: " + output.trim(); }
        lameIn.deleteFile();
        dest.deleteFile();
        if (! lameOut.moveFileTo (dest)) return "Cannot write " + dest.getFullPathName();
    }
    return {};
}

MasterExportResult MasterExportJob::execute()
{
    auto R = executeInner();
    if (spec.workFolder.getFileName() == "_fermata_work") spec.workFolder.deleteRecursively();
    return R;
}

MasterExportResult MasterExportJob::executeInner()
{
    MasterExportResult R;
    auto setStatus = [&] (const juce::String& s) { const juce::ScopedLock sl (lock); status = s; };
    if (earlyError.isNotEmpty()) { R.error = earlyError; return R; }
    const int n = (int) spec.items.size();
    if (n == 0 || bouncer == nullptr) { R.error = "Nothing to export."; return R; }
    if (! spec.destFolder.createDirectory()) { R.error = "Cannot create " + spec.destFolder.getFullPathName(); return R; }

    // ---- 1. render every edit through the mixer (32-bit float, nothing clipped)
    stage = 0; sub = 0.0f; setStatus ("Rendering through the mixer...");
    auto br = bouncer->render (&sub, &cancelFlag);
    if (br.cancelled) { R.cancelled = true; return R; }
    if (br.error.isNotEmpty()) { R.error = br.error; return R; }
    if ((int) br.files.size() != n) { R.error = "The renderer made a different number of files than expected."; return R; }
    bouncer.reset();

    // ---- 2. sample rate, and the peak of every file
    stage = 1; sub = 0.0f;
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::vector<juce::File> proc; std::vector<float> peaks ((size_t) n, 0.0f); std::vector<double> rates ((size_t) n, 0.0);
    R.frames.assign ((size_t) n, 0);
    for (int i = 0; i < n; ++i)
    {
        if (cancelFlag.load()) { R.cancelled = true; return R; }
        setStatus ("Converting " + juce::String (i + 1) + " of " + juce::String (n) + "...");
        const juce::File src (br.files[(size_t) i]);
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (src));
        if (r == nullptr) { R.error = "Cannot read " + src.getFileName(); return R; }
        const double native = r->sampleRate, T = masterTargetRate (spec.out, native);
        rates[(size_t) i] = T;
        juce::AudioBuffer<float> buf (2, 65536);
        if (std::abs (T - native) < 0.5)
        {
            for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += 65536)
            {
                const int m = (int) juce::jmin ((juce::int64) 65536, r->lengthInSamples - pos);
                r->read (&buf, 0, m, pos, true, true);
                for (int c = 0; c < 2; ++c) { const auto mm = buf.findMinMax (c, 0, m); peaks[(size_t) i] = juce::jmax (peaks[(size_t) i], std::abs (mm.getStart()), std::abs (mm.getEnd())); }
            }
            proc.push_back (src); R.frames[(size_t) i] = r->lengthInSamples;
        }
        else
        {
            const auto outFile = spec.workFolder.getChildFile ("rs-" + juce::String (i + 1).paddedLeft ('0', 2) + ".wav");
            auto w = audioops::makeWavWriter (outFile, T, 2, true);
            if (w == nullptr) { R.error = "Cannot write " + outFile.getFullPathName(); return R; }
            SincResampler rl (native, T), rr (native, T);
            std::vector<float> ol, orr; juce::int64 written = 0;
            auto flush = [&] ()
            {
                const size_t m = juce::jmin (ol.size(), orr.size());
                if (m == 0) return;
                for (auto* v : { &ol, &orr }) for (size_t k = 0; k < m; ++k) peaks[(size_t) i] = juce::jmax (peaks[(size_t) i], std::abs ((*v)[k]));
                const float* ptrs[2] = { ol.data(), orr.data() };
                w->writeFromFloatArrays (ptrs, 2, (int) m);
                written += (juce::int64) m;
                ol.erase (ol.begin(), ol.begin() + (long) m); orr.erase (orr.begin(), orr.begin() + (long) m);
            };
            for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += 65536)
            {
                if (cancelFlag.load()) { R.cancelled = true; return R; }
                const int m = (int) juce::jmin ((juce::int64) 65536, r->lengthInSamples - pos);
                r->read (&buf, 0, m, pos, true, true);
                rl.process (buf.getReadPointer (0), (size_t) m, ol); rr.process (buf.getReadPointer (1), (size_t) m, orr);
                flush();
                sub = (float) (((double) i + (double) (pos + m) / (double) r->lengthInSamples) / n);
            }
            rl.finish (ol); rr.finish (orr); flush();
            w.reset();
            proc.push_back (outFile); R.frames[(size_t) i] = written;
        }
        sub = (float) (i + 1) / (float) n;
    }

    // ---- 3. levels, dither, encoding, tags
    stage = 2; sub = 0.0f;
    const auto& o = spec.out;
    float loudest = 0.0f; for (auto pk : peaks) loudest = juce::jmax (loudest, pk);
    std::vector<juce::File> finals;
    float overall = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (cancelFlag.load()) { R.cancelled = true; for (auto& f : finals) if (spec.kind == MasterJobSpec::Kind::Files) f.deleteFile(); return R; }
        float gain = 1.0f;
        if (o.normalise)
        {
            const float ref = o.peakTogether ? loudest : peaks[(size_t) i];
            if (ref > 1.0e-9f) gain = juce::Decibels::decibelsToGain (o.peakDb) / ref;
        }
        const auto& item = spec.items[(size_t) i];
        std::vector<MasterFormat> fmts;
        if (spec.kind == MasterJobSpec::Kind::Files)
        {
            fmts.push_back (o.format);
            for (int f = 0; f < 5; ++f) if ((o.extraFormats & (1 << f)) != 0 && (MasterFormat) f != o.format) fmts.push_back ((MasterFormat) f);
        }
        else fmts.push_back (o.format);
        float pk = 0.0f;
        for (auto fmtNow : fmts)
        {
            const auto ext = masterFileExtension (fmtNow);
            const juce::File dest = spec.kind == MasterJobSpec::Kind::Files ? spec.destFolder.getNonexistentChildFile (sanitiseForFile (item.baseName), ext, true)
                                                                             : spec.workFolder.getChildFile (item.baseName + ".wav");
            setStatus ("Writing " + juce::String (i + 1) + " of " + juce::String (n) + ": " + dest.getFileName());
            float pkOne = 0.0f;
            const auto e = finalise (proc[(size_t) i], dest, rates[(size_t) i], gain, item, pkOne, fmtNow);
            if (e == "Cancelled.") { R.cancelled = true; for (auto& f : finals) if (spec.kind == MasterJobSpec::Kind::Files) f.deleteFile(); return R; }
            if (e.isNotEmpty()) { R.error = e; for (auto& f : finals) if (spec.kind == MasterJobSpec::Kind::Files) f.deleteFile(); return R; }
            pk = juce::jmax (pk, pkOne);
            finals.push_back (dest);
            if (spec.kind == MasterJobSpec::Kind::Files) R.files.add (dest.getFullPathName());
        }
        overall = juce::jmax (overall, pk);
        sub = (float) (i + 1) / (float) n * (spec.kind == MasterJobSpec::Kind::Disc ? 0.3f : 1.0f);
    }
    R.peakDb = juce::Decibels::gainToDecibels (overall, -100.0f);
    R.clipped = overall > 1.0f;

    // ---- 4. the disc: layout from the real lengths, then DDP / zip / WAV + CUE
    if (spec.kind == MasterJobSpec::Kind::Disc)
    {
        setStatus ("Building the DDP...");
        std::vector<juce::int64> frames (spec.disc.clips.size(), 0);
        for (int i = 0; i < n; ++i) frames[(size_t) spec.clipIndex[(size_t) i]] = R.frames[(size_t) i];
        const auto pq = computePq (spec.disc, frames);
        std::vector<juce::File> files;
        for (auto& t : pq.tracks)
            for (int i = 0; i < n; ++i) if (spec.clipIndex[(size_t) i] == t.clipIndex) files.push_back (finals[(size_t) i]);
        const auto name = sanitiseForFile (spec.discName);
        const auto ddpFolder = spec.destFolder.getChildFile (name + " DDP");
        const auto zip = spec.disc.makeZip ? spec.destFolder.getChildFile (name + " DDP.zip") : juce::File();
        std::atomic<float> inner { 0.0f };
        stage = 2; sub = 0.3f;
        auto e = ddp::writeFileset (ddpFolder, spec.disc, pq, files, zip, &inner, &cancelFlag);
        if (e == "Cancelled.") { R.cancelled = true; return R; }
        if (e.isNotEmpty()) { R.error = e; return R; }
        R.files.add (ddpFolder.getFullPathName()); if (zip != juce::File()) R.files.add (zip.getFullPathName());
        sub = 0.8f;
        juce::String rep; ddp::verifyFileset (ddpFolder, rep); R.report = rep;
        if (spec.disc.makeCue)
        {
            setStatus ("Writing the WAV and CUE...");
            const auto wav = spec.destFolder.getChildFile (name + ".wav"), cue = spec.destFolder.getChildFile (name + ".cue");
            e = ddp::writeWavAndCue (wav, cue, spec.disc, pq, files, &inner, &cancelFlag);
            if (e == "Cancelled.") { R.cancelled = true; return R; }
            if (e.isNotEmpty()) { R.error = e; return R; }
            R.files.add (wav.getFullPathName()); R.files.add (cue.getFullPathName());
        }
        sub = 1.0f;
    }
    return R;
}
} // namespace td

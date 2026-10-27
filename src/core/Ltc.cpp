#include "Ltc.h"
#include <algorithm>
#include <cmath>

namespace td
{
LtcDecoder::LtcDecoder (double sampleRate) : rate (sampleRate)
{
    tBit = rate / (25.0 * 80.0);
    minBit = rate / (30.5 * 80.0) * 0.85; maxBit = rate / (23.9 * 80.0) * 1.15;
    for (int i = 0; i < 80; ++i) { ring[i] = 0; ringPos[i] = 0.0; }
}

void LtcDecoder::feed (const float* x, int n)
{
    for (int i = 0; i < n; ++i, index += 1.0)
    {
        const float v = x[i];
        env = std::max ((double) std::fabs (v), env * 0.9995);
        const float th = (float) std::max (0.15 * env, 0.0015);
        // the latest real zero crossing (interpolated between two samples)
        if ((prev <= 0.0f && v > 0.0f) || (prev >= 0.0f && v < 0.0f))
        {
            const float d = v - prev;
            pendingZc = index - 1.0 + (d != 0.0f ? (double) (-prev / d) : 0.5);
        }
        // an edge counts once the signal has really gone to the other side
        if (polarity <= 0 && v > th)       { if (polarity != 0 && pendingZc >= 0.0) edge (pendingZc); polarity = 1; pendingZc = -1.0; }
        else if (polarity >= 0 && v < -th) { if (polarity != 0 && pendingZc >= 0.0) edge (pendingZc); polarity = -1; pendingZc = -1.0; }
        prev = v;
    }
}

void LtcDecoder::edge (double pos)
{
    if (e1 < 0.0) { e1 = pos; return; }
    const double d = pos - e1;
    if (d < tBit * 0.25 || d > tBit * 2.5) { e1 = pos; shortPending = false; return; }     // not a code: start again
    if (d > tBit * 0.75)                                                                    // a long interval: a 0
    {
        if (shortPending) shortPending = false;                                            // a lone short one before it: broken, drop it
        tBit = std::min (maxBit, std::max (minBit, tBit + 0.05 * (d - tBit)));
        bit (0, e1);
    }
    else                                                                                    // a short interval: two of them are a 1
    {
        if (! shortPending) { shortPending = true; shortStart = e1; }
        else
        {
            shortPending = false;
            tBit = std::min (maxBit, std::max (minBit, tBit + 0.05 * ((pos - shortStart) - tBit)));
            bit (1, shortStart);
        }
    }
    e1 = pos;
}

void LtcDecoder::bit (int value, double startPos)
{
    for (int i = 0; i < 79; ++i) { ring[i] = ring[i + 1]; ringPos[i] = ringPos[i + 1]; }
    ring[79] = value; ringPos[79] = startPos;
    if (ringCount < 80) ++ringCount;
    ++sinceSync;
    if (ringCount < 80 || sinceSync < 80) return;
    static const int sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
    for (int i = 0; i < 16; ++i) if (ring[64 + i] != sync[i]) return;
    tryFrame();
}

void LtcDecoder::tryFrame()
{
    auto field = [&] (int first, int bits) { int v = 0; for (int i = 0; i < bits; ++i) v |= ring[first + i] << i; return v; };
    Frame f;
    const int fu = field (0, 4), ft = field (8, 2), su = field (16, 4), st = field (24, 3), mu = field (32, 4), mt = field (40, 3), hu = field (48, 4), ht = field (56, 2);
    f.drop = ring[10] != 0;
    f.f = ft * 10 + fu; f.s = st * 10 + su; f.m = mt * 10 + mu; f.h = ht * 10 + hu;
    if (fu > 9 || su > 9 || mu > 9 || hu > 9 || f.f > 29 || f.s > 59 || f.m > 59 || f.h > 23) { sinceSync = 0; return; }
    f.pos = ringPos[0];
    if (store) found.push_back (f);
    last = f; ++total;
    sinceSync = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
static long long frameNumber (const LtcDecoder::Frame& f, int nominal, bool drop)
{
    long long n = (((long long) f.h * 60 + f.m) * 60 + f.s) * nominal + f.f;
    if (drop) { const long long totalMinutes = (long long) f.h * 60 + f.m; n -= 2 * (totalMinutes - totalMinutes / 10); }
    return n;
}

LtcResult analyseLtc (const std::vector<LtcDecoder::Frame>& fr, double rate)
{
    LtcResult r;
    if (fr.size() < 8 || rate <= 0.0) return r;
    // the frame length in samples: the middle of the steps between neighbouring frames that are exactly one frame apart
    std::vector<double> steps;
    for (size_t i = 1; i < fr.size(); ++i)
    {
        const auto &a = fr[i - 1], &b = fr[i];
        const bool next = (b.f == a.f + 1 && b.s == a.s && b.m == a.m && b.h == a.h) || (b.f == 0 && a.f >= 23 && (b.s != a.s || b.m != a.m || b.h != a.h));
        if (next) steps.push_back (b.pos - a.pos);
    }
    if (steps.size() < 6) return r;
    std::sort (steps.begin(), steps.end());
    const double frameLen = steps[steps.size() / 2];
    const double measured = rate / frameLen;
    int maxF = 0; bool anyDrop = false;
    for (auto& f : fr) { maxF = std::max (maxF, f.f); anyDrop = anyDrop || f.drop; }
    double fps; int nominal;
    if (anyDrop) { fps = 30000.0 / 1001.0; nominal = 30; }
    else
    {
        const double cands[] = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0 };
        fps = cands[0]; for (double c : cands) if (std::fabs (c - measured) < std::fabs (fps - measured)) fps = c;
        // frame numbers that were seen settle the doubtful cases (24 / 25 / 30 per second)
        if (maxF >= 25) fps = std::fabs (30000.0 / 1001.0 - measured) < std::fabs (30.0 - measured) ? 30000.0 / 1001.0 : 30.0;
        else if (maxF == 24) fps = 25.0;
        else if (maxF >= 20 && std::fabs (fps - 25.0) < 0.2) fps = std::fabs (24000.0 / 1001.0 - measured) < std::fabs (24.0 - measured) ? 24000.0 / 1001.0 : 24.0;
        nominal = (int) std::lround (fps);
    }
    r.fps = fps; r.drop = anyDrop; r.frameCount = (int) fr.size();
    // start of the file: every frame says "timecode T at sample P", so T - P / rate should be the same for all of them
    std::vector<double> starts; starts.reserve (fr.size());
    long long prevN = -1; double prevPos = 0.0;
    for (auto& f : fr)
    {
        const long long n = frameNumber (f, nominal, anyDrop);
        starts.push_back ((double) n / fps - f.pos / rate);
        if (prevN >= 0)
        {
            const long long expected = (long long) std::llround ((f.pos - prevPos) / frameLen);
            if (n - prevN != expected) ++r.jumps;
        }
        prevN = n; prevPos = f.pos;
    }
    std::sort (starts.begin(), starts.end());
    r.startSeconds = starts[starts.size() / 2];
    r.coveredSeconds = (fr.back().pos - fr.front().pos) / rate;
    r.found = true;
    r.firstText = timecodeText (r.startSeconds, fps, anyDrop);
    return r;
}

LtcResult scanFileForLtc (juce::AudioFormatManager& fm, const juce::File& file, int channel, double maxSeconds, const std::atomic<bool>* cancel)
{
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
    if (rd == nullptr || rd->numChannels < 1) return {};
    const int ch = juce::jlimit (0, (int) rd->numChannels - 1, channel);
    LtcDecoder dec (rd->sampleRate);
    juce::AudioBuffer<float> buf ((int) rd->numChannels, 1 << 16);
    juce::int64 total = rd->lengthInSamples;
    if (maxSeconds > 0.0) total = std::min (total, (juce::int64) (maxSeconds * rd->sampleRate));
    for (juce::int64 pos = 0; pos < total; pos += buf.getNumSamples())
    {
        if (cancel != nullptr && cancel->load()) return {};
        const int n = (int) std::min ((juce::int64) buf.getNumSamples(), total - pos);
        buf.clear();
        rd->read (&buf, 0, n, pos, true, true);
        dec.feed (buf.getReadPointer (ch), n);
    }
    return analyseLtc (dec.frames(), rd->sampleRate);
}

static void fieldsOfFrame (long long n, int nominal, bool drop, int& h, int& m, int& s, int& f)
{
    if (n < 0) n = 0;
    if (drop)   // the frame number -> drop-frame numbers (two frame numbers are skipped every minute except every tenth minute)
    {
        const long long perMin = 60LL * 30 - 2, per10 = 10 * perMin + 2;
        const long long d = n / per10, mm = n % per10;
        n += 18 * d + (mm > 1 ? 2 * ((mm - 2) / perMin) : 0);
    }
    f = (int) (n % nominal); n /= nominal;
    s = (int) (n % 60); n /= 60;
    m = (int) (n % 60); n /= 60;
    h = (int) (n % 24);
}

juce::String fpsName (double fps, bool drop)
{
    if (drop) return "29.97 DF";
    if (std::fabs (fps - 24000.0 / 1001.0) < 0.01) return "23.976";
    if (std::fabs (fps - 30000.0 / 1001.0) < 0.01) return "29.97";
    return juce::String ((int) std::lround (fps));
}

juce::String timecodeText (double seconds, double fps, bool drop)
{
    const int nominal = (int) std::lround (fps);
    long long n = (long long) std::floor (seconds * fps + 1.0e-6);
    if (n < 0) n = 0;
    if (drop)   // the frame number -> drop-frame numbers (two frame numbers are skipped every minute except every tenth minute)
    {
        const long long perMin = 60LL * 30 - 2, per10 = 10 * perMin + 2;
        const long long d = n / per10, m = n % per10;
        n += 18 * d + (m > 1 ? 2 * ((m - 2) / perMin) : 0);
    }
    const int f = (int) (n % nominal); n /= nominal;
    const int s = (int) (n % 60); n /= 60;
    const int m = (int) (n % 60); n /= 60;
    const int h = (int) (n % 24);
    auto two = [] (int v) { return juce::String (v).paddedLeft ('0', 2); };
    return two (h) + ":" + two (m) + ":" + two (s) + (drop ? ";" : ":") + two (f);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
void LtcLive::prepare (double sampleRate)
{
    rate = sampleRate;
    dec = std::make_unique<LtcDecoder> (sampleRate);
    dec->setStoreFrames (false);
    seen = 0; prevPos = -1.0; prevN = -1; lenEma = 0.0; maxF = maxFOld = 0; sinceWindow = 0; dropSeen = false; sinceFrame = 1 << 30; idx = 0;
    present.store (false);
}

void LtcLive::feedNothing (int count) noexcept
{
    idx += count; sinceFrame += count;
    if (sinceFrame > (long long) (rate * 0.5)) present.store (false, std::memory_order_relaxed);
}

void LtcLive::feed (const float* x, int count) noexcept
{
    if (dec == nullptr) return;
    const long long before = idx;
    dec->feed (x, count);
    idx += count;
    sinceWindow += count;
    if (sinceWindow > (long long) (rate * 4.0)) { maxFOld = maxF; maxF = 0; sinceWindow = 0; }
    if (dec->frameTotal() != seen)
    {
        seen = dec->frameTotal();
        const auto& f = dec->lastFrame();
        maxF = std::max (maxF, f.f);
        if (f.drop) { dropSeen = true; dropLast = idx; } else if (idx - dropLast > (long long) (rate * 3.0)) dropSeen = false;
        const int n = f.f;                                                          // (only the frame counter is needed for the step)
        if (prevPos >= 0.0 && ((n == prevN + 1) || (n == 0 && prevN >= 23)))
        {
            const double len = f.pos - prevPos;
            if (len > rate / 31.0 && len < rate / 23.0) lenEma = lenEma <= 0.0 ? len : lenEma + 0.1 * (len - lenEma);
        }
        prevPos = f.pos; prevN = n;
        sinceFrame = 0;
        // frame rate
        const double measured = lenEma > 0.0 ? rate / lenEma : 25.0;
        const int top = std::max (maxF, maxFOld);
        double fps; bool dr = dropSeen;
        if (dr) fps = 30000.0 / 1001.0;
        else
        {
            const double cands[] = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0 };
            fps = cands[0]; for (double c : cands) if (std::fabs (c - measured) < std::fabs (fps - measured)) fps = c;
            if (top >= 25) fps = std::fabs (30000.0 / 1001.0 - measured) < std::fabs (30.0 - measured) ? 30000.0 / 1001.0 : 30.0;
            else if (top == 24) fps = 25.0;
            else if (top >= 20 && std::fabs (fps - 25.0) < 0.2) fps = std::fabs (24000.0 / 1001.0 - measured) < std::fabs (24.0 - measured) ? 24000.0 / 1001.0 : 24.0;
        }
        const int nominal = (int) std::lround (fps);
        long long fn = (((long long) f.h * 60 + f.m) * 60 + f.s) * nominal + f.f;
        if (dr) { const long long tm = (long long) f.h * 60 + f.m; fn -= 2 * (tm - tm / 10); }
        fpsA.store (fps, std::memory_order_relaxed); dropA.store (dr, std::memory_order_relaxed); tcA.store ((double) fn / fps, std::memory_order_relaxed);
        present.store (true, std::memory_order_relaxed);
    }
    else
    {
        sinceFrame += count;
        if (sinceFrame > (long long) (rate * 0.5)) present.store (false, std::memory_order_relaxed);
    }
    (void) before;
}

LtcLive::Status LtcLive::status() const noexcept
{
    Status s; s.present = present.load (std::memory_order_relaxed); s.fps = fpsA.load (std::memory_order_relaxed);
    s.drop = dropA.load (std::memory_order_relaxed); s.tcSeconds = tcA.load (std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
void LtcGenerator::formatInfo (int code, double& fps, int& nominal, bool& drop)
{
    drop = false;
    switch (code)
    {
        case 0: fps = 24.0; nominal = 24; break;
        case 2: fps = 30000.0 / 1001.0; nominal = 30; drop = true; break;
        case 3: fps = 30000.0 / 1001.0; nominal = 30; break;
        case 4: fps = 30.0; nominal = 30; break;
        case 5: fps = 24000.0 / 1001.0; nominal = 24; break;
        default: fps = 25.0; nominal = 25; break;
    }
}

juce::String LtcGenerator::formatName (int code)
{
    double f; int n; bool d; formatInfo (code, f, n, d);
    return fpsName (f, d) + (d ? "" : " fps");
}

void LtcGenerator::prepare (double sampleRate) { rate = sampleRate; running = false; }

void LtcGenerator::start (int code, double startSeconds) noexcept
{
    formatInfo (code, fps, nominal, drop);
    bitLen = rate / (fps * 80.0);
    dayFrames = drop ? 2589408LL : (long long) nominal * 86400LL;
    frameNo = (long long) std::llround (std::fmod (std::max (0.0, startSeconds), 86400.0) * fps) % dayFrames;
    bitIdx = 0; phase = 0.0; halfDone = false; level = 1.0f; smooth = 0.0;
    buildFrame();
    running = true;
}

void LtcGenerator::startExact (int code, double seconds) noexcept
{
    formatInfo (code, fps, nominal, drop);
    bitLen = rate / (fps * 80.0);
    dayFrames = drop ? 2589408LL : (long long) nominal * 86400LL;
    const double frames = std::fmod (std::max (0.0, seconds), 86400.0) * fps;
    const double whole = std::floor (frames);
    frameNo = (long long) whole % dayFrames;
    const double bitPos = (frames - whole) * 80.0;                 // how far into the frame, in bits
    bitIdx = juce::jlimit (0, 79, (int) std::floor (bitPos));
    phase = (bitPos - std::floor (bitPos)) * bitLen;
    buildFrame();
    // the level a generator running since the frame's start would have now: it changes at every bit's start, and in the middle of every 1 bit
    float l = 1.0f;
    for (int i = 0; i < bitIdx; ++i) { if (bits[i] != 0) l = -l; l = -l; }
    halfDone = bits[bitIdx] != 0 && phase >= bitLen * 0.5;
    if (halfDone) l = -l;
    level = l;
    smooth = (double) (level * 0.35f);                              // (settled: no rise at the very first sample)
    running = true;
}

void LtcGenerator::buildFrame() noexcept
{
    int h, m, s, f; fieldsOfFrame (frameNo, nominal, drop, h, m, s, f);
    for (int i = 0; i < 80; ++i) bits[i] = 0;
    auto put = [&] (int first, int count, int v) { for (int i = 0; i < count; ++i) bits[first + i] = (v >> i) & 1; };
    put (0, 4, f % 10); put (8, 2, f / 10); bits[10] = drop ? 1 : 0;
    put (16, 4, s % 10); put (24, 3, s / 10); put (32, 4, m % 10); put (40, 3, m / 10); put (48, 4, h % 10); put (56, 2, h / 10);
    static const int sync[16] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
    for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];
    // the polarity bit (27 for 24 / 30 fps, 59 for 25 fps) makes the number of zeros even, so every frame starts with the same polarity
    const int pb = nominal == 25 ? 59 : 27;
    int zeros = 0; for (int i = 0; i < 80; ++i) if (i != pb && bits[i] == 0) ++zeros;
    bits[pb] = (zeros % 2 == 0) ? 1 : 0;
}

void LtcGenerator::fill (float* out, int count) noexcept
{
    const float amp = 0.35f;                                   // about -9 dBFS
    for (int i = 0; i < count; ++i)
    {
        if (phase >= bitLen)
        {
            phase -= bitLen; halfDone = false;
            if (++bitIdx >= 80) { bitIdx = 0; frameNo = (frameNo + 1) % dayFrames; buildFrame(); }
            level = -level;                                    // every bit starts with a change
        }
        else if (! halfDone && bits[bitIdx] != 0 && phase >= bitLen * 0.5) { level = -level; halfDone = true; }
        smooth += 0.55 * ((double) (level * amp) - smooth);    // a rise time of about two samples, like a real LTC generator
        out[i] = (float) smooth;
        phase += 1.0;
    }
}
} // namespace td

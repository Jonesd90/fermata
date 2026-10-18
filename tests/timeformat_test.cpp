// Headless test of the time readout's writing and reading of positions (src/core/TimeFormat.h).
#include "../src/core/TimeFormat.h"
#include <cstdio>

using namespace td;
using namespace td::timefmt;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define SECTION(name) std::printf ("-- %s\n", name)

static Format fmt (Mode m, int code = 1, double offset = 0.0, double rate = 48000.0)
{
    Format f; f.mode = m; f.ltcCode = code; f.offsetSeconds = offset; f.sampleRate = rate; return f;
}
static juce::String dimmed (const Text& t) { return t.text.substring (t.dimFrom, t.dimTo); }

int main()
{
    SECTION ("Time: hours : minutes : seconds . ms");
    {
        auto f = fmt (Mode::time);
        CHECK (format (f, 0.0).text == "00:00:00.000");
        CHECK (format (f, 83.456).text == "00:01:23.456");
        CHECK (format (f, 3723.5).text == "01:02:03.500");
        CHECK (format (f, 0.9999).text == "00:00:00.999");                  // cut, not rounded: the second has not begun yet
        CHECK (format (f, 360000.0).text == "100:00:00.000");               // more than 99 hours still fits
        CHECK (format (f, -2.5).text == "-00:00:02.500");
        CHECK (format (f, -0.0001).text == "00:00:00.000");                  // nothing to show: no minus sign
        CHECK (format (f, std::nan ("")).text == "00:00:00.000");

        // faint zeros: only leading ones, never the last digit of the seconds
        CHECK (dimmed (format (f, 0.0)) == "00:00:0");
        CHECK (dimmed (format (f, 7.0)) == "00:00:0");
        CHECK (dimmed (format (f, 83.0)) == "00:0");
        CHECK (dimmed (format (f, 3723.0)) == "0");                          // 01:02:03: only the zero in front of the 1 is faint
        CHECK (dimmed (format (f, 36000.0)) == "");                          // 10:00:00: nothing to dim
        CHECK (format (f, -2.5).dimFrom == 1);                               // the minus sign stays bright
        CHECK (dimmed (format (f, -2.5)) == "00:00:0");
    }

    SECTION ("Min:Sec");
    {
        auto f = fmt (Mode::minSec);
        CHECK (format (f, 0.0).text == "0:00.000");
        CHECK (format (f, 83.456).text == "1:23.456");
        CHECK (format (f, 754.0).text == "12:34.000");
        CHECK (format (f, 3723.5).text == "62:03.500");                      // minutes keep counting
        CHECK (format (f, -83.456).text == "-1:23.456");
    }

    SECTION ("Samples");
    {
        auto f = fmt (Mode::samples, 1, 0.0, 96000.0);
        CHECK (format (f, 0.0).text == "0");
        CHECK (format (f, 1.0).text == "96 000");
        CHECK (format (f, 38.9).text == "3 734 400");
        CHECK (format (f, -1.0).text == "-96 000");
        CHECK (format (fmt (Mode::samples, 1, 0.0, 44100.0), 10.0).text == "441 000");
    }

    SECTION ("Timecode");
    {
        auto f25 = fmt (Mode::timecode, 1);
        CHECK (format (f25, 0.0).text == "00:00:00:00");
        CHECK (format (f25, 1.0).text == "00:00:01:00");
        CHECK (format (f25, 1.52).text == "00:00:01:13");
        CHECK (format (f25, 3723.0).text == "01:02:03:00");
        CHECK (dimmed (format (f25, 0.0)) == "00:00:0");
        CHECK (dimmed (format (f25, 83.0)) == "00:0");

        auto f24 = fmt (Mode::timecode, 0);
        CHECK (format (f24, 1.5).text == "00:00:01:12");

        auto f30 = fmt (Mode::timecode, 4);
        CHECK (format (f30, 2.0).text == "00:00:02:00");

        // a timeline that starts at 01:00:00:00
        auto fo = fmt (Mode::timecode, 1, 3600.0);
        CHECK (format (fo, 0.0).text == "01:00:00:00");
        CHECK (format (fo, 10.0).text == "01:00:10:00");
        CHECK (dimmed (format (fo, 10.0)) == "0");                           // 01:00:10:00: the zero in front of the hour

        // drop-frame: ";" before the frames, and frame numbers 00 and 01 are skipped at each new minute except every tenth
        auto fd = fmt (Mode::timecode, 2);
        CHECK (format (fd, 0.0).text == "00:00:00;00");
        const double fps = 30000.0 / 1001.0;
        CHECK (format (fd, 1799.0 / fps + 1e-4).text == "00:00:59;29");
        CHECK (format (fd, 1800.0 / fps + 1e-4).text == "00:01:00;02");
        CHECK (format (fd, 17981.0 / fps + 1e-4).text == "00:09:59;29");
        CHECK (format (fd, 17982.0 / fps + 1e-4).text == "00:10:00;00");     // every tenth minute keeps its frames
    }

    SECTION ("Parsing typed times");
    {
        auto f = fmt (Mode::time);
        auto p = [&] (const Format& ff, const char* s) { auto r = parse (ff, s); return r ? *r : -9999.0; };
        CHECK (p (f, "90") == 90.0);
        CHECK (std::fabs (p (f, "90.5") - 90.5) < 1e-9);
        CHECK (std::fabs (p (f, "90,5") - 90.5) < 1e-9);                     // a comma works as a decimal point
        CHECK (p (f, "90s") == 90.0);
        CHECK (p (f, "1:30") == 90.0);
        CHECK (p (f, "1:02:03") == 3723.0);
        CHECK (std::fabs (p (f, "01:02:03.250") - 3723.25) < 1e-9);
        CHECK (p (f, "2m10s") == 130.0);
        CHECK (p (f, "1h2m3s") == 3723.0);
        CHECK (std::fabs (p (f, "1h 2m 3.5s") - 3723.5) < 1e-9);
        CHECK (p (f, "-5") == -5.0);
        CHECK (p (f, "  12:34  ") == 754.0);
        CHECK (p (f, "") == -9999.0);
        CHECK (p (f, "abc") == -9999.0);
        CHECK (p (f, "1:2:3:4") == -9999.0);                                 // four parts mean frames, which only Timecode reads
        CHECK (p (f, "1:x") == -9999.0);
        CHECK (p (f, "12m3") == -9999.0);                                    // a number with no letter after it
        CHECK (p (f, "1..5") == -9999.0);

        // what is written can be read back, in every mode
        for (auto m : { Mode::time, Mode::minSec, Mode::timecode, Mode::samples })
            for (double s : { 0.0, 0.5, 12.0, 83.456, 754.0, 3723.0, 7325.04 })
            {
                auto ff = fmt (m, 1, m == Mode::timecode ? 3600.0 : 0.0, 48000.0);
                auto back = parse (ff, format (ff, s).text);
                CHECK (back.has_value());
                const double tol = m == Mode::timecode ? 1.0 / 25.0 : 0.0011;
                if (back) CHECK (std::fabs (*back - s) <= tol);
            }
    }

    SECTION ("Parsing in Timecode and Samples modes");
    {
        auto f = fmt (Mode::timecode, 1);
        auto p = [&] (const Format& ff, const char* s) { auto r = parse (ff, s); return r ? *r : -9999.0; };
        CHECK (std::fabs (p (f, "00:00:01:13") - 1.52) < 1e-9);
        CHECK (p (f, "00:00:01:25") == -9999.0);                             // frame 25 does not exist at 25 fps
        CHECK (p (f, "00:60:00:00") == -9999.0);
        CHECK (p (f, "1:30") == 90.0);                                       // a short time is still minutes : seconds
        auto fo = fmt (Mode::timecode, 1, 3600.0);
        CHECK (std::fabs (p (fo, "01:00:10:00") - 10.0) < 1e-9);             // the start timecode is taken off

        auto fd = fmt (Mode::timecode, 2);
        const double fps = 30000.0 / 1001.0;
        CHECK (std::fabs (p (fd, "00:01:00;02") - 1800.0 / fps) < 1e-9);
        CHECK (std::fabs (p (fd, "00:10:00;00") - 17982.0 / fps) < 1e-9);
        for (double s : { 0.0, 10.0, 59.9, 60.2, 600.0, 3600.7 })
        {
            auto back = parse (fd, format (fd, s).text);
            CHECK (back.has_value() && *back <= s + 1e-6 && s - *back < 1.0 / fps + 1e-6);
        }

        auto fs = fmt (Mode::samples, 1, 0.0, 96000.0);
        CHECK (p (fs, "96000") == 1.0);
        CHECK (p (fs, "96 000") == 1.0);
        CHECK (p (fs, "1,234,567") == 1234567.0 / 96000.0);
        CHECK (p (fs, "48000 smp") == 0.5);
        CHECK (p (fs, "1:30") == 90.0);                                      // a time with a colon is still a time
        CHECK (p (fs, "2s") == 2.0);
    }

    SECTION ("Widest texts");
    {
        for (auto m : { Mode::time, Mode::minSec, Mode::timecode, Mode::samples })
        {
            auto f = fmt (m);
            CHECK (widestText (f).isNotEmpty());
            CHECK (format (f, 0.0).text.length() <= widestText (f).length());
        }
    }

    std::printf (failures == 0 ? "\nTIME FORMAT TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

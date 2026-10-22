#pragma once
#include "Common.h"

namespace td
{
/** The Mackie Control (MCU) protocol, as spoken by control surfaces such as the Avid S1 (through EuControl's "Mackie Control" emulation), the
    Mackie MCU itself and the many surfaces that copy it. Pure functions: what the surface sends is decoded into Events, and what Fermata wants
    shown on it (fader positions, the rings round the dials, the button lamps, the two lines of writing) is encoded into MIDI messages.
    Nothing here touches the project: see ui/Surface.h for what the events DO.

    The protocol in short (MIDI channel 1 unless said otherwise):
      faders       pitch bend, channel = strip + 1 (strip 0..7; 9 = master): 14 bits, 0 = bottom .. 16383 = top; sent both ways
      fader touch  note 0x68 + strip, velocity 0x7F = touched, 0 = let go
      dials        CC 0x10 + strip: value 0x01..0x3F = turned clockwise that many steps, 0x41..0x7F = anticlockwise
      dial rings   CC 0x30 + strip: Fermata -> surface, which lamps of the ring round the dial are lit (mode and position)
      buttons      note 0x00 .. 0x76, velocity 0x7F = pressed, 0 = released; the lamp in a button is lit by sending the same note back
      writing      SysEx F0 00 00 66 14 12 <position> <text> F7: 2 lines of 56 characters (position 0..55 the top line, 56..111 the bottom)
      7-segment    CC 0x4A / 0x4B: the two digits of the ASSIGNMENT display
      meters       channel pressure: (strip << 4) | level 0..12
      handshake    SysEx 00 00 66 14 00 (device query), 01 (the surface's serial and challenge), 02 (the host's reply), 03 (connected) */
namespace mcu
{
constexpr int kStrips = 8;
constexpr int kLcdWidth = 56, kLcdCell = 7;          // the writing: 56 characters a line, 7 per strip (6 shown, 1 gap)
constexpr int kFaderMax = 16383;

// ---- the buttons, by note number
enum Button : int
{
    RecBase = 0x00, SoloBase = 0x08, MuteBase = 0x10, SelectBase = 0x18, PushBase = 0x20,    // + strip
    Track = 0x28, Send = 0x29, Pan = 0x2A, PlugIn = 0x2B, Eq = 0x2C, Instrument = 0x2D,
    BankLeft = 0x2E, BankRight = 0x2F, ChannelLeft = 0x30, ChannelRight = 0x31, Flip = 0x32, Global = 0x33,
    NameValue = 0x34, SmpteBeats = 0x35,
    F1 = 0x36, F2, F3, F4, F5, F6, F7, F8,
    MidiTracks = 0x3E, Inputs = 0x3F, AudioTracks = 0x40, AudioInstr = 0x41, Aux = 0x42, Busses = 0x43, Outputs = 0x44, User = 0x45,
    Shift = 0x46, Option = 0x47, Control = 0x48, Alt = 0x49,
    Read = 0x4A, Write = 0x4B, Trim = 0x4C, Touch = 0x4D, Latch = 0x4E, Group = 0x4F,
    Save = 0x50, Undo = 0x51, Cancel = 0x52, Enter = 0x53, Marker = 0x54, Nudge = 0x55, Cycle = 0x56, Drop = 0x57, Replace = 0x58, Click = 0x59, SoloGlobal = 0x5A,
    Rewind = 0x5B, FastFwd = 0x5C, Stop = 0x5D, Play = 0x5E, Record = 0x5F,
    Up = 0x60, Down = 0x61, Left = 0x62, Right = 0x63, Zoom = 0x64, Scrub = 0x65,
    UserA = 0x66, UserB = 0x67, TouchBase = 0x68, MasterTouch = 0x70,
    SmpteLed = 0x71, BeatsLed = 0x72, RudeSolo = 0x73, Relay = 0x76,
    NumButtons = 0x78
};

/** "Mute 3", "F1", "Bank <", "Play" ...: the button a note number means. */
inline juce::String buttonName (int note)
{
    if (note >= RecBase    && note < RecBase + kStrips)    return "Rec "    + juce::String (note - RecBase + 1);
    if (note >= SoloBase   && note < SoloBase + kStrips)   return "Solo "   + juce::String (note - SoloBase + 1);
    if (note >= MuteBase   && note < MuteBase + kStrips)   return "Mute "   + juce::String (note - MuteBase + 1);
    if (note >= SelectBase && note < SelectBase + kStrips) return "Select " + juce::String (note - SelectBase + 1);
    if (note >= PushBase   && note < PushBase + kStrips)   return "Dial push " + juce::String (note - PushBase + 1);
    if (note >= TouchBase  && note < TouchBase + kStrips)  return "Fader touch " + juce::String (note - TouchBase + 1);
    if (note >= F1 && note <= F8) return "F" + juce::String (note - F1 + 1);
    switch (note)
    {
        case Track: return "Track"; case Send: return "Send"; case Pan: return "Pan"; case PlugIn: return "Plug-in / Inserts"; case Eq: return "EQ"; case Instrument: return "Instrument";
        case BankLeft: return "Bank <"; case BankRight: return "Bank >"; case ChannelLeft: return "Channel <"; case ChannelRight: return "Channel >";
        case Flip: return "Flip"; case Global: return "Global view"; case NameValue: return "Name / Value"; case SmpteBeats: return "SMPTE / Beats";
        case MidiTracks: return "MIDI tracks"; case Inputs: return "Inputs"; case AudioTracks: return "Audio tracks"; case AudioInstr: return "Audio instr"; case Aux: return "Aux";
        case Busses: return "Busses"; case Outputs: return "Outputs"; case User: return "User";
        case Shift: return "Shift"; case Option: return "Option"; case Control: return "Control"; case Alt: return "Alt";
        case Read: return "Read"; case Write: return "Write"; case Trim: return "Trim"; case Touch: return "Touch"; case Latch: return "Latch"; case Group: return "Group";
        case Save: return "Save"; case Undo: return "Undo"; case Cancel: return "Cancel"; case Enter: return "Enter"; case Marker: return "Marker"; case Nudge: return "Nudge";
        case Cycle: return "Cycle"; case Drop: return "Drop"; case Replace: return "Replace"; case Click: return "Click"; case SoloGlobal: return "Solo (global)";
        case Rewind: return "Rewind"; case FastFwd: return "Fast forward"; case Stop: return "Stop"; case Play: return "Play"; case Record: return "Record";
        case Up: return "Cursor up"; case Down: return "Cursor down"; case Left: return "Cursor left"; case Right: return "Cursor right"; case Zoom: return "Zoom"; case Scrub: return "Scrub";
        case UserA: return "User A"; case UserB: return "User B"; case MasterTouch: return "Master fader touch";
        default: break;
    }
    return "Note " + juce::String (note);
}

// ---- what the surface sends
struct Event
{
    enum class Kind { None, Button, Fader, Dial, Touch, Handshake, Other } kind = Kind::None;
    int index = 0;        // Button: the note number; Fader / Dial / Touch: the strip (0..7; 8 = the master fader)
    bool down = false;    // Button pressed / fader touched (false = released / let go)
    int value = 0;        // Fader: 0 .. 16383
    int delta = 0;        // Dial: steps turned, + clockwise, - anticlockwise
    bool hasReply = false;     // Handshake: 'reply' is to be sent back
    juce::MidiMessage reply;   // (a default MidiMessage is an empty SysEx: hasReply says whether this one means anything)
};

/** The Logic Control / MCU connection handshake: the surface sends its serial number and a 4-byte challenge, the host must answer with this. */
inline void handshakeResponse (const juce::uint8* l, juce::uint8* r) noexcept
{
    r[0] = (juce::uint8) (0x7F & (l[0] + (l[1] ^ 0x0A) - l[3]));
    r[1] = (juce::uint8) (0x7F & ((l[2] >> 4) ^ (l[0] + l[3])));
    r[2] = (juce::uint8) (0x7F & ((l[3] - (l[2] << 2)) ^ (l[0] | l[1])));
    r[3] = (juce::uint8) (0x7F & (l[1] - l[2] + (0xF0 ^ (l[3] << 4))));
}

inline bool isMcuSysex (const juce::MidiMessage& m)
{
    if (! m.isSysEx() || m.getSysExDataSize() < 5) return false;
    const auto* d = m.getSysExData();
    return d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x66 && (d[3] == 0x14 || d[3] == 0x15);
}

inline Event decode (const juce::MidiMessage& m)
{
    Event e;
    if (m.isPitchWheel())
    {
        const int ch = m.getChannel() - 1;
        if (ch < 0 || ch > kStrips) return e;
        e.kind = Event::Kind::Fader; e.index = ch; e.value = juce::jlimit (0, kFaderMax, m.getPitchWheelValue());
        return e;
    }
    if (m.isNoteOnOrOff())
    {
        const int n = m.getNoteNumber();
        const bool down = m.isNoteOn() && m.getVelocity() > 0;
        if (n >= TouchBase && n <= MasterTouch) { e.kind = Event::Kind::Touch; e.index = n - TouchBase; e.down = down; return e; }
        if (n >= 0 && n < NumButtons) { e.kind = Event::Kind::Button; e.index = n; e.down = down; return e; }
        e.kind = Event::Kind::Other; return e;
    }
    if (m.isController())
    {
        const int cc = m.getControllerNumber(), v = m.getControllerValue();
        if (cc >= 0x10 && cc < 0x10 + kStrips)
        {
            e.kind = Event::Kind::Dial; e.index = cc - 0x10;
            const int steps = v & 0x3F;
            e.delta = (v & 0x40) != 0 ? -juce::jmax (1, steps) : juce::jmax (1, steps);
            return e;
        }
        e.kind = Event::Kind::Other; return e;
    }
    if (isMcuSysex (m))
    {
        const auto* d = m.getSysExData(); const int n = m.getSysExDataSize();
        e.kind = Event::Kind::Handshake;
        if (d[4] == 0x01 && n >= 16)                                        // host connection query: serial (7) + challenge (4)
        {
            juce::uint8 out[17] = { 0x00, 0x00, 0x66, d[3], 0x02 };
            for (int i = 0; i < 7; ++i) out[5 + i] = d[5 + i];
            handshakeResponse (d + 12, out + 12);
            e.reply = juce::MidiMessage::createSysExMessage (out, 16); e.hasReply = true;
        }
        return e;                                                          // 03 (connected) / 04 (error): nothing to answer
    }
    e.kind = Event::Kind::Other;
    return e;
}

// ---- what Fermata sends
/** The device query that starts the handshake (the surface answers with 01; most surfaces work without it). */
inline juce::MidiMessage deviceQuery()
{
    const juce::uint8 d[] = { 0x00, 0x00, 0x66, 0x14, 0x00 };
    return juce::MidiMessage::createSysExMessage (d, 5);
}
inline juce::MidiMessage fader (int strip, int value14)        { return juce::MidiMessage::pitchWheel (juce::jlimit (0, kStrips, strip) + 1, juce::jlimit (0, kFaderMax, value14)); }
inline juce::MidiMessage lamp (int note, bool on)              { return juce::MidiMessage::noteOn (1, juce::jlimit (0, 127, note), (juce::uint8) (on ? 0x7F : 0x00)); }
/** The ring of 11 lamps round a dial. mode 0: one lamp; 1: from the centre outwards (a pan); 2: filled up from the left (a level); 3: spread from the centre.
    pos 0 = all off, 1 .. 11. centre: the lamp under the dial. */
inline juce::MidiMessage ring (int strip, int mode, int pos, bool centre = false)
{
    const int v = (juce::jlimit (0, 3, mode) << 4) | juce::jlimit (0, 11, pos) | (centre ? 0x40 : 0);
    return juce::MidiMessage::controllerEvent (1, 0x30 + juce::jlimit (0, kStrips - 1, strip), v);
}
/** Writing on the two-line display, from 'pos' (0 .. 111). Only 7-bit characters can be shown: anything else becomes '?'. */
inline juce::MidiMessage lcd (int pos, const juce::String& text)
{
    juce::MemoryBlock b;
    const juce::uint8 head[] = { 0x00, 0x00, 0x66, 0x14, 0x12, (juce::uint8) juce::jlimit (0, 2 * kLcdWidth - 1, pos) };
    b.append (head, 6);
    for (auto t = text.getCharPointer(); ! t.isEmpty(); ++t)
    {
        const auto c = *t;
        const juce::uint8 u = (c >= 32 && c < 127) ? (juce::uint8) c : (juce::uint8) '?';
        b.append (&u, 1);
    }
    return juce::MidiMessage::createSysExMessage (b.getData(), (int) b.getSize());
}
/** One cell of the display (7 characters: the text padded or cut to 6, then a space) for a strip, on line 0 (top) or 1. */
inline juce::String lcdCell (const juce::String& text)
{
    juce::String s = text.substring (0, kLcdCell - 1);
    while (s.length() < kLcdCell - 1) s += ' ';
    return s + ' ';
}
inline juce::MidiMessage lcdCellMessage (int line, int strip, const juce::String& cellText) { return lcd (line * kLcdWidth + strip * kLcdCell, lcdCell (cellText)); }
/** The two digits of the ASSIGNMENT display (7-segment): 0x4B is the left one. Digits and capital letters; anything else blank. */
inline juce::MidiMessage assignmentDigit (int which, juce::juce_wchar c)
{
    int code = 0;
    if (c >= '0' && c <= '9') code = (int) c;                    // 0x30 .. 0x39
    else if (c >= 'A' && c <= 'Z') code = (int) c - 0x40;        // 0x01 .. 0x1A
    else if (c >= 'a' && c <= 'z') code = (int) c - 0x60;
    return juce::MidiMessage::controllerEvent (1, which == 0 ? 0x4B : 0x4A, code & 0x7F);
}
/** A strip's meter: level 0 (off) .. 12 (full); 14 lights the overload, 15 clears it. */
inline juce::MidiMessage meter (int strip, int level) { return juce::MidiMessage::channelPressureChange (1, ((juce::jlimit (0, kStrips - 1, strip) << 4) | juce::jlimit (0, 15, level)) & 0x7F); }

// ---- the fader law: the same curve as the faders on the screen (LevelMeter::faderFrac in Uikit.h), so the surface's fader stands where the
// screen's does: -80 dB at the bottom, +12 at the top, 0 dB about two-thirds up. The bottom of the fader = off (-100 dB in the mixer).
inline double faderFrac (double db) noexcept
{
    static const double skew = std::log (0.5) / std::log (60.0 / 92.0);
    return std::pow (juce::jlimit (0.0, 1.0, (db + 80.0) / 92.0), skew);
}
inline int dbToFader (float db) noexcept
{
    if (db <= -79.5f) return 0;
    return juce::roundToInt (faderFrac ((double) db) * (double) kFaderMax);
}
inline float faderToDb (int value14) noexcept
{
    if (value14 <= 0) return -100.0f;
    static const double skew = std::log (0.5) / std::log (60.0 / 92.0);
    const double frac = juce::jlimit (0.0, 1.0, (double) value14 / (double) kFaderMax);
    const double db = std::pow (frac, 1.0 / skew) * 92.0 - 80.0;
    return (float) (std::round (db * 10.0) / 10.0);
}
/** A pan (-1 .. 1) as a ring position 1 .. 11 (6 = centre). */
inline int panToRing (float pan) noexcept { return 1 + juce::jlimit (0, 10, juce::roundToInt ((juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * 5.0f)); }
/** A level 0 .. 1 as a ring position 1 .. 11. */
inline int levelToRing (float frac) noexcept { return 1 + juce::jlimit (0, 10, juce::roundToInt (juce::jlimit (0.0f, 1.0f, frac) * 10.0f)); }
/** A meter level in dB as the surface's 0 .. 12 (-60 dB .. 0 dB). */
inline int meterLevel (float db) noexcept { return db <= -60.0f ? 0 : juce::jlimit (0, 12, juce::roundToInt ((db + 60.0f) / 60.0f * 12.0f)); }

/** A fader level in dB as the display shows it, 6 characters: " -inf ", "-12.5 ", " +0.0 ". */
inline juce::String dbText (float db)
{
    if (db <= -79.5f) return " -inf ";
    juce::String s = (db >= 0.0f ? "+" : "-") + juce::String (std::abs (db), 1);
    while (s.length() < 5) s = " " + s;
    return s.substring (0, 6) + " ";
}
/** A short description of a message for the monitor ("Fader 3 up to 0.0 dB", "Mute 2 pressed" ...). */
inline juce::String describe (const juce::MidiMessage& m)
{
    const auto e = decode (m);
    switch (e.kind)
    {
        case Event::Kind::Fader:  return (e.index == kStrips ? juce::String ("Master fader ") : "Fader " + juce::String (e.index + 1)) + " = " + juce::String (e.value) + "  (" + juce::String (faderToDb (e.value), 1) + " dB)";
        case Event::Kind::Dial:   return "Dial " + juce::String (e.index + 1) + (e.delta > 0 ? " clockwise " : " anticlockwise ") + juce::String (std::abs (e.delta));
        case Event::Kind::Touch:  return (e.index == kStrips ? juce::String ("Master fader") : "Fader " + juce::String (e.index + 1)) + (e.down ? " touched" : " let go");
        case Event::Kind::Button: return buttonName (e.index) + (e.down ? " pressed" : " released");
        case Event::Kind::Handshake:
        {
            const auto* d = m.getSysExData();
            switch (d[4]) { case 0x00: return "Device query"; case 0x01: return "Host connection query (serial + challenge)"; case 0x02: return "Host connection reply"; case 0x03: return "Connected";
                            case 0x04: return "Connection error"; case 0x12: return "Display text"; case 0x0E: return "Fader touch sensitivity"; default: break; }
            return "Mackie SysEx " + juce::String::toHexString (d[4]);
        }
        default: break;
    }
    if (m.isController())
    {
        const int cc = m.getControllerNumber();
        if (cc >= 0x30 && cc < 0x30 + kStrips) return "Ring " + juce::String (cc - 0x2F) + " = " + juce::String::toHexString (m.getControllerValue());
        if (cc == 0x4A || cc == 0x4B) return "Assignment digit";
        if (cc >= 0x40 && cc <= 0x49) return "Time display digit";
        return "CC " + juce::String (cc) + " = " + juce::String (m.getControllerValue());
    }
    if (m.isChannelPressure()) return "Meter " + juce::String ((m.getChannelPressureValue() >> 4) + 1) + " = " + juce::String (m.getChannelPressureValue() & 0x0F);
    if (m.isSysEx()) return "SysEx (" + juce::String (m.getSysExDataSize()) + " bytes)";
    return m.getDescription();
}
/** The raw bytes as hex: "E0 00 40". SysEx is cut after a few bytes. */
inline juce::String hexOf (const juce::MidiMessage& m)
{
    const int n = juce::jmin (12, m.getRawDataSize());
    juce::String s = juce::String::toHexString (m.getRawData(), n, 1).toUpperCase();
    return m.getRawDataSize() > n ? s + " ..." : s;
}
} // namespace mcu
} // namespace td

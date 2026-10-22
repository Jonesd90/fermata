#include "Surface.h"

namespace td
{
namespace
{
constexpr int kMonitorLines = 400;
constexpr float kDialPanStep = 0.02f, kDialDbStep = 0.5f, kDialGainStep = 0.5f;

juce::String panText (float pan)
{
    const int n = (int) std::lround (juce::jlimit (-1.0f, 1.0f, pan) * 100.0f);
    return n == 0 ? juce::String ("C") : n < 0 ? "L" + juce::String (-n) : "R" + juce::String (n);
}
juce::String timeNow() { return juce::Time::getCurrentTime().formatted ("%H:%M:%S"); }
bool looksLikeSurface (const juce::String& name)
{
    const auto n = name.toLowerCase();
    return n.contains ("eumidi") || n.contains ("eucon") || n.contains ("euphonix") || n.contains ("mackie") || n.contains ("mcu") || n.contains ("avid s1");
}
}

SurfaceController::SurfaceController (AppContext& a) : app (a)
{
    defaults();
    load();
    openPorts();
    startTimerHz (30);
}

SurfaceController::~SurfaceController()
{
    stopTimer();
    cancelPendingUpdate();
    if (out != nullptr) blank();
    closePorts();
}

// ============================================================================ settings
void SurfaceController::defaults()
{
    map.clear();
    auto bind = [this] (int note, int action) { map[note] = action; };
    // the S1's eight keys along the bottom, left to right: Shift, Control, Opt/Win, Alt, User 1 .. 4. In EuControl's Mackie mode they arrive as
    // Shift, Control, Option, Alt and the four cursor keys (learned on the user's S1 by the Anubis Bridge: 0x46 0x48 0x47 0x49 0x60 0x61 0x62 0x63).
    // The Mackie F1 .. F8 keys (a real Mackie surface) do the same eight jobs.
    bind (mcu::Shift,   surface::ZoomOut);       bind (mcu::F1, surface::ZoomOut);
    bind (mcu::Control, surface::ZoomIn);        bind (mcu::F2, surface::ZoomIn);
    bind (mcu::Option,  surface::TracksSmaller); bind (mcu::F3, surface::TracksSmaller);
    bind (mcu::Alt,     surface::TracksBigger);  bind (mcu::F4, surface::TracksBigger);
    bind (mcu::Up,      surface::WaveSmaller);   bind (mcu::F5, surface::WaveSmaller);
    bind (mcu::Down,    surface::WaveBigger);    bind (mcu::F6, surface::WaveBigger);
    bind (mcu::Left,    surface::MeterBridge);   bind (mcu::F7, surface::MeterBridge);
    bind (mcu::Right,   surface::MixerToggle);   bind (mcu::F8, surface::MixerToggle);
    bind (mcu::PlugIn, surface::DialGain);
    bind (mcu::Pan, surface::DialPan);
    bind (mcu::Flip, surface::FlipFaders);
    bind (mcu::BankLeft, surface::BankLeft); bind (mcu::BankRight, surface::BankRight);
    bind (mcu::ChannelLeft, surface::ChannelLeft); bind (mcu::ChannelRight, surface::ChannelRight);
    bind (mcu::Play, surface::PlayPause); bind (mcu::Stop, surface::StopPlay); bind (mcu::Record, surface::RecordTake);
    const int sm[] = { surface::SaLowCut, surface::SaSolo, surface::SaMute, surface::SaSelect, surface::SaDialHome };
    for (int i = 0; i < surface::NumStripButtons; ++i) stripMap[i] = sm[i];
    meters = true;
}

void SurfaceController::load()
{
    auto* st = app.props.getUserSettings();
    if (st == nullptr) return;
    want.inId = st->getValue ("surfaceIn"); want.inName = st->getValue ("surfaceInName");
    want.outId = st->getValue ("surfaceOut"); want.outName = st->getValue ("surfaceOutName");
    const auto t = st->getValue ("surfaceTarget", "follow");
    if (t == "off") mode = Target::Off;
    else if (t.startsWith ("pin:")) { mode = Target::Pinned; pin = juce::Uuid (t.substring (4)); }
    else mode = Target::Follow;
    if (st->containsKey ("surfaceMap"))
    {
        map.clear();
        for (auto& pair : juce::StringArray::fromTokens (st->getValue ("surfaceMap"), ",", {}))
        {
            const int note = pair.upToFirstOccurrenceOf (":", false, false).getIntValue(), action = pair.fromFirstOccurrenceOf (":", false, false).getIntValue();
            if (juce::isPositiveAndBelow (note, 128) && juce::isPositiveAndBelow (action, surface::NumActions)) map[note] = action;
        }
    }
    if (st->containsKey ("surfaceStrip"))
    {
        const auto v = juce::StringArray::fromTokens (st->getValue ("surfaceStrip"), ",", {});
        for (int i = 0; i < juce::jmin (v.size(), (int) surface::NumStripButtons); ++i) stripMap[i] = juce::jlimit (0, surface::NumStripActions - 1, v[i].getIntValue());
    }
    meters = st->getBoolValue ("surfaceMeters", true);
    law = st->getIntValue ("surfaceLaw", 0) == 1 ? mcu::FaderLaw::Screen : mcu::FaderLaw::EuControl;
}

void SurfaceController::save()
{
    auto* st = app.props.getUserSettings();
    if (st == nullptr) return;
    st->setValue ("surfaceIn", want.inId); st->setValue ("surfaceInName", want.inName);
    st->setValue ("surfaceOut", want.outId); st->setValue ("surfaceOutName", want.outName);
    st->setValue ("surfaceTarget", mode == Target::Off ? juce::String ("off") : mode == Target::Pinned ? "pin:" + pin.toString() : juce::String ("follow"));
    juce::StringArray m; for (auto& kv : map) m.add (juce::String (kv.first) + ":" + juce::String (kv.second));
    st->setValue ("surfaceMap", m.joinIntoString (","));
    juce::StringArray sm; for (int i = 0; i < surface::NumStripButtons; ++i) sm.add (juce::String (stripMap[i]));
    st->setValue ("surfaceStrip", sm.joinIntoString (","));
    st->setValue ("surfaceMeters", meters);
    st->setValue ("surfaceLaw", law == mcu::FaderLaw::Screen ? 1 : 0);
    app.props.saveIfNeeded();
}

void SurfaceController::setTarget (Target t, const juce::Uuid& p)
{
    mode = t; pin = t == Target::Pinned ? p : juce::Uuid::null();
    force = true;
    save();
    sendChangeMessage();
}

juce::Uuid SurfaceController::controlledMixer() const
{
    if (mode == Target::Off) return juce::Uuid::null();
    if (mode == Target::Pinned && app.findMixer (pin) != nullptr) return pin;
    return app.frontMixerId ? app.frontMixerId() : juce::Uuid::null();
}

int SurfaceController::actionOf (int note) const
{
    auto it = map.find (note);
    return it == map.end() ? -1 : it->second;
}
juce::String SurfaceController::bindingsText (int action) const
{
    juce::StringArray n;
    for (auto& kv : map) if (kv.second == action) n.add (mcu::buttonName (kv.first));
    return n.joinIntoString (", ");
}
void SurfaceController::startLearn (int action) { learnAction = juce::isPositiveAndBelow (action, surface::NumActions) ? action : -1; sendChangeMessage(); }
void SurfaceController::unbind (int action)
{
    for (auto it = map.begin(); it != map.end();) { if (it->second == action) it = map.erase (it); else ++it; }
    save(); sendChangeMessage();
}
void SurfaceController::resetMap()
{
    const auto keepMode = mode; const auto keepPin = pin; const auto keepPorts = want;
    defaults();
    mode = keepMode; pin = keepPin; want = keepPorts;
    save(); force = true; sendChangeMessage();
}
void SurfaceController::setStripAssign (int button, int action)
{
    if (! juce::isPositiveAndBelow (button, surface::NumStripButtons)) return;
    stripMap[button] = juce::jlimit (0, surface::NumStripActions - 1, action);
    save(); force = true; sendChangeMessage();
}
void SurfaceController::setFaderLaw (mcu::FaderLaw l) { law = l; save(); force = true; }

void SurfaceController::setMetersOn (bool on)
{
    meters = on; save(); force = true;
    if (! on) for (int i = 0; i < mcu::kStrips; ++i) { send (mcu::meter (i, 0)); sent[i].meter = -1; }      // (the surface's meters go dark)
}

// ============================================================================ the ports
void SurfaceController::setPorts (const juce::String& inIdentifier, const juce::String& outIdentifier)
{
    if (out != nullptr) blank();
    closePorts();
    want.inId = inIdentifier; want.outId = outIdentifier; want.inName.clear(); want.outName.clear();
    for (auto& d : juce::MidiInput::getAvailableDevices())  if (d.identifier == inIdentifier)  want.inName = d.name;
    for (auto& d : juce::MidiOutput::getAvailableDevices()) if (d.identifier == outIdentifier) want.outName = d.name;
    save();
    openPorts();
    sendChangeMessage();
}

void SurfaceController::openPorts()
{
    lastOpenTry = juce::Time::getMillisecondCounter();
    const auto ins = juce::MidiInput::getAvailableDevices(), outs = juce::MidiOutput::getAvailableDevices();
    bool changed = false;
    // a port that has gone (EuControl closed, the surface unplugged) is let go of, and looked for again
    if (in != nullptr)  { bool still = false; for (auto& d : ins)  still = still || d.identifier == in->getDeviceInfo().identifier;  if (! still) { juce::Logger::writeToLog ("Surface: MIDI in '" + in->getName() + "' has gone");  in.reset();  changed = true; } }
    if (out != nullptr) { bool still = false; for (auto& d : outs) still = still || d.identifier == out->getDeviceInfo().identifier; if (! still) { juce::Logger::writeToLog ("Surface: MIDI out '" + out->getName() + "' has gone"); out.reset(); changed = true; for (auto& s : sent) s = {}; } }
    // nothing chosen yet: a port that looks like the S1 (EuControl's "EuMidi" ports) or a Mackie surface is taken (until one is chosen in the panel)
    auto pick = [] (const juce::Array<juce::MidiDeviceInfo>& list, const juce::String& wantId, const juce::String& wantName) -> juce::String
    {
        if (wantId.isNotEmpty())
        {
            for (auto& d : list) if (d.identifier == wantId) return d.identifier;
            for (auto& d : list) if (wantName.isNotEmpty() && d.name == wantName) return d.identifier;       // (the same port under a new identifier)
            return {};
        }
        for (auto& d : list) if (looksLikeSurface (d.name)) return d.identifier;
        return {};
    };
    if (in == nullptr)
    {
        const auto id = pick (ins, want.inId, want.inName);
        if (id.isNotEmpty())
        {
            in = juce::MidiInput::openDevice (id, this);
            if (in != nullptr) { in->start(); juce::Logger::writeToLog ("Surface: MIDI in '" + in->getName() + "' open"); changed = true; }
        }
    }
    if (out == nullptr)
    {
        const auto id = pick (outs, want.outId, want.outName);
        if (id.isNotEmpty())
        {
            out = juce::MidiOutput::openDevice (id);
            if (out != nullptr)
            {
                juce::Logger::writeToLog ("Surface: MIDI out '" + out->getName() + "' open");
                for (auto& s : sent) s = {};
                sentFlip = sentPan = sentPlug = sentDigits = -1;
                send (mcu::deviceQuery());
                force = true; changed = true;
            }
        }
    }
    if (changed) sendChangeMessage();
}

void SurfaceController::closePorts()
{
    if (in != nullptr) { in->stop(); in.reset(); }
    out.reset();
    { const juce::ScopedLock sl (lock); pending.clear(); }
    for (auto& s : sent) s = {};
    active = false;
}

juce::String SurfaceController::statusText() const
{
    if (want.inId.isEmpty() && want.outId.isEmpty() && in == nullptr && out == nullptr) return "No MIDI ports chosen";
    if (in != nullptr && out != nullptr) return "Connected: " + in->getName() + (out->getName() != in->getName() ? " / " + out->getName() : juce::String());
    juce::StringArray missing;
    if (in == nullptr)  missing.add ("in '" + (want.inName.isNotEmpty() ? want.inName : want.inId) + "'");
    if (out == nullptr) missing.add ("out '" + (want.outName.isNotEmpty() ? want.outName : want.outId) + "'");
    return "Waiting for MIDI " + missing.joinIntoString (" and ") + " (is EuControl running?)";
}

// ============================================================================ MIDI in -> the message thread
void SurfaceController::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    if (m.isActiveSense() || m.isMidiClock()) return;
    { const juce::ScopedLock sl (lock); if (pending.size() < 4096) pending.push_back (m); }
    triggerAsyncUpdate();
}

void SurfaceController::handleAsyncUpdate()
{
    std::vector<juce::MidiMessage> got;
    { const juce::ScopedLock sl (lock); got.swap (pending); }
    for (auto& m : got)
    {
        log (false, m);
        handle (mcu::decode (m), m);
    }
}

void SurfaceController::log (bool outgoing, const juce::MidiMessage& m)
{
    if (monitorPaused || (outgoing && ! monitorShowsOutput)) return;
    lines.push_back ({ timeNow(), outgoing ? "out" : "in", mcu::hexOf (m), mcu::describe (m) });
    while ((int) lines.size() > kMonitorLines) lines.pop_front();
    sendChangeMessage();
}

void SurfaceController::send (const juce::MidiMessage& m)
{
    if (out == nullptr) return;
    out->sendMessageNow (m);
    if (monitorShowsOutput) log (true, m);
}

// ============================================================================ what the surface does
void SurfaceController::handle (const mcu::Event& e, const juce::MidiMessage&)
{
    using K = mcu::Event::Kind;
    switch (e.kind)
    {
        case K::Handshake: if (e.hasReply) send (e.reply); return;
        case K::Fader:     if (e.index < mcu::kStrips) moveFader (e.index, e.value); return;
        case K::Touch:     if (e.index < mcu::kStrips) touched[e.index] = e.down; return;
        case K::Dial:      turnDial (e.index, e.delta); return;
        case K::Button:    break;
        default:           return;
    }
    if (! e.down) return;                                              // nothing happens on a release
    const int n = e.index;
    if (learnAction >= 0)                                              // the panel is waiting for a key: this is it
    {
        if (n >= mcu::RecBase && n < mcu::PushBase + mcu::kStrips) { app.setNotice ("That is one of the strip keys: choose what they do in the 'strip keys' boxes."); learnAction = -1; sendChangeMessage(); return; }
        for (auto it = map.begin(); it != map.end();) { if (it->first == n) it = map.erase (it); else ++it; }
        map[n] = learnAction;
        learnAction = -1;
        save(); sendChangeMessage();
        app.setNotice ("'" + mcu::buttonName (n) + "' now does: " + surface::actionName (map[n]));
        return;
    }
    if (n >= mcu::RecBase    && n < mcu::RecBase + mcu::kStrips)    { doStripButton (n - mcu::RecBase,    surface::SbRec);    return; }
    if (n >= mcu::SoloBase   && n < mcu::SoloBase + mcu::kStrips)   { doStripButton (n - mcu::SoloBase,   surface::SbSolo);   return; }
    if (n >= mcu::MuteBase   && n < mcu::MuteBase + mcu::kStrips)   { doStripButton (n - mcu::MuteBase,   surface::SbMute);   return; }
    if (n >= mcu::SelectBase && n < mcu::SelectBase + mcu::kStrips) { doStripButton (n - mcu::SelectBase, surface::SbSelect); return; }
    if (n >= mcu::PushBase   && n < mcu::PushBase + mcu::kStrips)   { doStripButton (n - mcu::PushBase,   surface::SbPush);   return; }
    const int a = actionOf (n);
    if (a >= 0) doAction (a);
}

void SurfaceController::doAction (int a)
{
    auto key = [this] (const juce::KeyPress& k) { if (app.keyToWorkWindow && ! app.keyToWorkWindow (k)) app.setNotice ("No take or edit window has been in front yet: the surface's zoom keys act on the one you used last."); };
    switch (a)
    {
        case surface::ZoomOut:       key (juce::KeyPress (juce::KeyPress::leftKey));  break;
        case surface::ZoomIn:        key (juce::KeyPress (juce::KeyPress::rightKey)); break;
        case surface::TracksSmaller: key (juce::KeyPress (juce::KeyPress::upKey));    break;
        case surface::TracksBigger:  key (juce::KeyPress (juce::KeyPress::downKey));  break;
        case surface::WaveSmaller:   key (juce::KeyPress (',', juce::ModifierKeys(), ',')); break;
        case surface::WaveBigger:    key (juce::KeyPress ('.', juce::ModifierKeys(), '.')); break;
        case surface::MeterBridge:   if (app.showBridge) app.showBridge(); break;
        case surface::MixerToggle:   app.remoteMixer (0); break;
        case surface::DialGain:      dialGain = ! dialGain; force = true; break;
        case surface::DialPan:       dialGain = false; force = true; break;
        case surface::FlipFaders:    flipped = ! flipped; force = true; break;
        case surface::BankLeft:      bank = juce::jmax (0, bank - mcu::kStrips); banks[shownMixer] = bank; force = true; break;
        case surface::BankRight:     bank = juce::jlimit (0, juce::jmax (0, (int) chans.size() - mcu::kStrips), bank + mcu::kStrips); banks[shownMixer] = bank; force = true; break;
        case surface::ChannelLeft:   bank = juce::jmax (0, bank - 1); banks[shownMixer] = bank; force = true; break;
        case surface::ChannelRight:  bank = juce::jlimit (0, juce::jmax (0, (int) chans.size() - mcu::kStrips), bank + 1); banks[shownMixer] = bank; force = true; break;
        case surface::PlayPause:     app.remotePlayPause(); break;
        case surface::StopPlay:      app.stopPlayback(); break;
        case surface::RecordTake:    app.remoteRecord(); break;
        default: break;
    }
}

void SurfaceController::doStripButton (int strip, int button)
{
    const auto* c = chanAt (strip);
    if (c == nullptr) return;
    switch (stripAssign (button))
    {
        case surface::SaMute:  c->mute().set (! c->mute().get()); app.project.markDirty(); break;
        case surface::SaSolo:  if (c->hasSolo()) { c->solo().set (! c->solo().get()); app.project.markDirty(); } break;
        case surface::SaSelect: if (c->kind == Chan::Kind::Track && app.selectStripInMixer) app.selectStripInMixer (shownMixer, c->id); break;
        case surface::SaLowCut:
            if (c->hasLc())
            {
                auto& lc = c->s != nullptr ? c->s->lowCut : c->b->lowCut;
                lc.setOn (! lc.on.get());
                app.engine.checkLatency(); app.project.markDirty(); app.project.sendChangeMessage();
                showValue (strip, lc.on.get() ? "LC on" : "LC off");
            }
            break;
        case surface::SaDialHome:
            if (flipped) { c->level().set (0.0f); app.project.markDirty(); showValue (strip, "0 dB"); }
            else if (dialGain) { if (c->kind == Chan::Kind::Track) { setPreampGain (*c, 0.0f, true); bool has = false; const float g = preampGainOf (*c, has, true); showValue (strip, has ? "G " + juce::String (g, 1) : juce::String ("no pre")); } }
            else if (c->hasPan())
            {
                if (c->s != nullptr && c->numCh == 2) { c->s->panL.set (-1.0f); c->s->panR.set (1.0f); }
                else setPanMid (*c, 0.0f);
                app.project.markDirty(); showValue (strip, c->numCh == 2 ? "L / R" : "C");
            }
            break;
        default: break;
    }
}

void SurfaceController::moveFader (int strip, int v)
{
    sent[strip].fader = v;                                             // (what it sent is where it is: no echo back to it)
    const auto* c = chanAt (strip);
    if (c == nullptr) return;
    if (! flipped) { c->level().set (mcu::faderToDb (v, law)); app.project.markDirty(); return; }
    if (dialGain)
    {
        if (c->kind != Chan::Kind::Track) return;
        bool has = false; const float raw = preampGainOf (*c, has, false); if (! has) return;
        const float g = std::round ((float) v / (float) mcu::kFaderMax * 66.0f * 2.0f) / 2.0f;
        setPreampGain (*c, g - raw, false);
        showValue (strip, "G " + juce::String (preampGainOf (*c, has, true), 1));
    }
    else if (c->hasPan())
    {
        setPanMid (*c, (float) v / (float) mcu::kFaderMax * 2.0f - 1.0f);
        app.project.markDirty();
        showValue (strip, panText (panMid (*c)));
    }
}

void SurfaceController::turnDial (int strip, int delta)
{
    const auto* c = chanAt (strip);
    if (c == nullptr || delta == 0) return;
    if (flipped)                                                       // the dial moves the level while the fader has the pan / gain
    {
        const float cur = c->level().get();
        const float nv = juce::jlimit (-80.0f, 12.0f, (cur <= -79.5f ? -80.0f : cur) + (float) delta * kDialDbStep);
        c->level().set (nv <= -79.5f ? -100.0f : nv); app.project.markDirty();
        showValue (strip, mcu::dbText (nv).trim() + " dB");
        return;
    }
    if (dialGain)
    {
        if (c->kind != Chan::Kind::Track) { showValue (strip, "no pre"); return; }
        bool has = false; preampGainOf (*c, has, true);
        if (! has) { showValue (strip, "no pre"); return; }
        setPreampGain (*c, (float) delta * kDialGainStep, false);
        showValue (strip, "G " + juce::String (preampGainOf (*c, has, true), 1));
        return;
    }
    if (! c->hasPan()) return;
    setPanMid (*c, panMid (*c) + (float) delta * kDialPanStep);
    app.project.markDirty();
    showValue (strip, panText (panMid (*c)));
}

void SurfaceController::showValue (int strip, const juce::String& text)
{
    if (! juce::isPositiveAndBelow (strip, mcu::kStrips)) return;
    valueText[strip] = text; valueUntil[strip] = juce::Time::getMillisecondCounter() + 1500;
}

// ---- the pans and the preamp gain
float SurfaceController::panMid (const Chan& c) const
{
    if (c.b != nullptr) return c.b->pan.get();
    if (c.s == nullptr) return 0.0f;
    return c.numCh == 2 ? (c.s->panL.get() + c.s->panR.get()) * 0.5f : c.s->pan.get();
}
void SurfaceController::setPanMid (const Chan& c, float mid)
{
    mid = juce::jlimit (-1.0f, 1.0f, mid);
    if (c.b != nullptr) { c.b->pan.set (mid); return; }
    if (c.s == nullptr) return;
    if (c.numCh != 2) { c.s->pan.set (mid); return; }
    // a stereo track: both placements move together (the image shifts, its width is kept), as far as the outer one can go
    const float l = c.s->panL.get(), r = c.s->panR.get();
    float d = mid - (l + r) * 0.5f;
    d = juce::jlimit (-1.0f - juce::jmin (l, r), 1.0f - juce::jmax (l, r), d);
    c.s->panL.set (juce::jlimit (-1.0f, 1.0f, l + d)); c.s->panR.set (juce::jlimit (-1.0f, 1.0f, r + d));
}
float SurfaceController::preampGainOf (const Chan& c, bool& has, bool shown) const
{
    has = false;
    if (c.kind != Chan::Kind::Track) return 0.0f;
    auto* tr = app.project.findTrack (c.id);
    if (tr == nullptr) return 0.0f;
    const int idx = tr->inputOf (0);
    if (! juce::isPositiveAndBelow (idx, (int) app.project.inputs.size()) || app.project.preampDriver == nullptr || ! app.project.preampDriver->controls (idx)) return 0.0f;
    has = true;
    const auto& p = app.project.inputs[(size_t) idx].preamp;
    return shown ? p.gainDb + p.shownGainOffset() : p.gainDb;        // (shown: as the main window's read-out has it - the MT48's Pad moves the scale)
}
void SurfaceController::setPreampGain (const Chan& c, float deltaDb, bool home)
{
    auto* tr = app.project.findTrack (c.id);
    if (tr == nullptr || app.project.preampDriver == nullptr) return;
    for (int ch = 0; ch < c.numCh; ++ch)                              // a stereo track: both its inputs (each keeps its own offset)
    {
        const int idx = tr->inputOf (ch);
        if (! juce::isPositiveAndBelow (idx, (int) app.project.inputs.size()) || ! app.project.preampDriver->controls (idx)) continue;
        PreampSettings s = app.project.inputs[(size_t) idx].preamp;
        const float nv = home ? 20.0f - s.shownGainOffset() : s.gainDb + deltaDb;
        s.gainDb = juce::jlimit (0.0f, 66.0f, std::round (nv * 2.0f) / 2.0f);
        app.project.setPreamp (idx, s);
    }
}

// ============================================================================ the channels of the controlled mixer
std::vector<SurfaceController::Chan> SurfaceController::channels (MixerState& m) const
{
    std::vector<Chan> v;
    for (auto& t : app.project.tracks)
    {
        if (m.isStripHidden (t.id)) continue;
        Chan c; c.kind = Chan::Kind::Track; c.id = t.id; c.name = t.name; c.numCh = t.channelCount(); c.s = m.stripFor (t.id);
        v.push_back (c);
    }
    for (int pass = 0; pass < 2; ++pass)
        for (auto& b : app.project.buses)
            if (b.external == (pass == 1))
            {
                Chan c; c.kind = pass == 0 ? Chan::Kind::IntBus : Chan::Kind::ExtBus; c.id = b.id; c.name = b.name; c.numCh = 2; c.b = m.busFor (b.id);
                v.push_back (c);
            }
    for (auto& d : app.project.vcas)
        if (m.vcaShown.load() & (1u << (d.number - 1)))
        {
            Chan c; c.kind = Chan::Kind::Vca; c.id = d.id; c.name = "V" + d.label(); c.numCh = 0; c.v = &m.vcas[d.number - 1];
            v.push_back (c);
        }
    return v;
}

// ============================================================================ the feedback (30 times a second)
void SurfaceController::timerCallback()
{
    ++ticks;
    const auto now = juce::Time::getMillisecondCounter();
    if ((in == nullptr || out == nullptr || ticks % 60 == 0) && now - lastOpenTry > 1900) openPorts();      // a port that appears (EuControl started) is taken; one that goes is noticed
    if (out == nullptr) return;
    const auto id = controlledMixer();
    auto* m = app.findMixer (id);
    if (m == nullptr || ! app.hasProject())
    {
        if (active) blank();
        shownMixer = juce::Uuid::null();
        return;
    }
    if (id != shownMixer)
    {
        shownMixer = id;
        auto it = banks.find (id); bank = it != banks.end() ? it->second : 0;
        banner = m->name; bannerUntil = now + 1500; bannerShown = false;
        force = true;
    }
    chans = channels (*m);
    bank = juce::jlimit (0, juce::jmax (0, (int) chans.size() - mcu::kStrips), bank);
    show (force);
    force = false; active = true;
}

void SurfaceController::blank()
{
    for (int i = 0; i < mcu::kStrips; ++i)
    {
        send (mcu::fader (i, 0)); send (mcu::ring (i, 0, 0));
        for (int base : { mcu::RecBase, mcu::SoloBase, mcu::MuteBase, mcu::SelectBase }) send (mcu::lamp (base + i, false));
        if (meters) send (mcu::meter (i, 0));
        sent[i] = {};
    }
    for (int n : { mcu::Flip, mcu::PlugIn, mcu::Pan }) send (mcu::lamp (n, false));
    send (mcu::assignmentDigit (0, ' ')); send (mcu::assignmentDigit (1, ' '));
    send (mcu::lcd (0, juce::String::repeatedString (" ", mcu::kLcdWidth)));
    send (mcu::lcd (mcu::kLcdWidth, juce::String::repeatedString (" ", mcu::kLcdWidth)));
    sentFlip = sentPan = sentPlug = sentDigits = -1;
    active = false; bannerShown = false;
}

void SurfaceController::show (bool everything)
{
    const auto now = juce::Time::getMillisecondCounter();
    if (everything) { for (auto& s : sent) s = {}; sentFlip = sentPan = sentPlug = sentDigits = -1; bannerShown = false; }
    const bool bannerOn = now < bannerUntil;
    for (int i = 0; i < mcu::kStrips; ++i)
    {
        const auto* c = chanAt (i);
        auto& s = sent[i];
        // the fader: the level, or (flipped) the pan / gain
        int fv = 0;
        if (c != nullptr)
        {
            if (! flipped) fv = mcu::dbToFader (c->level().get(), law);
            else if (dialGain) { bool has = false; const float g = preampGainOf (*c, has, false); fv = has ? juce::roundToInt (g / 66.0f * (float) mcu::kFaderMax) : 0; }
            else if (c->hasPan()) fv = juce::roundToInt ((panMid (*c) + 1.0f) * 0.5f * (float) mcu::kFaderMax);
        }
        if (! touched[i] && fv != s.fader) { send (mcu::fader (i, fv)); s.fader = fv; }
        // the ring round the dial: the pan (from the centre), the gain (filled up), or (flipped) the level
        int rv = 0;
        if (c != nullptr)
        {
            if (flipped) rv = (2 << 4) | mcu::levelToRing ((float) mcu::faderFrac ((double) juce::jmax (-80.0f, c->level().get())));
            else if (dialGain) { bool has = false; const float g = preampGainOf (*c, has, false); rv = has ? ((2 << 4) | mcu::levelToRing (g / 66.0f)) : 0; }
            else if (c->hasPan()) { const float p = panMid (*c); rv = (1 << 4) | mcu::panToRing (p) | (std::abs (p) < 0.005f ? 0x40 : 0); }
        }
        if (rv != s.ring) { send (juce::MidiMessage::controllerEvent (1, 0x30 + i, rv & 0x7F)); s.ring = rv; }
        // the four lamps in the strip's keys
        const int bases[4] = { mcu::RecBase, mcu::SoloBase, mcu::MuteBase, mcu::SelectBase };
        for (int k = 0; k < 4; ++k)
        {
            bool on = false;
            if (c != nullptr)
                switch (stripMap[k])
                {
                    case surface::SaMute:   on = c->mute().get(); break;
                    case surface::SaSolo:   on = c->hasSolo() && c->solo().get(); break;
                    case surface::SaSelect: on = c->kind == Chan::Kind::Track && app.isStripSelectedInMixer && app.isStripSelectedInMixer (shownMixer, c->id); break;
                    case surface::SaLowCut: on = c->hasLc() && (c->s != nullptr ? c->s->lowCut : c->b->lowCut).on.get(); break;
                    default: break;
                }
            if ((int) on != s.lamp[k]) { send (mcu::lamp (bases[k] + i, on)); s.lamp[k] = (int) on; }
        }
        // the writing: the name above, the level (or the value just turned) below
        const auto top = c != nullptr ? mcu::lcdCell (c->name) : juce::String::repeatedString (" ", mcu::kLcdCell);
        if (top != s.top) { send (mcu::lcd (i * mcu::kLcdCell, top)); s.top = top; }
        if (! bannerOn)
        {
            juce::String bottom;
            if (c == nullptr) bottom = juce::String::repeatedString (" ", mcu::kLcdCell);
            else if (now < valueUntil[i]) bottom = mcu::lcdCell (valueText[i]);
            else bottom = mcu::dbText (c->level().get());
            if (bottom != s.bottom) { send (mcu::lcd (mcu::kLcdWidth + i * mcu::kLcdCell, bottom)); s.bottom = bottom; }
        }
        // the meter (15 times a second)
        if (meters && (ticks & 1) == 0)
        {
            int lv = 0;
            if (c != nullptr && c->v == nullptr)
            {
                const float rms = c->s != nullptr ? juce::jmax (c->s->rmsL.get(), c->s->rmsR.get()) : juce::jmax (c->b->rmsL.get(), c->b->rmsR.get());
                lv = rms > 1.0e-6f ? mcu::meterLevel (20.0f * std::log10 (rms)) : 0;
            }
            if (lv != s.meter) { send (mcu::meter (i, lv)); s.meter = lv; }
        }
    }
    if (bannerOn && ! bannerShown)                                     // the mixer's name across the bottom line for a moment when the surface moves to another mixer
    {
        juce::String b = "  " + banner;
        b = b.substring (0, mcu::kLcdWidth); while (b.length() < mcu::kLcdWidth) b += ' ';
        send (mcu::lcd (mcu::kLcdWidth, b));
        bannerShown = true;
        for (auto& s : sent) s.bottom.clear();                         // (written over when it goes)
    }
    if ((int) flipped != sentFlip)  { send (mcu::lamp (mcu::Flip, flipped));    sentFlip = (int) flipped; }
    if ((int) dialGain != sentPlug) { send (mcu::lamp (mcu::PlugIn, dialGain)); sentPlug = (int) dialGain; }
    if ((int) ! dialGain != sentPan) { send (mcu::lamp (mcu::Pan, ! dialGain)); sentPan = (int) ! dialGain; }
    const int first = juce::jmin (99, bank + 1);                      // the number of the first strip shown, on the two-digit display
    if (first != sentDigits) { send (mcu::assignmentDigit (0, (juce::juce_wchar) ('0' + first / 10))); send (mcu::assignmentDigit (1, (juce::juce_wchar) ('0' + first % 10))); sentDigits = first; }
}
} // namespace td

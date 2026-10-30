#include "RemoteControl.h"
#include "../core/SessionClock.h"
#include "../core/RemoteDesk.h"

namespace td
{
RemoteControlServer::RemoteControlServer (AppContext& a, int port)
    : app (a), hub ([this] (remote::Hub::Connection& c, const juce::String& line) { handleLine (c, line); })
{
    if (port <= 0) return;                                                 // (0 in the settings file switches the control port off)
    hub.log = [] (const juce::String& s) { juce::Logger::writeToLog (s); };
    if (! hub.start (port))
    {
        juce::Logger::writeToLog ("Control port: 127.0.0.1:" + juce::String (port) + " could not be opened (is another program, or another Fermata, using it?) - the Stream Deck cannot reach Fermata");
        return;
    }
    startTimer (50);                                                       // (the state is only sent when it has changed; the meters 20 times a second to whoever asked)
    app.peakListeners.add (this);
}

RemoteControlServer::~RemoteControlServer()
{
    *alive = false;
    stopTimer();
    cancelPendingUpdate();
    app.peakListeners.remove (this);
    hub.stop();                                                            // every controller's thread has ended before anything else goes
}

void RemoteControlServer::handleLine (remote::Hub::Connection& from, const juce::String& line)
{
    const auto cmd = remote::parseCommand (line);
    if (cmd.name.isEmpty()) return;
    if (cmd.name == "desk")                                                // (answered here: it only sets a flag of this connection)
    {
        from.setWantsDesk (cmd.on);
        if (cmd.on) deskWanted = true;
        return;
    }
    {
        const juce::ScopedLock sl (queueLock);
        if (queued.size() >= kMaxQueued)                                 // (a flood: what does not fit is dropped, said once in the log)
        {
            if (! queueFull) { queueFull = true; juce::Logger::writeToLog ("Control port: a controller is sending commands faster than they can be carried out; some are dropped"); }
            return;
        }
        queued.push_back (cmd);
    }
    triggerAsyncUpdate();
}

void RemoteControlServer::handleAsyncUpdate()
{
    std::vector<remote::Command> batch;
    { const juce::ScopedLock sl (queueLock); batch.swap (queued); queueFull = false; }
    for (auto& c : batch) if (*alive) execute (c);
}

void RemoteControlServer::execute (const remote::Command& c)
{
    namespace desk = remote::desk;
    auto& P = app.project;
    if (c.name == "record") app.remoteRecord();
    else if (c.name == "playpause") app.remotePlayPause();
    else if (c.name == "talk") app.remoteTalk (c.down);
    else if (c.name == "playback") app.remotePlayback();
    else if (c.name == "mixer") app.remoteMixer (c.index);
    else if (c.name == "prerec") app.setSessionMode (! app.sessionMode());
    else if (c.name == "recmode") app.setRecMode (! app.recMode());               // (3.18.0: the Rec half of the Rec | Pre button)
    else if (c.name == "hello") {}
    else if (app.hasProject())                                             // the Stream Deck + XL
    {
        if (c.name == "bridge" || c.name == "pitchpipe" || c.name == "click" || c.name == "minimiseall") { if (app.deskWindowToggle) app.deskWindowToggle (c.name); }
        else if (c.name == "openwin") { if (app.deskWindowToggle && remote::isOpenWin (c.what)) app.deskWindowToggle (c.what); }
        else if (c.name == "takewindow") { if (app.deskWindowToggle) app.deskWindowToggle ("take"); }
        else if (c.name == "armtoggle") app.remoteArmToggle();
        else if (c.name == "wavecolour") app.remoteWaveColour();
        else if (c.name == "loop") { if (app.deskLoopToggle) app.deskLoopToggle(); }
        else if (c.name == "wact") { if (app.deskAct && remote::isWindowAct (c.what)) app.deskAct (c.what); }
        else if (c.name == "monitor") desk::monitorCycle (P, c.input);
        else if (c.name == "talkin") { if (juce::isPositiveAndBelow (c.input, (int) P.inputs.size())) app.setCrMicInput (c.input, ! P.isCrMic (c.input)); }
        else if (c.name == "preamp") desk::preampKey (P, c.input, c.what);
        else if (c.name == "gain") desk::gainStep (P, c.input, (float) c.delta);
        else if (c.name == "mixout") desk::mixOutStep (P, c.index, (float) c.delta);
        else if (c.name == "mixmute") desk::mixOutMute (P, c.index);
        else if (c.name == "stage") { if (desk::stageStep (P, c.what, (float) c.delta, false)) app.applyStageMix(); }
        else if (c.name == "stagereset") { if (desk::stageStep (P, c.what, 0.0f, true)) app.applyStageMix(); }
    }
    lastSent.clear(); lastDesk.clear();                                    // send the new state straight away
}

remote::State RemoteControlServer::collect() const
{
    remote::State s;
    int number = 1;
    s.recording = app.takeBoxInfo (number);
    s.takeNumber = number;
    s.playing = app.isPlaying();
    s.talkbackReady = app.talkbackReady();
    s.playbackReady = ! app.project.tbOutputs.empty();
    s.crOpen = app.crMicOpen();
    s.pbOn = app.tbPlaybackOn();
    s.preRec = app.sessionMode();
    s.recMode = app.recMode();
    for (int i = 0; i < 2; ++i) s.mixerOpen[i] = app.mixerOpenAt && app.mixerOpenAt (i);
    {
        const int alt = app.project.altMixerIndex();
        s.hasMixer2 = alt >= 0;
        s.mixer2Name = alt >= 0 ? app.project.mixers[(size_t) alt]->name : juce::String ("Alt-Mixer");
    }
    const auto now = juce::Time::currentTimeMillis();
    s.timeOfDay = sessionclock::timeOfDay (now);
    juce::int64 left = 0;
    s.hasSession = app.sessionSecondsLeft (left);
    s.sessionLeft = left;
    s.sessionText = s.hasSession ? sessionclock::formatClock (left) : juce::String();
    return s;
}

juce::String RemoteControlServer::collectDesk()
{
    namespace desk = remote::desk;
    auto& P = app.project;
    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "desk");
    o->setProperty ("inputs", desk::inputsToVar (P));
    o->setProperty ("mixers", desk::mixersToVar (P));
    o->setProperty ("stage", desk::stageToVar (P));
    auto* w = new juce::DynamicObject();                                   // 0 not open, 1 shown, 3 minimised (minimiseall: 3 = there are windows to bring back)
    for (auto* what : { "bridge", "pitchpipe", "click", "take", "minimiseall", "editwin", "takedisplay", "organiser", "media", "mastering" })
        w->setProperty (what, app.deskWindowState ? app.deskWindowState (what) : 0);
    o->setProperty ("win", juce::var (w));
    o->setProperty ("arm", app.armState());                                // 0 none, 1 some, 2 all
    o->setProperty ("wc", app.waveColour);
    o->setProperty ("loop", app.deskLoopState ? app.deskLoopState() : -1);
    auto* wa = new juce::DynamicObject();                                  // -1 not available now (no such window), 0 off, 1 lit
    for (auto* what : remote::kWindowActs) wa->setProperty (what, app.deskActState ? app.deskActState (what) : -1);
    o->setProperty ("wact", juce::var (wa));
    o->setProperty ("rec", app.engine.isRecording());
    return juce::JSON::toString (juce::var (o), true);
}

void RemoteControlServer::peaksUpdated()
{
    if (hub.connections (true) == 0) return;
    if (inAcc.size() != app.inPeaks.size()) inAcc.assign (app.inPeaks.size(), 0.0f);
    for (size_t i = 0; i < inAcc.size(); ++i) inAcc[i] = juce::jmax (inAcc[i], app.inPeaks[i]);
    for (int slot = 0; slot < remote::kMixerSlots; ++slot)
    {
        const int mi = remote::desk::mixerIndexForSlot (app.project, slot);
        auto* b = mi >= 0 ? remote::desk::mainBusOf (app.project, *app.project.mixers[(size_t) mi]) : nullptr;
        if (b == nullptr) continue;
        mixAcc[slot][0] = juce::jmax (mixAcc[slot][0], b->remotePkL.take());
        mixAcc[slot][1] = juce::jmax (mixAcc[slot][1], b->remotePkR.take());
    }
}

juce::String RemoteControlServer::collectMeters()
{
    juce::String s ("{\"type\":\"meters\",\"in\":[");
    const int n = juce::jmin ((int) inAcc.size(), remote::desk::kMaxInputs);
    for (int i = 0; i < n; ++i) { s << (i > 0 ? "," : "") << remote::desk::peakDb (inAcc[(size_t) i]); inAcc[(size_t) i] = 0.0f; }
    s << "],\"mx\":[";
    for (int slot = 0; slot < remote::kMixerSlots; ++slot)
    {
        s << (slot > 0 ? "," : "") << "[" << remote::desk::peakDb (mixAcc[slot][0]) << "," << remote::desk::peakDb (mixAcc[slot][1]) << "]";
        mixAcc[slot][0] = mixAcc[slot][1] = 0.0f;
    }
    s << "]}";
    return s;
}

void RemoteControlServer::timerCallback()
{
    if (hub.connections() == 0) return;
    if (hub.takeNewConnection()) lastSent.clear();                         // a controller has just connected: it gets the state now
    const auto json = collect().toJson();
    if (json != lastSent) { lastSent = json; hub.broadcast (json); }
    if (deskWanted.exchange (false)) { lastDesk.clear(); noDeskSent = false; }   // a controller has just asked: send it everything
    if (hub.connections (true) == 0) return;
    if (! app.hasProject())
    {
        if (! noDeskSent) { noDeskSent = true; lastDesk.clear(); hub.broadcast ("{\"type\":\"nodesk\"}", true); }    // (a project was closed: the controller's keys go back to "open a project")
        return;
    }
    noDeskSent = false;
    const auto desk = collectDesk();
    if (desk != lastDesk) { lastDesk = desk; hub.broadcast (desk, true); }
    hub.broadcast (collectMeters(), true);
}
} // namespace td

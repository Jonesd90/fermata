#include "RemoteControl.h"
#include "../core/SessionClock.h"
#include "../core/RemoteDesk.h"

namespace td
{
struct RemoteControlServer::Client : juce::Thread
{
    Client (RemoteControlServer& o, std::unique_ptr<juce::StreamingSocket> s) : juce::Thread ("Fermata remote client"), owner (o), sock (std::move (s)) { startThread(); }
    ~Client() override { sock->close(); stopThread (2000); }
    void run() override
    {
        juce::String pending;
        char buf[1024];
        while (! threadShouldExit() && sock->isConnected())
        {
            const int ready = sock->waitUntilReady (true, 200);
            if (ready < 0) break;
            if (ready == 0) continue;
            const int n = sock->read (buf, (int) sizeof (buf), false);
            if (n <= 0) break;
            pending += juce::String::fromUTF8 (buf, n);
            if (pending.length() > 65536) pending.clear();                 // never grows without end
            for (int nl = pending.indexOfChar ('\n'); nl >= 0; nl = pending.indexOfChar ('\n'))
            {
                const auto line = pending.substring (0, nl).trim();
                pending = pending.substring (nl + 1);
                if (line.isNotEmpty()) owner.handleLine (*this, line);
            }
        }
        dead = true;
    }
    RemoteControlServer& owner;
    std::unique_ptr<juce::StreamingSocket> sock;
    std::atomic<bool> dead { false };
    std::atomic<bool> desk { false };                                      // wants the "desk" and "meters" messages (the Stream Deck + XL)
};

RemoteControlServer::RemoteControlServer (AppContext& a, int port) : juce::Thread ("Fermata remote"), app (a)
{
    if (port <= 0) return;
    listening = listener.createListener (port, "127.0.0.1");
    if (! listening) return;
    startThread();
    startTimer (50);                                                       // (the state is only sent when it has changed; the meters 20 times a second to whoever asked)
    app.peakListeners.add (this);
}

RemoteControlServer::~RemoteControlServer()
{
    *alive = false;
    stopTimer();
    app.peakListeners.remove (this);
    listener.close();
    stopThread (2000);
    const juce::ScopedLock sl (lock);
    clients.clear();
}

void RemoteControlServer::run()
{
    while (! threadShouldExit())
    {
        if (listener.waitUntilReady (true, 250) <= 0) continue;
        std::unique_ptr<juce::StreamingSocket> s (listener.waitForNextConnection());
        if (s == nullptr) continue;
        const juce::ScopedLock sl (lock);
        for (int i = clients.size(); --i >= 0;) if (clients[i]->dead.load()) clients.remove (i);
        if (clients.size() < 8) clients.add (new Client (*this, std::move (s)));
        lastSent.clear();                                                  // the new controller gets the state at the next tick
    }
}

void RemoteControlServer::handleLine (Client& from, const juce::String& line)
{
    const auto cmd = remote::parseCommand (line);
    if (cmd.name.isEmpty()) return;
    if (cmd.name == "desk")                                                // (answered here: it only sets a flag of this connection)
    {
        from.desk = cmd.on;
        deskWanted = true;
        return;
    }
    juce::MessageManager::callAsync ([this, cmd, flag = alive] { if (*flag) execute (cmd); });
}

MixerState* RemoteControlServer::panMixer() const
{
    auto& P = app.project;
    if (P.mixers.empty()) return nullptr;
    if (app.frontMixerId)
    {
        const auto id = app.frontMixerId();
        for (auto& m : P.mixers) if (m->id == id) return m.get();
    }
    return P.mixers.front().get();
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
    else if (c.name == "hello") {}
    else if (app.hasProject())                                             // the Stream Deck + XL
    {
        if (c.name == "bridge" || c.name == "pitchpipe" || c.name == "click" || c.name == "minimiseall") { if (app.deskWindowToggle) app.deskWindowToggle (c.name); }
        else if (c.name == "takewindow") { if (app.deskWindowToggle) app.deskWindowToggle ("take"); }
        else if (c.name == "armtoggle") app.remoteArmToggle();
        else if (c.name == "wavecolour") app.remoteWaveColour();
        else if (c.name == "loop") { if (app.deskLoopToggle) app.deskLoopToggle(); }
        else if (c.name == "monitor") desk::monitorCycle (P, c.input);
        else if (c.name == "talkin") { if (juce::isPositiveAndBelow (c.input, (int) P.inputs.size())) app.setCrMicInput (c.input, ! P.isCrMic (c.input)); }
        else if (c.name == "preamp") desk::preampKey (P, c.input, c.what);
        else if (c.name == "gain") desk::gainStep (P, c.input, (float) c.delta);
        else if (c.name == "pan") { if (auto* m = panMixer()) desk::panStep (P, *m, c.input, c.side, (float) c.delta, c.centre); }
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
    auto* pm = panMixer();
    o->setProperty ("panMixer", pm != nullptr ? pm->name : juce::String());
    o->setProperty ("inputs", desk::inputsToVar (P, pm));
    o->setProperty ("mixers", desk::mixersToVar (P));
    o->setProperty ("stage", desk::stageToVar (P));
    auto* w = new juce::DynamicObject();                                   // 0 not open, 1 shown, 3 minimised (minimiseall: 3 = there are windows to bring back)
    for (auto* what : { "bridge", "pitchpipe", "click", "take", "minimiseall" })
        w->setProperty (what, app.deskWindowState ? app.deskWindowState (what) : 0);
    o->setProperty ("win", juce::var (w));
    o->setProperty ("arm", app.armState());                                // 0 none, 1 some, 2 all
    o->setProperty ("wc", app.waveColour);
    o->setProperty ("loop", app.deskLoopState ? app.deskLoopState() : -1);
    o->setProperty ("rec", app.engine.isRecording());
    return juce::JSON::toString (juce::var (o), true);
}

void RemoteControlServer::peaksUpdated()
{
    if (deskClients() == 0) return;
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

int RemoteControlServer::deskClients() const
{
    const juce::ScopedLock sl (lock);
    int n = 0;
    for (auto* c : clients) if (! c->dead.load() && c->desk.load()) ++n;
    return n;
}

void RemoteControlServer::broadcast (const juce::String& json, bool deskOnly)
{
    const auto data = (json.replaceCharacters ("\r\n", "  ") + "\n").toUTF8();
    const juce::ScopedLock sl (lock);
    for (auto* c : clients)
        if (! c->dead.load() && c->sock->isConnected() && (! deskOnly || c->desk.load()))
            if (c->sock->write (data.getAddress(), (int) data.sizeInBytes() - 1) < 0) c->dead = true;
}

void RemoteControlServer::timerCallback()
{
    {
        const juce::ScopedLock sl (lock);
        if (clients.isEmpty()) return;
    }
    const auto json = collect().toJson();
    if (json != lastSent) { lastSent = json; broadcast (json); }
    if (deskWanted.exchange (false)) { lastDesk.clear(); noDeskSent = false; }   // a controller has just asked: send it everything
    if (deskClients() == 0) return;
    if (! app.hasProject())
    {
        if (! noDeskSent) { noDeskSent = true; lastDesk.clear(); broadcast ("{\"type\":\"nodesk\"}", true); }    // (a project was closed: the controller's keys go back to "open a project")
        return;
    }
    noDeskSent = false;
    const auto desk = collectDesk();
    if (desk != lastDesk) { lastDesk = desk; broadcast (desk, true); }
    broadcast (collectMeters(), true);
}
} // namespace td

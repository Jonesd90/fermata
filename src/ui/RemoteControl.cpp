#include "RemoteControl.h"
#include "../core/SessionClock.h"

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
                if (line.isNotEmpty()) owner.handleLine (line);
            }
        }
        dead = true;
    }
    RemoteControlServer& owner;
    std::unique_ptr<juce::StreamingSocket> sock;
    std::atomic<bool> dead { false };
};

RemoteControlServer::RemoteControlServer (AppContext& a, int port) : juce::Thread ("Fermata remote"), app (a), portNumber (port)
{
    if (port <= 0) return;
    listening = listener.createListener (port, "127.0.0.1");
    if (! listening) return;
    startThread();
    startTimer (200);
}

RemoteControlServer::~RemoteControlServer()
{
    *alive = false;
    stopTimer();
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

void RemoteControlServer::handleLine (const juce::String& line)
{
    const auto cmd = remote::parseCommand (line);
    if (cmd.name.isEmpty()) return;
    juce::MessageManager::callAsync ([this, cmd, flag = alive] { if (*flag) execute (cmd); });
}

void RemoteControlServer::execute (const remote::Command& c)
{
    if (c.name == "record") app.remoteRecord();
    else if (c.name == "playpause") app.remotePlayPause();
    else if (c.name == "talk") app.remoteTalk (c.down);
    else if (c.name == "playback") app.remotePlayback();
    else if (c.name == "mixer") app.remoteMixer (c.index);
    else if (c.name == "prerec") app.setSessionMode (! app.sessionMode());
    lastSent.clear();                                                      // send the new state straight away
}

remote::State RemoteControlServer::collect() const
{
    remote::State s;
    int number = 1;
    s.recording = app.takeBoxInfo (number);
    s.takeNumber = number;
    s.playing = app.isPlaying();
    s.talkbackReady = app.talkbackReady();
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

void RemoteControlServer::broadcast (const juce::String& json)
{
    const auto data = (json.replaceCharacters ("\r\n", "  ") + "\n").toUTF8();
    const juce::ScopedLock sl (lock);
    for (auto* c : clients)
        if (! c->dead.load() && c->sock->isConnected())
            if (c->sock->write (data.getAddress(), (int) data.sizeInBytes() - 1) < 0) c->dead = true;
}

void RemoteControlServer::timerCallback()
{
    {
        const juce::ScopedLock sl (lock);
        if (clients.isEmpty()) return;
    }
    const auto json = collect().toJson();
    if (json == lastSent) return;
    lastSent = json;
    broadcast (json);
}
} // namespace td

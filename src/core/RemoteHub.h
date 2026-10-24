#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace td::remote
{
/** The network side of the control port (see RemoteProtocol.h for the messages): a listening socket on 127.0.0.1 and one thread per controller that does
    all of that controller's reading AND writing. The rest of Fermata never waits for the network: broadcast() only puts the bytes in each controller's
    queue. A controller that stops reading is disconnected once more than kMaxQueued bytes wait for it (the Stream Deck plug-in connects again by itself and
    is then sent everything afresh). It knows nothing of the messages themselves: every line that arrives goes to the LineHandler, on that controller's thread.

    Why it is built this way (2.98.0): the messages used to be written straight to the sockets from the message thread. The bytes were taken from a
    temporary juce::String that had already been destroyed (the crash in RemoteControlServer::broadcast), a controller that stopped reading could hold the
    whole program up, a partly written message was never finished, and a connection that failed was never closed. */
class Hub : private juce::Thread
{
public:
    static constexpr int kMaxConnections = 8;
    static constexpr size_t kMaxQueued = (size_t) 4 << 20;          // 4 MB waiting for one controller: it has not read for many seconds
    static constexpr size_t kMaxLine = 65536;                       // a longer line from a controller is thrown away

    /** One connected controller. Only its own thread touches its socket. */
    class Connection : private juce::Thread
    {
    public:
        Connection (Hub& h, std::unique_ptr<juce::StreamingSocket> s)
            : juce::Thread ("Fermata control port client"), hub (h), sock (std::move (s)) { startThread(); }
        ~Connection() override
        {
            stopThread (4000);                                       // (it looks at its exit flag at least every 10 ms)
            sock->close();
        }

        /** The controller asked for (or stopped) the "desk" and "meters" messages (the Stream Deck + XL). */
        void setWantsDesk (bool on) noexcept    { desk = on; }
        bool wantsDesk() const noexcept         { return desk.load(); }
        /** The connection has ended (closed by the controller, an error, or it stopped reading); the hub deletes it soon. */
        bool isFinished() const noexcept        { return finished.load(); }

        /** Adds bytes to send. Any thread; never waits for the network. False = the controller has stopped reading: it is closed. */
        bool queue (const std::string& bytes)
        {
            if (finished.load()) return false;
            const juce::ScopedLock sl (outLock);
            if (outbox.size() + bytes.size() > kMaxQueued) { overflow = true; return false; }
            outbox += bytes;
            return true;
        }

    private:
        void run() override
        {
            char probe = 0;
            sock->read (&probe, 0, false);     // (JUCE makes a socket non-blocking when asked to read without waiting: from now on no send or receive can hold this thread)
            std::string in, out;
            size_t sent = 0;
            char buf[4096];
            juce::String why ("closed by the controller");
            while (! threadShouldExit())
            {
                if (overflow.load()) { why = "it stopped reading (more than 4 MB waiting)"; break; }
                if (! sock->isConnected()) break;
                if (sent >= out.size())                              // everything handed over: take what has been queued since
                {
                    out.clear(); sent = 0;
                    const juce::ScopedLock sl (outLock);
                    out.swap (outbox);
                }
                int readable = 0;
                if (sent < out.size())
                {
                    const int w = sock->waitUntilReady (false, 5);   // room to send? (waits at most 5 ms)
                    if (w < 0) { why = "network error"; break; }
                    if (w > 0)
                    {
                        const int n = sock->write (out.data() + sent, (int) juce::jmin ((size_t) 65536, out.size() - sent));
                        if (n < 0) { why = "network error"; break; }
                        sent += (size_t) n;                          // (a part is fine: the rest goes next time round)
                    }
                    readable = sock->waitUntilReady (true, 0);
                }
                else readable = sock->waitUntilReady (true, 10);
                if (readable < 0) { why = "network error"; break; }
                if (readable == 0) continue;
                const int n = sock->read (buf, (int) sizeof (buf), false);
                if (n <= 0) break;                                   // the controller closed the connection
                in.append (buf, (size_t) n);
                size_t from = 0, nl;
                while ((nl = in.find ('\n', from)) != std::string::npos)
                {
                    const auto line = juce::String::fromUTF8 (in.data() + from, (int) (nl - from)).trim();
                    from = nl + 1;
                    if (line.isNotEmpty() && hub.onLine != nullptr) hub.onLine (*this, line);
                }
                in.erase (0, from);
                if (in.size() > kMaxLine) in.clear();
            }
            finished = true;
            sock->close();                                           // the controller sees the end at once (and connects again)
            if (! threadShouldExit() && hub.log != nullptr) hub.log ("Control port: a controller disconnected (" + why + ")");
        }

        Hub& hub;
        std::unique_ptr<juce::StreamingSocket> sock;
        juce::CriticalSection outLock;
        std::string outbox;                                          // whole lines, waiting to be sent
        std::atomic<bool> desk { false }, finished { false }, overflow { false };
    };

    /** Called with every line a controller sends, on that controller's own thread. It must not call back into the hub (except Connection::setWantsDesk). */
    using LineHandler = std::function<void (Connection&, const juce::String& line)>;

    explicit Hub (LineHandler handler) : juce::Thread ("Fermata control port"), onLine (std::move (handler)) {}
    ~Hub() override { stop(); }

    /** Where the hub reports connections and disconnections (Fermata's log). Set before start(). */
    std::function<void (const juce::String&)> log;

    /** Starts listening on 127.0.0.1:port. False when the port cannot be opened (another program, or another Fermata, has it). */
    bool start (int port)
    {
        stop();
       #if ! JUCE_WINDOWS
        std::signal (SIGPIPE, SIG_IGN);                              // (a controller that has gone must give a write error, not end the program)
       #endif
        if (port <= 0 || port > 65535 || ! listener.createListener (port, "127.0.0.1")) return false;
        listening = true;
        startThread();
        return true;
    }
    /** Closes everything: the listening socket and every connection (their threads are stopped). */
    void stop()
    {
        stopThread (4000);                                           // (the accept thread looks at its exit flag every 200 ms)
        if (listening) { listener.close(); listening = false; }
        std::vector<std::unique_ptr<Connection>> gone;
        {
            const juce::ScopedLock sl (lock);
            while (! conns.isEmpty()) gone.emplace_back (conns.removeAndReturn (conns.size() - 1));
        }
        gone.clear();                                                // (outside the lock: each one waits for its thread)
    }
    bool isListening() const noexcept { return listening; }

    /** Sends one line (JSON, no newline needed) to every controller, or only to those that asked for the desk messages. Any thread; never waits for the network. */
    void broadcast (const juce::String& line, bool onlyDesk = false)
    {
        std::string bytes = line.toStdString();                     // our own copy, alive for as long as it is needed
        for (auto& ch : bytes) if (ch == '\n' || ch == '\r') ch = ' ';
        bytes += '\n';
        const juce::ScopedLock sl (lock);
        for (auto* c : conns)
            if (! onlyDesk || c->wantsDesk()) c->queue (bytes);
    }
    /** How many controllers are connected (or only those that asked for the desk messages). */
    int connections (bool onlyDesk = false) const
    {
        const juce::ScopedLock sl (lock);
        int n = 0;
        for (auto* c : conns) if (! c->isFinished() && (! onlyDesk || c->wantsDesk())) ++n;
        return n;
    }
    /** True once after a new controller has connected (so that it is sent the state at once). */
    bool takeNewConnection() noexcept { return newConnection.exchange (false); }

private:
    void run() override                                              // accepts connections and deletes the ones that have ended
    {
        while (! threadShouldExit())
        {
            sweep();
            const int r = listener.waitUntilReady (true, 200);
            if (r < 0) { wait (200); continue; }
            if (r == 0) continue;
            std::unique_ptr<juce::StreamingSocket> s (listener.waitForNextConnection());
            if (s == nullptr) continue;
            int count = 0;
            {
                const juce::ScopedLock sl (lock);
                if (conns.size() >= kMaxConnections) continue;      // (too many: this one is closed again)
                conns.add (new Connection (*this, std::move (s)));
                count = conns.size();
            }
            newConnection = true;
            if (log != nullptr) log ("Control port: a controller connected (" + juce::String (count) + (count == 1 ? " connection)" : " connections)"));
        }
    }
    void sweep()
    {
        std::vector<std::unique_ptr<Connection>> gone;
        const juce::ScopedLock sl (lock);
        for (int i = conns.size(); --i >= 0;)
            if (conns.getUnchecked (i)->isFinished()) gone.emplace_back (conns.removeAndReturn (i));
        // (the ended ones are deleted when 'gone' goes, after the lock: their threads have already finished)
    }

    LineHandler onLine;
    juce::StreamingSocket listener;
    bool listening = false;
    mutable juce::CriticalSection lock;
    juce::OwnedArray<Connection> conns;
    std::atomic<bool> newConnection { false };
};
} // namespace td::remote

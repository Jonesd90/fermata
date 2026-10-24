// Headless test of the control port's connections (src/core/RemoteHub.h): lines in, lines out, long lines, a controller that stops reading,
// too many controllers, stopping with controllers connected, and many threads at once (build it with -fsanitize=thread / address to check the locking).
#include "../src/core/RemoteHub.h"
#include <cstdio>
#include <thread>

using namespace td;
static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define SECTION(name) std::printf ("-- %s\n", name)

namespace
{
template <typename F> bool waitFor (F cond, int ms = 4000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (! cond()) { if (juce::Time::getMillisecondCounter() > end) return false; juce::Thread::sleep (2); }
    return true;
}

/** A pretend controller: a plain socket to the hub. */
struct Client
{
    juce::StreamingSocket s;
    std::string got;
    bool connect (int port) { return s.connect ("127.0.0.1", port, 2000); }
    void send (const std::string& t) { s.write (t.data(), (int) t.size()); }
    /** Reads whatever has arrived, waiting up to ms for the first bytes. False = the connection has ended. */
    bool pump (int ms = 0)
    {
        char buf[65536];
        for (;;)
        {
            const int r = s.waitUntilReady (true, ms);
            if (r < 0) return false;
            if (r == 0) return true;
            const int n = s.read (buf, (int) sizeof (buf), false);
            if (n <= 0) return false;
            got.append (buf, (size_t) n);
            ms = 0;
        }
    }
    size_t lines() const { size_t n = 0; for (char c : got) if (c == '\n') ++n; return n; }
    bool readLines (size_t n, int ms = 4000) { return waitFor ([&] { pump (5); return lines() >= n; }, ms); }
    /** Waits until the hub has closed the connection (reading and throwing away whatever is still on the way). */
    bool closedByHub (int ms = 6000) { return waitFor ([&] { const bool open = pump (5); got.clear(); return ! open; }, ms); }
};

int startOnFreePort (remote::Hub& hub)
{
    for (int p = 47911; p < 48111; ++p) if (hub.start (p)) return p;
    return 0;
}
}

int main()
{
    SECTION ("lines in: split across reads, CR LF, blank lines; the desk flag; a new connection is reported once");
    {
        juce::CriticalSection gotLock; juce::StringArray received;
        remote::Hub hub ([&] (remote::Hub::Connection& c, const juce::String& line)
        {
            const juce::ScopedLock sl (gotLock);
            received.add (line);
            if (line == "desk") c.setWantsDesk (true);
        });
        const int port = startOnFreePort (hub);
        CHECK (port != 0 && hub.isListening());
        CHECK (hub.connections() == 0 && ! hub.takeNewConnection());
        Client a; CHECK (a.connect (port));
        CHECK (waitFor ([&] { return hub.connections() == 1; }));
        CHECK (hub.takeNewConnection() && ! hub.takeNewConnection());
        a.send ("hello\r\n  \ndesk\npart"); juce::Thread::sleep (30); a.send ("ial\n");
        CHECK (waitFor ([&] { const juce::ScopedLock sl (gotLock); return received.size() >= 3; }));
        { const juce::ScopedLock sl (gotLock); CHECK (received.size() == 3 && received[0] == "hello" && received[1] == "desk" && received[2] == "partial"); }
        CHECK (waitFor ([&] { return hub.connections (true) == 1; }));

        SECTION ("lines out: to everyone, or only to those that asked for the desk messages; newlines inside a message never split it");
        Client b; CHECK (b.connect (port));
        CHECK (waitFor ([&] { return hub.connections() == 2; }));
        hub.broadcast ("one"); hub.broadcast ("two\nlines\r"); hub.broadcast ("desk only", true);
        CHECK (a.readLines (3) && a.got == "one\ntwo lines \ndesk only\n");
        CHECK (b.readLines (2)); juce::Thread::sleep (60); b.pump();
        CHECK (b.got == "one\ntwo lines \n");                                   // b never asked for the desk messages

        SECTION ("a long message arrives whole and in order (sent in parts)");
        std::string big (300000, 'x');
        for (size_t i = 0; i < big.size(); i += 997) big[i] = (char) ('a' + (int) ((i / 997) % 26));
        a.got.clear(); b.got.clear();
        hub.broadcast (juce::String (big)); hub.broadcast ("after");
        CHECK (a.readLines (2, 8000) && a.got == big + "\nafter\n");
        CHECK (b.readLines (2, 8000) && b.got == big + "\nafter\n");

        SECTION ("a controller that closes is gone at once");
        b.s.close();
        CHECK (waitFor ([&] { return hub.connections() == 1; }));
        hub.broadcast ("still here");
        a.got.clear(); CHECK (a.readLines (1) && a.got == "still here\n");
    }

    SECTION ("a controller that stops reading never holds Fermata up, and is let go once too much waits for it");
    {
        remote::Hub hub ([] (remote::Hub::Connection&, const juce::String&) {});
        const int port = startOnFreePort (hub);
        CHECK (port != 0);
        Client deaf; CHECK (deaf.connect (port));
        CHECK (waitFor ([&] { return hub.connections() == 1; }));
        const std::string chunk (65535, 'm');
        const juce::String line (chunk);
        double worst = 0.0;
        for (int i = 0; i < 200 && hub.connections() > 0; ++i)               // 13 MB: far more than the queue and the sockets' buffers hold
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            hub.broadcast (line);
            worst = juce::jmax (worst, juce::Time::getMillisecondCounterHiRes() - t0);
            juce::Thread::sleep (1);
        }
        CHECK (worst < 250.0);                                                  // (each call only queues the bytes)
        CHECK (waitFor ([&] { return hub.connections() == 0; }, 8000));
        CHECK (deaf.closedByHub());
        Client again; CHECK (again.connect (port));                            // it can connect again and is served as before
        CHECK (waitFor ([&] { return hub.connections() == 1; }));
        hub.broadcast ("fresh"); CHECK (again.readLines (1) && again.got == "fresh\n");
    }

    SECTION ("at most eight controllers; stopping closes them all promptly; the port can be opened again");
    {
        remote::Hub hub ([] (remote::Hub::Connection&, const juce::String&) {});
        const int port = startOnFreePort (hub);
        CHECK (port != 0);
        std::vector<std::unique_ptr<Client>> cs;
        for (int i = 0; i < remote::Hub::kMaxConnections; ++i) { cs.push_back (std::make_unique<Client>()); CHECK (cs.back()->connect (port)); }
        CHECK (waitFor ([&] { return hub.connections() == remote::Hub::kMaxConnections; }));
        Client extra; CHECK (extra.connect (port));                            // (the connection is accepted by the system, then closed by the hub)
        CHECK (extra.closedByHub());
        CHECK (hub.connections() == remote::Hub::kMaxConnections);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        hub.stop();
        CHECK (juce::Time::getMillisecondCounterHiRes() - t0 < 2000.0);
        CHECK (! hub.isListening() && hub.connections() == 0);
        bool allClosed = true; for (auto& c : cs) allClosed = c->closedByHub() && allClosed;
        CHECK (allClosed);
        CHECK (hub.start (port) && hub.isListening());
        Client after; CHECK (after.connect (port));
        CHECK (waitFor ([&] { return hub.connections() == 1; }));
    }

    SECTION ("many threads at once: broadcasting from three threads while controllers come and go");
    {
        std::atomic<int> handled { 0 };
        remote::Hub hub ([&] (remote::Hub::Connection& c, const juce::String& line) { if (line == "desk") c.setWantsDesk (true); ++handled; });
        const int port = startOnFreePort (hub);
        CHECK (port != 0);
        std::atomic<bool> stop { false };
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t)
            threads.emplace_back ([&, t]
            {
                int n = 0;
                while (! stop.load())
                {
                    ++n;
                    hub.broadcast ("{\"type\":\"meters\",\"n\":" + juce::String (n) + "}", (n + t) % 2 == 0);
                    (void) hub.connections ((n % 3) == 0);
                    juce::Thread::sleep (1);
                }
            });
        std::atomic<int> sessions { 0 };
        for (int t = 0; t < 4; ++t)
            threads.emplace_back ([&]
            {
                while (! stop.load())
                {
                    Client c;
                    if (! c.connect (port)) { juce::Thread::sleep (5); continue; }
                    c.send ("hello\ndesk\n");
                    c.pump (20);
                    c.send ("{\"cmd\":\"x\"}\n");
                    c.pump (10);
                    ++sessions;
                }
            });
        juce::Thread::sleep (1500);
        stop = true;
        for (auto& th : threads) th.join();
        CHECK (sessions.load() > 10 && handled.load() > 10);
        hub.stop();
        CHECK (hub.connections() == 0);
    }

    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

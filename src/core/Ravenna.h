#pragma once
#include "Preamp.h"
#include <atomic>
#include <mutex>

namespace td
{
/** A minimal WebSocket client (ws://, no compression, text messages only): just enough to talk to the web interface of a Merging device. */
class WebSocketClient
{
public:
    ~WebSocketClient() { close(); }
    bool connect (const juce::String& host, int port, const juce::String& path, const juce::String& origin, int timeoutMs, juce::String& error);
    bool sendText (const juce::String& text);
    /** 1 = a complete text message was read into 'out', 0 = nothing arrived within the time, -1 = the connection is gone. */
    int receive (juce::String& out, int timeoutMs);
    void close();
    /** Safe from another thread: makes any read in progress fail quickly. The owning thread then closes the socket itself. */
    void interrupt();
    bool isOpen() const { return socket != nullptr; }

    /** Builds one masked client frame (exposed for the tests). */
    static juce::MemoryBlock makeFrame (int opcode, const void* data, size_t size, const juce::uint8 mask[4]);

private:
    bool readExact (void* dst, size_t n, int timeoutMs);
    bool writeAll (const void* src, size_t n);
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection writeLock, sockLock;
};

/** One channel of a device's preamps as the hardware reports it. */
struct HwPreamp
{
    int          moduleId = 0, index = 0;
    juce::String name;                    // e.g. "Combo 1/2"
    bool         m48V = false, pad = false, lowCut = false, phase = false, cut = false, line = false, lift = false, zIn = false;
    int          micGain = 0, lineGain = 0;   // tenths of a dB (660 = 66.0 dB)
};

/** A Merging device (Anubis, Hapi, MT48) reached through its web interface: CometD over a WebSocket, at ws://<ip>/cometd/handshake.
    It connects in the background, reads what preamps the device has and their present settings, and sends changes.
    Nothing is ever pushed to the hardware on connect: the settings you see are the ones the hardware has. */
class RavennaDevice : private juce::Thread
{
public:
    RavennaDevice (juce::String name, juce::String host, int port = 80);
    ~RavennaDevice() override { stop(); }
    void start() { startThread(); }
    void stop();                                   // waits for the thread to finish
    void requestStop() { signalThreadShouldExit(); ws.interrupt(); }     // returns at once

    const juce::String& getName() const { return deviceName; }
    bool isConnected() const { return connected.load(); }
    juce::String getStatus() const;
    int numChannels() const;
    bool getChannel (int i, HwPreamp& out) const;
    /** Changes some fields of one channel ('fields' is a JSON object such as {"m48V":true}); false if there is no such channel or no connection. */
    bool sendChannelFields (int i, const juce::var& fields);
    /** Called on the device's thread whenever the list of channels or any setting changed. */
    std::function<void()> onUpdate;

    // parsing and path helpers, exposed for the tests
    static HwPreamp readChannel (const juce::var& module, int index);
    static bool applyPath (juce::var& module, const juce::String& relativePath, const juce::var& value);

private:
    void run() override;
    void session();
    void handleMessage (const juce::var& m);
    void handleSettings (const juce::var& data);
    void rebuildChannelList();
    void sendRaw (const juce::String& json);
    juce::String nextId() { return juce::String (idCounter.fetch_add (1) + 1); }

    juce::String deviceName, hostName; int portNumber;
    std::atomic<bool> connected { false };
    mutable juce::CriticalSection lock;
    juce::String status { "Starting" };
    std::map<int, juce::var> modules;                  // id -> module, from the settings dump
    struct Ref { int moduleId = 0, index = 0; };
    std::vector<Ref> refs;
    juce::StringArray outgoing;
    WebSocketClient ws;
    juce::String clientId;
    std::atomic<int> idCounter { 10 };
    bool needConnect = false; juce::int64 lastConnectMs = 0;
};

/** What ANEMAN (Merging's network manager, on this PC at port 6167) says about the network: which devices exist, their addresses, and
    which of their inputs are patched to which ASIO input. From that, the preamp devices and their 'first input' can be set up without typing. */
struct AnemanPlan
{
    bool ok = false;
    juce::String error;
    std::vector<PreampDeviceCfg> devices;     // ordered by first input
    juce::StringArray summary;                // one readable line per device and for the outputs
    juce::StringArray warnings;               // things that look odd (a gap or a different order in the patch)
    juce::String source;                      // the address ANEMAN answered on
};

/** Works out the plan from the two lists ANEMAN returns for {"Path":"$.Devices"} and {"Path":"$.Connections"}. No network access. */
AnemanPlan planFromAneman (const juce::var& devices, const juce::var& connections);
/** Asks ANEMAN (POST /get). Tries 'preferred' first (if not empty), then this PC (127.0.0.1 and its own addresses) on port 6167. Blocks: call from a background thread. */
AnemanPlan fetchAnemanPlan (const juce::String& preferred = {});

/** The PreampDriver for Merging devices. Driver input N goes to the device whose range [firstInput, firstInput + channels) contains it. */
class RavennaPreampDriver : public PreampDriver
{
public:
    explicit RavennaPreampDriver (std::vector<PreampDeviceCfg> devices);
    ~RavennaPreampDriver() override;
    juce::String getName() const override;
    bool isConnected() const override;
    bool apply (int inputIndex, const PreampSettings&) override;

    struct Info { juce::String name, host, status; bool connected = false; int channels = 0; int firstInput = 0; int patched = 0; };   // channels = with a preamp the device lets us control; patched = inputs ANEMAN sends to the ASIO device
    std::vector<Info> info() const;
    /** What the hardware has for a driver input; false if no device covers it. */
    bool read (int inputIndex, PreampSettings& out) const;
    /** Called on the message thread when the hardware's settings for an input changed (or were first read). */
    std::function<void (int inputIndex, const PreampSettings&)> onHardware;

    static PreampSettings toSettings (const HwPreamp&);

private:
    struct Dev { PreampDeviceCfg cfg; std::unique_ptr<RavennaDevice> dev; };
    bool locate (int inputIndex, Dev*& d, int& channel) const;
    void hardwareChanged();
    std::vector<std::unique_ptr<Dev>> devs;
    std::map<int, PreampSettings> lastReported;       // message thread only
    std::map<int, double> lastSentMs;                 // message thread only: when this program last changed an input (the device's answers in the next moment are not trusted)
    bool recheckPending = false;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    struct Link { std::mutex m; RavennaPreampDriver* owner = nullptr; };       // lets a device thread call back safely while this object is being destroyed
    std::shared_ptr<Link> link = std::make_shared<Link>();
};
} // namespace td

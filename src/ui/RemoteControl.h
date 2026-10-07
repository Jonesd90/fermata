#pragma once
#include "AppContext.h"
#include "../core/RemoteProtocol.h"

namespace td
{
/** The control port: lets a Stream Deck (or anything on this computer) press Fermata's main buttons and see their state.
    Listens on 127.0.0.1 only. See core/RemoteProtocol.h for the messages. */
class RemoteControlServer : private juce::Thread, private juce::Timer
{
public:
    RemoteControlServer (AppContext&, int port);
    ~RemoteControlServer() override;
    bool isListening() const noexcept { return listening; }
    int  getPort() const noexcept { return portNumber; }

private:
    struct Client;
    void run() override;                          // accepts connections
    void timerCallback() override;                // pushes the state when it changes
    void handleLine (const juce::String& line);   // called on a client's thread
    void execute (const remote::Command&);        // on the message thread
    remote::State collect() const;
    void broadcast (const juce::String& json);

    AppContext& app;
    int portNumber = 0;
    bool listening = false;
    juce::StreamingSocket listener;
    juce::CriticalSection lock;
    juce::OwnedArray<Client> clients;
    juce::String lastSent;
    int sinceLast = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};
} // namespace td

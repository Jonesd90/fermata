#pragma once
#include "AppContext.h"
#include "../core/RemoteProtocol.h"

namespace td
{
/** The control port: lets a Stream Deck (or anything on this computer) press Fermata's main buttons and see their state.
    Listens on 127.0.0.1 only. See core/RemoteProtocol.h for the messages and core/RemoteDesk.h for the Stream Deck + XL's part. */
class RemoteControlServer : private juce::Thread, private juce::Timer, private PeakListener
{
public:
    RemoteControlServer (AppContext&, int port);
    ~RemoteControlServer() override;

private:
    struct Client;
    void run() override;                          // accepts connections
    void timerCallback() override;                // pushes the state when it changes, and the meters to the Stream Deck + XL
    void peaksUpdated() override;                 // 30 times a second: the peaks the meters message carries
    void handleLine (Client&, const juce::String& line);   // called on a client's thread
    void execute (const remote::Command&);        // on the message thread
    remote::State collect() const;
    juce::String collectDesk();                   // the "desk" message: inputs, mixer outputs, stage levels, windows
    juce::String collectMeters();                 // the "meters" message (and starts the next peak period)
    MixerState* panMixer() const;                 // the mixer the pan dial works in: the one in front (as the control surface), else the processing mixer
    void broadcast (const juce::String& json, bool deskOnly = false);
    int deskClients() const;

    AppContext& app;
    bool listening = false;
    juce::StreamingSocket listener;
    juce::CriticalSection lock;
    juce::OwnedArray<Client> clients;
    juce::String lastSent, lastDesk;
    bool noDeskSent = false;                      // "nodesk" has been sent for the present time without a project
    std::atomic<bool> deskWanted { false };       // a client has just asked for the desk messages: send them at the next tick
    std::vector<float> inAcc;                     // the biggest peak of each input since the last meters message
    float mixAcc[remote::kMixerSlots][2] = {};    // the same for the main outputs of the processing mixer and cue mixers 1 - 5
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};
} // namespace td

#pragma once
#include "AppContext.h"
#include "../core/RemoteProtocol.h"
#include "../core/RemoteHub.h"

namespace td
{
/** The control port: lets a Stream Deck (or anything on this computer) press Fermata's main buttons and see their state.
    Listens on 127.0.0.1 only. The network side is remote::Hub (core/RemoteHub.h: one thread per controller, nothing here ever waits for a socket);
    core/RemoteProtocol.h has the messages and core/RemoteDesk.h the Stream Deck + XL's part. */
class RemoteControlServer : private juce::Timer, private PeakListener
{
public:
    RemoteControlServer (AppContext&, int port);
    ~RemoteControlServer() override;

private:
    void timerCallback() override;                // pushes the state when it changes, and the meters to the Stream Deck + XL
    void peaksUpdated() override;                 // 30 times a second: the peaks the meters message carries
    void handleLine (remote::Hub::Connection&, const juce::String& line);   // called on that controller's own thread
    void execute (const remote::Command&);        // on the message thread
    remote::State collect() const;
    juce::String collectDesk();                   // the "desk" message: inputs, mixer outputs, stage levels, windows
    juce::String collectMeters();                 // the "meters" message (and starts the next peak period)

    AppContext& app;
    std::atomic<bool> deskWanted { false };       // a controller has just asked for the desk messages: send them everything at the next tick
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // (message thread only: the commands queued by the controllers' threads look at it)
    juce::String lastSent, lastDesk;              // (message thread only)
    bool noDeskSent = false;                      // "nodesk" has been sent for the present time without a project
    std::vector<float> inAcc;                     // the biggest peak of each input since the last meters message
    float mixAcc[remote::kMixerSlots][2] = {};    // the same for the main outputs of the processing mixer and cue mixers 1 - 5
    remote::Hub hub;                              // (last: it goes first, so no controller's thread outlives what handleLine uses)
};
} // namespace td

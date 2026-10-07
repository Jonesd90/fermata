#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
class TakeTimeline;

/** A 'Take window': every take for one piece, laid out left to right in recording order.
    Click the ruler to place the playhead (R records, Space stops / plays), mark IN and OUT, then send the marked part to the edit. */
class TakeWindowComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::ChangeListener, private juce::Timer
{
public:
    TakeWindowComponent (AppContext&, const juce::Uuid& windowId);
    ~TakeWindowComponent() override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    // drag audio files (or a folder) from another program's session onto this window to import them as takes
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    TakeWindowDef* def() const { return app.project.findTakeWindow (windowId); }

    void markIn();
    void markOut();
    void markWholeSelected();
    void editMarkIn();
    void editMarkOut();
    void toEdit (bool atEditPlayhead = false);
    void sendToEdit (bool atEditPlayhead);
    void togglePlay();
    void toggleBarSearch();                                   // "Search for bar": asks for a bar number, then frames every take here that contains it
    void setBarSearch (int bar);
    int searchBar = 0;
    void playMarked();
    void loopChanged();                  // the Loop button / key L
    bool markedRange (juce::Uuid& take, double& in, double& out) const;
    void openEditWindow();
    void openBounce();
    void nudgeCursor (double seconds);
    // ---- transport: Start, Back 5 s, Play / Stop, Forward 5 s, End (of the last take); they also work while playing, which then carries on from the new place
    double transportNow() const;
    void goTo (double seconds);
    void jumpBy (double seconds) { goTo (transportNow() + seconds); }
    void goToEnd();
    bool hereRecording() const { return app.engine.isRecording() && def() != nullptr && def()->findGroup (app.engine.currentTakeId()) != nullptr; }
    bool wasRecordingHere = false; double recStart = 0.0;

    AppContext& app;
    juce::Uuid windowId;
    juce::Label nameLabel, timeLabel, statusLabel, markLabel;
    juce::TextEditor nameEditor;
    NextTakeBox takeBox;
    juce::Label sendCaption;
    juce::ComboBox sendBox;
    void fillSendBox();
    bool updatingSendBox = false;
    juce::TextButton armAllButton { "Arm all" }, armNoneButton { "Arm none" };
    juce::TextButton recordButton { "REC  [R]" }, zoomInButton { "+" }, zoomOutButton { "-" },
                     playButton { "Play [Space]" }, startButton { "|<" }, backButton { "<<" }, fwdButton { ">>" }, endTransportButton { ">|" }, playMarkedButton { "Play marked" }, inButton { "Bounce IN [I]" },
                     outButton { "Bounce OUT [O]" }, editInButton { "Edit IN [1]" }, editOutButton { "Edit OUT [2]" },
                     toEditButton { "To edit [3]" }, toEditAtButton { "To playhead [4]" }, editWindowButton { "Edit window" }, bounceButton { "Bounce Out..." },
                     importButton { "Import takes..." }, pitchButton { "Pitch..." }, pitchCurveButton { "Pitch curve..." }, repairButton { "Spectral Repair..." }, declickButton { "De-Click..." }, exportProcButton { "Export for Processing..." }, undoFixButton { "Undo fix" }, barSearchButton { "Search for bar" };
    juce::ToggleButton overdubBox { "Overdub" }, loopToggle { "Loop [L]" };
    PlayheadModeButton playheadMode;
    RainbowButton waveColourBtn;
    std::unique_ptr<TakeTimeline> timeline;
    std::unique_ptr<juce::Viewport> viewportOwner;
    std::unique_ptr<KeepKeysOnTimeline> keysKeeper;           // declared after the viewport: destroyed first
};
} // namespace td

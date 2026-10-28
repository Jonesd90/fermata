#pragma once
#include "AppContext.h"
#include "RecordMonitor.h"
#include "BarCoverage.h"
#include "TakeChart.h"
#include "Uikit.h"
#include "TimeReadout.h"

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
    /** The Loop button, for the Stream Deck: is it lit, and the same as pressing it (key L). */
    bool loopIsOn() const { return loopToggle.getToggleState(); }
    void toggleLoop();
    /** The Stream Deck's window keys: what = mark1 mark2 align search markers. State: -1 not in this window, 0 off, 1 lit. Action: the same as pressing that button. */
    int deskActState (const juce::String& what) const;
    void deskAct (const juce::String& what);
    // drag audio files (or a folder) from another program's session onto this window to import them as takes
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    TakeWindowDef* def() const { return app.project.findTakeWindow (windowId); }
    void updateLtcButton();

    void markWholeSelected();
    void editMarkIn();
    void editMarkOut();
    void toEdit (bool atEditPlayhead = false, bool insert = false);       // insert (Shift + 4): cut the edit and push the rest later; else a piece at the playhead overwrites
    void sendToEdit (bool atEditPlayhead, bool insert = false);
    void togglePlay();
    void toggleBarSearch();                                   // "Search for bar": asks for a bar number, then frames every take here that contains it
    void setBarSearch (int bar);
    int searchBar = 0;
    /** J / Shift + J (3.4.0): the same place in the next / previous take of this window, found by matching the music (TakeMatchService). */
    void jumpToSamePlace (bool forward);
    /** A click on the bar-coverage map or the take chart: frame the takes that have the bar, and go to it (in 'inTake' when it has the bar). */
    void goToBar (int bar, const juce::Uuid& inTake = juce::Uuid::null());
    /** A click on BARS at the left of the bar strip, or Shift+B (3.8.0): drops the take chart down from that corner, or puts it away. */
    void toggleTakeChart();
    void closeTakeChart();
    void placeTakeChart();                                    // under the bar strip, from its left corner, at the size it was last given (within the window)
    /** A right-click on the bar strip (3.8.0): asks how many bars the piece has. */
    void askPieceBars();
    std::unique_ptr<TakeChartComponent> chart;                // the take chart, while it is down
    void playMarked();
    void loopChanged();                  // the Loop button / key L
    bool markedRange (juce::Uuid& take, double& in, double& out) const;
    juce::String playLoop (const juce::Uuid& take, double in, double out);     // goes round the marked area on the window timeline
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
    juce::Label nameLabel, statusLabel, markLabel;
    RecordLamps recordLamps;                         // MAIN / 2ND / DEVICE (3.2.0): at the right end of the marks row; a click opens the Recording monitor
    BarCoverageStrip barMap;                         // the bar-coverage map (3.4.0): under the head, when any take has bars
    juce::TextEditor nameEditor;
    StackedField nameBlock { nameLabel, nameEditor };          // "Piece / Take Window Name:" above the name box, as tall as the Next Take box
    NextTakeBox takeBox;
    TimeReadout clock;                                        // the big time readout in the top bar
    TimeReadout lengthClock;                                  // the take's length (setLengthStyle: quieter, beside it)
    static constexpr int kClockH = 58;                        // as tall as the Next Take box
    juce::Label sendCaption;
    juce::ComboBox sendBox;
    void fillSendBox();
    bool updatingSendBox = false;
    IconButton armAllButton { "Arm all" }, armNoneButton { "Arm none" };
    juce::TextButton ltcButton { juce::String::fromUTF8 ("\xE2\x96\xB6 LTC") };       // the triangle: shows / hides the LTC track
    IconButton recordButton { "REC  [R]" }, playButton { "Play [Space]" }, startButton { "|<" }, backButton { "<<" }, fwdButton { ">>" }, endTransportButton { ">|" }, bounceButton { "Bounce Out..." },
               pitchButton { "Pitch..." }, repairButton { "Repair & DeClick..." }, undoFixButton { "Undo fix" };
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" };
    IconButton editInButton { "Mark 1 [1]" }, editOutButton { "Mark 2 [2]" },
               toEditButton { "To edit [3]" }, toEditAtButton { "To playhead [4]" }, editWindowButton { "Edit window" },
               importButton { "Import takes..." }, exportProcButton { "Export for Processing..." }, barSearchButton { "Search for bar" },
               alignButton { "Align spot mics" }, takeSheetButton { "Take sheet" };
    IconButton overdubBox { "Overdub" }, markersToggle { "Markers" };         // (toggles)
    LoopButton loopToggle;
    PlayheadModeButton playheadMode;
    RainbowButton waveColourBtn;
    std::unique_ptr<TakeTimeline> timeline;
    std::unique_ptr<juce::Viewport> viewportOwner;
    std::unique_ptr<KeepKeysOnTimeline> keysKeeper;           // declared after the viewport: destroyed first
};
} // namespace td

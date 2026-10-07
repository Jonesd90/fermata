#pragma once
#include "AppContext.h"
#include "Uikit.h"

namespace td
{
class EditTimeline;

/** The edit window: the pieces you sent from the take window(s), placed on a timeline. It stands on its own: it keeps working
    even if the takes are later removed from their take windows. Slide a piece with the mouse; Slip Left / Slip Right
    decide whether the pieces before / after it move along with it. */
class EditWindowComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::ChangeListener, private juce::Timer
{
public:
    EditWindowComponent (AppContext&, const juce::Uuid& editId);
    ~EditWindowComponent() override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    EditDef* edit() const { return app.project.findEdit (editId); }
    void togglePlay();
    void loopChanged();                  // the Loop button / key L: loop the area between marks 1 and 2
    bool loopRange (double& from, double& to) const;
    void trimSelected();
    void placeFromTakeWindow();
    void deleteSelected();
    void moveSelected (int delta);
    void setMark (bool in);
    void setFixMark (bool in);
    void markWholeSelected();
    void openBounce();
    void showTracksMenu();
    // ---- transport: Start, Back 5 s, Play / Stop, Forward 5 s, End (of the last piece). They work while playing too, and you can play on beyond the end.
    double transportNow() const;
    void goTo (double seconds);
    void jumpBy (double seconds) { goTo (transportNow() + seconds); }
    void goToEnd();
    void removeTrackFromEdit (const juce::Uuid& trackId);
    /** If the edit's audio belongs to tracks that are no longer in the project (so there is no mixer strip for them) asks whether to add them back. */
    void checkMixerStrips();

    bool stripAskOpen = false, skipStripAsk = false;     // the "more tracks than strips" question is open / was answered No (not asked again at every Play)
    AppContext& app;
    juce::Uuid editId;
    juce::Label nameCaption, infoLabel;
    juce::TextEditor nameEditor;
    juce::TextButton playButton { "Play [Space]" }, startButton { "|<" }, backButton { "<<" }, fwdButton { ">>" }, endTransportButton { ">|" }, trimButton { "Trim join [T]" }, deleteButton { "Delete piece" },
                     leftButton { "Move earlier" }, rightButton { "Move later" }, endButton { "Next goes at end" },
                     zoomInButton { "+" }, zoomOutButton { "-" }, bounceButton { "Bounce Out..." }, automationButton { "Automation" },
                     pitchButton { "Pitch..." }, pitchCurveButton { "Pitch curve..." }, repairButton { "Spectral Repair..." }, declickButton { "De-Click..." }, exportProcButton { "Export for Processing..." }, undoFixButton { "Undo fix" }, tracksButton { "Tracks..." }, importButton { "Import..." };
    juce::ToggleButton slipLeftToggle { "Slip Left" }, slipRightToggle { "Slip Right" }, loopToggle { "Loop [L]" };
    PlayheadModeButton playheadMode;
    std::unique_ptr<EditTimeline> timeline;
    std::unique_ptr<juce::Viewport> viewportOwner;
    std::unique_ptr<KeepKeysOnTimeline> keysKeeper;           // declared after the viewport: destroyed first
};
} // namespace td

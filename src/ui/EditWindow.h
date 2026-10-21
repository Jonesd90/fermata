#pragma once
#include "AppContext.h"
#include "Uikit.h"
#include "TimeReadout.h"

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
    void goToSeconds (double seconds) { goTo (seconds); }       // (the timeline's feedback flags use it)
    void removeTracks (const std::vector<juce::Uuid>& ids) { for (auto& t : ids) removeTrackFromEdit (t); }   // (the timeline's name menu)
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    EditDef* edit() const { return app.project.findEdit (editId); }
    void togglePlay();
    void loopChanged();                  // the Loop button / key L: loop the area between marks 1 and 2
    bool loopRange (double& from, double& to) const;
    void trimSelected();
    void buttSelected();
    void placeFromTakeWindow();
    void deleteSelected();
    void moveSelected (int delta);
    void setFixMark (bool in);
    void markWholeSelected();
    void openBounce();
    void showTracksMenu();
    void showFeedbackMenu();
    void importFeedback();
    void importFeedbackFiles (juce::Array<juce::File> files, double frameRate);
    std::unique_ptr<juce::FileChooser> feedbackChooser;
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
    TimeReadout clock;                                        // the big time readout in the top bar
    static constexpr int kClockH = 58;
    // the Buses / VCAs view: an audio track's waveform drawn behind the automation lanes, for reference
    juce::Label refCaption; juce::ComboBox refBox; std::vector<juce::Uuid> refIds; juce::String refSig;
    void fillRefBox();
    juce::TextEditor nameEditor;
    StackedField nameBlock { nameCaption, nameEditor };      // "Piece / Edit Window Name:" above the name box
    IconButton playButton { "Play [Space]" }, startButton { "|<" }, backButton { "<<" }, fwdButton { ">>" }, endTransportButton { ">|" }, trimButton { "Trim Window [T]" }, deleteButton { "Delete piece" },
               leftButton { "Move earlier" }, rightButton { "Move later" }, bounceButton { "Bounce Out..." }, mixerButton { "Edit Mixer" }, automationButton { "Automation" }, busViewButton { "Buses / VCAs" },
               pitchButton { "Pitch..." }, repairButton { "Repair & DeClick..." }, denoiseButton { "Denoise..." }, noiseModeButton { "Noise mode" }, reharmoniserButton { "ReHarmoniSer..." }, undoFixButton { "Undo fix" }, tracksButton { "Tracks..." }, feedbackButton { "Feedback..." },
               endButton { "Next goes at end" }, exportProcButton { "Export for Processing..." }, importButton { "Import..." },
               slipLeftToggle { "Slip Left" }, slipRightToggle { "Slip Right" }, shuffleToggle { "Shuffle" }, insertFlagToggle { "Insert flag" }, markersToggle { "Markers" }, feedbackToggle { "Feedback markers" };
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" }, ltcButton { juce::String::fromUTF8 ("\xE2\x96\xB6 LTC") };
    LoopButton loopToggle;
    PlayheadModeButton playheadMode;
    RainbowButton waveColourBtn;
    std::unique_ptr<EditTimeline> timeline;
    std::unique_ptr<juce::Viewport> viewportOwner;
    std::unique_ptr<KeepKeysOnTimeline> keysKeeper;           // declared after the viewport: destroyed first
};
} // namespace td

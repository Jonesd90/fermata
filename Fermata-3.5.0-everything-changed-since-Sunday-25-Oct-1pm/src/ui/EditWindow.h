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
    /** The Loop button, for the Stream Deck: is it lit, and the same as pressing it (key L). */
    bool loopIsOn() const { return loopToggle.getToggleState(); }
    void toggleLoop();
    /** The Stream Deck's window keys: what = mark1 mark2 align trim markers feedback. State: -1 not in this window, 0 off, 1 lit. Action: the same as pressing that button. */
    int deskActState (const juce::String& what) const;
    void deskAct (const juce::String& what);
    void mouseDown (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void goToSeconds (double seconds) { goTo (seconds); }       // (the timeline's feedback flags use it)
    void removeTracks (const std::vector<juce::Uuid>& ids) { for (auto& t : ids) removeTrackFromEdit (t); }   // (the timeline's name menu)
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    /** Plays from 'from' (to 'to', looping: as AppContext::playEdit) - the edit, or the visual guide on its own while the guide's S is on. */
    juce::String play (double from, double to = -1.0, bool loop = false, bool goOn = false);
    /** Playing this edit: starts again from where it is now (the guide's S was switched). */
    void replayFromPlayhead();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    EditDef* edit() const { return app.project.findEdit (editId); }
    void togglePlay();
    void loopChanged();                  // the Loop button / key L: loop the area between marks 1 and 2
    bool loopRange (double& from, double& to) const;
    void trimSelected();
    void buttSelected();
    void placeFromTakeWindow (bool insert = false);
    void deleteSelected();
    void moveSelected (int delta);
    void setFixMark (bool in);
    void snipAtPlayhead();                        // F1: a snip (an unnumbered join with a normal crossfade) at the playhead
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
    void refreshGuideButton();                    // the Guide button: lit when the guide is shown, dim when the edit has none

    bool stripAskOpen = false, skipStripAsk = false;     // the "more tracks than strips" question is open / was answered No (not asked again at every Play)
    AppContext& app;
    juce::Uuid editId;
    juce::Label nameCaption, infoLabel;
    TimeReadout clock;                                        // the big time readout in the top bar
    static constexpr int kClockH = 58;
    juce::TextEditor nameEditor;
    StackedField nameBlock { nameCaption, nameEditor };      // "Piece / Edit Window Name:" above the name box
    IconButton playButton { "Play [Space]" }, startButton { "|<" }, backButton { "<<" }, fwdButton { ">>" }, endTransportButton { ">|" }, trimButton { "Trim Window [T]" }, deleteButton { "Delete piece" },
               leftButton { "Move earlier" }, rightButton { "Move later" }, bounceButton { "Bounce Out..." }, videoButton { "Export for video edit..." }, aafButton { "Export AAF..." }, mixerButton { "Edit Mixer" }, automationButton { "Automation" },
               pitchButton { "Pitch..." }, repairButton { "Repair & DeClick..." }, reharmoniserButton { "ReHarmoniSer..." }, retimerButton { "ReTimEr..." }, undoFixButton { "Undo fix" }, tracksButton { "Tracks..." }, feedbackButton { "Feedback..." },
               endButton { "Next goes at end" }, exportProcButton { "Export for Processing..." }, importButton { "Import..." }, alignButton { "Align spot mics" }, roomToneButton { "Room tone" }, noiseModeButton { "Noise mode" },
               slipLeftToggle { "Slip Left" }, slipRightToggle { "Slip Right" }, shuffleToggle { "Shuffle" }, insertFlagToggle { "Insert flag" }, markersToggle { "Markers" }, feedbackToggle { "Feedback markers" };
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" }, ltcButton { juce::String::fromUTF8 ("\xE2\x96\xB6 LTC") }, guideToggle { "Guide" };
    LoopButton loopToggle;
    PlayheadModeButton playheadMode;
    RainbowButton waveColourBtn;
    std::unique_ptr<EditTimeline> timeline;
    std::unique_ptr<juce::Viewport> viewportOwner;
    std::unique_ptr<KeepKeysOnTimeline> keysKeeper;           // declared after the viewport: destroyed first
};
} // namespace td

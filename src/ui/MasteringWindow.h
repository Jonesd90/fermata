#pragma once
#include "AppContext.h"
#include <map>
#include <set>
#include "../core/MasterExport.h"
#include "../core/MasterRender.h"

namespace td
{
class MasterListPanel;
class MasterTimeline;
class MasterPqTable;
struct MasterField;

/** The outline of an edit's audio (the loudest level in each small slice), drawn inside its box on the DDP timeline. */
struct MasterEnvelope { std::vector<float> bins; double fraction = 1.0; };      // fraction: how much of the box the audio fills (the rest is the tail)

/** The Mastering window. Two views: the Virtual Master (the edits as audio files in any format, with levels and tags) and the DDP builder (the edits laid out as a CD
    with automatic PQ points, CD-Text, and a DDP fileset / WAV + CUE as the result). */
class MasteringComponent : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit MasteringComponent (AppContext&);
    ~MasteringComponent() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    // ---- model helpers
    MasteringDef& def() { return app.project.mastering; }
    const MasteringDef& def() const { return app.project.mastering; }
    const EditDef* editOf (const juce::Uuid&) const;
    juce::String editName (const juce::Uuid&) const;
    std::vector<juce::int64> clipFrames() const;          // 44.1 kHz length of every clip of the disc
    juce::String signature() const;                       // changes when the set / names / lengths of the edits change
    void touched() { app.project.markDirty(); }

    // ---- building / refreshing
    void setView (int v);
    void refreshAll (bool force);
    void refreshVm();
    void refreshDdp (bool reloadFields = true);
    void refreshSource();
    void refreshFormatControls();
    void refreshFields();
    void rebuildQualityBox();
    void layoutVm (juce::Rectangle<int> area);
    void layoutDdp (juce::Rectangle<int> area);
    MasterField* addField (juce::Component& parent, juce::OwnedArray<MasterField>& list, const juce::String& caption, std::function<juce::String*()> ref, int maxChars = 0);

    // ---- actions
    void startVmExport();
    void startDdpExport();
    void startJob (const MasterJobSpec&, bool disc);
    void jobFinished (const MasterExportResult&, bool disc);
    void setBusy (bool);
    void chooseFolder (bool disc);
    void applyGapToSelected (const juce::String& text);
    void setAllGaps (double seconds);
    void moveItem (int from, int to, bool disc);
    void swapClips (int a, int b);

    void requestEnvelope (const juce::Uuid& editId);
    void moveFlag (int kind, int clipIndex, int sector);
    void resetFlag (int kind, int clipIndex);
    std::map<juce::String, MasterEnvelope> envelopes;      // by edit id; message thread only
    std::map<juce::String, juce::String> envelopeKeys;     // what each stored outline was made from
    std::set<juce::String> envelopePending;

    AppContext& app;
    int view = 0;
    juce::TextButton vmTab { "Virtual Master" }, ddpTab { "DDP builder" };
    juce::Label renderCaption, tailCaption, renderNote;
    juce::ComboBox mixerBox, sourceBox, tailBox;
    std::vector<juce::Uuid> mixerIds, sourceIds;
    juce::String lastSignature;
    bool updating = false;

    // ---- the Virtual Master page
    std::unique_ptr<MasterListPanel> vmList;
    juce::TextButton vmAll { "Tick all" }, vmNone { "Tick none" }, vmUp { "Move up" }, vmDown { "Move down" };
    juce::Label fmtCaption, rateCaption, bitsCaption, ditherCaption, qualityCaption, levelCaption, albumCaption, fileCaption, nameCaption, folderCaption, vmStatus;
    juce::ComboBox formatBox, rateBox, bitsBox, ditherBox, qualityBox;
    juce::Label alsoCaption;
    juce::ToggleButton alsoToggle[5];
    juce::ToggleButton normToggle { "Set the peak level to" }, overallToggle { "Overall: one gain for all the files (they keep their balance)" },
                       individualToggle { "Individually: every file reaches the peak level on its own" }, numberToggle { "Number the files: 01 - name" };
    juce::TextEditor peakEditor, folderEditor;
    juce::Label peakUnit;
    juce::TextButton chooseButton { "Choose folder..." }, exportButton { "Export" }, showButton { "Show in folder" };
    juce::OwnedArray<MasterField> albumFields, fileFields;
    juce::Viewport vmSettings;
    struct VmHolder : juce::Component { } vmHolder;
    double progressValue = 0.0;                 // shown by the progress bars below (declared first: they hold a reference to it)
    juce::ProgressBar vmProgress;

    // ---- the DDP page
    std::unique_ptr<MasterListPanel> ddpList;
    std::unique_ptr<MasterTimeline> timeline;
    std::unique_ptr<MasterPqTable> pqTable;
    juce::Viewport pqView, rightView;
    struct RightHolder : juce::Component { } rightHolder;      // the selected track's panel: scrolls when the window is short, so nothing is ever lost
    juce::OwnedArray<MasterField> discFields, clipFields;
    juce::Label gapCaption, discCaption, clipCaption, pqCaption, warnings, ddpStatus, allGapsCaption, ddpFolderCaption, ddpPeakUnit;
    juce::TextEditor gapEditor, ddpFolderEditor, ddpPeakEditor;
    juce::ComboBox allGapsBox;
    juce::TextButton autoPq { "Auto PQ" }, isrcBtn { "ISRCs..." }, zoomIn { "+" }, zoomOut { "-" }, zoomFit { "Fit" }, ddpUp { "Move earlier" }, ddpDown { "Move later" },
                     ddpChoose { "Choose folder..." }, ddpExport { "Make DDP" }, ddpShow { "Show in folder" }, ddpCheck { "Check DDP" };
    juce::ToggleButton preToggle { "Pre-emphasis" }, copyToggle { "Copy permitted" }, zipToggle { "Also make a .zip" }, cueToggle { "Also make a WAV + CUE sheet" },
                       ddpNorm { "Set the peak level to" }, ddpTogether { "One gain for the whole disc" };
    juce::ProgressBar ddpProgress;
    // the playhead and the audition of the disc
    juce::TextButton playBtn { "Play" }, gapPlayBtn { "Play the gap" }, startBtn { "Track start" };
    juce::Label timeLabel;
    double playheadSec = 0.0, playStartSec = 0.0;
    bool discPlaying = false, fitted = false;
    juce::Uuid playToken;
    void setPlayhead (double seconds, bool fromUser);
    void startPlay (double fromSec, double toSec);
    void stopPlay();
    void togglePlay();
    void playGap();
    void zoomKey (bool in);
    void autoPqInPlace();
    void openIsrcTool();
    void centrePlayhead();
    void updateTimeLabel();
    void pollPlayback();
    std::vector<AppContext::DiscPlayItem> discItems() const;

    // ---- the renders (MasterRender.h): every ticked piece is rendered through its own mixer when this window is opened or comes to the front and something
    // changed; the disc is played from those renders, and the export uses them. At most two renders of a piece are kept.
    struct RenderView { juce::String text; bool bad = false; bool previous = false, backup = false; juce::File file; double rate = 0.0; juce::int64 frames = 0; };
    std::map<juce::String, RenderView> renders;                // by edit id; message thread only
    std::unique_ptr<MasterRenderJob> renderJob;
    double renderProgressValue = 0.0;
    juce::ProgressBar renderProgress;
    juce::Label renderStatus;
    juce::TextButton prevRenderBtn { "Use previous render" }, renderAgainBtn { "Render again" };
    bool wasActive = false;
    juce::uint32 recheckDue = 0;
    struct PendingPlay { bool on = false; double from = 0.0, to = -1.0; } pendingPlay;
    std::vector<juce::Uuid> includedEdits() const;
    bool checkRenders (bool force = false);          // true = every ticked piece has an up-to-date render (nothing was started)
    void scheduleRecheck (int ms = 800) { recheckDue = juce::jmax<juce::uint32> (1, juce::Time::getMillisecondCounter() + (juce::uint32) ms); }
    void renderFinished (const juce::String& error);
    void updateRenderButtons();
    int previewOutFirst() const;
    juce::Uuid selectedPiece() const { return view == 0 ? def().selectedItem : def().selectedClip; }

    std::unique_ptr<MasterExportJob> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastOutput;
    bool lastWasDisc = false;
};
} // namespace td

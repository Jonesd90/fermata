#pragma once
#include "AppContext.h"

namespace td
{
class TrimWaves;

/** The trim window for one join of an edit (or for the start or end of the edit, where only a fade-in or a fade-out exists): the outgoing piece (A, top) and the incoming piece (B, bottom) around the join,
    with the crossfade drawn over them. Drag a waveform to slide that audio under the join, drag the crossfade handles to
    shape the out- and in-fades independently, audition with F2, accept with Enter. */
class TrimComponent : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    /** atEnd: the end of the edit: regionBId is then the LAST piece and the window shows its fade-out. (The start of the edit is the first piece's fade-in.) */
    TrimComponent (AppContext&, const juce::Uuid& editId, const juce::Uuid& regionBId, bool atEnd = false);
    ~TrimComponent() override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Accept what has been done on the current join and show another join of the same edit. */
    void gotoJoin (const juce::Uuid& regionBId, bool atEnd = false);

    // ---- used by TrimWaves ----
    AppContext& getApp() { return app; }
    EditDef* edit() const { return app.project.findEdit (editId); }
    int joinIndex() const;                                  // 0 = the start of the edit, n = its end; -1 if that join no longer exists
    struct Refs { EditDef* e = nullptr; int k = -1; EditRegion* A = nullptr; EditRegion* B = nullptr; bool ok() const { return e != nullptr && k >= 0 && (A != nullptr || B != nullptr); } };
    Refs refs() const;
    juce::AudioFormatReader* readerA() const { return rdA.get(); }
    juce::AudioFormatReader* readerB() const { return rdB.get(); }
    int readersStamp() const { return stamp; }
    juce::int64 fileStartA() const { return fsA; }
    juce::int64 fileStartB() const { return fsB; }
    bool slipLeft() const  { return slipLeftToggle.getToggleState(); }
    bool slipRight() const { return slipRightToggle.getToggleState(); }
    bool isAuditioning() const { return app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == editId; }
    double viewSeconds = 2.0;
    juce::int64 centreSample = 0;
    void viewMoved();                                       // the view was panned: redraw (no edit changed)
    void dragChanged();                                     // while dragging: cheap
    void dragFinished();                                    // mouse released: tell everyone

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshReaders();
    void refreshControls();
    void describe();
    void recentre();
    void zoomBy (double factor);
    void slideA (double ms);
    void slideB (double ms);
    void moveJoinBy (double ms);
    void audition();
    void playOriginal (bool outgoing);
    void stopAll();
    void accept();
    void revert();
    void step (int delta);
    void resetFades();
    void copyFade (bool outToIn);
    void takeSnapshot();
    double preRoll() const  { return preSlider.getValue(); }
    double postRoll() const { return postSlider.getValue(); }

    AppContext& app;
    juce::Uuid editId, regionBId;
    bool endEdge = false;
    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReader> rdA, rdB;
    juce::Uuid readersForA, readersForB;
    int readersTrack = -1, stamp = 0;
    juce::int64 fsA = 0, fsB = 0;                           // where each reader's file starts in take samples (a corrected piece is a short file)
    std::vector<EditRegion> snapshot;                       // the edit as it was when this join was opened / last accepted

    std::unique_ptr<TrimWaves> waves;
    juce::Label infoLabel, fadeLabel, trackCaption, curveCaption, rollCaption;
    juce::ComboBox trackBox, curveBox;
    juce::Slider preSlider, postSlider;
    juce::TextButton zoomIn { "Zoom +" }, zoomOut { "Zoom -" }, centreButton { "Centre on join" }, resetFade { "Reset crossfade" },
                     acceptButton { "Accept  [Enter]" }, revertButton { "Revert this join" },
                     prevButton { "<< Accept, previous fade  [" }, nextButton { "Accept, next fade  ] >>" },
                     auditionButton { "Audition  [F2]" }, origA { "Play original A" }, origB { "Play original B" }, stopButton { "Stop" },
                     copyOutIn { "Copy out-fade to in  [A]" }, copyInOut { "Copy in-fade to out  [Z]" };
    juce::ToggleButton loopToggle { "Loop" }, slipLeftToggle { "Slip Left" }, slipRightToggle { "Slip Right" };
    struct Group { juce::Label label; juce::OwnedArray<juce::TextButton> buttons; };
    Group gA, gB, gJ;
    bool wasAuditioning = false, updatingControls = false;
};
} // namespace td

#pragma once
/** Re-HarmoniSer editor: a reusable juce::Component (spectrogram, Note Selection, Ensemble ReCentre, Note ReShape, Corrected Notes, View).
    It has no window, file or audio-device handling of its own. The host (Fermata, or the standalone app's shell) gives it the audio as float buffers
    and fills in EditorHost to let it hear things, and to take the result. All the maths is in the JUCE-free core (Engine.h, Spectrogram.h). */
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "Engine.h"
#include "Spectrogram.h"

namespace rehar
{
/** One confirmed note correction (an entry of Corrected Notes). It holds everything needed to render it again. */
struct NoteEdit
{
    int id = 0;
    double t0 = 0, t1 = 0, fLo = 0, fHi = 0;       // the box that was drawn (seconds in the audio, Hz)
    int midi = 57; double a4 = 440.0;
    Params params;
    Vec ts, f0, off;                               // the measured note (seconds, Hz) and the correction (cents) on the same grid
    bool enabled = true;
    juce::String label;
};

/** What the editor needs from whoever hosts it. Every call is made on the message thread. All of them are optional. */
struct EditorHost
{
    /** Make the host play the audio with these corrected channels (the same length as the original), or the original when null. done (error) when it is ready to play. */
    std::function<void (std::shared_ptr<const Block> corrected, std::function<void (const juce::String&)> done)> setCorrected;
    std::function<void (double fromSec, double toSec, bool loop)> play;      // seconds from the start of the audio
    std::function<void()> stop;
    std::function<double()> playPosition;                                    // seconds from the start of the audio, or a negative number when not playing
    std::function<void (double hz, double levelDb)> playTone;                // a short tone (the piano keys), at this level in dB (the Key level slider)
    std::function<void()> writeBack;                                         // the user pressed "Write back to clip" (setCorrected has already been called with the final audio)
    std::function<void()> cancel;                                            // the user pressed Cancel
    std::function<void (const juce::String&)> log;                           // errors and notes for the log file
};

class ReHarmoniserEditor : public juce::Component, private juce::Timer
{
public:
    ReHarmoniserEditor();
    ~ReHarmoniserEditor() override;

    EditorHost host;
    /** Gives the editor the audio to work on: block[channel][frame], all the same length. Starts the analysis (a progress bar shows). */
    void setAudio (std::shared_ptr<const Block> audio, int sampleRate);
    bool hasAppliedChanges() const;                 // is there anything in Corrected Notes that is switched on
    bool hasPending() const { return cand.has; }
    int appliedCount() const;
    const std::vector<NoteEdit>& editList() const { return edits; }

    /** Names for the channels of the audio (for the See mixer), in the order of the Block. Call before or after setAudio. */
    void setChannelNames (const juce::StringArray&);

    /** Shows a message on the status line (the host uses it while it is loading the audio). */
    void setStatus (const juce::String&, bool error = false);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class View; class Worker; class EditsList; struct Section;
    friend class View;
    struct Cand { bool has = false; double t0 = 0, t1 = 0, fLo = 0, fHi = 0; Vec ts, f0; int hint = 1, midi = 57; Plan plan; } cand;
    struct Hist { Params p; };

    // ---- the work
    void timerCallback() override;
    void startAnalysis();
    void selectBox (double t0, double t1, double fLo, double fHi);
    void measure();
    void replan();
    void renderAndSet (bool withPending, std::function<void()> then = {});
    void applyCorrection();
    void deselect();
    void clearChanges();
    void undoStep();
    void redoStep();
    void pushHistory();
    void audition();
    void togglePlay();
    void toggleOriginal();
    void writeBack();
    void rebuildEditsList();
    void loadAsPending (const NoteEdit&);
    void startJob (const juce::String& stage, std::function<juce::String (Worker&)> work, std::function<void (const juce::String&)> done);
    void syncSliders();
    void sliderChanged();
    void detectReference();
    void updateInfo();
    void updateButtons();
    void openSection (int idx);
    void layoutSections();
    void rebuildSeeStrips();
    void layoutSeeStrips();
    Vec seeGainsDb() const;                          // the See mixer as dB per channel (muted / not soloed = -100); empty = all flat
    void seeChanged();
    void recombine();
    juce::String noteName (int midi) const;
    static int parseNote (const juce::String&);
    double notePitchHz (int midi) const { return midiHz (midi, a4); }

    // ---- state
    std::shared_ptr<const Block> orig; int sr = 0; double dur = 0.0;
    struct Mix { size_t nf = 0, nb = 0; double hopSec = 0.01, df = 1.0; std::vector<uint8_t> v; };
    static std::shared_ptr<Mix> makeMix (const Spectrogram&);
    std::shared_ptr<const Mix> mix; double refDb = 0.0;
    std::shared_ptr<Spectrogram> spec;               // the per-channel pictures (the See mixer adds them again with new gains)
    bool seeDirty = false; juce::uint32 seeDirtyAt = 0; juce::StringArray chanNames;
    double a4 = 440.0; bool midiLocked = false;
    Params params; int partial = 1;
    std::vector<NoteEdit> edits, undone; int nextId = 1;
    bool busy = false; int refModeId = 1;
    std::vector<Hist> hist; size_t histPos = 0; bool inHistory = false;
    enum class HostState { Original, Corrected, CorrectedWithPending } hostState = HostState::Original;
    bool originalMode = false, hostBusy = false, auditionStale = false, analysing = false, measuring = false;
    double cueT = 0.0, loopIn = -1.0, loopOut = -1.0, playT = -1.0; bool looping = false;
    int serial = 0;
    juce::String lastStatus;

    // ---- the parts
    std::unique_ptr<View> view;
    std::unique_ptr<Worker> worker;
    juce::Label status, noteInfo, tipLabel;
    juce::TextButton playBtn { "Play" }, stopBtn { "Stop" }, loopBtn { "Loop" }, originalBtn { "Original" }, writeBtn { "Write back to clip" }, cancelBtn { "Cancel" };
    double progressValue = 0.0; juce::ProgressBar bar;
    // Note Selection
    juce::Label nameCap { {}, "meant to be" }; juce::TextEditor nameBox; juce::TextButton downBtn { "-" }, upBtn { "+" }, remeasureBtn { "Measure again" };
    juce::TextButton tipUse { "Use it" }, tipIgnore { "Ignore" };
    // sections
    std::vector<std::unique_ptr<Section>> sections; int openIdx = 2;
    juce::Label a4Cap { {}, "Reference A4" }; juce::TextEditor a4Box; juce::TextButton detectBtn { "Detect" }; juce::ComboBox refMode; juce::Label refInfo;
    struct Row { juce::Label cap; juce::Slider s; };
    Row moveRow, snapRow, strengthRow, keepRow, smoothRow, easeRow, holdRow, bwRow, linesRow, gainRow, keyRow;
    struct SeeStrip { juce::Label name; juce::TextButton m { "M" }, s { "S" }; juce::Slider g; };
    std::vector<std::unique_ptr<SeeStrip>> seeStrips; juce::Component seeHolder; juce::Viewport seeView;
    juce::ComboBox partialBox; juce::ToggleButton matchBox { "Level smoothing" }; juce::ToggleButton advancedBtn { "Advanced" };
    juce::TextButton auditionBtn { "Audition" }, applyBtn { "Apply correction" }, deselectBtn { "Deselect" }, clearBtn { "Clear changes" }, undoBtn { "Undo" }, redoBtn { "Redo" };
    juce::TextButton zoomTIn { "+" }, zoomTOut { "-" }, zoomPIn { "+" }, zoomPOut { "-" }, loopClearBtn { "Clear loop" };
    juce::Label zoomCap { {}, "Zoom time / pitch" };
    std::unique_ptr<EditsList> editsList; juce::Viewport editsView;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReHarmoniserEditor)
};
} // namespace rehar

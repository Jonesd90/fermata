#pragma once
#include "AppContext.h"
#include "../core/TimeAlign.h"
#include <map>

namespace td
{
/** Align spot mics: give every track its role (Main, Spot, Distance or Not used), press Analyse after a run-through (or later, in post, on an edit),
    and every spot is delayed so it lines up with the main pair(s) - in every mixer, the files untouched. Every piece (take window) keeps its own
    measurements and delays, as players, mics, distances and the temperature change from piece to piece; an edit measured in post gets its own
    too (otherwise it plays with the piece most of its audio comes from). See timealign::analyse. */
class AlignComponent : public juce::Component, private juce::Timer
{
public:
    AlignComponent (AppContext&, bool isEdit, const juce::Uuid& sourceId);
    ~AlignComponent() override;
    /** Another window's Align button: measure there from now on. */
    void setSource (bool isEdit, const juce::Uuid& id);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** What will be measured (the take or the edit, whole or between the marks): the files and where in them. */
    struct Source
    {
        bool ok = false, isEdit = false;
        juce::String text, error;
        double sampleRate = 48000.0;
        juce::int64 length = 0;
        struct TakeTrack { juce::File file; juce::int64 offset = 0; };
        std::map<juce::String, TakeTrack> takeFiles;           // track id -> its file (a take)
        std::vector<EditRegion> regions; juce::int64 from = 0;  // an edit
    };

private:
    struct Row;
    struct Job;
    Source makeSource() const;
    juce::String ownOwner() const { return sourceId.toString(); }               // the set this window measures into and changes
    /** The set the source plays with now (an edit without its own: its piece's); null = none yet. */
    const AlignSet* shownSet() const { return app.project.alignSetOf (app.project.alignOwnerFor (isEdit, sourceId)); }
    /** The source's own set, made (from the one it plays with now) the first time it is changed. */
    AlignSet& editableSet();
    juce::String pieceName (const juce::String& owner) const;
    MixerState* sourceMixer() const;                                             // the mixer whose pans count: an edit's own mixer, or the processing mixer
    StripState* stripOf (const juce::Uuid& trackId) const;
    void rebuildRows();
    void refreshRows();
    void analyse();
    void stopJob();
    void applyResults (const timealign::Analysis&, const std::vector<juce::Uuid>& ids, const juce::String& measuredOn);
    std::vector<timealign::TrackIn> trackInputs (std::vector<juce::Uuid>& ids) const;
    void tracksChanged();                                                        // the delays changed: the engine takes them now
    void useSuggestedPan (const juce::Uuid& trackId);
    void recalcForPans();
    void showCopyMenu();
    void timerCallback() override;

    AppContext& app;
    bool isEdit = false; juce::Uuid sourceId;
    juce::Label sourceLabel, pieceLabel, measuredLabel, mainNote, status, guessNote;
    juce::TextButton analyseButton { "Analyse" }, stopButton { "Stop" }, usePansButton { "Use all suggested pans" },
                     recalcButton { "Delays for the present pans" }, clearButton { "Clear this piece's delays" }, guessButton { "Guess the roles from the names" },
                     copyButton { "Copy from another piece..." };
    juce::TextButton abButton { "Alignment ON" };
    double progress = 0.0;
    juce::ProgressBar bar { progress };
    juce::Viewport viewport;
    juce::Component rowsHolder;
    juce::OwnedArray<Row> rows;
    std::unique_ptr<Job> job;
    InfoNote info;
    juce::String rowsSig;
    int headY = 0, tick = 0;
};
} // namespace td

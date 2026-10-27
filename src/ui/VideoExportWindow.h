#pragma once
#include "AppContext.h"
#include "Uikit.h"
#include "FixTools.h"
#include "../core/VideoExport.h"
#include "../core/AudioFormats.h"
#include "../core/Ltc.h"

namespace td
{
/** Export for video edit (3.2.0): the dialog. A folder, a name, the gap before every join (a quarter of a second unless changed), Export. The work
    runs on a thread (the mix is rendered through a copy of the project, as Bounce does); the result - the three files, how many pieces had code,
    anything left out - is shown in the window and written to the log. See core/VideoExport.h for what the files are. */
class VideoExportComponent : public juce::Component, private juce::Timer
{
public:
    VideoExportComponent (AppContext& a, const juce::Uuid& eid) : app (a), editId (eid), bar (progressValue)
    {
        auto* e = app.project.findEdit (editId);
        const juce::String editName = e != nullptr ? e->name : juce::String ("edit");
        auto cap = [this] (juce::Label& l, const juce::String& t, bool bold = false)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (13.0f, bold ? juce::Font::bold : juce::Font::plain)); addAndMakeVisible (l);
        };
        cap (title, "Export for video edit - " + editName, true); title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        cap (what, "Three stereo files of exactly the same length, from the first audible sample of the edit to the last:\n"
                   "   1.  " + editName + ".wav - the mix of the edit (its own mixer, main output), as Bounce Out makes it;\n"
                   "   2.  " + editName + " - LTC.wav - the takes' timecode as audio, with no fades: it starts the moment each piece begins to fade in and stops the gap "
                   "before the next piece begins to fade in, so no code runs through a crossfade; a piece shorter than the gap is left out;\n"
                   "   3.  " + editName + " - check.wav - the mix summed to one channel on the left, the timecode on the right, to check they agree.\n"
                   "The timecode is read from the LTC track of the take each piece came from (a Tentacle box, say): the cameras that carry the same time of day then line up with the exact takes used.");
        what.setJustificationType (juce::Justification::topLeft); what.setMinimumHorizontalScale (1.0f);
        cap (folderCaption, "Folder:"); cap (nameCaption, "Name:"); cap (gapCaption, "Gap before each join (seconds):");
        for (auto* ed : { &folderEditor, &nameEditor, &gapEditor }) { addAndMakeVisible (*ed); ed->setSelectAllWhenFocused (true); }
        auto* st = app.props.getUserSettings();
        folderEditor.setText (st != nullptr ? st->getValue ("videoExportFolder|" + app.project.projectFile.getFullPathName(), app.project.bouncedFolder().getChildFile ("For video").getFullPathName())
                                            : app.project.bouncedFolder().getChildFile ("For video").getFullPathName(), juce::dontSendNotification);
        nameEditor.setText (editName, juce::dontSendNotification);
        gapEditor.setText (st != nullptr ? st->getValue ("videoExportGap", "0.25") : "0.25", juce::dontSendNotification);
        gapEditor.setTooltip ("The silence before every join in the LTC file: the code stops this long before the next piece begins to fade in. A quarter of a second unless you have a reason to change it.");
        addAndMakeVisible (browse); browse.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (overdubs); overdubs.setToggleState (true, juce::dontSendNotification);
        overdubs.setTooltip ("The overdub layer's pieces count as pieces too (they are heard, so their timecode goes in). Untick to follow the main layer only.");
        addAndMakeVisible (go); go.onClick = [this] { run(); };
        go.setColour (juce::TextButton::buttonColourId, theme::accent);
        addAndMakeVisible (close); close.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); };
        addAndMakeVisible (show); show.onClick = [this] { if (lastFolder != juce::File()) lastFolder.revealToUser(); }; show.setVisible (false);
        addAndMakeVisible (bar); bar.setVisible (false);
        addAndMakeVisible (status); status.setJustificationType (juce::Justification::topLeft); status.setMinimumHorizontalScale (1.0f);
        status.setFont (juce::FontOptions (12.5f));
        setSize (720, 470);
    }
    ~VideoExportComponent() override { job.reset(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        what.setBounds (r.removeFromTop (124));
        r.removeFromTop (6);
        auto row = r.removeFromTop (grid::btnH);
        folderCaption.setBounds (row.removeFromLeft (60)); browse.setBounds (row.removeFromRight (90)); row.removeFromRight (6); folderEditor.setBounds (row);
        r.removeFromTop (6); row = r.removeFromTop (grid::btnH);
        nameCaption.setBounds (row.removeFromLeft (60)); nameEditor.setBounds (row.removeFromLeft (260)); row.removeFromLeft (16);
        gapCaption.setBounds (row.removeFromLeft (210)); gapEditor.setBounds (row.removeFromLeft (60));
        r.removeFromTop (6); row = r.removeFromTop (grid::btnH);
        overdubs.setBounds (row.removeFromLeft (360));
        r.removeFromTop (10);
        auto bottom = r.removeFromBottom (grid::btnH);
        close.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (6); go.setBounds (bottom.removeFromRight (110)); bottom.removeFromRight (6); show.setBounds (bottom.removeFromRight (120));
        r.removeFromBottom (8);
        bar.setBounds (r.removeFromBottom (18));
        r.removeFromBottom (6);
        status.setBounds (r);
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }

private:
    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Folder for the three files", juce::File (folderEditor.getText()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File()) folderEditor.setText (fc.getResult().getFullPathName(), juce::dontSendNotification);
        });
    }
    void say (const juce::String& text, bool warn)
    {
        status.setColour (juce::Label::textColourId, warn ? theme::warn : theme::text);
        status.setText (text, juce::dontSendNotification);
    }
    void run()
    {
        if (job != nullptr) { job->cancel(); say ("Cancelling...", false); return; }
        if (app.engine.isRecording()) { say ("Stop recording first.", true); return; }
        auto* e = app.project.findEdit (editId);
        if (e == nullptr) { say ("The edit no longer exists.", true); return; }
        videoexport::Settings st;
        st.folder = juce::File (folderEditor.getText().trim());
        if (folderEditor.getText().trim().isEmpty()) { say ("Choose a folder for the files.", true); return; }
        st.name = nameEditor.getText().trim();
        st.gapSeconds = gapEditor.getText().getDoubleValue();
        if (st.gapSeconds < 0.0 || st.gapSeconds > 5.0) { say ("The gap is in seconds, between 0 and 5 (a quarter of a second is the usual).", true); return; }
        st.includeOverdubs = overdubs.getToggleState();
        auto plan = std::make_shared<videoexport::Plan> (videoexport::planFor (app.project, *e, st));
        if (plan->error.isNotEmpty()) { say (plan->error, true); return; }
        if (plan->noLtcTrack) say ("Note: the project has no LTC track (Project Designer: tick the track that records the timecode). The LTC file will be silent.", true);
        if (auto* s = app.props.getUserSettings())
        {
            s->setValue ("videoExportFolder|" + app.project.projectFile.getFullPathName(), folderEditor.getText()); s->setValue ("videoExportGap", gapEditor.getText()); app.props.saveIfNeeded();
        }
        // the mix's settings and its renderer are made here, on the message thread (the plug-ins are re-created for the render copy); the thread only runs it
        auto bs = videoexport::mixSettingsFor (app.project, *e, *plan, st);
        if (bs.sources.empty()) { say ("The edit's mixer has no main output (an Ext bus ticked as Main): nothing to mix to.", true); return; }
        auto bouncer = std::make_shared<Bouncer> (app.project, bs);
        if (bouncer->getPrepareError().isNotEmpty()) { say (bouncer->getPrepareError(), true); return; }
        say ("Exporting...", false);
        bar.setVisible (true); show.setVisible (false); progressValue = 0.0; go.setButtonText ("Cancel"); close.setEnabled (false);
        for (auto* c : std::initializer_list<juce::Component*> { &folderEditor, &nameEditor, &gapEditor, &browse, &overdubs }) c->setEnabled (false);
        startTimerHz (10);
        juce::Component::SafePointer<VideoExportComponent> safe (this);
        job = std::make_unique<Job> (std::move (plan), std::move (bouncer), st, [safe] (const videoexport::Result& r) { if (safe != nullptr) safe->finished (r); });
    }
    /** The export on its own thread, like BounceJob: the window owns it and cancels it when it goes. */
    class Job : private juce::Thread
    {
    public:
        Job (std::shared_ptr<videoexport::Plan> p, std::shared_ptr<Bouncer> b, videoexport::Settings s, std::function<void (const videoexport::Result&)> done)
            : juce::Thread ("Export for video"), plan (std::move (p)), bouncer (std::move (b)), settings (std::move (s)), onDone (std::move (done))
        {
            td::registerAudioFormats (formats);
            startThread (juce::Thread::Priority::normal);
        }
        ~Job() override { cancelFlag = true; stopThread (20000); }
        float getProgress() const noexcept { return progress.load(); }
        void cancel() noexcept { cancelFlag = true; }
    private:
        void run() override
        {
            auto result = videoexport::exportForVideo (*plan, *bouncer, settings, formats, &progress, &cancelFlag);
            auto fn = onDone;
            juce::MessageManager::callAsync ([fn, result] { if (fn) fn (result); });
        }
        std::shared_ptr<videoexport::Plan> plan; std::shared_ptr<Bouncer> bouncer; videoexport::Settings settings;
        std::function<void (const videoexport::Result&)> onDone;
        juce::AudioFormatManager formats;
        std::atomic<float> progress { 0.0f };
        std::atomic<bool> cancelFlag { false };
    };
    void finished (const videoexport::Result& r)
    {
        stopTimer();
        job.reset();
        const auto& err = r.error;
        bar.setVisible (false); go.setButtonText ("Export"); close.setEnabled (true);
        for (auto* c : std::initializer_list<juce::Component*> { &folderEditor, &nameEditor, &gapEditor, &browse, &overdubs }) c->setEnabled (true);
        if (err.isNotEmpty()) { say (err, true); juce::Logger::writeToLog ("Export for video edit: " + err); return; }
        lastFolder = r.mixFile.getParentDirectory(); show.setVisible (true);
        juce::String text;
        text << "Done: " << juce::String ((double) r.lengthSamples / juce::jmax (1.0, app.project.findEdit (editId) != nullptr ? app.project.findEdit (editId)->sampleRate : 48000.0), 2) << " s, "
             << r.pieces << (r.pieces == 1 ? " piece, " : " pieces, ") << r.runs << (r.runs == 1 ? " run of timecode" : " runs of timecode")
             << (r.fps > 0.0 ? " at " + fpsName (r.fps, r.drop) + (r.drop ? "" : " fps") : juce::String()) << ".\n"
             << "   " << r.mixFile.getFileName() << "\n   " << r.ltcFile.getFileName() << "\n   " << r.checkFile.getFileName() << "\n";
        if (r.withoutCode > 0) text << r.withoutCode << (r.withoutCode == 1 ? " piece had no readable timecode (silence there).\n" : " pieces had no readable timecode (silence there).\n");
        if (r.dropped > 0) text << r.dropped << (r.dropped == 1 ? " very short piece was left out of the LTC file (as agreed).\n" : " very short pieces were left out of the LTC file (as agreed).\n");
        for (auto& n : r.notes) text << "   " << n << "\n";
        say (text, r.withoutCode > 0);
        juce::Logger::writeToLog ("Export for video edit: " + text.replace ("\n", " | "));

    }
    void timerCallback() override { if (job != nullptr) progressValue = (double) job->getProgress(); }

    AppContext& app;
    juce::Uuid editId;
    juce::Label title, what, folderCaption, nameCaption, gapCaption, status;
    juce::TextEditor folderEditor, nameEditor, gapEditor;
    juce::TextButton browse { "Browse..." }, go { "Export" }, close { "Close" }, show { "Show files" };
    juce::ToggleButton overdubs { "Include the overdub layer's pieces" };
    double progressValue = 0.0; juce::ProgressBar bar;
    std::unique_ptr<Job> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFolder;
};

/** Opens the dialog (from the edit window's button). */
inline void showVideoExport (AppContext& app, const juce::Uuid& editId, juce::Component* parent)
{
    auto* e = app.project.findEdit (editId);
    if (e == nullptr) return;
    if (e->isEmpty()) { fixtools::say ("Export for video edit", "The edit is empty."); return; }
    fixtools::launchDialog (std::make_unique<VideoExportComponent> (app, editId), "Export for video edit", parent);
}
} // namespace td

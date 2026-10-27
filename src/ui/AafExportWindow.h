#pragma once
#include "AppContext.h"
#include "Uikit.h"
#include "FixTools.h"
#include "../core/AafExport.h"
#include "../core/AudioFormats.h"
#include "../core/Ltc.h"

namespace td
{
/** Export AAF (3.3.0): the dialog. Where the .aaf goes, the timeline's start timecode, whether the overdub layer's pieces go in, Export. The work
    (reading every file's header and its timecode, then writing the file) runs on a thread; the result is shown in the window and logged.
    What the AAF holds is described in core/AafWrite.h and core/AafExport.h. */
class AafExportComponent : public juce::Component, private juce::Timer
{
public:
    AafExportComponent (AppContext& a, const juce::Uuid& eid) : app (a), editId (eid), bar (progressValue)
    {
        auto* e = app.project.findEdit (editId);
        const juce::String editName = e != nullptr ? e->name : juce::String ("edit");
        auto cap = [this] (juce::Label& l, const juce::String& t, bool bold = false)
        {
            l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (13.0f, bold ? juce::Font::bold : juce::Font::plain)); addAndMakeVisible (l);
        };
        cap (title, "Export AAF - " + editName, true); title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        cap (what, "The edit as an AAF for Pro Tools, DaVinci Resolve, Pyramix or any other program that reads one: every track's pieces with their crossfades, "
                   "and the takes' audio files LINKED by their paths - nothing is copied or embedded, so the other program must see the same drive, or relink by file name. "
                   "Each file carries its source timecode: the time of day its first sample was recorded, read from the take's LTC track (a Tentacle box, say), so a camera "
                   "synced to the same box lines up with every clip. The LTC track's own audio is left out (its code goes in as timecode).");
        what.setJustificationType (juce::Justification::topLeft); what.setMinimumHorizontalScale (1.0f);
        cap (fileCaption, "File:"); cap (tcCaption, "Timeline starts at timecode:");
        for (auto* ed : { &fileEditor, &tcEditor }) { addAndMakeVisible (*ed); ed->setSelectAllWhenFocused (true); }
        auto* st = app.props.getUserSettings();
        const auto defaultFolder = app.project.bouncedFolder().getChildFile ("AAF");
        const auto folder = st != nullptr ? juce::File (st->getValue ("aafExportFolder|" + app.project.projectFile.getFullPathName(), defaultFolder.getFullPathName())) : defaultFolder;
        fileEditor.setText (folder.getChildFile (sanitiseForFile (editName) + ".aaf").getFullPathName(), juce::dontSendNotification);
        tcEditor.setText (st != nullptr ? st->getValue ("aafExportStartTc", "01:00:00:00") : "01:00:00:00", juce::dontSendNotification);
        tcEditor.setTooltip ("The timecode of the first sample of the timeline in the other program (hours:minutes:seconds:frames). 01:00:00:00 is the usual.");
        addAndMakeVisible (browse); browse.onClick = [this] { chooseFile(); };
        addAndMakeVisible (overdubs); overdubs.setToggleState (true, juce::dontSendNotification);
        overdubs.setTooltip ("The overdub layer's pieces go in on tracks of their own, named \"<track> OD\" (they play at the same time as the pieces under them, so they cannot share a track).");
        addAndMakeVisible (go); go.onClick = [this] { run(); };
        go.setColour (juce::TextButton::buttonColourId, theme::accent);
        addAndMakeVisible (close); close.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0); };
        addAndMakeVisible (show); show.onClick = [this] { if (lastFile != juce::File()) lastFile.revealToUser(); }; show.setVisible (false);
        addAndMakeVisible (bar); bar.setVisible (false);
        addAndMakeVisible (status); status.setJustificationType (juce::Justification::topLeft); status.setMinimumHorizontalScale (1.0f);
        status.setFont (juce::FontOptions (12.5f));
        setSize (720, 430);
    }
    ~AafExportComponent() override { job.reset(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        what.setBounds (r.removeFromTop (96));
        r.removeFromTop (6);
        auto row = r.removeFromTop (grid::btnH);
        fileCaption.setBounds (row.removeFromLeft (40)); browse.setBounds (row.removeFromRight (90)); row.removeFromRight (6); fileEditor.setBounds (row);
        r.removeFromTop (6); row = r.removeFromTop (grid::btnH);
        tcCaption.setBounds (row.removeFromLeft (190)); tcEditor.setBounds (row.removeFromLeft (110)); row.removeFromLeft (16);
        overdubs.setBounds (row);
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
    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("The AAF to write", juce::File (fileEditor.getText()), "*.aaf");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File()) fileEditor.setText (fc.getResult().withFileExtension ("aaf").getFullPathName(), juce::dontSendNotification);
        });
    }
    void say (const juce::String& text, bool warn)
    {
        status.setColour (juce::Label::textColourId, warn ? theme::warn : theme::text);
        status.setText (text, juce::dontSendNotification);
    }
    /** "01:00:00:00" (or "1:00:00") as seconds; negative when it cannot be read. The frame part is read at 25 fps: it is only the timeline's label. */
    static double parseTimecode (const juce::String& s)
    {
        juce::StringArray parts; parts.addTokens (s.trim(), ":;.", "");
        if (parts.size() < 3 || parts.size() > 4) return -1.0;
        for (auto& p : parts) if (p.trim().isEmpty() || ! p.trim().containsOnly ("0123456789")) return -1.0;
        const double h = parts[0].getDoubleValue(), m = parts[1].getDoubleValue(), sec = parts[2].getDoubleValue(), f = parts.size() == 4 ? parts[3].getDoubleValue() : 0.0;
        if (h >= 24.0 || m >= 60.0 || sec >= 60.0) return -1.0;
        return h * 3600.0 + m * 60.0 + sec + f / 25.0;
    }
    void run()
    {
        if (job != nullptr) { job->cancel(); say ("Cancelling...", false); return; }
        if (app.engine.isRecording()) { say ("Stop recording first.", true); return; }
        auto* e = app.project.findEdit (editId);
        if (e == nullptr) { say ("The edit no longer exists.", true); return; }
        aafexport::Settings st;
        st.file = juce::File (fileEditor.getText().trim());
        if (fileEditor.getText().trim().isEmpty() || st.file.getParentDirectory() == juce::File()) { say ("Choose where the AAF goes.", true); return; }
        if (! st.file.hasFileExtension ("aaf")) st.file = st.file.withFileExtension ("aaf");
        st.timelineStartSeconds = parseTimecode (tcEditor.getText());
        if (st.timelineStartSeconds < 0.0) { say ("The timeline's start timecode should look like 01:00:00:00.", true); return; }
        st.includeOverdubs = overdubs.getToggleState();
        auto plan = std::make_shared<aafexport::Plan> (aafexport::planFor (app.project, *e, st));
        if (plan->error.isNotEmpty()) { say (plan->error, true); return; }
        if (auto* s = app.props.getUserSettings())
        {
            s->setValue ("aafExportFolder|" + app.project.projectFile.getFullPathName(), st.file.getParentDirectory().getFullPathName()); s->setValue ("aafExportStartTc", tcEditor.getText()); app.props.saveIfNeeded();
        }
        say (plan->noLtcTrack ? "Exporting... (the project has no LTC track, so no file will carry a source timecode)" : "Exporting...", plan->noLtcTrack);
        bar.setVisible (true); show.setVisible (false); progressValue = 0.0; go.setButtonText ("Cancel"); close.setEnabled (false);
        for (auto* c : std::initializer_list<juce::Component*> { &fileEditor, &tcEditor, &browse, &overdubs }) c->setEnabled (false);
        startTimerHz (10);
        juce::Component::SafePointer<AafExportComponent> safe (this);
        job = std::make_unique<Job> (std::move (plan), st, [safe] (const aafexport::Result& r) { if (safe != nullptr) safe->finished (r); });
    }
    /** The export on its own thread, like BounceJob: the window owns it and cancels it when it goes. */
    class Job : private juce::Thread
    {
    public:
        Job (std::shared_ptr<aafexport::Plan> p, aafexport::Settings s, std::function<void (const aafexport::Result&)> done)
            : juce::Thread ("Export AAF"), plan (std::move (p)), settings (std::move (s)), onDone (std::move (done))
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
            auto result = aafexport::exportAaf (*plan, settings, formats, &progress, &cancelFlag);
            auto fn = onDone;
            juce::MessageManager::callAsync ([fn, result] { if (fn) fn (result); });
        }
        std::shared_ptr<aafexport::Plan> plan; aafexport::Settings settings;
        std::function<void (const aafexport::Result&)> onDone;
        juce::AudioFormatManager formats;
        std::atomic<float> progress { 0.0f };
        std::atomic<bool> cancelFlag { false };
    };
    void finished (const aafexport::Result& r)
    {
        stopTimer();
        job.reset();
        bar.setVisible (false); go.setButtonText ("Export"); close.setEnabled (true);
        for (auto* c : std::initializer_list<juce::Component*> { &fileEditor, &tcEditor, &browse, &overdubs }) c->setEnabled (true);
        if (r.error.isNotEmpty()) { say (r.error, true); juce::Logger::writeToLog ("Export AAF: " + r.error); return; }
        lastFile = r.file; show.setVisible (true);
        juce::String text;
        text << "Done: " << r.file.getFileName() << " - " << r.tracks << (r.tracks == 1 ? " track, " : " tracks, ") << r.clips << (r.clips == 1 ? " clip, " : " clips, ")
             << r.files << (r.files == 1 ? " linked file" : " linked files") << ", timecode at " << fpsName (r.fps, r.drop) << (r.drop ? "" : " fps") << ".\n";
        if (r.withoutCode > 0) text << r.withoutCode << (r.withoutCode == 1 ? " file has no source timecode (written as 00:00:00:00).\n" : " files have no source timecode (written as 00:00:00:00).\n");
        if (r.unreadable > 0) text << r.unreadable << (r.unreadable == 1 ? " file could not be read here: it is linked by name only.\n" : " files could not be read here: they are linked by name only.\n");
        for (auto& n : r.notes) text << "   " << n << "\n";
        say (text, r.withoutCode > 0 || r.unreadable > 0);
        juce::Logger::writeToLog ("Export AAF: " + text.replace ("\n", " | "));
    }
    void timerCallback() override { if (job != nullptr) progressValue = (double) job->getProgress(); }

    AppContext& app;
    juce::Uuid editId;
    juce::Label title, what, fileCaption, tcCaption, status;
    juce::TextEditor fileEditor, tcEditor;
    juce::TextButton browse { "Browse..." }, go { "Export" }, close { "Close" }, show { "Show file" };
    juce::ToggleButton overdubs { "Include the overdub layer (on tracks of its own)" };
    double progressValue = 0.0; juce::ProgressBar bar;
    std::unique_ptr<Job> job;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFile;
};

/** Opens the dialog (from the edit window's button). */
inline void showAafExport (AppContext& app, const juce::Uuid& editId, juce::Component* parent)
{
    auto* e = app.project.findEdit (editId);
    if (e == nullptr) return;
    if (e->isEmpty()) { fixtools::say ("Export AAF", "The edit is empty."); return; }
    fixtools::launchDialog (std::make_unique<AafExportComponent> (app, editId), "Export AAF", parent);
}
} // namespace td

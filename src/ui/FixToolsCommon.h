#pragma once
// The fix tools' shared parts (3.18.6: FixTools.cpp was split in two; see FixTools.cpp and FixToolsEdits.cpp). Only those two files include this.
// What was 'static' there is 'inline' here: one copy for both files (gMakeProcessPage and gProcessPage among them).
#include "WinOwner.h"
#include "FixTools.h"
#include "BusyWindow.h"
#include "../reharmoniser/ReHarmoniserEditor.h"
#include "../core/ImportPlan.h"
#include "../core/ProjectCopy.h"
#include "../core/Aaf.h"
#include "../core/SpectralPicture.h"
#include "../core/EditFix.h"
#include "../core/SpectralPyramid.h"
#include "../core/BoxEdges.h"
#include "../core/Consonants.h"
#include "TimeReadout.h"
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <limits>
#include "../core/AudioFormats.h"

namespace td { namespace fixtools
{
// ======================================================================================================== helpers

/** 3.18.0: heavy work asks first while Rec mode is on (AppContext::recModeHolds). True = held: the caller stops there. If the person switches Rec off,
    'again' runs - with the window it was asked from, or nullptr if that has gone since. */
inline bool heldByRec (AppContext& app, const juce::String& what, juce::Component* parent, std::function<void (juce::Component*)> again)
{
    juce::Component::SafePointer<juce::Component> sp (parent);
    return app.recModeHolds (what, [sp, again] { again (sp.getComponent()); });
}

/** The program's own little settings file for the things the offline windows remember (their size and place, the way they look). */
inline juce::PropertiesFile& uiPrefs()
{
    static juce::PropertiesFile* f = [] ()
    {
        juce::PropertiesFile::Options o; o.applicationName = "ui-windows"; o.filenameSuffix = ".settings"; o.folderName = "Fermata"; o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML; o.millisecondsBeforeSaving = 0; o.ignoreCaseOfKeyNames = true;
        return new juce::PropertiesFile (o);                     // (kept for the whole run: it saves itself at every change)
    }();
    return *f;
}
/** Remembers where a window was and how big, until the next time it opens (a window that can be resized). */
struct BoundsKeeper : juce::ComponentListener
{
    juce::String key; juce::Rectangle<int> last;
    void componentMovedOrResized (juce::Component& c, bool, bool) override { last = c.getBounds(); }
    void componentBeingDeleted (juce::Component& c) override
    {
        if (last.getWidth() > 100 && last.getHeight() > 100) uiPrefs().setValue ("win." + key, last.toString());
        c.removeComponentListener (this); delete this;
    }
};

/** The Pitch window: two tabs, Pitch curve (first) and Pitch process. The curve page is the whole tool as it always was; the process page is the little semitone / cents dialog. */
class PitchTabs : public juce::Component
{
public:
    PitchTabs (std::unique_ptr<juce::Component> curve, std::unique_ptr<juce::Component> process)
    {
        pages[0] = std::move (curve); pages[1] = std::move (process);
        natural1 = { pages[1]->getWidth(), pages[1]->getHeight() };
        const char* names[] = { "Pitch curve", "Pitch process" };
        for (int i = 0; i < 2; ++i)
        {
            addChildComponent (*pages[(size_t) i]);
            auto& t = tabs[i]; t.setButtonText (names[i]); t.setClickingTogglesState (false); t.setWantsKeyboardFocus (false);
            t.setColour (juce::TextButton::buttonOnColourId, theme::accent);
            t.onClick = [this, i] { show (i); };
            addAndMakeVisible (t);
        }
        setWantsKeyboardFocus (true);
        setSize (juce::jmax (pages[0]->getWidth(), natural1.x), pages[0]->getHeight() + kHead);
        show (0);
    }
    bool keyPressed (const juce::KeyPress& k) override { return pages[(size_t) cur]->keyPressed (k); }
    void resized() override
    {
        auto r = getLocalBounds(); auto head = r.removeFromTop (kHead).reduced (6, 4);
        tabs[0].setBounds (head.removeFromLeft (130)); head.removeFromLeft (4); tabs[1].setBounds (head.removeFromLeft (130));
        pages[0]->setBounds (r);
        pages[1]->setBounds (juce::Rectangle<int> (natural1.x, natural1.y).withCentre ({ r.getCentreX(), r.getY() + natural1.y / 2 + 12 }));
    }
    void paint (juce::Graphics& g) override { g.fillAll (theme::window); }
private:
    static constexpr int kHead = 34;
    void show (int i)
    {
        cur = i; for (int k = 0; k < 2; ++k) tabs[k].setToggleState (k == i, juce::dontSendNotification);          // (by hand: a radio group would run the click action of the tab it turns off, which switches back)
        for (int k = 0; k < 2; ++k) pages[(size_t) k]->setVisible (k == i);
        if (i == 0) pages[0]->grabKeyboardFocus();
    }
    std::unique_ptr<juce::Component> pages[2]; juce::TextButton tabs[2]; juce::Point<int> natural1; int cur = 0;
};

/** A page that only says why it is not available. */
class NotePage : public juce::Component
{
public:
    explicit NotePage (const juce::String& t) : text (t) { setSize (470, 150); }
    void paint (juce::Graphics& g) override { g.setColour (theme::text); g.setFont (juce::FontOptions (14.0f)); g.drawFittedText (text, getLocalBounds().reduced (20), juce::Justification::centred, 6); }
private:
    juce::String text;
};

/** Set while pitchEdit / pitchTake are called only to MAKE the Pitch process page (they put it here instead of opening a window of its own). */
inline bool gMakeProcessPage = false;
inline std::unique_ptr<juce::Component> gProcessPage;
/** The curve page, with the process page beside it as a second tab when there is one (and a note in its place when the two streams of an edit point are open). */
inline std::unique_ptr<juce::Component> withPitchTabs (std::unique_ptr<juce::Component> curve, bool streams)
{
    if (gProcessPage == nullptr) return curve;
    std::unique_ptr<juce::Component> proc;
    if (! streams) { proc = std::move (gProcessPage); gProcessPage.reset(); }
    else proc = std::make_unique<NotePage> ("Pitch process works on the whole marked part at once, so it is not offered while the part contains an edit point (the two streams). "
                                                  "Draw the pitch line in each stream here, or mark a part without the edit point to use Pitch process.");        // (the page stays in gProcessPage for the second stream)
    return std::make_unique<PitchTabs> (std::move (curve), std::move (proc));
}

/** A tool's window whose content may hold its closing back: 'mayClose' is asked when the close button or Escape is pressed, and says false when it has
    asked the user something first (the answer then closes the window itself, with closeDialogOf). As JUCE's own dialog window otherwise. */
class GuardedDialogWindow : public juce::DialogWindow
{
public:
    GuardedDialogWindow (const juce::String& title, juce::Colour bg, float scale) : juce::DialogWindow (title, bg, true, true, scale) {}
    std::function<bool()> mayClose;
    void closeButtonPressed() override { if (mayClose == nullptr || mayClose()) setVisible (false); }
    bool escapeKeyPressed() override { closeButtonPressed(); return true; }
};

/** Opens a tool's window (modal, deleted when closed). 'mayClose' (optional): see GuardedDialogWindow. */

// ======================================================================================================== OUT STREAM / IN STREAM
/** When the marks 1 / 2 contain an edit point, an offline tool opens TWO windows over the same marked part: the OUT STREAM (the take that plays up to the edit, and carries on past it
    through its handle) and the IN STREAM (the take that plays from the edit, and starts before it). Each is corrected on its own and written back into its own piece, so the edit and its
    fades stay exactly where they are and no rendered piece over the join is needed. */
struct StreamSel
{
    bool on = false; juce::String label;
    std::vector<juce::Uuid> ids;                 // the pieces of this stream, in order
    bool extAfter = false, extBefore = false;    // OUT: carries on past its last piece to the end of the marked part; IN: starts before its first piece, at the start of the marked part
    bool has (const juce::Uuid& id) const { return std::find (ids.begin(), ids.end(), id) != ids.end(); }
    /** The stretch of the marked part [from,to) (timeline samples) that piece r of this stream is asked for (what is loaded, and what is corrected). */
    void span (const EditRegion& r, juce::int64 from, juce::int64 to, juce::int64& lo, juce::int64& hi) const
    {
        const bool first = ! ids.empty() && r.id == ids.front(), last = ! ids.empty() && r.id == ids.back();
        lo = (extBefore && first) ? from : juce::jmax (from, r.startSample);
        hi = (extAfter && last) ? to : juce::jmin (to, r.endSample());
        lo = juce::jmax (lo, r.startSample - r.srcIn);                                       // not before the first sample of the take
        if (r.sourceLength > 0) hi = juce::jmin (hi, r.startSample + (r.sourceLength - r.srcIn));
    }
};

inline juce::String regionsSignature (const std::vector<EditRegion>& v)
{
    juce::String s;
    for (auto& r : v) s << r.id.toString() << ':' << r.startSample << ':' << r.srcIn << ':' << r.srcOut << ':' << r.inStart << ':' << r.inEnd << ':' << r.outStart << ':' << r.outEnd << ';';
    return s;
}

/** What a stream window holds: a coloured banner that says which stream it is, and the tool's own window below it. */
class StreamFrame : public juce::Component
{
public:
    StreamFrame (std::unique_ptr<juce::Component> content, juce::Colour tint, juce::String big, juce::String smallCaption)
        : inner (std::move (content)), colour (tint), bigText (std::move (big)), smallText (std::move (smallCaption))
    {
        addAndMakeVisible (*inner);
        setSize (inner->getWidth(), inner->getHeight() + kBanner);
    }
    void focusInner() { inner->grabKeyboardFocus(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().removeFromTop (kBanner);
        g.setColour (colour); g.fillRect (r);
        g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        const int w = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)), bigText);
        g.drawText (bigText, r.withTrimmedLeft (14).withWidth (w + 8), juce::Justification::centredLeft);
        g.setColour (juce::Colours::white.withAlpha (0.9f)); g.setFont (juce::FontOptions (13.0f));
        g.drawText (smallText, r.withTrimmedLeft (14 + w + 24).withTrimmedRight (10), juce::Justification::centredLeft, true);
    }
    void resized() override { auto r = getLocalBounds(); r.removeFromTop (kBanner); inner->setBounds (r); }
    static constexpr int kBanner = 38;
    std::unique_ptr<juce::Component> inner;
private:
    juce::Colour colour; juce::String bigText, smallText;
};

struct StreamPair : std::enable_shared_from_this<StreamPair>
{
    AppContext* app = nullptr; juce::Uuid eid; std::vector<EditRegion> base; juce::String undoLabel, title;
    std::function<std::vector<RegionJob>*()> jobsOf[2]; juce::String suffix[2];
    bool on[2] = { false, false };                           // which stream's corrected pieces are in the edit just now
    std::function<bool()> commit[2]; bool written[2] = { false, false };
    std::unique_ptr<juce::DialogWindow> win[2];
    bool closing = false, committed = false;
    juce::String lastSig;

    /** The edit = the original + the corrected pieces of every stream that is 'on' (always computed from the original, so the order does not matter). False if the edit was changed by hand meanwhile. */
    bool refresh()
    {
        auto* e = app->project.findEdit (eid); if (e == nullptr) return false;
        if (regionsSignature (e->regions) != lastSig) return false;
        e->regions = base;
        bool ok = true;
        for (int s = 0; s < 2; ++s)
        {
            if (! on[s]) continue;
            auto* jb = jobsOf[s] ? jobsOf[s]() : nullptr;
            const auto cur = e->regions;
            if (jb == nullptr || jb->empty() || ! applyRegionJobs (*e, cur, *jb, suffix[s])) { e->regions = cur; on[s] = false; ok = false; }
        }
        app->project.changed();
        lastSig = regionsSignature (e->regions);
        return ok;
    }
    bool put (int s, bool want)
    {
        if (on[s] == want) return true;
        on[s] = want;
        return refresh();
    }
    void focus (int s)
    {
        if (win[s] == nullptr) return;
        win[s]->toFront (true);
        if (auto* f = dynamic_cast<StreamFrame*> (win[s]->getContentComponent())) f->focusInner();
    }
    /** Both windows close and, unless everything was written, nothing of what was made is kept. */
    void closeAll()
    {
        if (closing) return;
        closing = true;
        auto self = shared_from_this();
        juce::MessageManager::callAsync ([self]
        {
            self->win[0].reset(); self->win[1].reset();                      // (a window that is closed without being written puts everything back itself)
            if (! self->committed)
            {
                for (int q = 0; q < 2; ++q)                                  // a stream that was written back is only kept when the other one is too
                    if (self->written[q]) if (auto* jb = self->jobsOf[q] ? self->jobsOf[q]() : nullptr) { for (auto& rj : *jb) for (auto& r : rj.results) r.file.deleteFile(); jb->clear(); }
                self->on[0] = self->on[1] = false;
                self->refresh();
            }
        });
    }
    /** A stream window's 'Write back': that window goes away (the keyboard then belongs to the other one) and its corrected audio waits. When both streams have been written back,
        both go into the edit together, as one undo step. Closing the remaining window instead cancels everything. */
    void wrote (int s, std::function<bool()> c)
    {
        commit[s] = std::move (c); written[s] = true;
        if (! (written[0] && written[1]))
        {
            auto self = shared_from_this();
            juce::MessageManager::callAsync ([self, s] { if (self->closing) return; self->win[s].reset(); self->focus (1 - s); });
            return;
        }
        if (committed) return;
        committed = true;
        for (int q = 0; q < 2; ++q) { auto* jb = jobsOf[q] ? jobsOf[q]() : nullptr; on[q] = jb != nullptr && ! jb->empty(); }
        bool any = on[0] || on[1];
        bool ok = true;
        if (any) ok = refresh();
        if (ok)
        {
            for (int q = 0; q < 2; ++q) { if (commit[q]) commit[q](); if (auto* jb = jobsOf[q] ? jobsOf[q]() : nullptr) for (auto& rj : *jb) rj.results.clear(); }      // (the new files are kept now)
            if (any) { app->fixUndoLabel = undoLabel; app->fixUndo = [a = app, id = eid, old = base] { if (auto* e = a->project.findEdit (id)) { e->regions = old; a->project.changed(); } }; }
        }
        else
        {
            if (auto* e = app->project.findEdit (eid)) { e->regions = base; app->project.changed(); }
            for (int q = 0; q < 2; ++q) if (auto* jb = jobsOf[q] ? jobsOf[q]() : nullptr) { for (auto& rj : *jb) for (auto& r : rj.results) r.file.deleteFile(); jb->clear(); }
            for (int q = 0; q < 2; ++q) on[q] = false;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle (title)
                                              .withMessage ("The edit was changed while the two streams were open (or the corrected audio no longer fits it), so nothing was changed. Please try again.").withButton ("OK"), nullptr);
        }
        closing = true;
        auto self = shared_from_this();
        juce::MessageManager::callAsync ([self] { self->win[0].reset(); self->win[1].reset(); });
    }
};

class StreamWindow : public juce::DialogWindow
{
public:
    StreamWindow (const juce::String& t, juce::Colour bg, std::weak_ptr<StreamPair> p) : juce::DialogWindow (t, bg, true, true), pair (std::move (p)) {}
    ~StreamWindow() override { removeMouseListener (&raiser); }
    void closeButtonPressed() override { if (auto p = pair.lock()) p->closeAll(); }
    /** Escape does nothing here (JUCE's dialog window hid itself on Escape, which left this one of the pair hidden but open, with its fixes in the edit; closing
        both windows throws away what has not been written back, so that is left to the close button or Cancel). */
    bool escapeKeyPressed() override { return true; }
    /** Clicking Stop / Play (or anything else in the tool) must never send the window behind the main window: after every click the window is put back on top, without taking the focus.
        (A separate listener that only looks at mouse-ups: the window itself must NOT get the mouse events of its children, or a drag on the spectrogram would move the window.) */
    struct RaiseAfterClick : public juce::MouseListener
    {
        explicit RaiseAfterClick (juce::Component& o) : owner (&o) {}
        void mouseUp (const juce::MouseEvent&) override
        {
            juce::Component::SafePointer<juce::Component> self (owner);
            juce::Timer::callAfterDelay (120, [self] { if (self != nullptr && self->isVisible()) self->toFront (false); });
        }
        juce::Component* owner;
    };
    RaiseAfterClick raiser { *this };
    std::weak_ptr<StreamPair> pair;
};

/** The two windows side by side, each tinted (orange = the out stream, as in the trim window; blue = the in stream); closing either one closes both. */
inline void launchStream (const std::shared_ptr<StreamPair>& pair, int slot, std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around)
{
    const auto tint = slot == 0 ? juce::Colour (0xffc4511a) : juce::Colour (0xff1f6f8b);
    auto w = std::make_unique<StreamWindow> (title, theme::window.interpolatedWith (tint, 0.28f), pair);
    const juce::String big = slot == 0 ? "OUT STREAM" : "IN STREAM";
    const juce::String smallCaption = slot == 0 ? "the take that plays UP TO the edit point (carried on past it, where its crossfade uses it)  -  Write back closes this window; the other stream is then yours"
                                         : "the take that plays FROM the edit point (started before it, where its crossfade uses it)  -  Write back closes this window; the other stream is then yours";
    w->setContentOwned (new StreamFrame (std::move (content), tint.darker (0.15f), big, smallCaption), true);
    w->setUsingNativeTitleBar (true);
    w->setResizable (true, true);
    juce::Rectangle<int> area (0, 0, 1900, 1000);
    if (around != nullptr) area = juce::Desktop::getInstance().getDisplays().getDisplayForRect (around->getScreenBounds())->userArea;
    else if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) area = d->userArea;
    const int half = area.getWidth() / 2;
    w->setBounds (juce::Rectangle<int> (area.getX() + (slot == 0 ? 0 : half), area.getY(), half, area.getHeight()).reduced (4));
    w->setVisible (true);
    w->addMouseListener (&w->raiser, true);                      // (so that a click on any button inside it is seen: see StreamWindow::RaiseAfterClick)
    if (around != nullptr) if (auto* top = around->getTopLevelComponent()) makeOwnedBy (*w, *top);        // an owned window always stays above the window that owns it
    w->toFront (true);
    pair->win[slot] = std::move (w);
}

/** 0 = no edit point between the marks (the tool works as it always did), 1 = exactly one (the two streams), -1 = more than one (a message was shown). */
inline int detectStreams (const EditDef& e, juce::int64 from, juce::int64 to, StreamSel& out, StreamSel& in, const juce::String& title)
{
    std::vector<int> touched;
    for (int i = 0; i < (int) e.regions.size(); ++i) if (e.regions[(size_t) i].endSample() > from && e.regions[(size_t) i].startSample < to) touched.push_back (i);
    int join = -1, joins = 0;
    for (size_t q = 1; q < touched.size(); ++q) if (touched[q] == touched[q - 1] + 1 && ! e.isFixJoin (touched[q])) { ++joins; join = (int) q; }
    if (joins == 0) return 0;
    if (joins > 1) { say (title, "The marked part contains " + juce::String (joins) + " edit points. Two streams (OUT and IN) can be corrected at one edit point at a time: mark a shorter part with only one edit point in it."); return -1; }
    out = StreamSel(); in = StreamSel();
    out.on = in.on = true; out.label = "OUT STREAM"; in.label = "IN STREAM"; out.extAfter = true; in.extBefore = true;
    for (int q = 0; q < (int) touched.size(); ++q) (q < join ? out : in).ids.push_back (e.regions[(size_t) touched[(size_t) q]].id);
    return 1;
}

inline std::shared_ptr<StreamPair> makePair (AppContext& app, const juce::Uuid& eid, const juce::String& title, const juce::String& undoLabel)
{
    auto p = std::make_shared<StreamPair>();
    p->app = &app; p->eid = eid; p->title = title; p->undoLabel = undoLabel;
    if (auto* e = app.project.findEdit (eid)) p->base = e->regions;
    p->lastSig = regionsSignature (p->base);
    return p;
}

inline void closeDialogOf (juce::Component* c)
{
    if (auto* dw = c->findParentComponentOfClass<juce::DialogWindow>())
    {
        if (auto* sw = dynamic_cast<StreamWindow*> (dw)) { sw->closeButtonPressed(); return; }          // one stream window closes the pair
        dw->exitModalState (0);
    }
}


inline juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager fm; static bool done = false;
    if (! done) { td::registerAudioFormats (fm); done = true; }
    return fm;
}

/** The common look of a dialog: a title line, a progress bar and the Apply / Cancel buttons are laid out by each dialog itself. */
class JobDialogBase : public juce::Component, protected juce::Timer
{
public:
    JobDialogBase() : bar (progressValue)
    {
        getProperties().set (ownsRKeyProperty(), true);     // R here runs the window's own job (audition / do the fix): it does not start a recording
        addChildComponent (bar); addAndMakeVisible (status);
        status.setColour (juce::Label::textColourId, theme::warn); status.setMinimumHorizontalScale (1.0f);
    }
    ~JobDialogBase() override { stopTimer(); job.release(); }          // (asks the work to stop and never waits for it: see AudioJob::release)
protected:
    void timerCallback() override { progressValue = (double) job.progress(); }
    /** 'ifGone' (optional) runs instead of 'done' when the window has been closed before the job ended: it tidies away what the job made (its files). */
    void runJob (AudioJob::Work w, std::function<void (const juce::String&)> done, std::function<void (const juce::String&)> ifGone = {})
    {
        bar.setVisible (true); progressValue = 0.0; startTimerHz (10);
        juce::Component::SafePointer<JobDialogBase> self (this);
        job.start (std::move (w), [self, done, ifGone] (const juce::String& err)
        {
            if (self == nullptr) { if (ifGone) ifGone (err); return; }
            self->stopTimer(); self->bar.setVisible (false);
            self->running = false; self->jobEnded (err);
            if (done) done (err);
        });
        running = true;
    }
    virtual void jobEnded (const juce::String&) {}
    AudioJob& job = *new AudioJob();                                   // (handed back with release() in the destructor: it may outlive this window)
    bool running = false;
    double progressValue = 0.0; juce::ProgressBar bar; juce::Label status;
};


// ======================================================================================================== the repair dialog
/** One change made in the repair window: what to do and where (samples from the start of the loaded area). */
struct RepairOp { FixSpec spec; juce::int64 t0 = 0, t1 = 0; juce::int64 placed = -1; };      // placed: a pasted consonant - where it was first put (its nudges are counted from there)

struct RepairContext
{
    juce::String title, what;
    double rate = 48000.0;
    juce::int64 length = 0;                                                             // samples in the scanned area
    std::function<std::vector<std::vector<float>> (AudioJob&)> load;                     // job thread: one mono vector per track, 'length' samples
    std::function<juce::String (const std::vector<RepairOp>&, AudioJob&)> prepare;      // job thread: makes the repaired files with ALL these changes, covering the whole loaded area; nothing is changed yet
    std::function<bool()> apply;                                                        // message thread: puts the prepared files in place (so playing / the edit window use them)
    std::function<void()> unapply, discard, accept, stop;                               // message thread: back to the original / delete the prepared files / keep what is in place / stop playing
    std::function<void (double fromSec, double toSec, bool loop)> play;                 // message thread: plays from..to (seconds from the start of the area), optionally looped
    std::function<void (bool)> setLooping, hold;                                        // message thread
    std::function<double()> position;                                                   // message thread: seconds from the start of the area, or < 0 when this is not playing
    std::function<void (std::function<bool()>)> pairWritten;                            // one stream window of an OUT / IN pair: 'Write back' only makes this stream ready (the pair puts both into the edit together)
    std::vector<denoise::Profile> roomMono;                                             // the project's room tone for each picture track (the mono mix; an invalid profile = none for that track)
    juce::String roomSource;                                                            // where the project's room tone came from ("" = none learned)
    // ---- consonants (3.6.0): copied per track and channel, kept with the project, pasted on top ----
    std::vector<juce::Uuid> trackIds; std::vector<juce::String> trackNames;            // the picture's tracks, in its order (one row of 'load' each)
    std::function<std::vector<std::vector<std::vector<float>>> (juce::int64 a, juce::int64 b, AudioJob&)> readChannels;   // job thread: [a, b) of the area, per track ALL its channels, as the files are (before this window's fixes)
    juce::String where; double areaStart = 0.0;                                         // the take or edit, and the time of the area's start in it (for a kept consonant's "from")
    std::function<std::vector<ConsonantDef>()> consonantList;                           // message thread: the project's kept consonants
    std::function<juce::String (const ConsonantAudio&, const juce::String& name, const juce::String& from, juce::Uuid& id)> keepConsonant;   // message thread: keeps one (its files and the list); "" or why not
    std::function<void (const juce::Uuid&, const juce::String&)> renameConsonant;
    std::function<void (const juce::Uuid&)> forgetConsonant;
    std::function<std::shared_ptr<const ConsonantAudio> (const juce::Uuid&, juce::String& error)> loadConsonant;
};

// ======================================================================================================== applying a fix to the model
inline juce::File takeFixDest (const juce::File& orig, const FixSpec& spec)
{
    auto stem = orig.getFileNameWithoutExtension();
    const int p = stem.lastIndexOf (" (");
    if (p > 0 && stem.endsWith (")") && (stem.substring (p).containsAnyOf ("+-") || stem.contains ("repair") || stem.contains ("declick") || stem.contains ("pitch curve") || stem.contains ("consonant"))) stem = stem.substring (0, p);
    return audioops::uniqueFile (orig.getParentDirectory().getChildFile (stem + " (" + spec.shortName() + ").wav"));
}

inline void setUndoForTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, std::vector<TakeFile> oldFiles, const juce::String& label)
{
    app.armFixUndo (label, [&app, wid, gid, oldFiles]
    {
        if (auto* w = app.project.findTakeWindow (wid)) if (auto* g = w->findGroup (gid)) { g->files = oldFiles; app.project.changed(); }
    }, false, wid, gid);
}

inline void setUndoForEdit (AppContext& app, const juce::Uuid& eid, std::vector<EditRegion> oldRegions, const juce::String& label)
{
    app.armFixUndo (label, [&app, eid, oldRegions]
    {
        if (auto* e = app.project.findEdit (eid)) { e->regions = oldRegions; app.project.changed(); }
    }, true, eid);
}

inline juce::File editFixDest (const juce::File& folder, const EditRegion& r, const RegionFile& f, const FixSpec& spec)
{
    return audioops::uniqueFile (folder.getChildFile (sanitiseForFile (r.takeName.upToFirstOccurrenceOf (" (", false, false) + " - " + f.trackName) + " (" + spec.shortName() + ").wav"));
}

/** Prepares the file lists of one piece [a,b) of a region (message thread). */
inline bool prepRegionJob (const juce::File& folder, const EditRegion& r, RegionJob& j, juce::int64 a, juce::int64 b, juce::int64 outFrom, juce::int64 outTo,
                           juce::int64 fixFrom, juce::int64 fixTo, const FixSpec& spec, const std::vector<juce::Uuid>& onlyTracks = {})
{
    j.regionId = r.id; j.a = a; j.b = b; j.outFrom = outFrom; j.outTo = outTo; j.fixFrom = fixFrom; j.fixTo = fixTo;
    j.ops = { audioops::FixOp { spec, fixFrom, fixTo } };
    for (size_t fi = 0; fi < r.files.size(); ++fi)
    {
        auto& f = r.files[fi];
        if (! onlyTracks.empty() && std::find (onlyTracks.begin(), onlyTracks.end(), f.trackId) == onlyTracks.end()) continue;
        j.idx.push_back (fi); j.trackOf.push_back (f.trackId);
        j.src.push_back (f.file); j.fileStart.push_back (f.fileStart); j.dest.push_back (editFixDest (folder, r, f, spec));
        // two files of one region must not get the same new name
        for (size_t k = 0; k + 1 < j.dest.size(); ++k) if (j.dest[k] == j.dest.back()) j.dest.back() = audioops::uniqueFile (j.dest.back().getSiblingFile (j.dest.back().getFileNameWithoutExtension() + " b.wav"));
    }
    return ! j.src.empty();
}

/** Job thread: makes the new short files of one region job. */
inline juce::String runRegionJob (RegionJob& j, AudioJob& job, float p0, float p1)        // (what to do is in j.ops, made by prepRegionJob)
{
    j.results.clear();
    for (size_t k = 0; k < j.src.size(); ++k)
    {
        if (job.cancelled()) { for (auto& r : j.results) r.file.deleteFile(); j.results.clear(); return "Cancelled."; }
        job.setProgress (p0 + (p1 - p0) * (float) k / (float) j.src.size());
        juce::String err;
        auto ops = j.ops;                                       // a denoise: this file's own room-tone profiles go into its ops
        for (auto& o : ops) if (o.spec.kind == FixSpec::Kind::Denoise) o.spec.dnProfiles = j.profilesFor ? j.profilesFor (k, o.spec) : nullptr;
        for (auto& o : ops) if (o.spec.kind == FixSpec::Kind::Paste) audioops::resolvePaste (o.spec, k < j.trackOf.size() ? j.trackOf[k] : juce::Uuid::null());     // (its own track's channels of a pasted consonant)
        auto res = audioops::fixPiece (formats(), j.src[k], j.fileStart[k], j.outFrom, j.outTo, ops, j.dest[k], err);
        if (! res.ok) { for (auto& r : j.results) r.file.deleteFile(); j.results.clear(); return err; }
        j.results.push_back (res);
    }
    return {};
}

/** The denoiser's settings as a window hands them to the file code: its parameters and the boxes it learns the room tone from (seconds from the start of the area). */
struct SpectralDenoiseSettings
{
    denoise::Params params;
    std::vector<denoise::SelRect> sel;                        // the boxes the room tone was learned from (its time ranges and its part of the spectrum)
};

/** The tracks of an edit between 'from' and 'to' and, for each, where its audio comes from along that stretch: whoever owns each moment (the piece that is on
    top there). 'only' (Ctrl + Alt + drag) limits the tracks. Built on the message thread from the regions as they are now; read on any thread. */
struct TrackPart { juce::File file; juce::int64 fileStart = 0, srcA = 0, n = 0, dst = 0; };     // srcA: the take sample the part starts at; dst: the sample of the stretch it lands on
struct TrackParts
{
    std::vector<juce::Uuid> ids; std::vector<juce::String> names; std::vector<std::vector<TrackPart>> parts;
    int pieces = 0;
};
inline TrackParts buildTrackParts (const EditDef& e, juce::int64 from, juce::int64 to, const std::vector<juce::Uuid>& only)
{
    TrackParts tp;
    for (auto& r : e.regions) if (r.endSample() > from && r.startSample < to)
    {
        ++tp.pieces;
        for (auto& f : r.files)
        {
            if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
            if (std::find (tp.ids.begin(), tp.ids.end(), f.trackId) == tp.ids.end()) { tp.ids.push_back (f.trackId); tp.names.push_back (f.trackName); tp.parts.emplace_back(); }
        }
    }
    std::vector<juce::int64> cuts { from, to };
    for (auto& r : e.regions) { if (r.startSample > from && r.startSample < to) cuts.push_back (r.startSample); if (r.endSample() > from && r.endSample() < to) cuts.push_back (r.endSample()); }
    std::sort (cuts.begin(), cuts.end()); cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        const auto t0 = cuts[k], t1 = cuts[k + 1];
        const int ri = e.regionAt (t0 + (t1 - t0) / 2);
        if (ri < 0) continue;
        const auto& r = e.regions[(size_t) ri];
        for (auto& f : r.files)
        {
            const auto ti = std::find (tp.ids.begin(), tp.ids.end(), f.trackId);
            if (ti == tp.ids.end()) continue;
            tp.parts[(size_t) (ti - tp.ids.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (t0 - r.startSample), t1 - t0, t0 - from });
        }
    }
    return tp;
}
/** The mono mix of track i over [at, at + n) of the stretch (samples from its start): silence where no piece is heard. Readers are opened as needed
    ('cache', optional, keeps every reader it opened, for the next call). */
struct ReaderCache { std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> readers; std::mutex lock; };
inline void readTrackMix (const std::vector<TrackPart>& parts, juce::int64 at, juce::int64 n, float* dst, ReaderCache* cache = nullptr)
{
    std::fill (dst, dst + n, 0.0f);
    for (auto& pt : parts)
    {
        const auto p0 = juce::jmax (at, pt.dst), p1 = juce::jmin (at + n, pt.dst + pt.n);     // the part of this piece inside the range (stretch samples)
        if (p1 <= p0) continue;
        juce::int64 rel = pt.srcA - pt.fileStart + (p0 - pt.dst), cnt = p1 - p0, o = p0 - at;
        if (rel < 0) { cnt += rel; o -= rel; rel = 0; }
        if (cnt <= 0) continue;
        std::vector<std::vector<float>> ch;
        if (cache != nullptr)
        {
            std::lock_guard<std::mutex> l (cache->lock);
            auto& r = cache->readers[pt.file.getFullPathName()];
            if (r == nullptr) r = audioops::openReader (formats(), pt.file);
            if (r == nullptr) continue;
            audioops::readRange (*r, rel, cnt, ch);
        }
        else { auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue; audioops::readRange (*r, rel, cnt, ch); }
        for (auto& x : ch) for (size_t k = 0; k < x.size(); ++k) dst[(size_t) o + k] += x[k] / (float) ch.size();
    }
}
/** The files' room-tone profiles, per track per channel, from the parts of them that lie under the boxes (the boxes' times are seconds from the start of the
    stretch). Empty + error if a track's files hold nothing under the boxes. */
inline juce::String learnFileProfiles (const TrackParts& tp, const SpectralDenoiseSettings& s, double rate, std::vector<std::vector<denoise::Profile>>& out, AudioJob& j)
{
    out.assign (tp.ids.size(), {});
    // only the boxes are read from each piece (with a quarter of a second more each side, for the analysis window): the piece under a box can be a whole
    // movement, tens of minutes of every track, which was all read just to learn from a few seconds of it
    std::vector<std::pair<juce::int64, juce::int64>> reads;                                   // (samples of the stretch tp covers, merged)
    for (auto& sp : denoise::selectedSpans (s.sel))
    {
        const auto a = juce::jmax ((juce::int64) 0, (juce::int64) std::floor ((sp.first - 0.25) * rate)), b = (juce::int64) std::ceil ((sp.second + 0.25) * rate);
        if (! reads.empty() && a <= reads.back().second) reads.back().second = juce::jmax (reads.back().second, b); else reads.push_back ({ a, b });
    }
    for (size_t i = 0; i < tp.parts.size(); ++i)
    {
        std::vector<denoise::ProfileLearner> learners;
        for (auto& pt : tp.parts[i])
            for (auto& rd : reads)
            {
                if (j.cancelled()) return "Cancelled.";
                juce::int64 d0 = juce::jmax (pt.dst, rd.first), d1 = juce::jmin (pt.dst + pt.n, rd.second);      // the part of this piece the box reaches
                if (d1 <= d0) continue;
                juce::int64 rel = pt.srcA - pt.fileStart + (d0 - pt.dst);                                          // (its file sample)
                if (rel < 0) { d0 -= rel; rel = 0; }
                if (d1 <= d0) continue;
                auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, d1 - d0, ch);
                if (learners.size() < ch.size()) learners.resize (ch.size());
                for (size_t cc = 0; cc < ch.size(); ++cc) learners[cc].add (ch[cc].data(), (juce::int64) ch[cc].size(), rate, s.params, s.sel, (double) d0 / rate);
            }
        for (auto& l : learners) { if (l.covered() == 0) return "The boxes lie over a part of the edit that has no audio on " + tp.names[i] + "."; out[i].push_back (l.finish()); }
        if (out[i].empty()) return "The boxes do not reach any audio of " + tp.names[i] + ".";
    }
    return {};
}
/** A denoise among the changes of an edit's pieces: gives every job the room-tone profiles of its files - the project's for the file's track, or, for a
    change whose room tone is a box of the loaded area, profiles learned from the real files under that box (once per box). The box's time is seconds from
    the start of the area 'tp' covers. */
inline juce::String attachRoomTone (std::vector<RegionJob>& jobs, const std::vector<RepairOp>& ops, const std::vector<EditRegion>& regions, const TrackParts& tp,
                                    std::shared_ptr<const Project::RoomTone> projectRT, double rate, AudioJob& j)
{
    bool any = false; for (auto& o : ops) any = any || o.spec.kind == FixSpec::Kind::Denoise;
    if (! any) return {};
    // each box used as the room tone (usually one), learned once: per track, one profile per channel
    auto boxes = std::make_shared<std::map<std::pair<double, double>, std::vector<std::vector<denoise::Profile>>>>();
    for (auto& o : ops)
    {
        if (o.spec.kind != FixSpec::Kind::Denoise || ! (o.spec.dnBoxT1 > o.spec.dnBoxT0)) continue;
        const auto key = std::make_pair (o.spec.dnBoxT0, o.spec.dnBoxT1);
        if (boxes->count (key)) continue;
        SpectralDenoiseSettings s; s.params = o.spec.dn; s.sel = { denoise::SelRect { o.spec.dnBoxT0, o.spec.dnBoxT1, 0.0, 1.0e9, true } };
        std::vector<std::vector<denoise::Profile>> out;
        const auto err = learnFileProfiles (tp, s, rate, out, j);
        if (err.isNotEmpty()) return err;
        for (auto& t : out) for (auto& pr : t) pr.weight.clear();                 // (the whole spectrum: the box being denoised sets the range)
        (*boxes)[key] = std::move (out);
    }
    bool usesProject = false; for (auto& o : ops) usesProject = usesProject || (o.spec.kind == FixSpec::Kind::Denoise && ! (o.spec.dnBoxT1 > o.spec.dnBoxT0));
    auto ids = std::make_shared<std::vector<juce::Uuid>> (tp.ids);
    for (auto& rj : jobs)
    {
        const EditRegion* r = nullptr; for (auto& x : regions) if (x.id == rj.regionId) r = &x;
        if (r == nullptr) continue;
        auto trackOfFile = std::make_shared<std::vector<juce::Uuid>>();
        for (auto fi : rj.idx) trackOfFile->push_back (fi < r->files.size() ? r->files[fi].trackId : juce::Uuid::null());
        if (usesProject)                                                          // the project's room tone must have every track of the piece, at this rate (a track
            for (auto fi : rj.idx)                                                // without one used to be left as it was, though the denoise said it was done)
            {
                if (fi >= r->files.size()) continue;
                const auto& f = r->files[fi];
                const auto* t = projectRT != nullptr ? projectRT->forTrack (f.trackId) : nullptr;
                if (t == nullptr || t->perChannel.empty() || ! t->perChannel.front().valid())
                    return "The project's room tone has nothing for " + f.trackName + ". Learn it again with the edit window's Room tone button, "
                           "or draw a box over room only and press L (or leave that track out with Ctrl + Alt + drag).";
                if (std::abs (t->perChannel.front().sampleRate - rate) > 0.5)
                    return "The project's room tone for " + f.trackName + " was learned at " + juce::String (t->perChannel.front().sampleRate / 1000.0, 1) + " kHz, but this audio is at "
                           + juce::String (rate / 1000.0, 1) + " kHz. Learn it again with the edit window's Room tone button, or draw a box over room only and press L.";
            }
        rj.profilesFor = [boxes, ids, trackOfFile, projectRT] (size_t k, const FixSpec& spec) -> std::shared_ptr<const std::vector<denoise::Profile>>
        {
            if (k >= trackOfFile->size()) return nullptr;
            const auto tid = (*trackOfFile)[k];
            if (spec.dnBoxT1 > spec.dnBoxT0)
            {
                auto it = boxes->find (std::make_pair (spec.dnBoxT0, spec.dnBoxT1));
                if (it == boxes->end()) return nullptr;
                for (size_t i = 0; i < ids->size() && i < it->second.size(); ++i) if ((*ids)[i] == tid && ! it->second[i].empty()) return std::make_shared<const std::vector<denoise::Profile>> (it->second[i]);
                return nullptr;
            }
            if (projectRT == nullptr) return nullptr;
            if (auto* t = projectRT->forTrack (tid)) if (! t->perChannel.empty()) return std::make_shared<const std::vector<denoise::Profile>> (t->perChannel);
            return nullptr;
        };
    }
    return {};
}
/** One repair / de-click / gain job per piece of 'regions' between 'from' and 'to': the changes that fall inside each piece, in its own samples (as the
    repair window's prepare, without the OUT / IN streams), the files made (job thread). */
inline juce::String runRepairJobs (std::vector<RegionJob>& jobs, AudioJob& j)
{
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto err = runRegionJob (jobs[i], j, (float) i / (float) jobs.size(), (float) (i + 1) / (float) jobs.size());
        if (err.isNotEmpty()) { for (auto& rj : jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs.clear(); return err; }
    }
    return {};
}
/** Job thread: the lost samples of every change that looks for them (De-click, or a box round a click), found on all the tracks of its piece together - they are
    one event, at the same sample on every track, and the finder wants a jump seen on a fifth of the tracks before it takes it for one (on its own, each file took
    any odd sample of its own for one, and the tracks were then mended differently: their timing no longer agreed where the time squeeze put samples back).
    Read five seconds at a time, every track, so a De-click over a whole edit does not need it all in memory. */
inline juce::String findLostTogether (std::vector<RegionJob>& jobs, double rate, AudioJob& j)
{
    const auto pad = (juce::int64) (0.25 * rate), chunk = (juce::int64) (5.0 * rate);
    juce::int64 total = 0, done = 0;                                          // (for the progress bar)
    for (auto& rj : jobs) for (auto& op : rj.ops) if (! op.spec.lostKnown && audioops::findsLost (op.spec, rate, op.s0, op.s1)) total += op.s1 - op.s0;
    for (auto& rj : jobs)
        for (auto& op : rj.ops)
        {
            if (op.spec.lostKnown || ! audioops::findsLost (op.spec, rate, op.s0, op.s1)) continue;
            std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
            for (auto& f : rj.src) readers.push_back (audioops::openReader (formats(), f));
            std::vector<FixSpec::Lost> lost;
            for (juce::int64 w0 = op.s0; w0 < op.s1; w0 += chunk)
            {
                if (j.cancelled()) return "Cancelled.";
                if (total > 0) j.setProgress ((float) ((double) done / (double) total));
                done += juce::jmin (chunk, op.s1 - w0);
                const auto w1 = juce::jmin (op.s1, w0 + chunk), c0 = w0 - pad, c1 = w1 + pad;            // (take samples: the stretch searched, and what is read round it)
                std::vector<std::vector<float>> all;
                for (size_t k = 0; k < readers.size(); ++k)
                {
                    if (readers[k] == nullptr) continue;
                    std::vector<std::vector<float>> ch;
                    if (! audioops::readRange (*readers[k], c0 - rj.fileStart[k], c1 - c0, ch)) return "Could not read " + rj.src[k].getFileName();
                    for (auto& c : ch) all.push_back (std::move (c));
                }
                if (all.empty()) continue;
                for (auto l : audioops::findLost (all, rate, (long) (w0 - c0), (long) (w1 - c0), op.spec.sensitivity)) { l.at += w0 - op.s0; lost.push_back (l); }
            }
            op.spec.lost = std::move (lost); op.spec.lostKnown = true;
        }
    return {};
}

/** Noise mode's fixes: one job per piece of 'regions' between 'from' and 'to' that a change reaches. The piece put into the edit is the span of the changes in
    that region with a quarter of a second more each side (within the region): the joins - 30 ms cross-fades with the take as it was - then lie on sound the fix
    did not change. (The piece used to be just the box: a box shorter than 60 ms, about the size of a click, was left out of the edit altogether although the fix
    said it was done, and the outer 15 ms of a longer box kept some of the old sound in the cross-fades.) The time squeeze that puts lost samples back stays 35 ms
    clear of the piece's ends, so the joins cross-fade sound with the same timing. */
inline juce::String prepareRepairJobs (const juce::File& fixFolder, const std::vector<EditRegion>& regions, juce::int64 from, juce::int64 to, double rate, const std::vector<RepairOp>& ops,
                                       const std::vector<juce::Uuid>& only, std::vector<RegionJob>& jobs, AudioJob& j, bool makeFiles = true)
{
    if (ops.empty()) return "Nothing to do.";
    const auto h = (juce::int64) (1.0 * rate), shortest = (juce::int64) (0.03 * rate);
    const auto margin = (juce::int64) (0.25 * rate), xf = (juce::int64) (0.035 * rate);
    for (auto& r : regions)
    {
        if (r.endSample() <= from || r.startSample >= to) continue;
        const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
        if (r1 - r0 < 2) continue;
        std::vector<audioops::FixOp> mine;
        juce::int64 lo = r1, hi = r0;                                           // the span of the changes inside this region
        for (auto& o : ops)
        {
            const auto i0 = juce::jmax (r0, from + o.t0), i1 = juce::jmin (r1, from + o.t1);
            if (i1 - i0 < 2) continue;
            if (o.t1 - o.t0 > shortest && i1 - i0 <= shortest) continue;
            mine.push_back (audioops::FixOp { o.spec, r.srcIn + (i0 - r.startSample), r.srcIn + (i1 - r.startSample) });
            lo = juce::jmin (lo, i0); hi = juce::jmax (hi, i1);
        }
        if (mine.empty() || hi <= lo) continue;
        lo = juce::jmax (r0, lo - margin); hi = juce::jmin (r1, hi + margin);  // the piece that is made again and put in
        const auto pa = r.srcIn + (lo - r.startSample), pb = r.srcIn + (hi - r.startSample);
        for (auto& op : mine)
            if (audioops::findsLost (op.spec, rate, op.s0, op.s1)) { op.spec.lostLo = (pa + xf) - op.s0; op.spec.lostHi = (pb - xf) - op.s0; }
        RegionJob rj;
        fixFolder.createDirectory();
        prepRegionJob (fixFolder, r, rj, pa, pb, pa - h, pb + h, pa, pb, ops.back().spec, only);
        rj.ops = mine;
        jobs.push_back (std::move (rj));
    }
    if (jobs.empty()) return "Nothing to do in this area.";
    if (! makeFiles) return {};
    if (const auto err = findLostTogether (jobs, rate, j); err.isNotEmpty()) { jobs.clear(); return err; }
    return runRepairJobs (jobs, j);
}

/** The ReTouch window's consonant list (3.6.0): the project's kept consonants, keeping one (its files in the project's Consonants folder, then the list),
    renaming, deleting (its files too) and reading one back. Message thread. */
inline void wireConsonants (RepairContext& c, AppContext& app)
{
    c.consonantList = [&app] { return app.project.consonants; };
    c.keepConsonant = [&app] (const ConsonantAudio& a, const juce::String& name, const juce::String& from, juce::Uuid& id) -> juce::String
    {
        ConsonantDef d; d.id = juce::Uuid(); d.name = name; d.from = from; d.made = juce::Time::getCurrentTime();
        const auto err = consonants::save (a, d, app.project.consonantsFolder());
        if (err.isNotEmpty()) return err;
        app.project.consonants.push_back (d);
        id = d.id;
        app.project.changed();
        juce::Logger::writeToLog ("Consonant kept: \"" + name + "\" from " + from + ", " + juce::String ((int) d.tracks.size()) + " tracks, "
                                  + juce::String (juce::roundToInt (1000.0 * (double) d.length / d.sampleRate)) + " ms, in " + app.project.consonantsFolder().getFullPathName());
        return {};
    };
    c.renameConsonant = [&app] (const juce::Uuid& id, const juce::String& name)
    {
        for (auto& d : app.project.consonants) if (d.id == id) d.name = name;
        app.project.changed();
    };
    c.forgetConsonant = [&app] (const juce::Uuid& id)
    {
        auto& v = app.project.consonants;
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->id == id) { for (auto& t : it->tracks) t.file.deleteFile(); v.erase (it); break; }
        app.project.changed();
    };
    c.loadConsonant = [&app] (const juce::Uuid& id, juce::String& err) -> std::shared_ptr<const ConsonantAudio>
    {
        const auto* d = app.project.findConsonant (id);
        if (d == nullptr) { err = "That consonant is no longer kept in this project."; return nullptr; }
        return consonants::load (*d, formats(), err);
    };
}


// ---- small look-ups used on both sides (take window, edit window) ----
inline TakeWindowDef* takeWin (AppContext& app, const juce::Uuid& wid) { return app.project.findTakeWindow (wid); }

/** The take and the marked part (seconds in the take) from the take window's marks 1 / 2 marks; null + message if none. */
inline TakeGroup* markedTake (TakeWindowDef& w, double& in, double& out, const juce::String& title)
{
    auto* g = w.findGroup (w.editTake);
    if (g == nullptr || w.editIn < 0.0 || w.editOut <= w.editIn)
    {
        say (title, "First mark the part with mark 1 and mark 2 (in one take). To be safe, mark a little more than you need.");
        return nullptr;
    }
    in = w.editIn; out = juce::jmin (w.editOut, g->lengthSeconds());
    return g;
}

inline EditDef* editOf (AppContext& app, const juce::Uuid& id) { return app.project.findEdit (id); }
}} // namespace td::fixtools

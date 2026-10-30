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
void say (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle (title).withMessage (text).withButton ("OK"), nullptr);
}

/** 3.18.0: heavy work asks first while Rec mode is on (AppContext::recModeHolds). True = held: the caller stops there. If the person switches Rec off,
    'again' runs - with the window it was asked from, or nullptr if that has gone since. */
static bool heldByRec (AppContext& app, const juce::String& what, juce::Component* parent, std::function<void (juce::Component*)> again)
{
    juce::Component::SafePointer<juce::Component> sp (parent);
    return app.recModeHolds (what, [sp, again] { again (sp.getComponent()); });
}

/** The program's own little settings file for the things the offline windows remember (their size and place, the way they look). */
static juce::PropertiesFile& uiPrefs()
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
static bool gMakeProcessPage = false;
static std::unique_ptr<juce::Component> gProcessPage;
/** The curve page, with the process page beside it as a second tab when there is one (and a note in its place when the two streams of an edit point are open). */
static std::unique_ptr<juce::Component> withPitchTabs (std::unique_ptr<juce::Component> curve, bool streams)
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
void launchDialog (std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around, bool resizable, std::function<bool()> mayClose)
{
    juce::DialogWindow* dw = nullptr;
    if (mayClose != nullptr)
    {
        auto* g = new GuardedDialogWindow (title, theme::window, around != nullptr ? juce::Component::getApproximateScaleFactorForComponent (around) : 1.0f);
        g->mayClose = std::move (mayClose);
        g->setContentOwned (content.release(), true);
        g->centreAroundComponent (around, g->getWidth(), g->getHeight());
        g->setResizable (resizable, false);
        g->setUsingNativeTitleBar (true);
        g->setAlwaysOnTop (juce::WindowUtils::areThereAnyAlwaysOnTopWindows());
        g->enterModalState (true, nullptr, true);
        dw = g;
    }
    else
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (content.release());
        o.dialogTitle = title;
        o.dialogBackgroundColour = theme::window;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = resizable;
        o.componentToCentreAround = around;
        dw = o.launchAsync();
    }
    if (dw != nullptr && resizable)
    {
        const auto key = title.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- ");
        auto saved = juce::Rectangle<int>::fromString (uiPrefs().getValue ("win." + key));
        if (saved.getWidth() >= 300 && saved.getHeight() >= 200)
        {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto area = displays.getDisplayForRect (saved)->userArea;
            saved.setSize (juce::jmin (saved.getWidth(), area.getWidth()), juce::jmin (saved.getHeight(), area.getHeight()));
            // keep it on a screen (the screens may have changed since): if less than 120 pixels of it would show, it is centred instead
            if (! area.getIntersection (saved).expanded (0).contains (saved.getX() + 60, saved.getY() + 20) && ! area.contains (saved.getCentre())) saved.setCentre (area.getCentre());
            saved.setPosition (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - saved.getWidth()), saved.getX()), juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - saved.getHeight()), saved.getY()));
            dw->setBounds (saved);
        }
        auto* k = new BoundsKeeper(); k->key = key; k->last = dw->getBounds(); dw->addComponentListener (k);
    }
}

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
static void launchStream (const std::shared_ptr<StreamPair>& pair, int slot, std::unique_ptr<juce::Component> content, const juce::String& title, juce::Component* around)
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
static int detectStreams (const EditDef& e, juce::int64 from, juce::int64 to, StreamSel& out, StreamSel& in, const juce::String& title)
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

static std::shared_ptr<StreamPair> makePair (AppContext& app, const juce::Uuid& eid, const juce::String& title, const juce::String& undoLabel)
{
    auto p = std::make_shared<StreamPair>();
    p->app = &app; p->eid = eid; p->title = title; p->undoLabel = undoLabel;
    if (auto* e = app.project.findEdit (eid)) p->base = e->regions;
    p->lastSig = regionsSignature (p->base);
    return p;
}

static void closeDialogOf (juce::Component* c)
{
    if (auto* dw = c->findParentComponentOfClass<juce::DialogWindow>())
    {
        if (auto* sw = dynamic_cast<StreamWindow*> (dw)) { sw->closeButtonPressed(); return; }          // one stream window closes the pair
        dw->exitModalState (0);
    }
}

bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;wave;bwf;w64;aif;aiff;aifc;flac"); }

static juce::AudioFormatManager& formats()
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

// ======================================================================================================== the pitch dialog
struct PitchContext
{
    bool allowPad = false;
    juce::String title, what;
    std::function<juce::String (double cents, double padSeconds, AudioJob&)> work;     // job thread
    std::function<void()> finish;                                                       // message thread, after success
};

class PitchDialog : public JobDialogBase
{
public:
    explicit PitchDialog (PitchContext c) : ctx (std::move (c))
    {
        setSize (470, ctx.allowPad ? 236 : 204);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setJustificationType (juce::Justification::topLeft); intro.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (intro);
        for (auto* l : { &semiCap, &centCap, &padCap, &total }) { addAndMakeVisible (l); l->setColour (juce::Label::textColourId, theme::text); }
        semiCap.setText ("Semitones", juce::dontSendNotification); centCap.setText ("Cents", juce::dontSendNotification);
        padCap.setText ("Process extra, each end", juce::dontSendNotification);
        semi.setSliderStyle (juce::Slider::LinearHorizontal); semi.setRange (-12, 12, 1); semi.setValue (0, juce::dontSendNotification);
        cents.setSliderStyle (juce::Slider::LinearHorizontal); cents.setRange (-100, 100, 1); cents.setValue (0, juce::dontSendNotification);
        for (auto* s : { &semi, &cents }) { s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22); s->setScrollWheelEnabled (false); s->setDoubleClickReturnValue (true, 0.0); s->onValueChange = [this] { updateTotal(); }; addAndMakeVisible (s); }
        semi.setTooltip ("Whole semitones, -12 to +12 (double-click for 0)");
        cents.setTooltip ("Hundredths of a semitone: +100 cents is one semitone up. Use this for a bit that went slightly flat or sharp (double-click for 0)");
        for (int i = 0; i <= 9; ++i) pad.addItem (i == 0 ? juce::String ("none") : juce::String (i) + " s", i + 1);
        pad.setSelectedId (1, juce::dontSendNotification); pad.setTooltip ("Also shifts this much extra audio before and after the marked part, so you can move the edit point later and still be in the corrected audio");
        addChildComponent (padCap); addChildComponent (pad); padCap.setVisible (ctx.allowPad); pad.setVisible (ctx.allowPad);
        apply.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        addAndMakeVisible (apply); addAndMakeVisible (cancel);
        apply.setColour (juce::TextButton::buttonColourId, theme::accent);
        updateTotal();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        { auto t = r.removeFromTop (22); intro.placeRightOf (t); } r.removeFromTop (4);
        auto row = [&] (juce::Component& a, juce::Component& b) { auto x = r.removeFromTop (28); a.setBounds (x.removeFromLeft (150)); b.setBounds (x); r.removeFromTop (4); };
        row (semiCap, semi); row (centCap, cents);
        if (ctx.allowPad) { auto x = r.removeFromTop (28); padCap.setBounds (x.removeFromLeft (170)); pad.setBounds (x.removeFromLeft (100)); r.removeFromTop (4); }
        total.setBounds (r.removeFromTop (24));
        auto bottom = r.removeFromBottom (30);
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); apply.setBounds (bottom.removeFromRight (140));
        auto s = r.removeFromBottom (24); bar.setBounds (s.withTrimmedRight (0)); status.setBounds (r.removeFromBottom (22));
    }
private:
    double totalCents() const { return semi.getValue() * 100.0 + cents.getValue(); }
    void updateTotal()
    {
        const double c = totalCents();
        total.setText ("Total: " + juce::String (c >= 0 ? "+" : "") + juce::String ((int) std::round (c)) + " cents  ("
                       + juce::String (c >= 0 ? "up" : "down") + " " + juce::String (std::abs (c) / 100.0, 2) + " semitones)", juce::dontSendNotification);
        apply.setEnabled (std::abs (c) >= 1.0 && ! running);
    }
    void start()
    {
        const double c = totalCents(), padSec = (double) (pad.getSelectedId() - 1);
        if (std::abs (c) < 1.0) return;
        apply.setEnabled (false); status.setText ("Working...", juce::dontSendNotification);
        auto work = ctx.work; auto fin = ctx.finish;
        juce::Component::SafePointer<PitchDialog> self (this);
        runJob ([work, c, padSec] (AudioJob& j) { return work (c, padSec, j); },
                [self, fin] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty()) { if (fin) fin(); closeDialogOf (self.getComponent()); }
                    else { self->status.setText (err, juce::dontSendNotification); self->updateTotal(); }
                });
    }
    PitchContext ctx;
    InfoNote intro; juce::Label semiCap, centCap, padCap, total;
    juce::Slider semi, cents; juce::ComboBox pad; juce::TextButton apply { "Correct pitch" }, cancel { "Cancel" };
};

// ======================================================================================================== the pitch-curve dialog
struct CurveContext
{
    bool allowPad = false;
    juce::String what;
    double rate = 48000.0;
    juce::int64 length = 0;                                                                // samples in the marked part
    std::function<std::vector<std::vector<float>> (AudioJob&)> load;                        // job thread: the marked part as one (mixed) vector, for the picture behind the line
    std::function<juce::String (const FixSpec&, double padSeconds, AudioJob&)> prepare;     // job thread: makes the corrected files (nothing is changed yet)
    std::function<bool()> apply;                                                            // message thread: puts the prepared files in place, for listening
    std::function<void()> stop, revert, accept;                                             // message thread
    std::function<void (double fromSec, double toSec, bool loop)> play;                     // message thread: plays from 'fromSec' to 'toSec' (seconds into the marked part), looped or not: nothing either side
    std::function<void()> unapply;                                                          // message thread: puts the original files back but keeps the corrected ones, to switch between the two
    std::function<void (bool)> setLooping;                                                  // message thread: loop switched on / off while playing
    std::function<double()> position;                                                       // message thread: seconds from the start of the marked part, or < 0 when this is not playing
    std::function<void (bool)> hold;                                                        // message thread: true while the dialog is open (see AppContext::undoHold)
    std::function<void (std::function<bool()>)> pairWritten;                                // one stream window of an OUT / IN pair: Accept only makes this stream ready (the pair puts both into the edit together)
};

/** The line the user draws: time across, pitch up and down (+100 cents at the top, -100 at the bottom, 0 in the middle). */
class CurveView : public juce::Component
{
public:
    PitchCurve curve;
    std::function<void()> onChanged;
    std::function<void (const juce::String&)> onInfo;

    CurveView() { setWantsKeyboardFocus (true); }
    void setDuration (double s) { duration = juce::jmax (0.01, s); vt0 = 0.0; vt1 = duration; repaint(); }
    // ---- the view: the part of the time that is shown (this is what Play plays) and how many cents are shown up and down
    double visT0() const { return vt0; }
    double visT1() const { return juce::jmin (duration, vt1); }
    void resetView() { vt0 = 0.0; vt1 = duration; kc = PitchCurve::kRange; repaint(); }
    void zoomTime (double factor)
    {
        const double span = juce::jlimit (juce::jmin (0.05, duration), duration, (vt1 - vt0) * factor);
        // the zoom keeps the playhead where it is on the screen: the one running across the picture, or else the one you placed by clicking the ruler;
        // with neither in view it zooms around the middle
        const double at = (playT >= vt0 && playT <= vt1) ? playT : (cueT >= vt0 && cueT <= vt1) ? cueT : 0.5 * (vt0 + vt1);
        const double frac = (vt1 - vt0) > 0.0 ? (at - vt0) / (vt1 - vt0) : 0.5;
        setWindow (at - frac * span, span);
    }
    void zoomCents (double factor) { kc = juce::jlimit (5.0, (double) PitchCurve::kRange, kc * factor); repaint(); }
    /** Left / right = zoom out / in in time (right in), up / down = zoom out / in vertically (down in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); return true; }
        if (k == juce::KeyPress::downKey)  { zoomCents (0.7); return true; }
        if (k == juce::KeyPress::upKey)    { zoomCents (1.0 / 0.7); return true; }
        return false;
    }
    void setEnvelope (std::vector<float> e)
    {
        float mx = 1.0e-6f; for (auto v : e) mx = juce::jmax (mx, v);
        for (auto& v : e) v /= mx;
        env = std::move (e); repaint();
    }
    int count() const { return (int) curve.pts.size(); }
    /** The play position (seconds from the start of the marked part); < 0 hides it. */
    void setPlayhead (double t) { if (std::abs (t - playT) > 1.0e-6) { playT = t; repaint(); } }
    /** The playhead you place by clicking the time ruler: where Play starts, and the point the zoom keys keep still. */
    double playFrom() const { return cueT >= vt0 && cueT < vt1 ? cueT : vt0; }
    void clearPoints() { curve.pts.clear(); selected = -1; edited(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wellDark);
        const auto pl = plot();
        g.setColour (theme::wave); g.fillRect (pl);
        // what is there to correct: the sound itself, faintly
        if (! env.empty())
        {
            const float mid = (float) pl.getCentreY(), half = (float) pl.getHeight() * 0.5f * 0.9f;
            juce::Path p;
            const int n = (int) env.size();
            for (int x = 0; x < pl.getWidth(); ++x)
            {
                const double tt = vt0 + (double) x / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0);
                const float v = env[(size_t) juce::jlimit (0, n - 1, (int) (tt / duration * (double) n))];
                p.addRectangle ((float) (pl.getX() + x), mid - v * half * 0.5f, 1.0f, juce::jmax (1.0f, v * half));
            }
            g.setColour (theme::dimText.withAlpha (0.28f)); g.fillPath (p);
        }
        g.setFont (juce::FontOptions (11.0f));
        for (double c : { 100.0, 50.0, 25.0, 0.0, -25.0, -50.0, -100.0 })
        {
            if (std::abs (c) > kc + 1.0e-9) continue;
            const int y = yOf (c);
            const bool major = c == 100.0 || c == 0.0 || c == -100.0;
            g.setColour (theme::grid.withAlpha (c == 0.0 ? 1.0f : major ? 0.8f : 0.45f)); g.drawHorizontalLine (y, (float) pl.getX(), (float) pl.getRight());
            g.setColour (theme::dimText); g.drawText ((c > 0 ? "+" : "") + juce::String ((int) c), 0, y - 7, pl.getX() - 6, 14, juce::Justification::right);
        }
        g.setColour (theme::dimText); g.drawText ("cents", 0, pl.getY() - 2, pl.getX() - 6, 12, juce::Justification::right);
        {   // the time ruler above the line: drag it to move the view left and right, double-click to see it all
            const juce::Rectangle<int> ruler (pl.getX(), 0, pl.getWidth(), pl.getY() - 2);
            g.setColour (juce::Colours::white.withAlpha (0.07f)); g.fillRect (ruler);
            const double span = vt1 - vt0;
            static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
            double step = 600.0; for (double st : steps) if (st / span * pl.getWidth() >= 70.0) { step = st; break; }
            for (double t = std::ceil (vt0 / step - 1.0e-9) * step; t <= vt1 + 1.0e-9; t += step)
            {
                const int x = xOf (t);
                g.setColour (theme::grid.withAlpha (0.4f)); g.drawVerticalLine (x, (float) ruler.getY() + 12.0f, (float) pl.getBottom());
                g.setColour (theme::dimText); g.drawText (juce::String (t, step < 0.1 ? 2 : step < 1 ? 1 : 0) + " s", x + 2, ruler.getY(), 60, ruler.getHeight(), juce::Justification::centredLeft);
            }
        }
        juce::Graphics::ScopedSaveState clipToPlot (g);
        g.reduceClipRegion (pl);
        // the line: level (dashed) when nothing is drawn, otherwise the points joined, held level before the first and after the last
        juce::Path line;
        if (curve.pts.empty())
        {
            juce::Path flat; flat.startNewSubPath ((float) pl.getX(), (float) yOf (0)); flat.lineTo ((float) pl.getRight(), (float) yOf (0));
            const float dash[] = { 6.0f, 5.0f }; juce::PathStrokeType (2.0f).createDashedStroke (line, flat, dash, 2);
            g.setColour (theme::accent); g.fillPath (line);
            g.setColour (theme::dimText); g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Click to add a point (the first sits at 0 cents, later ones where you click).  Drag points to shape the pitch line.  Double-click (or Delete) removes a point.", pl.reduced (10, 0).withHeight (pl.getHeight() / 2 - 14), juce::Justification::centred);
        }
        else
        {
            line.startNewSubPath ((float) pl.getX(), (float) yOf (curve.pts.front().cents));
            for (auto& p : curve.pts) line.lineTo ((float) xOf (p.t), (float) yOf (p.cents));
            line.lineTo ((float) pl.getRight(), (float) yOf (curve.pts.back().cents));
            g.setColour (theme::accent); g.strokePath (line, juce::PathStrokeType (2.2f));
            for (int i = 0; i < count(); ++i)
            {
                const auto c = juce::Point<float> ((float) xOf (curve.pts[(size_t) i].t), (float) yOf (curve.pts[(size_t) i].cents));
                const bool sel = i == selected;
                const float r = sel ? 7.0f : 5.5f;
                g.setColour (sel ? juce::Colour (0xffffd24a) : theme::text); g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
                g.setColour (theme::wellDark); g.drawEllipse (c.x - r, c.y - r, 2 * r, 2 * r, 1.2f);
            }
        }
        if (playT < 0.0 && cueT >= vt0 && cueT <= vt1)         // the playhead you placed on the ruler
        {
            g.setColour (theme::playhead); g.drawVerticalLine (xOf (cueT), (float) pl.getY(), (float) pl.getBottom());
        }
        if (playT >= 0.0)                                  // the playhead runs across the line, with a dot on the line itself
        {
            const double t = juce::jlimit (0.0, duration, playT);
            const int x = xOf (t), y = yOf (curve.centsAt (t));
            g.setColour (theme::playhead); g.drawVerticalLine (x, (float) pl.getY(), (float) pl.getBottom());
            g.fillEllipse ((float) x - 5.0f, (float) y - 5.0f, 10.0f, 10.0f);
            g.setColour (theme::wellDark); g.drawEllipse ((float) x - 5.0f, (float) y - 5.0f, 10.0f, 10.0f, 1.2f);
        }
        g.setColour (theme::border); g.drawRect (pl);
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = hit (e.getPosition());
        if (i >= 0) say (curve.pts[(size_t) i]);
        else if (plot().contains (e.getPosition())) say ({ toT (e.x), curve.centsAt (toT (e.x)) }, true);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (e.y < plot().getY() - 2 && e.x >= plot().getX()) { panning = true; panT0 = vt0; return; }          // the ruler: click to put the playhead there, drag to move the view
        int i = hit (e.getPosition());
        if (e.mods.isPopupMenu()) { if (i >= 0) removeAt (i); return; }
        if (i < 0)
        {
            if (! plot().contains (e.getPosition())) return;
            PitchCurve::Point p { juce::jlimit (0.0, duration, toT (e.x)), curve.pts.empty() ? 0.0 : snapC (toC (e.y)) };          // the first point anchors at 0 cents; every later one goes where it is clicked (snapping to 0 when close)
            curve.pts.push_back (p); curve.sortPoints();
            for (int k = 0; k < count(); ++k) if (curve.pts[(size_t) k].t == p.t && curve.pts[(size_t) k].cents == p.cents) i = k;
            edited();
        }
        selected = i; dragging = true; repaint();
        if (i >= 0) say (curve.pts[(size_t) i]);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (panning) { setWindow (panT0 - (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, plot().getWidth()) * (vt1 - vt0), vt1 - vt0); return; }
        if (! dragging || selected < 0 || selected >= count()) return;
        const double lo = selected > 0 ? curve.pts[(size_t) selected - 1].t + 0.002 : 0.0;
        const double hi = selected + 1 < count() ? curve.pts[(size_t) selected + 1].t - 0.002 : duration;
        auto& p = curve.pts[(size_t) selected];
        p.t = juce::jlimit (juce::jmin (lo, hi), juce::jmax (lo, hi), toT (e.x)); p.cents = snapC (toC (e.y));       // whole cents; near the middle line it snaps to exactly 0
        say (p); edited();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (panning && e.getDistanceFromDragStart() < 4) { cueT = toT (e.x); repaint(); }                  // a click on the ruler (not a drag) places the playhead
        dragging = false; panning = false;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (e.y < plot().getY() - 2) { resetView(); return; } const int i = hit (e.getPosition()); if (i >= 0) removeAt (i); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && selected >= 0 && selected < count()) { removeAt (selected); return true; }
        return zoomKey (k);
    }
private:
    juce::Rectangle<int> plot() const { return getLocalBounds().withTrimmedLeft (54).withTrimmedBottom (8).withTrimmedTop (26).withTrimmedRight (10); }
    int xOf (double t) const { const auto p = plot(); return p.getX() + (int) std::lround ((t - vt0) / (vt1 - vt0) * (double) p.getWidth()); }
    int yOf (double c) const { const auto p = plot(); return p.getCentreY() - (int) std::lround (c / kc * (double) p.getHeight() * 0.5); }
    double toT (int x) const { const auto p = plot(); return juce::jlimit (0.0, duration, vt0 + (double) (x - p.getX()) / (double) juce::jmax (1, p.getWidth()) * (vt1 - vt0)); }
    double toC (int y) const { const auto p = plot(); return juce::jlimit (-PitchCurve::kRange, PitchCurve::kRange, (double) (p.getCentreY() - y) / (double) juce::jmax (1, p.getHeight()) * 2.0 * kc); }
    /** Whole cents; within 1 cent, or within 7 pixels, of the middle line it snaps to exactly 0 (so you are not fighting to hit it, however far you have zoomed). */
    double snapC (double c) const { const double r = std::round (c); return std::abs (r) <= 1.0 || std::abs (yOf (c) - yOf (0.0)) <= 7 ? 0.0 : r; }
    int hit (juce::Point<int> pos) const
    {
        int best = -1; double bd = 11.0 * 11.0;
        for (int i = 0; i < count(); ++i)
        {
            const double dx = xOf (curve.pts[(size_t) i].t) - pos.x, dy = yOf (curve.pts[(size_t) i].cents) - pos.y, d = dx * dx + dy * dy;
            if (d <= bd) { bd = d; best = i; }
        }
        return best;
    }
    void removeAt (int i) { curve.pts.erase (curve.pts.begin() + i); selected = -1; edited(); }
    void edited() { repaint(); if (onChanged) onChanged(); }
    void say (const PitchCurve::Point& p, bool lineOnly = false)
    {
        if (onInfo) onInfo ((lineOnly ? "The line at " : "Point at ") + juce::String (p.t, 2) + " s:  " + juce::String (p.cents >= 0 ? "+" : "") + juce::String (p.cents, lineOnly ? 1 : 0) + " cents");
    }
    void setWindow (double t0, double span)
    {
        span = juce::jlimit (juce::jmin (0.05, duration), duration, span);
        vt0 = juce::jlimit (0.0, juce::jmax (0.0, duration - span), t0); vt1 = vt0 + span; repaint();
    }
    double cueT = -1.0, duration = 1.0, vt0 = 0.0, vt1 = 1.0, kc = PitchCurve::kRange, panT0 = 0.0; std::vector<float> env; int selected = -1; bool dragging = false, panning = false; double playT = -1.0;
};

class CurveDialog : public JobDialogBase
{
public:
    explicit CurveDialog (CurveContext c) : ctx (std::move (c))
    {
        setSize (1000, 560);
        if (ctx.hold) ctx.hold (true);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::dimText);
        view.setDuration ((double) ctx.length / ctx.rate);
        view.onInfo = [this] (const juce::String& t) { info.setText (t, juce::dontSendNotification); };
        view.onChanged = [this] { lineChanged(); };
        for (int i = 0; i <= 9; ++i) pad.addItem (i == 0 ? juce::String ("none") : juce::String (i) + " s", i + 1);
        pad.setSelectedId (1, juce::dontSendNotification);
        pad.setTooltip ("Also corrects this much extra audio before and after the marked part (the line stays level there), so you can move the edit points outward later");
        pad.onChange = [this] { lineChanged(); };
        padCap.setText ("Process extra, each end", juce::dontSendNotification); padCap.setColour (juce::Label::textColourId, theme::text);
        addChildComponent (pad); addChildComponent (padCap); pad.setVisible (ctx.allowPad); padCap.setVisible (ctx.allowPad);
        for (auto* b : { &auditionBtn, &playBtn, &stopBtn, &revertBtn, &acceptBtn, &closeBtn, &resetBtn }) addAndMakeVisible (b);
        addAndMakeVisible (loopBtn); addAndMakeVisible (hearToggle);
        playBtn.setTooltip ("Plays the part you can see (left / right arrows zoom in and out in time, down / up zoom in / out in cents; click the ruler to place the playhead, drag it to move; R = Preview) with the sound as it is now: the original until you Preview, then the corrected sound if 'Hear corrected' is lit. With Loop lit it goes round and round. Key: Space");
        hearToggle.setTooltip ("Lit: you hear the CORRECTED sound. Off: you hear the ORIGINAL. Click it while playing to switch at once and hear the difference, without touching the line. (Available after Preview.)");
        hearToggle.onClick = [this] { hearChanged(); };
        loopBtn.setTooltip ("Repeat the marked part over and over until you press Stop");
        loopBtn.onClick = [this] { const bool on = loopBtn.getToggleState(); if (! playing()) return; if (on) { if (ctx.play) ctx.play (view.visT0(), view.visT1(), true); } else if (ctx.setLooping) ctx.setLooping (false); };
        playBtn.onClick = [this] { if (ctx.stop) ctx.stop(); if (ctx.play) ctx.play (view.playFrom(), view.visT1(), loopBtn.getToggleState()); };
        auditionBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        auditionBtn.setButtonText ("Preview");
        auditionBtn.setTooltip ("Calculates the corrected sound (this takes a moment) and plays it. Then use 'Hear corrected' to switch between the corrected and the original sound. Nothing is final until you press Accept.");
        stopBtn.setTooltip ("Stops the playback");
        revertBtn.setTooltip ("Puts the original sound back");
        acceptBtn.setTooltip ("Keeps the corrected sound (Undo fix brings the original back)");
        resetBtn.setTooltip ("Removes every point: the line goes back to level");
        auditionBtn.onClick = [this] { audition(); };
        stopBtn.onClick = [this] { if (ctx.stop) ctx.stop(); };
        revertBtn.onClick = [this] { doRevert ("Back to the original sound."); };
        acceptBtn.onClick = [this] { accept(); };
        resetBtn.onClick = [this] { view.clearPoints(); };
        closeBtn.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        status.setText ("Looking at the sound...", juce::dontSendNotification);
        if (ctx.pairWritten)
        {
            acceptBtn.setButtonText ("Accept stream"); closeBtn.setButtonText ("Cancel both");
            acceptBtn.setTooltip ("Gets this stream ready and closes this window (with no Preview first, it leaves this stream as it is). When BOTH streams have been accepted the corrected audio goes into the edit, with the edit point and its fades exactly where they are. Cancel (or the X) closes both windows and changes nothing.");
        }
        refresh();
        ticker.owner = this; ticker.startTimerHz (30);
        setWantsKeyboardFocus (true);                                              // Space plays / stops
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<CurveDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        auto load = ctx.load;
        auto data = std::make_shared<std::vector<std::vector<float>>>();
        juce::Component::SafePointer<CurveDialog> self (this);
        runJob ([load, data] (AudioJob& j) -> juce::String { *data = load (j); return j.cancelled() ? juce::String ("Cancelled.") : juce::String(); },
                [self, data] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->loaded = true;
                    if (err.isEmpty() && ! data->empty() && ! data->front().empty())
                    {
                        const auto& x = data->front(); const int bins = 1200;
                        std::vector<float> env ((size_t) bins, 0.0f);
                        for (size_t i = 0; i < x.size(); ++i) { auto& b = env[i * (size_t) bins / x.size()]; b = juce::jmax (b, std::abs (x[i])); }
                        self->view.setEnvelope (std::move (env));
                    }
                    self->status.setText ("Click on the line area to put in points (each starts at 0 cents: drag it up for sharper, down for flatter). Then press Preview, and Space plays.", juce::dontSendNotification);
                    self->refresh();
                });
    }
    ~CurveDialog() override
    {
        ticker.stopTimer(); ticker.owner = nullptr;
        if (ctx.stop) ctx.stop();
        if (tentative && ! accepted && ctx.revert) ctx.revert();       // closing without Accept puts everything back
        if (ctx.hold) ctx.hold (false);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        auto rowC = r.removeFromBottom (32);
        r.removeFromBottom (4);
        bar.setBounds (r.removeFromBottom (22).reduced (2, 2));                           // the progress bar has a full-width row of its own
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        info.setBounds (r.removeFromBottom (20).reduced (2, 0));
        closeBtn.setBounds (rowC.removeFromRight (90)); rowC.removeFromRight (8);
        acceptBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (8);
        revertBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (16);
        stopBtn.setBounds (rowC.removeFromRight (70)); rowC.removeFromRight (8);
        loopBtn.setBounds (rowC.removeFromRight (66)); rowC.removeFromRight (8);
        playBtn.setBounds (rowC.removeFromRight (70)); rowC.removeFromRight (8);
        auditionBtn.setBounds (rowC.removeFromRight (100)); rowC.removeFromRight (14);
        hearToggle.setBounds (rowC.removeFromRight (150));
        resetBtn.setBounds (rowC.removeFromLeft (100)); rowC.removeFromLeft (14);
        if (ctx.allowPad) { padCap.setBounds (rowC.removeFromLeft (150)); pad.setBounds (rowC.removeFromLeft (80)); }
        view.setBounds (r);
    }
private:
    void refresh()
    {
        const bool ready = loaded && ! running;
        auditionBtn.setEnabled (ready && ! view.curve.isFlat());
        stopBtn.setEnabled (ready);
        playBtn.setEnabled (ready);
        hearToggle.setEnabled (ready && tentative && ! stale);
        revertBtn.setEnabled (ready && tentative);
        acceptBtn.setEnabled (ready && ((tentative && ! stale) || (ctx.pairWritten != nullptr && ! tentative)));
        resetBtn.setEnabled (ready && view.count() > 0);
        pad.setEnabled (ready);
    }
    void lineChanged()
    {
        if (tentative && ! stale) { stale = true; status.setText ("The line changed: press Preview to hear it (Accept is available after that).", juce::dontSendNotification); }
        refresh();
    }
    void doRevert (const juce::String& msg)
    {
        if (ctx.stop) ctx.stop();
        if (tentative && ctx.revert) ctx.revert();
        tentative = false; stale = false; corrected = false; hearToggle.setToggleState (false, juce::dontSendNotification);
        status.setText (msg, juce::dontSendNotification); refresh();
    }
    void audition()
    {
        if (running || ! loaded || view.curve.isFlat()) return;
        if (ctx.stop) ctx.stop();
        if (tentative && ctx.revert) ctx.revert();                         // always from the original sound
        tentative = false; stale = false; corrected = false; hearToggle.setToggleState (false, juce::dontSendNotification); refresh();
        FixSpec spec; spec.kind = FixSpec::Kind::Pitch; spec.useCurve = true; spec.curve = view.curve; spec.curve.sortPoints();
        const double padSec = ctx.allowPad ? (double) (pad.getSelectedId() - 1) : 0.0;
        spec.curveZero = padSec * ctx.rate;
        status.setText ("Calculating the corrected sound...", juce::dontSendNotification);
        auditionBtn.setEnabled (false); acceptBtn.setEnabled (false); revertBtn.setEnabled (false); resetBtn.setEnabled (false); pad.setEnabled (false);
        auto prepare = ctx.prepare;
        juce::Component::SafePointer<CurveDialog> self (this);
        runJob ([prepare, spec, padSec] (AudioJob& j) { return prepare (spec, padSec, j); },
                [self] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err == "Cancelled." ? juce::String ("Cancelled. Nothing was changed.") : err, juce::dontSendNotification); self->refresh(); return; }
                    if (self->ctx.apply && self->ctx.apply())
                    {
                        self->tentative = true; self->stale = false; self->corrected = true;
                        self->hearToggle.setToggleState (true, juce::dontSendNotification);
                        if (self->ctx.play) self->ctx.play (self->view.playFrom(), self->view.visT1(), self->loopBtn.getToggleState());
                        self->status.setText ("Playing the corrected sound.  'Hear corrected' switches to the original and back; Accept keeps it, Revert goes back, or change the line and Preview again.", juce::dontSendNotification);
                    }
                    else self->status.setText ("The audio changed while it was being calculated, so nothing was changed. Please try again.", juce::dontSendNotification);
                    self->refresh();
                });
    }
    void accept()
    {
        if (running) return;
        if (ctx.pairWritten && ! tentative)                                  // an untouched stream: it is left as it is
        {
            if (ctx.stop) ctx.stop();
            accepted = true; ctx.pairWritten ([] { return true; });
            return;
        }
        if (! tentative || stale) return;
        if (ctx.stop) ctx.stop();
        if (! corrected)                                                // the original was being heard: put the corrected sound back before keeping it
        {
            if (! (ctx.apply && ctx.apply())) { status.setText ("The audio changed, so the corrected sound is no longer available. Press Preview again.", juce::dontSendNotification); tentative = false; refresh(); return; }
            corrected = true;
        }
        if (ctx.pairWritten)
        {
            accepted = true; ctx.pairWritten ([] { return true; });
            return;
        }
        if (ctx.accept) ctx.accept();
        accepted = true;
        closeDialogOf (this);
    }
    bool playing() const { return ctx.position && ctx.position() >= 0.0; }
    /** 'Hear corrected': switches between the corrected and the original sound at the same place, so the difference can be heard straight away. */
    void hearChanged()
    {
        const bool wantCorrected = hearToggle.getToggleState();
        if (! tentative || stale) { hearToggle.setToggleState (corrected, juce::dontSendNotification); status.setText ("Press Preview first: there is no corrected sound to listen to yet.", juce::dontSendNotification); return; }
        const bool wasPlaying = playing(); const double at = wasPlaying ? ctx.position() : 0.0;
        if (ctx.stop) ctx.stop();
        if (wantCorrected) { if (! (ctx.apply && ctx.apply())) { status.setText ("The audio changed, so the corrected sound is no longer available. Press Preview again.", juce::dontSendNotification); tentative = false; refresh(); return; } }
        else if (ctx.unapply) ctx.unapply();
        corrected = wantCorrected;
        if (wasPlaying && ctx.play) ctx.play (loopBtn.getToggleState() || at < view.visT0() || at >= view.visT1() ? view.visT0() : at, view.visT1(), loopBtn.getToggleState());      // carries on from the same place (a loop starts again at the beginning)
        status.setText (wantCorrected ? "Hearing the CORRECTED sound." : "Hearing the ORIGINAL sound.", juce::dontSendNotification);
    }
public:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)                                      // Space: play the marked part, or stop
        {
            if (spaceIsForRecording()) return true;
            if (loaded && ! running) { if (playing()) { if (ctx.stop) ctx.stop(); } else if (ctx.play) ctx.play (view.playFrom(), view.visT1(), loopBtn.getToggleState()); }
            return true;
        }
        if ((k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R') && ! k.getModifiers().isAnyModifierKeyDown())      // R: audition the line as it is
        {
            if (auditionBtn.isEnabled()) auditionBtn.triggerClick();
            return true;
        }
        return view.zoomKey (k);
    }
private:
    void tick()
    {
        const double t = ctx.position ? ctx.position() : -1.0;
        view.setPlayhead (t);
        if (t >= 0.0 && ! running) info.setText ("Playing:  " + juce::String (t, 2) + " s,  line " + juce::String (view.curve.centsAt (t) >= 0 ? "+" : "") + juce::String (view.curve.centsAt (t), 1) + " cents", juce::dontSendNotification);
        playBtn.setButtonText (t >= 0.0 ? "Restart" : "Play");
    }
    struct Ticker : juce::Timer { CurveDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    CurveContext ctx;
    InfoNote intro; juce::Label info, padCap;
    CurveView view; juce::ComboBox pad;
    juce::TextButton auditionBtn { "Audition" }, playBtn { "Play" }, stopBtn { "Stop" }, revertBtn { "Revert" }, acceptBtn { "Accept" }, closeBtn { "Cancel" }, resetBtn { "Reset line" };
    juce::ToggleButton loopBtn { "Loop" }, hearToggle { "Hear corrected" };
    bool loaded = false, tentative = false, stale = false, accepted = false, corrected = false;
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

/** How the spectrogram is drawn (the same ideas as the Spectrogram Settings of iZotope RX). Remembered between sessions. */
struct SpectroSettings
{
    bool   sharp = true;            // the picture is made for what is in view, with reassignment (sharp lines); off = the quick, softer picture
    bool   reassign = true;
    int    palette = 0;             // 0 cyan to orange, 1 standard (heat), 2 grey, 3 ice, 4 rainbow
    double lo = -134.3, hi = -5.3;  // the levels (dB) at the two ends of the colours
    double detail = 0.5;            // 0 = finer pitch detail, 1 = finer time detail
    double reduceAbove = 240.0;     // seconds in view above which fewer analysis points are used
    void load (juce::PropertiesFile* f)
    {
        if (f == nullptr) return;
        sharp = f->getBoolValue ("specSharp", true); reassign = f->getBoolValue ("specReassign", true); palette = juce::jlimit (0, 4, f->getIntValue ("specPalette", 0));
        lo = f->getDoubleValue ("specLo", -134.3); hi = f->getDoubleValue ("specHi", -5.3); detail = juce::jlimit (0.0, 1.0, f->getDoubleValue ("specDetail", 0.5)); reduceAbove = f->getDoubleValue ("specReduce", 240.0);
        if (hi - lo < 6.0) { lo = -134.3; hi = -5.3; }
    }
    void save (juce::PropertiesFile* f) const
    {
        if (f == nullptr) return;
        f->setValue ("specSharp", sharp); f->setValue ("specReassign", reassign); f->setValue ("specPalette", palette); f->setValue ("specLo", lo); f->setValue ("specHi", hi);
        f->setValue ("specDetail", detail); f->setValue ("specReduce", reduceAbove);
    }
};
static juce::PropertiesFile* gSpecPrefs = nullptr;                      // the program's settings file (set when a repair window is opened)

struct PictureRequest { juce::int64 id = 0; double t0 = 0.0, t1 = 1.0; int w = 0, h = 0; PictureAxis axis; PictureOptions opt; };

class SpectrogramView : public juce::Component
{
public:
    std::function<void()> onSelection;
    std::function<void (const PictureRequest&)> onNeedDetail;     // asks for the sharp picture of what is in view (made on another thread, then given back with setDetail)
    std::function<void (const juce::String&)> onNote;         // a line for the status line (colour roll, scale, view)

    void setData (MergedSpectrogram s, double seconds, bool keepSelection = false)
    {
        const bool same = spec.frames > 0 && std::abs (seconds - duration) < 1.0e-9;
        spec = std::move (s); duration = juce::jmax (0.001, seconds); dT0 = 0.0; dT1 = duration;
        if (! keepSelection) hasSel = false;
        if (! same) resetView();
        detailStale = true; scheduleDetail();
        dirty = true; repaint();
    }
    /** A quick picture that covers only part of the time (Noise mode: a slice of the whole edit's pyramid for what is in view): its first frame
        stands for 'dataT0' seconds and its last for 'dataT1'; 'seconds' is the whole length. The view, the selection and the sharp picture stay. */
    void setDataRange (MergedSpectrogram s, double dataT0, double dataT1, double seconds)
    {
        const bool fresh = spec.frames == 0;
        spec = std::move (s); duration = juce::jmax (0.001, seconds); dT0 = dataT0; dT1 = juce::jmax (dataT0 + 1.0e-6, dataT1);
        if (fresh) { resetView(); scheduleDetail(); }
        dirty = true; repaint();
    }
    std::function<void()> onView;                                  // the part of the time in view changed (zoom, pan, reset)
    /** The sound changed under the picture: the sharp picture is made again. */
    void invalidateDetail() { detailStale = true; dirty = true; repaint(); scheduleDetail(); }
    /** The sharp picture asked for with onNeedDetail (ignored if the view has moved on and asked again since). */
    void setDetail (DetailPicture p)
    {
        if (p.id != detailSeq || ! p.valid()) return;
        detail = std::move (p); detailStale = false; detailOpt = lastOpt; dirty = true; repaint();
    }
    const SpectroSettings& settings() const { return ss; }
    void setSettings (const SpectroSettings& n) { ss = n; buildLut(); detailStale = ss.sharp ? true : detailStale; dirty = true; repaint(); scheduleDetail(); }
    void resized() override { scheduleDetail(); }
    bool hasSelection() const { return hasSel; }
    /** Shows the sound around the box that the repair learns from (dotted): 'pct' is a % of the box, 'direction' 0 = left and right, 1 = up and down, 2 = both. */
    void setSurround (bool show, double pct, int direction, double afterWeight = 0.5) { showSurround = show; surroundPct = pct; surroundDir = direction; surroundAfter = juce::jlimit (0.0, 1.0, afterWeight); repaint(); }
    double selT0() const { return juce::jmin (a.x, b.x); }   // seconds
    double selT1() const { return juce::jmax (a.x, b.x); }
    double selF0() const { return juce::jmin (a.y, b.y); }   // Hz
    double selF1() const { return juce::jmax (a.y, b.y); }
    void clearSelection() { hasSel = false; repaint(); if (onSelection) onSelection(); }
    /** Seconds from the start of the area; < 0 hides the playhead. */
    void setPlayhead (double t) { if (std::abs (t - playT) > 1.0e-6) { playT = t; repaint(); } }
    /** The playhead you place by clicking the time ruler: where Play starts, and the point the zoom keys keep still. */
    double playFrom() const { return cueT >= vt0 && cueT < vt1 ? cueT : vt0; }
    /** The transport: put the placed playhead at t (seconds; < 0 takes it away), and the width of the page shown. */
    void placeCue (double t) { cueT = t < 0.0 ? -1.0 : juce::jlimit (0.0, duration, t); repaint(); }
    /** The playhead placed by clicking the ruler (seconds from the start of the area), or -1 when none is placed. */
    double cueTime() const { return cueT; }
    double pageSpan() const { return vt1 - vt0; }
    /** Where Play starts: the placed playhead if it is on this page (the very end counts), else the left edge of the page. */
    double startPoint() const { return cueT >= vt0 && cueT <= vt1 ? cueT : vt0; }
    /** Makes sure time t is on the page: if it is not, the page is turned so that t is a tenth of the way in (the zoom stays). */
    void showTime (double t) { if (t >= vt0 && t < vt1) return; setWindow (t - 0.1 * (vt1 - vt0), vt1 - vt0); }
    /** Turns to the page that starts at t0 (clamped so the last page ends with the audio; the zoom stays). */
    void showFrom (double t0) { setWindow (t0, vt1 - vt0); }

    // ---- the view: which part of the time is shown (this is what Play plays), which pitches, and how the pitches are spread up the picture
    double visT0() const { return vt0; }
    double visT1() const { return vt1; }
    void resetView() { vt0 = 0.0; vt1 = duration; fLo = kMinHz; fHi = maxHz(); dirty = true; repaint(); scheduleDetail(); if (onView) onView(); }
    /** factor < 1 zooms in, > 1 zooms out, around the middle of what is shown. */
    void zoomTime (double factor)
    {
        const double span = juce::jlimit (juce::jmin (0.05, duration), duration, (vt1 - vt0) * factor);
        // the zoom keeps the playhead where it is on the screen: the one running across the picture, or else the one you placed by clicking the ruler;
        // with neither in view it zooms around the middle
        const double at = (playT >= vt0 && playT <= vt1) ? playT : (cueT >= vt0 && cueT <= vt1) ? cueT : 0.5 * (vt0 + vt1);
        const double frac = (vt1 - vt0) > 0.0 ? (at - vt0) / (vt1 - vt0) : 0.5;
        setWindow (at - frac * span, span);
    }
    void zoomFreq (double factor)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        const double span = juce::jlimit (std::log (1.6), hi - lo, std::log (fHi / fLo) * factor);
        setFreqWindow (0.5 * (std::log (fLo) + std::log (fHi)) - 0.5 * span, span);
    }
    /** Dragging the pitch scale at the left moves what is shown up and down (the picture follows the mouse) and never changes how much is shown:
        the shift is made in the units the scale is drawn in (Mel, straight Hz, or octaves), from the window as it was when the drag began. 'down' is the drag
        distance as a fraction of the picture's height (down = positive = higher pitches come into view). */
    void dragPitchWindow (double down)
    {
        auto toU = [this] (double f) { return mel ? PictureAxis::melOf (f) : warp < 0.5 ? f : std::log (f); };
        auto fromU = [this] (double u) { return mel ? PictureAxis::hzOfMel (u) : warp < 0.5 ? u : std::exp (u); };
        const double u0 = toU (panF), u1 = toU (panF1), span = u1 - u0;
        const double minU = toU (kMinHz), maxU = toU (maxHz());
        double lo = u0 + down * span;
        lo = juce::jlimit (minU, juce::jmax (minU, maxU - span), lo);
        fLo = juce::jmax ((double) kMinHz, fromU (lo)); fHi = juce::jmin ((double) maxHz(), fromU (lo + span));
        if (fHi <= fLo * 1.01) fHi = fLo * 1.01;
        dirty = true; repaint(); scheduleDetail();
    }
    /** 0 = pitch spread evenly (linear), 1 = every octave the same height (musical); in between is a blend. */
    void setScale (double w) { warp = juce::jlimit (0.0, 1.0, w); mel = false; dirty = true; repaint(); scheduleDetail(); }
    /** The Mel scale (the pitch scale of iZotope RX). */
    void setMel() { mel = true; dirty = true; repaint(); scheduleDetail(); }
    /** Left / right = zoom out / in in time (right in), up / down = zoom out / in vertically (down in). */
    bool zoomKey (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::rightKey) { zoomTime (0.7); note(); return true; }
        if (k == juce::KeyPress::leftKey)  { zoomTime (1.0 / 0.7); note(); return true; }
        if (k == juce::KeyPress::downKey)  { zoomFreq (0.7); return true; }
        if (k == juce::KeyPress::upKey)    { zoomFreq (1.0 / 0.7); return true; }
        return false;
    }
    /** The round colour control (bottom left): each step is another colour map and another range of levels, so quiet noises show up. */
    void rollColour (double amount) { setRoll (rollPos + amount); }
    double rollValue() const { return rollPos; }
    void setRoll (double p)
    {
        rollPos = std::fmod (std::fmod (p, (double) kPresets) + (double) kPresets, (double) kPresets);
        preset = (int) std::floor (rollPos + 0.5) % kPresets;
        buildLut(); dirty = true; repaint();
        if (onNote) onNote ("Colour: " + juce::String (presetAt (preset).name) + ".  Roll the round control smoothly (drag up / down, or the mouse wheel) to blend between the looks and show quieter sounds; click to jump to the next one; double-click for the standard colours.");
    }

    SpectrogramView() { ss.load (gSpecPrefs); debounce.owner = this; buildLut(); setMouseCursor (juce::MouseCursor::CrosshairCursor); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::wellDark);
        auto plot = plotArea();
        if (dirty || image.getWidth() != plot.getWidth() || image.getHeight() != plot.getHeight()) rebuildImage (plot);
        if (image.isValid()) g.drawImageAt (image, plot.getX(), plot.getY());
        else { g.setColour (juce::Colours::white); g.drawText ("Scanning...", plot, juce::Justification::centred); }
        g.setFont (juce::FontOptions (11.0f));
        // the pitch scale
        {
            static const double ticks[] = { 50, 100, 200, 300, 500, 700, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000, 20000 };
            int lastLabelY = plot.getBottom() + 20;
            for (double f : ticks)
            {
                if (f < fLo || f > fHi) continue;
                const int y = hzToY (f, plot);
                g.setColour (juce::Colours::white.withAlpha (0.14f)); g.drawHorizontalLine (y, (float) plot.getX(), (float) plot.getRight());
                if (lastLabelY - y < 14) continue;
                lastLabelY = y;
                g.setColour (juce::Colours::white.withAlpha (0.8f)); g.drawText (f >= 1000 ? juce::String (f / 1000.0, f >= 10000 || std::fmod (f, 1000.0) == 0.0 ? 0 : 1) + "k" : juce::String ((int) f), 2, y - 7, plot.getX() - 6, 14, juce::Justification::right);
            }
        }
        // the time ruler (above the picture): drag it to move the view left and right, double-click to see everything
        {
            auto ruler = rulerArea();
            g.setColour (juce::Colours::white.withAlpha (0.07f)); g.fillRect (ruler);
            g.setColour (theme::border); g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());
            const double span = vt1 - vt0;
            static const double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
            double step = 600.0; for (double s : steps) if (s / span * plot.getWidth() >= 70.0) { step = s; break; }
            const double sub = step / 5.0;
            for (double t = std::ceil (vt0 / sub - 1.0e-9) * sub; t <= vt1 + 1.0e-9; t += sub)
            {
                const int x = tToX (t, plot);
                const bool major = std::abs (t / step - std::round (t / step)) < 1.0e-6;
                g.setColour (juce::Colours::white.withAlpha (major ? 0.7f : 0.3f)); g.drawVerticalLine (x, (float) (ruler.getBottom() - (major ? 10 : 5)), (float) (ruler.getBottom() - 1));
                if (! major) continue;
                g.setColour (juce::Colours::white.withAlpha (0.15f)); g.drawVerticalLine (x, (float) plot.getY(), (float) plot.getBottom());
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawText (juce::String (t, step < 0.1 ? 2 : step < 1 ? 1 : 0) + " s", x + 2, ruler.getY(), 60, ruler.getHeight() - 8, juce::Justification::centredLeft);
            }
        }
        {
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (plot);
            if (hasSel && showSurround)
            {
                const double t0 = selT0(), t1 = selT1(), f0 = selF0(), f1 = selF1();
                const double dt = (t1 - t0) * surroundPct / 100.0, df = (f1 - f0) * surroundPct / 100.0;
                auto box = [&] (double ta, double tb, double fa, double fb)
                {
                    ta = juce::jmax (0.0, ta); tb = juce::jmin (duration, tb); fa = juce::jmax (kMinHz, fa); fb = juce::jmin (maxHz(), fb);
                    if (tb <= ta || fb <= fa) return;
                    const int x0 = tToX (ta, plot), x1 = tToX (tb, plot), y0 = hzToY (fb, plot), y1 = hzToY (fa, plot);
                    juce::Rectangle<int> r (x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0));
                    g.setColour (juce::Colour (0x2229d3ff)); g.fillRect (r);
                    juce::Path p; p.addRectangle (r.toFloat());
                    juce::Path dashed; const float pattern[] = { 4.0f, 4.0f };
                    juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, pattern, 2);
                    g.setColour (juce::Colour (0xff29d3ff)); g.fillPath (dashed);
                };
                if (surroundDir != 1) { box (t0 - 2.0 * dt * (1.0 - surroundAfter), t0, f0, f1); box (t1, t1 + 2.0 * dt * surroundAfter, f0, f1); }      // before and after the box: shared out by the before / after weighting (50/50 = equal, 0 = all before)
                if (surroundDir != 0) { box (t0, t1, f1, f1 + df); box (t0, t1, f0 - df, f0); }          // above and below it
            }
            if (hasSel)
            {
                auto r = selRect();
                g.setColour (juce::Colour (0x44ffd24a)); g.fillRect (r);
                g.setColour (juce::Colour (0xffffd24a)); g.drawRect (r, 2);
            }
            if (playT < 0.0 && cueT >= vt0 && cueT <= vt1 && duration > 0.0)          // the playhead you placed on the ruler
            {
                const int x = tToX (cueT, plot);
                g.setColour (theme::playhead); g.fillRect (x - 1, plot.getY(), 2, plot.getHeight());
            }
            if (playT >= 0.0 && duration > 0.0 && playT >= vt0 && playT <= vt1)
            {
                const int x = tToX (playT, plot);
                g.setColour (theme::playhead); g.fillRect (x - 1, plot.getY(), 2, plot.getHeight());
            }
        }
        // the round colour control
        {
            const auto k = knobArea().toFloat(); const auto c = k.getCentre(); const float r = k.getWidth() * 0.5f;
            g.setColour (juce::Colour (0xff22262c)); g.fillEllipse (k);
            g.setColour (theme::border); g.drawEllipse (k.reduced (0.5f), 1.2f);
            for (int i = 0; i < kPresets; ++i)                                        // one dot per colour map, the lit one is the one in use
            {
                const float ang = juce::MathConstants<float>::twoPi * (float) i / (float) kPresets - juce::MathConstants<float>::halfPi;
                g.setColour (i == preset ? juce::Colours::white.withAlpha (0.8f) : juce::Colours::white.withAlpha (0.28f));
                g.fillEllipse (c.x + std::cos (ang) * (r - 3.0f) - 1.4f, c.y + std::sin (ang) * (r - 3.0f) - 1.4f, 2.8f, 2.8f);
            }
            g.setColour (lut[170]); g.fillEllipse (c.x - r * 0.45f, c.y - r * 0.45f, r * 0.9f, r * 0.9f);
            const float ang = juce::MathConstants<float>::twoPi * (float) rollPos / (float) kPresets - juce::MathConstants<float>::halfPi;
            g.setColour (juce::Colours::white); g.drawLine (c.x, c.y, c.x + std::cos (ang) * (r - 4.5f), c.y + std::sin (ang) * (r - 4.5f), 1.8f);
            g.setColour (juce::Colours::white.withAlpha (0.75f)); g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (presetAt (preset).name) + "   (colour roll)", (int) k.getRight() + 8, (int) k.getY(), 300, (int) k.getHeight(), juce::Justification::centredLeft);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        mode = Mode::none;
        if (knobArea().expanded (3).contains (e.getPosition())) { mode = Mode::knob; rollStart = rollPos; return; }
        if (rulerArea().contains (e.getPosition())) { mode = Mode::pan; panT0 = vt0; return; }
        auto pl = plotArea();
        if (e.x < pl.getX() && e.y >= pl.getY() && e.y < pl.getBottom()) { mode = Mode::pitchPan; panF = fLo; panF1 = fHi; return; }
        if (const int ed = edgesAt (e.getPosition()))                    // on an edge or corner of the box: drag it to resize the box
        {
            a = { selT0(), selF0() }; b = { selT1(), selF1() };        // a = lower-left corner, b = upper-right
            const auto r = selRect();                                  // where the pointer is on the edge, so the edge does not jump to the pointer
            grabDx = (ed & 3) ? e.x - ((ed & 1) ? r.getX() : r.getRight()) : 0;
            grabDy = (ed & 12) ? e.y - ((ed & 4) ? r.getY() : r.getBottom()) : 0;
            mode = Mode::resize; edgeMask = ed; return;
        }
        if (! pl.contains (e.getPosition())) return;
        mode = Mode::select; a = toTF (e.getPosition()); b = a; hasSel = true; repaint();
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int ed = edgesAt (e.getPosition());
        using C = juce::MouseCursor;
        setMouseCursor (ed == 0 ? C (C::CrosshairCursor)
                        : ed == 1 || ed == 2 ? C (C::LeftRightResizeCursor)
                        : ed == 4 || ed == 8 ? C (C::UpDownResizeCursor)
                        : ed == (1 | 4) ? C (C::TopLeftCornerResizeCursor) : ed == (2 | 4) ? C (C::TopRightCornerResizeCursor)
                        : ed == (1 | 8) ? C (C::BottomLeftCornerResizeCursor) : C (C::BottomRightCornerResizeCursor));
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto pl = plotArea();
        if (mode == Mode::select) { b = toTF (e.getPosition()); repaint(); }
        else if (mode == Mode::resize)
        {
            // Each side belongs to one axis: a left / right edge changes the width and nothing else, a top / bottom edge changes the height and nothing else;
            // only a corner changes both. The edge being dragged follows the pointer (where it was grabbed), the opposite edge stays where it is, and the
            // box is kept at least a few pixels across so it cannot collapse to a line while it is being resized.
            const auto pl2 = plotArea();
            const auto q = toTF ({ e.x - grabDx, e.y - grabDy });
            if (edgeMask & 3)                                              // width: a = left, b = right (they may swap places if the edge is dragged across)
            {
                const double minT = juce::jmax (0.002, 3.0 * (vt1 - vt0) / (double) juce::jmax (1, pl2.getWidth()));
                double& mv = (edgeMask & 1) ? a.x : b.x;
                const double fixedT = (edgeMask & 1) ? b.x : a.x;
                double v = q.x;
                if (std::abs (v - fixedT) < minT)
                {
                    const double side = v >= fixedT ? 1.0 : -1.0;
                    v = fixedT + side * minT;
                    if (v < 0.0 || v > duration) v = fixedT - side * minT;
                }
                if (v >= 0.0 && v <= duration) mv = v;
            }
            if (edgeMask & 12)                                             // height: b.y = top (highest pitch), a.y = bottom
            {
                double& mv = (edgeMask & 4) ? b.y : a.y;
                const double fixedF = (edgeMask & 4) ? a.y : b.y;
                const int yFixed = hzToY (fixedF, pl2);
                const int yTop = pl2.getY(), yBottom = pl2.getBottom() - 1;
                int yy = juce::jlimit (yTop, yBottom, e.y - grabDy);
                if (std::abs (yy - yFixed) < 3)
                {
                    const int side = yy >= yFixed ? 1 : -1;
                    yy = yFixed + side * 3;
                    if (yy < yTop || yy > yBottom) yy = yFixed - side * 3;
                }
                if (yy >= yTop && yy <= yBottom) mv = toTF ({ pl2.getX(), yy }).y;
            }
            repaint();
        }
        else if (mode == Mode::pan) setWindow (panT0 - (double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0), vt1 - vt0);
        else if (mode == Mode::pitchPan) dragPitchWindow ((double) e.getDistanceFromDragStartY() / (double) juce::jmax (1, pl.getHeight()));
        else if (mode == Mode::knob)
        {
            setRoll (rollStart - (double) e.getDistanceFromDragStartY() / 45.0);       // smooth: about 45 px per colour look
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto m = mode; mode = Mode::none;
        if (m == Mode::knob && e.getDistanceFromDragStart() < 4) { setRoll (std::floor (rollPos + 0.5) + (e.mods.isRightButtonDown() || e.mods.isShiftDown() ? -1 : 1)); return; }
        if (m == Mode::pan && e.getDistanceFromDragStart() < 4)                  // a click (not a drag) on the time ruler puts the playhead there
        {
            const auto pl = plotArea();
            cueT = juce::jlimit (0.0, duration, vt0 + (double) (e.x - pl.getX()) / (double) juce::jmax (1, pl.getWidth()) * (vt1 - vt0));
            repaint(); return;
        }
        if (m == Mode::pan || m == Mode::pitchPan) { note(); return; }
        if (m != Mode::select && m != Mode::resize) return;
        if (selT1() - selT0() < 0.002) hasSel = false;
        repaint(); if (onSelection) onSelection();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (knobArea().expanded (3).contains (e.getPosition())) { rollColour ((double) w.deltaY * 2.0); return; }
        Component::mouseWheelMove (e, w);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (knobArea().expanded (3).contains (e.getPosition())) { setRoll (0.0); return; }                 // back to the standard colours
        if (rulerArea().contains (e.getPosition())) { resetView(); note(); return; }                               // everything again
        if (plotArea().contains (e.getPosition())) clearSelection();
    }
private:
    static constexpr double kMinHz = 40.0;
    static constexpr int kPresets = 8;
    enum class Mode { none, select, pan, pitchPan, knob, resize };
    int edgeMask = 0;                                           // while resizing the box: 1 left, 2 right, 4 top, 8 bottom
    int grabDx = 0, grabDy = 0;                                 // while resizing: how far the pointer was from the edge it took hold of (pixels)
    struct Stop { float t, r, g, b; };
    struct Preset { const char* name; int palette; float lo, hi, gamma; };
    static const Preset& presetAt (int i)
    {
        static const Preset p[kPresets] = {
            { "Standard",                 0, -100.0f, -15.0f, 1.0f },
            { "Strong sounds only",       0,  -80.0f, -20.0f, 1.0f },
            { "Show quiet sounds",        0, -115.0f, -45.0f, 1.0f },
            { "Show very quiet noise",    2, -125.0f, -65.0f, 0.8f },
            { "Grey",                     1, -100.0f, -15.0f, 1.0f },
            { "Grey, quiet sounds",       1, -120.0f, -50.0f, 0.7f },
            { "Rainbow",                  3, -100.0f, -20.0f, 1.0f },
            { "Rainbow, quiet sounds",    3, -125.0f, -60.0f, 0.85f } };
        return p[juce::jlimit (0, kPresets - 1, i)];
    }
    static juce::Colour ramp (const Stop* s, int n, float t)
    {
        for (int i = 1; i < n; ++i)
            if (t <= s[i].t) { const float f = (t - s[i - 1].t) / (s[i].t - s[i - 1].t); return juce::Colour::fromFloatRGBA (s[i - 1].r + f * (s[i].r - s[i - 1].r), s[i - 1].g + f * (s[i].g - s[i - 1].g), s[i - 1].b + f * (s[i].b - s[i - 1].b), 1.0f); }
        return juce::Colour::fromFloatRGBA (s[n - 1].r, s[n - 1].g, s[n - 1].b, 1.0f);
    }
    /** The look in use: the two neighbouring presets blended by how far the knob is between them, so it changes continuously. */
    Preset blended (const Preset*& pa, const Preset*& pb, float& f) const
    {
        const int i0 = (int) std::floor (rollPos) % kPresets, i1 = (i0 + 1) % kPresets;
        f = (float) (rollPos - std::floor (rollPos));
        pa = &presetAt (i0); pb = &presetAt (i1);
        return Preset { pa->name, pa->palette, pa->lo + f * (pb->lo - pa->lo), pa->hi + f * (pb->hi - pa->hi), pa->gamma + f * (pb->gamma - pa->gamma) };
    }
    static juce::Colour paletteAt (int pal, float t)
    {
        static const Stop heat[] = { { 0.0f, 0, 0, 0.05f }, { 0.25f, 0.2f, 0.0f, 0.4f }, { 0.5f, 0.75f, 0.1f, 0.35f }, { 0.75f, 1.0f, 0.55f, 0.1f }, { 1.0f, 1.0f, 1.0f, 0.8f } };
        static const Stop grey[] = { { 0.0f, 0, 0, 0 }, { 1.0f, 1, 1, 1 } };
        static const Stop ice[]  = { { 0.0f, 0, 0, 0.05f }, { 0.35f, 0, 0.15f, 0.6f }, { 0.7f, 0, 0.75f, 0.95f }, { 1.0f, 1, 1, 1 } };
        static const Stop cyanOrange[] = { { 0.0f, 0.0f, 0.0f, 0.04f }, { 0.18f, 0.02f, 0.10f, 0.32f }, { 0.36f, 0.0f, 0.38f, 0.62f }, { 0.52f, 0.0f, 0.80f, 0.85f }, { 0.64f, 0.55f, 0.92f, 0.70f }, { 0.78f, 1.0f, 0.82f, 0.25f }, { 0.90f, 1.0f, 0.52f, 0.05f }, { 1.0f, 1.0f, 0.95f, 0.85f } };
        static const Stop rain[] = { { 0.0f, 0, 0, 0.15f }, { 0.2f, 0.15f, 0, 0.7f }, { 0.4f, 0, 0.6f, 1.0f }, { 0.6f, 0.1f, 0.9f, 0.3f }, { 0.8f, 1.0f, 0.9f, 0.0f }, { 1.0f, 1.0f, 0.15f, 0.1f } };
        return pal == 0 ? ramp (heat, 5, t) : pal == 1 ? ramp (grey, 2, t) : pal == 2 ? ramp (ice, 4, t) : pal == 4 ? ramp (cyanOrange, 8, t) : ramp (rain, 6, t);
    }
    void buildLut()
    {
        const Preset *pa, *pb; float f; blended (pa, pb, f);
        for (int i = 0; i < 256; ++i)
        {
            const float u = (float) i / 255.0f;
            const auto ca = paletteAt (pa->palette, std::pow (u, pa->gamma)), cb = paletteAt (pb->palette, std::pow (u, pb->gamma));
            lut[(size_t) i] = ca.interpolatedWith (cb, f);
            static const int paletteIds[] = { 4, 0, 1, 2, 3 };                          // the sharp picture uses the palette chosen in the spectrogram settings, with the roll's gamma
            lutD[(size_t) i] = paletteAt (paletteIds[juce::jlimit (0, 4, ss.palette)], std::pow (u, pa->gamma + f * (pb->gamma - pa->gamma)));
        }
    }
    /** The two ends of the colours (dB) for the sharp picture: the settings, shifted by the colour roll's look. */
    void detailRange (float& lo, float& hi) const
    {
        const Preset *pa, *pb; float f; const Preset P = blended (pa, pb, f);
        lo = (float) ss.lo + (P.lo + 100.0f); hi = (float) ss.hi + (P.hi + 15.0f);
        if (hi - lo < 6.0f) hi = lo + 6.0f;
    }
    PictureAxis axisNow() const { PictureAxis ax; ax.fLo = fLo; ax.fHi = fHi; ax.warp = warp; ax.mel = mel; return ax; }
    void scheduleDetail() { if (ss.sharp && spec.frames > 0) debounce.startTimer (260); }
    void requestDetailNow()
    {
        if (! ss.sharp || spec.frames <= 0 || ! onNeedDetail) return;
        const auto pl = plotArea();
        if (pl.getWidth() < 50 || pl.getHeight() < 50) return;
        PictureRequest rq; rq.t0 = vt0; rq.t1 = vt1; rq.w = pl.getWidth(); rq.h = pl.getHeight(); rq.axis = axisNow();
        rq.opt.reassign = ss.reassign; rq.opt.detail = ss.detail; rq.opt.reduceAboveSec = ss.reduceAbove;
        const bool optSame = detailOpt.reassign == rq.opt.reassign && std::abs (detailOpt.detail - rq.opt.detail) < 1.0e-9 && std::abs (detailOpt.reduceAboveSec - rq.opt.reduceAboveSec) < 1.0e-9;
        if (detail.valid() && ! detailStale && optSame && detail.w == rq.w && detail.h == rq.h && std::abs (detail.t0 - rq.t0) < 1.0e-9 && std::abs (detail.t1 - rq.t1) < 1.0e-9 && detail.axis.same (rq.axis)) return;
        rq.id = ++detailSeq; lastOpt = rq.opt;
        onNeedDetail (rq);
    }
    struct Debounce : juce::Timer { SpectrogramView* owner = nullptr; void timerCallback() override { stopTimer(); if (owner != nullptr) owner->requestDetailNow(); } } debounce;
    double maxHz() const { return juce::jmin (22000.0, spec.sampleRate * 0.5); }
    juce::Rectangle<int> plotArea() const { return getLocalBounds().withTrimmedLeft (46).withTrimmedTop (22).withTrimmedBottom (30); }
    juce::Rectangle<int> rulerArea() const { auto pl = plotArea(); return { pl.getX(), 0, pl.getWidth(), pl.getY() }; }
    juce::Rectangle<int> knobArea() const { return { 8, getHeight() - 28, 24, 24 }; }
    void note() { if (onNote) onNote ("Showing " + juce::String (vt0, 2) + " - " + juce::String (vt1, 2) + " s.  Right / left arrows zoom in / out in time, down / up in pitch; click the ruler to place the playhead, drag it to move; double-click the ruler to see it all."); }
    void setWindow (double t0, double span)
    {
        span = juce::jlimit (juce::jmin (0.05, duration), duration, span);
        t0 = juce::jlimit (0.0, juce::jmax (0.0, duration - span), t0);
        vt0 = t0; vt1 = t0 + span; dirty = true; repaint(); scheduleDetail(); if (onView) onView();
    }
    void setFreqWindow (double logLo, double logSpan)
    {
        const double lo = std::log (kMinHz), hi = std::log (maxHz());
        logSpan = juce::jlimit (std::log (1.6), hi - lo, logSpan);
        logLo = juce::jlimit (lo, hi - logSpan, logLo);
        fLo = std::exp (logLo); fHi = std::exp (logLo + logSpan); dirty = true; repaint(); scheduleDetail();
    }
    // pitch <-> height: the Mel scale, or a blend of the straight line (even Hz) and the logarithm (even octaves)
    double posOf (double hz) const { return axisNow().posOf (hz); }
    double hzOf (double pos) const { return axisNow().hzOf (pos); }
    int tToX (double t, juce::Rectangle<int> pl) const { return pl.getX() + (int) std::lround ((t - vt0) / juce::jmax (1.0e-9, vt1 - vt0) * pl.getWidth()); }
    int hzToY (double hz, juce::Rectangle<int> pl) const { return pl.getY() + (int) std::lround ((1.0 - posOf (hz)) * (pl.getHeight() - 1)); }
    juce::Point<double> toTF (juce::Point<int> p) const
    {
        auto pl = plotArea();
        const double t = juce::jlimit (0.0, duration, vt0 + (double) (p.x - pl.getX()) / juce::jmax (1, pl.getWidth()) * (vt1 - vt0));
        const int yy = juce::jlimit (0, pl.getHeight() - 1, p.y - pl.getY());
        return { t, hzOf (1.0 - (double) yy / (double) juce::jmax (1, pl.getHeight() - 1)) };
    }
    /** Which edges of the box the point is on (1 left, 2 right, 4 top, 8 bottom, or two of them at a corner); 0 = not on the box's border. */
    int edgesAt (juce::Point<int> p) const
    {
        if (! hasSel) return 0;
        const auto r = selRect();                                             // (the rules - sides belong to one axis, only a real corner takes both - are in core/BoxEdges.h, with a test)
        return boxedit::edgesAt ({ r.getX(), r.getY(), r.getWidth(), r.getHeight() }, p.x, p.y);
    }
    juce::Rectangle<int> selRect() const
    {
        auto pl = plotArea();
        const int x0 = tToX (selT0(), pl), x1 = tToX (selT1(), pl);
        const int y0 = hzToY (selF1(), pl), y1 = hzToY (selF0(), pl);
        return { x0, y0, juce::jmax (2, x1 - x0), juce::jmax (2, y1 - y0) };
    }
    /** The picture of what is in view, made at the size it is shown (so zooming in shows more detail, within what was scanned). */
    void rebuildImage (juce::Rectangle<int> pl)
    {
        dirty = false;
        const int W = pl.getWidth(), H = pl.getHeight();
        if (spec.frames <= 0 || W < 2 || H < 2) { image = juce::Image(); return; }
        image = juce::Image (juce::Image::RGB, W, H, false);
        juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
        std::vector<int> rb ((size_t) H); std::vector<float> rf ((size_t) H);
        for (int y = 0; y < H; ++y)
        {
            const double hz = hzOf (1.0 - (double) y / (double) (H - 1));
            rb[(size_t) y] = 0; rf[(size_t) y] = (float) hz;                       // (rf holds the pitch of this row; spec.level() blends the two analysis layers)
        }
        const Preset *qa, *qb; float qf; const Preset P = blended (qa, qb, qf);
        const float lo = P.lo, inv = 255.0f / (P.hi - P.lo);
        for (int x = 0; x < W; ++x)
        {
            const double t = vt0 + ((double) x + 0.5) / (double) W * (vt1 - vt0);
            const double fp = juce::jlimit (0.0, (double) spec.frames - 1.0001, (t - dT0) / (dT1 - dT0) * (double) (spec.frames - 1));
            const int f0 = (int) fp; const float ff = (float) (fp - f0); const int f1 = juce::jmin (spec.frames - 1, f0 + 1);
            for (int y = 0; y < H; ++y)
            {
                const float d0 = spec.level (f0, (double) rf[(size_t) y]), d1 = spec.level (f1, (double) rf[(size_t) y]);
                const float d = d0 * (1.0f - ff) + d1 * ff;
                const int idx = juce::jlimit (0, 255, (int) ((d - lo) * inv));
                bd.setPixelColour (x, y, lut[(size_t) idx]);
            }
        }
        // the sharp picture, where it covers the view (it replaces the quick one; the quick one shows while the sharp one is being made)
        if (ss.sharp && detail.valid() && ! detailStale && std::abs (detail.t0 - vt0) < 1.0e-6 && std::abs (detail.t1 - vt1) < 1.0e-6 && detail.axis.same (axisNow()))
        {
            float dlo, dhi; detailRange (dlo, dhi); const float dinv = 255.0f / (dhi - dlo);
            const double dSpan = juce::jmax (1.0e-9, detail.t1 - detail.t0);
            std::vector<int> drow ((size_t) H, -1);
            for (int y = 0; y < H; ++y)
            {
                const double hz = rf[(size_t) y];
                if (hz >= detail.axis.fLo && hz <= detail.axis.fHi) drow[(size_t) y] = juce::jlimit (0, detail.h - 1, (int) std::lround ((1.0 - detail.axis.posOf (hz)) * (double) (detail.h - 1)));
            }
            for (int x = 0; x < W; ++x)
            {
                const double t = vt0 + ((double) x + 0.5) / (double) W * (vt1 - vt0);
                const int col = (int) std::floor ((t - detail.t0) / dSpan * (double) detail.w);
                if (col < 0 || col >= detail.w) continue;
                for (int y = 0; y < H; ++y)
                {
                    const int row = drow[(size_t) y];
                    if (row < 0) continue;
                    const float db = detail.db[(size_t) row * (size_t) detail.w + (size_t) col];
                    bd.setPixelColour (x, y, lutD[(size_t) juce::jlimit (0, 255, (int) ((db - dlo) * dinv))]);
                }
            }
        }
    }
    MergedSpectrogram spec; double duration = 1.0, dT0 = 0.0, dT1 = 1.0; juce::Image image; bool dirty = true;
    double vt0 = 0.0, vt1 = 1.0, fLo = kMinHz, fHi = 20000.0, warp = 1.0;
    int preset = 0; double rollPos = 0.0, rollStart = 0.0; Mode mode = Mode::none; double panT0 = 0.0, panF = kMinHz, panF1 = 20000.0;
    std::array<juce::Colour, 256> lut, lutD;
    SpectroSettings ss; bool mel = true; DetailPicture detail; bool detailStale = true; juce::int64 detailSeq = 0; PictureOptions detailOpt, lastOpt;
    bool hasSel = false; juce::Point<double> a, b; double playT = -1.0, cueT = -1.0;
    bool showSurround = false; double surroundPct = 100.0; int surroundDir = 0; double surroundAfter = 0.5;
};

class RepairDialog : public JobDialogBase
{
public:
    explicit RepairDialog (RepairContext c) : ctx (std::move (c))
    {
        setSize (1100, 760);
        intro.setText (ctx.what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::text);
        dirBox.addItem ("Direction: Horizontal", 1); dirBox.addItem ("Direction: Vertical", 2); dirBox.addItem ("Direction: 2D (both)", 3);
        dirBox.setSelectedId (1, juce::dontSendNotification); dirBox.onChange = [this] { refreshControls(); invalidate(); };
        dirBox.setTooltip ("Where the repair finds its material. Left and right suits steady sounds (hum, a held note). Up and down suits short bursts (a click, a cough) with music above and below. Both blends the two.");
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (sensCap, "Click sensitivity"); cap (strengthCap, "Strength"); cap (surroundCap, "Surrounding length"); cap (weightCap, "Before <-> After");
        cap (amountCap, "Amount"); cap (residualCap, "Residual"); cap (roomCap, "");
        auto slider = [this] (juce::Slider& sl, double lo, double hi, double step, double v, const juce::String& suffix)
        {
            sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (lo, hi, step); sl.setValue (v, juce::dontSendNotification); sl.setScrollWheelEnabled (false);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 22); sl.setTextValueSuffix (suffix);
            sl.onValueChange = [this] { updateSurround(); invalidate(); };
            addAndMakeVisible (sl);
        };
        slider (sens, 1, 10, 1, 6, ""); slider (strength, 0, 100, 1, 80, " %"); slider (surround, 10, 400, 5, 100, " %"); slider (weight, 0, 100, 1, 50, " %");
        slider (amount, -6.0, 12.0, 0.5, 0.0, " dB"); slider (residual, -30.0, -3.0, 1.0, -12.0, " dB");
        amount.setDoubleClickReturnValue (true, 0.0); residual.setDoubleClickReturnValue (true, -12.0);
        amount.setTooltip ("Denoise: how much louder than measured the room tone is taken to be. 0 = as learned; +3 dB takes a little more out, -3 dB a little less. Double-click for 0.");
        residual.setTooltip ("Denoise: how much of the room tone is left in the box (the gain never goes below this). -12 dB leaves a quiet, natural floor; -24 dB is a lot. Double-click for -12.");
        roomBtn.onClick = [this] { roomFromBox(); };
        denoiseBtn.onClick = [this] { preview (2); };
        roomBtn.setTooltip ("Draw a box over a part of THIS picture that holds nothing but the room (its pitch range does not matter: the whole spectrum is learned) and press this: that box is the room tone for Denoise here, instead of the project's. Key: L");
        denoiseBtn.setTooltip ("Denoise: takes the background noise out of the box you drew - its time span and its pitch range, on every track - with the room tone (the project's, from the edit window's Room tone button, or a box of this picture marked with L). "
                               "Above and below the box's pitch range nothing is touched; the denoising fades in and out at the box's edges. Same rules as ReTouch: nothing is written yet, Play to listen, Undo if it did not work. Key: N");
        denoiseBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        addAndMakeVisible (roomBtn); addAndMakeVisible (denoiseBtn);
        strength.setTooltip ("How much of the repair replaces the original sound inside the box. 100 % is a full repair; less keeps some of the original.");
        surround.setTooltip ("How much of the sound around the box is used, as a percentage of the box: its length in time (left and right) and its height in pitch (up and down).");
        weight.setTooltip ("0 uses only the sound before the box, 100 only the sound after it, 50 uses both equally (a cross-fade). Applies to left and right.\n"
                           "Attenuate a stray consonant: leave it at 50 and Fermata finds, for every track, the side away from the main consonant; move it to choose a side yourself.");
        methodBox.addItem ("Method: Attenuate", 1); methodBox.addItem ("Method: Replace", 2);
        methodBox.addItem ("Method: Stray consonant", 3);
        methodBox.setSelectedId (1, juce::dontSendNotification);
        methodBox.setTooltip ("Attenuate keeps the music that is under the noise and only turns down what sticks out above the surrounding sound: the best for a click, a pencil drop or a cough over a held note. Replace throws the box away and builds new sound from the surroundings: for gaps and loud, wide damage.\n\n"
                               "Attenuate a stray consonant: for tidying a doubled or messy consonant to one. Draw the box round the EARLY or LATE copy only (from where the stray one starts up to where the main one starts, "
                               "or from where the main one ends to where the stray one stops), from about 2 kHz (1.5 kHz for sh) to the top. Each track is matched to the side of the box AWAY from the main consonant "
                               "(the vowel before an early copy, the sound after a late one), so the main consonant next to the box does not count as 'normal' sound and the stray copy comes right down; "
                               "vowels that carry on through the box keep their level. The sound just before the box dies away naturally into it and the box rises to meet the sound after it, so there are no hard edges: "
                               "cover all of the stray copy and a little past it on the clean side. Use 100 % strength.");
        for (int b : { 128, 256, 512, 1024, 2048, 4096 }) bandsBox.addItem ("Bands: " + juce::String (b), b);
        bandsBox.setSelectedId (1024, juce::dontSendNotification);
        bandsBox.setTooltip ("How finely the sound is split into pitches for Attenuate. More bands follow steady notes more exactly; fewer bands are sharper in time (clicks). 1024 suits most things.");
        linkBtn.setToggleState (true, juce::dontSendNotification);
        linkBtn.setTooltip ("A stereo file is turned down by the SAME amount on both sides (the amount found for the louder side), with each side's own phase kept. The time difference between the microphones and the level difference between them, which make the stereo image, are then not changed. Untick to treat every channel on its own.");
        linkBtn.onClick = [this] { invalidate(); };
        multiBtn.setToggleState (true, juce::dontSendNotification);
        multiBtn.setTooltip ("Attenuate works with a long window for the bass and a short one for the top and joins them smoothly, so both low notes and clicks are matched well.");
        specBtn.setTooltip ("How the picture is drawn: sharpness, colours, and the levels shown.");
        specBtn.onClick = [this] { openSpectroSettings(); };
        methodBox.onChange = [this] { refreshControls(); invalidate(); };
        bandsBox.onChange = [this] { invalidate(); };
        multiBtn.onClick = [this] { invalidate(); };
        view.onNeedDetail = [this] (const PictureRequest& rq) { startDetail (rq); };
        for (auto* x : std::initializer_list<juce::Component*> { &methodBox, &bandsBox, &multiBtn, &linkBtn, &specBtn, &dirBox, &previewBtn, &declickBtn, &compareBtn, &revertBtn, &finishBtn, &close, &playBtn, &stopBtn, &loopBtn, &hearCap, &hearBox, &scaleBox, &detailBox }) addAndMakeVisible (x);
        scaleBox.addItem ("Pitch scale: Mel (as iZotope RX)", 6); scaleBox.addItem ("Pitch scale: linear (even Hz)", 1); scaleBox.addItem ("Pitch scale: mostly linear", 2); scaleBox.addItem ("Pitch scale: halfway", 3);
        scaleBox.addItem ("Pitch scale: mostly musical", 4); scaleBox.addItem ("Pitch scale: musical (every octave the same height)", 5);
        scaleBox.setSelectedId (6, juce::dontSendNotification); view.setMel();
        scaleBox.setTooltip ("How the pitches are spread up the picture. Linear: every 1000 Hz takes the same height (the high end gets room, the low end is squashed). Musical: every octave takes the same height (like a piano). The steps in between blend the two.");
        detailBox.addItem ("Bass detail: blended (sharp top, fine bass)", 1); detailBox.addItem ("Bass detail: off (sharpest in time)", 2); detailBox.addItem ("Bass detail: extra fine", 3);
        detailBox.setSelectedId (1, juce::dontSendNotification);
        detailBox.setTooltip ("The picture is worked out twice: with a short window (sharp in time, good for clicks) and with a long one (sharp in pitch, good for bass notes). Blended: the long one is used at the bottom, the short one at the top, and they fade into each other in between (250 Hz to 1.5 kHz). Off: the short one only. Extra fine: an even longer window for the bass (below about 800 Hz).");
        detailBox.onChange = [this] { rescan(); };
        scaleBox.onChange = [this] { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; const int id = scaleBox.getSelectedId(); if (id == 6) view.setMel(); else view.setScale (w[juce::jlimit (0, 4, id - 1)]); };
        view.onNote = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };
        hearCap.setText ("Listen to", juce::dontSendNotification); hearCap.setColour (juce::Label::textColourId, theme::text);
        hearBox.addItem ("Original", 1); hearBox.addItem ("Fixed (after Fix)", 2); hearBox.setSelectedId (1, juce::dontSendNotification);
        hearBox.setTooltip ("Which sound Play plays: the audio as it is, or the audio with the repair applied (available after you press ReTouch or De-click). Press Play while a loop runs to hear the other one.");
        playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        playBtn.setTooltip ("Plays the part of the picture you can see, from the red playhead if you have clicked the time ruler to place one (zoom in with the left / right arrows to play a shorter piece, zoom out to hear more), through the mixers, with a playhead on the picture. No repair is needed: use it to listen for what has to be mended. Key: Space");
        loopBtn.setTooltip ("Repeat what is playing until you press Stop");
        stopBtn.setTooltip ("Stops playing");
        playBtn.onClick = [this] { playPressed(); };
        stopBtn.onClick = [this] { if (ctx.stop) ctx.stop(); };
        loopBtn.onClick = [this] { if (! isPlayingNow()) return; if (loopBtn.getToggleState()) playPressed(); else if (ctx.setLooping) ctx.setLooping (false); };
        hearBox.onChange = [this] { if (isPlayingNow()) playPressed(); };
        ticker.owner = this; ticker.startTimerHz (30);
        if (ctx.hold) ctx.hold (true);
        previewBtn.onClick = [this] { preview (0); };
        gainBtn.onClick = [this] { openGain(); };
        gainBtn.setTooltip ("Gain: makes the sound inside the box louder or quieter (from silence up to +18 dB), and nothing else: for a resonance, a ring or a note in one spot that should stand out less, or more. "
                            "Draw the box round it and press G. The gain fades in and out smoothly at the box's edges.");
        addAndMakeVisible (gainBtn); gainBtn.setWantsKeyboardFocus (false);
        declickBtn.onClick = [this] { preview (1); };
        compareBtn.onClick = [this] { toggleCompare(); };
        revertBtn.onClick = [this] { revert(); };
        finishBtn.onClick = [this] { finish(); };
        close.onClick = [this] { if (running) job.requestCancel(); else if (mayClose()) closeDialogOf (this); };
        previewBtn.setColour (juce::TextButton::buttonColourId, theme::accent); declickBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        finishBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        declickBtn.setTooltip ("Finds and mends the clicks in the time span of the box (or everywhere if there is no box; the pitch range of the box is ignored) and shows the result. Set how many it finds with Click sensitivity. A digital dropout (a block of samples that went missing: the sound jumps in one sample on every track at once) is mended by putting the missing samples back, on every track in the same way. Same rules as ReTouch: nothing is written yet, Play to listen, Undo if it did not work.");
        previewBtn.setTooltip ("ReTouch: rebuilds or turns down the box you drew from its surroundings and shows the result. (A small box round a click that reaches down into the bass: if the click is a digital dropout, the samples that went missing are put back first, on every track in the same way, and then the box is left alone; Click sensitivity decides how readily such a dropout is found.) It does not play, and nothing is written to the files yet: press Play to listen, and Undo if it did not work. You can then draw a box round another problem and press ReTouch or De-click again: the earlier fixes stay.");
        finishBtn.setTooltip ("Writes all the fixes into the files (everything between the marks is rendered again in one go). The Undo command of the window brings the original back afterwards.");
        close.setTooltip ("Closes the window and puts everything back as it was (nothing is kept).");
        revertBtn.setTooltip ("Takes the last fix off again. Press it again to take off the one before.");
        compareBtn.setTooltip ("Switches the picture between the original and the repaired sound.");
        view.onSelection = [this] { refreshControls(); invalidate(); };
        setupTabs();
        setupConsonants();
        loadControls();
        refreshControls();
        close.setButtonText ("Cancel");
        if (ctx.pairWritten)
        {
            finishBtn.setButtonText ("Write back this stream"); close.setButtonText ("Cancel both");
            finishBtn.setTooltip ("Gets this stream ready and closes this window (press it on an untouched stream to leave that one as it is). When BOTH streams have been written back the corrected audio goes into the edit, with the edit point and its fades exactly where they are. Cancel (or the X) closes both windows and changes nothing.");
        }
        setWantsKeyboardFocus (true);                                              // Space plays / stops, whatever was clicked last
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RepairDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        status.setText ("Scanning all the tracks...", juce::dontSendNotification);
        auto load = ctx.load; double rate = ctx.rate; double secs = (double) ctx.length / ctx.rate;
        juce::Component::SafePointer<RepairDialog> self (this);
        auto spec = std::make_shared<MergedSpectrogram>();
        origTracks = std::make_shared<std::vector<std::vector<float>>>();
        auto keep = origTracks;
        const int detailN = detailSize();
        runJob ([load, rate, spec, keep, detailN] (AudioJob& j) -> juce::String
                {
                    auto tracks = load (j);
                    if (j.cancelled()) return "Cancelled.";
                    if (tracks.empty()) return "There is no audio here.";
                    *spec = SpectralRepair::spectrogram (tracks, rate, 1600, detailN);
                    *keep = std::move (tracks);
                    return spec->frames > 0 ? juce::String() : juce::String ("The marked area is too short to show (it needs at least about 0.05 s).");
                },
                [self, spec, secs] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); return; }
                    self->origSpec = std::make_shared<MergedSpectrogram> (*spec);
                    self->view.setData (std::move (*spec), secs);
                    self->status.setText ("Press Space (or Play) to listen to what is in view (left / right arrows zoom in time, down / up zoom in / out in pitch, click the ruler to place the playhead, drag it to move; R = ReTouch, D = De-click). Drag a box around the noise (drag its edges or corners to resize it), set the controls, then press ReTouch (or De-click), then Play to listen. Undo if it did not work; or draw a box round the next problem and ReTouch / De-click again (the earlier fixes stay). Write back to file when you are happy.  Double-click the picture to clear the box.", juce::dontSendNotification);
                    self->scanned = true; self->refreshControls();
                });
    }
    juce::String prefKey (const char* k) const { return juce::String ("repair.") + k; }
    /** What the controls were set to last time (the picture settings, the repair settings): put back when the window opens, saved when it closes. */
    void loadControls()
    {
        auto& p = uiPrefs();
        auto num = [&] (const char* k, juce::Slider& s) { const auto key = prefKey (k); if (p.containsKey (key)) s.setValue (juce::jlimit (s.getMinimum(), s.getMaximum(), p.getDoubleValue (key)), juce::dontSendNotification); };
        auto id = [&] (const char* k, juce::ComboBox& b) { const auto key = prefKey (k); if (p.containsKey (key) && b.indexOfItemId (p.getIntValue (key)) >= 0) b.setSelectedId (p.getIntValue (key), juce::dontSendNotification); };
        num ("sens", sens); num ("strength", strength); num ("surround", surround); num ("weight", weight); num ("amount", amount); num ("residual", residual);
        id ("method", methodBox); id ("bands", bandsBox); id ("dir", dirBox); id ("scale", scaleBox); id ("detail", detailBox);
        if (p.containsKey (prefKey ("multi"))) multiBtn.setToggleState (p.getBoolValue (prefKey ("multi")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("link"))) linkBtn.setToggleState (p.getBoolValue (prefKey ("link")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("roll"))) view.setRoll (p.getDoubleValue (prefKey ("roll")));
        num ("consLevel", level);
        tab = juce::jlimit (0, (int) kTabCount - 1, p.getIntValue (prefKey ("tab"), kTabTouch)); lightTab();
        const int sc = scaleBox.getSelectedId();
        if (sc == 6) view.setMel(); else { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; view.setScale (w[juce::jlimit (0, 4, sc - 1)]); }
    }
    void saveControls()
    {
        auto& p = uiPrefs();
        p.setValue (prefKey ("sens"), sens.getValue()); p.setValue (prefKey ("strength"), strength.getValue()); p.setValue (prefKey ("surround"), surround.getValue()); p.setValue (prefKey ("weight"), weight.getValue());
        p.setValue (prefKey ("amount"), amount.getValue()); p.setValue (prefKey ("residual"), residual.getValue());
        p.setValue (prefKey ("method"), methodBox.getSelectedId()); p.setValue (prefKey ("bands"), bandsBox.getSelectedId()); p.setValue (prefKey ("dir"), dirBox.getSelectedId());
        p.setValue (prefKey ("scale"), scaleBox.getSelectedId()); p.setValue (prefKey ("detail"), detailBox.getSelectedId());
        p.setValue (prefKey ("multi"), multiBtn.getToggleState()); p.setValue (prefKey ("link"), linkBtn.getToggleState()); p.setValue (prefKey ("roll"), view.rollValue()); p.setValue (prefKey ("tab"), tab);
        p.setValue (prefKey ("consLevel"), level.getValue());
    }
    /** Closing (Cancel, the X or Escape) with fixes made but not written back asks first (they used to go without a word - Escape is easily pressed).
        False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (committed.empty() || accepted) return true;
        const int n = (int) committed.size();
        juce::Component::SafePointer<RepairDialog> self (this);
        confirmAsync ("Spectral Repair", juce::String (n) + (n == 1 ? " fix has" : " fixes have") + " not been written back to the file yet. Close the window and throw "
                      + (n == 1 ? "it" : "them") + " away? (Write back to file keeps " + (n == 1 ? "it" : "them") + ".)",
                      n == 1 ? "Throw it away" : "Throw them away", [self] { if (self != nullptr) closeDialogOf (self.getComponent()); });
        return false;
    }
    ~RepairDialog() override
    {
        saveControls();
        ticker.stopTimer(); ticker.owner = nullptr;
        if (detailCancel != nullptr) detailCancel->store (true);
        if (ctx.stop) ctx.stop();
        if (! accepted)                                                    // closing without Commit puts everything back as it was
        {
            if (applied && ctx.unapply) ctx.unapply();
            if (prepared && ctx.discard) ctx.discard();
        }
        if (ctx.hold) ctx.hold (false);
    }
    // ---- the tools at the bottom: one tab for each tool (only one set of controls shows at a time, so the picture keeps the room), then the transport and the fixes ----
    enum { kTabTouch, kTabGain, kTabClick, kTabDenoise, kTabConsonant, kTabCount };
    static constexpr int kTabRows = (kTabCount + 1) / 2;                          // the tabs stand in two columns
    static constexpr int kRowH = 24, kRowGap = 4, kPanelPad = 6, kTabW = 104, kTabGap = 12, kCapGap = 12, kTransportH = 26, kActionW = 116;      // (kActionW: the button that does the work, the same size and in the same place on every tab)
    static constexpr int sliderW = 140;                                           // a slider with its number: short on purpose
    static constexpr int kChoicesW = 184 + 8 + 180 + 8 + 96 + 8 + 132 + 8 + 112;                       // the five choices of ReTouch in a row
    static constexpr int kAmountsW = (58 + sliderW) + kCapGap + (116 + sliderW) + kCapGap + (100 + sliderW) + 16 + kActionW;   // its three sliders and the button in a row
    static constexpr int kListenW = 70 + 6 + 60 + 6 + 64 + 12 + 62 + 170, kBarMinW = 90, kFixesW = 120 + 8 + 70 + 8 + 176 + 8 + 96;
    int tab = kTabTouch;
    juce::TextButton tabBtn[kTabCount];
    juce::Label gainHint;
    juce::Rectangle<int> panelR, contentR;                                       // the tool panel and the part of it that holds the controls (set by layoutBottom)
    /** ReTouch has two rows (the choices; the sliders and the button) and, where the window is narrow, one more for each that does not fit in a row. */
    static int reTouchRows (int w) { return (w >= kChoicesW ? 1 : 2) + (w >= kAmountsW ? 1 : 2); }
    static int transportRows (int w) { return w >= kListenW + 8 + kBarMinW + 16 + kFixesW ? 1 : 2; }
    static juce::Rectangle<int> rowOf (juce::Rectangle<int> inner, int row) { return { inner.getX(), inner.getY() + row * (kRowH + kRowGap), inner.getWidth(), kRowH }; }
    /** Puts a control at the left of the row (nothing if it is hidden: the ones after it move up to fill the space). */
    static void putLeft (juce::Rectangle<int>& row, juce::Component& c, int w, int gap = 8)
    {
        if (! c.isVisible()) return;
        c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
    }
    /** A caption and the slider after it. */
    static void putSlider (juce::Rectangle<int>& row, juce::Label& cap, juce::Slider& sl, int capW)
    {
        if (! sl.isVisible()) { cap.setBounds ({}); return; }
        cap.setBounds (row.removeFromLeft (capW)); sl.setBounds (row.removeFromLeft (sliderW)); row.removeFromLeft (kCapGap);
    }
    std::vector<juce::Component*> tabControls (int t)
    {
        switch (t)
        {
            case kTabTouch:   return { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &strengthCap, &strength, &surroundCap, &surround, &weightCap, &weight, &previewBtn };
            case kTabGain:    return { &gainBtn, &gainHint };
            case kTabClick:   return { &sensCap, &sens, &declickBtn };
            case kTabConsonant: return { &copyBtn, &copyMoveBtn, &consHint, &consCap, &consBox, &renameBtn, &forgetBtn, &levelCap, &level,
                                         &nudgeCap, &nudgeL5, &nudgeL1, &nudgeRead, &nudgeR1, &nudgeR5, &pasteBtn };
            default:          return { &amountCap, &amount, &residualCap, &residual, &roomBtn, &roomCap, &denoiseBtn };
        }
    }
    /** Lights the tab that is showing and no other. (By hand, not with a radio group: JUCE's radio group runs the click action of the key it turns off, and that put ReTouch back on.) */
    void lightTab() { for (int i = 0; i < kTabCount; ++i) tabBtn[i].setToggleState (i == tab, juce::dontSendNotification); }
    void selectTab (int t)
    {
        tab = juce::jlimit (0, (int) kTabCount - 1, t);
        lightTab();
        refreshControls();                                                       // (which also lays the controls out again)
    }
    void setupTabs()
    {
        const char* names[] = { "ReTouch [R]", "Gain [G]", "De-click [D]", "Denoise [N]", "Consonants [C]" };
        const char* tips[] = { "ReTouch: rebuilds or turns down the box you drew from its surroundings (key R).", "Gain: makes the sound in the box louder or quieter (key G).",
                               "De-click: finds and mends clicks (key D).", "Denoise: takes the room noise out of the box (keys L and N).",
                               "Consonants: copy a consonant (every track, every channel, as each heard it), move an early or late one, or paste a kept one where one is missing (key C; P pastes; comma and full stop nudge)." };
        for (int i = 0; i < kTabCount; ++i)
        {
            auto& t = tabBtn[i]; t.setButtonText (names[i]); t.setTooltip (tips[i]); t.setClickingTogglesState (false);
            t.setColour (juce::TextButton::buttonOnColourId, theme::accent);
            t.onClick = [this, i] { selectTab (i); };
            addAndMakeVisible (t);
        }
        lightTab();
        gainHint.setText ("Louder or quieter inside the box, from silence up to +18 dB, and nothing else. Draw a box, then press Gain [G].", juce::dontSendNotification);
        gainHint.setColour (juce::Label::textColourId, theme::dimText); gainHint.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (gainHint);
    }
    /** The controls of the tab that is showing, in the panel's content area. */
    void placeTab (juce::Rectangle<int> in, int rows)
    {
        auto lastRow = rowOf (in, rows - 1);                                     // the button that does the work is in the same place whichever tab shows: the bottom right corner of the panel
        const auto action = lastRow.removeFromRight (kActionW);
        switch (tab)
        {
            case kTabTouch:
            {   // first row what to do and how, then how much, and the button that does it
                int row = 0;
                auto r = rowOf (in, row++);
                putLeft (r, methodBox, 184); putLeft (r, dirBox, 180); putLeft (r, bandsBox, 96);
                if (in.getWidth() < kChoicesW) r = rowOf (in, row++);                                        // narrow: the two ticks on a row of their own
                putLeft (r, multiBtn, 132); putLeft (r, linkBtn, 112);
                r = rowOf (in, row++);
                if (in.getWidth() >= kAmountsW)
                {
                    previewBtn.setBounds (action); r.removeFromRight (kActionW + 16);
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116); putSlider (r, weightCap, weight, 100);
                }
                else                                                                                           // narrow: Before <-> After and the button go on a second row
                {
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116);
                    r = rowOf (in, row++);
                    putSlider (r, weightCap, weight, 100);
                    previewBtn.setBounds (action);
                }
                break;
            }
            case kTabGain:
            {
                gainBtn.setBounds (action); gainHint.setBounds (rowOf (in, 0));
                break;
            }
            case kTabClick:
            {
                auto r = rowOf (in, 0);
                sensCap.setBounds (r.removeFromLeft (112)); r.removeFromLeft (8); sens.setBounds (r.removeFromLeft (sliderW));
                declickBtn.setBounds (action);
                break;
            }
            case kTabConsonant:
            {   // copy; the kept ones; then the level, the nudge and Paste (in the action corner)
                auto r0 = rowOf (in, 0);
                putLeft (r0, copyBtn, 150); putLeft (r0, copyMoveBtn, 150, 12); consHint.setBounds (r0);
                auto r1 = rowOf (in, 1);
                consCap.setBounds (r1.removeFromLeft (44)); r1.removeFromLeft (4);
                forgetBtn.setBounds (r1.removeFromRight (70)); r1.removeFromRight (6); renameBtn.setBounds (r1.removeFromRight (84)); r1.removeFromRight (8);
                consBox.setBounds (r1.withWidth (juce::jmin (r1.getWidth(), 520)));
                pasteBtn.setBounds (action);
                levelCap.setBounds (lastRow.removeFromLeft (44)); level.setBounds (lastRow.removeFromLeft (sliderW)); lastRow.removeFromLeft (kCapGap + 4);
                nudgeCap.setBounds (lastRow.removeFromLeft (48));
                nudgeL5.setBounds (lastRow.removeFromLeft (34)); lastRow.removeFromLeft (3); nudgeL1.setBounds (lastRow.removeFromLeft (28)); lastRow.removeFromLeft (3);
                nudgeRead.setBounds (lastRow.removeFromLeft (70)); lastRow.removeFromLeft (3);
                nudgeR1.setBounds (lastRow.removeFromLeft (28)); lastRow.removeFromLeft (3); nudgeR5.setBounds (lastRow.removeFromLeft (34));
                break;
            }
            default:
            {
                auto r1 = rowOf (in, 0), r2 = rowOf (in, 1);
                amountCap.setBounds (r1.removeFromLeft (62)); amount.setBounds (r1.removeFromLeft (sliderW)); r1.removeFromLeft (kCapGap);
                residualCap.setBounds (r1.removeFromLeft (66)); residual.setBounds (r1.removeFromLeft (sliderW));
                denoiseBtn.setBounds (action); r2.removeFromRight (kActionW + 8);
                roomBtn.setBounds (r2.removeFromLeft (190)); r2.removeFromLeft (8); roomCap.setBounds (r2);
                break;
            }
        }
    }
    /** The tool panel (the tabs in two columns on the left, the controls of the chosen one beside them) and under it the transport and the fixes.
        Returns the height they need at this width (place = false only measures). */
    int layoutBottom (juce::Rectangle<int> area, bool place)
    {
        const int tabsW = 2 * kTabW + kRowGap;
        const int contentW = juce::jmax (100, area.getWidth() - 2 * kPanelPad - tabsW - kTabGap);
        const int rows = juce::jmax ((int) kTabRows, reTouchRows (contentW));     // (the same height whichever tab shows, so the picture does not jump; the tabs need their rows)
        const int panelH = 2 * kPanelPad + rows * kRowH + (rows - 1) * kRowGap;
        const int trRows = transportRows (area.getWidth());
        const int total = panelH + trRows * (kRowGap + kTransportH);
        if (! place) return total;
        panelR = { area.getX(), area.getY(), area.getWidth(), panelH };
        auto inner = panelR.reduced (kPanelPad);
        auto tabsArea = inner.removeFromLeft (tabsW); inner.removeFromLeft (kTabGap);
        for (int i = 0; i < kTabCount; ++i) tabBtn[i].setBounds (tabsArea.getX() + (i % 2) * (kTabW + kRowGap), tabsArea.getY() + (i / 2) * (kRowH + kRowGap), kTabW, kRowH);
        contentR = inner;
        placeTab (contentR, rows);
        auto placeFixes = [this] (juce::Rectangle<int> r)
        {
            close.setBounds (r.removeFromRight (96)); r.removeFromRight (8);
            finishBtn.setBounds (r.removeFromRight (176)); r.removeFromRight (8);
            revertBtn.setBounds (r.removeFromRight (70)); r.removeFromRight (8);
            compareBtn.setBounds (r.removeFromRight (120));
        };
        auto row = juce::Rectangle<int> (area.getX(), panelR.getBottom() + kRowGap, area.getWidth(), kTransportH);
        if (trRows == 1) placeFixes (row.removeFromRight (kFixesW));
        playBtn.setBounds (row.removeFromLeft (70)); row.removeFromLeft (6);
        stopBtn.setBounds (row.removeFromLeft (60)); row.removeFromLeft (6);
        loopBtn.setBounds (row.removeFromLeft (64)); row.removeFromLeft (12);
        hearCap.setBounds (row.removeFromLeft (62)); hearBox.setBounds (row.removeFromLeft (170)); row.removeFromLeft (8);
        bar.setBounds (row.withTrimmedRight (trRows == 1 ? 16 : 0).reduced (0, 3));                      // (the progress bar of a scan or a fix, shown only while it runs)
        if (trRows == 2) placeFixes ({ area.getX(), row.getBottom() + kRowGap, area.getWidth(), kTransportH });
        return total;
    }
    void paint (juce::Graphics& g) override
    {
        if (panelR.isEmpty()) return;
        const auto b = panelR.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.16f)); g.fillRoundedRectangle (b, 5.0f);
        const auto line = theme::manuscript() ? theme::giltDim() : theme::border.brighter (0.25f);
        g.setColour (line); g.drawRoundedRectangle (b, 5.0f, 1.0f);
        const float dx = (float) contentR.getX() - (float) kTabGap * 0.5f;
        g.setColour (line.withAlpha (0.7f)); g.drawLine (dx, b.getY() + 5.0f, dx, b.getBottom() - 5.0f, 1.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        const int bottomH = layoutBottom (r, false);                             // how high the tools need to be at this width
        auto bottom = r.removeFromBottom (bottomH);
        layoutBottom (bottom, true);
        r.removeFromBottom (4);
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        { auto ir = r.removeFromBottom (22); specBtn.setBounds (ir.removeFromRight (150)); ir.removeFromRight (6); scaleBox.setBounds (ir.removeFromRight (300)); ir.removeFromRight (6); detailBox.setBounds (ir.removeFromRight (210)); info.setBounds (ir.reduced (2, 0)); }
        view.setBounds (r);
        repaint (bottom.expanded (2));
    }
protected:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)                                      // Space: play what is loaded, or stop
        {
            if (spaceIsForRecording()) return true;
            if (scanned && ! running) { if (isPlayingNow()) { if (ctx.stop) ctx.stop(); } else playPressed(); }
            return true;
        }
        if ((k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R') && ! k.getModifiers().isAnyModifierKeyDown())      // R: do the fix with the settings as they are
        {
            selectTab (kTabTouch);
            if (previewBtn.isEnabled()) preview (0);
            return true;
        }
        if ((k.getTextCharacter() == 'n' || k.getTextCharacter() == 'N') && ! k.getModifiers().isAnyModifierKeyDown())      // N: denoise the box
        {
            selectTab (kTabDenoise);
            if (denoiseBtn.isEnabled()) preview (2);
            return true;
        }
        if ((k.getTextCharacter() == 'l' || k.getTextCharacter() == 'L') && ! k.getModifiers().isAnyModifierKeyDown())      // L: this box is the room tone
        {
            selectTab (kTabDenoise);
            if (roomBtn.isEnabled()) roomFromBox();
            return true;
        }
        if ((k.getTextCharacter() == 'g' || k.getTextCharacter() == 'G') && ! k.getModifiers().isAnyModifierKeyDown())      // G: change the gain of the box
        {
            selectTab (kTabGain);
            if (gainBtn.isEnabled()) openGain();
            return true;
        }
        if ((k.getTextCharacter() == 'd' || k.getTextCharacter() == 'D') && ! k.getModifiers().isAnyModifierKeyDown())
        {
            selectTab (kTabClick);
            if (declickBtn.isEnabled()) preview (1);
            return true;
        }
        if ((k.getTextCharacter() == 'c' || k.getTextCharacter() == 'C') && ! k.getModifiers().isAnyModifierKeyDown())      // C: the Consonants tab
        {
            selectTab (kTabConsonant);
            return true;
        }
        if ((k.getTextCharacter() == 'p' || k.getTextCharacter() == 'P') && ! k.getModifiers().isAnyModifierKeyDown())      // P: paste the chosen consonant at the playhead
        {
            selectTab (kTabConsonant);
            if (pasteBtn.isEnabled()) pasteConsonant();
            return true;
        }
        {   // , and . nudge the consonant just pasted 1 ms earlier / later; < and > (Shift with them) 5 ms
            const auto ch = k.getTextCharacter();
            const int ms = ch == ',' ? -1 : ch == '.' ? 1 : ch == '<' ? -5 : ch == '>' ? 5 : 0;
            if (ms != 0 && ! k.getModifiers().isCommandDown() && ! k.getModifiers().isAltDown())
            {
                if (scanned && (running ? pasteRunning : lastIsPaste())) { selectTab (kTabConsonant); nudge (ms); }
                return true;
            }
        }
        if (scanned && view.zoomKey (k)) return true;                           // arrows: right / left zoom in / out in time, up / down in pitch
        return false;
    }
private:
    /** The room tone Denoise uses: a box of this picture marked with L (its time span; the whole spectrum of it), else the project's. */
    bool haveRoomTone() const { if (rtBoxT1 > rtBoxT0) return true; for (auto& p : ctx.roomMono) if (p.valid()) return true; return false; }
    /** The same in a few words, for the little caption in the Denoise box (the full sentence is its tooltip). */
    juce::String roomShortText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (ctx.roomSource.isNotEmpty()) return "Room tone: the project's";
        return "Room tone: none yet";
    }
    juce::String roomText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: the box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s of this picture";
        if (ctx.roomSource.isNotEmpty()) return "Room tone: the project's (" + ctx.roomSource + ")";
        return "Room tone: none yet - draw a box over room only and press L, or use the edit window's Room tone button";
    }
    void roomFromBox()
    {
        if (! scanned || running || ! view.hasSelection()) return;
        rtBoxT0 = view.selT0(); rtBoxT1 = view.selT1();
        if (rtBoxT1 - rtBoxT0 < 0.5) { status.setText ("The room-tone box is too short: make it at least half a second long (a second or more is better).", juce::dontSendNotification); rtBoxT0 = rtBoxT1 = -1.0; refreshControls(); return; }     // (what learnDenoise needs: a shorter box was taken here, then refused at Write back)
        status.setText (roomText() + ". Now draw a box round the noise to take out and press Denoise (N).", juce::dontSendNotification);
        refreshControls();
    }      // one window for both: ReTouch and De-click share the picture, the box and the list of fixes
    void updateSurround() { const bool lr = dirBox.getSelectedId() != 2; view.setSurround (scanned && ! consonantMethod(), surround.getValue(), dirBox.getSelectedId() - 1, lr ? weight.getValue() / 100.0 : 0.5); }
    bool consonantMethod() const { return methodBox.getSelectedId() == 3; }
    bool lookingAcross() const { return dirBox.getSelectedId() != 2 || consonantMethod(); }
    /** The changes that make up the repaired sound: everything accepted so far, plus the change being previewed. */
    std::vector<RepairOp> currentOps() const
    {
        return committed;
    }
    bool haveRepair() const { return ! committed.empty(); }
    void invalidate()
    {
        if (running) { changedWhileRunning = true; return; }
        dropPendingPrepared();
    }
    /** Back to the original sound, and the repaired files thrown away (they no longer match the settings). */
    void dropPrepared()
    {
        if (applied) { if (ctx.stop) ctx.stop(); if (ctx.unapply) ctx.unapply(); applied = false; }
        if (prepared) { if (ctx.discard) ctx.discard(); prepared = false; }
        preparedOps = 0;
    }
    /** Files that include a change which is not accepted (yet) are dropped; files that match the accepted changes are kept. */
    void dropPendingPrepared() { if (prepared && preparedOps != committed.size()) dropPrepared(); }
    bool isPlayingNow() const { return ctx.position && ctx.position() >= 0.0; }
    void tick()
    {
        const double t = ctx.position ? ctx.position() : -1.0;
        view.setPlayhead (t);
        playBtn.setButtonText (t >= 0.0 ? "Restart" : "Play");
    }
    struct Ticker : juce::Timer { RepairDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    /** What is played: the part of the picture that is in view (zoom in to hear less, out to hear more), not just the box. */
    void playRange()
    {
        if (ctx.play) ctx.play (view.playFrom(), juce::jmin (seconds(), view.visT1()), loopBtn.getToggleState());
    }
    void playOriginal()
    {
        if (applied) { if (ctx.stop) ctx.stop(); if (ctx.unapply) ctx.unapply(); applied = false; }
        playRange();
        status.setText ("Playing the ORIGINAL sound, the part you can see. The playhead is on the picture.", juce::dontSendNotification);
        refreshControls();
    }
    void playRepaired()
    {
        if (! applied)
        {
            if (! ctx.apply || ! ctx.apply()) { status.setText ("The audio changed while the repair was being made, so it cannot be played. Press Fix again.", juce::dontSendNotification); prepared = false; preparedOps = 0; refreshControls(); return; }
            applied = true;
        }
        playRange();
        status.setText ("Playing the REPAIRED sound (through the repair). Write back to file keeps it, Undo takes the last fix off, or draw another box and ReTouch / De-click again.", juce::dontSendNotification);
        refreshControls();
    }
    void startRepairedPlayback()
    {
        const auto ops = currentOps();
        if (running || ops.empty()) return;
        if (prepared && preparedOps == ops.size()) { playRepaired(); return; }
        dropPrepared();
        auto prep = ctx.prepare;
        status.setText ("Making the repaired sound ready to listen to...", juce::dontSendNotification);
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
        juce::Component::SafePointer<RepairDialog> self (this);
        const auto n = ops.size();
        runJob ([prep, ops] (AudioJob& j) { return prep (ops, j); },
                [self, n] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err == "Cancelled." ? juce::String ("Cancelled.") : err, juce::dontSendNotification); self->refreshControls(); return; }
                    self->prepared = true; self->preparedOps = n;
                    if (self->changedWhileRunning) { self->changedWhileRunning = false; self->invalidate(); return; }
                    self->playRepaired();
                });
    }
    void playPressed()
    {
        if (! scanned || running) return;
        if (ctx.stop) ctx.stop();
        if (hearBox.getSelectedId() == 2)
        {
            if (haveRepair()) { startRepairedPlayback(); return; }
            status.setText ("There is no repair to listen to yet: press ReTouch or De-click first. Playing the original.", juce::dontSendNotification);
        }
        playOriginal();
    }
    void refreshControls()
    {
        for (auto* c : tabControls (tab)) c->setVisible (true);                    // (the rules below hide what the chosen method does not use)
        updateSurround();
        const bool cons = consonantMethod();
        const bool att = methodBox.getSelectedId() == 1 || cons;
        for (auto* c : std::initializer_list<juce::Component*> { &strength, &strengthCap, &methodBox }) c->setVisible (true);
        for (auto* c : std::initializer_list<juce::Component*> { &dirBox, &surround, &surroundCap }) c->setVisible (! cons);      // a stray consonant: always the sound before / after, its own search length
        weightCap.setText (cons ? "Side (50 = auto)" : "Before <-> After", juce::dontSendNotification);
        bandsBox.setVisible (att); multiBtn.setVisible (att); linkBtn.setVisible (att);
        weight.setVisible (lookingAcross()); weightCap.setVisible (lookingAcross());
        sens.setVisible (true); sensCap.setVisible (true);
        for (int t = 0; t < kTabCount; ++t) if (t != tab) for (auto* c : tabControls (t)) c->setVisible (false);      // only the tab that is showing
        if (view.hasSelection())
            info.setText ("Selected: " + juce::String (view.selT0(), 3) + " - " + juce::String (view.selT1(), 3) + " s  ("
                          + juce::String ((view.selT1() - view.selT0()) * 1000.0, 0) + " ms),  " + juce::String ((int) view.selF0()) + " - " + juce::String ((int) view.selF1()) + " Hz"
                          + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        else info.setText (juce::String ("Nothing selected")
                           + (committed.empty() ? juce::String() : juce::String ("      ") + juce::String ((int) committed.size()) + " fix(es) made"), juce::dontSendNotification);
        const bool ready = scanned && ! running;
        playBtn.setEnabled (ready); stopBtn.setEnabled (ready); loopBtn.setEnabled (ready); hearBox.setEnabled (ready);
        previewBtn.setEnabled (ready && view.hasSelection()); declickBtn.setEnabled (ready); gainBtn.setEnabled (ready && view.hasSelection());
        denoiseBtn.setEnabled (ready && view.hasSelection() && haveRoomTone()); roomBtn.setEnabled (ready && view.hasSelection());
        roomCap.setText (roomShortText(), juce::dontSendNotification); roomCap.setTooltip (roomText());
        finishBtn.setEnabled (ready && (haveRepair() || ctx.pairWritten != nullptr));
        revertBtn.setEnabled (ready && ! history.empty());
        compareBtn.setEnabled (ready && ! history.empty());
        compareBtn.setButtonText (showingAfter ? "Show before" : "Show fixed");
        refreshConsonantList();
        const bool canCopy = ready && view.hasSelection() && ctx.readChannels != nullptr && ctx.keepConsonant != nullptr;
        copyBtn.setEnabled (canCopy); copyMoveBtn.setEnabled (canCopy);
        const bool haveSel = selectedConsonant() != nullptr;
        consBox.setEnabled (! consList.empty()); renameBtn.setEnabled (haveSel); forgetBtn.setEnabled (haveSel);
        pasteBtn.setEnabled (ready && haveSel);
        const bool adj = scanned && (running ? pasteRunning : lastIsPaste());
        for (auto* b : { &nudgeL5, &nudgeL1, &nudgeR1, &nudgeR5 }) b->setEnabled (adj);
        nudgeRead.setText (nudgeText(), juce::dontSendNotification);
        resized();                                   // (what is shown decides how the boxes are packed)
    }
    /** The settings as they are now (and the part of the area they apply to). what: 0 = ReTouch (or Gain), 1 = De-click, 2 = Denoise. */
    FixSpec buildSpec (juce::int64& t0, juce::int64& t1, int what) const
    {
        FixSpec spec; t0 = 0; t1 = ctx.length;
        if (view.hasSelection()) { t0 = (juce::int64) std::llround (view.selT0() * ctx.rate); t1 = (juce::int64) std::llround (view.selT1() * ctx.rate); }
        if (what == 2)
        {
            spec.kind = FixSpec::Kind::Denoise; spec.f0 = view.selF0(); spec.f1 = view.selF1();
            spec.dn.amountDb = amount.getValue(); spec.dn.residualDb = residual.getValue();
            spec.dnBoxT0 = rtBoxT1 > rtBoxT0 ? rtBoxT0 : -1.0; spec.dnBoxT1 = rtBoxT1 > rtBoxT0 ? rtBoxT1 : -1.0;
        }
        else if (what == 0)
        {
            spec.kind = FixSpec::Kind::Patch; spec.f0 = view.selF0(); spec.f1 = view.selF1(); spec.sensitivity = sens.getValue();         // (the click sensitivity also decides how readily lost samples (a digital dropout) in a small box are found)
            spec.repair.mode = methodBox.getSelectedId() == 2 ? RepairMode::Replace : RepairMode::Attenuate;
            spec.repair.consonant = consonantMethod();
            spec.repair.bands = bandsBox.getSelectedId(); spec.repair.multiRes = multiBtn.getToggleState(); spec.repair.linkChannels = linkBtn.getToggleState();
            spec.repair.strength = strength.getValue() / 100.0;
            spec.repair.direction = spec.repair.consonant ? RepairDirection::LeftRight : (RepairDirection) (dirBox.getSelectedId() - 1);
            spec.repair.contextPct = surround.getValue();
            spec.repair.weighting = weight.getValue() / 100.0;
            if (gainNext) { spec.repair.mode = RepairMode::Gain; spec.repair.gainDb = gainNextDb; spec.repair.strength = 1.0; spec.repair.consonant = false; }
        }
        else { spec.kind = FixSpec::Kind::Declick; spec.sensitivity = sens.getValue(); }
        return spec;
    }
    double seconds() const { return (double) ctx.length / ctx.rate; }
    /** The Gain pop-up: a slider from silence to +18 dB for the box; Apply makes it a fix like ReTouch (Undo takes it off). */
    void openGain()
    {
        if (! scanned || running || ! view.hasSelection()) return;
        struct GainPop : public juce::Component
        {
            juce::Slider sl; juce::TextButton apply { "Apply" }, cancel { "Cancel" }; juce::Label cap;
            std::function<void (double)> onApply;
            GainPop (double start)
            {
                cap.setText ("Gain of the box", juce::dontSendNotification); cap.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (cap);
                sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (-60.0, 18.0, 0.1); sl.setValue (start, juce::dontSendNotification);
                sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22); sl.setDoubleClickReturnValue (true, 0.0);
                sl.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : (v > 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };
                sl.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.retainCharacters ("-+0123456789.").getDoubleValue(); };
                sl.setSkewFactorFromMidPoint (-6.0);
                addAndMakeVisible (sl); addAndMakeVisible (apply); addAndMakeVisible (cancel);
                apply.setColour (juce::TextButton::buttonColourId, theme::accent);
                apply.onClick = [this] { const double v = sl.getValue(); auto cb = onApply; if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); if (cb) cb (v); };
                cancel.onClick = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
                setSize (380, 92);
            }
            void resized() override
            {
                auto r = getLocalBounds().reduced (8);
                cap.setBounds (r.removeFromTop (20)); sl.setBounds (r.removeFromTop (28)); r.removeFromTop (6);
                cancel.setBounds (r.removeFromRight (80)); r.removeFromRight (6); apply.setBounds (r.removeFromRight (80));
            }
        };
        auto pop = std::make_unique<GainPop> (uiPrefs().getDoubleValue (prefKey ("gainDb"), -6.0));
        juce::Component::SafePointer<RepairDialog> self (this);
        pop->onApply = [self] (double db)
        {
            if (self == nullptr) return;
            uiPrefs().setValue (self->prefKey ("gainDb"), db);
            if (std::abs (db) < 0.05) { self->status.setText ("Gain 0 dB: nothing to change.", juce::dontSendNotification); return; }
            self->gainNext = true; self->gainNextDb = db <= -59.95 ? -200.0 : db;
            self->preview (0);
            self->gainNext = false;
        };
        juce::CallOutBox::launchAsynchronously (std::move (pop), gainBtn.getScreenBounds(), nullptr);
    }
    void preview (int what)
    {
        if (! scanned || running || origTracks == nullptr) return;
        if (what == 2 && ! haveRoomTone()) { status.setText ("No room tone: draw a box over a part that is only room and press L, or use the edit window's Room tone button first.", juce::dontSendNotification); return; }
        juce::int64 t0 = 0, t1 = 0; const auto spec = buildSpec (t0, t1, what);
        startFix (spec, t0, t1);
    }
    /** Makes one change on a job thread - on the picture's sound as it is now (every fix so far), track by track - and keeps it as the next fix (Undo takes it
        off). 'placed': a pasted consonant, where it was first put (a nudge keeps it, so the window can say how far it has been moved). */
    void startFix (const FixSpec& spec, juce::int64 t0, juce::int64 t1, juce::int64 placed = -1)
    {
        if (! scanned || running || origTracks == nullptr) return;
        dropPendingPrepared();
        pvSpec = spec; pvT0 = t0; pvT1 = t1; pvPlaced = placed;
        pasteRunning = spec.kind == FixSpec::Kind::Paste;
        auto src = origTracks; const double rate = ctx.rate;
        const auto trackIds = ctx.trackIds;
        struct Out { MergedSpectrogram spec; std::shared_ptr<std::vector<std::vector<float>>> tracks; FixSpec fix; SpectralRepair::DeclickReport rep { 0, 0, 0, 0 }; };
        auto out = std::make_shared<Out>();
        status.setText (spec.kind == FixSpec::Kind::Denoise ? "Denoising the box..." : spec.kind == FixSpec::Kind::Paste ? "Pasting the consonant..." : "Fixing...", juce::dontSendNotification);
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); denoiseBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false);
        juce::Component::SafePointer<RepairDialog> self (this);
        const int detailN = detailSize();
        auto roomMono = std::make_shared<std::vector<denoise::Profile>> (ctx.roomMono);
        auto firstTracks = history.empty() ? origTracks : history.front().tracks;   // the sound as it was loaded, before any of these fixes
        runJob ([src, firstTracks, out, spec, t0, t1, rate, detailN, roomMono, trackIds] (AudioJob& j) -> juce::String
                {
                    auto tracks = std::make_shared<std::vector<std::vector<float>>> (*src);       // from the last accepted state
                    // a denoise: each picture track's room tone - the box of this picture, learned from the track as it was loaded (as the files are, when they are
                    // written: once a fix had denoised the room tone itself, the picture learned a profile far too low from it), or the project's for that track
                    std::vector<std::shared_ptr<const std::vector<denoise::Profile>>> dnProf;
                    if (spec.kind == FixSpec::Kind::Denoise)
                        for (size_t i = 0; i < tracks->size(); ++i)
                        {
                            denoise::Profile pr;
                            if (spec.dnBoxT1 > spec.dnBoxT0)
                            {
                                const auto& x = firstTracks != nullptr && i < firstTracks->size() ? (*firstTracks)[i] : (*tracks)[i];
                                const auto a = juce::jlimit ((juce::int64) 0, (juce::int64) x.size(), (juce::int64) std::llround (spec.dnBoxT0 * rate)), b = juce::jlimit ((juce::int64) 0, (juce::int64) x.size(), (juce::int64) std::llround (spec.dnBoxT1 * rate));
                                if (b > a) pr = denoise::learnProfile ({ std::vector<float> (x.begin() + a, x.begin() + b) }, rate, spec.dn);
                            }
                            else if (i < roomMono->size()) pr = (*roomMono)[i];
                            if (! pr.valid()) return "There is no room tone for track " + juce::String ((int) i + 1) + " (learn it with the edit window's Room tone button, or mark a box of this picture with L).";
                            dnProf.push_back (std::make_shared<const std::vector<denoise::Profile>> (std::vector<denoise::Profile> { pr }));
                        }
                    // lost samples (a digital jump where a block of samples went missing) are one event on every track: they are found on all the tracks together, and the
                    // list goes with the fix, so the files are written with exactly the same mend
                    auto fix = spec;
                    if (audioops::findsLost (spec, rate, t0, t1) && ! tracks->empty())
                    {
                        const long L = (long) (*tracks)[0].size();
                        fix.lost = audioops::findLost (*tracks, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), spec.sensitivity);
                        for (auto& l : fix.lost) l.at += juce::jlimit ((juce::int64) 0, (juce::int64) L, t0) - t0;          // (from t0 itself)
                        fix.lostKnown = true;
                    }
                    // a stray consonant: which side of the box the main consonant is on is ONE answer for all the tracks (found on all of them together); it goes with
                    // the fix so the files are written the same way
                    if (fix.kind == FixSpec::Kind::Patch && fix.repair.mode == RepairMode::Attenuate && fix.repair.consonant && std::abs (fix.repair.weighting - 0.5) < 0.04 && ! tracks->empty())
                    {
                        const long L = (long) (*tracks)[0].size();
                        std::vector<const std::vector<float>*> tp; for (auto& t : *tracks) tp.push_back (&t);
                        const auto d = SpectralRepair::consonantSides (tp, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), fix.f0, fix.f1);
                        fix.repair.consonantBefore = d.first; fix.repair.consonantEdge = d.second;
                    }
                    out->fix = fix;
                    for (size_t i = 0; i < tracks->size(); ++i)
                    {
                        if (j.cancelled()) return "Cancelled.";
                        j.setProgress ((float) i / (float) tracks->size());
                        std::vector<std::vector<float>> ch; ch.push_back (std::move ((*tracks)[i]));
                        auto fi = fix; if (i < dnProf.size()) fi.dnProfiles = dnProf[i];
                        if (fi.kind == FixSpec::Kind::Paste) audioops::resolvePaste (fi, i < trackIds.size() ? trackIds[i] : juce::Uuid::null());     // (the picture's row: the track's channels, averaged)
                        audioops::applyFix (ch, rate, (long) t0, (long) t1, fi, (long) (0.02 * rate));
                        (*tracks)[i] = std::move (ch[0]);
                        if (fix.kind == FixSpec::Kind::Declick) { const auto r = SpectralRepair::lastDeclickReport(); out->rep.mended += r.mended; out->rep.lines += r.lines; out->rep.linesMended += r.linesMended; out->rep.linesLeft += r.linesLeft; }
                    }
                    out->fix.dnProfiles = nullptr;                                                // (the profiles are not part of the fix that is kept)
                    out->fix.pasteChannels = nullptr;                                             // (nor is the one track's part of a pasted consonant: each file gets its own)
                    out->spec = SpectralRepair::spectrogram (*tracks, rate, 1600, detailN);
                    out->tracks = tracks;
                    return {};
                },
                [self, out] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->nextNote.clear(); self->pendingNudge = 0; self->pendingLevel = false; self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    // the fix is kept straight away (it stays temporary until "Write back to file"); Undo takes it off again
                    self->history.push_back ({ self->origTracks, self->origSpec });
                    self->pvSpec = out->fix;                                             // (with the lost samples found on all the tracks)
                    self->committed.push_back (RepairOp { self->pvSpec, self->pvT0, self->pvT1, self->pvPlaced });
                    self->origTracks = out->tracks;
                    self->origSpec = std::make_shared<MergedSpectrogram> (out->spec);
                    self->view.setData (std::move (out->spec), self->seconds(), true);
                    self->showingAfter = true;
                    self->dropPendingPrepared();
                    self->changedWhileRunning = false;
                    self->hearBox.setSelectedId (2, juce::dontSendNotification);
                    juce::String lostNote = self->nextNote; self->nextNote.clear();
                    for (auto& l : out->fix.lost)
                        lostNote << "Digital dropout at " << juce::String ((double) (self->pvT0 + l.at) / self->ctx.rate, 3) << " s: " << l.count << " samples were missing on every track; put back. ";
                    if (out->fix.kind == FixSpec::Kind::Patch && out->fix.repair.mode == RepairMode::Gain)
                        lostNote << "Gain " << (out->fix.repair.gainDb <= -120.0 ? juce::String ("-inf") : (out->fix.repair.gainDb > 0 ? "+" : "") + juce::String (out->fix.repair.gainDb, 1)) << " dB on the box. ";
                    if (out->fix.kind == FixSpec::Kind::Denoise)
                        lostNote << "Denoised the box (" << (int) out->fix.f0 << " - " << (int) out->fix.f1 << " Hz, residual " << juce::String (out->fix.dn.residualDb, 0) << " dB). ";
                    if (out->fix.kind == FixSpec::Kind::Declick)
                    {
                        const auto& r = out->rep;
                        lostNote << "De-click, all tracks: " << r.linesMended << " vertical line(s) seen in the picture and mended whole, " << r.mended << " click(s) found in the waveform and mended"
                                 << (r.linesLeft > 0 ? ", " + juce::String (r.linesLeft) + " line(s) STILL THERE (try a higher sensitivity, or ReTouch a box round each)" : juce::String (", none left in the picture")) << ". ";
                    }
                    if (out->fix.kind == FixSpec::Kind::Paste && out->fix.paste != nullptr)
                    {
                        const auto missing = consonants::tracksMissing (*out->fix.paste, self->ctx.trackIds);
                        int here = 0; for (auto& id : self->ctx.trackIds) if (out->fix.paste->forTrack (id) != nullptr) ++here;
                        lostNote << "Consonant pasted at " << juce::String ((double) (self->pvT0 + out->fix.paste->lead) / self->ctx.rate, 3) << " s on " << here << (here == 1 ? " track" : " tracks")
                                 << (std::abs (out->fix.pasteGainDb) >= 0.05 ? ", " + juce::String (out->fix.pasteGainDb, 1) + " dB" : juce::String())
                                 << (self->pvPlaced >= 0 && self->pvT0 != self->pvPlaced ? ", nudged " + self->nudgeText() : juce::String()) << ". ";
                        int none = 0; for (auto& id : self->ctx.trackIds) if (out->fix.paste->forTrack (id) == nullptr) ++none;
                        if (none > 0) lostNote << none << (none == 1 ? " track of this window was" : " tracks of this window were") << " not in it and get nothing. ";
                        if (! missing.isEmpty()) lostNote << "(It also has " << missing.joinIntoString (", ") << ", not in this window.) ";
                        lostNote << "Press Play to listen; nudge it with comma / full stop (1 ms) or Shift with them (5 ms), or change Level; Undo takes it off. ";
                        self->status.setText (lostNote, juce::dontSendNotification);
                        self->refreshControls();
                        self->followPending();
                        return;
                    }
                    if (out->fix.kind == FixSpec::Kind::Patch && out->fix.repair.consonant)
                    {
                        const auto& r = out->fix.repair;
                        if (r.consonantBefore < 0.0) lostNote << "Stray consonant: matched to the side you chose with Before / After. ";
                        else if (r.consonantEdge > 0) lostNote << "Stray consonant: the main one is just AFTER the box, so every track was matched to the sound before it. ";
                        else if (r.consonantEdge < 0) lostNote << "Stray consonant: the main one is just BEFORE the box, so every track was matched to the sound after it. ";
                        else lostNote << "Stray consonant: no main consonant right next to the box, so both sides were used (" << juce::roundToInt (r.consonantBefore * 100.0) << " % before). ";
                    }
                    self->status.setText (lostNote + "Fix " + juce::String ((int) self->committed.size()) + " done (not written yet). Press Play to listen to it. If it worked, draw a box round the next problem and press ReTouch or De-click again, or press Write back to file. If not, press Undo and try again.", juce::dontSendNotification);
                    self->refreshControls();
                });
        for (auto* b : { &nudgeL5, &nudgeL1, &nudgeR1, &nudgeR5 }) b->setEnabled (pasteRunning);     // (a nudge pressed now follows when this one is made)
    }
    int detailSize() const { return juce::jlimit (0, 2, detailBox.getSelectedId() - 1); }
    /** Redraws the picture of the sound as it is now with another low-end detail (a longer or shorter analysis window). */
    void rescan()
    {
        if (! scanned || running || origTracks == nullptr) return;
        auto src = origTracks; const double rate = ctx.rate; const int n = detailSize();
        auto out = std::make_shared<MergedSpectrogram>();
        status.setText ("Redrawing with another low-end detail...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        runJob ([src, out, rate, n] (AudioJob&) -> juce::String { *out = SpectralRepair::spectrogram (*src, rate, 1600, n); return {}; },
                [self, out] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty() && out->frames > 0)
                    {
                        self->origSpec = std::make_shared<MergedSpectrogram> (*out);
                        self->view.setData (std::move (*out), self->seconds(), true);
                        self->showingAfter = true;
                        self->status.setText ("Redrawn. Blended: fine bass below, sharp clicks above. Choose \"off\" for the sharpest timing everywhere.", juce::dontSendNotification);
                    }
                    self->refreshControls();
                });
    }
    /** The audio the picture shows now (the fixed sound, or the sound before the last fix). */
    std::shared_ptr<std::vector<std::vector<float>>> shownTracks() const
    {
        if (! showingAfter && ! history.empty() && history.back().tracks != nullptr) return history.back().tracks;
        return origTracks;
    }
    /** Makes the sharp picture of what is in view on its own thread (the previous request, if still running, is dropped). */
    void startDetail (const PictureRequest& rq)
    {
        auto tr = shownTracks(); if (tr == nullptr || tr->empty()) return;
        if (detailCancel != nullptr) detailCancel->store (true);
        auto cancel = std::make_shared<std::atomic<bool>> (false); detailCancel = cancel;
        const double rate = ctx.rate; juce::Component::SafePointer<RepairDialog> self (this);
        std::thread ([tr, rq, rate, cancel, self]
        {
            try                                                                  // (nothing may escape a thread: out of memory leaves the quick picture, nothing worse)
            {
                auto pic = picture::render (*tr, rate, rq.t0, rq.t1, rq.w, rq.h, rq.axis, rq.opt, [cancel] { return cancel->load(); });
                pic.id = rq.id;
                if (cancel->load()) return;
                juce::MessageManager::callAsync ([self, p = std::make_shared<DetailPicture> (std::move (pic))] { if (self != nullptr) self->view.setDetail (std::move (*p)); });
            }
            catch (...) {}
        }).detach();
    }
    void openSpectroSettings();
    void toggleCompare()
    {
        if (history.empty() || origSpec == nullptr || history.back().spec == nullptr) return;
        showingAfter = ! showingAfter;
        view.setData (showingAfter ? *origSpec : *history.back().spec, seconds(), true);
        status.setText (showingAfter ? "Showing the fixed sound." : "Showing the sound before the last fix.", juce::dontSendNotification);
        refreshControls();
    }
    /** Undo: takes the last fix off again (as many times as there are fixes). */
    void revert()
    {
        if (history.empty() || running) return;
        if (ctx.stop) ctx.stop();
        origTracks = history.back().tracks; origSpec = history.back().spec; history.pop_back();
        if (! committed.empty()) committed.pop_back();
        dropPendingPrepared();
        showingAfter = true;
        if (committed.empty()) hearBox.setSelectedId (1, juce::dontSendNotification);
        view.setData (*origSpec, seconds(), true);
        status.setText (committed.empty() ? "Undone: back to the original. Nothing is changed." : "Last fix undone (" + juce::String ((int) committed.size()) + " fix(es) left).", juce::dontSendNotification);
        refreshControls();
    }
    /** Finish: everything between the marks is rendered again in one go, with every accepted change, and put back. */
    void finish()
    {
        if (running) return;
        const auto ops = currentOps();
        if (ops.empty())
        {
            if (ctx.pairWritten) { if (ctx.stop) ctx.stop(); accepted = true; ctx.pairWritten ([] { return true; }); return; }          // an untouched stream is left as it is; this window goes away
            closeDialogOf (this); return;
        }
        if (prepared && preparedOps == ops.size()) { finishCommit(); return; }
        dropPrepared();
        auto prep = ctx.prepare;
        previewBtn.setEnabled (false); declickBtn.setEnabled (false); finishBtn.setEnabled (false); revertBtn.setEnabled (false); compareBtn.setEnabled (false); playBtn.setEnabled (false);
        status.setText ("Writing: repairing every track from the first mark to the last and making the new files...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        const auto n = ops.size();
        runJob ([prep, ops] (AudioJob& j) { return prep (ops, j); },
                [self, n] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isEmpty()) { self->prepared = true; self->preparedOps = n; self->finishCommit(); }
                    else { self->status.setText (err, juce::dontSendNotification); self->refreshControls(); }
                });
    }
    void finishCommit()
    {
        if (ctx.stop) ctx.stop();
        if (ctx.pairWritten)
        {
            accepted = true;                                         // (the corrected files wait in the pair; this window goes away and the other stream is next)
            ctx.pairWritten ([] { return true; });
            return;
        }
        if (! applied)
        {
            if (! ctx.apply || ! ctx.apply()) { status.setText ("The audio changed while the repair was being made, so nothing was changed. Please try again.", juce::dontSendNotification); prepared = false; preparedOps = 0; refreshControls(); return; }
            applied = true;
        }
        if (ctx.accept) ctx.accept();
        accepted = true;
        closeDialogOf (this);
    }
    RepairContext ctx;
    InfoNote intro; juce::Label info, sensCap, strengthCap, surroundCap, weightCap, amountCap, residualCap, roomCap;
    SpectrogramView view; juce::ComboBox dirBox, scaleBox, detailBox, methodBox, bandsBox; juce::ToggleButton multiBtn { "Multi-resolution" }, linkBtn { "Link stereo" }; juce::TextButton specBtn { "Spectrogram settings..." };
    std::shared_ptr<std::atomic<bool>> detailCancel; juce::Slider sens, strength, surround, weight, amount, residual;
    juce::TextButton roomBtn { "This box is the room tone [L]" }, denoiseBtn { "Denoise [N]" }; double rtBoxT0 = -1.0, rtBoxT1 = -1.0;
    juce::TextButton playBtn { "Play" }, stopBtn { "Stop" };
    juce::ToggleButton loopBtn { "Loop" };
    juce::Label hearCap; juce::ComboBox hearBox;
    bool prepared = false, applied = false, accepted = false, changedWhileRunning = false;
    size_t preparedOps = 0;                                        // how many changes the prepared files contain
    std::vector<RepairOp> committed;                               // the changes accepted so far (in order)
    juce::TextButton previewBtn { "ReTouch [R]" }, declickBtn { "De-click [D]" }, compareBtn { "Show before" }, revertBtn { "Undo" }, finishBtn { "Write back to file" }, close { "Cancel" };
    juce::TextButton gainBtn { "Gain [G]" };
    bool gainNext = false; double gainNextDb = 0.0;                // the next fix is a gain change of the box (set by the Gain pop-up)
    struct Step { std::shared_ptr<std::vector<std::vector<float>>> tracks; std::shared_ptr<MergedSpectrogram> spec; };
    std::vector<Step> history;                                     // the state before each fix, for Undo
    bool scanned = false, showingAfter = false;
    std::shared_ptr<std::vector<std::vector<float>>> origTracks;
    std::shared_ptr<MergedSpectrogram> origSpec;
    FixSpec pvSpec; juce::int64 pvT0 = 0, pvT1 = 0, pvPlaced = -1;

    // ---- consonants (3.6.0): Copy / Copy and remove keep what the stray-consonant ReTouch takes out of the box (every track, every channel); Paste adds a kept one
    //      at the playhead; the last paste can be nudged and its level changed (it is taken off and put back) ----
    juce::TextButton copyBtn { "Copy consonant" }, copyMoveBtn { "Copy and remove" }, renameBtn { "Rename..." }, forgetBtn { "Delete" }, pasteBtn { "Paste here [P]" };
    juce::TextButton nudgeL5 { "<<" }, nudgeL1 { "<" }, nudgeR1 { ">" }, nudgeR5 { ">>" };
    juce::Label consHint, consCap, levelCap, nudgeCap, nudgeRead;
    juce::ComboBox consBox;
    juce::Slider level;
    std::vector<ConsonantDef> consList;                            // the project's kept consonants, as the list shows them
    juce::String consListKey;                                      // (what the list was made from: it is made again only when that changes)
    juce::Uuid consSel;                                            // the one chosen in the list
    std::map<juce::String, std::shared_ptr<const ConsonantAudio>> consCache;    // the audio of the ones pasted while this window is open
    juce::String lastConsName { "t" };
    juce::String nextNote;                                         // said first when the fix under way is done (Copy and remove: what to do next)
    bool pasteRunning = false;                                     // the fix under way is a paste (or a nudge of one)
    juce::int64 pendingNudge = 0; bool pendingLevel = false;       // nudges / a level change pressed while it is being made

    void setupConsonants()
    {
        auto label = [this] (juce::Label& l, const juce::String& t, bool dim = false)
        {
            l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, dim ? theme::dimText : theme::text); l.setMinimumHorizontalScale (0.8f); addChildComponent (l);
        };
        label (consHint, "Draw the box round the consonant from about 2 kHz up, then Copy.", true);
        label (consCap, "Kept:"); label (levelCap, "Level"); label (nudgeCap, "Nudge");
        nudgeRead.setText ({}, juce::dontSendNotification); nudgeRead.setJustificationType (juce::Justification::centred); nudgeRead.setColour (juce::Label::textColourId, theme::text); addChildComponent (nudgeRead);
        nudgeRead.setTooltip ("How far the consonant just pasted has been nudged from where it was first put.");
        copyBtn.setTooltip ("Keeps the consonant in the box with the project: what the stray-consonant ReTouch would take out of it (the consonant itself, above the vowel and the room), "
                            "on EVERY track and every channel, each as that microphone heard it - nothing is mixed down. The audio here is not changed. "
                            "It can be pasted into any ReTouch window of this project, today or weeks later (until you delete it).");
        copyMoveBtn.setTooltip ("For an early or late consonant: keeps it (as Copy consonant does) and takes it out here, as the stray-consonant ReTouch does at full strength. "
                                "Then click the time ruler where it should sound and press Paste here [P]: each track's own part of it goes back on that track.");
        renameBtn.setTooltip ("Gives the chosen consonant another name (t, s, sh, soft d ...).");
        forgetBtn.setTooltip ("Takes the chosen consonant off this project's list and deletes its audio. Fixes already made with it stay as they are.");
        pasteBtn.setTooltip ("Adds the chosen consonant on top of the sound, the start of the box it was copied from at the playhead you placed by clicking the time ruler. "
                             "Each track gets its own part of it (a track it was not copied from gets nothing). Same rules as ReTouch: Play to listen, Undo takes it off, Write back keeps it. Key: P");
        consBox.setTooltip ("The consonants kept with this project (Copy consonant adds one; Copy and remove adds one too). Paste uses the one chosen here.");
        consBox.onChange = [this]
        {
            const int i = consBox.getSelectedItemIndex();
            if (juce::isPositiveAndBelow (i, (int) consList.size())) consSel = consList[(size_t) i].id;
            refreshControls();
        };
        level.setSliderStyle (juce::Slider::LinearHorizontal); level.setRange (-24.0, 12.0, 0.5); level.setValue (0.0, juce::dontSendNotification); level.setScrollWheelEnabled (false);
        level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22); level.setTextValueSuffix (" dB"); level.setDoubleClickReturnValue (true, 0.0);
        level.setTooltip ("How loud the consonant goes in, against how it was copied (0 dB = as it was). Changing it changes the consonant just pasted, too. Double-click for 0 dB.");
        level.onValueChange = [this] { if (! level.isMouseButtonDown()) levelChanged(); };
        level.onDragEnd = [this] { levelChanged(); };
        addChildComponent (level);
        auto nb = [this] (juce::TextButton& b, int ms, const juce::String& tip)
        {
            b.setTooltip (tip); b.onClick = [this, ms] { nudge (ms); }; b.setWantsKeyboardFocus (false); addChildComponent (b);
        };
        nb (nudgeL5, -5, "The consonant just pasted 5 ms earlier (Shift + comma)."); nb (nudgeL1, -1, "The consonant just pasted 1 ms earlier (comma).");
        nb (nudgeR1, 1, "The consonant just pasted 1 ms later (full stop)."); nb (nudgeR5, 5, "The consonant just pasted 5 ms later (Shift + full stop).");
        copyBtn.onClick = [this] { copyConsonant (false); };
        copyMoveBtn.onClick = [this] { copyConsonant (true); };
        renameBtn.onClick = [this] { renamePressed(); };
        forgetBtn.onClick = [this] { forgetPressed(); };
        pasteBtn.onClick = [this] { pasteConsonant(); };
        pasteBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        for (auto* c : std::initializer_list<juce::Component*> { &copyBtn, &copyMoveBtn, &renameBtn, &forgetBtn, &pasteBtn, &consBox }) { addChildComponent (c); c->setWantsKeyboardFocus (false); }
    }
    static juce::String clockText (double seconds)
    {
        const int m = (int) (seconds / 60.0); const double sec = seconds - 60.0 * m;
        return juce::String (m) + ":" + (sec < 10.0 ? "0" : "") + juce::String (sec, 2);
    }
    const ConsonantDef* selectedConsonant() const { for (auto& c : consList) if (c.id == consSel) return &c; return nullptr; }
    bool lastIsPaste() const { return ! committed.empty() && committed.back().spec.kind == FixSpec::Kind::Paste; }
    juce::String nudgeText() const
    {
        if (! lastIsPaste() || committed.back().placed < 0) return {};
        const double ms = 1000.0 * (double) (committed.back().t0 - committed.back().placed) / ctx.rate;
        return (ms > 0.04 ? "+" : "") + juce::String (ms, std::abs (ms - std::round (ms)) < 0.05 ? 0 : 1) + " ms";
    }
    /** The list, from the project (made again only when the project's list has changed). */
    void refreshConsonantList()
    {
        if (! ctx.consonantList) return;
        auto list = ctx.consonantList();
        juce::String key; for (auto& c : list) key << c.id.toString() << c.name << c.from << ";";
        if (key == consListKey) return;
        consListKey = key; consList = std::move (list);
        consBox.clear (juce::dontSendNotification);
        for (int i = 0; i < (int) consList.size(); ++i) consBox.addItem (consList[(size_t) i].listText(), i + 1);
        int at = -1; for (int i = 0; i < (int) consList.size(); ++i) if (consList[(size_t) i].id == consSel) at = i;
        if (at < 0 && ! consList.empty()) { at = (int) consList.size() - 1; consSel = consList.back().id; }      // (none chosen yet: the newest)
        if (at >= 0) consBox.setSelectedItemIndex (at, juce::dontSendNotification);
        consBox.setTextWhenNothingSelected (consList.empty() ? "None kept yet: draw a box round a consonant and press Copy" : "Choose one");
    }
    /** Copy (and remove): on a job thread, every track's channels round the box are read from the files, this window's fixes so far put on them, and the
        stray-consonant ReTouch's difference kept (consonants::extract). Then it is named (Copy) or named for where it came from (Copy and remove, which then
        takes it out here like the ReTouch). */
    void copyConsonant (bool remove)
    {
        if (! scanned || running || ! view.hasSelection() || ! ctx.readChannels || ! ctx.keepConsonant) return;
        juce::int64 t0 = 0, t1 = 0;
        auto spec = buildSpec (t0, t1, 0);
        spec = consonants::copySpec (spec);
        if (t1 - t0 < (juce::int64) (0.005 * ctx.rate)) { status.setText ("The box is too short to hold a consonant.", juce::dontSendNotification); return; }
        const auto pad = (juce::int64) std::llround ((1.0 + 2.0 * (double) (t1 - t0) / ctx.rate) * ctx.rate);       // (the ReTouch measures the sound round the box)
        const auto a = juce::jmax ((juce::int64) 0, t0 - pad), b = juce::jmin (ctx.length, t1 + pad);
        auto read = ctx.readChannels; const auto ids = ctx.trackIds; const auto names = ctx.trackNames; const double rate = ctx.rate;
        const auto ops = committed; auto picture = origTracks;
        auto out = std::make_shared<ConsonantAudio>();
        status.setText (remove ? "Copying the consonant (every track) and taking it out here..." : "Copying the consonant (every track)...", juce::dontSendNotification);
        juce::Component::SafePointer<RepairDialog> self (this);
        runJob ([read, ids, names, rate, ops, picture, out, spec, t0, t1, a, b] (AudioJob& j) -> juce::String
                {
                    auto tracks = read (a, b, j);
                    if (j.cancelled()) return "Cancelled.";
                    if (tracks.size() != ids.size()) return "The tracks could not all be read.";
                    // this window's fixes so far, where they reach this stretch (a denoise is left out: it needs each file's room tone, and only touches the background)
                    for (auto& o : ops)
                    {
                        if (o.spec.kind == FixSpec::Kind::Denoise || o.t1 <= a || o.t0 >= b) continue;
                        for (size_t i = 0; i < tracks.size(); ++i)
                        {
                            auto s = o.spec; if (s.kind == FixSpec::Kind::Paste) audioops::resolvePaste (s, ids[i]);
                            audioops::applyFix (tracks[i], rate, (long) (o.t0 - a), (long) (o.t1 - a), s, (long) (0.02 * rate));
                        }
                    }
                    // which side of the box is the background: one answer for all the tracks, found on the picture as the ReTouch finds it
                    auto s = spec;
                    if (std::abs (s.repair.weighting - 0.5) < 0.04 && picture != nullptr && ! picture->empty())
                    {
                        const long L = (long) (*picture)[0].size();
                        std::vector<const std::vector<float>*> tp; for (auto& t : *picture) tp.push_back (&t);
                        const auto d = SpectralRepair::consonantSides (tp, rate, (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t0), (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, t1), s.f0, s.f1);
                        s.repair.consonantBefore = d.first; s.repair.consonantEdge = d.second;
                    }
                    *out = consonants::extract (ids, names, tracks, rate, (long) (t0 - a), (long) (t1 - a), s);
                    if (out->tracks.empty()) return "Nothing to copy: the ReTouch found nothing in the box that stands out from the sound beside it. Draw the box round the consonant only, from about 2 kHz up.";
                    return {};
                },
                [self, out, remove, spec, t0, t1] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    const auto from = self->ctx.where + ", " + clockText (self->ctx.areaStart + (double) t0 / self->ctx.rate);
                    if (remove)
                    {
                        juce::Uuid id; const auto e = self->ctx.keepConsonant (*out, "moved", from, id);
                        if (e.isNotEmpty()) { self->status.setText (e, juce::dontSendNotification); self->refreshControls(); return; }
                        self->consSel = id; self->consCache[id.toString()] = out;
                        self->refreshControls();
                        self->nextNote = "Copied (kept as \"moved\") and taken out here. Now click the time ruler where it should start and press Paste here [P]. ";
                        self->startFix (spec, t0, t1);                         // (taken out here as the stray-consonant ReTouch does, at full strength)
                        return;
                    }
                    auto keep = [self, out, from] (const juce::String& name)
                    {
                        if (self == nullptr) return;
                        juce::Uuid id; const auto e = self->ctx.keepConsonant (*out, name, from, id);
                        if (e.isNotEmpty()) { self->status.setText (e, juce::dontSendNotification); self->refreshControls(); return; }
                        self->lastConsName = name; self->consSel = id; self->consCache[id.toString()] = out;
                        self->status.setText ("Kept \"" + name + "\" (" + juce::String ((int) out->tracks.size()) + (out->tracks.size() == 1 ? " track, " : " tracks, ")
                                              + juce::String (juce::roundToInt (1000.0 * (double) out->length() / out->sampleRate)) + " ms) with the project: paste it here, or in any ReTouch window of this project, whenever you need it.",
                                              juce::dontSendNotification);
                        self->refreshControls();
                    };
                    askName ("Keep this consonant", "A name for it (t, s, sh, soft d ...). It is kept with the project, every track and channel as recorded.", self->lastConsName, keep);
                    self->refreshControls();
                });
    }
    static void askName (const juce::String& title, const juce::String& message, const juce::String& suggestion, std::function<void (const juce::String&)> done)
    {
        auto* aw = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("n", suggestion);
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, done] (int res)
        {
            const auto n = aw->getTextEditorContents ("n").trim();
            delete aw;
            if (res == 1 && n.isNotEmpty() && done) done (n);
        }));
    }
    void renamePressed()
    {
        const auto* c = selectedConsonant(); if (c == nullptr || ! ctx.renameConsonant) return;
        const auto id = c->id;
        juce::Component::SafePointer<RepairDialog> self (this);
        askName ("Rename consonant", "A new name for \"" + c->name + "\" (" + c->from + ").", c->name, [self, id] (const juce::String& n)
        {
            if (self == nullptr) return;
            self->ctx.renameConsonant (id, n); self->refreshControls();
        });
    }
    void forgetPressed()
    {
        const auto* c = selectedConsonant(); if (c == nullptr || ! ctx.forgetConsonant) return;
        const auto id = c->id;
        juce::Component::SafePointer<RepairDialog> self (this);
        confirmAsync ("Delete consonant", "Take \"" + c->name + "\" (" + c->from + ") off this project's list and delete its audio? Fixes already made with it stay as they are.", "Delete",
                      [self, id] { if (self == nullptr) return; self->ctx.forgetConsonant (id); self->consCache.erase (id.toString()); self->consSel = juce::Uuid::null(); self->refreshControls(); });
    }
    /** Paste: the chosen consonant, its box's start at the placed playhead, at the level set; a fix like the others. */
    void pasteConsonant()
    {
        if (! scanned || running) return;
        const auto* c = selectedConsonant();
        if (c == nullptr) { status.setText ("Choose a kept consonant in the list first (or copy one: draw a box round it and press Copy consonant).", juce::dontSendNotification); return; }
        const double cue = view.cueTime();
        if (cue < 0.0) { status.setText ("Click the time ruler where the consonant should start (a red playhead appears), then press Paste here [P].", juce::dontSendNotification); return; }
        auto audio = consonantAudio (*c);
        if (audio == nullptr) return;
        if (std::abs (audio->sampleRate - ctx.rate) > 0.5) { status.setText ("\"" + c->name + "\" was copied at " + juce::String (audio->sampleRate / 1000.0, 1) + " kHz and this audio is at " + juce::String (ctx.rate / 1000.0, 1) + " kHz: it cannot be pasted here.", juce::dontSendNotification); return; }
        int here = 0; for (auto& id : ctx.trackIds) if (audio->forTrack (id) != nullptr) ++here;
        if (here == 0)
        {
            status.setText ("None of the tracks \"" + c->name + "\" was copied from (" + juce::StringArray (audio->trackNames.data(), (int) audio->trackNames.size()).joinIntoString (", ")
                            + ") are in this window, so there is nothing to paste on.", juce::dontSendNotification);
            return;
        }
        const auto t0 = (juce::int64) std::llround (cue * ctx.rate) - audio->lead, t1 = t0 + audio->length();
        const auto edge = (juce::int64) std::llround (0.04 * ctx.rate);                   // (clear of the joins at the ends of the part, where the new audio is crossfaded in)
        if (t0 < edge || t1 > ctx.length - edge) { status.setText ("The consonant would run past the part loaded in this window. Place the playhead further in, or mark a wider part (keys 1 and 2) and open the window again.", juce::dontSendNotification); return; }
        FixSpec spec; spec.kind = FixSpec::Kind::Paste; spec.paste = audio; spec.pasteGainDb = level.getValue();
        startFix (spec, t0, t1, t0);
    }
    std::shared_ptr<const ConsonantAudio> consonantAudio (const ConsonantDef& c)
    {
        auto it = consCache.find (c.id.toString());
        if (it != consCache.end()) return it->second;
        if (! ctx.loadConsonant) return nullptr;
        juce::String err;
        auto a = ctx.loadConsonant (c.id, err);
        if (a == nullptr) { status.setText (err.isNotEmpty() ? err : "The consonant's audio could not be read.", juce::dontSendNotification); return nullptr; }
        consCache[c.id.toString()] = a;
        return a;
    }
    /** The consonant just pasted, again: moved by 'ms' (and at the level set now). It is taken off and put back, so Undo still takes it off in one go. */
    void nudge (int ms)
    {
        const auto d = (juce::int64) std::llround ((double) ms * 0.001 * ctx.rate);
        if (running && pasteRunning) { pendingNudge += d; return; }            // (pressed again while the last nudge is still being made: it follows when that is done)
        adjustPaste (d);
    }
    void levelChanged()
    {
        if (running && pasteRunning) { pendingLevel = true; return; }
        if (lastIsPaste() && std::abs (committed.back().spec.pasteGainDb - level.getValue()) >= 0.01) adjustPaste (0);
    }
    /** A paste (or a nudge of one) has been made: nudges and level changes that came in meanwhile follow now, in one go. */
    void followPending()
    {
        const auto d = pendingNudge; const bool lv = pendingLevel;
        pendingNudge = 0; pendingLevel = false;
        if (d != 0 || (lv && lastIsPaste() && std::abs (committed.back().spec.pasteGainDb - level.getValue()) >= 0.01)) adjustPaste (d);
    }
    void adjustPaste (juce::int64 delta)
    {
        if (! scanned || running || ! lastIsPaste()) return;
        auto op = committed.back();
        op.t0 += delta; op.t1 += delta; op.spec.pasteGainDb = level.getValue();
        const auto edge = (juce::int64) std::llround (0.04 * ctx.rate);
        if (op.t0 < edge || op.t1 > ctx.length - edge) { status.setText ("It cannot go further: it would run past the part loaded in this window.", juce::dontSendNotification); return; }
        revert();
        startFix (op.spec, op.t0, op.t1, op.placed);
    }
};

/** The spectrogram settings (the ones iZotope RX has, as far as they apply here). Changes show at once and are remembered. */
class SpectroSettingsDialog : public juce::Component
{
public:
    SpectroSettingsDialog (const SpectroSettings& s, std::function<void (const SpectroSettings&)> apply) : cur (s), onApply (std::move (apply))
    {
        setSize (440, 330);
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (cPal, "Colour map"); cap (cLo, "Lowest level (dB)"); cap (cHi, "Highest level (dB)"); cap (cDet, "Detail"); cap (cRed, "Fewer points above (s)");
        sharpBtn.setButtonText ("Sharp picture for what is in view (high-quality rendering)"); sharpBtn.setToggleState (cur.sharp, juce::dontSendNotification);
        reBtn.setButtonText ("Enable reassignment (crisp lines)"); reBtn.setToggleState (cur.reassign, juce::dontSendNotification);
        pal.addItem ("Cyan to orange", 1); pal.addItem ("Heat (standard)", 2); pal.addItem ("Grey", 3); pal.addItem ("Ice", 4); pal.addItem ("Rainbow", 5);
        pal.setSelectedId (cur.palette + 1, juce::dontSendNotification);
        auto sl = [this] (juce::Slider& x, double a, double b, double st, double v) { x.setRange (a, b, st); x.setValue (v, juce::dontSendNotification); x.setSliderStyle (juce::Slider::LinearHorizontal); x.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22); addAndMakeVisible (x); x.onValueChange = [this] { push(); }; };
        sl (lo, -160.0, -40.0, 0.1, cur.lo); sl (hi, -60.0, 0.0, 0.1, cur.hi); sl (det, 0.0, 1.0, 0.05, cur.detail); sl (red, 20.0, 1200.0, 10.0, cur.reduceAbove);
        det.setTooltip ("Left: finer pitch (long analysis window). Right: finer time (short window).");
        reset.setButtonText ("Back to the RX-style defaults");
        reset.onClick = [this] { SpectroSettings d; cur = d; pal.setSelectedId (1, juce::dontSendNotification); lo.setValue (d.lo, juce::dontSendNotification); hi.setValue (d.hi, juce::dontSendNotification); det.setValue (d.detail, juce::dontSendNotification); red.setValue (d.reduceAbove, juce::dontSendNotification); sharpBtn.setToggleState (true, juce::dontSendNotification); reBtn.setToggleState (true, juce::dontSendNotification); onApply (cur); };
        for (auto* c : std::initializer_list<juce::Component*> { &sharpBtn, &reBtn, &pal, &reset }) addAndMakeVisible (c);
        sharpBtn.onClick = [this] { push(); }; reBtn.onClick = [this] { push(); }; pal.onChange = [this] { push(); };
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto row = [&r] { auto x = r.removeFromTop (30); r.removeFromTop (6); return x; };
        sharpBtn.setBounds (row()); reBtn.setBounds (row());
        auto a = row(); cPal.setBounds (a.removeFromLeft (150)); pal.setBounds (a);
        a = row(); cLo.setBounds (a.removeFromLeft (150)); lo.setBounds (a);
        a = row(); cHi.setBounds (a.removeFromLeft (150)); hi.setBounds (a);
        a = row(); cDet.setBounds (a.removeFromLeft (150)); det.setBounds (a);
        a = row(); cRed.setBounds (a.removeFromLeft (150)); red.setBounds (a);
        reset.setBounds (r.removeFromTop (30).removeFromLeft (230));
    }
private:
    void push()
    {
        cur.sharp = sharpBtn.getToggleState(); cur.reassign = reBtn.getToggleState(); cur.palette = juce::jmax (0, pal.getSelectedId() - 1);
        cur.lo = lo.getValue(); cur.hi = hi.getValue(); if (cur.hi - cur.lo < 6.0) cur.hi = cur.lo + 6.0;
        cur.detail = det.getValue(); cur.reduceAbove = red.getValue();
        onApply (cur);
    }
    SpectroSettings cur; std::function<void (const SpectroSettings&)> onApply;
    juce::Label cPal, cLo, cHi, cDet, cRed; juce::ComboBox pal; juce::Slider lo, hi, det, red;
    juce::ToggleButton sharpBtn, reBtn; juce::TextButton reset;
};

void RepairDialog::openSpectroSettings()
{
    juce::Component::SafePointer<RepairDialog> self (this);
    launchDialog (std::make_unique<SpectroSettingsDialog> (view.settings(), [self] (const SpectroSettings& n)
    {
        if (self == nullptr) return;
        self->view.setSettings (n); n.save (gSpecPrefs);
        if (gSpecPrefs != nullptr) gSpecPrefs->saveIfNeeded();
    }), "Spectrogram settings", this);
}

// ======================================================================================================== the order of the imported tracks
class TrackOrderDialog : public juce::Component, private juce::ListBoxModel
{
public:
    TrackOrderDialog (std::vector<std::pair<juce::String, juce::String>> keysAndNames, std::function<void (const std::vector<juce::String>&)> onOk)
        : items (std::move (keysAndNames)), done (std::move (onOk))
    {
        setSize (380, 520);
        list.setModel (this); list.setRowHeight (24); addAndMakeVisible (list);
        intro.setText ("Put the tracks in the order you want them, top to bottom (in the take windows, edit windows and mixers). Select a track and use Move up / Move down.", juce::dontSendNotification);
        intro.setJustificationType (juce::Justification::topLeft); intro.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (intro);
        for (auto* b : { &up, &down, &ok, &cancel }) addAndMakeVisible (b);
        ok.setColour (juce::TextButton::buttonColourId, theme::accent);
        up.onClick = [this] { move (-1); }; down.onClick = [this] { move (1); };
        ok.onClick = [this] { std::vector<juce::String> k; for (auto& i : items) k.push_back (i.first); if (done) done (k); closeDialogOf (this); };
        cancel.onClick = [this] { closeDialogOf (this); };
        list.selectRow (0);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        intro.setBounds (r.removeFromTop (54)); r.removeFromTop (4);
        auto bottom = r.removeFromBottom (30); r.removeFromBottom (6);
        cancel.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (8); ok.setBounds (bottom.removeFromRight (90));
        auto mv = r.removeFromBottom (28); up.setBounds (mv.removeFromLeft (120)); mv.removeFromLeft (8); down.setBounds (mv.removeFromLeft (120)); r.removeFromBottom (6);
        list.setBounds (r);
    }
private:
    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row < 0 || row >= (int) items.size()) return;
        if (sel) g.fillAll (theme::accent.withAlpha (0.35f));
        g.setColour (theme::text); g.setFont (juce::FontOptions (14.0f));
        g.drawText (juce::String (row + 1) + "   " + items[(size_t) row].second, 8, 0, w - 12, h, juce::Justification::centredLeft);
    }
    void move (int d)
    {
        const int r = list.getSelectedRow(), t = r + d;
        if (r < 0 || t < 0 || t >= (int) items.size()) return;
        std::swap (items[(size_t) r], items[(size_t) t]);
        list.updateContent(); list.selectRow (t); list.repaint();
    }
    std::vector<std::pair<juce::String, juce::String>> items; std::function<void (const std::vector<juce::String>&)> done;
    juce::ListBox list; juce::Label intro; juce::TextButton up { "Move up" }, down { "Move down" }, ok { "OK" }, cancel { "Cancel" };
};

// ======================================================================================================== the import dialog
class ImportDialog : public JobDialogBase
{
public:
    ImportDialog (AppContext& a, const juce::Uuid& wid, const juce::Uuid& eid, std::vector<ImportSource> srcs, juce::StringArray problemNotes)
        : sources (std::move (srcs)), problems (std::move (problemNotes)), app (a), windowId (wid), editId (eid)
    {
        setSize (680, 680);
        for (auto* x : std::initializer_list<juce::Component*> { &summary, &doIt, &cancel }) addAndMakeVisible (x);
        summary.setMultiLine (true); summary.setReadOnly (true); summary.setCaretVisible (false); summary.setScrollbarsShown (true);
        summary.setColour (juce::TextEditor::backgroundColourId, theme::field); summary.setFont (juce::FontOptions (13.0f));
        if (auto* st = app.props.getUserSettings())
        {
            juce::StringArray ks; ks.addTokens (st->getValue ("importTrackOrder", {}), "|", "");
            for (auto& k : ks) if (k.isNotEmpty()) trackOrder.push_back (k);
        }
        // how the file names are read: Automatic, or the program that recorded them (when Automatic gets it wrong)
        namingCap.setText ("Recorded with:", juce::dontSendNotification); namingCap.setColour (juce::Label::textColourId, theme::text);
        addAndMakeVisible (namingCap);
        namingBox.addItem ("Automatic", 1); namingBox.addItem ("Pyramix  (piece 001_mic-L / -R / -C)", 2); namingBox.addItem ("Pro Tools  (piece track 01, .L / .R)", 3);
        namingBox.addItem ("SADiE  (piece-Mic01)", 4); namingBox.addItem ("Other: by file length (every file of a take is the same length)", 5);
        namingBox.setSelectedId (1, juce::dontSendNotification);
        namingBox.setTooltip ("How Fermata reads the file names to find the takes and the tracks. Automatic tries every way it knows and says which it used. "
                              "If the takes or tracks come out wrong, choose the program that recorded the files: the files are read again that way at once.");
        namingBox.onChange = [this] { setBase(); };
        addAndMakeVisible (namingBox);
        pieceBox.onClick = [this] { refreshSummary(); };
        addAndMakeVisible (orderButton);
        orderButton.setTooltip ("Choose the order of the tracks (top to bottom). Fermata remembers it for the next import.");
        orderButton.onClick = [this] { openOrder(); };
        joinBox.setButtonText ("Join left / right pairs into stereo tracks (one stereo strip and one stereo track each). Tick the ones you want stereo, untick the ones to keep as mono tracks:");
        joinBox.onClick = [this] { for (auto* b : pairBoxes) b->setToggleState (joinBox.getToggleState(), juce::dontSendNotification); rebuildPlan(); };
        pairView.setViewedComponent (&pairHolder, false); pairView.setScrollBarsShown (true, false);
        numBox.setButtonText ("Also join names ending in 1 and 2 (Mic 1 + Mic 2) into stereo tracks");
        numBox.onClick = [this] { buildPairList(); rebuildPlan(); };
        setBase();
        // the files are one set of tracks: each goes on the track named in its file name, whatever else the name says (numbers, the piece)
        addAndMakeVisible (byNameBox);
        byNameBox.setButtonText ("One set of tracks: put each file on the track named in its file name (AB, SPOT1 ...) - numbers and the rest of the name are ignored");
        byNameBox.setTooltip ("For files whose names carry the piece and the take numbers (\"Rorate Caeli SPOT4 19 18\"), which would otherwise each make new tracks. "
                              "A file goes on the project's track whose name its name ends with, once the numbers at the end are taken off; a file with no such "
                              "track gets a track of its own (named without the numbers, shared by the takes). The takes are still told apart by their lengths.");
        if (auto* st = app.props.getUserSettings()) byNameBox.setToggleState (st->getBoolValue ("importByTrackName", false), juce::dontSendNotification);
        byNameBox.onClick = [this] { if (auto* st = app.props.getUserSettings()) st->setValue ("importByTrackName", byNameBox.getToggleState()); refreshSummary(); };
        addAndMakeVisible (copyBox); addAndMakeVisible (linkBox);
        copyBox.setButtonText ("Copy the audio into this project's folder (recommended)"); linkBox.setButtonText ("Use the audio files where they are (nothing is copied)");
        copyBox.setRadioGroupId (4711); linkBox.setRadioGroupId (4711);
        copyBox.setToggleState (true, juce::dontSendNotification);
        copyBox.onClick = [this] { refreshSummary(); }; linkBox.onClick = [this] { refreshSummary(); };
        doIt.setColour (juce::TextButton::buttonColourId, theme::accent);
        doIt.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
    }
    /** (Re)reads the files the way the "Recorded with" box says and sets up everything that depends on it. */
    void setBase()
    {
        static const ImportNaming ways[] = { ImportNaming::Auto, ImportNaming::Pyramix, ImportNaming::ProTools, ImportNaming::Sadie, ImportNaming::ByLength };
        basePlan = buildImportPlan (sources, ways[juce::jlimit (0, 4, namingBox.getSelectedId() - 1)]);
        for (auto& pr : problems) basePlan.notes.add (pr);
        plan = basePlan;
        std::set<juce::String> pieceNames; for (auto& t : basePlan.takes) if (t.piece.isNotEmpty()) pieceNames.insert (t.piece);
        const bool pieces = editId.isNull() && pieceNames.size() > 1;
        pieceBox.setVisible (pieces);
        if (pieces)
        {
            pieceBox.setButtonText ("The file names name " + juce::String ((int) pieceNames.size()) + " different pieces: put each piece in its own take window (named after the piece)");
            pieceBox.setToggleState (true, juce::dontSendNotification);
        }
        pairBoxes.clear(); pairCands.clear();
        const bool pairs = hasStereoPairs (basePlan);
        joinBox.setVisible (pairs); pairView.setVisible (pairs);
        numBox.setVisible (pairs && joinStereoPairs (basePlan, true).tracks.size() < joinStereoPairs (basePlan, false).tracks.size());    // there are names ending 1 and 2 as well
        if (pairs)
        {
            joinBox.setToggleState (true, juce::dontSendNotification); numBox.setToggleState (false, juce::dontSendNotification);
            buildPairList();
            rebuildPlan();
        }
        else refreshSummary();
        resized();
    }
    bool numbersOn() const { return numBox.isVisible() && numBox.getToggleState(); }
    /** One tick box per left / right pair that could be a stereo track. What was kept mono last time stays unticked. */
    void buildPairList()
    {
        std::set<juce::String> keepMono;
        if (pairBoxes.isEmpty())
        {
            if (auto* st = app.props.getUserSettings())
            {
                juce::StringArray ks; ks.addTokens (st->getValue ("importMonoPairs", {}), "|", "");
                for (auto& k : ks) if (k.isNotEmpty()) keepMono.insert (k);
            }
        }
        else for (int i = 0; i < pairBoxes.size(); ++i) if (! pairBoxes[i]->getToggleState()) keepMono.insert (pairCands[(size_t) i].key);
        pairBoxes.clear(); pairCands = stereoPairCandidates (basePlan, numbersOn());
        for (auto& c : pairCands)
        {
            auto* b = pairBoxes.add (new juce::ToggleButton());
            b->setButtonText (c.name + "     (" + c.leftName + " + " + c.rightName + ")  =  one stereo track");
            b->setToggleState (keepMono.count (c.key) == 0, juce::dontSendNotification);
            b->onClick = [this] { rebuildPlan(); };
            pairHolder.addAndMakeVisible (b);
        }
        pairHolder.setSize (juce::jmax (100, pairView.getWidth() - 20), juce::jmax (1, pairBoxes.size()) * 22);
        for (int i = 0; i < pairBoxes.size(); ++i) pairBoxes[i]->setBounds (0, i * 22, pairHolder.getWidth(), 22);
        resized();
    }
    void rebuildPlan()
    {
        if (pairBoxes.isEmpty() && pairCands.empty() && ! joinBox.isVisible()) { plan = basePlan; refreshSummary(); return; }
        std::set<juce::String> only; int ticked = 0;
        for (int i = 0; i < pairBoxes.size(); ++i) if (pairBoxes[i]->getToggleState()) { only.insert (pairCands[(size_t) i].key); ++ticked; }
        plan = ticked > 0 ? joinStereoPairs (basePlan, numbersOn(), &only) : basePlan;
        joinBox.setToggleState (ticked == pairBoxes.size() && ticked > 0, juce::dontSendNotification);
        refreshSummary();
    }
    bool linking() const { return linkBox.getToggleState(); }
    bool byName() const { return byNameBox.getToggleState(); }
    bool splitting() const { return pieceBox.isVisible() && pieceBox.getToggleState(); }
    /** The tracks in the order the person chose (anything not in that list keeps its place after the others). */
    void applyOrder (ImportPlan& pl) const
    {
        if (trackOrder.empty()) return;
        auto rank = [&] (const juce::String& k) { for (size_t i = 0; i < trackOrder.size(); ++i) if (trackOrder[i] == k) return (int) i; return 1000000; };
        std::stable_sort (pl.tracks.begin(), pl.tracks.end(), [&] (const ImportTrack& x, const ImportTrack& y) { return rank (x.key) < rank (y.key); });
    }
    void openOrder()
    {
        applyOrder (plan);
        std::vector<std::pair<juce::String, juce::String>> items; for (auto& t : plan.tracks) items.push_back ({ t.key, t.name });
        juce::Component::SafePointer<ImportDialog> self (this);
        launchDialog (std::make_unique<TrackOrderDialog> (std::move (items), [self] (const std::vector<juce::String>& keys)
        {
            if (self == nullptr) return;
            self->trackOrder = keys;
            self->refreshSummary();
        }), "Track order", this);
    }
    void refreshSummary()
    {
        applyOrder (plan);
        juce::String t;
        t << juce::String ((int) plan.takes.size()) << " take(s), " << juce::String ((int) plan.tracks.size()) << " track(s)."
          << (plan.readAs.isNotEmpty() ? "   (The names were read as " + plan.readAs + (namingBox.getSelectedId() == 1 ? " - automatically; if this is wrong, choose the program in \"Recorded with\" below" : juce::String()) + ".)" : juce::String())
          << "\n\n";
        juce::String lastPiece; int n = 0;
        for (size_t i = 0; i < plan.takes.size(); ++i)
        {
            const auto& tk = plan.takes[i];
            if (splitting() && tk.piece != lastPiece) { lastPiece = tk.piece; n = 0; t << (i > 0 ? "\n" : "") << "Take window  \"" << tk.piece << "\":\n"; }
            ++n;
            t << "Take " << juce::String (splitting() ? n : (int) i + 1) << (tk.number > 0 ? "  (number " + juce::String (tk.number) + " in the file names)" : juce::String())
              << ":  " << juce::String ((int) tk.parts.size()) << " track(s),  " << formatTimeShort ((double) tk.length / juce::jmax (1.0, tk.sampleRate), 2)
              << ",  " << juce::String (tk.sampleRate, 0) << " Hz\n";
        }
        if (byName())
        {
            // what the tick box does: each file's track, by the name in the file name
            t << "\nTracks (each file on the track named in its file name; new ones are added):\n";
            std::map<juce::String, juce::StringArray> onto; std::vector<juce::String> order;
            for (auto& tr : plan.tracks)
            {
                const auto* m = app.project.trackNamedIn (tr.name, tr.channels);
                const auto target = m != nullptr ? m->name : Project::withoutEndNumbers (tr.name) + "  (new track)";
                if (! onto.count (target)) order.push_back (target);
                onto[target].add (tr.name);
            }
            for (auto& o : order)
                t << "   " << o << "   <   " << onto[o][0] << (onto[o].size() > 1 ? " and " + juce::String (onto[o].size() - 1) + " more" : juce::String()) << "\n";
        }
        else
        {
            t << "\nTracks, in this order (matched to the tracks you already have by name; new ones are added):\n";
            for (auto& tr : plan.tracks) t << "   " << tr.name << (tr.channels == 2 ? "  (stereo)" : "") << "\n";
        }
        if (plan.notes.size() > 0) { t << "\nNotes:\n"; for (auto& note : plan.notes) t << "   " << note << "\n"; }
        if (! editId.isNull()) t << "\nEach take is placed in the edit, one after the other, with every track on its own row of the edit window (ready for mixing).\n";
        if (linking())
            t << "\nThe files are used where they are, exactly as they are (nothing is copied; if you move or delete them the project cannot find the audio). Left / right pairs joined into a stereo track and the channels of multichannel files still have to be written as new files in this project's folder.";
        else
            t << "\nThe audio is copied into this project's folder as 24-bit WAV files; your original files are not touched.";
        summary.setText (t, false);
        doIt.setButtonText (editId.isNull() ? "Import " + juce::String ((int) plan.takes.size()) + " take(s)" : "Bring into the edit");
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bottom = r.removeFromBottom (30);
        orderButton.setBounds (bottom.removeFromLeft (150));
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); doIt.setBounds (bottom.removeFromRight (180));
        linkBox.setBounds (r.removeFromBottom (24)); copyBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        byNameBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        { auto row = r.removeFromBottom (26); namingCap.setBounds (row.removeFromLeft (110)); namingBox.setBounds (row.removeFromLeft (juce::jmin (440, row.getWidth())).reduced (0, 1)); r.removeFromBottom (4); }
        if (pieceBox.isVisible()) pieceBox.setBounds (r.removeFromBottom (24));
        if (numBox.isVisible()) numBox.setBounds (r.removeFromBottom (24));
        if (joinBox.isVisible())
        {
            pairView.setBounds (r.removeFromBottom (juce::jmin (130, juce::jmax (24, pairBoxes.size() * 22 + 4))));
            pairHolder.setSize (juce::jmax (100, pairView.getWidth() - 20), juce::jmax (1, pairBoxes.size()) * 22);
            for (int i = 0; i < pairBoxes.size(); ++i) pairBoxes[i]->setBounds (0, i * 22, pairHolder.getWidth(), 22);
            joinBox.setBounds (r.removeFromBottom (24)); r.removeFromBottom (4);
        }
        r.removeFromBottom (6); bar.setBounds (r.removeFromBottom (22)); status.setBounds (r.removeFromBottom (22));
        summary.setBounds (r);
    }
private:
    std::vector<ImportSource> sources; juce::StringArray problems;     // the files as read (header information), and what could not be read
    ImportPlan basePlan; std::vector<juce::String> trackOrder;
    juce::Label namingCap; juce::ComboBox namingBox;
    juce::ToggleButton joinBox, numBox, copyBox, linkBox, pieceBox, byNameBox; juce::TextButton orderButton { "Track order..." };
    std::vector<StereoPairCandidate> pairCands; juce::OwnedArray<juce::ToggleButton> pairBoxes; juce::Component pairHolder; juce::Viewport pairView;
    void start()
    {
        auto* w = app.project.findTakeWindow (windowId);
        auto* ed = app.project.findEdit (editId);
        if (editId.isNull() ? w == nullptr : ed == nullptr) return;
        if (ed != nullptr && ! ed->isEmpty() && ! plan.takes.empty() && std::abs (ed->sampleRate - plan.takes.front().sampleRate) > 0.5)
        {
            status.setText ("This audio is " + juce::String (plan.takes.front().sampleRate, 0) + " Hz but the edit is " + juce::String (ed->sampleRate, 0) + " Hz.", juce::dontSendNotification);
            return;
        }
        if (! pairBoxes.isEmpty())
            if (auto* st = app.props.getUserSettings())
            {
                juce::StringArray keep; for (int i = 0; i < pairBoxes.size(); ++i) if (! pairBoxes[i]->getToggleState()) keep.add (pairCands[(size_t) i].key);
                st->setValue ("importMonoPairs", keep.joinIntoString ("|"));
            }
        doIt.setEnabled (false); status.setText (linking() ? "Reading the files..." : "Copying the audio...", juce::dontSendNotification);
        const bool link = linking(), named = byName();
        applyOrder (plan);
        const bool reorder = ! trackOrder.empty();
        if (reorder) if (auto* st = app.props.getUserSettings()) { juce::StringArray ks; for (auto& k : trackOrder) ks.add (k); st->setValue ("importTrackOrder", ks.joinIntoString ("|")); }
        if (editId.isNull() && splitting())
        {
            // one take window per piece (an existing window of the same name is used), each numbering its own takes
            struct Part { juce::Uuid wid; ImportPlan plan; juce::File folder; juce::String label; std::vector<int> numbers; std::vector<std::vector<juce::File>> files; bool created = false; };
            auto parts = std::make_shared<std::vector<Part>>();
            std::vector<juce::String> pieces;
            for (auto& t : plan.takes) if (std::find (pieces.begin(), pieces.end(), t.piece) == pieces.end()) pieces.push_back (t.piece);
            for (auto& pc : pieces)
            {
                Part part; part.plan = plan; part.plan.takes.clear(); part.plan.tracks.clear();
                std::set<juce::String> keys;
                for (auto& t : plan.takes) if (t.piece == pc) { part.plan.takes.push_back (t); for (auto& pt : t.parts) keys.insert (pt.trackKey); }
                for (auto& tr : plan.tracks) if (keys.count (tr.key)) part.plan.tracks.push_back (tr);
                TakeWindowDef* tw = nullptr;
                for (auto& x : app.project.takeWindows) if (x->name.equalsIgnoreCase (pc.isEmpty() ? w->name : pc)) { tw = x.get(); break; }
                if (tw == nullptr) { tw = &app.project.addTakeWindow (pc.isEmpty() ? w->name : pc); part.created = true; }
                part.wid = tw->id; part.folder = app.project.takeFolder (*tw); part.label = tw->labelFor (TakeGroup());
                int next = tw->nextNumber;
                for (size_t i = 0; i < part.plan.takes.size(); ++i) part.numbers.push_back (next++);
                parts->push_back (std::move (part));
            }
            juce::Component::SafePointer<ImportDialog> safe (this); auto& appRef = app;
            runJob ([parts, link] (AudioJob& j) -> juce::String
                    {
                        for (size_t k = 0; k < parts->size(); ++k)
                        {
                            auto& pt = (*parts)[k];
                            const auto err = copyImportFiles (formats(), pt.plan, pt.folder, pt.label, pt.numbers, pt.files, &j, link);
                            if (err.isEmpty()) continue;
                            // The pieces copied before this one used to be left on the disk, in no take (and a second try copied them again under "(2)"
                            // names). Their new files are taken away again here: copies made a moment ago, never put in a take - the originals are not
                            // touched, and a file that is an original (linked, not copied) is left alone.
                            for (size_t m = 0; m < k; ++m)
                            {
                                auto& done = (*parts)[m];
                                auto isOriginal = [&] (const juce::File& f) { for (auto& src : done.plan.sources) if (src.file == f) return true; return false; };
                                for (auto& take : done.files) for (auto& f : take) if (f != juce::File() && ! isOriginal (f)) f.deleteFile();
                                done.files.clear();
                            }
                            return err;
                        }
                        return {};
                    },
                    [safe, parts, &appRef, reorder, named] (const juce::String& err)
                    {
                        if (err.isNotEmpty())                 // the take windows made for this import, still empty: taken away again (a window of the project's own, used by name, stays)
                            for (auto& pt : *parts)
                                if (pt.created) if (auto* tw = appRef.project.findTakeWindow (pt.wid); tw != nullptr && tw->groups.empty()) appRef.deleteWindowAndFile (false, pt.wid);
                        if (safe == nullptr) return;
                        if (err.isNotEmpty()) { safe->status.setText (err + "  Nothing was added; the files copied so far were taken away again.", juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                        busy::begin ("Adding the takes", "Putting the imported takes into their take windows");
                        for (auto& pt : *parts) finishImport (appRef, pt.wid, pt.plan, pt.files, reorder, named);
                        busy::endAfterPendingMessages();
                        closeDialogOf (safe.getComponent());
                    });
            return;
        }
        std::vector<int> numbers; for (size_t i = 0; i < plan.takes.size(); ++i) numbers.push_back ((w != nullptr ? w->nextNumber : 1) + (int) i);
        const auto folder = w != nullptr ? app.project.takeFolder (*w) : app.project.audioFolder().getChildFile ("Imported into " + sanitiseForFile (ed->name));
        const auto label = w != nullptr ? w->labelFor (TakeGroup()) : juce::String ("Imported");
        auto result = std::make_shared<std::vector<std::vector<juce::File>>>();
        auto planCopy = std::make_shared<ImportPlan> (plan);
        auto* self = this; auto& appRef = app; const auto wid = windowId; const auto eid = editId;
        juce::Component::SafePointer<ImportDialog> safe (this);
        runJob ([planCopy, folder, label, numbers, result, link] (AudioJob& j) { return copyImportFiles (formats(), *planCopy, folder, label, numbers, *result, &j, link); },
                [safe, planCopy, result, &appRef, wid, eid, self, reorder, named] (const juce::String& err)
                {
                    juce::ignoreUnused (self);
                    if (safe == nullptr) return;
                    if (err.isNotEmpty()) { safe->status.setText (err, juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                    busy::begin ("Adding the takes", eid.isNull() ? "Putting the imported takes into the take window" : "Putting the imported audio into the edit");
                    if (eid.isNull()) finishImport (appRef, wid, *planCopy, *result, reorder, named); else finishImportToEdit (appRef, eid, *planCopy, *result, reorder, named);
                    busy::endAfterPendingMessages();
                    closeDialogOf (safe.getComponent());
                });
    }

    /** The imported tracks take the places they already have in the project's list, in the order of plan.tracks (other tracks stay where they are). */
    static void orderProjectTracks (AppContext& app, const ImportPlan& plan, const std::map<juce::String, juce::Uuid>& trackFor)
    {
        auto& p = app.project;
        std::vector<juce::Uuid> ids;
        for (auto& tr : plan.tracks) { auto it = trackFor.find (tr.key); if (it != trackFor.end() && std::find (ids.begin(), ids.end(), it->second) == ids.end()) ids.push_back (it->second); }
        std::vector<size_t> slots;
        for (size_t i = 0; i < p.tracks.size(); ++i) if (std::find (ids.begin(), ids.end(), p.tracks[i].id) != ids.end()) slots.push_back (i);
        if (slots.size() != ids.size() || ids.size() < 2) return;
        std::vector<TrackDef> picked;
        for (auto& id : ids) for (auto& t : p.tracks) if (t.id == id) { picked.push_back (t); break; }
        if (picked.size() != ids.size()) return;
        for (size_t k = 0; k < slots.size(); ++k) p.tracks[slots[k]] = picked[k];
    }

    /** Finds (by name and channel count) or creates the project track of each import track. 'byName' (the "one set of tracks" box): a name that is
        not a track's goes on the track named in it (Project::trackNamedIn: "Rorate Caeli SPOT4 19 18" -> SPOT4), else on a new track named without
        the numbers at its end, shared by every import track with that name; and two files of one take never go on the same track. */
    static std::map<juce::String, juce::Uuid> mapTracks (AppContext& app, const ImportPlan& plan, TakeWindowDef* w, bool byName = false)
    {
        auto& p = app.project;
        std::map<juce::String, juce::Uuid> trackFor;
        std::set<int> usedInputs; for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) usedInputs.insert (t.inputOf (c));
        auto makeTrack = [&] (const juce::String& trackName, int channels) -> juce::Uuid
        {
            int first = -1;
            for (int i = 0; i + channels <= (int) p.inputs.size() && first < 0; ++i)
            {
                bool freeSlot = true; for (int c = 0; c < channels; ++c) freeSlot = freeSlot && ! usedInputs.count (i + c);
                if (freeSlot) first = i;
            }
            auto& nt = p.addTrack (trackName, channels == 2 ? TrackFormat::Stereo : TrackFormat::Mono, first);
            if (first < 0) nt.inputs.fill (-1); else for (int c = 0; c < channels; ++c) usedInputs.insert (first + c);
            return nt.id;
        };
        std::map<juce::String, juce::Uuid> made;                              // byName: the new track of each name without its numbers (+ channels)
        for (auto& tr : plan.tracks)
        {
            const TrackDef* found = nullptr;
            for (auto& t : p.tracks) if (t.name.equalsIgnoreCase (tr.name) && t.channelCount() == tr.channels) { found = &t; break; }
            if (found == nullptr && byName) found = p.trackNamedIn (tr.name, tr.channels);
            juce::Uuid id;
            if (found != nullptr) id = found->id;
            else if (byName)
            {
                const auto base = Project::withoutEndNumbers (tr.name);
                const auto k = base.toLowerCase() + "#" + juce::String (tr.channels);
                if (auto it = made.find (k); it != made.end()) id = it->second;
                else id = made[k] = makeTrack (base.isNotEmpty() ? base : tr.name, tr.channels);
            }
            else id = makeTrack (tr.name, tr.channels);
            trackFor[tr.key] = id;
            if (w != nullptr) w->unhide (id);
        }
        if (byName)                                                         // a second file of a take on the same track gets a track of its own
            for (auto& tk : plan.takes)
            {
                std::set<juce::Uuid> used;
                for (auto& pt : tk.parts)
                {
                    if (used.insert (trackFor[pt.trackKey]).second) continue;
                    const auto id = makeTrack (pt.trackName, pt.trackChannels);
                    trackFor[pt.trackKey] = id; used.insert (id);
                    if (w != nullptr) w->unhide (id);
                }
            }
        return trackFor;
    }

    /** The window the audio was being imported into was deleted while the files were copied (it used to end without a word): the copies stay where they are, and it is said. */
    static void importTargetGone (const char* what, const std::vector<std::vector<juce::File>>& files)
    {
        juce::File folder;
        for (auto& take : files) for (auto& f : take) if (f != juce::File()) { folder = f.getParentDirectory(); break; }
        showError ("Import", "The " + juce::String (what) + " the audio was being imported into has gone meanwhile, so nothing was added."
                             + (folder != juce::File() ? "\n\nThe files copied for it are still in " + folder.getFullPathName() + " (nothing was deleted); import them again into another window if you want them." : juce::String()));
    }
    /** Edit window import: one piece of the edit per imported take (placed one after the other), each file on its own track. */
    static void finishImportToEdit (AppContext& app, const juce::Uuid& eid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files, bool reorder = false, bool byName = false)
    {
        auto* e = app.project.findEdit (eid);
        if (e == nullptr) { importTargetGone ("edit", files); return; }
        auto trackFor = mapTracks (app, plan, nullptr, byName);
        if (reorder) orderProjectTracks (app, plan, trackFor);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            EditRegion r;
            r.id = juce::Uuid();
            r.takeName = files[ti].empty() ? juce::String ("Imported") : files[ti].front().getFileNameWithoutExtension();
            r.sampleRate = tk.sampleRate; r.sourceLength = tk.length; r.srcIn = 0; r.srcOut = tk.length;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                RegionFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                if (auto* tr = app.project.findTrack (f.trackId)) f.trackName = tr->name;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                r.files.push_back (f);
            }
            if (r.files.empty() || r.length() <= 0) continue;
            if (! e->trackIds.empty())
                for (auto& f : r.files) if (std::find (e->trackIds.begin(), e->trackIds.end(), f.trackId) == e->trackIds.end()) e->trackIds.push_back (f.trackId);
            e->insertIndex = -1;
            e->insertRegion (r);
        }
        app.project.structureChanged();
        app.project.changed();
    }

    static void finishImport (AppContext& app, const juce::Uuid& wid, const ImportPlan& plan, const std::vector<std::vector<juce::File>>& files, bool reorder = false, bool byName = false)
    {
        auto* w = app.project.findTakeWindow (wid);
        if (w == nullptr) { importTargetGone ("take window", files); return; }
        auto& p = app.project;
        auto trackFor = mapTracks (app, plan, w, byName);
        if (reorder) orderProjectTracks (app, plan, trackFor);
        for (size_t ti = 0; ti < plan.takes.size() && ti < files.size(); ++ti)
        {
            const auto& tk = plan.takes[ti];
            TakeGroup g; g.id = juce::Uuid(); g.number = w->nextNumber++;
            g.startSeconds = w->groups.empty() ? 0.0 : std::ceil (w->endSeconds() + 2.0);
            g.lengthSamples = tk.length; g.sampleRate = tk.sampleRate; g.recordedAt = tk.modified;
            for (size_t pi = 0; pi < tk.parts.size() && pi < files[ti].size(); ++pi)
            {
                TakeFile f; f.trackId = trackFor[tk.parts[pi].trackKey]; f.trackName = tk.parts[pi].trackName;
                f.file = files[ti][pi]; f.numChannels = tk.parts[pi].trackChannels;
                if (tk.parts[pi].source >= 0 && tk.parts[pi].source < (int) plan.sources.size()) f.sourceName = plan.sources[(size_t) tk.parts[pi].source].file.getFileName();
                if (tk.parts[pi].source2 >= 0 && tk.parts[pi].source2 < (int) plan.sources.size()) f.sourceName2 = plan.sources[(size_t) tk.parts[pi].source2].file.getFileName();
                if (auto* tr = p.findTrack (f.trackId)) f.trackName = tr->name;
                g.files.push_back (f);
            }
            w->groups.push_back (std::move (g));
        }
        p.structureChanged();
    }

    AppContext& app; juce::Uuid windowId, editId; ImportPlan plan;
    juce::TextEditor summary; juce::TextButton doIt { "Import" }, cancel { "Cancel" };
};

static void openImportDialog (AppContext& app, const juce::Uuid& wid, const juce::Uuid& eid, const juce::Array<juce::File>& chosen, juce::Component* parent)
{
    juce::Array<juce::File> files;
    for (auto& f : chosen)
    {
        if (f.isDirectory()) { for (auto& c : f.findChildFiles (juce::File::findFiles, false)) if (isAudioFile (c)) files.add (c); }
        else if (isAudioFile (f)) files.add (f);
    }
    if (files.isEmpty()) { say ("Import takes", "No audio files were found. Fermata imports WAV (including polyphonic / multichannel WAV), AIFF and FLAC files."); return; }
    // Every file is opened to read its length, rate and channels. With hundreds of files (or a slow or network drive) that takes a while, so it is
    // done on another thread, with the "please wait" window up saying how far it has got (and a Cancel button). The import window follows.
    struct Shared
    {
        juce::Array<juce::File> files;
        std::atomic<int> done { 0 };
        std::atomic<bool> cancel { false };
        std::vector<ImportSource> sources;
        juce::StringArray problems;
    };
    struct Ticker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } };
    auto sh = std::make_shared<Shared>();
    sh->files = files;
    const int total = files.size();
    busy::begin ("Reading the audio files", juce::String (total) + " files");
    busy::setCancel ([sh] { sh->cancel = true; });
    auto* ticker = new Ticker();                                           // (made and deleted on the message thread only)
    ticker->fn = [sh, total] { busy::step ("File " + juce::String (sh->done.load()) + " of " + juce::String (total), (double) sh->done.load() / (double) juce::jmax (1, total)); };
    ticker->startTimer (100);
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    auto* appPtr = &app;
    const bool launched = juce::Thread::launch ([sh, ticker, appPtr, wid, eid, safeParent]
    {
        juce::AudioFormatManager fm; td::registerAudioFormats (fm);            // (its own, so nothing is shared with the message thread)
        sh->sources = inspectFiles (fm, sh->files, sh->problems, [sh] (int d, int) { sh->done = d; return ! sh->cancel.load(); });
        juce::MessageManager::callAsync ([sh, ticker, appPtr, wid, eid, safeParent]
        {
            delete ticker;
            busy::setCancel (nullptr);
            busy::end();
            if (sh->cancel.load()) return;
            if (sh->sources.empty()) { say ("Import takes", "None of those files could be read.\n\n" + sh->problems.joinIntoString ("\n")); return; }
            launchDialog (std::make_unique<ImportDialog> (*appPtr, wid, eid, std::move (sh->sources), sh->problems), eid.isNull() ? "Import takes" : "Import into the edit", safeParent.getComponent());
        });
    });
    if (! launched) { delete ticker; busy::setCancel (nullptr); busy::end(); say ("Import takes", "Could not start reading the files. Please try again."); }
}

void importTakes (AppContext& app, const juce::Uuid& wid, const juce::Array<juce::File>& files, juce::Component* parent, const juce::Uuid& intoEdit)
{
    if (heldByRec (app, intoEdit.isNull() ? "Importing takes" : "Importing into the edit", parent,
                   [ap = &app, wid, files, intoEdit] (juce::Component* p) { importTakes (*ap, wid, files, p, intoEdit); })) return;
    if (! files.isEmpty()) { openImportDialog (app, wid, intoEdit, files, parent); return; }
    auto chooser = std::make_shared<juce::FileChooser> ("Choose the audio files of the session (select them all)", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.flac");
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems | juce::FileBrowserComponent::canSelectFiles,
                          [chooser, &app, wid, intoEdit, safeParent] (const juce::FileChooser& fc)
                          { if (fc.getResults().size() > 0) openImportDialog (app, wid, intoEdit, fc.getResults(), safeParent.getComponent()); });
}

void showImportMenu (AppContext& app, const juce::Uuid& wid, juce::Component* source, const juce::Uuid& intoEdit)
{
    juce::PopupMenu m;
    m.addItem (1, "Choose audio files...");
    m.addItem (2, "Choose a folder (every audio file in it)...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (source), [&app, wid, intoEdit, source = juce::Component::SafePointer<juce::Component> (source)] (int r)
    {
        if (r == 1) importTakes (app, wid, {}, source.getComponent(), intoEdit);
        else if (r == 2)
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Choose the folder with the session's audio files");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [chooser, &app, wid, intoEdit, source] (const juce::FileChooser& fc)
                                  { if (fc.getResult() != juce::File()) openImportDialog (app, wid, intoEdit, { fc.getResult() }, source.getComponent()); });
        }
    });
}

// ======================================================================================================== applying a fix to the model
static juce::File takeFixDest (const juce::File& orig, const FixSpec& spec)
{
    auto stem = orig.getFileNameWithoutExtension();
    const int p = stem.lastIndexOf (" (");
    if (p > 0 && stem.endsWith (")") && (stem.substring (p).containsAnyOf ("+-") || stem.contains ("repair") || stem.contains ("declick") || stem.contains ("pitch curve") || stem.contains ("consonant"))) stem = stem.substring (0, p);
    return audioops::uniqueFile (orig.getParentDirectory().getChildFile (stem + " (" + spec.shortName() + ").wav"));
}

static void setUndoForTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, std::vector<TakeFile> oldFiles, const juce::String& label)
{
    app.armFixUndo (label, [&app, wid, gid, oldFiles]
    {
        if (auto* w = app.project.findTakeWindow (wid)) if (auto* g = w->findGroup (gid)) { g->files = oldFiles; app.project.changed(); }
    }, false, wid, gid);
}

static void setUndoForEdit (AppContext& app, const juce::Uuid& eid, std::vector<EditRegion> oldRegions, const juce::String& label)
{
    app.armFixUndo (label, [&app, eid, oldRegions]
    {
        if (auto* e = app.project.findEdit (eid)) { e->regions = oldRegions; app.project.changed(); }
    }, true, eid);
}

static juce::File editFixDest (const juce::File& folder, const EditRegion& r, const RegionFile& f, const FixSpec& spec)
{
    return audioops::uniqueFile (folder.getChildFile (sanitiseForFile (r.takeName.upToFirstOccurrenceOf (" (", false, false) + " - " + f.trackName) + " (" + spec.shortName() + ").wav"));
}

/** Prepares the file lists of one piece [a,b) of a region (message thread). */
static bool prepRegionJob (const juce::File& folder, const EditRegion& r, RegionJob& j, juce::int64 a, juce::int64 b, juce::int64 outFrom, juce::int64 outTo,
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
static juce::String runRegionJob (RegionJob& j, const FixSpec& spec, AudioJob& job, float p0, float p1)
{
    juce::ignoreUnused (spec);                                // (what to do is in j.ops, made by prepRegionJob)
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
static TrackParts buildTrackParts (const EditDef& e, juce::int64 from, juce::int64 to, const std::vector<juce::Uuid>& only)
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
static void readTrackMix (const std::vector<TrackPart>& parts, juce::int64 at, juce::int64 n, float* dst, ReaderCache* cache = nullptr)
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
static juce::String learnFileProfiles (const TrackParts& tp, const SpectralDenoiseSettings& s, double rate, std::vector<std::vector<denoise::Profile>>& out, AudioJob& j)
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
static juce::String attachRoomTone (std::vector<RegionJob>& jobs, const std::vector<RepairOp>& ops, const std::vector<EditRegion>& regions, const TrackParts& tp,
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
static juce::String runRepairJobs (std::vector<RegionJob>& jobs, const FixSpec& last, AudioJob& j)
{
    for (size_t i = 0; i < jobs.size(); ++i)
    {
        const auto err = runRegionJob (jobs[i], last, j, (float) i / (float) jobs.size(), (float) (i + 1) / (float) jobs.size());
        if (err.isNotEmpty()) { for (auto& rj : jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs.clear(); return err; }
    }
    return {};
}
/** Job thread: the lost samples of every change that looks for them (De-click, or a box round a click), found on all the tracks of its piece together - they are
    one event, at the same sample on every track, and the finder wants a jump seen on a fifth of the tracks before it takes it for one (on its own, each file took
    any odd sample of its own for one, and the tracks were then mended differently: their timing no longer agreed where the time squeeze put samples back).
    Read five seconds at a time, every track, so a De-click over a whole edit does not need it all in memory. */
static juce::String findLostTogether (std::vector<RegionJob>& jobs, double rate, AudioJob& j)
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
static juce::String prepareRepairJobs (const juce::File& fixFolder, const std::vector<EditRegion>& regions, juce::int64 from, juce::int64 to, double rate, const std::vector<RepairOp>& ops,
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
    return runRepairJobs (jobs, ops.back().spec, j);
}

/** The ReTouch window's consonant list (3.6.0): the project's kept consonants, keeping one (its files in the project's Consonants folder, then the list),
    renaming, deleting (its files too) and reading one back. Message thread. */
static void wireConsonants (RepairContext& c, AppContext& app)
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

// ======================================================================================================== take window: pitch / repair
static TakeWindowDef* takeWin (AppContext& app, const juce::Uuid& wid) { return app.project.findTakeWindow (wid); }

/** The take and the marked part (seconds in the take) from the take window's marks 1 / 2 marks; null + message if none. */
static TakeGroup* markedTake (AppContext& app, TakeWindowDef& w, double& in, double& out, const juce::String& title)
{
    auto* g = w.findGroup (w.editTake);
    if (g == nullptr || w.editIn < 0.0 || w.editOut <= w.editIn)
    {
        say (title, "First mark the part with mark 1 and mark 2 (in one take). To be safe, mark a little more than you need.");
        return nullptr;
    }
    juce::ignoreUnused (app);
    in = w.editIn; out = juce::jmin (w.editOut, g->lengthSeconds());
    return g;
}

/** Indices of the files of the take that are marked (all of them, or the tracks chosen with Ctrl + Alt + drag). */
static std::vector<size_t> takeSel (const TakeWindowDef& w, const TakeGroup& g)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < g.files.size(); ++i) if (w.editUses (g.files[i].trackId)) v.push_back (i);
    return v;
}

void pitchTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (! gMakeProcessPage && heldByRec (app, "Pitch correction", parent, [ap = &app, wid] (juce::Component* p) { pitchTake (*ap, wid, p); })) return;
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, "Pitch correction"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch correction", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    PitchContext c; c.allowPad = false;
    c.what = "Shifts the pitch of the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + ") on every track of the take. "
             "New audio files are made and put in the same place; the originals are not touched (Undo fix brings them back).";
    auto jobFiles = std::make_shared<audioops::TakeFixResult>();
    auto spec = std::make_shared<FixSpec>();
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch correction", "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> srcAll; for (auto i : sel) srcAll.push_back (g->files[i].file);
    c.work = [srcAll, in, out, rate, jobFiles, spec] (double cents, double, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents;
        std::vector<juce::File> src = srcAll, dst;
        for (auto& f : src) dst.push_back (takeFixDest (f, *spec));
        *jobFiles = audioops::fixTakeFiles (formats(), src, dst, (juce::int64) std::llround (in * rate), (juce::int64) std::llround (out * rate), *spec, &j);
        return jobFiles->error;
    };
    c.finish = [&app, wid, gid, jobFiles, spec, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || jobFiles->newFiles.size() != sel.size()) return;
        auto old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = jobFiles->newFiles[k];
        setUndoForTake (app, wid, gid, old, "pitch correction");
        app.project.changed();
    };
    if (gMakeProcessPage) { gProcessPage = std::make_unique<PitchDialog> (std::move (c)); return; }
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

void repairTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "ReTouch, DeClick & Denoise", parent, [ap = &app, wid] (juce::Component* p) { repairTake (*ap, wid, p); })) return;
    const juce::String nm = "Spectral Repair";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, nm); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (nm, "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> src; std::vector<juce::Uuid> srcTracks; for (auto i : sel) { src.push_back (g->files[i].file); srcTracks.push_back (g->files[i].trackId); }
    auto projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
    RepairContext c; c.rate = rate; c.length = to - from;
    c.roomSource = projectRT->source;
    for (auto& id : srcTracks) { const auto* t = projectRT->forTrack (id); c.roomMono.push_back (t != nullptr ? t->mono : denoise::Profile()); }
    c.what = "All " + juce::String ((int) src.size()) + " tracks of " + w->displayName (*g) + " are shown as one picture. Draw a box round the noise (time and pitch), then press ReTouch "
             "(or De-click if it is clicks, or Denoise for background noise with the room tone): the box is rebuilt from the clean sound next to it, on every track. New files are made; the originals stay.";
    for (auto i : sel) c.trackNames.push_back (g->files[i].trackName);
    c.trackIds = srcTracks;
    c.where = w->displayName (*g); c.areaStart = in;
    c.readChannels = [src, from] (juce::int64 a, juce::int64 b, AudioJob& j)
    {
        std::vector<std::vector<std::vector<float>>> out;
        for (auto& f : src)
        {
            if (j.cancelled()) break;
            std::vector<std::vector<float>> ch;
            auto r = audioops::openReader (formats(), f);
            if (r != nullptr) audioops::readRange (*r, from + a, b - a, ch);
            if (ch.empty()) ch.push_back (std::vector<float> ((size_t) juce::jmax ((juce::int64) 0, b - a), 0.0f));
            out.push_back (std::move (ch));
        }
        return out;
    };
    wireConsonants (c, app);
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, src.size()));
            auto r = audioops::openReader (formats(), src[i]); if (r == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*r, from, to - from, ch);
            std::vector<float> mono (ch.empty() ? 0 : ch[0].size(), 0.0f);
            for (auto& x : ch) for (size_t n = 0; n < mono.size(); ++n) mono[n] += x[n] / (float) ch.size();
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    c.prepare = [src, srcTracks, from, st, projectRT, rate] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        std::vector<audioops::FixOp> fo;
        for (auto& o : ops) fo.push_back (audioops::FixOp { o.spec, from + o.t0, from + o.t1 });
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, ops.back().spec));
        // a denoise: each file's room-tone profiles - learned from the box of this take (on the file itself, every channel), or the project's for its track
        auto boxes = std::make_shared<std::map<std::pair<double, double>, std::vector<audioops::DenoiseProfiles>>>();
        for (auto& o : ops)
        {
            if (o.spec.kind != FixSpec::Kind::Denoise || ! (o.spec.dnBoxT1 > o.spec.dnBoxT0)) continue;
            const auto key = std::make_pair (o.spec.dnBoxT0, o.spec.dnBoxT1);
            if (boxes->count (key)) continue;
            std::vector<audioops::DenoiseProfiles> per;
            for (auto& f : srcF)
            {
                juce::String err;
                per.push_back (audioops::learnDenoise (formats(), f, from + (juce::int64) std::llround (o.spec.dnBoxT0 * rate), from + (juce::int64) std::llround (o.spec.dnBoxT1 * rate), o.spec.dn, err));
                if (err.isNotEmpty()) return err;
            }
            (*boxes)[key] = std::move (per);
        }
        auto profilesFor = [boxes, srcTracks, projectRT] (size_t k, const FixSpec& spec) -> std::shared_ptr<const std::vector<denoise::Profile>>
        {
            if (spec.dnBoxT1 > spec.dnBoxT0)
            {
                auto it = boxes->find (std::make_pair (spec.dnBoxT0, spec.dnBoxT1));
                if (it == boxes->end() || k >= it->second.size()) return nullptr;
                return std::make_shared<const std::vector<denoise::Profile>> (it->second[k].perChannel);
            }
            if (k < srcTracks.size()) if (auto* t = projectRT->forTrack (srcTracks[k])) if (! t->perChannel.empty()) return std::make_shared<const std::vector<denoise::Profile>> (t->perChannel);
            return nullptr;
        };
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, fo, &j, profilesFor, srcTracks);     // one new file per track with every change in it (a pasted consonant: each its own track's part)
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;
    };
    c.discard = deleteFiles;
    c.accept = [&app, wid, gid, st] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, "spectral repair"); app.project.changed(); };
    c.play = [&app, wid, gid, in, nm] (double a, double b, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + a, in + b, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<RepairDialog> (std::move (c));
    juce::Component::SafePointer<RepairDialog> sp (dlg.get());
    launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}

void pitchCurveTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "The pitch curve", parent, [ap = &app, wid] (juce::Component* p) { pitchCurveTake (*ap, wid, p); })) return;
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, "Pitch curve"); if (g == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    const auto gid = g->id; const double rate = g->sampleRate;
    const juce::int64 from = (juce::int64) std::llround (in * rate), to = (juce::int64) std::llround (out * rate);
    const auto sel = takeSel (*w, *g);
    if (sel.empty()) { say ("Pitch curve", "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    std::vector<juce::File> src; for (auto i : sel) src.push_back (g->files[i].file);
    CurveContext c; c.rate = rate; c.length = to - from;
    c.what = "Draw how the pitch should change along the marked part (" + juce::String (out - in, 2) + " s of " + w->displayName (*g) + "), on every track. "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Preview (key R) to hear it, then Accept or Revert. "
             "The original files are never touched.";
    struct State { audioops::TakeFixResult res; std::vector<TakeFile> old; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    c.load = [src, from, to] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (to - from), 0.0f);
        for (auto& f : src)
        {
            if (j.cancelled()) break;
            auto rd = audioops::openReader (formats(), f); if (rd == nullptr) continue;
            std::vector<std::vector<float>> ch; audioops::readRange (*rd, from, to - from, ch);
            for (auto& x : ch) for (size_t n = 0; n < x.size() && n < mix.size(); ++n) mix[n] += x[n];
        }
        std::vector<std::vector<float>> o; o.push_back (std::move (mix)); return o;
    };
    c.prepare = [src, from, to, st] (const FixSpec& spec, double, AudioJob& j) -> juce::String
    {
        std::vector<juce::File> srcF = src, dst;
        for (auto& f : srcF) dst.push_back (takeFixDest (f, spec));
        st->res = audioops::fixTakeFiles (formats(), srcF, dst, from, to, spec, &j);
        return st->res.error;
    };
    auto deleteFiles = [st] { for (auto& f : st->res.newFiles) f.deleteFile(); st->res.newFiles.clear(); };
    c.apply = [&app, wid, gid, st, deleteFiles, sel]
    {
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 == nullptr || st->res.newFiles.size() != sel.size()) { deleteFiles(); return false; }
        st->old = g2->files;
        for (size_t k = 0; k < sel.size() && sel[k] < g2->files.size(); ++k) g2->files[sel[k]].file = st->res.newFiles[k];
        st->applied = true; app.project.changed();
        return true;
    };
    c.revert = [&app, wid, gid, st, deleteFiles]
    {
        if (st->applied)
        {
            auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
            st->applied = false;
        }
        deleteFiles();
    };
    c.unapply = [&app, wid, gid, st]
    {
        if (! st->applied) return;
        auto* w2 = takeWin (app, wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        if (g2 != nullptr) { g2->files = st->old; app.project.changed(); }
        st->applied = false;                                  // (the corrected files are kept, ready to be put back)
    };
    c.accept = [&app, wid, gid, st] { st->res.newFiles.clear(); st->applied = false; setUndoForTake (app, wid, gid, st->old, "pitch curve"); app.project.changed(); };
    c.play = [&app, wid, gid, in, out] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playTake (wid, gid, in + fromSec, juce::jmin (out, in + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, wid, gid, in]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Take || app.playInfo.windowId != wid || app.playInfo.id != gid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - in);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    launchDialog (withPitchTabs (std::make_unique<CurveDialog> (std::move (c)), false), "Pitch", parent, true);
}

/** The Pitch button of the take window: Pitch curve first, Pitch process on the second tab. */
void pitchTabsTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "Pitch correction", parent, [ap = &app, wid] (juce::Component* p) { pitchTabsTake (*ap, wid, p); })) return;
    gProcessPage.reset(); gMakeProcessPage = true; pitchTake (app, wid, parent); gMakeProcessPage = false;
    if (gProcessPage == nullptr) return;                                   // (a message was shown: nothing is marked)
    pitchCurveTake (app, wid, parent);
    gProcessPage.reset();
}

// ======================================================================================================== edit window: pitch / repair
static EditDef* editOf (AppContext& app, const juce::Uuid& id) { return app.project.findEdit (id); }

void pitchEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (! gMakeProcessPage && heldByRec (app, "Pitch correction", parent, [ap = &app, eid] (juce::Component* p) { pitchEdit (*ap, eid, p); })) return;
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch correction", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch correction", "There is no audio between the marks."); return; }
    PitchContext c; c.allowPad = true;
    c.what = "Shifts the pitch of the audio between marks 1 and 2 (" + juce::String (e->fixOut - e->fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)) on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The audio is replaced in the same place by new files; the originals are not touched (Undo fix brings them back). "
             "'Process extra' also corrects that much more audio each side, so you can move the edit points outward later.";
    auto jobs = std::make_shared<std::vector<RegionJob>>(); auto spec = std::make_shared<FixSpec>();
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.work = [fixFolder, regionsCopy, tIn, tOut, rate, jobs, spec, only] (double cents, double padSec, AudioJob& j) -> juce::String
    {
        spec->kind = FixSpec::Kind::Pitch; spec->cents = cents; jobs->clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, a - pad, b + pad, a - pad, b + pad, *spec, only);
            jobs->push_back (std::move (rj));
        }
        for (size_t i = 0; i < jobs->size(); ++i)
        {
            const auto err = runRegionJob ((*jobs)[i], *spec, j, (float) i / (float) jobs->size(), (float) (i + 1) / (float) jobs->size());
            if (err.isNotEmpty()) { for (auto& done : *jobs) for (auto& r : done.results) r.file.deleteFile(); jobs->clear(); return err; }
        }
        return {};
    };
    c.finish = [&app, eid, jobs, spec]
    {
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || jobs->empty()) return;
        const auto old = e2->regions;
        const bool allOk = applyRegionJobs (*e2, old, *jobs, spec->shortName());
        if (! allOk) { e2->regions = old; say ("Pitch correction", "The edit changed while the audio was being processed, so nothing was changed. Please try again."); return; }
        setUndoForEdit (app, eid, old, "pitch correction");
        app.project.changed();
    };
    if (gMakeProcessPage) { gProcessPage = std::make_unique<PitchDialog> (std::move (c)); return; }
    launchDialog (std::make_unique<PitchDialog> (std::move (c)), "Pitch correction", parent);
}

static void openRepair (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void repairEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "ReTouch, DeClick & Denoise", parent, [ap = &app, eid] (juce::Component* p) { repairEdit (*ap, eid, p); })) return;
    const juce::String nm = "Spectral Repair";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (nm, "First mark the area with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 lastEnd = 0; int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) { ++pieces; lastEnd = juce::jmax (lastEnd, r.endSample()); }
    if (pieces == 0) { say (nm, "There is no audio between the marks."); return; }
    const auto from = tIn, to = juce::jmin (tOut, lastEnd);
    if (to - from < (juce::int64) (0.05 * rate)) { say (nm, "The marked area is too short (at least about 0.05 s)."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, from, to, outS, inS, nm);
    if (sm < 0) return;
    if (sm == 0) { openRepair (app, eid, parent, from, to, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, nm, "spectral repair");
    openRepair (app, eid, parent, from, to, outS, pair, 0);
    openRepair (app, eid, parent, from, to, inS, pair, 1);
    pair->focus (0);                                           // the keyboard (Space ...) starts with the OUT stream
}

static void openRepair (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    const juce::String nm = "Spectral Repair";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    // the pieces that are heard in the marked area (in a stream window: only that stream's pieces)
    int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id))) ++pieces;
    const auto only = e->fixTracks;
    // the tracks (one picture row each) and, for each, where its audio comes from along the marked area: whoever owns each moment (the piece that is on top there)
    struct Part { juce::File file; juce::int64 fileStart = 0, srcA = 0, n = 0, dst = 0; };
    std::vector<juce::Uuid> trackIds; std::vector<std::vector<Part>> parts;
    {
        for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id)))
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                if (std::find (trackIds.begin(), trackIds.end(), f.trackId) == trackIds.end()) { trackIds.push_back (f.trackId); parts.emplace_back(); }
            }
        if (ss.on)
        {
            // a stream: ONLY its own take's audio, as it is in the files - the out stream carries on past the edit into its handle, the in stream starts before it
            juce::int64 prevHi = from;
            for (auto& id : ss.ids)
            {
                const auto* rp = [&] () -> const EditRegion* { for (auto& r : e->regions) if (r.id == id) return &r; return nullptr; }();
                if (rp == nullptr) continue;
                const auto& r = *rp; juce::int64 lo = 0, hi = 0; ss.span (r, from, to, lo, hi);
                lo = juce::jmax (lo, prevHi);
                if (hi <= lo) continue;
                prevHi = hi;
                for (auto& f : r.files)
                {
                    const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                    if (ti == trackIds.end()) continue;
                    parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (lo - r.startSample), hi - lo, lo - from });
                }
            }
        }
        else
        {
            std::vector<juce::int64> cuts { from, to };
            for (auto& r : e->regions) { if (r.startSample > from && r.startSample < to) cuts.push_back (r.startSample); if (r.endSample() > from && r.endSample() < to) cuts.push_back (r.endSample()); }
            std::sort (cuts.begin(), cuts.end()); cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
            for (size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                const auto t0 = cuts[k], t1 = cuts[k + 1];
                const int ri = e->regionAt (t0 + (t1 - t0) / 2);
                if (ri < 0) continue;                                                       // a gap: silence
                const auto& r = e->regions[(size_t) ri];
                for (auto& f : r.files)
                {
                    const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                    if (ti == trackIds.end()) continue;
                    parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, r.srcIn + (t0 - r.startSample), t1 - t0, t0 - from });
                }
            }
        }
    }
    if (trackIds.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the marked area."); if (pair != nullptr) pair->closeAll(); return; }
    // the same parts as a TrackParts (for a denoise: the room tone learned from a box of this area on the real files), and the project's room tone for the picture tracks
    auto tpArea = std::make_shared<TrackParts>();
    tpArea->ids = trackIds;
    for (size_t i = 0; i < parts.size(); ++i) { tpArea->names.push_back (juce::String()); tpArea->parts.emplace_back(); for (auto& pt : parts[i]) tpArea->parts.back().push_back ({ pt.file, pt.fileStart, pt.srcA, pt.n, pt.dst }); }
    for (auto& r : e->regions) for (auto& f : r.files) for (size_t i = 0; i < trackIds.size(); ++i) if (f.trackId == trackIds[i] && tpArea->names[i].isEmpty()) tpArea->names[i] = f.trackName;
    auto projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
    RepairContext c; c.rate = rate; c.length = to - from;
    c.roomSource = projectRT->source;
    for (auto& id : trackIds) { const auto* t = projectRT->forTrack (id); c.roomMono.push_back (t != nullptr ? t->mono : denoise::Profile()); }
    const juce::String lead = ss.on ? ss.label + ": the sound of the " + (slot == 0 ? "take that plays up to the edit point, carried on past it (its crossfade uses that part)" : "take that plays from the edit point, started before it (its crossfade uses that part)")
                                      + ", on its own. Correct it here; correct the other stream in the other window; then press Write back in BOTH. The edit point and its fades stay exactly where they are. " : juce::String();
    c.what = lead + (juce::String (only.empty() ? "All " : "The ") + juce::String ((int) trackIds.size()) + " tracks between marks 1 and 2 are shown as one picture, "
                       + (pieces > 1 && ! ss.on ? "across the edit points (" + juce::String (pieces) + " pieces). " : juce::String ("")) + "Draw a box round the noise (time and pitch), then press ReTouch (or De-click if it is clicks, or Denoise for background noise with the room tone): "
                       "the box is rebuilt from the clean sound next to it, on every track. Press Play to listen, Undo if it did not work, and Write back to file when you are happy. Each piece is repaired in its own place: the edit points and their fades stay exactly as they are, and the original files are not touched (Undo fix brings them back).");
    const auto total = to - from;
    c.trackIds = trackIds; c.trackNames = tpArea->names;
    c.where = e->name; c.areaStart = (double) from / rate;
    c.readChannels = [parts] (juce::int64 a, juce::int64 b, AudioJob& j)
    {
        // each track's channels over [a, b) of the area: whoever owns each moment, every channel as its file has it (silence where no piece is heard)
        std::vector<std::vector<std::vector<float>>> out;
        const auto n = juce::jmax ((juce::int64) 0, b - a);
        for (auto& tparts : parts)
        {
            if (j.cancelled()) break;
            std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
            int nc = 1;
            for (auto& pt : tparts) { readers.push_back (audioops::openReader (formats(), pt.file)); if (readers.back() != nullptr) nc = juce::jmax (nc, (int) readers.back()->numChannels); }
            std::vector<std::vector<float>> ch ((size_t) nc, std::vector<float> ((size_t) n, 0.0f));
            for (size_t k = 0; k < tparts.size(); ++k)
            {
                const auto& pt = tparts[k];
                if (readers[k] == nullptr) continue;
                const auto p0 = juce::jmax (a, pt.dst), p1 = juce::jmin (b, pt.dst + pt.n);
                if (p1 <= p0) continue;
                juce::int64 rel = pt.srcA - pt.fileStart + (p0 - pt.dst), cnt = p1 - p0, o = p0 - a;
                if (rel < 0) { cnt += rel; o -= rel; rel = 0; }
                if (cnt <= 0) continue;
                std::vector<std::vector<float>> x; audioops::readRange (*readers[k], rel, cnt, x);
                if (x.empty()) continue;
                for (size_t c2 = 0; c2 < ch.size(); ++c2)
                {
                    const auto& src2 = x[juce::jmin (c2, x.size() - 1)];
                    for (juce::int64 q = 0; q < cnt && q < (juce::int64) src2.size(); ++q) ch[c2][(size_t) (o + q)] = src2[(size_t) q];
                }
            }
            out.push_back (std::move (ch));
        }
        return out;
    };
    wireConsonants (c, app);
    c.load = [parts, total] (AudioJob& j)
    {
        std::vector<std::vector<float>> tracks;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (j.cancelled()) break;
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, parts.size()));
            std::vector<float> mono ((size_t) total, 0.0f);
            for (auto& pt : parts[i])
            {
                auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                juce::int64 rel = pt.srcA - pt.fileStart, n = pt.n, dst = pt.dst;
                if (rel < 0) { n += rel; dst -= rel; rel = 0; }
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, n, ch);
                for (auto& x : ch) for (size_t k = 0; k < x.size() && dst + (juce::int64) k < total; ++k) mono[(size_t) dst + k] += x[k] / (float) ch.size();
            }
            tracks.push_back (std::move (mono));
        }
        return tracks;
    };
    struct State { std::vector<RegionJob> jobs; std::vector<EditRegion> old; juce::String suffix; PreviewApplied applied; };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    auto deleteFiles = [st] { for (auto& rj : st->jobs) for (auto& r : rj.results) r.file.deleteFile(); st->jobs.clear(); };
    c.prepare = [fixFolder, regionsCopy, from, to, rate, st, only, deleteFiles, ss, tpArea, projectRT] (const std::vector<RepairOp>& ops, AudioJob& j) -> juce::String
    {
        if (ops.empty()) return "Nothing to do.";
        deleteFiles();
        st->suffix = ops.back().spec.shortName();
        const auto h = (juce::int64) (1.0 * rate);        // each piece is made again from its own sound with 1 s of untouched sound either side (the audio next to the marks, for the fades)
        const auto shortest = (juce::int64) (0.03 * rate);
        // each piece gets the changes that fall inside it, in ITS OWN samples: the same box or click search, applied to its own audio
        juce::int64 prevHi = from;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= from || r.startSample >= to) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
            // the stretch this piece corrects: its own part of the marked area, and (a stream) the part of its take past the edit point, which its crossfade uses
            juce::int64 lo = r0, hi = r1;
            if (ss.on) { ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            if (hi - lo < 2) continue;
            const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
            std::vector<audioops::FixOp> mine;
            for (auto& o : ops)
            {
                const auto i0 = juce::jmax (lo, from + o.t0), i1 = juce::jmin (hi, from + o.t1);
                if (i1 - i0 < 2) continue;
                if (o.spec.kind == FixSpec::Kind::Paste)                                 // a pasted consonant keeps its own start, even where it reaches across this piece's start:
                {                                                                        // each piece adds its own part of it, in the right place
                    mine.push_back (audioops::FixOp { o.spec, r.srcIn + (from + o.t0 - r.startSample), r.srcIn + (from + o.t1 - r.startSample) });
                    continue;
                }
                if (o.t1 - o.t0 > shortest && i1 - i0 <= shortest) continue;           // a sliver of a bigger search is not taken for 'the click itself'
                const auto s0 = r.srcIn + (i0 - r.startSample);
                auto sp = o.spec;
                if (sp.lostKnown)
                {
                    // the lost samples found on all the tracks: those in this piece, counted from its own start; the time squeeze that puts them back stays inside the piece
                    // that is put in (35 ms clear of its ends: the joins there cross-fade with the untouched take, which must have the same timing)
                    std::vector<FixSpec::Lost> keep;
                    for (auto l : sp.lost) { const auto atEdit = from + o.t0 + l.at; if (atEdit >= i0 && atEdit < i1) { l.at = atEdit - i0; keep.push_back (l); } }
                    sp.lost = keep;
                    const auto xf = (juce::int64) (0.035 * rate);
                    sp.lostLo = (a + xf) - s0; sp.lostHi = (b - xf) - s0;
                }
                mine.push_back (audioops::FixOp { sp, s0, r.srcIn + (i1 - r.startSample) });
            }
            if (mine.empty()) continue;
            if (r1 <= r0) continue;
            RegionJob rj;
            const auto fa = juce::jmin (a, r.srcIn + (lo - r.startSample)), fb = juce::jmax (b, r.srcIn + (hi - r.startSample));
            prepRegionJob (fixFolder, r, rj, a, b, fa - h, fb + h, fa, fb, ops.back().spec, only);
            rj.ops = mine;
            st->jobs.push_back (std::move (rj));
        }
        if (st->jobs.empty()) return "Nothing to do in this area.";
        {   // a denoise among the changes: every file's room-tone profiles (the project's, or learned from the box of this area)
            const auto err = attachRoomTone (st->jobs, ops, *regionsCopy, *tpArea, projectRT, rate, j);
            if (err.isNotEmpty()) { deleteFiles(); return err; }
        }
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            const auto err = runRegionJob (st->jobs[i], ops.back().spec, j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { deleteFiles(); return err; }
        }
        return {};
    };
    if (pair != nullptr)
    {
        // a stream window: the pair puts the corrected pieces of both streams into the edit (always from the original edit), and keeps one undo step for both
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "repair";
        c.apply = [pair, slot, st] { if (st->jobs.empty()) return false; return pair->put (slot, true); };
        c.unapply = [pair, slot] { pair->put (slot, false); };
        c.accept = [] {};
        c.pairWritten = [pair, slot] (std::function<bool()> commit) { pair->wrote (slot, std::move (commit)); };
    }
    else
    {
        c.apply = [&app, eid, st, deleteFiles]
        {
            auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) return false;
            st->old = e2->regions;
            const bool ok = applyRegionJobs (*e2, st->old, st->jobs, st->suffix);
            if (! ok) { e2->regions = st->old; deleteFiles(); return false; }
            st->applied = true; app.project.changed();
            return true;
        };
        c.unapply = [&app, eid, st]
        {
            if (! st->applied) return;
            if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
            st->applied = false;
        };
        c.accept = [&app, eid, st] { for (auto& rj : st->jobs) rj.results.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, "spectral repair"); app.project.changed(); };
    }
    c.discard = deleteFiles;
    const double areaStart = (double) from / rate;
    c.play = [&app, eid, areaStart, nm] (double x, double y, bool loop)
    {
        const auto err = app.playEdit (eid, areaStart + x, areaStart + y, loop);
        if (err.isNotEmpty()) say (nm, err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, areaStart]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - areaStart);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<RepairDialog> (std::move (c));
    juce::Component::SafePointer<RepairDialog> sp (dlg.get());
    if (pair != nullptr) launchStream (pair, slot, std::move (dlg), nm + " - " + ss.label, parent);
    else launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}


// ======================================================================================================== Noise mode (edit window)
/** The whole edit in one spectral window, built once at several resolutions (SpectralPyramid), with Gain, ReTouch, De-click and the background denoiser
    all to hand: each is applied the moment it is made, on the result of the last, as new piece files put into the edit (the edit plays through them at once);
    Undo takes the last one off; Write back to clip keeps what has been done (the window stays open); Finished closes and throws away what was not written. */
class NoiseModeDialog : public JobDialogBase
{
public:
    NoiseModeDialog (AppContext& a, const juce::Uuid& editId, double sampleRate, juce::int64 editLength, std::vector<juce::Uuid> onlyTracks, const juce::String& what)
        : app (a), eid (editId), rate (sampleRate), length (editLength), only (std::move (onlyTracks))
    {
        setSize (1100, 800);
        fixFolder = app.project.audioFolder().getChildFile ("Fixes");
        if (auto* e = app.project.findEdit (eid)) { base = e->regions; cur = std::make_shared<TrackParts> (buildTrackParts (*e, 0, length, only)); }
        projectRT = std::make_shared<const Project::RoomTone> (app.project.roomTone);
        intro.setText (what, juce::dontSendNotification); intro.setColour (juce::Label::textColourId, theme::text); intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro); addAndMakeVisible (view); addAndMakeVisible (info);
        info.setColour (juce::Label::textColourId, theme::text);
        auto cap = [this] (juce::Label& l, const juce::String& t) { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); };
        cap (sensCap, "Click sensitivity"); cap (strengthCap, "Strength"); cap (surroundCap, "Surrounding"); cap (weightCap, "Before <-> After");
        cap (amountCap, "Amount"); cap (residualCap, "Residual"); cap (roomCap, "");
        auto slider = [this] (juce::Slider& sl, double lo, double hi, double step, double v, const juce::String& suffix, int boxW)
        {
            sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (lo, hi, step); sl.setValue (v, juce::dontSendNotification); sl.setScrollWheelEnabled (false);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, boxW, 22); sl.setTextValueSuffix (suffix);
            sl.onValueChange = [this] { updateSurround(); };
            addAndMakeVisible (sl);
        };
        slider (sens, 1, 10, 1, 6, "", 50); slider (strength, 0, 100, 1, 80, " %", 56); slider (surround, 10, 400, 5, 100, " %", 56); slider (weight, 0, 100, 1, 50, " %", 56);
        slider (amount, -6.0, 12.0, 0.5, 0.0, " dB", 62); slider (residual, -30.0, -3.0, 1.0, -12.0, " dB", 62);
        amount.setDoubleClickReturnValue (true, 0.0); residual.setDoubleClickReturnValue (true, -12.0);
        methodBox.addItem ("Method: Attenuate", 1); methodBox.addItem ("Method: Replace", 2);
        methodBox.addItem ("Method: Stray consonant", 3);
        methodBox.setSelectedId (1, juce::dontSendNotification);
        dirBox.addItem ("Direction: Horizontal", 1); dirBox.addItem ("Direction: Vertical", 2); dirBox.addItem ("Direction: 2D (both)", 3);
        dirBox.setSelectedId (1, juce::dontSendNotification);
        for (int b : { 128, 256, 512, 1024, 2048, 4096 }) bandsBox.addItem ("Bands: " + juce::String (b), b);
        bandsBox.setSelectedId (1024, juce::dontSendNotification);
        multiBtn.setToggleState (true, juce::dontSendNotification); linkBtn.setToggleState (true, juce::dontSendNotification);
        methodBox.setTooltip ("ReTouch. Attenuate: keeps the music under the noise and only turns down what sticks out (a stray consonant is the same, matched to the side away from the main one). Replace: rebuilds the box from the surroundings.");
        dirBox.setTooltip ("Where the material comes from: the sound before and after the box (Horizontal), the sound above and below it (Vertical), or both (2D).");
        strength.setTooltip ("How much of the repair replaces the original sound inside the box."); surround.setTooltip ("How much of the sound around the box is used, as a percentage of the box.");
        weight.setTooltip ("0 uses only the sound before the box, 100 only the sound after it, 50 both."); sens.setTooltip ("De-click: how readily clicks are found (1 few .. 10 many).");
        amount.setTooltip ("Denoise: how much louder than measured the room tone is taken to be. Double-click for 0."); residual.setTooltip ("Denoise: how much of the room tone is left in the box (the gain never goes below this). Double-click for -12.");
        specBtn.setTooltip ("How the picture is drawn: sharpness, colours, and the levels shown.");
        specBtn.onClick = [this] { openSpectroSettings(); };
        for (auto* x : std::initializer_list<juce::Component*> { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &specBtn, &scaleBox, &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn,
                                                                 &revertBtn, &finishBtn, &doneBtn, &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn, &loopBtn, &clock }) addAndMakeVisible (x);
        scaleBox.addItem ("Pitch scale: Mel (as iZotope RX)", 6); scaleBox.addItem ("Pitch scale: linear (even Hz)", 1); scaleBox.addItem ("Pitch scale: mostly linear", 2); scaleBox.addItem ("Pitch scale: halfway", 3);
        scaleBox.addItem ("Pitch scale: mostly musical", 4); scaleBox.addItem ("Pitch scale: musical (every octave the same height)", 5);
        scaleBox.setSelectedId (6, juce::dontSendNotification); view.setMel();
        scaleBox.onChange = [this] { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; const int id = scaleBox.getSelectedId(); if (id == 6) view.setMel(); else view.setScale (w[juce::jlimit (0, 4, id - 1)]); };
        view.onNote = [this] (const juce::String& t) { status.setText (t, juce::dontSendNotification); };
        view.onNeedDetail = [this] (const PictureRequest& rq) { startDetail (rq); };
        view.onView = [this] { sliceForView(); };
        view.onSelection = [this] { refreshControls(); };
        dirBox.onChange = [this] { refreshControls(); }; methodBox.onChange = [this] { refreshControls(); };
        // the transport, as in the edit window: start, back 5 s, play / stop, forward 5 s, end - and a clock that can be typed into
        startBtn.setIcon (Icon::Start, "Back to the start of the edit [Home]"); rewBtn.setIcon (Icon::Rewind, "Back 5 seconds");
        playBtn.setIcon (Icon::Play, "Play / Stop [Space]"); fwdBtn.setIcon (Icon::Forward, "Forward 5 seconds"); endBtn.setIcon (Icon::End, "To the end of the edit [End]");
        playBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
        playBtn.setTooltip ("Plays from the red playhead (click the time ruler to place it; without one, from the left of the page) through the mixers, all the way to the END of the edit: the page turns by itself as the playhead reaches its edge. "
                            "Every fix is heard the moment it is made. Press again to stop. Key: Space");
        startBtn.setTooltip ("The playhead goes back to the beginning of the edit (and keeps playing from there if it was playing).");
        rewBtn.setTooltip ("Back 5 seconds"); fwdBtn.setTooltip ("Forward 5 seconds");
        endBtn.setTooltip ("The playhead goes to the end of the edit.");
        loopBtn.setTooltip ("Repeat the PAGE you can see until you press Stop. Without Loop, playing carries on to the end of the whole edit and the page turns by itself.");
        playBtn.onClick = [this] { if (! built || running) return; if (isPlayingNow()) app.stopPlayback(); else playPressed(); };
        startBtn.onClick = [this] { goTo (0.0); };
        rewBtn.onClick = [this] { jumpBy (-5.0); };
        fwdBtn.onClick = [this] { jumpBy (5.0); };
        endBtn.onClick = [this] { goTo (seconds()); };
        loopBtn.onClick = [this]
        {
            if (! built || running || ! isPlayingNow()) return;
            const double pos = juce::jmax (0.0, app.playheadSeconds());
            app.stopPlayback(); playRange (pos);                         // (carries on from where it is: now looping the page, or on to the end)
        };
        clock.setSampleRate (rate);
        clock.setCaption ("Edit");
        clock.setTooltip ("Where the playhead is. Double-click to type a time and jump there; click the small label to change how the time is written.");
        clock.onJump = [this] (double t) { goTo (t); };
        clock.follow ([this] { return transportPos(); }, [this] { return isPlayingNow() ? TimeReadout::State::playing : TimeReadout::State::stopped; }, 30);
        for (auto* l : { &info, &roomCap }) { l->setFont (juce::FontOptions (12.5f)); l->setMinimumHorizontalScale (0.7f); }
        status.setFont (juce::FontOptions (12.5f)); status.setMinimumHorizontalScale (0.75f);
        gainBtn.onClick = [this] { openGain(); }; retouchBtn.onClick = [this] { retouch (false); }; declickBtn.onClick = [this] { retouch (true); };
        learnBtn.onClick = [this] { roomFromBox(); }; denoiseBtn.onClick = [this] { denoise(); };
        revertBtn.onClick = [this] { revert(); }; finishBtn.onClick = [this] { writeBack(); }; doneBtn.onClick = [this] { if (running) job.requestCancel(); else if (mayClose()) closeDialogOf (this); };
        for (auto* b : { &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn }) b->setColour (juce::TextButton::buttonColourId, theme::accent);
        finishBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f7a46));
        gainBtn.setTooltip ("Gain: makes the sound inside the box louder or quieter (fading in and out at its edges), and nothing else. Key: G");
        retouchBtn.setTooltip ("ReTouch: rebuilds or turns down the box you drew from its surroundings, on every track, and puts it into the edit at once. Key: R");
        declickBtn.setTooltip ("De-click: finds and mends the clicks in the time span of the box (or of the whole edit if there is no box). Key: D");
        learnBtn.setTooltip ("Draw a box over a part of the picture that holds nothing but the room (its pitch range does not matter: the whole spectrum is learned) and press this: that box is the room tone for Denoise here, instead of the project's (the edit window's Room tone button). Key: L");
        denoiseBtn.setTooltip ("Denoise: takes the background noise out of the box you drew - its time span and its pitch range, on every track - with the room tone, and puts it into the edit at once. Above and below the box's pitch range nothing is touched; the denoising fades in and out at the box's edges. Key: N");
        revertBtn.setTooltip ("Takes the last fix off again (as many times as there are fixes since the last Write back).");
        finishBtn.setTooltip ("Keeps everything done so far in the edit (the Undo fix command of the edit window brings the audio back to how it was when this window opened, or when you last wrote back) and carries on: the window stays open.");
        doneBtn.setTooltip ("Closes the window. Fixes made since the last Write back are thrown away.");
        ticker.owner = this; ticker.startTimerHz (30);
        app.undoHold += 1;
        setupTabs();
        loadControls();
        setWantsKeyboardFocus (true);
        for (auto* ch : getChildren()) if (dynamic_cast<juce::Button*> (ch) != nullptr || dynamic_cast<juce::ComboBox*> (ch) != nullptr) ch->setWantsKeyboardFocus (false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NoiseModeDialog> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
        refreshControls();
        buildPicture();
    }
    ~NoiseModeDialog() override
    {
        saveControls();
        ticker.stopTimer(); ticker.owner = nullptr;
        if (detailCancel != nullptr) detailCancel->store (true);
        app.stopPlayback();
        discardUncommitted();
        app.undoHold -= 1;
    }
    // ---- the tools at the bottom: one tab for each tool (only one set of controls shows at a time, so the picture keeps the room), then the transport and the fixes ----
    enum { kTabTouch, kTabGain, kTabClick, kTabDenoise, kTabCount };
    static constexpr int kRowH = 24, kRowGap = 4, kPanelPad = 6, kTabW = 104, kTabGap = 12, kCapGap = 12, kTransportH = 26, kActionW = 116;      // (kActionW: the button that does the work, the same size and in the same place on every tab)
    static constexpr int sliderW = 140;                                           // a slider with its number: short on purpose
    static constexpr int kChoicesW = 184 + 8 + 180 + 8 + 96 + 8 + 132 + 8 + 112;                       // the five choices of ReTouch in a row
    static constexpr int kAmountsW = (58 + sliderW) + kCapGap + (116 + sliderW) + kCapGap + (100 + sliderW) + 16 + kActionW;   // its three sliders and the button in a row
    static constexpr int kBarMinW = 90, kFixesW = 90 + 8 + 150 + 8 + 150;
    int tab = kTabTouch;
    juce::TextButton tabBtn[kTabCount];
    juce::Label gainHint;
    juce::Rectangle<int> panelR, contentR;                                       // the tool panel and the part of it that holds the controls (set by layoutBottom)
    /** ReTouch has two rows (the choices; the sliders and the button) and, where the window is narrow, one more for each that does not fit in a row. */
    static int reTouchRows (int w) { return (w >= kChoicesW ? 1 : 2) + (w >= kAmountsW ? 1 : 2); }
    int listenW() const { return 5 * 34 + 4 * 2 + 6 + 64 + 8 + clock.getIdealWidth (28) + 4; }                // the transport: five buttons, Loop and the clock
    int transportRows (int w) const { return w >= listenW() + 8 + kBarMinW + 16 + kFixesW ? 1 : 2; }
    static juce::Rectangle<int> rowOf (juce::Rectangle<int> inner, int row) { return { inner.getX(), inner.getY() + row * (kRowH + kRowGap), inner.getWidth(), kRowH }; }
    /** Puts a control at the left of the row (nothing if it is hidden: the ones after it move up to fill the space). */
    static void putLeft (juce::Rectangle<int>& row, juce::Component& c, int w, int gap = 8)
    {
        if (! c.isVisible()) return;
        c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap);
    }
    /** A caption and the slider after it. */
    static void putSlider (juce::Rectangle<int>& row, juce::Label& cap, juce::Slider& sl, int capW)
    {
        if (! sl.isVisible()) { cap.setBounds ({}); return; }
        cap.setBounds (row.removeFromLeft (capW)); sl.setBounds (row.removeFromLeft (sliderW)); row.removeFromLeft (kCapGap);
    }
    std::vector<juce::Component*> tabControls (int t)
    {
        switch (t)
        {
            case kTabTouch:   return { &methodBox, &dirBox, &bandsBox, &multiBtn, &linkBtn, &strengthCap, &strength, &surroundCap, &surround, &weightCap, &weight, &retouchBtn };
            case kTabGain:    return { &gainBtn, &gainHint };
            case kTabClick:   return { &sensCap, &sens, &declickBtn };
            default:          return { &amountCap, &amount, &residualCap, &residual, &learnBtn, &roomCap, &denoiseBtn };
        }
    }
    /** Lights the tab that is showing and no other. (By hand, not with a radio group: JUCE's radio group runs the click action of the key it turns off, and that put ReTouch back on.) */
    void lightTab() { for (int i = 0; i < kTabCount; ++i) tabBtn[i].setToggleState (i == tab, juce::dontSendNotification); }
    void selectTab (int t)
    {
        tab = juce::jlimit (0, (int) kTabCount - 1, t);
        lightTab();
        refreshControls();                                                       // (which also lays the controls out again)
    }
    void setupTabs()
    {
        const char* names[] = { "ReTouch [R]", "Gain [G]", "De-click [D]", "Denoise [N]" };
        const char* tips[] = { "ReTouch: rebuilds or turns down the box you drew from its surroundings (key R).", "Gain: makes the sound in the box louder or quieter (key G).",
                               "De-click: finds and mends clicks (key D).", "Denoise: takes the room noise out of the box (keys L and N)." };
        for (int i = 0; i < kTabCount; ++i)
        {
            auto& t = tabBtn[i]; t.setButtonText (names[i]); t.setTooltip (tips[i]); t.setClickingTogglesState (false);
            t.setColour (juce::TextButton::buttonOnColourId, theme::accent);
            t.onClick = [this, i] { selectTab (i); };
            addAndMakeVisible (t);
        }
        lightTab();
        gainHint.setText ("Louder or quieter inside the box, from silence up to +18 dB, and nothing else. Draw a box, then press Gain [G].", juce::dontSendNotification);
        gainHint.setColour (juce::Label::textColourId, theme::dimText); gainHint.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (gainHint);
    }
    /** The controls of the tab that is showing, in the panel's content area. */
    void placeTab (juce::Rectangle<int> in, int rows)
    {
        auto lastRow = rowOf (in, rows - 1);                                     // the button that does the work is in the same place whichever tab shows: the bottom right corner of the panel
        const auto action = lastRow.removeFromRight (kActionW);
        switch (tab)
        {
            case kTabTouch:
            {   // first row what to do and how, then how much, and the button that does it
                int row = 0;
                auto r = rowOf (in, row++);
                putLeft (r, methodBox, 184); putLeft (r, dirBox, 180); putLeft (r, bandsBox, 96);
                if (in.getWidth() < kChoicesW) r = rowOf (in, row++);                                        // narrow: the two ticks on a row of their own
                putLeft (r, multiBtn, 132); putLeft (r, linkBtn, 112);
                r = rowOf (in, row++);
                if (in.getWidth() >= kAmountsW)
                {
                    retouchBtn.setBounds (action); r.removeFromRight (kActionW + 16);
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116); putSlider (r, weightCap, weight, 100);
                }
                else                                                                                           // narrow: Before <-> After and the button go on a second row
                {
                    putSlider (r, strengthCap, strength, 58); putSlider (r, surroundCap, surround, 116);
                    r = rowOf (in, row++);
                    putSlider (r, weightCap, weight, 100);
                    retouchBtn.setBounds (action);
                }
                break;
            }
            case kTabGain:
            {
                gainBtn.setBounds (action); gainHint.setBounds (rowOf (in, 0));
                break;
            }
            case kTabClick:
            {
                auto r = rowOf (in, 0);
                sensCap.setBounds (r.removeFromLeft (112)); r.removeFromLeft (8); sens.setBounds (r.removeFromLeft (sliderW));
                declickBtn.setBounds (action);
                break;
            }
            default:
            {
                auto r1 = rowOf (in, 0), r2 = rowOf (in, 1);
                amountCap.setBounds (r1.removeFromLeft (62)); amount.setBounds (r1.removeFromLeft (sliderW)); r1.removeFromLeft (kCapGap);
                residualCap.setBounds (r1.removeFromLeft (66)); residual.setBounds (r1.removeFromLeft (sliderW));
                denoiseBtn.setBounds (action); r2.removeFromRight (kActionW + 8);
                learnBtn.setBounds (r2.removeFromLeft (190)); r2.removeFromLeft (8); roomCap.setBounds (r2);
                break;
            }
        }
    }
    /** The tool panel (the tabs in two columns on the left, the controls of the chosen one beside them) and under it the transport and the fixes.
        Returns the height they need at this width (place = false only measures). */
    int layoutBottom (juce::Rectangle<int> area, bool place)
    {
        const int tabsW = 2 * kTabW + kRowGap;
        const int contentW = juce::jmax (100, area.getWidth() - 2 * kPanelPad - tabsW - kTabGap);
        const int rows = juce::jmax (2, reTouchRows (contentW));                  // (the same height whichever tab shows, so the picture does not jump)
        const int panelH = 2 * kPanelPad + rows * kRowH + (rows - 1) * kRowGap;
        const int trRows = transportRows (area.getWidth());
        const int total = panelH + trRows * (kRowGap + kTransportH);
        if (! place) return total;
        panelR = { area.getX(), area.getY(), area.getWidth(), panelH };
        auto inner = panelR.reduced (kPanelPad);
        auto tabsArea = inner.removeFromLeft (tabsW); inner.removeFromLeft (kTabGap);
        for (int i = 0; i < kTabCount; ++i) tabBtn[i].setBounds (tabsArea.getX() + (i % 2) * (kTabW + kRowGap), tabsArea.getY() + (i / 2) * (kRowH + kRowGap), kTabW, kRowH);
        contentR = inner;
        placeTab (contentR, rows);
        auto placeFixes = [this] (juce::Rectangle<int> r)
        {
            doneBtn.setBounds (r.removeFromRight (150)); r.removeFromRight (8);
            finishBtn.setBounds (r.removeFromRight (150)); r.removeFromRight (8);
            revertBtn.setBounds (r.removeFromRight (90));
        };
        auto row = juce::Rectangle<int> (area.getX(), panelR.getBottom() + kRowGap, area.getWidth(), kTransportH);
        if (trRows == 1) placeFixes (row.removeFromRight (kFixesW));
        for (auto* b : { &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn }) { b->setBounds (row.removeFromLeft (34).withSizeKeepingCentre (34, 26)); row.removeFromLeft (2); }
        row.removeFromLeft (6); loopBtn.setBounds (row.removeFromLeft (64)); row.removeFromLeft (8);
        const int clockW = clock.getIdealWidth (28) + 4;
        clock.setBounds (row.removeFromLeft (clockW)); row.removeFromLeft (8);
        bar.setBounds (row.withTrimmedRight (trRows == 1 ? 16 : 0).reduced (0, 3));                      // (the progress bar, while something is being made)
        if (trRows == 2) placeFixes ({ area.getX(), row.getBottom() + kRowGap, area.getWidth(), kTransportH });
        return total;
    }
    void paint (juce::Graphics& g) override
    {
        if (panelR.isEmpty()) return;
        const auto b = panelR.toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.16f)); g.fillRoundedRectangle (b, 5.0f);
        const auto line = theme::manuscript() ? theme::giltDim() : theme::border.brighter (0.25f);
        g.setColour (line); g.drawRoundedRectangle (b, 5.0f, 1.0f);
        const float dx = (float) contentR.getX() - (float) kTabGap * 0.5f;
        g.setColour (line.withAlpha (0.7f)); g.drawLine (dx, b.getY() + 5.0f, dx, b.getBottom() - 5.0f, 1.0f);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        intro.placeTopRight (*this, 6);                                          // the explanation is behind the blue i
        const int bottomH = layoutBottom (r, false);                             // how high the tools need to be at this width
        auto bottom = r.removeFromBottom (bottomH);
        layoutBottom (bottom, true);
        r.removeFromBottom (4);
        status.setBounds (r.removeFromBottom (22).reduced (2, 0));
        {
            auto ir = r.removeFromBottom (22);
            specBtn.setBounds (ir.removeFromRight (150)); ir.removeFromRight (6); scaleBox.setBounds (ir.removeFromRight (270)); ir.removeFromRight (6);
            info.setBounds (ir.reduced (2, 0));
        }
        view.setBounds (r);
        repaint (bottom.expanded (2));
    }
protected:
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)
        {
            if (spaceIsForRecording()) return true;
            if (built && ! running) { if (isPlayingNow()) app.stopPlayback(); else playPressed(); }
            return true;
        }
        const auto ch = k.getTextCharacter();
        if (! k.getModifiers().isAnyModifierKeyDown())
        {
            if (ch == 'r' || ch == 'R') { selectTab (kTabTouch); if (retouchBtn.isEnabled()) retouch (false); return true; }
            if (ch == 'g' || ch == 'G') { selectTab (kTabGain); if (gainBtn.isEnabled()) openGain(); return true; }
            if (ch == 'd' || ch == 'D') { selectTab (kTabClick); if (declickBtn.isEnabled()) retouch (true); return true; }
            if (ch == 'l' || ch == 'L') { selectTab (kTabDenoise); if (learnBtn.isEnabled()) roomFromBox(); return true; }
            if (ch == 'n' || ch == 'N') { selectTab (kTabDenoise); if (denoiseBtn.isEnabled()) denoise(); return true; }
        }
        if (k == juce::KeyPress::homeKey) { if (built && ! running) goTo (0.0); return true; }
        if (k == juce::KeyPress::endKey)  { if (built && ! running) goTo (seconds()); return true; }
        if (built && view.zoomKey (k)) return true;
        return false;
    }
private:
    struct Step { std::vector<EditRegion> before; std::vector<RegionJob> jobs; juce::String label; double t0 = 0.0, t1 = 0.0; };
    juce::String prefKey (const char* k) const { return juce::String ("noisemode.") + k; }
    void loadControls()
    {
        auto& p = uiPrefs();
        auto num = [&] (const char* k, juce::Slider& s) { const auto key = prefKey (k); if (p.containsKey (key)) s.setValue (juce::jlimit (s.getMinimum(), s.getMaximum(), p.getDoubleValue (key)), juce::dontSendNotification); };
        auto id = [&] (const char* k, juce::ComboBox& b) { const auto key = prefKey (k); if (p.containsKey (key) && b.indexOfItemId (p.getIntValue (key)) >= 0) b.setSelectedId (p.getIntValue (key), juce::dontSendNotification); };
        num ("sens", sens); num ("strength", strength); num ("surround", surround); num ("weight", weight); num ("amount", amount); num ("residual", residual);
        id ("method", methodBox); id ("bands", bandsBox); id ("dir", dirBox); id ("scale", scaleBox);
        tab = juce::jlimit (0, (int) kTabCount - 1, p.getIntValue (prefKey ("tab"), kTabTouch)); lightTab();
        if (p.containsKey (prefKey ("multi"))) multiBtn.setToggleState (p.getBoolValue (prefKey ("multi")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("link"))) linkBtn.setToggleState (p.getBoolValue (prefKey ("link")), juce::dontSendNotification);
        if (p.containsKey (prefKey ("roll"))) view.setRoll (p.getDoubleValue (prefKey ("roll")));
        const int sc = scaleBox.getSelectedId();
        if (sc == 6) view.setMel(); else { static const double w[] = { 0.0, 0.25, 0.5, 0.75, 1.0 }; view.setScale (w[juce::jlimit (0, 4, sc - 1)]); }
    }
    void saveControls()
    {
        auto& p = uiPrefs();
        p.setValue (prefKey ("sens"), sens.getValue()); p.setValue (prefKey ("strength"), strength.getValue()); p.setValue (prefKey ("surround"), surround.getValue()); p.setValue (prefKey ("weight"), weight.getValue());
        p.setValue (prefKey ("amount"), amount.getValue()); p.setValue (prefKey ("residual"), residual.getValue());
        p.setValue (prefKey ("method"), methodBox.getSelectedId()); p.setValue (prefKey ("bands"), bandsBox.getSelectedId()); p.setValue (prefKey ("dir"), dirBox.getSelectedId());
        p.setValue (prefKey ("scale"), scaleBox.getSelectedId()); p.setValue (prefKey ("tab"), tab);
        p.setValue (prefKey ("multi"), multiBtn.getToggleState()); p.setValue (prefKey ("link"), linkBtn.getToggleState()); p.setValue (prefKey ("roll"), view.rollValue());
    }
    double seconds() const { return (double) length / rate; }
    EditDef* edit() const { return app.project.findEdit (eid); }
    bool isPlayingNow() const { return app.isPlaying() && app.playInfo.kind == AppContext::PlayInfo::Kind::Edit && app.playInfo.id == eid; }
    void tick()
    {
        const bool playing = isPlayingNow();
        const double pos = playing ? juce::jmax (0.0, app.playheadSeconds()) : -1.0;
        view.setPlayhead (pos);
        playBtn.setButtonText (playing ? "Stop" : "Play");                     // (the picture on the button turns into a stop square)
        // the page turns with the playhead: when it reaches the right edge of the page it was on, the next page comes (until the end of the edit)
        if (playing && ! loopBtn.getToggleState() && lastTickPos >= view.visT0() && lastTickPos < view.visT1()
            && pos >= view.visT1() - 0.01 * view.pageSpan() && view.visT1() < seconds() - 1.0e-6)
            view.showFrom (view.visT1());
        lastTickPos = pos;
    }
    struct Ticker : juce::Timer { NoiseModeDialog* owner = nullptr; void timerCallback() override { if (owner != nullptr) owner->tick(); } } ticker;
    bool consonantMethod() const { return methodBox.getSelectedId() == 3; }
    bool lookingAcross() const { return dirBox.getSelectedId() != 2 || consonantMethod(); }
    void updateSurround() { view.setSurround (built && view.hasSelection() && ! consonantMethod(), surround.getValue(), dirBox.getSelectedId() - 1, dirBox.getSelectedId() != 2 ? weight.getValue() / 100.0 : 0.5); }
    void refreshControls()
    {
        for (auto* c : tabControls (tab)) c->setVisible (true);                    // (the rules below hide what the chosen method does not use)
        updateSurround();
        const bool ready = built && ! running;
        const bool cons = consonantMethod();
        const bool att = methodBox.getSelectedId() == 1 || cons;
        for (auto* c : std::initializer_list<juce::Component*> { &dirBox, &surround, &surroundCap }) c->setVisible (! cons);      // a stray consonant: always the sound before / after, its own search length
        weightCap.setText (cons ? "Side (50 = auto)" : "Before <-> After", juce::dontSendNotification);
        bandsBox.setVisible (att); multiBtn.setVisible (att); linkBtn.setVisible (att);
        weight.setVisible (lookingAcross()); weightCap.setVisible (lookingAcross());
        for (int t = 0; t < kTabCount; ++t) if (t != tab) for (auto* c : tabControls (t)) c->setVisible (false);      // only the tab that is showing
        for (auto* b : std::initializer_list<juce::Button*> { &startBtn, &rewBtn, &playBtn, &fwdBtn, &endBtn, &loopBtn }) b->setEnabled (ready);
        clock.setEnabled (ready);
        gainBtn.setEnabled (ready && view.hasSelection()); retouchBtn.setEnabled (ready && view.hasSelection()); declickBtn.setEnabled (ready);
        learnBtn.setEnabled (ready && view.hasSelection()); denoiseBtn.setEnabled (ready && view.hasSelection() && haveRoomTone());
        revertBtn.setEnabled (ready && ! steps.empty()); finishBtn.setEnabled (ready && ! steps.empty());
        roomCap.setText (roomShortText(), juce::dontSendNotification); roomCap.setTooltip (roomText());
        juce::String t;
        if (view.hasSelection()) t << "Box: " << juce::String (view.selT0(), 2) << " - " << juce::String (view.selT1(), 2) << " s, " << (int) view.selF0() << " - " << (int) view.selF1() << " Hz.  ";
        t << juce::String ((int) steps.size()) << " fix(es) not yet written back";
        info.setText (t, juce::dontSendNotification);
        resized();                                                               // (what is shown decides how the controls are packed)
    }
    // ---- the picture: the pyramid (built once), slices of it for what is in view, and the sharp picture of a zoomed-in view read from the files
    void buildPicture()
    {
        pyramid = std::make_shared<SpectralPyramid>();
        auto py = pyramid; auto parts = cur; const double sr = rate; const double secs = seconds();
        status.setText ("Building the picture of the whole edit (" + juce::String (secs / 60.0, 1) + " min, " + juce::String ((int) parts->ids.size()) + " tracks)... you can wait, or close and come back.", juce::dontSendNotification);
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        pyramidBusy = true;
        runJob ([py, parts, sr, secs] (AudioJob& j) -> juce::String
                {
                    if (parts->ids.empty()) return "There is no audio in this edit.";
                    py->setUp (sr, secs);
                    const int frames = py->levels[0].frames;
                    ReaderCache cache;
                    for (size_t i = 0; i < parts->parts.size(); ++i)
                    {
                        const auto& pp = parts->parts[i];
                        const bool ok = py->addTrack (0, frames, [&pp, &cache] (juce::int64 start, int n, float* dst) { readTrackMix (pp, start, n, dst, &cache); }, i == 0,
                                                      [&j, &i, &parts] { j.setProgress ((float) i / (float) parts->parts.size()); return j.cancelled(); });
                        if (! ok) return "Cancelled.";
                    }
                    py->settle();
                    return {};
                },
                [self] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->pyramidBusy = false;
                    if (err.isNotEmpty()) { self->status.setText (err, juce::dontSendNotification); return; }
                    self->built = true;
                    self->sliceForView();
                    self->status.setText ("Ready. Space plays from the red playhead (or the left of the page) to the end of the edit, and the page turns by itself (arrows zoom: right / left in time, down / up in pitch; click the ruler to place the playhead, drag it to move, double-click it for the whole edit). "
                                          "Draw a box and press G (gain), R (ReTouch), D (de-click) or N (denoise, with the project's room tone - or draw a box over room only and press L first). Each fix goes into the edit at once; Undo takes it off; Write back keeps them.", juce::dontSendNotification);
                    self->refreshControls();
                });
    }
    /** The quick picture for what is in view: a slice of the best level of the pyramid (the whole picture is far too big to hand over at once). */
    void sliceForView()
    {
        if (pyramidBusy || pyramid == nullptr || ! pyramid->valid()) return;         // (busy first: while the job builds the pyramid its levels are being filled)
        const double t0 = juce::jmax (0.0, view.visT0()), t1 = juce::jmin (seconds(), view.visT1());
        if (t1 <= t0) return;
        double a = 0.0, b = 0.0;
        auto s = pyramid->slice (t0, t1, 2400, a, b);
        view.setDataRange (std::move (s), a, b, seconds());
    }
    /** Makes the quick picture again over [t0, t1] seconds (after a fix): the frames of that stretch, every track, read from the pieces as they are now. */
    void refreshPicture (double t0, double t1)
    {
        if (pyramid == nullptr || ! pyramid->valid()) return;
        auto py = pyramid; auto parts = cur;
        const int f0 = juce::jmax (0, py->frameAt (t0 - 1.0)), f1 = juce::jmin (py->levels[0].frames, py->frameAt (t1 + 1.0) + 1);
        pyramidBusy = true;
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        runJob ([py, parts, f0, f1] (AudioJob& j) -> juce::String
                {
                    ReaderCache cache;
                    for (size_t i = 0; i < parts->parts.size(); ++i)
                    {
                        const auto& pp = parts->parts[i];
                        if (! py->addTrack (f0, f1, [&pp, &cache] (juce::int64 start, int n, float* dst) { readTrackMix (pp, start, n, dst, &cache); }, i == 0, [&j] { return j.cancelled(); })) return "Cancelled.";
                    }
                    py->settle (f0, f1);
                    return {};
                },
                [self] (const juce::String&)
                {
                    if (self == nullptr) return;
                    self->pyramidBusy = false;
                    self->sliceForView(); self->view.invalidateDetail();
                    self->refreshControls();
                });
        refreshControls();                                                   // (the fixes and Undo wait until the picture is made again)
    }
    void startDetail (const PictureRequest& rq)
    {
        if (! built || cur == nullptr) return;
        if (rq.t1 - rq.t0 > kDetailMaxSeconds) return;                               // zoomed far out: the quick picture is all there is (the sharp one would read the whole edit)
        if (detailCancel != nullptr) detailCancel->store (true);
        auto cancel = std::make_shared<std::atomic<bool>> (false); detailCancel = cancel;
        auto parts = cur; const double sr = rate; juce::Component::SafePointer<NoiseModeDialog> self (this);
        {   // every track of the view is read into memory: a long view of many tracks is left to the quick picture (36 tracks of 3 minutes at 96 kHz were 2.5 GB)
            const double samples = (rq.t1 - rq.t0 + 1.0) * sr * (double) parts->parts.size();
            if (samples * sizeof (float) > 512.0 * 1024.0 * 1024.0) return;
        }
        std::thread ([parts, rq, sr, cancel, self]
        {
            try                                                                  // (nothing may escape a thread: out of memory leaves the quick picture, nothing worse)
            {
                const double margin = 0.5;
                const juce::int64 s0 = juce::jmax<juce::int64> (0, (juce::int64) std::llround ((rq.t0 - margin) * sr)), s1 = (juce::int64) std::llround ((rq.t1 + margin) * sr);
                std::vector<std::vector<float>> tracks (parts->parts.size());
                ReaderCache cache;
                for (size_t i = 0; i < parts->parts.size(); ++i)
                {
                    if (cancel->load()) return;                                  // (a newer view, or the window closed: let go before reading more)
                    tracks[i].assign ((size_t) juce::jmax<juce::int64> (0, s1 - s0), 0.0f);
                    readTrackMix (parts->parts[i], s0, s1 - s0, tracks[i].data(), &cache);
                }
                const double off = (double) s0 / sr;
                auto pic = picture::render (tracks, sr, rq.t0 - off, rq.t1 - off, rq.w, rq.h, rq.axis, rq.opt, [cancel] { return cancel->load(); });
                pic.id = rq.id; pic.t0 = rq.t0; pic.t1 = rq.t1;
                if (cancel->load()) return;
                juce::MessageManager::callAsync ([self, p = std::make_shared<DetailPicture> (std::move (pic))] { if (self != nullptr) self->view.setDetail (std::move (*p)); });
            }
            catch (...) {}
        }).detach();
    }
    void openSpectroSettings()
    {
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        launchDialog (std::make_unique<SpectroSettingsDialog> (view.settings(), [self] (const SpectroSettings& n)
        {
            if (self == nullptr) return;
            self->view.setSettings (n); n.save (gSpecPrefs);
            if (gSpecPrefs != nullptr) gSpecPrefs->saveIfNeeded();
        }), "Spectrogram settings", this);
    }
    // ---- playing: from the playhead to the end of the whole edit (the page turns with it); Loop repeats the page in view
    /** Where the playhead is: the running one, else where Play would start. */
    double transportPos() const { return isPlayingNow() ? juce::jmax (0.0, app.playheadSeconds()) : view.startPoint(); }
    void playRange (double from)
    {
        const bool loop = loopBtn.getToggleState();
        from = juce::jlimit (0.0, seconds(), from);
        if (loop) { if (from < view.visT0() || from >= view.visT1()) from = view.visT0(); }
        else if (from >= seconds() - 0.01) { from = 0.0; view.placeCue (0.0); view.showTime (0.0); }        // (nothing left to play from the very end: start again)
        lastPlayTo = loop ? juce::jmin (seconds(), view.visT1()) : seconds();
        lastTickPos = from;
        const auto err = app.playEdit (eid, from, lastPlayTo, loop);
        if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification);
    }
    void playPressed() { if (! built || running) return; app.stopPlayback(); playRange (view.startPoint()); }
    /** The playhead goes to t (the page turns to show it); if it was playing it carries on from there. */
    void goTo (double t)
    {
        if (! built || running) return;
        t = juce::jlimit (0.0, seconds(), t);
        const bool wasPlaying = isPlayingNow();
        view.placeCue (t); view.showTime (t);
        if (wasPlaying)
        {
            app.stopPlayback();
            if (t < seconds() - 0.01) playRange (t);
        }
        clock.setPosition (t);
    }
    void jumpBy (double seconds_) { goTo (transportPos() + seconds_); }
    /** The edit changed under a running playback (a fix went in or came out): it starts again from where it was. */
    void restartIfPlaying()
    {
        if (! isPlayingNow()) return;
        const double pos = app.playheadSeconds();
        app.stopPlayback();
        if (loopBtn.getToggleState()) { const auto err = app.playEdit (eid, view.startPoint(), lastPlayTo, true); if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification); }
        else if (pos < lastPlayTo) { const auto err = app.playEdit (eid, pos, lastPlayTo, false); if (err.isNotEmpty()) status.setText (err, juce::dontSendNotification); }
    }
    // ---- the fixes: each is made on the job thread from the pieces as they are now, then put into the edit (the next one works on the result)
    using Prepare = std::function<juce::String (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j)>;
    void runFix (const juce::String& label, const juce::String& suffix, double t0, double t1, Prepare prepare)
    {
        auto* e = edit(); if (e == nullptr || running || ! built) return;
        auto regions = std::make_shared<std::vector<EditRegion>> (e->regions);
        auto jobs = std::make_shared<std::vector<RegionJob>>();
        status.setText (label + "...", juce::dontSendNotification);
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        refreshControls();
        for (auto* b : { &gainBtn, &retouchBtn, &declickBtn, &learnBtn, &denoiseBtn, &revertBtn, &finishBtn }) b->setEnabled (false);
        runJob ([regions, jobs, prepare] (AudioJob& j) { return prepare (*regions, *jobs, j); },
                [self, regions, jobs, label, suffix, t0, t1] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    auto deleteFiles = [jobs] { for (auto& rj : *jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs->clear(); };
                    if (err.isNotEmpty()) { deleteFiles(); self->status.setText (err, juce::dontSendNotification); self->refreshControls(); return; }
                    auto* ed = self->edit();
                    if (ed == nullptr) { deleteFiles(); return; }
                    Step st; st.before = ed->regions; st.jobs = std::move (*jobs); st.label = label; st.t0 = t0; st.t1 = t1;
                    if (! applyRegionJobs (*ed, st.before, st.jobs, suffix))
                    {
                        ed->regions = st.before; for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
                        self->status.setText ("The fix could not be put into the edit (" + gApplyWhy + ").", juce::dontSendNotification); self->refreshControls(); return;
                    }
                    self->steps.push_back (std::move (st));
                    self->app.project.changed();
                    self->cur = std::make_shared<TrackParts> (buildTrackParts (*ed, 0, self->length, self->only));
                    self->restartIfPlaying();
                    self->status.setText (label + " done and in the edit (" + juce::String ((int) self->steps.size()) + " fix(es) not yet written back). Play to listen; Undo takes it off; Write back to clip keeps it.", juce::dontSendNotification);
                    self->refreshControls();
                    self->refreshPicture (t0, t1);
                },
                [jobs] (const juce::String&) { for (auto& rj : *jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs->clear(); });   // (the window was closed meanwhile: its new files go)
    }
    FixSpec repairSpec (bool declick) const
    {
        FixSpec spec;
        if (declick) { spec.kind = FixSpec::Kind::Declick; spec.sensitivity = sens.getValue(); return spec; }
        spec.kind = FixSpec::Kind::Patch; spec.f0 = view.selF0(); spec.f1 = view.selF1(); spec.sensitivity = sens.getValue();
        spec.repair.mode = methodBox.getSelectedId() == 2 ? RepairMode::Replace : RepairMode::Attenuate;
        spec.repair.consonant = consonantMethod();
        spec.repair.bands = bandsBox.getSelectedId(); spec.repair.multiRes = multiBtn.getToggleState(); spec.repair.linkChannels = linkBtn.getToggleState();
        spec.repair.strength = strength.getValue() / 100.0;
        spec.repair.direction = spec.repair.consonant ? RepairDirection::LeftRight : (RepairDirection) (dirBox.getSelectedId() - 1);
        spec.repair.contextPct = surround.getValue(); spec.repair.weighting = weight.getValue() / 100.0;
        return spec;
    }
    void retouch (bool declick)
    {
        if (! declick && ! view.hasSelection()) return;
        juce::int64 t0 = 0, t1 = length;
        if (view.hasSelection()) { t0 = (juce::int64) std::llround (view.selT0() * rate); t1 = (juce::int64) std::llround (view.selT1() * rate); }
        const auto spec = repairSpec (declick);
        const RepairOp op { spec, t0, t1 };
        const auto folder = fixFolder; const double sr = rate; const auto len = length; const auto onlyT = only;
        runFix (declick ? "De-click" : "ReTouch", declick ? "declick" : "repair", (double) t0 / rate, (double) t1 / rate,
                [folder, sr, len, onlyT, op] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j) { return prepareRepairJobs (folder, regions, 0, len, sr, { op }, onlyT, jobs, j); });
    }
    void openGain()
    {
        if (! built || running || ! view.hasSelection()) return;
        struct GainPop : public juce::Component
        {
            juce::Slider sl; juce::TextButton apply { "Apply" }, cancel { "Cancel" }; juce::Label cap;
            std::function<void (double)> onApply;
            GainPop (double start)
            {
                cap.setText ("Gain of the box", juce::dontSendNotification); cap.setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (cap);
                sl.setSliderStyle (juce::Slider::LinearHorizontal); sl.setRange (-60.0, 18.0, 0.1); sl.setValue (start, juce::dontSendNotification);
                sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22); sl.setDoubleClickReturnValue (true, 0.0);
                sl.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : (v > 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };
                sl.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.retainCharacters ("-+0123456789.").getDoubleValue(); };
                sl.setSkewFactorFromMidPoint (-6.0);
                addAndMakeVisible (sl); addAndMakeVisible (apply); addAndMakeVisible (cancel);
                apply.setColour (juce::TextButton::buttonColourId, theme::accent);
                apply.onClick = [this] { const double v = sl.getValue(); auto cb = onApply; if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); if (cb) cb (v); };
                cancel.onClick = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
                setSize (380, 92);
            }
            void resized() override
            {
                auto r = getLocalBounds().reduced (8);
                cap.setBounds (r.removeFromTop (20)); sl.setBounds (r.removeFromTop (28)); r.removeFromTop (6);
                cancel.setBounds (r.removeFromRight (80)); r.removeFromRight (6); apply.setBounds (r.removeFromRight (80));
            }
        };
        auto pop = std::make_unique<GainPop> (uiPrefs().getDoubleValue (prefKey ("gainDb"), -6.0));
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        pop->onApply = [self] (double db)
        {
            if (self == nullptr || ! self->view.hasSelection()) return;
            uiPrefs().setValue (self->prefKey ("gainDb"), db);
            if (std::abs (db) < 0.05) { self->status.setText ("Gain 0 dB: nothing to change.", juce::dontSendNotification); return; }
            auto spec = self->repairSpec (false);
            spec.repair.mode = RepairMode::Gain; spec.repair.gainDb = db <= -59.95 ? -200.0 : db; spec.repair.strength = 1.0; spec.repair.consonant = false;
            const juce::int64 t0 = (juce::int64) std::llround (self->view.selT0() * self->rate), t1 = (juce::int64) std::llround (self->view.selT1() * self->rate);
            const RepairOp op { spec, t0, t1 };
            const auto folder = self->fixFolder; const double sr = self->rate; const auto len = self->length; const auto onlyT = self->only;
            self->runFix ("Gain", "gain", (double) t0 / sr, (double) t1 / sr,
                          [folder, sr, len, onlyT, op] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j) { return prepareRepairJobs (folder, regions, 0, len, sr, { op }, onlyT, jobs, j); });
        };
        juce::CallOutBox::launchAsynchronously (std::move (pop), gainBtn.getScreenBounds(), nullptr);
    }
    bool haveRoomTone() const { return rtBoxT1 > rtBoxT0 || projectRT->valid(); }
    /** The same in a few words, for the little caption in the Denoise box (the full sentence is its tooltip). */
    juce::String roomShortText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (projectRT->valid()) return "Room tone: the project's";
        return "Room tone: none yet";
    }
    juce::String roomText() const
    {
        if (rtBoxT1 > rtBoxT0) return "Room tone: the box at " + juce::String (rtBoxT0, 2) + " - " + juce::String (rtBoxT1, 2) + " s";
        if (projectRT->valid()) return "Room tone: the project's (" + projectRT->source + ")";
        return "Room tone: none yet - draw a box over room only and press L, or use the edit window's Room tone button";
    }
    /** L: the box is the room tone (learned from the files under it when a denoise is made). */
    void roomFromBox()
    {
        if (! built || running || ! view.hasSelection()) return;
        rtBoxT0 = view.selT0(); rtBoxT1 = view.selT1();
        if (rtBoxT1 - rtBoxT0 < 0.5) { status.setText ("The room-tone box is too short: make it at least half a second long (a second or more is better).", juce::dontSendNotification); rtBoxT0 = rtBoxT1 = -1.0; refreshControls(); return; }     // (what learnDenoise needs: a shorter box was taken here, then refused at Write back)
        rtParts = cur;
        status.setText (roomText() + ". Now draw a box round the noise to take out and press Denoise (N).", juce::dontSendNotification);
        refreshControls();
    }
    /** N: the box denoised - its time span and pitch range, every track - with the room tone, straight into the edit. */
    void denoise()
    {
        if (! built || running || ! view.hasSelection() || ! haveRoomTone() || cur == nullptr) return;
        FixSpec spec; spec.kind = FixSpec::Kind::Denoise; spec.f0 = view.selF0(); spec.f1 = view.selF1();
        spec.dn.amountDb = amount.getValue(); spec.dn.residualDb = residual.getValue();
        spec.dnBoxT0 = rtBoxT1 > rtBoxT0 ? rtBoxT0 : -1.0; spec.dnBoxT1 = rtBoxT1 > rtBoxT0 ? rtBoxT1 : -1.0;
        const juce::int64 t0 = (juce::int64) std::llround (view.selT0() * rate), t1 = (juce::int64) std::llround (view.selT1() * rate);
        const RepairOp op { spec, t0, t1 };
        const auto folder = fixFolder; const double sr = rate; const auto len = length; const auto onlyT = only; auto prt = projectRT;
        auto parts = rtBoxT1 > rtBoxT0 && rtParts != nullptr ? rtParts : cur;      // (the room-tone box is learned from the sound as it was when it was marked)
        runFix ("Denoise", "denoise", (double) t0 / rate, (double) t1 / rate,
                [folder, sr, len, onlyT, op, parts, prt] (const std::vector<EditRegion>& regions, std::vector<RegionJob>& jobs, AudioJob& j)
                {
                    // the jobs are made (the files not yet): their room-tone profiles first, then the files
                    std::vector<RepairOp> ops { op };
                    auto err = prepareRepairJobs (folder, regions, 0, len, sr, ops, onlyT, jobs, j, false);
                    if (err.isNotEmpty()) return err;
                    err = attachRoomTone (jobs, ops, regions, *parts, prt, sr, j);
                    if (err.isNotEmpty()) { for (auto& rj : jobs) for (auto& r : rj.results) r.file.deleteFile(); jobs.clear(); return err; }
                    return runRepairJobs (jobs, op.spec, j);
                });
    }
    void revert()
    {
        auto* e = edit(); if (e == nullptr || steps.empty() || running) return;
        auto st = std::move (steps.back()); steps.pop_back();
        e->regions = st.before;
        for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
        app.project.changed();
        cur = std::make_shared<TrackParts> (buildTrackParts (*e, 0, length, only));
        if (rtParts != nullptr)                                                 // the room-tone box marked on the sound of this fix: its files are gone, so the sound as it is now
        {
            bool gone = false;
            for (auto& tps : rtParts->parts) for (auto& pt : tps) for (auto& rj : st.jobs) for (auto& r : rj.results) gone = gone || pt.file == r.file;
            if (gone) rtParts = cur;
        }
        restartIfPlaying();
        status.setText (st.label + " taken off again (" + juce::String ((int) steps.size()) + " fix(es) left).", juce::dontSendNotification);
        refreshPicture (st.t0, st.t1);
    }
    /** Write back to clip: what has been done stays in the edit (one Undo fix step back to 'base'); the window carries on. */
    void writeBack()
    {
        if (running || ! keepSteps()) return;
        status.setText ("Written back: the fixes are in the edit (Undo fix in the edit window brings back the audio as it was). Carry on, or press Finished.", juce::dontSendNotification);
        refreshControls();
    }
    bool keepSteps()
    {
        auto* e = edit(); if (e == nullptr || steps.empty()) return false;
        for (auto& st : steps) for (auto& rj : st.jobs) rj.results.clear();
        setUndoForEdit (app, eid, base, "noise mode (" + juce::String ((int) steps.size()) + " fixes)");
        base = e->regions; steps.clear();
        app.project.changed();
        return true;
    }
public:
    /** Closing (the X, Escape or Finished) with fixes not yet written back asks first: keep them (as Write back to clip does), throw them away, or stay
        (they used to go without a word - Escape is easily pressed). False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (steps.empty()) return true;
        const int n = (int) steps.size();
        juce::Component::SafePointer<NoiseModeDialog> self (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Noise mode")
                                          .withMessage (juce::String (n) + (n == 1 ? " fix has" : " fixes have") + " not been written back to the clip yet. Keep "
                                                        + (n == 1 ? "it" : "them") + " in the edit, or throw " + (n == 1 ? "it" : "them") + " away?")
                                          .withButton (n == 1 ? "Keep it" : "Keep them").withButton (n == 1 ? "Throw it away" : "Throw them away").withButton ("Cancel"),
                                      [self] (int r)
                                      {
                                          if (self == nullptr || r == 0) return;
                                          if (r == 1) self->keepSteps();                  // (a fix still being made when the window closes is not kept)
                                          closeDialogOf (self.getComponent());            // (the ones not kept are thrown away as it closes)
                                      });
        return false;
    }
private:
    void discardUncommitted()
    {
        if (steps.empty()) return;
        if (auto* e = edit()) { e->regions = base; app.project.changed(); }
        for (auto& st : steps) for (auto& rj : st.jobs) for (auto& r : rj.results) r.file.deleteFile();
        steps.clear();
    }
    static constexpr double kDetailMaxSeconds = 180.0;
    AppContext& app; juce::Uuid eid; double rate = 48000.0; juce::int64 length = 0; std::vector<juce::Uuid> only; juce::File fixFolder;
    InfoNote intro; juce::Label info, sensCap, strengthCap, surroundCap, weightCap, amountCap, residualCap, roomCap;
    SpectrogramView view; juce::ComboBox methodBox, dirBox, bandsBox, scaleBox; juce::ToggleButton multiBtn { "Multi-resolution" }, linkBtn { "Link stereo" };
    juce::TextButton specBtn { "Spectrogram settings..." };
    juce::Slider sens, strength, surround, weight, amount, residual;
    IconButton startBtn { "Start" }, rewBtn { "Rewind" }, playBtn { "Play" }, fwdBtn { "Forward" }, endBtn { "End" };
    juce::ToggleButton loopBtn { "Loop" };
    TimeReadout clock;
    double lastTickPos = -1.0;                                          // where the playhead was at the last tick (for the page turn)
    juce::TextButton gainBtn { "Gain [G]" }, retouchBtn { "ReTouch [R]" }, declickBtn { "De-click [D]" }, learnBtn { "Room tone [L]" }, denoiseBtn { "Denoise [N]" };
    juce::TextButton revertBtn { "Undo" }, finishBtn { "Write back to clip" }, doneBtn { "Finished de-noising" };
    std::shared_ptr<std::atomic<bool>> detailCancel;
    std::shared_ptr<SpectralPyramid> pyramid; bool pyramidBusy = false, built = false;
    std::shared_ptr<TrackParts> cur;                                    // the tracks and their pieces as the edit is now (replaced after every fix)
    std::shared_ptr<TrackParts> rtParts;                                // ... as they were when the room-tone box was marked (L): it is learned from that sound - once the
                                                                        // room tone itself had been denoised, a profile learned from it again was some 12 dB too low
    std::vector<EditRegion> base;                                       // the edit as it was when the window opened, or at the last Write back
    std::vector<Step> steps;                                            // the fixes since, each with the state before it (for Undo)
    std::shared_ptr<const Project::RoomTone> projectRT;                 // the project's room tone as it was when the window opened
    double rtBoxT0 = -1.0, rtBoxT1 = -1.0;                              // a box of the picture marked as the room tone (seconds), else the project's
    double lastPlayTo = 0.0;
};

void noiseModeEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Noise mode", parent, [ap = &app, eid] (juce::Component* p) { noiseModeEdit (*ap, eid, p); })) return;
    const juce::String nm = "Noise mode";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    juce::int64 end = 0; for (auto& r : e->regions) end = juce::jmax (end, r.endSample());
    if (end < (juce::int64) (1.0 * rate)) { say (nm, "There is no audio in this edit yet."); return; }
    const auto only = e->fixTracks;
    const auto tp = buildTrackParts (*e, 0, end, only);
    if (tp.ids.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the edit."); return; }
    const juce::String what = juce::String (only.empty() ? "All " : "The ") + juce::String ((int) tp.ids.size()) + " tracks of the whole edit (" + juce::String ((double) end / rate / 60.0, 1) + " min, " + juce::String (tp.pieces) + " pieces) in one picture, built once at several zoom levels. "
        "Gain, ReTouch, De-click and Denoise work on the box you draw (as in the ReTouch window); Denoise uses the project's room tone (the edit window's Room tone button), or a box of this picture marked with L. "
        "Every fix goes into the edit the moment it is made, as new piece files (the originals are never touched), and the next fix works on the result; Play hears them at once. "
        "Play carries on from the red playhead to the end of the edit and the page turns by itself (Loop repeats the page you can see); the transport buttons, the clock (double-click to type a time) and the Home / End keys work as in the edit window. "
        "Undo takes the last one off. Write back to clip keeps what has been done (one Undo fix step in the edit window) and the window stays open; Finished closes it (if some fixes are not written back yet, it asks whether to keep them).";
    gSpecPrefs = app.props.getUserSettings();
    auto dlg = std::make_unique<NoiseModeDialog> (app, eid, rate, end, only, what);
    juce::Component::SafePointer<NoiseModeDialog> sp (dlg.get());
    launchDialog (std::move (dlg), nm, parent, true, [sp] { return sp == nullptr || sp->mayClose(); });
}

// ======================================================================================================== Re-HarmoniSer (edit window)
class ReHarmoniserDialog : public JobDialogBase
{
public:
    ReHarmoniserDialog()
    {
        addAndMakeVisible (editor);
        int w = 1280, h = 800;
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) { w = juce::jmin (w, disp->userArea.getWidth() - 40); h = juce::jmin (h, disp->userArea.getHeight() - 90); }
        setSize (juce::jmax (900, w), juce::jmax (620, h));
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ReHarmoniserDialog> (this)] { if (safe != nullptr) safe->editor.grabKeyboardFocus(); });
    }
    ~ReHarmoniserDialog() override
    {
        if (onClosing) onClosing();
    }
    void resized() override { editor.setBounds (getLocalBounds()); }
    void start (AudioJob::Work w, std::function<void (const juce::String&)> d) { runJob (std::move (w), std::move (d)); }
    void closeMe() { closeDialogOf (this); }
    /** Closing (Cancel, the X or Escape) with corrected notes that have not been written back asks first, as Spectral Repair does (3.0.2: Escape threw them
        away without a word). False when it has asked: the answer closes the window. */
    bool mayClose()
    {
        if (isWrittenBack != nullptr && isWrittenBack()) return true;
        const int n = (int) editor.editList().size();
        if (n == 0) return true;
        juce::Component::SafePointer<ReHarmoniserDialog> self (this);
        confirmAsync ("ReHarmoniSer", juce::String (n) + (n == 1 ? " corrected note has" : " corrected notes have") + " not been written back yet. Close the window and throw "
                      + (n == 1 ? "it" : "them") + " away? (Write back to clip keeps " + (n == 1 ? "it" : "them") + ".)",
                      n == 1 ? "Throw it away" : "Throw them away", [self] { if (self != nullptr) self->closeMe(); });
        return false;
    }
    std::function<bool()> isWrittenBack;                                // set by openReharmoniser: the corrections went into the edit (or this stream is done)
    juce::String jobText = "Working";                                   // what the progress bar in the editor says while a job runs
    void timerCallback() override { JobDialogBase::timerCallback(); editor.setProgress ((double) job.progress(), jobText); }
    void jobEnded (const juce::String&) override { editor.setProgress (-2.0); }
    rehar::ReHarmoniserEditor editor;
    std::function<void()> onClosing;
};

static void openReharmoniser (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void reharmoniserEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "The ReHarmoniSer", parent, [ap = &app, eid] (juce::Component* p) { reharmoniserEdit (*ap, eid, p); })) return;
    const juce::String nm = "ReHarmoniSer";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (app.engine.isRecording()) { say (nm, "Stop recording first."); return; }
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (nm, "First mark the area with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. Mark the whole stretch that has the notes to correct, with a little music round it (at least 2 seconds is best)."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 lastEnd = 0; int pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) { ++pieces; lastEnd = juce::jmax (lastEnd, r.endSample()); }
    if (pieces == 0) { say (nm, "There is no audio between the marks."); return; }
    const auto from = tIn, to = juce::jmin (tOut, lastEnd);
    if (to - from < (juce::int64) (0.5 * rate)) { say (nm, "The marked area is too short (at least about half a second, and at least 2 seconds is best)."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, from, to, outS, inS, nm);
    if (sm < 0) return;
    if (sm == 0) { openReharmoniser (app, eid, parent, from, to, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, nm, "reharmoniser");
    openReharmoniser (app, eid, parent, from, to, outS, pair, 0);
    openReharmoniser (app, eid, parent, from, to, inS, pair, 1);
    pair->focus (0);
}

static void openReharmoniser (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 from, juce::int64 to, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    const juce::String nm = "ReHarmoniSer";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto only = e->fixTracks;
    struct Part { juce::File file; juce::int64 fileStart = 0; size_t reg = 0; };       // a file of a region that is heard between the marks
    std::vector<juce::Uuid> trackIds; std::vector<std::vector<Part>> parts;
    auto regs = std::make_shared<std::vector<EditRegion>>();                              // the regions heard between the marks (their fades can reach a little past their ends)
    {
        for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && (! ss.on || ss.has (r.id)))
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                if (std::find (trackIds.begin(), trackIds.end(), f.trackId) == trackIds.end()) { trackIds.push_back (f.trackId); parts.emplace_back(); }
            }
        if (ss.on) { for (auto& r : e->regions) if (r.endSample() > from && r.startSample < to && ss.has (r.id)) regs->push_back (r); }          // a stream: only its own pieces
        else for (auto& r : e->regions) if (r.fadeOutFinish() > from && r.fadeInBegin() < to) regs->push_back (r);
        for (size_t ri = 0; ri < regs->size(); ++ri)
            for (auto& f : (*regs)[ri].files)
            {
                const auto ti = std::find (trackIds.begin(), trackIds.end(), f.trackId);
                if (ti == trackIds.end()) continue;
                parts[(size_t) (ti - trackIds.begin())].push_back ({ f.file, f.fileStart, ri });
            }
    }
    if (trackIds.empty()) { say (nm, "None of the tracks you marked with Ctrl + Alt + drag are in the marked area."); if (pair != nullptr) pair->closeAll(); return; }

    // what the window and the glue share
    struct State
    {
        std::vector<EditRegion> base;                       // the edit as it was when the window opened
        std::vector<RegionJob> jobs; PreviewApplied applied; bool accepted = false;
        std::vector<int> chBase, chCount;                   // for each track: where its channels start in the block, and how many it has
        std::vector<juce::Uuid> trackIds;
    };
    auto st = std::make_shared<State>(); st->applied.app = &app;
    st->base = e->regions; st->trackIds = trackIds;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto total = to - from;
    auto dlg = std::make_unique<ReHarmoniserDialog>();
    auto* d = dlg.get();
    juce::Component::SafePointer<ReHarmoniserDialog> self (d);
    app.undoHold += 1;
    auto discardFiles = [st] { for (auto& rj : st->jobs) for (auto& r : rj.results) r.file.deleteFile(); st->jobs.clear(); };
    auto unapply = [&app, eid, st, discardFiles, pair, slot]
    {
        if (st->applied) { if (pair != nullptr) pair->put (slot, false); else if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->base; app.project.changed(); } }
        st->applied = false; discardFiles();
    };
    // The corrected audio is only put into the edit while it is being played (so it goes through the edit's mixer); it is taken out again when playback stops,
    // and it stays in the edit for good only when Write back to clip is pressed.
    auto putIn = [&app, eid, st, pair, slot]() -> bool
    {
        if (st->jobs.empty() || st->applied) return true;
        if (pair != nullptr) { st->applied = true; if (pair->put (slot, true)) return true; st->applied = false; return false; }
        auto* e2 = app.project.findEdit (eid); if (e2 == nullptr) return false;
        if (! applyRegionJobs (*e2, st->base, st->jobs, "reharmonised")) { e2->regions = st->base; app.project.changed(); return false; }
        st->applied = true; app.project.changed();
        return true;
    };
    auto takeOut = [&app, eid, st, pair, slot]
    {
        if (! st->applied) return;
        if (pair != nullptr) { st->applied = false; pair->put (slot, false); return; }
        if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->base; app.project.changed(); }
        st->applied = false;
    };
    d->onClosing = [&app, st, unapply]
    {
        app.stopPlayback();
        if (! st->accepted) unapply();
        app.undoHold -= 1;
    };
    d->setWantsKeyboardFocus (false);

    // ---- the editor's host
    auto& h = d->editor.host;
    h.log = [] (const juce::String& s) { juce::Logger::writeToLog (s); };
    h.spaceIsTaken = [] { return spaceIsForRecording(); };           // (as in every other window: the Space that stops a take does nothing else)
    h.loadSetting = [] (const juce::String& k) { return uiPrefs().getValue ("reharmoniser." + k); };
    h.saveSetting = [] (const juce::String& k, const juce::String& v) { uiPrefs().setValue ("reharmoniser." + k, v); };
    d->editor.restoreViewSettings();
    h.play = [&app, eid, from, rate, nm, putIn] (double x, double y, bool loop)
    {
        if (! putIn()) { say (nm, "The corrected audio does not fit the edit any more (" + gApplyWhy + ")."); return; }
        const auto err = app.playEdit (eid, (double) from / rate + x, (double) from / rate + y, loop);
        if (err.isNotEmpty()) say (nm, err);
        app.engine.setPlaybackLooping (loop);
    };
    h.stop = [&app, takeOut] { app.stopPlayback(); takeOut(); };
    h.playPosition = [&app, eid, from, rate, takeOut]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) { takeOut(); return -1.0; }
        return juce::jmax (0.0, app.playheadSeconds() - (double) from / rate);
    };
    h.playTone = [&app, eid, nm] (double hz, double levelDb)         // a piano key: a short tone, to the outputs the edit's mixer uses
    {
        const double sr = app.engine.getSampleRate();
        if (sr < 8000.0 || hz < 20.0 || hz > 8000.0) return;
        const int n = (int) (sr * 1.3);
        std::vector<float> ch ((size_t) n);
        const double amp = std::pow (10.0, juce::jlimit (-60.0, 0.0, levelDb) / 20.0) / 1.3;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double env = juce::jmin (1.0, t / 0.02, juce::jmax (0.0, (1.3 - t) / 0.25));
            ch[(size_t) i] = (float) (amp * env * (std::sin (juce::MathConstants<double>::twoPi * hz * t) + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 2.0 * hz * t)));
        }
        static int toneCounter = 0;
        toneCounter = (toneCounter + 1) % 12;
        const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto file = tmpDir.getChildFile ("fermata-key-tone-" + juce::String (toneCounter) + ".wav");
        app.stopPlayback();
        file.deleteFile();                                           // (a WAV writer adds to a file that is already there, so an old one must go first)
        if (! audioops::writeWav (file, { ch, ch }, sr)) return;
        int outFirst = 0;
        if (auto* mx = app.project.mixerOfEdit (eid))
        {
            auto outOf = [&] (const juce::Uuid& busId) { for (auto& b : mx->busPool) if (b->busId == busId) return b->outFirst.load(); return -1; };
            int f = -1;
            for (auto& b : app.project.buses) if (b.external && mx->busFor (b.id)->isMain.get()) { f = outOf (b.id); if (f >= 0) break; }     // a main output first
            if (f < 0) for (auto& b : app.project.buses) if (b.external) { f = outOf (b.id); if (f >= 0) break; }
            if (f >= 0) outFirst = f;
        }
        const auto err = app.playRenders ({ { file, 0.0, sr, (juce::int64) n } }, 0.0, -1.0, juce::Uuid::null(), outFirst);
        if (err.isNotEmpty()) say (nm, err);
    };
    h.cancel = [self] { if (self != nullptr && self->mayClose()) self->closeMe(); };         // (asks first when corrected notes would be thrown away)
    d->isWrittenBack = [st] { return st->accepted; };
    h.leaveUnchangedOk = pair != nullptr;
    h.writeBack = [&app, eid, st, self, putIn, pair, slot]
    {
        if (self == nullptr) return;
        if (pair != nullptr)
        {
            // an OUT / IN stream window: this stream is ready (or left unchanged when nothing was corrected); both go into the edit together when both are ready
            app.stopPlayback();
            if (st->applied) { st->applied = false; pair->put (slot, false); }
            st->accepted = true;                                     // (the corrected files wait in the pair; this window goes away and the other stream is next)
            pair->wrote (slot, [] { return true; });
            return;
        }
        if (st->jobs.empty()) { self->editor.setStatus ("Nothing to write back.", true); return; }
        app.stopPlayback();
        if (! putIn()) { self->editor.setStatus ("The corrected audio does not fit the edit any more (" + gApplyWhy + ").", true); return; }
        for (auto& rj : st->jobs) rj.results.clear();              // the new files are kept now
        st->jobs.clear(); st->applied = false; st->accepted = true;
        setUndoForEdit (app, eid, st->base, "reharmoniser");
        app.project.changed();
        self->closeMe();
    };
    h.setCorrected = [&app, eid, st, self, unapply, fixFolder, from, to, rate, ss, pair] (std::shared_ptr<const rehar::Correction> corr, std::function<void (const juce::String&)> done)
    {
        if (self == nullptr) return;
        app.stopPlayback();
        unapply();
        if (corr == nullptr) { done ({}); return; }
        // Each piece of the edit that the marks touch is made again from its OWN files: every track of that piece is loaded together with up to 1 s of
        // untouched sound either side (the part of the take that its crossfades use), the corrections are applied to that, and the new files are written.
        // So the takes on either side of an edit point each get the pitch change for their own sound, and the edit points and their fades stay as they were.
        struct Piece { RegionJob rj; juce::int64 editStart = 0, srcIn = 0, A = 0, B = 0; bool rawBefore = false, rawAfter = false; std::vector<int> base, cnt; };
        auto pieces = std::make_shared<std::vector<Piece>>();
        const auto h1 = (juce::int64) (1.0 * rate);
        FixSpec spec; spec.kind = FixSpec::Kind::Reharmonise;
        int nch = 0; for (int c : st->chCount) nch += c;
        juce::int64 prevHi = from;
        for (auto& r : st->base)
        {
            if (r.endSample() <= from || r.startSample >= to) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto r0 = juce::jmax (from, r.startSample), r1 = juce::jmin (to, r.endSample());
            if (r1 - r0 < 2) continue;
            juce::int64 lo = r0, hi = r1;                                     // a stream also corrects the part of its take past the edit point (its crossfade uses it)
            if (ss.on) { ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            Piece p; p.editStart = r.startSample; p.srcIn = r.srcIn;
            const auto a = r.srcIn + (r0 - r.startSample), b = r.srcIn + (r1 - r.startSample);
            const auto fa = juce::jmin (a, r.srcIn + (lo - r.startSample)), fb = juce::jmax (b, r.srcIn + (hi - r.startSample));
            if (! prepRegionJob (fixFolder, r, p.rj, a, b, fa - h1, fb + h1, a, b, spec)) continue;
            // the stretch [A, B) that every file of the piece can give. A file can be a little shorter than the region says (the recording stopped a few
            // samples early): the piece then ends where the files end, and what is beyond stays exactly as it was.
            juce::int64 A = fa - h1, B = fb + h1; bool ok = true;
            for (size_t k = 0; k < p.rj.src.size(); ++k)
            {
                auto rd = audioops::openReader (formats(), p.rj.src[k]);
                if (rd == nullptr) { ok = false; break; }
                A = juce::jmax (A, p.rj.fileStart[k]); B = juce::jmin (B, p.rj.fileStart[k] + (juce::int64) rd->lengthInSamples);
            }
            const auto a2 = juce::jmax (a, A), b2 = juce::jmin (b, B);
            if (! ok || b2 - a2 < 2) continue;
            // a piece the corrections cannot reach is left exactly as it is (no new files, no new joins)
            if (! rehar::correctionReaches (*corr, (double) ((r.startSample + (A - r.srcIn)) - from) / rate, (double) ((r.startSample + (B - r.srcIn)) - from) / rate)) continue;
            p.rj.a = a2; p.rj.b = b2; p.rj.fixFrom = a2; p.rj.fixTo = b2; p.rj.outFrom = A; p.rj.outTo = B; p.A = A; p.B = B;
            p.rawBefore = a2 > r.srcIn; p.rawAfter = b2 < r.srcOut;           // a join made here (not an edit point): outside the marks it must be exactly the original
            for (size_t k = 0; k < p.rj.idx.size(); ++k)
            {
                const auto& f = r.files[p.rj.idx[k]];
                int bi = -1, ci = 1;
                for (size_t t = 0; t < st->trackIds.size(); ++t) if (st->trackIds[t] == f.trackId) { bi = st->chBase[t]; ci = st->chCount[t]; break; }
                p.base.push_back (bi); p.cnt.push_back (ci);
            }
            pieces->push_back (std::move (p));
        }
        if (pieces->empty()) { done ("Nothing to write in this area."); return; }
        self->jobText = "Making the corrected audio";
        self->start ([pieces, corr, from, nch, rate, fixFolder] (AudioJob& j) -> juce::String
        {
            fixFolder.createDirectory();
            auto cleanup = [&] { for (auto& p : *pieces) { for (auto& r : p.rj.results) r.file.deleteFile(); p.rj.results.clear(); } };
            for (size_t pi = 0; pi < pieces->size(); ++pi)
            {
                auto& p = (*pieces)[pi]; auto& rj = p.rj; rj.results.clear();
                const auto n = p.B - p.A;
                if (n < 16) { cleanup(); return "Nothing to process in this piece."; }
                if (j.cancelled()) { cleanup(); return "Cancelled."; }
                // the piece's own audio, every track together (what the corrections work on)
                rehar::Block blk ((size_t) nch, std::vector<float> ((size_t) n, 0.0f));
                std::vector<std::vector<std::vector<float>>> raw (rj.src.size());
                std::vector<double> fileRate (rj.src.size(), rate);
                for (size_t k = 0; k < rj.src.size(); ++k)
                {
                    auto rd = audioops::openReader (formats(), rj.src[k]);
                    if (rd == nullptr) { cleanup(); return "Cannot read " + rj.src[k].getFileName(); }
                    fileRate[k] = rd->sampleRate;
                    if (! audioops::readRange (*rd, p.A - rj.fileStart[k], n, raw[k])) { cleanup(); return "Cannot read " + rj.src[k].getFileName(); }
                    if (p.base[k] >= 0)
                        for (size_t c = 0; c < raw[k].size(); ++c)
                        {
                            auto& dstv = blk[(size_t) p.base[k] + juce::jmin (c, (size_t) p.cnt[k] - 1)];
                            for (size_t q = 0; q < raw[k][c].size() && q < dstv.size(); ++q) dstv[q] = raw[k][c][q];
                        }
                    if (p.base[k] >= 0) raw[k].clear();                       // (the corrected channels are written, not these)
                }
                const size_t nBefore = (size_t) (p.rj.a - p.A), nAfter = (size_t) (p.B - p.rj.b);
                rehar::Block keepBefore, keepAfter;
                if (p.rawBefore) { keepBefore.assign (blk.size(), {}); for (size_t c = 0; c < blk.size(); ++c) keepBefore[c].assign (blk[c].begin(), blk[c].begin() + (long) nBefore); }
                if (p.rawAfter)  { keepAfter.assign (blk.size(), {});  for (size_t c = 0; c < blk.size(); ++c) keepAfter[c].assign (blk[c].end() - (long) nAfter, blk[c].end()); }
                rehar::Hooks hk; hk.cancelled = [&j] { return j.cancelled(); };
                hk.progress = [&j, pi, np = pieces->size()] (const char*, double f) { j.setProgress ((float) (((double) pi + 0.9 * f) / (double) np)); };
                try { rehar::renderCorrection (*corr, blk, (int) std::lround (rate), (double) ((p.editStart + (p.A - p.srcIn)) - from) / rate, hk); }
                catch (const rehar::Cancelled&) { cleanup(); return "Cancelled."; }
                for (auto& chn : blk) if ((juce::int64) chn.size() != n) { cleanup(); return "The corrected audio is not exactly the same length as the original, so it was not used."; }
                if (p.rawBefore) for (size_t c = 0; c < blk.size(); ++c) std::copy (keepBefore[c].begin(), keepBefore[c].end(), blk[c].begin());
                if (p.rawAfter)  for (size_t c = 0; c < blk.size(); ++c) std::copy (keepAfter[c].begin(), keepAfter[c].end(), blk[c].end() - (long) nAfter);
                for (size_t k = 0; k < rj.src.size(); ++k)
                {
                    std::vector<std::vector<float>> ch = std::move (raw[k]);
                    if (p.base[k] >= 0)
                    {
                        auto rd = audioops::openReader (formats(), rj.src[k]);
                        const size_t nc = rd != nullptr ? (size_t) rd->numChannels : (size_t) 1;
                        ch.assign (nc, {});
                        for (size_t c = 0; c < nc; ++c) ch[c] = blk[(size_t) p.base[k] + juce::jmin (c, (size_t) p.cnt[k] - 1)];
                    }
                    if (! audioops::writeWav (rj.dest[k], ch, fileRate[k])) { cleanup(); return "Cannot write " + rj.dest[k].getFileName(); }
                    audioops::PieceResult res; res.file = rj.dest[k]; res.from = p.A; res.to = p.B; res.ok = true;
                    rj.results.push_back (res);
                }
            }
            return {};
        },
        [&app, eid, st, self, pieces, done, pair] (const juce::String& err)
        {
            if (self == nullptr) return;
            if (err.isNotEmpty())
            {
                for (auto& p : *pieces) for (auto& r : p.rj.results) r.file.deleteFile();
                done (err == "Cancelled." ? err : "Could not write the corrected audio: " + err); return;
            }
            auto* e2 = app.project.findEdit (eid);
            if (e2 == nullptr) { done ("The edit is gone."); return; }
            st->jobs.clear(); for (auto& p : *pieces) st->jobs.push_back (p.rj);
            if (pair == nullptr) e2->regions = st->base;                 // (the edit itself is not changed here: the corrected audio goes in when it is played, and for good at Write back to clip)
            st->applied = false;
            done ({});
        });
    };

    // ---- load the audio (every channel of every track, between the marks), then give it to the editor
    auto loaded = std::make_shared<rehar::Block>();
    st->chBase.assign (parts.size(), 0); st->chCount.assign (parts.size(), 1);
    d->editor.setStatus ("Reading the audio...");
    d->jobText = "Reading the audio";
    d->start ([parts, regs, from, to, total, st, loaded, ss] (AudioJob& j) -> juce::String
    {
        int nch = 0;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            int cnt = 1;
            for (auto& pt : parts[i]) { auto r = audioops::openReader (formats(), pt.file); if (r != nullptr) { cnt = (int) r->numChannels; break; } }
            st->chBase[i] = nch; st->chCount[i] = juce::jmax (1, cnt); nch += st->chCount[i];
        }
        loaded->assign ((size_t) nch, std::vector<float> ((size_t) total, 0.0f));
        // what is loaded is what is heard: each region's own fades (the crossfades between the takes) are applied, so the picture and the pitch
        // measurements see the join as it sounds. (Volume changes are not applied: the files themselves are corrected, and those play on top.)
        std::vector<std::vector<float>> gains (regs->size());
        std::vector<juce::int64> gainFrom (regs->size(), 0);
        juce::int64 prevHi = from;
        for (size_t ri = 0; ri < regs->size(); ++ri)
        {
            const auto& r = (*regs)[ri];
            if (ss.on)                                     // a stream: its own take as it is in the files (no fades, nothing of the other take), the out stream carrying on past the edit, the in stream starting before it
            {
                juce::int64 lo = 0, hi = 0; ss.span (r, from, to, lo, hi); lo = juce::jmax (lo, prevHi);
                if (hi <= lo) continue;
                prevHi = hi; gainFrom[ri] = lo; gains[ri].assign ((size_t) (hi - lo), 1.0f);
                continue;
            }
            const auto t0 = juce::jmax (from, r.fadeInBegin()), t1 = juce::jmin (to, r.fadeOutFinish());
            if (t1 <= t0) continue;
            gainFrom[ri] = t0; gains[ri].resize ((size_t) (t1 - t0));
            for (juce::int64 t = t0; t < t1; ++t) gains[ri][(size_t) (t - t0)] = r.gainAt (t);
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (j.cancelled()) return "Cancelled.";
            j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, parts.size()));
            for (auto& pt : parts[i])
            {
                const auto& r = (*regs)[pt.reg]; const auto& g = gains[pt.reg];
                if (g.empty()) continue;
                auto rd = audioops::openReader (formats(), pt.file); if (rd == nullptr) continue;
                const auto t0 = gainFrom[pt.reg];
                juce::int64 rel = (r.srcIn + (t0 - r.startSample)) - pt.fileStart, n = (juce::int64) g.size(), dst = t0 - from, gi = 0;
                if (rel < 0) { n += rel; dst -= rel; gi -= rel; rel = 0; }                       // before the start of the file: silence
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*rd, rel, n, ch);
                for (size_t c = 0; c < ch.size(); ++c)
                {
                    auto& out = (*loaded)[(size_t) st->chBase[i] + juce::jmin (c, (size_t) st->chCount[i] - 1)];
                    for (size_t k = 0; k < ch[c].size() && dst + (juce::int64) k < total; ++k) out[(size_t) dst + k] += ch[c][k] * g[(size_t) (gi + (juce::int64) k)];
                }
            }
        }
        return {};
    },
    [&app, st, self, loaded, rate] (const juce::String& err)
    {
        if (self == nullptr) return;
        if (err.isNotEmpty()) { self->editor.setStatus (err, true); return; }
        juce::StringArray names;                                    // for the See mixer: the track names (L / R when a track has two channels)
        for (size_t i = 0; i < st->trackIds.size(); ++i)
        {
            const auto* tr = app.project.findTrack (st->trackIds[i]);
            const auto tn = tr != nullptr ? tr->name : juce::String ("Track " + juce::String ((int) i + 1));
            const int cnt = i < st->chCount.size() ? st->chCount[i] : 1;
            for (int c = 0; c < cnt; ++c) names.add (cnt > 1 ? tn + (cnt == 2 ? (c == 0 ? " L" : " R") : " " + juce::String (c + 1)) : tn);
        }
        self->editor.setChannelNames (names);
        self->editor.setAudio (loaded, (int) std::lround (rate));
    });
    if (pair != nullptr)
    {
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "reharmonised";
        launchStream (pair, slot, std::move (dlg), nm + " - " + ss.label, parent);
    }
    else launchDialog (std::move (dlg), nm, parent, true, [self] { return self == nullptr || self->mayClose(); });      // Escape and the X ask first too
}

// ---- the pitch curve (draw the pitch against time, audition it, accept or revert)
struct EditCurveState { std::vector<RegionJob> jobs; std::vector<EditRegion> old; PreviewApplied applied; };

static void openPitchCurve (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 tIn, juce::int64 tOut, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot);

void pitchCurveEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "The pitch curve", parent, [ap = &app, eid] (juce::Component* p) { pitchCurveEdit (*ap, eid, p); })) return;
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (app.engine.isRecording()) { say ("Pitch curve", "Stop recording first."); return; }
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say ("Pitch curve", "First mark the part with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key. The pitch line is drawn between the two marks."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) ++pieces;
    if (pieces == 0) { say ("Pitch curve", "There is no audio between the marks."); return; }
    StreamSel outS, inS;
    const int sm = detectStreams (*e, tIn, tOut, outS, inS, "Pitch curve");
    if (sm < 0) return;
    if (sm == 0) { openPitchCurve (app, eid, parent, tIn, tOut, StreamSel(), nullptr, -1); return; }
    auto pair = makePair (app, eid, "Pitch curve", "pitch curve");
    openPitchCurve (app, eid, parent, tIn, tOut, outS, pair, 0);
    openPitchCurve (app, eid, parent, tIn, tOut, inS, pair, 1);
    pair->focus (0);
}

static void openPitchCurve (AppContext& app, const juce::Uuid& eid, juce::Component* parent, juce::int64 tIn, juce::int64 tOut, const StreamSel& ss, std::shared_ptr<StreamPair> pair, int slot)
{
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    juce::int64 pieces = 0;
    for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut && (! ss.on || ss.has (r.id))) ++pieces;
    const double fixIn = e->fixIn, fixOut = e->fixOut;
    CurveContext c; c.allowPad = true; c.rate = rate; c.length = tOut - tIn;
    const juce::String lead = ss.on ? ss.label + ": the sound of the " + (slot == 0 ? "take that plays up to the edit point, carried on past it" : "take that plays from the edit point, started before it")
                                      + ", on its own. Draw its line here, the other stream's line in the other window, then Accept in BOTH; the edit point and its fades stay exactly where they are. " : juce::String();
    c.what = lead + "Draw how the pitch should change along the audio between marks 1 and 2 (" + juce::String (fixOut - fixIn, 2) + " s, " + juce::String (pieces) + " piece(s)), on " + (e->fixTracks.empty() ? juce::String ("every track") : juce::String ((int) e->fixTracks.size()) + " chosen track(s)") + ". "
             "The middle line is no change; up raises the pitch (+100 cents at the top), down lowers it. Press Preview (key R) to hear it, then Accept or Revert. "
             "The original files are never touched.";
    auto st = std::make_shared<EditCurveState>(); st->applied.app = &app;
    const auto fixFolder = app.project.audioFolder().getChildFile ("Fixes");
    const auto regionsCopy = std::make_shared<std::vector<EditRegion>> (e->regions);
    const auto only = e->fixTracks;
    c.load = [regionsCopy, tIn, tOut, only, ss] (AudioJob& j)
    {
        std::vector<float> mix ((size_t) (tOut - tIn), 0.0f);
        juce::int64 prevHi = tIn;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut || r.files.empty()) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            if (j.cancelled()) break;
            juce::int64 from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            if (ss.on) { ss.span (r, tIn, tOut, from, to); from = juce::jmax (from, prevHi); if (to > from) prevHi = to; }      // a stream: its own take, carried on past the edit (out) / started before it (in)
            if (to <= from) continue;
            const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            for (auto& f : r.files)
            {
                if (! only.empty() && std::find (only.begin(), only.end(), f.trackId) == only.end()) continue;
                auto rd = audioops::openReader (formats(), f.file); if (rd == nullptr) continue;
                juce::int64 rel = a - f.fileStart, n = b - a, dst = from - tIn;
                if (rel < 0) { n += rel; dst -= rel; rel = 0; }
                if (n <= 0) continue;
                std::vector<std::vector<float>> ch; audioops::readRange (*rd, rel, n, ch);
                for (auto& x : ch) for (size_t k = 0; k < x.size() && (size_t) dst + k < mix.size(); ++k) mix[(size_t) dst + k] += x[k];
            }
        }
        std::vector<std::vector<float>> out; out.push_back (std::move (mix)); return out;
    };
    c.prepare = [fixFolder, regionsCopy, tIn, tOut, rate, st, only, ss] (const FixSpec& spec, double padSec, AudioJob& j) -> juce::String
    {
        st->jobs.clear();
        const auto pad = (juce::int64) std::llround (padSec * rate);
        juce::int64 prevHi = tIn;
        for (auto& r : *regionsCopy)
        {
            if (r.endSample() <= tIn || r.startSample >= tOut) continue;
            if (ss.on && ! ss.has (r.id)) continue;
            const auto from = juce::jmax (tIn, r.startSample), to = juce::jmin (tOut, r.endSample());
            juce::int64 lo = from, hi = to;                                  // a stream also corrects the part of its take past the edit point, which its crossfade uses
            if (ss.on) { ss.span (r, tIn, tOut, lo, hi); lo = juce::jmax (lo, prevHi); if (hi > lo) prevHi = hi; }
            if (to <= from) continue;
            lo = juce::jmin (lo, from); hi = juce::jmax (hi, to);
            FixSpec sj = spec; sj.curveZero = (double) pad - (double) (lo - tIn);          // curve time 0 is mark 1, wherever this piece starts
            RegionJob rj; const auto a = r.srcIn + (from - r.startSample), b = r.srcIn + (to - r.startSample);
            const auto fa = r.srcIn + (lo - r.startSample), fb = r.srcIn + (hi - r.startSample);
            prepRegionJob (fixFolder, r, rj, a, b, fa - pad, fb + pad, fa - pad, fb + pad, sj, only);
            rj.curveZero = sj.curveZero;
            st->jobs.push_back (std::move (rj));
        }
        for (size_t i = 0; i < st->jobs.size(); ++i)
        {
            FixSpec sj = spec; sj.curveZero = st->jobs[i].curveZero;
            const auto err = runRegionJob (st->jobs[i], sj, j, (float) i / (float) st->jobs.size(), (float) (i + 1) / (float) st->jobs.size());
            if (err.isNotEmpty()) { for (auto& done : st->jobs) for (auto& r : done.results) r.file.deleteFile(); st->jobs.clear(); return err; }
        }
        return {};
    };
    auto deleteFiles = [st] { for (auto& j : st->jobs) for (auto& r : j.results) r.file.deleteFile(); st->jobs.clear(); };
    if (pair != nullptr)
    {
        pair->jobsOf[slot] = [st] { return &st->jobs; };
        pair->suffix[slot] = "pitch curve";
        c.apply = [pair, slot, st] { if (st->jobs.empty()) return false; st->applied = true; return pair->put (slot, true); };
        c.revert = [pair, slot, st, deleteFiles] { if (st->applied) { st->applied = false; pair->put (slot, false); } deleteFiles(); };
        c.unapply = [pair, slot, st] { if (! st->applied) return; st->applied = false; pair->put (slot, false); };
        c.accept = [] {};
        c.pairWritten = [pair, slot] (std::function<bool()> commit) { pair->wrote (slot, std::move (commit)); };
    }
    else
    {
        c.apply = [&app, eid, st, deleteFiles]
        {
            auto* e2 = app.project.findEdit (eid); if (e2 == nullptr || st->jobs.empty()) { deleteFiles(); return false; }
            st->old = e2->regions;
            const bool allOk = applyRegionJobs (*e2, st->old, st->jobs, "pitch curve");
            if (! allOk) { e2->regions = st->old; deleteFiles(); return false; }
            st->applied = true; app.project.changed();
            return true;
        };
        c.revert = [&app, eid, st, deleteFiles]
        {
            if (st->applied) { if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); } st->applied = false; }
            deleteFiles();
        };
        c.unapply = [&app, eid, st]
        {
            if (! st->applied) return;
            if (auto* e2 = app.project.findEdit (eid)) { e2->regions = st->old; app.project.changed(); }
            st->applied = false;                                  // (the corrected files are kept, ready to be put back)
        };
        c.accept = [&app, eid, st] { st->jobs.clear(); st->applied = false; setUndoForEdit (app, eid, st->old, "pitch curve"); app.project.changed(); };
    }
    c.play = [&app, eid, fixIn, fixOut] (double fromSec, double toSec, bool loop)
    {
        const auto err = app.playEdit (eid, fixIn + fromSec, juce::jmin (fixOut, fixIn + toSec), loop);          // the part in view (inside the marked part), nothing either side
        if (err.isNotEmpty()) say ("Pitch curve", err);
    };
    c.setLooping = [&app] (bool on) { app.engine.setPlaybackLooping (on); };
    c.position = [&app, eid, fixIn]
    {
        if (! app.isPlaying() || app.playInfo.kind != AppContext::PlayInfo::Kind::Edit || app.playInfo.id != eid) return -1.0;
        return juce::jmax (0.0, app.playheadSeconds() - fixIn);
    };
    c.stop = [&app] { app.stopPlayback(); };
    c.hold = [&app] (bool on) { app.undoHold += on ? 1 : -1; };
    auto dlg = withPitchTabs (std::make_unique<CurveDialog> (std::move (c)), pair != nullptr);
    if (pair != nullptr) launchStream (pair, slot, std::move (dlg), "Pitch - " + ss.label, parent);
    else launchDialog (std::move (dlg), "Pitch", parent, true);
}

/** The Pitch button of the edit window: Pitch curve first, Pitch process on the second tab. */
void pitchTabsEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Pitch correction", parent, [ap = &app, eid] (juce::Component* p) { pitchTabsEdit (*ap, eid, p); })) return;
    gProcessPage.reset(); gMakeProcessPage = true; pitchEdit (app, eid, parent); gMakeProcessPage = false;
    if (gProcessPage == nullptr) return;                                   // (a message was shown: nothing is marked)
    pitchCurveEdit (app, eid, parent);
    gProcessPage.reset();
}

void undoLastFix (AppContext& app)
{
    if (! app.fixUndo) { say ("Undo fix", "There is no pitch correction, repair or denoise to undo."); return; }
    if (! app.fixUndoStillValid())
    {
        // the edit has moved on since the fix: putting its pieces back as they were before the fix would take those changes with it (3.1.4)
        const auto label = app.fixUndoLabel;
        app.fixUndo = nullptr;
        say ("Undo fix", "It has changed since the " + label + ", so Undo fix cannot take just that out any more. Ctrl+Z steps back through the changes, the " + label + " among them.");
        return;
    }
    auto f = std::move (app.fixUndo); app.fixUndo = nullptr;
    f();
}

// ======================================================================================================== copy the whole project / save as
/** Shows the progress of copying a project (every file is copied, then read back and compared). */
class CopyProjectDialog : public JobDialogBase
{
public:
    CopyProjectDialog (AppContext& a, juce::File dest, juce::String name, bool audio, bool switchWhenDone)
        : app (a), destFolder (std::move (dest)), newName (std::move (name)), copyAudio (audio), saveAs (switchWhenDone)
    {
        setSize (560, 190);
        title.setText (juce::String (saveAs ? "Save as: " : "Copy of the whole project: ") + destFolder.getFullPathName(), juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, theme::text); title.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (title);
        addAndMakeVisible (cancel); addChildComponent (reveal);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        reveal.onClick = [this] { report.projectFile.getParentDirectory().revealToUser(); };
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText ("Starting...", juce::dontSendNotification);
        app.saveNow();
        auto root = app.project.toVar();                       // a snapshot: what is copied is the project as it is now
        const auto src = app.project.projectFolder();
        juce::Component::SafePointer<CopyProjectDialog> self (this);
        auto text = std::make_shared<std::pair<juce::CriticalSection, juce::String>>();
        shared = text;
        auto rep = std::make_shared<projectcopy::Report>();
        runJob ([root, src, dest = destFolder, nm = newName, audio = copyAudio, text, rep] (AudioJob& j) -> juce::String
                {
                    const auto err = projectcopy::run (root, src, dest, nm, audio, [&] (float p, const juce::String& t)
                                                       { j.setProgress (p); { const juce::ScopedLock sl (text->first); text->second = t; } return ! j.cancelled(); }, *rep);
                    return err;
                },
                [self, rep] (const juce::String& err)
                {
                    if (self == nullptr) return;
                    self->report = *rep;
                    self->finished (err);
                });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        title.setBounds (r.removeFromTop (44));
        status.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
        bar.setBounds (r.removeFromTop (22)); r.removeFromTop (12);
        auto row = r.removeFromBottom (30);
        cancel.setBounds (row.removeFromRight (110)); row.removeFromRight (8); reveal.setBounds (row.removeFromRight (150));
    }
private:
    void timerCallback() override
    {
        JobDialogBase::timerCallback();
        if (auto s = shared.lock()) { const juce::ScopedLock sl (s->first); if (running) status.setText (s->second, juce::dontSendNotification); }
    }
    void finished (const juce::String& err)
    {
        cancel.setButtonText ("Close");
        if (err.isNotEmpty())
        {
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. The original project was not touched; the unfinished copy can be deleted.") : err, juce::dontSendNotification);
            status.setBounds (status.getBounds().withHeight (60));
            return;
        }
        juce::String t = "Done: " + juce::String (report.files) + " files, " + juce::File::descriptionOfSizeInBytes (report.bytes) + ", each read back and checked.";
        if (report.missing > 0) t << "  " << juce::String (report.missing) << " file(s) the project refers to could not be found and were not copied.";
        status.setText (t, juce::dontSendNotification);
        status.setBounds (status.getBounds().withHeight (60));
        reveal.setVisible (true);
        if (saveAs) { const auto f = report.projectFile; juce::MessageManager::callAsync ([&a = app, f] { a.openProject (f); }); }     // (the application, not this dialog: it may be gone by then)
    }
    AppContext& app; juce::File destFolder; juce::String newName; bool copyAudio, saveAs;
    projectcopy::Report report;
    std::weak_ptr<std::pair<juce::CriticalSection, juce::String>> shared;
    std::shared_ptr<std::pair<juce::CriticalSection, juce::String>> keep;
    juce::Label title;
    juce::TextButton cancel { "Cancel" }, reveal { "Show the folder" };
};

static std::unique_ptr<juce::FileChooser> copyChooser;

void copyProject (AppContext& app, juce::Component* parent, bool saveAs)
{
    if (heldByRec (app, saveAs ? "Save as (it copies the audio)" : "Making a copy of the project", parent, [ap = &app, saveAs] (juce::Component* p) { copyProject (*ap, p, saveAs); })) return;
    if (app.engine.isRecording()) { say ("Recording", "Stop recording first."); return; }
    auto start = [&]
    {
        juce::File last (app.props.getUserSettings()->getValue ("copyFolder"));
        return last.isDirectory() ? last : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    };
    auto choose = [&app, parent, saveAs, start] (bool copyAudio)
    {
        copyChooser = std::make_unique<juce::FileChooser> (saveAs ? "Save as: choose where, and name the project (a folder with this name is created)"
                                                                  : "Copy the whole project: choose where (an external drive, say), and name the copy (a folder with this name is created)",
                                                           start().getChildFile (app.project.name + (saveAs ? " 2" : " copy") + ".fermata"), "*.fermata");
        copyChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [&app, parent, saveAs, copyAudio] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            const auto name = f.getFileNameWithoutExtension();
            auto folder = f.getParentDirectory().getChildFile (name);
            app.props.getUserSettings()->setValue ("copyFolder", f.getParentDirectory().getFullPathName()); app.props.saveIfNeeded();
            launchDialog (std::make_unique<CopyProjectDialog> (app, folder, name, copyAudio, saveAs), saveAs ? "Save project as" : "Copy the whole project", parent);
        });
    };
    if (! saveAs) { choose (true); return; }
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon).withTitle ("Save as")
                                      .withMessage ("Save this project under a new name and in a new folder.\n\n"
                                                    "Copy everything: all the recordings and bounces are copied too, so the new folder is complete on its own.\n"
                                                    "Project file only: just the project (the edits, mixes and so on) is saved; the audio stays where it is.")
                                      .withButton ("Copy everything").withButton ("Project file only").withButton ("Cancel"),
                                  [choose] (int r) { if (r == 1) choose (true); else if (r == 2) choose (false); });
}

// ======================================================================================================== Export for Processing
/** A small window with a progress bar for a background job; it closes itself when the job has finished fine (after 'onSuccess'), and shows the problem otherwise. */
class SimpleJobDialog : public JobDialogBase
{
public:
    SimpleJobDialog (const juce::String& text, AudioJob::Work work, std::function<void()> onSuccess, std::function<void()> onFail)
    {
        setSize (480, 120);
        status.setColour (juce::Label::textColourId, theme::text);
        status.setText (text, juce::dontSendNotification);
        addAndMakeVisible (cancel);
        cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        runJob (std::move (work), [this, onSuccess, onFail] (const juce::String& err)
        {
            if (err.isEmpty())
            {
                if (onSuccess) onSuccess();
                juce::Component::SafePointer<SimpleJobDialog> sp (this);
                juce::MessageManager::callAsync ([sp] { if (sp != nullptr) closeDialogOf (sp.getComponent()); });
                return;
            }
            if (onFail) onFail();
            status.setColour (juce::Label::textColourId, theme::warn);
            status.setText (err == "Cancelled." ? juce::String ("Cancelled. Nothing was changed.") : err, juce::dontSendNotification);
            cancel.setButtonText ("Close");
        });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        status.setBounds (r.removeFromTop (24)); r.removeFromTop (6);
        bar.setBounds (r.removeFromTop (20)); r.removeFromTop (10);
        cancel.setBounds (r.removeFromBottom (28).removeFromRight (110));
    }
private:
    juce::TextButton cancel { "Cancel" };
};

// ======================================================================================================== the room tone (edit window button)
/** Learns the project's room tone from one source on the job thread: per track, a profile per channel of its file(s) and one for the mono mix. */
static juce::String learnRoomTone (const std::vector<std::pair<juce::Uuid, juce::String>>& tracks, const std::function<void (size_t trackIndex, juce::int64 start, int n, std::vector<std::vector<float>>& out)>& readTrack,
                                   juce::int64 lengthSamples, double rate, const juce::String& source, Project::RoomTone& out, AudioJob& j)
{
    out = Project::RoomTone(); out.source = source;
    const juce::int64 len = juce::jmin (lengthSamples, (juce::int64) (60.0 * rate));          // (a minute says everything)
    if (len < (juce::int64) (0.5 * rate)) return "The room tone is too short (at least half a second, better a few seconds).";
    denoise::Params p;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (j.cancelled()) return "Cancelled.";
        j.setProgress ((float) i / (float) juce::jmax ((size_t) 1, tracks.size()));
        std::vector<std::vector<float>> ch;
        readTrack (i, 0, (int) len, ch);
        if (ch.empty()) continue;
        Project::RoomToneTrack t; t.trackId = tracks[i].first; t.trackName = tracks[i].second;
        std::vector<float> mono (ch[0].size(), 0.0f);
        for (auto& c : ch) { t.perChannel.push_back (denoise::learnProfile ({ c }, rate, p)); for (size_t k = 0; k < c.size() && k < mono.size(); ++k) mono[k] += c[k] / (float) ch.size(); }
        t.mono = denoise::learnProfile ({ mono }, rate, p);
        for (auto& pr : t.perChannel) if (! pr.valid()) return "The room tone is too short for the analysis window.";
        if (t.mono.valid()) out.tracks.push_back (std::move (t));
    }
    if (out.tracks.empty()) return "No audio was found in the room tone.";
    return {};
}

/** A take marked as the room tone, anywhere in the project. */
struct RoomToneCandidate { juce::Uuid windowId, takeId; juce::String text; double in = 0.0, out = 0.0; };
static std::vector<RoomToneCandidate> roomToneCandidates (const Project& project)
{
    std::vector<RoomToneCandidate> v;
    for (auto& w : project.takeWindows)
        for (auto& g : w->groups)
            if (g.roomTone && g.lengthSamples > 0)
            {
                RoomToneCandidate c; c.windowId = w->id; c.takeId = g.id;
                c.in = g.roomToneIn >= 0.0 ? g.roomToneIn : 0.0; c.out = g.roomToneIn >= 0.0 ? juce::jmin (g.lengthSeconds(), g.roomToneOut) : g.lengthSeconds();
                c.text = formatTimeShort (c.in) + " - " + formatTimeShort (c.out) + " of take " + juce::String (g.number) + " of " + w->name + (g.roomToneIn < 0.0 ? " (the whole take)" : "");
                v.push_back (c);
            }
    return v;
}

class RoomToneDialog : public juce::Component
{
public:
    RoomToneDialog (AppContext& a, const juce::Uuid& editId) : app (a), eid (editId)
    {
        setSize (640, 250);
        const auto& rt = app.project.roomTone;
        head.setText (rt.valid() ? "The room tone in use: " + rt.source + " (" + juce::String ((int) rt.tracks.size()) + " tracks). Choose another to replace it."
                                 : "No room tone has been learned yet. The ReTouch and Noise mode windows denoise with it: every track's own noise, learned once from a part of the recording that holds nothing but the room.", juce::dontSendNotification);
        head.setColour (juce::Label::textColourId, theme::text); head.setJustificationType (juce::Justification::topLeft); addAndMakeVisible (head);
        cands = roomToneCandidates (app.project);
        for (size_t i = 0; i < cands.size(); ++i) takeBox.addItem (cands[i].text, (int) i + 1);
        if (! cands.empty()) takeBox.setSelectedId (1, juce::dontSendNotification);
        takeBtn.setButtonText (cands.empty() ? "Use the take marked as the room tone  (none is marked yet)" : "Use the take marked as the room tone:");
        takeBtn.setEnabled (! cands.empty()); takeBox.setEnabled (cands.size() > 1);
        takeBtn.setTooltip ("In a take window, right-click the take that was recorded with nothing but the room in it (or set marks 1 / 2 on the silent part first) and choose \"This whole take is the room tone\" or \"Marks 1 - 2 of this take are the room tone\". One take per take window; it can be in any take window.");
        auto* e = app.project.findEdit (eid);
        const bool marks = e != nullptr && e->fixIn >= 0.0 && e->fixOut > e->fixIn + 0.5;
        marksBtn.setButtonText (marks ? "Use marks 1 - 2 of this edit:  " + formatTimeShort (e->fixIn) + " - " + formatTimeShort (e->fixOut) : "Use marks 1 - 2 of this edit  (set them first)");
        marksBtn.setEnabled (marks);
        marksBtn.setTooltip ("Marks 1 and 2 (keys 1 / 2 at the playhead) round a part of the edit that holds nothing but the room, on every track.");
        laterBtn.setTooltip ("Closes this. Go and mark the room tone - in a take window (right-click a take) or with marks 1 / 2 here - and press the Room tone button again.");
        for (auto* b : { &takeBtn, &marksBtn, &laterBtn }) addAndMakeVisible (b);
        addChildComponent (takeBox); takeBox.setVisible (cands.size() > 1);
        takeBtn.onClick = [this] { chooseTake(); };
        marksBtn.onClick = [this] { chooseMarks(); };
        laterBtn.onClick = [this] { closeDialogOf (this); };
        takeBtn.setColour (juce::TextButton::buttonColourId, theme::accent); marksBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        head.setBounds (r.removeFromTop (70)); r.removeFromTop (10);
        takeBtn.setBounds (r.removeFromTop (34)); r.removeFromTop (4);
        if (takeBox.isVisible()) { takeBox.setBounds (r.removeFromTop (26).withTrimmedLeft (30)); r.removeFromTop (8); } else r.removeFromTop (6);
        marksBtn.setBounds (r.removeFromTop (34)); r.removeFromTop (14);
        laterBtn.setBounds (r.removeFromTop (34));
    }
private:
    void chooseTake()
    {
        const int i = juce::jlimit (0, (int) cands.size() - 1, takeBox.getSelectedId() - 1);
        if (cands.empty()) return;
        const auto c = cands[(size_t) i];
        juce::Component::SafePointer<RoomToneDialog> self (this);
        confirmAsync ("Room tone", "You have selected " + c.text + " as the room tone. Is this correct?", "Yes, learn it", [self, c]
        {
            if (self == nullptr) return;
            auto* w = self->app.project.findTakeWindow (c.windowId); if (w == nullptr) return;
            auto* g = w->findGroup (c.takeId); if (g == nullptr) return;
            std::vector<std::pair<juce::Uuid, juce::String>> tracks; std::vector<juce::File> files;
            for (auto& f : g->files) { tracks.push_back ({ f.trackId, f.trackName }); files.push_back (f.file); }
            const double rate = g->sampleRate > 0 ? g->sampleRate : 48000.0;
            const juce::int64 s0 = (juce::int64) std::llround (c.in * rate), len = (juce::int64) std::llround ((c.out - c.in) * rate);
            self->learn (tracks, [files, s0] (size_t k, juce::int64 start, int n, std::vector<std::vector<float>>& out)
                         { auto r = audioops::openReader (formats(), files[k]); if (r != nullptr) audioops::readRange (*r, s0 + start, n, out); }, len, rate, c.text);
        });
    }
    void chooseMarks()
    {
        auto* e = app.project.findEdit (eid); if (e == nullptr || e->fixIn < 0.0 || e->fixOut <= e->fixIn) return;
        const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
        const auto from = (juce::int64) std::llround (e->fixIn * rate), to = (juce::int64) std::llround (e->fixOut * rate);
        const auto text = formatTimeShort (e->fixIn) + " - " + formatTimeShort (e->fixOut) + " of " + e->name;
        auto tp = std::make_shared<TrackParts> (buildTrackParts (*e, from, to, e->fixTracks));
        if (tp->ids.empty()) { say ("Room tone", "There is no audio between the marks."); return; }
        juce::Component::SafePointer<RoomToneDialog> self (this);
        confirmAsync ("Room tone", "You have selected " + text + " (" + juce::String ((int) tp->ids.size()) + " tracks) as the room tone. Is this correct?", "Yes, learn it", [self, tp, from, to, rate, text]
        {
            if (self == nullptr) return;
            std::vector<std::pair<juce::Uuid, juce::String>> tracks;
            for (size_t i = 0; i < tp->ids.size(); ++i) tracks.push_back ({ tp->ids[i], tp->names[i] });
            // each track's channels over the stretch: the pieces heard there, file by file (a track is one file per piece; the first piece's channel count rules)
            self->learn (tracks, [tp, rate] (size_t i, juce::int64 start, int n, std::vector<std::vector<float>>& out)
            {
                out.clear();
                for (auto& pt : tp->parts[i])
                {
                    const auto p0 = juce::jmax (start, pt.dst), p1 = juce::jmin (start + n, pt.dst + pt.n);
                    if (p1 <= p0) continue;
                    juce::int64 rel = pt.srcA - pt.fileStart + (p0 - pt.dst), cnt = p1 - p0, o = p0 - start;
                    if (rel < 0) { cnt += rel; o -= rel; rel = 0; }
                    if (cnt <= 0) continue;
                    auto r = audioops::openReader (formats(), pt.file); if (r == nullptr) continue;
                    std::vector<std::vector<float>> ch; audioops::readRange (*r, rel, cnt, ch);
                    if (out.empty()) out.assign (ch.size(), std::vector<float> ((size_t) n, 0.0f));
                    for (size_t c = 0; c < ch.size() && c < out.size(); ++c) for (size_t k = 0; k < ch[c].size() && o + (juce::int64) k < n; ++k) out[c][(size_t) o + k] = ch[c][k];
                }
            }, to - from, rate, text);
        });
    }
    void learn (std::vector<std::pair<juce::Uuid, juce::String>> tracks, std::function<void (size_t, juce::int64, int, std::vector<std::vector<float>>&)> read, juce::int64 len, double rate, const juce::String& text)
    {
        auto out = std::make_shared<Project::RoomTone>();
        auto& a = app;
        auto dlg = std::make_unique<SimpleJobDialog> ("Learning the room tone of " + juce::String ((int) tracks.size()) + " tracks...",
                                                     [tracks, read, len, rate, text, out] (AudioJob& j) { return learnRoomTone (tracks, read, len, rate, text, *out, j); },
                                                     [&a, out] { a.project.roomTone = *out; a.project.changed(); say ("Room tone", "The room tone is learned: " + out->source + ", " + juce::String ((int) out->tracks.size()) + " tracks. The ReTouch and Noise mode windows will denoise with it (draw a box and press Denoise). It is kept with the project."); },
                                                     std::function<void()>());
        auto* parent = getParentComponent();
        closeDialogOf (this);
        launchDialog (std::move (dlg), "Room tone", parent);
    }
    AppContext& app; juce::Uuid eid;
    juce::Label head; juce::TextButton takeBtn, marksBtn, laterBtn { "Not now - I'll go and select it" }; juce::ComboBox takeBox;
    std::vector<RoomToneCandidate> cands;
};

void roomToneEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (editOf (app, eid) == nullptr) return;
    launchDialog (std::make_unique<RoomToneDialog> (app, eid), "Room tone", parent);
}

/** Asks for the name of a processing job; 'done' gets the (non-empty) name. */
static void askProcessingName (const juce::String& message, const juce::String& suggestion, std::function<void (const juce::String&)> done)
{
    auto* aw = new juce::AlertWindow ("Export for Processing", message, juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("n", suggestion);
    aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, done] (int res)
    {
        const auto n = aw->getTextEditorContents ("n").trim();
        delete aw;
        if (res == 1 && n.isNotEmpty() && done) done (n);
    }));
}

/** A new folder for the job inside Processing Media, named after the job. */
static juce::File processingJobFolder (AppContext& app, const juce::String& name)
{
    const auto base = app.project.processingFolder();
    base.createDirectory();
    auto f = base.getChildFile (sanitiseForFile (name));
    for (int i = 2; f.exists() && i < 1000; ++i) f = base.getChildFile (sanitiseForFile (name) + " (" + juce::String (i) + ")");
    return f;
}

struct ExportJob { std::vector<juce::File> src, dest; std::vector<juce::int64> fileStart; juce::int64 a = 0, b = 0; juce::File folder; };

static AudioJob::Work exportWork (std::shared_ptr<ExportJob> j)
{
    return [j] (AudioJob& job) -> juce::String
    {
        j->folder.createDirectory();
        for (size_t i = 0; i < j->src.size(); ++i)
        {
            if (job.cancelled()) { j->folder.deleteRecursively(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, j->src.size()));
            juce::String err;
            if (! audioops::exportRange (formats(), j->src[i], j->fileStart[i], j->a, j->b, j->dest[i], err)) { j->folder.deleteRecursively(); return err; }
        }
        return {};
    };
}

static std::shared_ptr<ExportJob> makeExportJob (AppContext& app, const juce::String& name, const std::vector<juce::File>& src, const std::vector<juce::String>& trackNames,
                                                 const std::vector<juce::int64>& fileStart, juce::int64 a, juce::int64 b)
{
    auto j = std::make_shared<ExportJob>();
    j->a = a; j->b = b; j->src = src; j->fileStart = fileStart;
    j->folder = processingJobFolder (app, name);
    for (size_t i = 0; i < src.size(); ++i)
    {
        auto d = audioops::uniqueFile (j->folder.getChildFile (sanitiseForFile (name + " - " + trackNames[i]) + ".wav"));
        for (auto& o : j->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        j->dest.push_back (d);
    }
    return j;
}

static juce::String exportedMessage (const juce::String& name, const juce::File& folder)
{
    return "'" + name + "' is in the folder\n" + folder.getFullPathName() + "\n\nOpen the files in your other software (e.g. iZotope RX), correct them and save OVER the same files, "
           "keeping the same length, sample rate and channels. Then right-click the 'Waiting for corrected audio' block and choose 'Re-link corrected audio'. "
           "Fermata keeps none of these files open, so the other program can overwrite them freely.";
}

/** The piece of region 'rid' that the exported take range [a,b) covers becomes a piece of its own that waits for corrected audio. The exported range may reach past the piece into its handle
    (an OUT / IN stream export): the corrected file then also gives the audio under the crossfade. False if the region is no longer there as it was. */
static bool markWaiting (EditDef& e2, const juce::Uuid& rid, juce::int64 a, juce::int64 b, const juce::String& name, const std::vector<juce::File>& dest, const std::vector<juce::Uuid>& trk, bool subset)
{
    int i = e2.indexOf (rid);
    if (i < 0 || e2.regions[(size_t) i].waiting.active()) return false;
    const auto pa = juce::jmax (a, e2.regions[(size_t) i].srcIn), pb = juce::jmin (b, e2.regions[(size_t) i].srcOut);
    if (pb <= pa) return false;
    const bool cutAfter = pb < e2.regions[(size_t) i].srcOut, cutBefore = pa > e2.regions[(size_t) i].srcIn;
    if (cutAfter)  e2.splitRegion (i, pb - e2.regions[(size_t) i].srcIn);
    if (cutBefore) i = e2.splitRegion (i, pa - e2.regions[(size_t) i].srcIn);
    auto& mid = e2.regions[(size_t) i];
    mid.waiting.name = name; mid.waiting.from = a; mid.waiting.to = b; mid.waiting.files = dest;
    mid.waiting.tracks = subset ? trk : std::vector<juce::Uuid>();
    if (cutBefore) { mid.setInCurve (FadeCurve::Linear); if (i > 0) e2.regions[(size_t) i - 1].setOutCurve (FadeCurve::Linear); }
    if (cutAfter)  { mid.setOutCurve (FadeCurve::Linear); if (i + 1 < (int) e2.regions.size()) e2.regions[(size_t) i + 1].setInCurve (FadeCurve::Linear); }
    if (cutBefore) { mid.fixIn = true; if (i > 0) e2.regions[(size_t) i - 1].fixOut = true; }                 // not edit points
    if (cutAfter)  { mid.fixOut = true; if (i + 1 < (int) e2.regions.size()) e2.regions[(size_t) i + 1].fixIn = true; }
    return true;
}

void exportForProcessingEdit (AppContext& app, const juce::Uuid& eid, juce::Component* parent)
{
    if (heldByRec (app, "Export for Processing", parent, [ap = &app, eid] (juce::Component* p) { exportForProcessingEdit (*ap, eid, p); })) return;
    const juce::String title = "Export for Processing";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    if (e->fixIn < 0.0 || e->fixOut <= e->fixIn) { say (title, "First mark the part to send out with keys 1 (IN) and 2 (OUT) in this window: put the white playhead where you want it and press the key."); return; }
    const double rate = e->sampleRate > 0 ? e->sampleRate : 48000.0;
    const auto tIn = (juce::int64) std::llround (e->fixIn * rate), tOut = (juce::int64) std::llround (e->fixOut * rate);
    // an edit point between the marks: the two takes are sent out separately (the OUT stream and / or the IN stream), each as it is in the file: full volume, no fade
    {
        juce::int64 lastEnd = 0;
        for (auto& r : e->regions) if (r.endSample() > tIn && r.startSample < tOut) lastEnd = juce::jmax (lastEnd, r.endSample());
        StreamSel outS, inS;
        const int sm = lastEnd > tIn ? detectStreams (*e, tIn, juce::jmin (tOut, lastEnd), outS, inS, title) : 0;
        if (sm < 0) return;
        if (sm == 1)
        {
            struct Stream { juce::Uuid rid; juce::int64 a = 0, b = 0; juce::String label; std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart; std::vector<juce::Uuid> trk; };
            auto plan = std::make_shared<std::vector<Stream>>();      // [0] = out, [1] = in (a stream that cannot be sent has an empty 'src')
            plan->resize (2);
            const bool subset = ! e->fixTracks.empty();
            juce::String notes;
            auto fill = [&] (Stream& st, const EditRegion& r, juce::int64 lo, juce::int64 hi, const juce::String& label)
            {
                st.label = label;
                lo = juce::jmax (lo, r.startSample - r.srcIn); if (r.sourceLength > 0) hi = juce::jmin (hi, r.startSample + (r.sourceLength - r.srcIn));
                if (hi - lo < (juce::int64) (0.01 * rate) || r.waiting.active()) return;
                st.rid = r.id; st.a = r.srcIn + (lo - r.startSample); st.b = r.srcIn + (hi - r.startSample);
                for (auto& f : r.files) { if (! e->fixUses (f.trackId)) continue; st.src.push_back (f.file); st.names.push_back (f.trackName); st.fstart.push_back (f.fileStart); st.trk.push_back (f.trackId); }
            };
            const EditRegion* A = nullptr; const EditRegion* B = nullptr;
            for (auto& r : e->regions) { if (r.id == outS.ids.back()) A = &r; if (r.id == inS.ids.front()) B = &r; }
            if (A == nullptr || B == nullptr) return;
            // OUT: from mark 1 to mark 2, or to where it has faded out completely, whichever comes first.  IN: from where its fade-in begins (or mark 1) to mark 2.  No fades are applied to what is sent.
            fill ((*plan)[0], *A, juce::jmax (tIn, A->startSample), juce::jmin (tOut, A->fadeOutFinish()), "Out Stream");
            fill ((*plan)[1], *B, juce::jmax (tIn, B->fadeInBegin()), juce::jmin (tOut, B->endSample()), "In Stream");
            if (outS.ids.size() > 1 && tIn < A->startSample) notes << "\n\nNOTE: the OUT stream before " << formatTimeShort ((double) A->startSample / rate, 3) << " is made of an earlier fix, so only the part from there on is sent.";
            if (inS.ids.size() > 1 && tOut > B->endSample()) notes << "\n\nNOTE: the IN stream is sent up to the end of its piece (" << formatTimeShort ((double) B->endSample() / rate, 3) << "); what follows is made of an earlier fix.";
            const bool haveOut = ! (*plan)[0].src.empty(), haveIn = ! (*plan)[1].src.empty();
            if (! haveOut && ! haveIn) { say (title, "There is nothing to send: the marked part is too short on both sides of the edit point, or the tracks you chose with Ctrl + Alt + drag are not there."); return; }
            auto ask = [&app, eid, plan, notes, parent = juce::Component::SafePointer<juce::Component> (parent), subset, title] (bool wantOut, bool wantIn)
            {
                if (wantOut && (*plan)[0].src.empty()) { say (title, "The OUT stream has nothing to send between the marks."); return; }
                if (wantIn && (*plan)[1].src.empty()) { say (title, "The IN stream has nothing to send between the marks."); return; }
                askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". 'Out Stream' / 'In Stream' is added to the end of it. Each becomes a folder in Processing Media with one file per track, "
                                   "in the format they were recorded, at full volume with no fade applied.", "", [&app, eid, plan, notes, parent, subset, wantOut, wantIn] (const juce::String& name)
                {
                    struct Made { std::shared_ptr<ExportJob> job; juce::Uuid rid; juce::int64 a, b; juce::String full; std::vector<juce::Uuid> trk; };
                    auto made = std::make_shared<std::vector<Made>>();
                    for (int q = 0; q < 2; ++q)
                    {
                        if (! (q == 0 ? wantOut : wantIn)) continue;
                        auto& st = (*plan)[(size_t) q]; const auto full = name + " - " + st.label;
                        made->push_back ({ makeExportJob (app, full, st.src, st.names, st.fstart, st.a, st.b), st.rid, st.a, st.b, full, st.trk });
                    }
                    AudioJob::Work work = [made] (AudioJob& job) -> juce::String { for (auto& m : *made) { const auto err = exportWork (m.job) (job); if (err.isNotEmpty()) { for (auto& o : *made) o.job->folder.deleteRecursively(); return err; } } return {}; };
                    auto onSuccess = [&app, eid, made, subset, notes]
                    {
                        auto* e2 = app.project.findEdit (eid);
                        const auto old = e2 != nullptr ? e2->regions : std::vector<EditRegion>();
                        bool ok = e2 != nullptr;
                        if (ok) for (auto& m : *made) ok = markWaiting (*e2, m.rid, m.a, m.b, m.full, m.job->dest, m.trk, subset) && ok;
                        if (! ok)
                        {
                            if (e2 != nullptr) e2->regions = old;
                            for (auto& m : *made) m.job->folder.deleteRecursively();
                            say ("Export for Processing", "The edit changed while the audio was being exported, so nothing was changed. Please try again."); return;
                        }
                        setUndoForEdit (app, eid, old, "export for processing");
                        app.project.changed();
                        juce::String msg;
                        for (auto& m : *made) msg << "'" << m.full << "' is in the folder\n" << m.job->folder.getFullPathName() << "\n\n";
                        msg << "Open the files in your other software (e.g. iZotope RX), correct them and save OVER the same files, keeping the same length, sample rate and channels. "
                               "Then right-click each 'Waiting for corrected audio' block and choose 'Re-link corrected audio'. Fermata keeps none of these files open, so the other program can overwrite them freely." << notes;
                        say ("Export for Processing", msg);
                    };
                    launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", work, onSuccess, nullptr), "Export for Processing", parent.getComponent());
                });
            };
            // (built by hand: JUCE's message box only shows up to THREE buttons, so with four none of them appeared)
            auto* w = new juce::AlertWindow (title,
                                             "The marked part has an edit point in it. Which take do you want to send out?\n\n"
                                             "OUT stream: the take that plays up to the edit point, from mark 1 until it is completely faded out (or mark 2, whichever comes first).\n"
                                             "IN stream: the take that plays from the edit point, from where its fade-in begins (or mark 1) to mark 2.\n\n"
                                             "Each is sent at full volume with no fade applied, so the edit and its crossfade stay in place and each take can be corrected on its own. 'Both' sends two separate sets of files.",
                                             juce::MessageBoxIconType::QuestionIcon);
            w->addButton ("Out stream", 1, juce::KeyPress ('o'));
            w->addButton ("In stream", 2, juce::KeyPress ('i'));
            w->addButton ("Both", 3, juce::KeyPress ('b'));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([ask, haveOut, haveIn] (int r)
            {
                juce::ignoreUnused (haveOut, haveIn);
                if (r == 1) ask (true, false); else if (r == 2) ask (false, true); else if (r == 3) ask (true, true);
            }), true);
            return;
        }
    }
    const EditRegion* region = nullptr;
    for (auto& r : e->regions) if (tIn >= r.startSample && tIn < r.endSample()) region = &r;
    if (region == nullptr) { say (title, "Mark 1 is not on any piece of audio."); return; }
    if (region->waiting.active()) { say (title, "This piece is already waiting for corrected audio. Re-link it first (right-click the 'Waiting for corrected audio' block)."); return; }
    const auto from = tIn, to = juce::jmin (tOut, region->endSample());
    if (to - from < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short (it must be inside one piece, at least about 0.01 s)."); return; }
    const auto a = region->srcIn + (from - region->startSample), b = a + (to - from);
    const auto rid = region->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart; std::vector<juce::Uuid> trk;
    for (auto& f : region->files) { if (! e->fixUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (f.fileStart); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Ctrl + Alt + drag are in this piece."); return; }
    const bool subset = ! e->fixTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, eid, rid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, eid, rid, a, b, j, name, trk, subset]
        {
            auto* e2 = app.project.findEdit (eid);
            const auto old = e2 != nullptr ? e2->regions : std::vector<EditRegion>();
            if (e2 == nullptr || ! markWaiting (*e2, rid, a, b, name, j->dest, trk, subset))
            {
                if (e2 != nullptr) e2->regions = old;
                j->folder.deleteRecursively(); say ("Export for Processing", "The edit changed while the audio was being exported, so nothing was changed. Please try again."); return;
            }
            setUndoForEdit (app, eid, old, "export for processing");
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

void exportForProcessingTake (AppContext& app, const juce::Uuid& wid, juce::Component* parent)
{
    if (heldByRec (app, "Export for Processing", parent, [ap = &app, wid] (juce::Component* p) { exportForProcessingTake (*ap, wid, p); })) return;
    const juce::String title = "Export for Processing";
    auto* w = takeWin (app, wid); if (w == nullptr) return;
    double in = 0, out = 0; auto* g = markedTake (app, *w, in, out, title); if (g == nullptr) return;
    if (app.engine.isRecording()) { say (title, "Stop recording first."); return; }
    const double rate = g->sampleRate > 0 ? g->sampleRate : 48000.0;
    const auto a = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (in * rate)), b = juce::jmin (g->lengthSamples, (juce::int64) std::llround (out * rate));
    if (b - a < (juce::int64) (0.01 * rate)) { say (title, "The marked part is too short."); return; }
    for (auto& wp : g->waiting) if (wp.active() && a < wp.to && b > wp.from) { say (title, "Part of this is already waiting for corrected audio ('" + wp.name + "'). Re-link it first (right-click the block)."); return; }
    const auto gid = g->id;
    std::vector<juce::File> src; std::vector<juce::String> names; std::vector<juce::int64> fstart;
    std::vector<juce::Uuid> trk;
    for (auto& f : g->files) { if (! w->editUses (f.trackId)) continue; src.push_back (f.file); names.push_back (f.trackName); fstart.push_back (0); trk.push_back (f.trackId); }
    if (src.empty()) { say (title, "None of the tracks you marked with Ctrl + Alt + drag are in this take."); return; }
    const bool subset = ! w->editTracks.empty();
    askProcessingName ("Give this job a name, e.g. \"bar 4\" or \"click removal\". It becomes a folder in Processing Media with " + juce::String ((int) src.size()) + " file(s), one per track, "
                       "in the format they were recorded.", "", [&app, wid, gid, a, b, src, names, fstart, trk, subset, parent = juce::Component::SafePointer<juce::Component> (parent)] (const juce::String& name)
    {
        auto j = makeExportJob (app, name, src, names, fstart, a, b);
        auto onSuccess = [&app, wid, gid, a, b, j, name, trk, subset]
        {
            auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
            if (g2 == nullptr) { j->folder.deleteRecursively(); return; }
            const auto old = g2->waiting;
            WaitingPiece wp; wp.name = name; wp.from = a; wp.to = b; wp.files = j->dest; if (subset) wp.tracks = trk;
            g2->waiting.push_back (wp);
            app.armFixUndo ("export for processing", [&app, wid, gid, old]
            {
                if (auto* w3 = app.project.findTakeWindow (wid)) if (auto* g3 = w3->findGroup (gid)) { g3->waiting = old; app.project.changed(); }
            }, false, wid, gid);
            app.project.changed();
            say ("Export for Processing", exportedMessage (name, j->folder));
        };
        launchDialog (std::make_unique<SimpleJobDialog> ("Exporting '" + name + "'...", exportWork (j), onSuccess, nullptr), "Export for Processing", parent.getComponent());
    });
}

/** "" if the corrected files can be used, else the sentence(s) that say what is wrong. */
static juce::String checkWaitingFiles (const WaitingPiece& wp, const std::vector<int>& channels, double rate)
{
    juce::String problems;
    for (size_t i = 0; i < wp.files.size(); ++i)
    {
        const auto p = audioops::checkReplacement (formats(), wp.files[i], wp.to - wp.from, i < channels.size() ? channels[i] : 1, rate);
        if (p.isNotEmpty()) problems << p << "\n";
    }
    return problems;
}

void relinkEdit (AppContext& app, const juce::Uuid& eid, const juce::Uuid& rid, juce::Component*)
{
    const juce::String title = "Re-link corrected audio";
    auto* e = editOf (app, eid); if (e == nullptr) return;
    const int i = e->indexOf (rid); if (i < 0) return;
    const auto& r = e->regions[(size_t) i];
    if (! r.waiting.active()) return;
    // which file of the piece each exported file belongs to
    std::vector<size_t> where;
    for (size_t k = 0; k < r.waiting.files.size(); ++k)
    {
        size_t fi = r.files.size();
        if (r.waiting.tracks.empty()) fi = k;
        else for (size_t q = 0; q < r.files.size(); ++q) if (r.files[q].trackId == r.waiting.tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = r.srcIn >= r.waiting.from && r.srcOut <= r.waiting.to && (! r.waiting.tracks.empty() || r.waiting.files.size() == r.files.size());          // (an OUT / IN stream file reaches past the piece, into its handle)
    for (auto fi : where) if (fi >= r.files.size()) fits = false;
    if (! fits) { say (title, "This piece was changed after it was sent out, so the corrected files no longer fit it. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (r.files[fi].numChannels);
    const auto problems = checkWaitingFiles (r.waiting, ch, r.sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    const auto old = e->regions;
    auto& m = e->regions[(size_t) i];
    for (size_t k = 0; k < where.size(); ++k) { m.files[where[k]].file = m.waiting.files[k]; m.files[where[k]].fileStart = m.waiting.from; }
    m.takeName = m.takeName.upToFirstOccurrenceOf (" (", false, false) + " (" + m.waiting.name + ")";
    const auto wf = m.waiting.from, wt = m.waiting.to;
    m.waiting = WaitingPiece();
    e->keepFixFadesInside (i, wf, wt);          // the crossfade must not start before the corrected audio does
    setUndoForEdit (app, eid, old, "re-link corrected audio");
    app.project.changed();
}

void relinkTake (AppContext& app, const juce::Uuid& wid, const juce::Uuid& gid, juce::int64 waitingFrom, juce::Component* parent)
{
    const juce::String title = "Re-link corrected audio";
    auto* w = takeWin (app, wid); auto* g = w ? w->findGroup (gid) : nullptr; if (g == nullptr) return;
    const WaitingPiece* wp = nullptr; for (auto& x : g->waiting) if (x.active() && x.from == waitingFrom) wp = &x;
    if (wp == nullptr) return;
    std::vector<size_t> where;                                   // which file of the take each exported file belongs to
    for (size_t k = 0; k < wp->files.size(); ++k)
    {
        size_t fi = g->files.size();
        if (wp->tracks.empty()) fi = k;
        else for (size_t q = 0; q < g->files.size(); ++q) if (g->files[q].trackId == wp->tracks[k]) fi = q;
        where.push_back (fi);
    }
    bool fits = wp->tracks.empty() ? wp->files.size() == g->files.size() : true;
    for (auto fi : where) if (fi >= g->files.size()) fits = false;
    if (! fits) { say (title, "The take has changed since this was sent out. Use 'Stop waiting: keep the original audio'."); return; }
    std::vector<int> ch; for (auto fi : where) ch.push_back (g->files[fi].numChannels);
    const auto problems = checkWaitingFiles (*wp, ch, g->sampleRate);
    if (problems.isNotEmpty()) { say (title, problems + "\nNothing was changed. Fix the files in your other software (same length, sample rate and channels) and try again."); return; }
    struct Splice { std::vector<juce::File> src, repl, dest; std::vector<size_t> where; juce::int64 from, to; juce::String name; };
    auto sp = std::make_shared<Splice>();
    sp->from = wp->from; sp->to = wp->to; sp->name = wp->name; sp->repl = wp->files; sp->where = where;
    for (auto fi : where)
    {
        auto& f = g->files[fi];
        sp->src.push_back (f.file);
        auto d = audioops::uniqueFile (f.file.getSiblingFile (f.file.getFileNameWithoutExtension() + " (" + sanitiseForFile (wp->name) + ").wav"));
        for (auto& o : sp->dest) if (o == d) d = d.getSiblingFile (d.getFileNameWithoutExtension() + " b.wav");
        sp->dest.push_back (d);
    }
    auto work = [sp] (AudioJob& job) -> juce::String
    {
        for (size_t i = 0; i < sp->src.size(); ++i)
        {
            if (job.cancelled()) { for (auto& d : sp->dest) d.deleteFile(); return "Cancelled."; }
            job.setProgress ((float) i / (float) juce::jmax ((size_t) 1, sp->src.size()));
            juce::String err;
            if (! audioops::spliceFile (formats(), sp->src[i], sp->repl[i], sp->from, sp->to, sp->dest[i], 0.005, err)) { for (auto& d : sp->dest) d.deleteFile(); return err; }
        }
        return {};
    };
    auto onSuccess = [&app, wid, gid, sp]
    {
        auto* w2 = app.project.findTakeWindow (wid); auto* g2 = w2 ? w2->findGroup (gid) : nullptr;
        bool okIdx = g2 != nullptr; if (okIdx) for (auto fi : sp->where) if (fi >= g2->files.size()) okIdx = false;
        if (! okIdx) { for (auto& d : sp->dest) d.deleteFile(); return; }
        const auto old = g2->files;
        for (size_t k = 0; k < sp->where.size(); ++k) g2->files[sp->where[k]].file = sp->dest[k];
        g2->waiting.erase (std::remove_if (g2->waiting.begin(), g2->waiting.end(), [sp] (const WaitingPiece& x) { return x.from == sp->from; }), g2->waiting.end());
        setUndoForTake (app, wid, gid, old, "re-link corrected audio");
        app.project.changed();
    };
    launchDialog (std::make_unique<SimpleJobDialog> ("Putting the corrected audio into the take...", work, onSuccess, nullptr), title, parent);
}

// ======================================================================================================== Project > Import AAF as edit windows
struct AafFound { juce::File file; int channels = 1; juce::int64 length = 0; double rate = 0.0; bool hasTrack = false; juce::Uuid trackId; juce::String trackName; };
struct AafCand { juce::String name; juce::File file; bool hasTrack = false; juce::Uuid trackId; juce::String trackName; };

class AafImportDialog : public JobDialogBase
{
public:
    AafImportDialog (AppContext& a, const juce::File& f, AafResult first) : app (a), aafFile (f), res (std::move (first))
    {
        setSize (700, 560);
        for (auto* l : { &fileLabel, &gapCap, &folderLabel }) { addAndMakeVisible (l); l->setColour (juce::Label::textColourId, theme::text); }
        fileLabel.setText ("AAF:  " + f.getFullPathName(), juce::dontSendNotification);
        gapCap.setText ("A new piece starts after this many seconds of silence:", juce::dontSendNotification);
        gap.setText ("10", juce::dontSendNotification); gap.setInputRestrictions (6, "0123456789.");
        gap.setTooltip ("Nothing playing on any track for longer than this = the next piece (its own edit window). Press Enter to apply.");
        gap.onReturnKey = [this] { reread(); gap.giveAwayKeyboardFocus(); }; gap.onFocusLost = [this] { reread(); };
        addAndMakeVisible (gap);
        folderLabel.setText ("Audio is found in the takes you imported into Fermata. Not there?  Also look in a folder:  (none)", juce::dontSendNotification);
        folderButton.onClick = [this]
        {
            auto chooser = std::make_shared<juce::FileChooser> ("Choose the folder with the session's audio files (e.g. the Pyramix audio folder)");
            juce::Component::SafePointer<AafImportDialog> self (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [chooser, self] (const juce::FileChooser& fc)
            {
                if (self == nullptr || fc.getResult() == juce::File()) return;
                self->extraFolder = fc.getResult();
                self->folderLabel.setText ("Audio is found in the takes you imported into Fermata, and in the folder:  " + self->extraFolder.getFullPathName() + "  (used where it is, not copied)", juce::dontSendNotification);
            });
        };
        addAndMakeVisible (folderButton);
        addAndMakeVisible (summary); addAndMakeVisible (doIt); addAndMakeVisible (cancel);
        summary.setMultiLine (true); summary.setReadOnly (true); summary.setCaretVisible (false); summary.setScrollbarsShown (true);
        summary.setColour (juce::TextEditor::backgroundColourId, theme::field); summary.setFont (juce::FontOptions (13.0f));
        doIt.setColour (juce::TextButton::buttonColourId, theme::accent);
        doIt.onClick = [this] { start(); }; cancel.onClick = [this] { if (running) job.requestCancel(); else closeDialogOf (this); };
        refreshSummary();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        fileLabel.setBounds (r.removeFromTop (22)); r.removeFromTop (4);
        { auto x = r.removeFromTop (26); gapCap.setBounds (x.removeFromLeft (360)); gap.setBounds (x.removeFromLeft (60)); } r.removeFromTop (6);
        { auto x = r.removeFromTop (26); folderButton.setBounds (x.removeFromLeft (150)); } folderLabel.setBounds (r.removeFromTop (40)); r.removeFromTop (4);
        auto bottom = r.removeFromBottom (30);
        cancel.setBounds (bottom.removeFromRight (100)); bottom.removeFromRight (8); doIt.setBounds (bottom.removeFromRight (230));
        r.removeFromBottom (6); bar.setBounds (r.removeFromBottom (22)); status.setBounds (r.removeFromBottom (22));
        summary.setBounds (r);
    }
private:
    void reread()
    {
        if (running) return;
        const double g = juce::jlimit (0.5, 3600.0, gap.getText().getDoubleValue() > 0.0 ? gap.getText().getDoubleValue() : 10.0);
        if (std::abs (g - lastGap) < 1.0e-9) return;
        lastGap = g;
        auto r = readAaf (aafFile, g);
        if (r.error.isNotEmpty()) { status.setText (r.error, juce::dontSendNotification); return; }
        res = std::move (r); status.setText ({}, juce::dontSendNotification); refreshSummary();
    }
    void refreshSummary()
    {
        juce::String t;
        t << juce::String ((int) res.pieces.size()) << " piece(s) found on the AAF's timeline, " << juce::String ((int) res.trackNames.size()) << " track(s), "
          << juce::String (res.clips) << " clips, " << juce::String (res.sampleRate, 0) << " Hz.\n\n";
        for (size_t i = 0; i < res.pieces.size(); ++i)
        {
            const auto& pc = res.pieces[i];
            t << juce::String ((int) i + 1) << ".  " << pc.name << "   -   " << juce::String (pc.regions.size()) << " pieces of takes, " << juce::String (pc.lengthSeconds / 60.0, 1) << " min, at "
              << formatTimeShort (pc.startSeconds, 2) << " in the AAF\n";
        }
        t << "\nTracks:  " << res.trackNames.size() << (res.trackNames.empty() ? juce::String() : juce::String ("  (") + res.trackNames.front() + " ... " + res.trackNames.back() + ")") << "\n";
        if (res.notes.size() > 0) { t << "\nNotes:\n"; for (auto& n : res.notes) t << "   " << n << "\n"; }
        t << "\nEach piece becomes its own edit window, with the takes laid out where they sat in the AAF, the track names kept and the cross-fades kept. "
             "The audio files are NOT copied: the edits use the files you already imported (matched by their original names), or the ones in the extra folder.";
        summary.setText (t, false);
        doIt.setButtonText ("Make " + juce::String ((int) res.pieces.size()) + " edit window(s)");
        doIt.setEnabled (! res.pieces.empty());
    }
    void start()
    {
        reread();
        if (res.pieces.empty()) return;
        doIt.setEnabled (false); status.setText ("Finding the audio files...", juce::dontSendNotification);
        std::vector<AafCand> cands;
        for (auto& w : app.project.takeWindows)
            for (auto& g : w->groups)
                for (auto& f : g.files)
                {
                    for (auto& nm : { f.sourceName, f.sourceName2, f.file.getFileName() })
                        if (nm.isNotEmpty()) cands.push_back ({ nm.toLowerCase(), f.file, true, f.trackId, f.trackName });      // (a joined stereo file answers to both of its mono names)
                }
        auto resCopy = std::make_shared<AafResult> (res);
        auto found = std::make_shared<std::map<juce::String, AafFound>>();
        auto missing = std::make_shared<juce::StringArray>();
        const auto extra = extraFolder;
        juce::Component::SafePointer<AafImportDialog> safe (this);
        auto& appRef = app;
        runJob ([cands, resCopy, found, missing, extra] (AudioJob& j) -> juce::String
                {
                    std::map<juce::String, AafCand> index;
                    for (auto& c : cands) index.emplace (c.name, c);                          // Fermata's own files first
                    if (extra.isDirectory())
                        for (auto& f : extra.findChildFiles (juce::File::findFiles, true, "*"))
                        {
                            if (j.cancelled()) return "Cancelled.";
                            if (isAudioFile (f)) index.emplace (f.getFileName().toLowerCase(), AafCand { f.getFileName().toLowerCase(), f, false, {}, {} });
                        }
                    std::set<juce::String> needed; std::map<juce::String, juce::String> shown;
                    for (auto& pc : resCopy->pieces) for (auto& r : pc.regions) for (auto& f : r.files) { needed.insert (f.fileName.toLowerCase()); shown[f.fileName.toLowerCase()] = f.fileName; }
                    size_t n = 0;
                    for (auto& name : needed)
                    {
                        if (j.cancelled()) return "Cancelled.";
                        j.setProgress ((float) ++n / (float) needed.size());
                        auto it = index.find (name);
                        if (it == index.end() || ! it->second.file.existsAsFile()) { missing->add (shown[name]); continue; }
                        std::unique_ptr<juce::AudioFormatReader> rd (formats().createReaderFor (it->second.file));
                        if (rd == nullptr) { missing->add (shown[name] + " (cannot be read)"); continue; }
                        AafFound af; af.file = it->second.file; af.channels = (int) rd->numChannels; af.length = rd->lengthInSamples; af.rate = rd->sampleRate;
                        af.hasTrack = it->second.hasTrack; af.trackId = it->second.trackId; af.trackName = it->second.trackName;
                        (*found)[name] = af;
                    }
                    return {};
                },
                [safe, resCopy, found, missing, &appRef] (const juce::String& err)
                {
                    if (safe == nullptr) return;
                    if (err.isNotEmpty()) { safe->status.setText (err, juce::dontSendNotification); safe->doIt.setEnabled (true); return; }
                    const auto msg = build (appRef, *resCopy, *found, *missing);
                    closeDialogOf (safe.getComponent());
                    say ("Import AAF", msg);
                });
    }

    /** Message thread: makes the project tracks and one edit per piece. Returns the text to tell the person. */
    static juce::String build (AppContext& app, const AafResult& res, const std::map<juce::String, AafFound>& found, const juce::StringArray& missing)
    {
        auto& p = app.project;
        std::set<int> usedInputs; for (auto& t : p.tracks) for (int c = 0; c < t.channelCount(); ++c) if (t.inputOf (c) >= 0) usedInputs.insert (t.inputOf (c));
        int newTracks = 0;
        std::map<juce::String, juce::Uuid> madeTrack;                                       // tracks made here for files that are not in a take window (by AAF track name)
        // a file that is already in a take window keeps that window's track (a stereo track made from Omni-L and Omni-R is one track); any other file gets a track of its own name
        auto trackFor = [&] (const AafRegionFile& af, const AafFound& fd, juce::Uuid& id, juce::String& name) -> bool
        {
            if (fd.hasTrack && p.findTrack (fd.trackId) != nullptr) { id = fd.trackId; name = p.findTrack (fd.trackId)->name; return true; }
            if (madeTrack.count (af.trackName)) { id = madeTrack[af.trackName]; name = af.trackName; return true; }
            const int ch = juce::jlimit (1, 2, fd.channels);
            const TrackDef* tf = nullptr;
            for (auto& t : p.tracks) if (t.name.equalsIgnoreCase (af.trackName) && t.channelCount() == ch) { tf = &t; break; }
            if (tf == nullptr)
            {
                int first = -1;
                for (int i = 0; i + ch <= (int) p.inputs.size() && first < 0; ++i)
                {
                    bool freeSlot = true; for (int c = 0; c < ch; ++c) freeSlot = freeSlot && ! usedInputs.count (i + c);
                    if (freeSlot) first = i;
                }
                auto& nt = p.addTrack (af.trackName, ch == 2 ? TrackFormat::Stereo : TrackFormat::Mono, first);
                if (first < 0) nt.inputs.fill (-1); else for (int c = 0; c < ch; ++c) usedInputs.insert (first + c);
                tf = &nt; ++newTracks;
            }
            madeTrack[af.trackName] = tf->id; id = tf->id; name = tf->name;
            return true;
        };
        std::map<juce::String, juce::Uuid> aafTrackId;
        int edits = 0, regionsMade = 0, regionsLost = 0, rateSkipped = 0, shortened = 0; juce::Uuid firstEdit;
        for (auto& pc : res.pieces)
        {
            std::vector<EditRegion> regs;
            for (auto& ar : pc.regions)
            {
                EditRegion r; r.id = juce::Uuid(); r.takeName = ar.takeName; r.sampleRate = res.sampleRate;
                r.srcIn = ar.srcIn; r.srcOut = ar.srcOut; r.startSample = ar.startSample;
                r.inStart = ar.inStart; r.inEnd = ar.inEnd; r.outStart = ar.outStart; r.outEnd = ar.outEnd;
                juce::int64 minLen = (std::numeric_limits<juce::int64>::max)();
                std::set<juce::String> seenFiles;
                for (auto& af : ar.files)
                {
                    auto it = found.find (af.fileName.toLowerCase());
                    if (it == found.end()) continue;
                    if (std::abs (it->second.rate - res.sampleRate) > 0.5) { ++rateSkipped; continue; }
                    juce::Uuid tid; juce::String tname;
                    trackFor (af, it->second, tid, tname);
                    aafTrackId[af.trackName] = tid;
                    if (! seenFiles.insert (it->second.file.getFullPathName()).second) continue;          // the left and right halves of one stereo file: one entry
                    RegionFile rf; rf.trackId = tid; rf.trackName = tname; rf.file = it->second.file; rf.numChannels = juce::jlimit (1, 2, it->second.channels);
                    r.files.push_back (rf); minLen = juce::jmin (minLen, it->second.length);
                }
                if (r.files.empty()) { ++regionsLost; continue; }
                r.sourceLength = minLen;
                if (r.srcOut > minLen) { r.srcOut = minLen; ++shortened; }
                if (r.srcIn < 0 || r.srcIn >= r.srcOut) { ++regionsLost; continue; }
                regs.push_back (std::move (r));
            }
            if (regs.empty()) continue;
            auto& e = p.addEdit (pc.name, juce::Uuid::null());             // (from no take window: null, not a made-up id)
            e.sampleRate = res.sampleRate;
            for (auto& tn : pc.trackNames)
            {
                auto ti = aafTrackId.find (tn);
                if (ti != aafTrackId.end() && std::find (e.trackIds.begin(), e.trackIds.end(), ti->second) == e.trackIds.end()) e.trackIds.push_back (ti->second);
            }
            e.regions = std::move (regs); regionsMade += (int) e.regions.size();
            e.insertIndex = -1; e.hardenEnds(); e.clampAll();
            if (edits++ == 0) firstEdit = e.id;
        }
        p.structureChanged(); p.changed();
        if (! firstEdit.isNull() && app.showEdit) app.showEdit (firstEdit);
        juce::String t;
        if (edits == 0) t << "No edit windows were made: none of the audio files named in the AAF could be found.\n\n";
        else t << juce::String (edits) << " edit window(s) made, with " << juce::String (regionsMade) << " pieces of takes in all" << (newTracks > 0 ? ", and " + juce::String (newTracks) + " new track(s)" : juce::String())
               << ". The first is open; the others are in the Edits menu.\n\n";
        if (! missing.isEmpty())
        {
            t << juce::String (missing.size()) << " audio file(s) named in the AAF were not found (import those takes first, or choose the folder they are in):\n";
            for (int i = 0; i < juce::jmin (8, missing.size()); ++i) t << "   " << missing[i] << "\n";
            if (missing.size() > 8) t << "   ...\n";
            t << "\n";
        }
        if (regionsLost > 0) t << juce::String (regionsLost) << " piece(s) were left out because none of their files were found.\n";
        if (rateSkipped > 0) t << juce::String (rateSkipped) << " file(s) were left out because their sample rate is not the AAF's (" << juce::String (res.sampleRate, 0) << " Hz).\n";
        if (shortened > 0) t << juce::String (shortened) << " piece(s) were shortened to the length of their file.\n";
        return t.trimEnd();
    }

    AppContext& app; juce::File aafFile; AafResult res; juce::File extraFolder; double lastGap = 10.0;
    juce::Label fileLabel, gapCap, folderLabel; juce::TextEditor gap, summary;
    juce::TextButton folderButton { "Also look in a folder..." }, doIt { "Make edit windows" }, cancel { "Cancel" };
};

void importAaf (AppContext& app, juce::Component* parent)
{
    if (heldByRec (app, "Importing an AAF", parent, [ap = &app] (juce::Component* p) { importAaf (*ap, p); })) return;
    auto chooser = std::make_shared<juce::FileChooser> ("Choose the AAF file", juce::File(), "*.aaf");
    juce::Component::SafePointer<juce::Component> safeParent (parent);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [chooser, &app, safeParent] (const juce::FileChooser& fc)
    {
        if (fc.getResult() == juce::File()) return;
        auto r = readAaf (fc.getResult(), 10.0);
        if (r.error.isNotEmpty()) { say ("Import AAF", r.error); return; }
        launchDialog (std::make_unique<AafImportDialog> (app, fc.getResult(), std::move (r)), "Import AAF as edit windows", safeParent.getComponent(), true);
    });
}

}} // namespace td::fixtools

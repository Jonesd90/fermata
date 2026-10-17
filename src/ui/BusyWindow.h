#pragma once
#include "Uikit.h"
#include <functional>

namespace td
{
/** The "please wait" window: shown while a project opens and while audio files are read or imported, so a long job never looks like a crash.

    Work done on the message thread (opening a project) calls busy::step() now and then. The window then paints itself at once (at most about
    25 times a second, so it costs nothing), and because painting asks Windows for its messages the program is not marked "Not responding".
    Work done on another thread (reading the files to import) just leaves the window up; it moves on its own, and can offer a Cancel button.

    begin / end may be nested: the window stays until the outermost end(). Message thread only. */
namespace busy
{
/** On Windows the normal (Direct2D) drawing only happens when the program gets back to its message loop, which a long job on the message thread does
    not do. A window that must be seen during such a job uses the software renderer instead: that one can be told to paint at once. */
inline void paintImmediately (juce::Component& c)
{
    if (auto* peer = c.getPeer())
    {
        const auto engines = peer->getAvailableRenderingEngines();
        for (int i = 0; i < engines.size(); ++i)
            if (engines[i].containsIgnoreCase ("software")) { if (peer->getCurrentRenderingEngine() != i) peer->setCurrentRenderingEngine (i); break; }
    }
}

class Window : public juce::Component, private juce::Timer
{
public:
    explicit Window (juce::Rectangle<int> over)
    {
        setOpaque (true);
        setSize (480, 138);
        setAlwaysOnTop (true);
        setCentrePosition (over.getCentre());
        addChildComponent (cancelButton);
        cancelButton.onClick = [this] { if (auto cb = onCancel) { cancelButton.setEnabled (false); detail = "Stopping..."; repaint(); cb(); } };
        addToDesktop (juce::ComponentPeer::windowIsTemporary);
        paintImmediately (*this);
        setVisible (true);
        toFront (false);
        startTimerHz (30);                               // the moving bar, whenever the message loop is free (a job on another thread)
    }

    juce::String title, detail;
    double progress = -1.0;                              // 0..1, or < 0 when how far it has got is not known (the bar then moves to and fro)
    std::function<void()> onCancel;

    void setCancel (std::function<void()> cb)
    {
        onCancel = std::move (cb);
        cancelButton.setVisible (onCancel != nullptr);
        cancelButton.setEnabled (true);
        resized();
    }
    void paintNow()
    {
        repaint();
        if (auto* peer = getPeer()) peer->performAnyPendingRepaintsNow();
    }
    void resized() override { cancelButton.setBounds (getWidth() - 106, getHeight() - 38, 90, 26); }

    void paint (juce::Graphics& g) override { paintPicture (g, getLocalBounds(), title, detail, progress, cancelButton.isVisible()); }

    /** The picture itself (also used by the tests to look at it). */
    static void paintPicture (juce::Graphics& g, juce::Rectangle<int> b, const juce::String& title, const juce::String& detail, double progress, bool withCancel)
    {
        g.fillAll (theme::window);
        g.setColour (theme::border); g.drawRect (b, 1);
        g.setColour (theme::accent); g.fillRect (0, 0, b.getWidth(), 3);
        drawFermataLogo (g, juce::Rectangle<float> (18.0f, 24.0f, 86.0f, 46.0f), theme::text, theme::accent);
        auto text = b.withTrimmedLeft (122).withTrimmedRight (18);
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (title, text.withY (22).withHeight (24), juce::Justification::centredLeft, true);
        g.setColour (theme::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (detail, text.withY (48).withHeight (34), juce::Justification::topLeft, 2, 0.9f);

        const int barRight = withCancel ? b.getWidth() - 120 : b.getWidth() - 18;
        const auto bar = juce::Rectangle<float> (18.0f, (float) b.getHeight() - 30.0f, (float) barRight - 18.0f, 8.0f);
        g.setColour (theme::field); g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (theme::accent);
        if (progress >= 0.0)
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (8.0f, bar.getWidth() * (float) juce::jlimit (0.0, 1.0, progress))), 4.0f);
        else
        {
            // no known end: a block slides to and fro (it moves each time the window is painted)
            const double t = std::fmod (juce::Time::getMillisecondCounterHiRes() / 1600.0, 2.0);
            const float f = (float) (t < 1.0 ? t : 2.0 - t), w = bar.getWidth() * 0.28f;
            g.fillRoundedRectangle (bar.withX (bar.getX() + (bar.getWidth() - w) * f).withWidth (w), 4.0f);
        }
    }

private:
    void timerCallback() override { if (progress < 0.0) repaint(); }
    juce::TextButton cancelButton { "Cancel" };
};

struct State
{
    std::unique_ptr<Window> window;
    int depth = 0;
    juce::uint32 lastPaint = 0;
    std::function<juce::Rectangle<int>()> area;          // where the window goes (the main window), set by the program at start-up
};
inline State& state() { static State s; return s; }

/** Where the window is centred (the main window's area). Without it, a little below the middle of the main screen (below the start-up picture). */
inline void setAreaProvider (std::function<juce::Rectangle<int>()> f) { state().area = std::move (f); }

inline juce::Rectangle<int> placeNow()
{
    auto& s = state();
    if (s.area) { const auto r = s.area(); if (! r.isEmpty()) return r; }
    const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
    return screen.withY (screen.getY() + 230);
}

/** Shows the window (or, inside another begin / end, changes its title). */
inline void begin (const juce::String& title, const juce::String& detail = {})
{
    auto& s = state();
    if (s.window == nullptr) s.window = std::make_unique<Window> (placeNow());
    ++s.depth;
    s.window->title = title; s.window->detail = detail; s.window->progress = -1.0;
    s.window->paintNow();
    s.lastPaint = juce::Time::getMillisecondCounter();
}

/** What is being done now (empty = keep the text), and how far it has got (0..1, or < 0 if not known). Cheap to call often: it only paints
    when 40 ms have passed since the last time. */
inline void step (const juce::String& detail, double progress = -1.0)
{
    auto& s = state();
    if (s.window == nullptr) return;
    if (detail.isNotEmpty()) s.window->detail = detail;
    s.window->progress = progress;
    const auto now = juce::Time::getMillisecondCounter();
    if (now - s.lastPaint < 40) return;
    s.lastPaint = now;
    s.window->paintNow();
}

/** A Cancel button on the window (nullptr removes it). For jobs on another thread. */
inline void setCancel (std::function<void()> cb) { if (auto* w = state().window.get()) w->setCancel (std::move (cb)); }

inline bool isShowing() { return state().window != nullptr; }

inline void end()
{
    auto& s = state();
    if (s.depth > 0 && --s.depth == 0) s.window.reset();
}

/** Closes the window only once the messages already waiting have been handled (the windows redrawing for the new project, for example):
    those come after the work itself, and the program would look stuck if the window went first. */
inline void endAfterPendingMessages()
{
    juce::MessageManager::callAsync ([] { juce::MessageManager::callAsync ([] { end(); }); });
}

struct Scope
{
    explicit Scope (const juce::String& title, const juce::String& detail = {}) { begin (title, detail); }
    ~Scope() { end(); }
};
} // namespace busy
} // namespace td

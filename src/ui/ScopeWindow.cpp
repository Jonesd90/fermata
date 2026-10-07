#include "ScopeWindow.h"

namespace td
{
PhaseScopeComponent::PhaseScopeComponent (AppContext& a, const juce::Uuid& id) : app (a), mixerId (id)
{
    bufL.assign (4096, 0.0f); bufR.assign (4096, 0.0f);
    sourceCaption.setText ("Show:", juce::dontSendNotification);
    gainCaption.setText ("Scope gain:", juce::dontSendNotification);
    for (auto* l : { &sourceCaption, &gainCaption, &readout }) { l->setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); }
    readout.setJustificationType (juce::Justification::centredRight);
    readout.setColour (juce::Label::textColourId, theme::dimText);
    for (int i = 0; i < 6; ++i) gainBox.addItem ((i == 0 ? "0 dB" : "+" + juce::String (i * 6) + " dB"), i + 1);
    gainBox.setSelectedId (3, juce::dontSendNotification);           // +12 dB: music at normal levels fills the scope
    gainBox.setTooltip ("Makes quiet signals bigger on the scope (it does not change the sound)");
    addAndMakeVisible (gainBox);
    sourceBox.onChange = [this] { if (! updating) chooseSource(); };
    sourceBox.setTooltip ("Which audio track, Int Bus or Ext Bus of this mixer goes through the scope (taken after its fader)");
    addAndMakeVisible (sourceBox);
    fillSources();
    app.project.structure.addChangeListener (this);
    startTimerHz (30);
    setSize (460, 540);
}

PhaseScopeComponent::~PhaseScopeComponent()
{
    app.project.structure.removeChangeListener (this);
    app.engine.setScopeSource (nullptr);
}

MixerState* PhaseScopeComponent::mixer() const
{
    for (auto& m : app.project.mixers) if (m->id == mixerId) return m.get();
    return nullptr;
}

void PhaseScopeComponent::fillSources()
{
    juce::String sig;
    for (auto& t : app.project.tracks) sig << t.id.toString() << t.name << "|";
    for (auto& b : app.project.buses) sig << b.id.toString() << b.name << (b.external ? "E" : "I") << "|";
    if (sig == lastSig) return;
    lastSig = sig;
    updating = true;
    const auto* m = mixer();
    const void* cur = app.engine.getScopeSource();
    sourceBox.clear (juce::dontSendNotification);
    sources.clear();
    int selected = 0;
    auto add = [&] (bool isTrack, const juce::Uuid& id, const juce::String& text, const void* ptr)
    {
        sources.push_back ({ isTrack, id });
        sourceBox.addItem (text, (int) sources.size());
        if (ptr != nullptr && ptr == cur) selected = (int) sources.size();
    };
    if (m != nullptr)
    {
        auto* mm = const_cast<MixerState*> (m);
        for (auto& t : app.project.tracks) add (true, t.id, "Track: " + t.name, mm->stripFor (t.id));
        for (auto& b : app.project.buses) if (! b.external) add (false, b.id, "Int bus: " + b.name, mm->busFor (b.id));
        for (auto& b : app.project.buses) if (b.external)   add (false, b.id, "Ext bus: " + b.name, mm->busFor (b.id));
    }
    if (selected != 0) sourceBox.setSelectedId (selected, juce::dontSendNotification);
    else { sourceBox.setSelectedId (0, juce::dontSendNotification); sourceBox.setTextWhenNothingSelected ("(choose what to look at)"); app.engine.setScopeSource (nullptr); }
    updating = false;
}

void PhaseScopeComponent::chooseSource()
{
    auto* m = mixer();
    const int i = sourceBox.getSelectedId() - 1;
    if (m == nullptr || i < 0 || (size_t) i >= sources.size()) return;
    const auto& s = sources[(size_t) i];
    app.engine.setScopeSource (s.isTrack ? (const void*) m->stripFor (s.id) : (const void*) m->busFor (s.id));
}

void PhaseScopeComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (28);
    sourceCaption.setBounds (top.removeFromLeft (44));
    sourceBox.setBounds (top);
    r.removeFromTop (6);
    auto row2 = r.removeFromTop (26);
    gainCaption.setBounds (row2.removeFromLeft (84));
    gainBox.setBounds (row2.removeFromLeft (100).reduced (0, 1));
    readout.setBounds (row2);
    r.removeFromTop (8);
    corrArea = r.removeFromBottom (44);
    r.removeFromBottom (8);
    const int side = juce::jmin (r.getWidth(), r.getHeight());
    scopeArea = juce::Rectangle<int> (side, side).withCentre (r.getCentre());
    trail = juce::Image (juce::Image::RGB, juce::jmax (1, scopeArea.getWidth()), juce::jmax (1, scopeArea.getHeight()), true);
    { juce::Graphics g (trail); g.fillAll (juce::Colour (0xff0c0e11)); }
}

void PhaseScopeComponent::timerCallback()
{
    fillSources();
    if (! trail.isValid()) return;
    drawTrace();
    repaint();
}

void PhaseScopeComponent::drawTrace()
{
    const int n = app.engine.getScopeSource() != nullptr ? app.engine.readScope (bufL.data(), bufR.data(), (int) bufL.size()) : 0;
    juce::Graphics g (trail);
    g.setColour (juce::Colour (0xff0c0e11).withAlpha (0.30f));                  // the old picture fades: the trace leaves a short tail
    g.fillRect (trail.getBounds());
    const float gain = std::pow (10.0f, (float) (gainBox.getSelectedId() - 1) * 6.0f / 20.0f);
    const float cx = (float) trail.getWidth() * 0.5f, cy = (float) trail.getHeight() * 0.5f, rad = juce::jmin (cx, cy) - 6.0f;
    double sLR = 0, sLL = 0, sRR = 0;
    const int recent = juce::jmin (n, 1536);                                  // about 30 ms: what happened since the last picture
    g.setColour (juce::Colour (0xff4fd1e8).withAlpha (0.55f));
    for (int i = 0; i < n; ++i)
    {
        const float l = bufL[(size_t) i], r = bufR[(size_t) i];
        sLR += (double) l * r; sLL += (double) l * l; sRR += (double) r * r;
        if (i < n - recent) continue;
        const float x = (l - r) * 0.70711f * gain, y = (l + r) * 0.70711f * gain;       // 45 degrees: mono is up and down
        const float px = cx + juce::jlimit (-1.0f, 1.0f, x) * rad, py = cy - juce::jlimit (-1.0f, 1.0f, y) * rad;
        g.fillRect (px - 0.8f, py - 0.8f, 1.6f, 1.6f);
    }
    const double den = std::sqrt (sLL * sRR);
    const float c = den > 1.0e-9 ? (float) (sLR / den) : 0.0f;
    correlation += (c - correlation) * 0.25f;
    lvlL = n > 0 ? (float) std::sqrt (sLL / n) : 0.0f; lvlR = n > 0 ? (float) std::sqrt (sRR / n) : 0.0f;
    readout.setText (app.engine.getScopeSource() == nullptr ? "Choose a track or bus above"
                                                              : "Correlation " + juce::String (correlation, 2) + "   L " + juce::String (juce::Decibels::gainToDecibels (lvlL, -99.0f), 1)
                                                                + "  R " + juce::String (juce::Decibels::gainToDecibels (lvlR, -99.0f), 1) + " dB RMS", juce::dontSendNotification);
}

void PhaseScopeComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    if (scopeArea.isEmpty()) return;
    g.drawImageAt (trail, scopeArea.getX(), scopeArea.getY());
    // graticule: the circle, the L and R diagonals, and the mono axis
    auto b = scopeArea.toFloat();
    const auto c = b.getCentre(); const float rad = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 6.0f;
    g.setColour (juce::Colours::white.withAlpha (0.16f));
    g.drawEllipse (c.x - rad, c.y - rad, rad * 2, rad * 2, 1.0f);
    g.drawEllipse (c.x - rad * 0.5f, c.y - rad * 0.5f, rad, rad, 1.0f);
    g.drawLine (c.x, c.y - rad, c.x, c.y + rad); g.drawLine (c.x - rad, c.y, c.x + rad, c.y);
    const float d = rad * 0.70711f;
    g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d); g.drawLine (c.x - d, c.y + d, c.x + d, c.y - d);
    g.setColour (theme::dimText); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("L", (int) (c.x - d) - 16, (int) (c.y - d) - 14, 16, 14, juce::Justification::centred, false);
    g.drawText ("R", (int) (c.x + d), (int) (c.y - d) - 14, 16, 14, juce::Justification::centred, false);
    g.drawText ("MONO", (int) c.x - 20, (int) (c.y - rad) - 14, 40, 14, juce::Justification::centred, false);
    g.drawText ("OUT OF PHASE", (int) (c.x + rad) - 74, (int) c.y + 2, 74, 12, juce::Justification::centredRight, false);

    // correlation meter: -1 (out of phase) .. 0 .. +1 (mono)
    auto cb = corrArea.reduced (4, 10).withHeight (14).toFloat();
    g.setColour (theme::wellDark); g.fillRect (cb);
    g.setColour (theme::border); g.drawRect (cb, 1.0f);
    const float zero = cb.getCentreX(), pos = zero + correlation * cb.getWidth() * 0.5f;
    g.setColour (correlation < -0.1f ? juce::Colour (0xffe8281e) : correlation < 0.3f ? juce::Colour (0xffff9a1f) : juce::Colour (0xff35c46b));
    g.fillRect (juce::Rectangle<float> (juce::jmin (zero, pos), cb.getY() + 2, std::abs (pos - zero), cb.getHeight() - 4));
    g.setColour (theme::text); g.fillRect (zero - 0.5f, cb.getY(), 1.0f, cb.getHeight());
    g.setColour (theme::dimText); g.setFont (juce::FontOptions (10.0f));
    g.drawText ("-1", corrArea.getX() + 4, cb.getBottom() + 2, 24, 12, juce::Justification::centredLeft, false);
    g.drawText ("0", (int) zero - 12, (int) cb.getBottom() + 2, 24, 12, juce::Justification::centred, false);
    g.drawText ("+1", corrArea.getRight() - 28, (int) cb.getBottom() + 2, 24, 12, juce::Justification::centredRight, false);
}
} // namespace td

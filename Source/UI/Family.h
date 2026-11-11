// Flip Fella family UI pieces (shared with Backtrack): flip cards, dials, pills, chips, Pebble.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "BinaryData.h"

namespace fam
{
namespace col
{
    const juce::Colour bg { 0xff050505 }, face { 0xff0d0d0d }, housing { 0xff060606 }, card { 0xff1b1b1b },
        cardHi { 0xff252525 }, cardLow { 0xff141414 }, ink { 0xfff3f0e8 }, dim { 0xffa29e95 }, faint { 0xff8a867d },
        line { 0xff3a3a3a }, ring { 0xff1a1a1a }, red { 0xffe8392f }, teal { 0xff3fd6c8 }, coral { 0xffff7a59 },
        blob { 0xff4d5bff }, blobShade { 0xff2329a8 }, green { 0xff4fcf6a }, blue { 0xff4d8dff }, pupil { 0xff0b0b12 };
}

inline juce::Typeface::Ptr typeface()
{
    static juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (BinaryData::RobotoCondensedBold_ttf,
                                                                            BinaryData::RobotoCondensedBold_ttfSize);
    return tf;
}

inline juce::Font font (float h, float kerning = 0.06f)
{
    return juce::Font (juce::FontOptions (typeface()).withHeight (h).withKerningFactor (kerning));
}

inline void label (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, float h = 13.0f,
                   juce::Colour c = col::dim, juce::Justification j = juce::Justification::centredLeft)
{
    g.setColour (c);
    g.setFont (font (h, 0.08f));
    g.drawText (text, r, j, false);
}

//==============================================================================
// Split-flap card. When the text changes the top half flips down to reveal the new value.
class FlipCard : public juce::Component, private juce::Timer
{
public:
    explicit FlipCard (float textHeight = 30.0f) : textH (textHeight) {}

    void setText (const juce::String& t, juce::Colour c = col::ink)
    {
        colour = c;
        if (t == current) return;
        previous = current;
        current = t;
        progress = 0.0f;
        startTimerHz (60);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (col::housing);
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (juce::Colour (0xff3c3c3c));
        g.fillRect (b.getX() + 1, b.getCentreY() - 6, 3.0f, 12.0f);
        g.fillRect (b.getRight() - 4, b.getCentreY() - 6, 3.0f, 12.0f);

        auto c = b.reduced (6.0f);
        auto top = c.withHeight (c.getHeight() * 0.5f);
        auto bot = c.withTrimmedTop (c.getHeight() * 0.5f);

        auto drawHalf = [&] (juce::Rectangle<float> clip, const juce::String& text, float squash, bool upper)
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (clip.toNearestInt());
            if (squash < 1.0f)
            {
                const float pivot = c.getCentreY();
                g.addTransform (juce::AffineTransform::scale (1.0f, juce::jmax (0.001f, squash), 0.0f, pivot));
            }
            juce::ColourGradient grad (upper ? col::cardHi : col::card, 0, c.getY(), upper ? col::card : col::cardLow, 0, c.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (c, 7.0f);
            g.setColour (colour);
            g.setFont (font (textH, 0.02f));
            g.drawFittedText (text, c.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
        };

        if (progress >= 1.0f || previous.isEmpty())
        {
            drawHalf (top, current, 1.0f, true);
            drawHalf (bot, current, 1.0f, false);
        }
        else
        {
            drawHalf (top, current, 1.0f, true);   // new top behind the flap
            drawHalf (bot, previous, 1.0f, false); // old bottom until the flap lands
            if (progress < 0.5f) drawHalf (top, previous, 1.0f - progress * 2.0f, true);
            else                 drawHalf (bot, current, (progress - 0.5f) * 2.0f, false);
        }

        g.setColour (juce::Colours::black);
        g.fillRect (c.getX(), c.getCentreY() - 1.5f, c.getWidth(), 3.0f);
    }

private:
    juce::String current, previous;
    juce::Colour colour { col::ink };
    float textH, progress = 1.0f;

    void timerCallback() override
    {
        progress += 1.0f / (0.28f * 60.0f);
        if (progress >= 1.0f) { progress = 1.0f; stopTimer(); }
        repaint();
    }
};

//==============================================================================
// Rotary dial bound to a parameter. The parameter can be swapped when the mode changes.
class Dial : public juce::Component, private juce::Timer
{
public:
    using TextFn = std::function<juce::String (float)>;

    Dial (juce::AudioProcessorValueTreeState& s, bool heroDial = false) : state (s), hero (heroDial) { startTimerHz (30); }

    void bind (const juce::String& paramId, const juce::String& caption, TextFn fn = {}, bool redPointer = false, bool secondary = false)
    {
        param = state.getParameter (paramId);
        cap = caption;
        textFn = std::move (fn);
        red = redPointer;
        secondaryCap = secondary;
        setVisible (param != nullptr);
        repaint();
    }

    void unbind() { param = nullptr; setVisible (false); }

    float getValue() const { return param ? param->getValue() : 0.0f; }

    std::function<void (juce::Graphics&, juce::Point<float>, float)> extraLabels;

    void paint (juce::Graphics& g) override
    {
        if (param == nullptr) return;
        const float v = param->getValue();
        auto b = getLocalBounds().toFloat();
        const float capH = hero ? 0.0f : 18.0f;
        const float valH = hero ? 0.0f : 22.0f;
        auto dialArea = b.withTrimmedTop (capH).withTrimmedBottom (valH);
        const float size = juce::jmin (dialArea.getWidth(), dialArea.getHeight()) - (hero ? 60.0f : 0.0f);
        auto d = juce::Rectangle<float> (size, size).withCentre (dialArea.getCentre());
        const auto c = d.getCentre();
        const float r = size * 0.5f;

        if (! hero)
            label (g, cap, getLocalBounds().withHeight (18), 13.0f, secondaryCap ? col::faint : col::dim, juce::Justification::centred);

        g.setColour (col::ring);
        g.fillEllipse (d);
        g.setColour (col::bg);
        g.fillEllipse (d.reduced (r * (hero ? 0.185f : 0.1f)));

        const int ticks = hero ? 20 : 10;
        g.setColour (col::ink);
        for (int i = 0; i <= ticks; ++i)
        {
            const float a = juce::degreesToRadians (-135.0f + 270.0f * (float) i / (float) ticks);
            const float r1 = r * (hero ? 0.84f : 0.8f), r2 = r * (hero ? 0.95f : 0.92f);
            g.drawLine (c.x + std::sin (a) * r1, c.y - std::cos (a) * r1, c.x + std::sin (a) * r2, c.y - std::cos (a) * r2, 2.0f);
        }
        if (extraLabels) extraLabels (g, c, r);

        const float ang = juce::degreesToRadians (-135.0f + 270.0f * v);
        juce::Path ptr;
        if (hero) ptr.addRoundedRectangle (-2.0f, -r * 0.64f, 4.0f, r * 0.77f, 2.0f);
        else      ptr.addRoundedRectangle (-3.0f, -r * 0.68f, 6.0f, r * 0.83f, 2.0f);
        g.setColour (red || hero ? col::red : col::ink);
        g.fillPath (ptr, juce::AffineTransform::rotation (ang).translated (c));
        const float hub = hero ? 9.0f : 5.0f;
        g.setColour (hero ? col::red : col::ink);
        g.fillEllipse (juce::Rectangle<float> (hub * 2, hub * 2).withCentre (c));
        g.setColour (col::bg);
        g.fillEllipse (juce::Rectangle<float> (hub * 0.7f, hub * 0.7f).withCentre (c));

        if (! hero)
        {
            const juce::String t = textFn ? textFn (v) : param->getCurrentValueAsText();
            label (g, t, getLocalBounds().removeFromBottom (20), 15.0f, col::ink, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (param == nullptr) return;
        dragStart = param->getValue();
        dragY = e.position.y;
        param->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (param == nullptr) return;
        const float sens = e.mods.isShiftDown() ? 1200.0f : (hero ? 400.0f : 250.0f);
        param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, dragStart + (dragY - e.position.y) / sens));
    }
    void mouseUp (const juce::MouseEvent&) override { if (param) param->endChangeGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (param == nullptr) return;
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getDefaultValue());
        param->endChangeGesture();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (param == nullptr) return;
        param->beginChangeGesture();
        param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, param->getValue() + w.deltaY * 0.05f));
        param->endChangeGesture();
    }

private:
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter* param = nullptr;
    juce::String cap;
    TextFn textFn;
    bool hero = false, red = false, secondaryCap = false;
    float dragStart = 0.0f, dragY = 0.0f, lastValue = -1.0f;

    void timerCallback() override
    {
        if (param && param->getValue() != lastValue) { lastValue = param->getValue(); repaint(); }
    }
};

//==============================================================================
// Row of pill buttons bound to a choice / int parameter.
class Pills : public juce::Component, private juce::Timer
{
public:
    explicit Pills (juce::AudioProcessorValueTreeState& s) : state (s) { startTimerHz (20); }

    void bind (const juce::String& paramId, juce::StringArray labels, float textH = 13.0f)
    {
        param = state.getParameter (paramId);
        names = std::move (labels);
        th = textH;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (param == nullptr || names.isEmpty()) return;
        const int sel = selected();
        const float gap = 6.0f;
        const float w = (getWidth() - gap * (names.size() - 1)) / (float) names.size();
        for (int i = 0; i < names.size(); ++i)
        {
            auto r = juce::Rectangle<float> (i * (w + gap), 0.0f, w, (float) getHeight()).reduced (1.0f);
            const bool on = i == sel;
            g.setColour (on ? col::ink : col::line);
            if (on) g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            else    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 2.0f);
            label (g, names[i], r.toNearestInt(), th, on ? col::bg : col::ink, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (param == nullptr || names.isEmpty()) return;
        const int i = juce::jlimit (0, names.size() - 1, (int) (e.position.x / getWidth() * names.size()));
        const int steps = param->getNumSteps() > 1 ? param->getNumSteps() - 1 : names.size() - 1;
        param->beginChangeGesture();
        param->setValueNotifyingHost (steps > 0 ? (float) i / (float) steps : 0.0f);
        param->endChangeGesture();
    }

private:
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter* param = nullptr;
    juce::StringArray names;
    float th = 13.0f;
    int last = -1;

    int selected() const
    {
        const int steps = param->getNumSteps() > 1 ? param->getNumSteps() - 1 : names.size() - 1;
        return juce::roundToInt (param->getValue() * (float) steps);
    }
    void timerCallback() override
    {
        if (param && selected() != last) { last = selected(); repaint(); }
    }
};

//==============================================================================
// Toggle chip bound to a bool parameter.
class Chip : public juce::Component, private juce::Timer
{
public:
    Chip (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, juce::String text, float textH = 11.0f)
        : param (s.getParameter (paramId)), name (std::move (text)), th (textH) { startTimerHz (20); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = param->getValue() > 0.5f;
        g.setColour (on ? col::ink : col::line);
        if (on) g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        else    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 2.0f);
        label (g, name, r.toNearestInt(), th, on ? col::bg : col::ink, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getValue() > 0.5f ? 0.0f : 1.0f);
        param->endChangeGesture();
    }
    void setText (const juce::String& t) { if (t != name) { name = t; repaint(); } }

private:
    juce::RangedAudioParameter* param;
    juce::String name;
    float th;
    bool last = false;
    void timerCallback() override
    {
        const bool on = param->getValue() > 0.5f;
        if (on != last) { last = on; repaint(); }
    }
};

//==============================================================================
// Big rounded button with a small subtitle. Press and release are reported separately.
class BigButton : public juce::Component
{
public:
    std::function<void()> onPress, onRelease;

    void set (const juce::String& t, const juce::String& sub, juce::Colour border)
    {
        if (t == text && sub == subtitle && border == edge) return;
        text = t; subtitle = sub; edge = border;
        repaint();
    }
    void setTextHeight (float h) { th = h; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (down ? col::ring : col::bg);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (edge);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 2.0f);
        if (subtitle.isEmpty())
            label (g, text, r.toNearestInt(), th, col::ink, juce::Justification::centred);
        else
        {
            auto ri = r.toNearestInt();
            const int total = (int) th + 16;
            auto block = ri.withSizeKeepingCentre (ri.getWidth() - 16, total);
            label (g, text, block.removeFromTop ((int) th + 2), th, col::ink, juce::Justification::centred);
            label (g, subtitle, block, 11.0f, col::dim, juce::Justification::centred);
        }
    }
    void mouseDown (const juce::MouseEvent&) override { down = true; repaint(); if (onPress) onPress(); }
    void mouseUp (const juce::MouseEvent&) override { down = false; repaint(); if (onRelease) onRelease(); }

private:
    juce::String text, subtitle;
    juce::Colour edge { col::line };
    float th = 18.0f;
    bool down = false;
};

//==============================================================================
// Pebble: the blob that lives on every Flip Fella plugin.
class Pebble : public juce::Component, private juce::Timer
{
public:
    enum Mood { Idle, Recording, Reversing, PitchUp, PitchDown, Clipping, Asleep, Holding, FootTap, Kept };

    Pebble() { startTimerHz (60); }

    void setBase (Mood m) { base = m; }
    void react (Mood m) { if (base == Asleep) return; event = m; eventTime = 0.0f; }
    void setLevel (float l) { level = l; }
    void setTranscend (bool on) { transcend = on; }
    Mood current() const { return event != Idle ? event : base; }

    void paint (juce::Graphics& g) override
    {
        const Mood m = current();
        auto b = getLocalBounds().toFloat();
        const float s = juce::jmin (b.getWidth(), b.getHeight()) / 160.0f;
        g.addTransform (juce::AffineTransform::translation (-20.0f, -20.0f).scaled (s).translated (b.getX(), b.getY()));

        float dx = 0, dy = 0, sx = 1, sy = 1, rot = 0;
        const float et = eventTime;
        if (m == Idle || m == Holding) dy = -3.0f * (0.5f - 0.5f * std::cos (t * 2.4f));
        if (m == PitchUp)
        {
            const float k = juce::jlimit (0.0f, 1.0f, et / 0.7f);
            dy = -18.0f * std::sin (k * juce::MathConstants<float>::pi);
            sy = 1.0f + 0.1f * std::sin (k * juce::MathConstants<float>::pi);
            sx = 2.0f - sy;
        }
        if (m == PitchDown)
        {
            const float k = juce::jlimit (0.0f, 1.0f, et / 0.3f);
            sy = 1.0f - 0.16f * k; sx = 1.0f + 0.12f * k;
        }
        if (m == Reversing) rot = -juce::MathConstants<float>::twoPi * juce::jlimit (0.0f, 1.0f, et / 0.9f);
        if (m == Clipping) dx = 3.0f * std::sin (t * 60.0f);
        if (m == FootTap) dy = -3.0f * (0.5f - 0.5f * std::cos (t * 2.4f));
        if (m == Kept)
        {
            // two happy nods
            const float k = std::sin (juce::jlimit (0.0f, 0.9f, et) / 0.45f * juce::MathConstants<float>::pi);
            rot = juce::degreesToRadians (6.0f) * std::abs (k);
            dy = 4.0f * std::abs (k);
        }

        juce::Graphics::ScopedSaveState ss (g);
        g.addTransform (juce::AffineTransform::scale (sx, sy, 100.0f, 148.0f)
                            .rotated (rot, 100.0f, 100.0f)
                            .translated (dx, dy));

        // Glow ring
        juce::Colour glow = transcend ? col::green : col::teal;
        float glowAlpha = 0.35f + 0.5f * juce::jlimit (0.0f, 1.0f, level * 3.0f);
        if (m == Recording || m == Holding) { glow = m == Holding ? col::coral : col::red; glowAlpha = 0.55f + 0.45f * std::sin (t * 8.0f); }
        if (m == Reversing) glow = col::coral;
        if (m == PitchDown) glow = col::blue;
        if (m == Clipping) { glow = col::red; glowAlpha = 1.0f; }
        if (m == Kept) { glow = col::green; glowAlpha = 0.8f; }
        if (m != Asleep)
        {
            g.setColour (glow.withAlpha (juce::jlimit (0.1f, 1.0f, glowAlpha)));
            g.drawEllipse (34, 44, 132, 112, 5.0f);
        }

        // FootTap: a little foot pats along with yours
        if (m == FootTap)
        {
            juce::Graphics::ScopedSaveState fs (g);
            const float pat = std::abs (std::sin (juce::jlimit (0.0f, 0.9f, et) / 0.3f * juce::MathConstants<float>::pi));
            g.addTransform (juce::AffineTransform::rotation (-juce::degreesToRadians (25.0f) * pat, 116.0f, 150.0f));
            g.setColour (col::blobShade);
            g.fillEllipse (116, 143, 32, 14);
        }

        // Body
        g.setColour (col::blobShade);
        g.fillEllipse (49, 61, 116, 96);
        g.setColour (m == Asleep ? juce::Colour (0xff3a44b8) : col::blob);
        g.fillEllipse (42, 52, 116, 96);

        const auto dark = col::pupil;
        auto eye = [&] (float x, float y, float r) { g.setColour (juce::Colours::white); g.fillEllipse (x - r, y - r, r * 2, r * 2); };
        auto dot = [&] (float x, float y, float r, juce::Colour c) { g.setColour (c); g.fillEllipse (x - r, y - r, r * 2, r * 2); };
        auto stroke = [&] (const juce::Path& p, float w, juce::Colour c = col::pupil)
        { g.setColour (c); g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)); };

        juce::Path mouth;
        switch (m)
        {
            case Kept:
            {
                juce::Path happy;
                happy.startNewSubPath (68, 96); happy.quadraticTo (80, 82, 92, 96);
                happy.startNewSubPath (108, 96); happy.quadraticTo (120, 82, 132, 96);
                stroke (happy, 5);
                mouth.startNewSubPath (86, 116); mouth.quadraticTo (100, 132, 114, 116);
                stroke (mouth, 6);
                break;
            }
            case Idle:
            case Holding:
            case FootTap:
            {
                const float blink = blinkAmount();
                const auto pupil = transcend ? juce::Colour (0xff1f7a35) : dark;
                for (float ex : { 80.0f, 120.0f })
                {
                    g.setColour (juce::Colours::white);
                    g.fillEllipse (ex - 14, 92 - 14 * blink, 28, 28 * blink);
                    if (blink > 0.3f) dot (ex + 2, 94, 7 * blink, pupil);
                }
                mouth.startNewSubPath (90, 120); mouth.quadraticTo (100, 130, 110, 120);
                stroke (mouth, 6);
                break;
            }
            case Recording:
                eye (80, 90, 16); eye (120, 90, 16);
                dot (80, 86, 8, dark); dot (120, 86, 8, dark);
                g.setColour (dark); g.fillEllipse (92, 113, 16, 20);
                dot (158, 44, 8, col::red.withAlpha (0.5f + 0.5f * std::sin (t * 8.0f)));
                break;
            case Reversing:
                eye (80, 92, 14); eye (120, 92, 14);
                for (float ex : { 80.0f, 120.0f })
                {
                    juce::Path sp;
                    sp.addCentredArc (ex, 92, 6, 6, 0, 0, juce::MathConstants<float>::twoPi * 0.85f, true);
                    sp.addCentredArc (ex, 92, 3, 3, 0, 0, juce::MathConstants<float>::pi, true);
                    stroke (sp, 3);
                }
                mouth.startNewSubPath (86, 122); mouth.quadraticTo (93, 115, 100, 122); mouth.quadraticTo (107, 129, 114, 122);
                stroke (mouth, 5);
                break;
            case PitchUp:
            {
                juce::Path brows;
                brows.startNewSubPath (66, 72); brows.lineTo (92, 66);
                brows.startNewSubPath (108, 66); brows.lineTo (134, 72);
                stroke (brows, 5);
                eye (80, 90, 14); eye (120, 90, 14);
                dot (80, 86, 7, dark); dot (120, 86, 7, dark);
                mouth.startNewSubPath (84, 114); mouth.quadraticTo (100, 136, 116, 114); mouth.closeSubPath();
                g.setColour (dark); g.fillPath (mouth);
                break;
            }
            case PitchDown:
            {
                eye (80, 96, 14); eye (120, 96, 14);
                dot (81, 100, 7, dark); dot (121, 100, 7, dark);
                g.setColour (col::blob); g.fillRect (64.0f, 80.0f, 32.0f, 15.0f); g.fillRect (104.0f, 80.0f, 32.0f, 15.0f);
                juce::Path lids; lids.startNewSubPath (66, 95); lids.lineTo (94, 95); lids.startNewSubPath (106, 95); lids.lineTo (134, 95);
                stroke (lids, 4);
                mouth.startNewSubPath (90, 124); mouth.lineTo (110, 124);
                stroke (mouth, 6);
                break;
            }
            case Clipping:
            {
                juce::Path x;
                for (float ex : { 80.0f, 120.0f }) { x.startNewSubPath (ex - 10, 82); x.lineTo (ex + 10, 102); x.startNewSubPath (ex + 10, 82); x.lineTo (ex - 10, 102); }
                stroke (x, 6, juce::Colours::white);
                mouth.startNewSubPath (84, 124);
                for (int i = 1; i <= 5; ++i) mouth.lineTo (84.0f + i * 6.0f, i % 2 ? 118.0f : 124.0f);
                stroke (mouth, 5);
                break;
            }
            case Asleep:
            {
                juce::Path lids;
                lids.startNewSubPath (68, 98); lids.quadraticTo (80, 106, 92, 98);
                lids.startNewSubPath (108, 98); lids.quadraticTo (120, 106, 132, 98);
                stroke (lids, 5);
                mouth.startNewSubPath (94, 122); mouth.quadraticTo (100, 126, 106, 122);
                stroke (mouth, 5);
                g.setColour (col::dim);
                g.setFont (font (22.0f));
                g.drawText ("z", 140, 40 - (int) (4 * std::sin (t)), 20, 24, juce::Justification::centred);
                g.setFont (font (16.0f));
                g.drawText ("z", 156, 22 - (int) (4 * std::sin (t + 1)), 16, 20, juce::Justification::centred);
                break;
            }
        }
    }

private:
    Mood base = Idle, event = Idle;
    float t = 0.0f, eventTime = 0.0f, level = 0.0f, nextBlink = 3.0f, blinkT = -1.0f;
    bool transcend = false;

    float blinkAmount() const
    {
        if (blinkT < 0.0f) return 1.0f;
        return juce::jlimit (0.1f, 1.0f, std::abs (blinkT - 0.08f) / 0.08f);
    }

    void timerCallback() override
    {
        const float dt = 1.0f / 60.0f;
        t += dt;
        if (event != Idle)
        {
            eventTime += dt;
            const float len = event == Clipping || event == FootTap ? 0.9f : 1.2f;
            if (eventTime > len) event = Idle;
        }
        nextBlink -= dt;
        if (nextBlink <= 0.0f) { blinkT = 0.0f; nextBlink = 2.5f + juce::Random::getSystemRandom().nextFloat() * 3.0f; }
        if (blinkT >= 0.0f) { blinkT += dt; if (blinkT > 0.16f) blinkT = -1.0f; }
        repaint();
    }
};

//==============================================================================
// Chip that steps through a choice parameter (GRIT: OFF, 24-BIT, 12-BIT).
class CycleChip : public juce::Component, private juce::Timer
{
public:
    CycleChip (juce::AudioProcessorValueTreeState& s, const juce::String& paramId, juce::StringArray labels)
        : param (s.getParameter (paramId)), names (std::move (labels)) { startTimerHz (20); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const int i = index();
        const bool on = i > 0;
        g.setColour (on ? col::ink : col::line);
        if (on) g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        else    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 2.0f);
        label (g, names[i], r.toNearestInt(), 11.0f, on ? col::bg : col::ink, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override
    {
        const int n = names.size();
        param->beginChangeGesture();
        param->setValueNotifyingHost ((float) ((index() + 1) % n) / (float) (n - 1));
        param->endChangeGesture();
    }

private:
    juce::RangedAudioParameter* param;
    juce::StringArray names;
    int last = -1;
    int index() const { return juce::jlimit (0, names.size() - 1, juce::roundToInt (param->getValue() * (float) (names.size() - 1))); }
    void timerCallback() override { if (index() != last) { last = index(); repaint(); } }
};

//==============================================================================
// Toggle pill with its own callback, for things that aren't parameters.
class PillButton : public juce::Component
{
public:
    std::function<void()> onClick;
    void set (const juce::String& t, bool isOn, float textH = 12.0f)
    {
        if (t == text && isOn == on && textH == th) return;
        text = t; on = isOn; th = textH; repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (on ? col::ink : col::line);
        if (on) g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        else    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 2.0f);
        label (g, text, r.toNearestInt(), th, on ? col::bg : col::ink, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

private:
    juce::String text;
    bool on = false;
    float th = 12.0f;
};

//==============================================================================
// One on-screen footswitch: letter, tap job, hold job. Press and release are reported,
// so holding it on screen does exactly what holding the real footswitch does.
class FootButton : public juce::Component
{
public:
    std::function<void()> onPress, onRelease;

    void set (const juce::String& l, const juce::String& tapJob, const juce::String& holdJob, bool lit, juce::Colour c)
    {
        if (l == letter && tapJob == tap && holdJob == hold && lit == on && c == colour) return;
        letter = l; tap = tapJob; hold = holdJob; on = lit; colour = c;
        repaint();
    }
    void setDown (bool d) { if (d != down) { down = d; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (down ? col::ring : col::bg);
        g.fillRoundedRectangle (r, 14.0f);
        g.setColour (on || down ? colour : col::line);
        g.drawRoundedRectangle (r, 14.0f, 2.0f);
        auto in = getLocalBounds().reduced (12, 10);
        g.setColour (on ? colour : col::dim);
        g.setFont (font (26.0f, 0.02f));
        g.drawText (letter, in.removeFromTop (28), juce::Justification::centredLeft);
        in.removeFromTop (4);
        label (g, tap, in.removeFromTop (18), 14.0f, col::ink);
        in.removeFromTop (2);
        label (g, "HOLD " + juce::String (juce::CharPointer_UTF8 ("\xc2\xb7 ")) + hold, in.removeFromTop (14), 10.0f, col::dim);
    }
    void mouseDown (const juce::MouseEvent&) override { setDown (true); if (onPress) onPress(); }
    void mouseUp (const juce::MouseEvent&) override { setDown (false); if (onRelease) onRelease(); }

private:
    juce::String letter, tap, hold;
    juce::Colour colour { col::ink };
    bool on = false, down = false;
};

//==============================================================================
// Thin horizontal slider with a teal thumb.
class ThinSlider : public juce::Component
{
public:
    std::function<void (float)> onChange;
    void setValue (float v) { v = juce::jlimit (0.0f, 1.0f, v); if (std::abs (v - value) > 1.0e-4f) { value = v; repaint(); } }
    float getValue() const { return value; }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float cy = r.getCentreY();
        auto track = juce::Rectangle<float> (r.getX() + 6, cy - 3, r.getWidth() - 12, 6);
        g.setColour (col::ring); g.fillRoundedRectangle (track, 3.0f);
        g.setColour (col::teal); g.fillRoundedRectangle (track.withWidth (track.getWidth() * value), 3.0f);
        g.fillEllipse (juce::Rectangle<float> (14, 14).withCentre ({ track.getX() + track.getWidth() * value, cy }));
    }
    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }

private:
    float value = 0.0f;
    void drag (const juce::MouseEvent& e)
    {
        setValue ((e.position.x - 6.0f) / juce::jmax (1.0f, getWidth() - 12.0f));
        if (onChange) onChange (value);
    }
};

//==============================================================================
// Text link in teal, for PEDAL PAGE and BACK.
class LinkButton : public juce::Component
{
public:
    std::function<void()> onClick;
    juce::String text;
    juce::Justification just { juce::Justification::centredRight };
    void paint (juce::Graphics& g) override { label (g, text, getLocalBounds(), 13.0f, isMouseOver() ? col::ink : col::teal, just); }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
};

} // namespace fam

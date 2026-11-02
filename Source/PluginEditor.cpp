#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

// =====================================================================================
// Palette helpers
// =====================================================================================
namespace ff
{
Font font (float height, float kerning)
{
    static Typeface::Ptr typeface = Typeface::createSystemTypefaceFor (BinaryData::RobotoCondensedBold_ttf,
                                                                       BinaryData::RobotoCondensedBold_ttfSize);
    return Font (FontOptions (typeface).withHeight (height).withKerningFactor (kerning));
}

void label (Graphics& g, const String& text, Rectangle<int> r, Justification j, Colour c, float size)
{
    g.setColour (c);
    g.setFont (font (size, 0.08f));
    g.drawText (text, r, j, false);
}
} // namespace ff

namespace ui
{
static double nowSeconds() { return Time::getMillisecondCounterHiRes() * 0.001; }

static void setParam (RangedAudioParameter* p, float plain)
{
    if (p == nullptr) return;
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
    p->endChangeGesture();
}

static float plainOf (const RangedAudioParameter* p) { return p->convertFrom0to1 (p->getValue()); }

// =====================================================================================
// FlipCard
// =====================================================================================
void FlipCard::setValue (const String& v)
{
    if (v == current) return;
    previous = current;
    current = v;
    progress = previous.isEmpty() ? 1.0f : 0.0f;
    repaint();
}

void FlipCard::advance (double dt)
{
    if (progress < 1.0f)
    {
        progress = jmin (1.0f, progress + (float) (dt / 0.26));
        repaint();
    }
}

void FlipCard::drawHalf (Graphics& g, Rectangle<float> card, const String& text, bool top, float scale, bool shade)
{
    const float hinge = card.getCentreY();
    const float halfH = card.getHeight() * 0.5f;
    if (scale <= 0.001f) return;

    Graphics::ScopedSaveState save (g);
    auto clip = top ? Rectangle<float> (card.getX(), hinge - halfH * scale, card.getWidth(), halfH * scale)
                    : Rectangle<float> (card.getX(), hinge, card.getWidth(), halfH * scale);
    g.reduceClipRegion (clip.getSmallestIntegerContainer());
    g.addTransform (AffineTransform::scale (1.0f, scale, 0.0f, hinge));

    if (top) g.setGradientFill (ColourGradient (ff::cardHi, 0, card.getY(), ff::card, 0, hinge, false));
    else     g.setGradientFill (ColourGradient (ff::card, 0, hinge, ff::cardLow, 0, card.getBottom(), false));
    g.fillRoundedRectangle (card, 7.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (textHeight, 0.02f));
    g.drawText (text, card, Justification::centred, false);

    if (shade)
    {
        g.setColour (Colours::black.withAlpha (0.45f * (1.0f - scale)));
        g.fillRoundedRectangle (card, 7.0f);
    }
}

void FlipCard::paint (Graphics& g)
{
    auto outer = getLocalBounds().toFloat();
    g.setColour (ff::housing);
    g.fillRoundedRectangle (outer, 10.0f);
    auto card = outer.reduced (6.0f);

    if (progress >= 1.0f)
    {
        drawHalf (g, card, current, true, 1.0f, false);
        drawHalf (g, card, current, false, 1.0f, false);
    }
    else
    {
        drawHalf (g, card, current, true, 1.0f, false);
        drawHalf (g, card, previous, false, 1.0f, false);
        const float eased = 1.0f - std::pow (1.0f - progress, 2.0f);
        if (eased < 0.5f) drawHalf (g, card, previous, true, 1.0f - eased * 2.0f, true);
        else              drawHalf (g, card, current, false, (eased - 0.5f) * 2.0f, true);
    }

    g.setColour (Colours::black);
    g.fillRect (card.getX(), card.getCentreY() - 1.5f, card.getWidth(), 3.0f);
    g.setColour (ff::pin);
    g.fillRoundedRectangle (outer.getX() + 1.0f, card.getCentreY() - 6.0f, 3.0f, 12.0f, 1.0f);
    g.fillRoundedRectangle (outer.getRight() - 4.0f, card.getCentreY() - 6.0f, 3.0f, 12.0f, 1.0f);
}

// =====================================================================================
// Pills and chips
// =====================================================================================
Pills::Pills (AudioProcessorValueTreeState& s, const String& paramId, StringArray l, float size)
    : labels (std::move (l)), textSize (size)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*s.getParameter (paramId), [this] (float v)
    {
        selected = (int) std::lround (v);
        repaint();
    });
    attachment->sendInitialUpdate();
}

Rectangle<float> Pills::cell (int i) const
{
    const float gap = 8.0f;
    const float w = ((float) getWidth() - gap * (float) (labels.size() - 1)) / (float) labels.size();
    return { (float) i * (w + gap), 0.0f, w, (float) getHeight() };
}

void Pills::paint (Graphics& g)
{
    for (int i = 0; i < labels.size(); ++i)
    {
        auto b = cell (i).reduced (1.0f);
        const float r = b.getHeight() * 0.5f;
        const bool on = i == selected;
        if (on)
        {
            g.setColour (ff::ink);
            g.fillRoundedRectangle (b, r);
        }
        else
        {
            g.setColour (ff::line);
            g.drawRoundedRectangle (b.reduced (1.0f), r, 2.0f);
        }
        g.setColour (on ? ff::bg : ff::ink);
        g.setFont (ff::font (textSize, 0.06f));
        g.drawText (labels[i], b, Justification::centred, false);
    }
}

void Pills::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < labels.size(); ++i)
        if (cell (i).contains (e.position))
        {
            attachment->setValueAsCompleteGesture ((float) i);
            return;
        }
}

ToggleChip::ToggleChip (AudioProcessorValueTreeState& s, const String& paramId, const String& t, float size)
    : text (t), textSize (size)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*s.getParameter (paramId), [this] (float v) { on = v > 0.5f; repaint(); });
    attachment->sendInitialUpdate();
}

void ToggleChip::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float r = b.getHeight() * 0.5f;
    if (on)
    {
        g.setColour (ff::ink);
        g.fillRoundedRectangle (b, r);
    }
    else
    {
        g.setColour (ff::line);
        g.drawRoundedRectangle (b.reduced (1.0f), r, 2.0f);
    }
    g.setColour (on ? ff::bg : ff::ink);
    g.setFont (ff::font (textSize, 0.06f));
    g.drawText (text, b, Justification::centred, false);
}

void ToggleChip::mouseDown (const MouseEvent&)
{
    attachment->setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

// =====================================================================================
// Look and feel
// =====================================================================================
void FlipLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                        Slider::SliderStyle, Slider& s)
{
    const float cy = (float) y + (float) h * 0.5f;
    const float left = (float) x + 8.0f, right = (float) (x + w) - 8.0f;
    g.setColour (ff::line);
    g.fillRoundedRectangle (left, cy - 2.0f, right - left, 4.0f, 2.0f);
    g.setColour (ff::ink.withAlpha (s.isMouseOverOrDragging() ? 1.0f : 0.85f));
    g.fillRoundedRectangle (left, cy - 2.0f, jmax (0.0f, pos - left), 4.0f, 2.0f);
    g.setColour (ff::ink);
    g.fillEllipse (pos - 8.0f, cy - 8.0f, 16.0f, 16.0f);
    g.setColour (ff::bg);
    g.fillEllipse (pos - 2.5f, cy - 2.5f, 5.0f, 5.0f);
}

void FlipLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const float size = (float) jmin (w, h);
    const Point<float> c ((float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f);
    const float r = size * 0.5f - 1.0f;

    g.setColour (ff::ring);
    g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - r * 0.9f, c.y - r * 0.9f, r * 1.8f, r * 1.8f);

    g.setColour (ff::ink);
    for (int i = 0; i <= 10; ++i)
    {
        const float a = start + (end - start) * (float) i / 10.0f;
        const Point<float> p1 (c.x + std::sin (a) * r * 0.80f, c.y - std::cos (a) * r * 0.80f);
        const Point<float> p2 (c.x + std::sin (a) * r * 0.92f, c.y - std::cos (a) * r * 0.92f);
        g.drawLine ({ p1, p2 }, 2.0f);
    }

    const bool red = s.getProperties()["red"];
    g.setColour (red ? ff::red : ff::ink);
    const float a = start + pos * (end - start);
    Path hand;
    hand.addRoundedRectangle (-3.0f, -r * 0.68f, 6.0f, r * 0.68f + r * 0.15f, 2.0f);
    g.fillPath (hand, AffineTransform::rotation (a).translated (c));
    g.fillEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - 1.8f, c.y - 1.8f, 3.6f, 3.6f);
}

void FlipLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const float rad = r.getHeight() * 0.5f;
    g.setColour (down ? ff::card : ff::bg);
    g.fillRoundedRectangle (r, rad);
    g.setColour (highlighted || down ? ff::ink : ff::line);
    g.drawRoundedRectangle (r.reduced (1.0f), rad, 2.0f);
}

void FlipLookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool, bool)
{
    g.setColour (ff::ink);
    g.setFont (ff::font (14.0f, 0.08f));
    g.drawText (b.getButtonText(), b.getLocalBounds(), Justification::centred, false);
}

// =====================================================================================
// Dial and slider
// =====================================================================================
Dial::Dial (AudioProcessorValueTreeState& s, const String& paramId, const String& c, bool redHand) : caption (c), red (redHand)
{
    param = s.getParameter (paramId);
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider.setMouseCursor (MouseCursor::PointingHandCursor);
    slider.getProperties().set ("red", red);
    slider.setDoubleClickReturnValue (true, (double) param->convertFrom0to1 (param->getDefaultValue()));
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
    slider.onValueChange = [this] { repaint(); };
}

void Dial::setModulated (float position, bool active)
{
    if (active || modActive) repaint();
    modPos = position;
    modActive = active;
}

void Dial::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (20).withTrimmedBottom (24).withSizeKeepingCentre (80, 80));
}

void Dial::paint (Graphics& g)
{
    auto r = getLocalBounds();
    ff::label (g, caption.toUpperCase(), r.removeFromTop (18), Justification::centred);
    const String value = valueText.isNotEmpty() ? valueText : param->getCurrentValueAsText();
    ff::label (g, value.toUpperCase(), r.removeFromBottom (20), Justification::centred, ff::ink, 15.0f);

    // Where the LFO, envelope or a recorded movement has really put this knob
    if (modActive)
    {
        const auto sb = slider.getBounds().toFloat();
        const auto c = sb.getCentre();
        const float rad = sb.getWidth() * 0.5f + 5.0f;
        const float a = MathConstants<float>::pi * (1.25f + 1.5f * jlimit (0.0f, 1.0f, modPos)) - MathConstants<float>::twoPi;
        g.setColour (ff::red);
        g.fillEllipse (c.x + std::sin (a) * rad - 3.5f, c.y - std::cos (a) * rad - 3.5f, 7.0f, 7.0f);
    }
}

LabelledSlider::LabelledSlider (AudioProcessorValueTreeState& s, const String& paramId, const String& c) : caption (c)
{
    param = s.getParameter (paramId);
    slider.setSliderStyle (Slider::LinearHorizontal);
    slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    slider.setMouseCursor (MouseCursor::PointingHandCursor);
    slider.setDoubleClickReturnValue (true, (double) param->convertFrom0to1 (param->getDefaultValue()));
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, paramId, slider);
    slider.onValueChange = [this] { repaint(); };
}

void LabelledSlider::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (18));
}

void LabelledSlider::paint (Graphics& g)
{
    auto top = getLocalBounds().removeFromTop (16);
    ff::label (g, caption.toUpperCase(), top);
    ff::label (g, param->getCurrentValueAsText().toUpperCase(), top, Justification::centredRight, ff::ink);
}

// =====================================================================================
// VoiceDial: 12 stops round a Karlsson face, grouped Blur, Synth, Glitch, Swell, User
// =====================================================================================
static constexpr float kStopStep = 270.0f / 11.0f;
static float stopDegrees (int i) { return -135.0f + kStopStep * (float) i; }

VoiceDial::VoiceDial (JetstreamProcessor& p) : proc (p)
{
    voiceParam = p.apvts.getParameter (ParamIDs::voice);
    slotParam = p.apvts.getParameter (ParamIDs::presetSlot);
    setMouseCursor (MouseCursor::PointingHandCursor);
}

Point<float> VoiceDial::centre() const { return { getWidth() * 0.5f, getHeight() * 140.0f / 260.0f }; }
float VoiceDial::scale() const { return jmin ((float) getWidth() / 328.0f, (float) getHeight() / 260.0f); }

int VoiceDial::currentStop() const { return jlimit (0, 9, roundToInt (plainOf (voiceParam))); }

void VoiceDial::paint (Graphics& g)
{
    const float s = scale();
    const auto c = centre();
    auto at = [&] (float deg, float r) { const float a = degreesToRadians (deg); return Point<float> (c.x + std::sin (a) * r * s, c.y - std::cos (a) * r * s); };

    const int stop = currentStop();
    const int slot = roundToInt (plainOf (slotParam));
    const int group = stop <= 3 ? 0 : (stop <= 5 ? 1 : (stop <= 7 ? 2 : 3));

    // group arcs round the rim
    struct Arc { const char* name; int a, b; float labelDeg; Justification j; };
    const Arc arcs[5] = { { "BLUR", 0, 3, -101.0f, Justification::centredRight }, { "SYNTH", 4, 5, -27.0f, Justification::centred },
                          { "GLITCH", 6, 7, 27.0f, Justification::centred }, { "SWELL", 8, 9, 72.0f, Justification::centredLeft },
                          { "USER", 10, 11, 125.0f, Justification::centred } };
    for (int i = 0; i < 5; ++i)
    {
        const bool lit = i < 4 ? group == i : slot != 1;
        Path arc;
        const float a0 = degreesToRadians (stopDegrees (arcs[i].a) - 9.0f), a1 = degreesToRadians (stopDegrees (arcs[i].b) + 9.0f);
        arc.addCentredArc (c.x, c.y, 114.0f * s, 114.0f * s, 0.0f, a0, a1, true);
        g.setColour (lit ? ff::ink : ff::line);
        g.strokePath (arc, PathStrokeType (3.0f * s, PathStrokeType::curved, PathStrokeType::rounded));

        const auto lp = at (arcs[i].labelDeg, 132.0f);
        Rectangle<float> box (lp.x - 40 * s, lp.y - 9 * s, 80 * s, 18 * s);
        if (arcs[i].j == Justification::centredRight) box = box.withX (lp.x - 72 * s);
        if (arcs[i].j == Justification::centredLeft) box = box.withX (lp.x - 8 * s);
        g.setColour (lit ? ff::ink : ff::dim);
        g.setFont (ff::font (13.0f * s, 0.08f));
        g.drawText (arcs[i].name, box, arcs[i].j, false);
    }

    g.setColour (ff::ring);
    g.fillEllipse (c.x - 106 * s, c.y - 106 * s, 212 * s, 212 * s);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - 100 * s, c.y - 100 * s, 200 * s, 200 * s);

    static const char* stopNames[12] = { "0", "-1", "+1", "\xc2\xb1" "1", "A", "B", "A", "B", "A", "B", "A", "B" };
    for (int i = 0; i < 12; ++i)
    {
        const float deg = stopDegrees (i);
        g.setColour (ff::ink);
        g.drawLine (Line<float> (at (deg, 92), at (deg, 102)), 4.0f * s);
        const auto lp = at (deg, 76);
        const bool lit = i < 10 ? i == stop : (slot == (i == 10 ? 0 : 2));
        g.setColour (lit ? ff::ink : ff::dim);
        g.setFont (ff::font (15.0f * s, 0.0f));
        g.drawText (String::fromUTF8 (stopNames[i]), Rectangle<float> (lp.x - 16 * s, lp.y - 9 * s, 32 * s, 18 * s), Justification::centred, false);
    }

    // a dot under a User stop that holds a saved preset
    for (int i = 0; i < 2; ++i)
        if (proc.hasPresetSlot (i == 0 ? 0 : 2))
        {
            const auto dp = at (stopDegrees (10 + i), 62);
            g.setColour (ff::dim);
            g.fillEllipse (dp.x - 2 * s, dp.y - 2 * s, 4 * s, 4 * s);
        }

    Path needle;
    needle.addRectangle (-1.4f * s, -62.0f * s, 2.8f * s, 80.0f * s);
    g.setColour (ff::red);
    g.fillPath (needle, AffineTransform::rotation (degreesToRadians (stopDegrees (stop))).translated (c));
    g.fillEllipse (c.x - 7 * s, c.y - 7 * s, 14 * s, 14 * s);
    g.setColour (ff::bg);
    g.fillEllipse (c.x - 2.6f * s, c.y - 2.6f * s, 5.2f * s, 5.2f * s);
}

int VoiceDial::stopAt (Point<float> p) const
{
    const auto c = centre();
    const float dist = p.getDistanceFrom (c) / scale();
    if (dist < 30.0f || dist > 150.0f) return -1;
    const float deg = radiansToDegrees (std::atan2 (p.x - c.x, c.y - p.y));
    const int i = roundToInt ((deg + 135.0f) / kStopStep);
    return (i >= 0 && i <= 11) ? i : -1;
}

void VoiceDial::pick (int stop)
{
    if (stop < 0) return;
    if (stop >= 10) // User A / B recall the presets saved on the A and B buttons
    {
        const int slot = stop == 10 ? 0 : 2;
        setParam (slotParam, roundToInt (plainOf (slotParam)) == slot ? 1.0f : (float) slot);
    }
    else
    {
        setParam (voiceParam, (float) stop);
    }
    repaint();
}

void VoiceDial::mouseDown (const MouseEvent& e)
{
    dragStop = stopAt (e.position);
    dragStart = (float) currentStop();
    pick (dragStop);
}

void VoiceDial::mouseDrag (const MouseEvent& e)
{
    // Drag up or down to step through the voices
    const int stop = jlimit (0, 9, roundToInt (dragStart - (float) e.getDistanceFromDragStartY() / 22.0f));
    if (std::abs (e.getDistanceFromDragStartY()) > 6 && stop != currentStop())
        pick (stop);
}

void VoiceDial::mouseUp (const MouseEvent&) { dragStop = -1; }

void VoiceDial::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    pick (jlimit (0, 9, currentStop() + (w.deltaY > 0 ? 1 : -1)));
}

// =====================================================================================
// Sampler buttons (A, B) and lights
// =====================================================================================
SamplerButton::SamplerButton (JetstreamProcessor& p, int ch) : proc (p), channel (ch)
{
    param = p.apvts.getParameter (ch == 0 ? ParamIDs::footA : ParamIDs::footB);
    setMouseCursor (MouseCursor::PointingHandCursor);
}

void SamplerButton::set (bool v) { setParam (param, v ? 1.0f : 0.0f); }
void SamplerButton::mouseDown (const MouseEvent&) { pressed = true; set (true); repaint(); }
void SamplerButton::mouseUp (const MouseEvent&) { pressed = false; set (false); repaint(); }

void SamplerButton::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const auto& eng = proc.getEngine();
    const float lvl = channel == 0 ? eng.uiEnvA.load() : eng.uiEnvB.load();
    const Colour col = channel == 0 ? ff::teal : ff::coral;
    const bool manual = roundToInt (proc.apvts.getRawParameterValue (ParamIDs::trigger)->load()) == 2;
    const bool sounding = lvl > 0.02f;

    g.setColour (pressed ? ff::card : ff::housing);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (sounding ? col : ff::line);
    g.drawRoundedRectangle (r.reduced (1.0f), 12.0f, 2.0f);

    const Point<float> led (r.getX() + 22.0f, r.getCentreY());
    g.setColour (col.withAlpha (0.22f * jlimit (0.0f, 1.0f, lvl * 1.5f)));
    g.fillEllipse (led.x - 12.0f, led.y - 12.0f, 24.0f, 24.0f);
    g.setColour (sounding ? col.interpolatedWith (ff::ink, 0.0f).withMultipliedBrightness (0.5f + 0.5f * jlimit (0.0f, 1.0f, lvl * 1.5f))
                          : col.withMultipliedBrightness (0.35f));
    g.fillEllipse (led.x - 7.0f, led.y - 7.0f, 14.0f, 14.0f);

    auto text = r.withTrimmedLeft (42.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (22.0f, 0.08f));
    g.drawText (channel == 0 ? "A" : "B", text.removeFromTop (r.getHeight() * 0.58f).withTrimmedTop (6.0f), Justification::bottomLeft, false);
    ff::label (g, manual ? (sounding ? "HOLDING" : "TAP TO CAPTURE") : (sounding ? "SOUNDING" : "READY"),
               text.toNearestInt(), Justification::topLeft, ff::dim, 11.0f);
}

void Lights::paint (Graphics& g)
{
    const auto& eng = proc.getEngine();
    const float a = jlimit (0.0f, 1.0f, eng.uiEnvA.load() * 1.4f);
    const float b = jlimit (0.0f, 1.0f, eng.uiEnvB.load() * 1.4f);
    const float in = jlimit (0.0f, 1.0f, eng.uiInput.load() * 3.0f);
    const float mod = eng.uiDim.load();
    const float cy = getHeight() * 0.5f;
    float x = (getWidth() - 266.0f) * 0.5f;

    auto led = [&] (float cx, float size, Colour col, float amount)
    {
        g.setColour (col.withAlpha (0.22f * amount));
        g.fillEllipse (cx - size * 0.5f - 5.0f, cy - size * 0.5f - 5.0f, size + 10.0f, size + 10.0f);
        g.setColour (col.withMultipliedBrightness (0.3f + 0.7f * amount));
        g.fillEllipse (cx - size * 0.5f, cy - size * 0.5f, size, size);
    };
    auto dots = [&] (float x0)
    {
        g.setColour (ff::line);
        for (int i = 0; i < 6; ++i) g.fillEllipse (x0 + (float) i * 5.0f, cy - 1.0f, 2.0f, 2.0f);
    };

    ff::label (g, "A", Rectangle<float> (x, 0, 14, (float) getHeight()).toNearestInt(), Justification::centred, ff::teal, 14.0f);
    led (x + 34.0f, 18.0f, ff::teal, a);
    dots (x + 50.0f);
    led (x + 87.0f, 10.0f, ff::ink, in);
    dots (x + 97.0f);
    led (x + 140.0f, 18.0f, ff::coral, b);
    ff::label (g, "B", Rectangle<float> (x + 156.0f, 0, 14, (float) getHeight()).toNearestInt(), Justification::centred, ff::coral, 14.0f);
    led (x + 196.0f, 12.0f, ff::red, 0.25f + 0.75f * mod);
    ff::label (g, "MOD", Rectangle<float> (x + 210.0f, 0, 56, (float) getHeight()).toNearestInt(), Justification::centredLeft, ff::dim, 13.0f);
}

// =====================================================================================
// Rec, Learn, presets, bypass
// =====================================================================================
RecButton::RecButton (JetstreamProcessor& p, int t, const String& s) : proc (p), target (t), text (s)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
}

void RecButton::mouseDown (const MouseEvent&) { proc.toggleRecording (target); repaint(); }

void RecButton::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float r = b.getHeight() * 0.5f;
    const int state = proc.recordingState (target);
    const float prog = proc.recordingProgress (target);

    if (state == JetstreamProcessor::RecRecording)
    {
        g.setColour (ff::red);
        g.fillRoundedRectangle (b, r);
        g.setColour (Colours::black.withAlpha (0.25f));
        g.fillRoundedRectangle (b.withWidth (b.getWidth() * prog), r);
    }
    else
    {
        g.setColour (state == JetstreamProcessor::RecPlaying ? ff::ink : ff::line);
        g.drawRoundedRectangle (b.reduced (1.0f), r, 2.0f);
    }

    const float pulse = state == JetstreamProcessor::RecPlaying ? 0.5f + 0.5f * (float) std::sin (nowSeconds() * 6.0) : 1.0f;
    g.setColour (state == JetstreamProcessor::RecRecording ? ff::ink : ff::red.withAlpha (pulse));
    const float tw = GlyphArrangement::getStringWidth (ff::font (13.0f, 0.06f), text);
    g.fillEllipse (b.getCentreX() - tw * 0.5f - 9.0f, b.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (13.0f, 0.06f));
    g.drawText (text, b.withTrimmedLeft (12.0f), Justification::centred, false);
}

void LearnButton::paint (Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float r = b.getHeight() * 0.5f;
    const int st = proc.learnState();
    const String text = st == 1 ? "LISTENING" : (st == 2 ? "SET" : (st == 3 ? "NO SIGNAL" : "LEARN"));
    g.setColour (st == 1 ? ff::ink : ff::line);
    g.drawRoundedRectangle (b.reduced (1.0f), r, 2.0f);
    const float pulse = st == 1 ? 0.4f + 0.6f * (float) std::abs (std::sin (nowSeconds() * 5.0)) : 1.0f;
    g.setColour (ff::red.withAlpha (pulse));
    const float tw = GlyphArrangement::getStringWidth (ff::font (14.0f, 0.08f), text);
    g.fillEllipse (b.getCentreX() - tw * 0.5f - 10.0f, b.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (ff::ink);
    g.setFont (ff::font (14.0f, 0.08f));
    g.drawText (text, b.withTrimmedLeft (12.0f), Justification::centred, false);
}

PresetToggle::PresetToggle (JetstreamProcessor& p) : proc (p)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
}

Rectangle<float> PresetToggle::cell (int i) const
{
    const float w[3] = { 40.0f, 52.0f, 40.0f };
    float x = 0;
    for (int k = 0; k < i; ++k) x += w[k] + 4.0f;
    return { x, 0.0f, w[i], (float) getHeight() - 8.0f };
}

void PresetToggle::paint (Graphics& g)
{
    const int sel = (int) std::lround (proc.apvts.getRawParameterValue (ParamIDs::presetSlot)->load());
    static const char* labels[3] = { "A", "LIVE", "B" };
    for (int i = 0; i < 3; ++i)
    {
        auto b = cell (i);
        const bool on = i == sel;
        const bool flash = i == savedCell && nowSeconds() < savedFlashUntil;
        g.setColour (flash ? ff::red : (on ? ff::ink : ff::card));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (Colours::black.withAlpha (on ? 0.3f : 0.85f));
        g.fillRect (b.getX(), b.getCentreY() - 0.75f, 7.0f, 1.5f);
        g.fillRect (b.getRight() - 7.0f, b.getCentreY() - 0.75f, 7.0f, 1.5f);
        g.setColour (on && ! flash ? ff::bg : ff::ink);
        g.setFont (ff::font (i == 1 ? 13.0f : 15.0f, 0.06f));
        g.drawText (flash ? "SAVED" : labels[i], b, Justification::centred, false);

        if (i != 1 && proc.hasPresetSlot (i))
        {
            g.setColour (ff::dim);
            g.fillEllipse (b.getCentreX() - 2.0f, b.getBottom() + 3.0f, 4.0f, 4.0f);
        }
    }
}

void PresetToggle::mouseDown (const MouseEvent& e)
{
    pressedCell = -1;
    for (int i = 0; i < 3; ++i)
        if (cell (i).contains (e.position)) pressedCell = i;
    downAt = nowSeconds();
    saved = false;
    startTimer (40);
}

void PresetToggle::timerCallback()
{
    if (pressedCell >= 0 && pressedCell != 1 && ! saved && nowSeconds() - downAt > 0.8)
    {
        proc.savePresetSlot (pressedCell);
        saved = true;
        savedCell = pressedCell;
        savedFlashUntil = nowSeconds() + 0.9;
    }
    if (pressedCell < 0 && nowSeconds() > savedFlashUntil)
        stopTimer();
    repaint();
}

void PresetToggle::mouseUp (const MouseEvent&)
{
    if (pressedCell >= 0 && ! saved)
        setParam (proc.apvts.getParameter (ParamIDs::presetSlot), (float) pressedCell);
    pressedCell = -1;
    repaint();
}

BypassButton::BypassButton (JetstreamProcessor& p) : proc (p)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*p.apvts.getParameter (ParamIDs::bypass), [this] (float v) { bypassed = v > 0.5f; repaint(); });
    attachment->sendInitialUpdate();
}

void BypassButton::mouseDown (const MouseEvent&) { attachment->setValueAsCompleteGesture (bypassed ? 0.0f : 1.0f); }

void BypassButton::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = ! bypassed;
    g.setColour (ff::housing);
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (on ? ff::ink : ff::line);
    g.drawRoundedRectangle (r.reduced (1.0f), 12.0f, 2.0f);

    const Point<float> led (r.getX() + 25.0f, r.getCentreY());
    if (on)
    {
        g.setColour (ff::red.withAlpha (0.22f));
        g.fillEllipse (led.x - 12.0f, led.y - 12.0f, 24.0f, 24.0f);
        g.setColour (ff::red);
    }
    else
    {
        g.setColour (ff::pin);
    }
    g.fillEllipse (led.x - 7.0f, led.y - 7.0f, 14.0f, 14.0f);

    g.setColour (on ? ff::ink : ff::dim);
    g.setFont (ff::font (20.0f, 0.08f));
    g.drawText (on ? "ON" : "BYPASSED", r.withTrimmedLeft (46.0f), Justification::centredLeft, false);

    auto cap = r.withTrimmedRight (16.0f);
    g.setColour (ff::dim);
    g.setFont (ff::font (12.0f, 0.06f));
    g.drawText ("TAP BYPASS", cap.withTrimmedBottom (cap.getHeight() * 0.5f).withTrimmedTop (8.0f), Justification::centredRight, false);
    g.drawText ("TAILS RING OUT", cap.withTrimmedTop (cap.getHeight() * 0.5f).withTrimmedBottom (8.0f), Justification::centredRight, false);
}

// =====================================================================================
// MemoryStrip and Pebble
// =====================================================================================
MemoryStrip::MemoryStrip (JetstreamProcessor& p) : proc (p), hold (p.apvts, ParamIDs::hold, "HOLD", 14.0f)
{
    addAndMakeVisible (hold);
    addAndMakeVisible (clear);
    clear.onClick = [this] { proc.clearMemory(); };
}

Rectangle<float> MemoryStrip::cellBounds (int i) const
{
    return { 240.0f + (float) i * 72.0f, ((float) getHeight() - 60.0f) * 0.5f, 64.0f, 60.0f };
}

void MemoryStrip::resized()
{
    hold.setBounds (682, (getHeight() - 44) / 2, 84, 44);
    clear.setBounds (774, (getHeight() - 44) / 2, 84, 44);
}

void MemoryStrip::mouseDown (const MouseEvent& e)
{
    // Clicking a cell points Dimension at it
    for (int i = 0; i < jet::JetEngine::kBlocks; ++i)
        if (cellBounds (i).contains (e.position))
            setParam (proc.apvts.getParameter (ParamIDs::dimension), ((float) i + 0.5f) / (float) jet::JetEngine::kBlocks);
}

void MemoryStrip::update (double t, double dt)
{
    now = t;
    const auto& eng = proc.getEngine();
    level = eng.uiInput.load();
    envA = eng.uiEnvA.load();
    envB = eng.uiEnvB.load();
    held = proc.apvts.getRawParameterValue (ParamIDs::hold)->load() > 0.5f;
    bypassed = proc.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f;
    if (level > 0.004f || envA > 0.01f || envB > 0.01f) lastSound = t;
    asleep = bypassed || t - lastSound > 5.0;

    filled = 0;
    for (int b = 0; b < jet::JetEngine::kBlocks; ++b) filled += eng.uiFilled[b].load() ? 1 : 0;
    const int writing = eng.uiWriting.load();
    for (int b = 0; b < jet::JetEngine::kBlocks; ++b)
        flash[b] = b == writing ? 1.0f : flash[b] * (float) std::exp (-dt / 0.3);

    const int caps = eng.uiCaptures.load();
    if (caps != lastCaptures) { hop = 1.0f; lastCaptures = caps; }
    hop *= (float) std::exp (-dt / 0.12);

    const float lookTarget = envA > envB + 0.05f ? -0.8f : (envB > envA + 0.05f ? 0.8f : 0.0f);
    look += (lookTarget - look) * (float) (1.0 - std::exp (-dt / 0.15));
    const float mouthTarget = jlimit (0.0f, 1.0f, (level - 0.02f) * 5.0f);
    mouth += (mouthTarget - mouth) * (float) (1.0 - std::exp (-dt / 0.06));

    if (t > nextBlink)
    {
        blinkUntil = t + 0.13;
        nextBlink = t + 2.5 + Random::getSystemRandom().nextDouble() * 3.0;
    }
    blinking = t < blinkUntil;

    const bool glitchB = roundToInt (proc.apvts.getRawParameterValue (ParamIDs::voice)->load()) == jet::VGlitchB;
    status = String (filled) + " OF 6";
    detail = held ? "HELD" : (writing >= 0 ? "RECORDING" : (glitchB ? "SAVES WITH SET" : "GLITCH B ONLY"));
    repaint();
}

void MemoryStrip::paintPebble (Graphics& g, Rectangle<float> a)
{
    // Pebble from Flip Fella. Glows teal while A sounds, coral while B sounds,
    // holds its breath while the memory is held and dozes when bypassed.
    const float s = a.getWidth() / 140.0f;
    const float bob = asleep ? 0.0f : 1.5f * (float) std::sin (now * 2.4);
    const float breathe = asleep ? 1.0f + 0.025f * (float) std::sin (now * 1.6) : 1.0f;
    const float dy = bob - hop * 12.0f;
    const auto tf = AffineTransform::translation (-100.0f, -100.0f).scaled (s * breathe, s / breathe)
                        .translated (a.getCentreX(), a.getCentreY() + 6.0f * s);
    auto ell = [&] (float cx, float cy, float rx, float ry, Colour col)
    {
        Path p; p.addEllipse (cx - rx, cy - ry, rx * 2, ry * 2);
        g.setColour (col); g.fillPath (p, tf);
    };
    auto stroke = [&] (Path& p, float w, Colour col = ff::faceInk)
    {
        g.setColour (col);
        g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded), tf);
    };

    // glow ring
    const float glowA = jlimit (0.0f, 1.0f, envA * 1.5f), glowB = jlimit (0.0f, 1.0f, envB * 1.5f);
    if (glowA + glowB > 0.02f)
    {
        Path ringPath; ringPath.addEllipse (100 - 66, 100 - 56 + dy, 132, 112);
        const Colour col = glowA >= glowB ? ff::teal : ff::coral;
        stroke (ringPath, 5.0f, col.withAlpha (0.65f * jmax (glowA, glowB)));
    }

    ell (107, 109 + dy, 58, 48, ff::blobShade);
    ell (100, 100 + dy, 58, 48, ff::blob);

    if (asleep || blinking)
    {
        for (float ex : { 80.0f, 120.0f })
        {
            Path lid; lid.startNewSubPath (ex - 12, 94 + dy); lid.quadraticTo (ex, 102 + dy, ex + 12, 94 + dy);
            stroke (lid, 6.0f);
        }
    }
    else
    {
        for (float ex : { 80.0f, 120.0f })
        {
            ell (ex, 92 + dy, 14.0f, 14.0f, ff::eye);
            ell (ex + look * 5.0f, 94 + dy, 7.0f, 7.0f, ff::faceInk);
        }
    }

    if (asleep)
    {
        Path m; m.startNewSubPath (94, 124 + dy); m.lineTo (106, 124 + dy);
        stroke (m, 6.0f);
    }
    else if (held) // holding its breath
    {
        ell (100, 123 + dy, 6.0f, 6.0f, ff::faceInk);
    }
    else if (mouth > 0.15f)
    {
        ell (100, 124 + dy, 7.0f + 3.0f * mouth, 4.0f + 8.0f * mouth, ff::faceInk);
    }
    else
    {
        Path m; m.startNewSubPath (90, 120 + dy); m.quadraticTo (100, 130 + dy, 110, 120 + dy);
        stroke (m, 6.0f);
    }

    if (asleep)
    {
        g.setFont (ff::font (15.0f, 0.0f));
        for (int k = 0; k < 3; ++k)
        {
            const float ph = (float) std::fmod (now * 0.5 + k / 3.0, 1.0);
            g.setColour (ff::ink.withAlpha (1.0f - ph));
            g.drawText ("z", Rectangle<float> (a.getRight() - 14.0f + ph * 8.0f, a.getY() + 10.0f - ph * 18.0f, 16.0f, 16.0f),
                        Justification::centred, false);
        }
    }
}

void MemoryStrip::paint (Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (ff::face);
    g.fillRoundedRectangle (r.toFloat(), 12.0f);

    auto inner = r.reduced (18, 14);
    paintPebble (g, inner.removeFromLeft (76).toFloat());
    inner.removeFromLeft (18);
    auto info = inner.removeFromLeft (120);
    ff::label (g, "MEMORY", info.removeFromTop (18));
    ff::label (g, status, info.removeFromTop (30), Justification::centredLeft, ff::ink, 22.0f);
    ff::label (g, detail, info.removeFromTop (18));

    const auto& eng = proc.getEngine();
    const bool glitchB = roundToInt (proc.apvts.getRawParameterValue (ParamIDs::voice)->load()) == jet::VGlitchB;
    const int sel = eng.uiSelected.load();
    for (int b = 0; b < jet::JetEngine::kBlocks; ++b)
    {
        auto c = cellBounds (b);
        const bool full = eng.uiFilled[b].load();
        if (full)
        {
            g.setColour (ff::housing);
            g.fillRoundedRectangle (c, 8.0f);
            const bool selected = glitchB && b == sel;
            g.setColour (selected ? ff::red : ff::line.interpolatedWith (ff::ink, flash[b]));
            g.drawRoundedRectangle (c.reduced (1.0f), 8.0f, 2.0f);
            const float barW = 4.0f, step = 8.0f;
            const float x0 = c.getCentreX() - (step * (jet::JetEngine::kOverview - 1) + barW) * 0.5f;
            g.setColour (ff::ink);
            for (int k = 0; k < jet::JetEngine::kOverview; ++k)
            {
                const float h = jmax (4.0f, eng.uiOverview[b][k].load() * 30.0f);
                g.fillRoundedRectangle (x0 + (float) k * step, c.getCentreY() - h * 0.5f, barW, h, 1.0f);
            }
        }
        else
        {
            Path border; border.addRoundedRectangle (c.reduced (1.0f), 8.0f);
            Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            PathStrokeType (2.0f).createDashedStroke (dashed, border, dashes, 2);
            g.setColour (flash[b] > 0.05f ? ff::ink.withAlpha (flash[b]) : ff::line);
            g.fillPath (dashed);
            ff::label (g, flash[b] > 0.5f ? "REC" : "EMPTY", c.toNearestInt(), Justification::centred, ff::dim, 11.0f);
        }
    }
}
} // namespace ui

// =====================================================================================
// Editor
// =====================================================================================
using namespace ui;

JetstreamEditor::JetstreamEditor (JetstreamProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      inputMode (p.apvts, ParamIDs::inputMode, { "GUITAR", "SYNTH", "BASS" }),
      learn (p),
      sensitivity (p.apvts, ParamIDs::sensitivity, "Sens"),
      presets (p),
      trigger (p.apvts, ParamIDs::trigger, { "POLY", "MONO", "MANUAL" }),
      fsMode (p.apvts, ParamIDs::fsMode, { "TOGGLE", "LATCH", "MOMENT" }, 13.0f),
      envShape (p.apvts, ParamIDs::envShape, "Env shape"),
      envTime (p.apvts, ParamIDs::envTime, "Env time"),
      buttonA (p, 0), buttonB (p, 1),
      voiceDial (p),
      lights (p),
      dimension (p.apvts, ParamIDs::dimension, "Dimension", true),
      lfoDepth (p.apvts, ParamIDs::lfoDepth, "LFO depth"),
      lfoRate (p.apvts, ParamIDs::lfoRate, "LFO rate"),
      lfoShape (p.apvts, ParamIDs::lfoShape, "LFO shape"),
      dimSource (p.apvts, ParamIDs::dimSource, { "LFO", "ENV", "OFF" }),
      sync (p.apvts, ParamIDs::sync, "SYNC"),
      recDim (p, JetstreamProcessor::RecDimension, "REC DIM"),
      recDrive (p, JetstreamProcessor::RecDrive, "REC DRV"),
      autoLevel (p.apvts, ParamIDs::autoLevel, "AUTO LEVEL", 12.0f),
      drive (p.apvts, ParamIDs::drive, "Drive"),
      tone (p.apvts, ParamIDs::tone, "Tone"),
      dry (p.apvts, ParamIDs::dry, "Dry"),
      master (p.apvts, ParamIDs::master, "Master"),
      width (p.apvts, ParamIDs::width, "Width"),
      air (p.apvts, ParamIDs::air, "Air"),
      strip (p),
      bypass (p)
{
    setLookAndFeel (&lnf);
    lnf.setColour (TextButton::buttonColourId, ff::bg);
    lnf.setColour (TextButton::textColourOffId, ff::ink);

    triggerCard.setTextHeight (42.0f);
    groupCard.setTextHeight (42.0f);
    variantCard.setTextHeight (42.0f);

    for (auto* c : std::initializer_list<Component*> { &inputMode, &learn, &sensitivity, &presets, &triggerCard, &trigger, &fsMode,
                                                       &envShape, &envTime, &buttonA, &buttonB, &groupCard, &variantCard, &voiceDial,
                                                       &lights, &dimension, &lfoDepth, &lfoRate, &lfoShape, &dimSource, &sync, &recDim,
                                                       &recDrive, &autoLevel, &drive, &tone, &dry, &master, &width, &air, &strip, &bypass })
        addAndMakeVisible (c);

    setSize (1260, 648);
    lastTick = Time::getMillisecondCounterHiRes() * 0.001;
    startTimerHz (60);
}

JetstreamEditor::~JetstreamEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void JetstreamEditor::section (Graphics& g, Rectangle<int> r) const
{
    g.setColour (ff::face);
    g.fillRoundedRectangle (r.toFloat(), 12.0f);
}

void JetstreamEditor::paint (Graphics& g)
{
    g.fillAll (ff::bg);
    g.setColour (ff::ink);
    g.setFont (ff::font (26.0f, 0.04f));
    g.drawText ("JETSTREAM", 20, 20, 150, 40, Justification::centredLeft, false);
    ff::label (g, "SAMPLED SUSTAIN, SILKY STEREO", { 172, 20, 280, 40 });
    ff::label (g, "INPUT", { 488, 20, 50, 40 });
    ff::label (g, "MIDI + AUDIO", { 996, 20, 96, 40 });

    section (g, { 20, 76, 270, 440 });
    section (g, { 310, 76, 350, 440 });
    section (g, { 680, 76, 270, 440 });
    section (g, { 970, 76, 270, 440 });

    ff::label (g, "SAMPLERS", { 36, 92, 200, 16 });
    ff::label (g, "VOICE", { 326, 92, 200, 16 });
    ff::label (g, "MOTION", { 696, 92, 100, 16 });
    ff::label (g, String::fromUTF8 ("DIMENSION · ") + dimMeaning, { 760, 92, 174, 16 }, Justification::centredRight, ff::ink);
    ff::label (g, "OUTPUT", { 986, 92, 100, 16 });
}

void JetstreamEditor::resized()
{
    // header
    inputMode.setBounds (540, 24, 210, 32);
    learn.setBounds (762, 24, 100, 32);
    sensitivity.setBounds (872, 22, 116, 36);
    presets.setBounds (1100, 24, 140, 40);

    // samplers
    triggerCard.setBounds (36, 120, 238, 72);
    trigger.setBounds (36, 204, 238, 32);
    envShape.setBounds (42, 248, 110, 128);
    envTime.setBounds (158, 248, 110, 128);
    fsMode.setBounds (36, 388, 238, 32);
    buttonA.setBounds (36, 432, 114, 64);
    buttonB.setBounds (160, 432, 114, 64);

    // voice
    groupCard.setBounds (326, 120, 212, 72);
    variantCard.setBounds (544, 120, 100, 72);
    voiceDial.setBounds (335, 202, 300, 238);
    lights.setBounds (326, 452, 318, 28);

    // motion
    dimension.setBounds (700, 116, 110, 128);
    lfoDepth.setBounds (819, 116, 110, 128);
    lfoRate.setBounds (700, 252, 110, 128);
    lfoShape.setBounds (819, 252, 110, 128);
    dimSource.setBounds (696, 424, 238, 32);
    sync.setBounds (696, 464, 74, 32);
    recDim.setBounds (778, 464, 74, 32);
    recDrive.setBounds (860, 464, 74, 32);

    // output
    autoLevel.setBounds (1128, 86, 96, 24);
    drive.setBounds (990, 116, 110, 128);
    tone.setBounds (1109, 116, 110, 128);
    dry.setBounds (990, 252, 110, 128);
    master.setBounds (1109, 252, 110, 128);
    width.setBounds (986, 404, 238, 40);
    air.setBounds (986, 456, 238, 40);

    strip.setBounds (20, 532, 1220, 96);
    bypass.setBounds (1002, 548, 220, 64);
}

void JetstreamEditor::timerCallback()
{
    const double now = Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = jlimit (0.0, 0.1, now - lastTick);
    lastTick = now;
    auto& s = proc.apvts;
    const auto& eng = proc.getEngine();

    for (auto* c : { &triggerCard, &groupCard, &variantCard })
        c->advance (dt);

    // Flip cards
    static const char* triggerNames[] = { "POLY", "MONO", "MANUAL" };
    triggerCard.setValue (triggerNames[jlimit (0, 2, roundToInt (s.getRawParameterValue (ParamIDs::trigger)->load()))]);

    const int voice = jlimit (0, jet::kNumVoices - 1, roundToInt (s.getRawParameterValue (ParamIDs::voice)->load()));
    const int slot = roundToInt (s.getRawParameterValue (ParamIDs::presetSlot)->load());
    static const char* groupNames[] = { "BLUR", "BLUR", "BLUR", "BLUR", "SYNTH", "SYNTH", "GLITCH", "GLITCH", "SWELL", "SWELL" };
    static const char* variantNames[] = { "0", "-1", "+1", "\xc2\xb1" "1", "A", "B", "A", "B", "A", "B" };
    if (slot != 1)
    {
        groupCard.setValue ("USER");
        variantCard.setValue (slot == 0 ? "A" : "B");
    }
    else
    {
        groupCard.setValue (groupNames[voice]);
        variantCard.setValue (String::fromUTF8 (variantNames[voice]));
    }

    // What Dimension does in this voice
    const int group = jet::groupOf (voice);
    const char* meaning = group == jet::GroupSynth ? "FILTER" : (group == jet::GroupGlitchA ? "LENGTH" : (group == jet::GroupGlitchB ? "MEMORY" : "SPACE"));
    if (dimMeaning != meaning) { dimMeaning = meaning; repaint (696, 88, 240, 24); }

    const float dimEff = eng.uiDim.load();
    if (group == jet::GroupGlitchA)
    {
        static const char* lens[] = { "45 MS", "90 MS", "180 MS", "360 MS" };
        dimension.setValueText (lens[jlimit (0, 3, (int) (std::min (dimEff, 0.999f) * 4.0f))]);
    }
    else if (group == jet::GroupGlitchB)
    {
        dimension.setValueText ("CELL " + String (eng.uiSelected.load() + 1));
    }
    else
    {
        dimension.setValueText ({});
    }
    const float dimKnob = s.getRawParameterValue (ParamIDs::dimension)->load();
    const bool dimMoving = std::abs (dimEff - dimKnob) > 0.01f;
    dimension.setModulated (dimEff, dimMoving);

    // Tempo-synced rate reads as a note length
    if (proc.currentBpm() > 0.0f)
        lfoRate.setValueText (names::divisions[names::divisionIndex (s.getRawParameterValue (ParamIDs::lfoRate)->load())]);
    else
        lfoRate.setValueText ({});

    strip.update (now, dt);
    buttonA.repaint();
    buttonB.repaint();
    lights.repaint();
    learn.repaint();
    recDim.repaint();
    recDrive.repaint();
    voiceDial.repaint();
}

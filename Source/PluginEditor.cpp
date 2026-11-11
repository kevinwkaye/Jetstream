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
    const int slot = nowSeconds() < userLitUntil ? userLit : 1; // 0 User A, 2 User B, 1 neither
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
        if (proc.hasUser (i))
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
    if (stop >= 10) return; // User A and B act on release: tap recalls, hold stores
    else
    {
        setParam (voiceParam, (float) stop);
    }
    repaint();
}

void VoiceDial::mouseDown (const MouseEvent& e)
{
    proc.touchPanel();
    downAt = nowSeconds();
    dragStop = stopAt (e.position);
    dragStart = (float) currentStop();
    pick (dragStop);
}

void VoiceDial::mouseDrag (const MouseEvent& e)
{
    // Drag up or down to step through the voices
    const int stop = jlimit (0, 9, roundToInt (dragStart - (float) e.getDistanceFromDragStartY() / 22.0f));
    if (std::abs (e.getDistanceFromDragStartY()) > 6 && stop != currentStop())
    {
        dragStop = -1; // dragging off a User stop doesn't recall it
        pick (stop);
    }
}

void VoiceDial::mouseUp (const MouseEvent&)
{
    if (dragStop >= 10)
    {
        const int slot = dragStop - 10;
        if (nowSeconds() - downAt > 0.8) proc.storeUser (slot);
        else proc.recallUser (slot);
        userLit = slot == 0 ? 0 : 2;
        userLitUntil = nowSeconds() + 2.0;
        repaint();
    }
    dragStop = -1;
}

void VoiceDial::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    pick (jlimit (0, 9, currentStop() + (w.deltaY > 0 ? 1 : -1)));
}

// =====================================================================================
// Sampler buttons (A, B) and lights
// =====================================================================================
SamplerButton::SamplerButton (JetstreamProcessor& p, int ch) : proc (p), channel (ch)
{
    setMouseCursor (MouseCursor::PointingHandCursor);
}

// these are footswitches A and B, so tap and hold do exactly what the real ones do
void SamplerButton::mouseDown (const MouseEvent&) { pressed = true; proc.footDown (channel); repaint(); }
void SamplerButton::mouseUp (const MouseEvent&) { pressed = false; proc.footUp (channel); repaint(); }

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
    ff::label (g, manual ? (sounding ? "HOLDING" : "TAP TO CAPTURE") : (sounding ? "HOLD TO SUSTAIN" : "TAP TO CATCH"),
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

// =====================================================================================
// Preset strip
// =====================================================================================
static const String dotChar (CharPointer_UTF8 ("\xc2\xb7"));

static void wrapText (Graphics& g, const String& t, Rectangle<int> r, float h, Colour c, int lines)
{
    g.setColour (c);
    g.setFont (Font (FontOptions (h)));
    g.drawFittedText (t, r, Justification::topLeft, lines, 1.0f);
}

PresetStrip::PresetStrip (JetstreamProcessor& p) : proc (p)
{
    for (auto* c : std::initializer_list<Component*> { &prev, &next, &card, &save }) addAndMakeVisible (c);
    prev.set (CharPointer_UTF8 ("\xe2\x97\x80"), {}, fam::col::line);
    next.set (CharPointer_UTF8 ("\xe2\x96\xb6"), {}, fam::col::line);
    prev.setTextHeight (12.0f);
    next.setTextHeight (12.0f);
    prev.onPress = [this] { proc.touchPanel(); proc.stepPreset (-1); };
    next.onPress = [this] { proc.touchPanel(); proc.stepPreset (1); };
    save.onClick = [this] { proc.saveUserPreset(); savedFlash = 1.5f; };
    startTimerHz (20);
}

void PresetStrip::resized()
{
    prev.setBounds (0, 4, 34, 32);
    card.setBounds (40, 0, 250, 40);
    next.setBounds (296, 4, 34, 32);
    save.setBounds (338, 4, 58, 32);
}

void PresetStrip::paint (Graphics&) {}

void PresetStrip::timerCallback()
{
    const int pi = proc.currentPreset();
    card.setText (String (pi + 1).paddedLeft ('0', 2) + " " + proc.presetName (pi) + (proc.presetEdited() ? " *" : ""));
    save.set (savedFlash > 0.0f ? "SAVED" : "SAVE", savedFlash > 0.0f, 12.0f);
    if (savedFlash > 0.0f) savedFlash -= 1.0f / 20.0f;
}

// =====================================================================================
// Footswitch strip
// =====================================================================================
FootStrip::FootStrip (JetstreamProcessor& p, std::function<void()> openPedalPage) : proc (p)
{
    for (int i = 0; i < 4; ++i)
    {
        feet.push_back (std::make_unique<fam::FootButton>());
        addAndMakeVisible (*feet.back());
        feet.back()->onPress = [this, i] { proc.footDown (i); };
        feet.back()->onRelease = [this, i] { proc.footUp (i); };
    }
    for (auto* c : std::initializer_list<Component*> { &keeper, &expCard, &expSlider, &pedalLink }) addAndMakeVisible (c);
    keeper.set ("KEEPER", "HOLD C+D, 1 S", fam::col::green);
    keeper.setTextHeight (17.0f);
    keeper.onPress = [this] { proc.touchPanel(); proc.keepNow(); };
    expSlider.onChange = [this] (float v)
    {
        proc.touchPanel();
        if (proc.get ("expOn") < 0.5f) proc.setPlain ("expOn", 1.0f);
        proc.setPlain ("exp", v);
    };
    pedalLink.text = String (CharPointer_UTF8 ("PEDAL PAGE \xe2\x80\xba"));
    pedalLink.onClick = std::move (openPedalPage);
    startTimerHz (30);
}

void FootStrip::mouseDown (const MouseEvent&) { proc.touchPanel(); }

void FootStrip::paint (Graphics& g)
{
    g.setColour (ff::face);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f);
    ff::label (g, "FOOTSWITCHES", { 16, 10, 200, 18 });
    ff::label (g, "TAP A+B " + dotChar + " BYPASS     TAP C+D " + dotChar + " NEXT JETSTREAM", { 220, 10, 400, 18 }, Justification::centredRight, ff::faint, 11.0f);
    ff::label (g, "EXPRESSION", { 636, 10, 160, 18 });
    ff::label (g, "HEEL " + proc.expressionRangeText (false) + " " + dotChar + " TOE " + proc.expressionRangeText (true), { 636, 80, 180, 16 }, Justification::centredLeft, ff::dim, 10.0f);
    if (proc.get ("expOn") < 0.5f) ff::label (g, "NOT MOVED YET", { 636, 118, 180, 16 }, Justification::centredLeft, ff::faint, 10.0f);
    ff::label (g, "TRY THIS", { 986, 10, 200, 18 }, Justification::centredLeft, ff::teal);
    wrapText (g, proc.presetHint (proc.currentPreset()), { 986, 34, 218, 70 }, 14.0f, ff::ink, 4);
}

void FootStrip::resized()
{
    for (int i = 0; i < 4; ++i) feet[(size_t) i]->setBounds (16 + i * 152, 34, 144, 100);
    expCard.setBounds (636, 34, 180, 40);
    expSlider.setBounds (632, 96, 188, 22);
    keeper.setBounds (836, 34, 132, 100);
    pedalLink.setBounds (1070, 112, 134, 18);
}

void FootStrip::timerCallback()
{
    const int down = proc.uiFootDown.load();
    const bool held = proc.get ("hold") > 0.5f;
    const auto& eng = proc.getEngine();
    const bool lit[4] = { eng.uiEnvA.load() > 0.02f, eng.uiEnvB.load() > 0.02f, held, false };
    const Colour fc[4] = { ff::teal, ff::coral, ff::red, ff::ink };
    for (int i = 0; i < 4; ++i)
    {
        String tap = proc.footJobTap (i);
        if (i == 2 && held) tap = "MEMORY HELD";
        feet[(size_t) i]->set (String::charToString ((juce_wchar) ('A' + i)), tap, proc.footJobHold (i), lit[i], fc[i]);
        feet[(size_t) i]->setDown ((down >> i) & 1);
    }
    const int et = jlimit (0, jet::ExpCount - 1, (int) std::lround (proc.get ("expTarget")));
    expCard.setText (jet::expTargetNames()[(size_t) et]);
    if (! expSlider.isMouseButtonDown()) expSlider.setValue (proc.uiExp.load());
    repaint (630, 76, 200, 60);
    repaint (980, 30, 240, 80);
}

// =====================================================================================
// Pedal page
// =====================================================================================
PedalPage::PedalPage (JetstreamProcessor& p, std::function<void()> close) : proc (p)
{
    setOpaque (true);
    back.text = String (CharPointer_UTF8 ("\xe2\x80\xb9 BACK TO THE PLUGIN"));
    back.onClick = std::move (close);
    for (auto* c : std::initializer_list<Component*> { &back, &expTargetCard, &mapCard, &expSlider, &calButton, &learnExp, &defaults,
                                                       &exportButton, &mapDown, &mapUp, &learnKeep, &pebble })
        addAndMakeVisible (c);
    for (int i = 0; i < 4; ++i)
    {
        tapCards.push_back (std::make_unique<fam::FlipCard> (15.0f));
        holdCards.push_back (std::make_unique<fam::FlipCard> (15.0f));
        learn.push_back (std::make_unique<fam::BigButton>());
        addAndMakeVisible (*tapCards.back());
        addAndMakeVisible (*holdCards.back());
        addAndMakeVisible (*learn.back());
        learn.back()->setTextHeight (12.0f);
        learn.back()->onPress = [this, i] { proc.learnFootswitch (i); };
    }
    auto pills = [this] (std::vector<std::unique_ptr<fam::PillButton>>& v, int n)
    { for (int i = 0; i < n; ++i) { v.push_back (std::make_unique<fam::PillButton>()); addAndMakeVisible (*v.back()); } };
    pills (targetPills, jet::ExpCount); pills (curvePills, 3); pills (smoothPills, 3); pills (inputPills, 2); pills (keeperPills, 2);
    for (int i = 0; i < jet::ExpCount; ++i)
        targetPills[(size_t) i]->onClick = [this, i]
        {
            // a new target starts on its full sweep; each preset keeps its own until you change it
            if ((int) std::lround (proc.get ("expTarget")) == i) return;
            proc.setPlain ("expTarget", (float) i);
            proc.setPlain ("expLo", 0.0f);
            proc.setPlain ("expHi", 1.0f);
        };
    for (int i = 0; i < 3; ++i)
    {
        curvePills[(size_t) i]->onClick = [this, i] { proc.pedals().curve = i; proc.saveSettings(); };
        smoothPills[(size_t) i]->onClick = [this, i] { proc.pedals().expSmooth = i; proc.saveSettings(); };
    }
    inputPills[0]->onClick = [this] { proc.setFootDevice ({}); };
    inputPills[1]->onClick = [this] { proc.setFootDevice ("none"); };
    keeperPills[0]->onClick = [this] { proc.setKeeperLive (true); };
    keeperPills[1]->onClick = [this] { proc.setKeeperLive (false); };

    expSlider.onChange = [this] (float v)
    {
        if (proc.get ("expOn") < 0.5f) proc.setPlain ("expOn", 1.0f);
        proc.setPlain ("exp", v);
    };
    calButton.setTextHeight (13.0f);
    calButton.onPress = [this] { proc.calibrateExpression(); };
    learnExp.setTextHeight (12.0f);
    learnExp.onPress = [this] { proc.learnExpression(); };
    learnKeep.setTextHeight (12.0f);
    learnKeep.onPress = [this] { proc.learnKeeper(); };
    defaults.set ("DEFAULTS", {}, fam::col::line);
    defaults.setTextHeight (12.0f);
    defaults.onPress = [this] { proc.resetFootswitches(); };
    exportButton.set ("EXPORT KEEPERS", {}, fam::col::green);
    exportButton.setTextHeight (13.0f);
    exportButton.onPress = [this] { proc.exportKeepers(); };
    mapDown.set (CharPointer_UTF8 ("\xe2\x97\x80"), {}, fam::col::line);
    mapUp.set (CharPointer_UTF8 ("\xe2\x96\xb6"), {}, fam::col::line);
    mapDown.setTextHeight (11.0f);
    mapUp.setTextHeight (11.0f);
    mapDown.onPress = [this] { proc.setPedalMapChannel ((proc.pedals().pedalMapChannel + 16) % 17); };
    mapUp.onPress = [this] { proc.setPedalMapChannel ((proc.pedals().pedalMapChannel + 1) % 17); };
    startTimerHz (20);
}

void PedalPage::paint (Graphics& g)
{
    g.fillAll (ff::bg);
    g.setColour (ff::ink);
    g.setFont (ff::font (26.0f, 0.04f));
    g.drawText ("JETSTREAM", 20, 20, 150, 40, Justification::centredLeft);
    ff::label (g, "PEDAL PAGE", { 176, 20, 200, 40 });

    for (auto r : { Rectangle<float> (20, 76, 788, 520), { 828, 76, 412, 520 }, { 20, 612, 1220, 168 } })
    {
        g.setColour (ff::face);
        g.fillRoundedRectangle (r, 12.0f);
    }

    auto& s = proc.pedals();
    ff::label (g, "FOOTSWITCHES", { 36, 92, 130, 30 });
    ff::label (g, deviceText, { 166, 92, 520, 30 }, Justification::centredLeft, proc.pedalsDirect() ? ff::teal : ff::dim, 12.0f);

    const int learning = proc.learningWhat();
    for (int i = 0; i < 4; ++i)
    {
        auto r = Rectangle<int> (36 + i * 192, 136, 180, 290);
        g.setColour (ff::bg);
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        if (learning == i) { g.setColour (ff::teal); g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 12.0f, 2.0f); }
        g.setColour (ff::ink);
        g.setFont (ff::font (30.0f, 0.02f));
        g.drawText (String::charToString ((juce_wchar) ('A' + i)), r.getX() + 12, r.getY() + 10, 30, 32, Justification::centredLeft);
        ff::label (g, "FOOTSWITCH " + String::charToString ((juce_wchar) ('A' + i)), { r.getX() + 44, r.getY() + 18, 130, 18 }, Justification::centredLeft, ff::dim, 11.0f);
        ff::label (g, "TAP", { r.getX() + 12, r.getY() + 48, 100, 14 }, Justification::centredLeft, ff::dim, 10.0f);
        ff::label (g, "HOLD", { r.getX() + 12, r.getY() + 120, 100, 14 }, Justification::centredLeft, ff::dim, 10.0f);
        const String listen = proc.pedalsDirect() ? "LISTENS TO CC " + String (s.fsCC[(size_t) i]) + ", ANY CHANNEL"
                                                  : "LISTENS TO CC " + String (s.fsCC[(size_t) i]) + " ON CHANNEL " + String (s.fsChannel[(size_t) i]);
        g.setColour (ff::dim);
        g.setFont (ff::font (11.0f, 0.05f));
        g.drawFittedText (listen, { r.getX() + 12, r.getY() + 192, 156, 30 }, Justification::topLeft, 2, 1.0f);
    }
    struct Combo { const char* name; const char* tap; const char* hold; };
    const Combo combos[2] = { { "A+B", "BYPASS, PRESS BOTH TOGETHER", nullptr }, { "C+D", "FOOTSWITCHES TO THE NEXT JETSTREAM", "KEEPER" } };
    for (int i = 0; i < 2; ++i)
    {
        auto r = Rectangle<int> (36 + i * 384, 438, 372, 52);
        g.setColour (ff::bg);
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        g.setColour (ff::ink);
        g.setFont (ff::font (20.0f, 0.04f));
        g.drawText (combos[i].name, r.getX() + 14, r.getY(), 56, r.getHeight(), Justification::centredLeft);
        ff::label (g, "TAP " + dotChar + " " + String (combos[i].tap), { r.getX() + 76, r.getY() + 8, 290, 18 }, Justification::centredLeft, ff::ink, 12.0f);
        if (combos[i].hold != nullptr)
            ff::label (g, "HOLD 1 S " + dotChar + " " + String (combos[i].hold), { r.getX() + 76, r.getY() + 26, 290, 18 }, Justification::centredLeft, ff::ink, 12.0f);
    }
    wrapText (g, "Any momentary footswitch works. Defaults: CC 20 to 23 on channel 16, or any channel when Jetstream reads the "
                 "footswitch's USB cable directly. Settings are shared by every Flip Fella plugin on this computer.",
              { 36, 504, 640, 44 }, 13.0f, ff::dim, 3);
    ff::label (g, "ORIGINAL MIDI MAP " + dotChar + " CC 80 A, 81 B, 82 BYPASS, 83 HOLD", { 36, 556, 420, 28 }, Justification::centredLeft, ff::dim, 11.0f);

    // expression
    ff::label (g, "EXPRESSION", { 844, 88, 200, 22 });
    ff::label (g, "HEEL " + proc.expressionRangeText (false) + " " + dotChar + " TOE " + proc.expressionRangeText (true), { 1026, 120, 200, 16 }, Justification::centredLeft, ff::ink, 11.0f);
    ff::label (g, "SAVED WITH EACH PRESET", { 1026, 136, 200, 16 }, Justification::centredLeft, ff::dim, 11.0f);
    ff::label (g, "TARGET", { 844, 160, 200, 16 }, Justification::centredLeft, ff::dim, 11.0f);
    ff::label (g, "PEDAL POSITION " + String (roundToInt (proc.uiExp.load() * 127.0f)) + " " + dotChar + " HEEL " + String (s.heel) + " " + dotChar + " TOE " + String (s.toe),
               { 844, 284, 380, 16 }, Justification::centredLeft, ff::dim, 11.0f);
    wrapText (g, calNote, { 844, 384, 380, 34 }, 13.0f, ff::dim, 2);
    ff::label (g, "CURVE", { 844, 420, 200, 18 }, Justification::centredLeft, ff::dim, 11.0f);
    ff::label (g, "SMOOTHING", { 844, 480, 200, 18 }, Justification::centredLeft, ff::dim, 11.0f);

    // bottom strip
    ff::label (g, "FOOTSWITCH INPUT", { 150, 628, 240, 18 });
    ff::label (g, proc.footInputStatus().toUpperCase(), { 150, 686, 270, 16 }, Justification::centredLeft, ff::faint, 10.0f);
    ff::label (g, "KEEPERS ARE TAGGED", { 432, 628, 240, 18 });
    ff::label (g, proc.lastKeeper() > 0 ? "LAST KEPT: NUMBER " + String (proc.lastKeeper()) : String ("NOTHING KEPT YET"), { 432, 686, 260, 16 }, Justification::centredLeft, ff::faint, 10.0f);
    wrapText (g, "In Ableton, switch Track, Sync and Remote off for the footswitch input. Jetstream reads it directly, "
                 "so every copy just works. Notes on a MIDI track still trigger the samplers.", { 940, 632, 284, 90 }, 13.0f, ff::dim, 5);
}

void PedalPage::resized()
{
    back.setBounds (1000, 20, 240, 40);
    for (int i = 0; i < 4; ++i)
    {
        const int x = 36 + i * 192;
        tapCards[(size_t) i]->setBounds (x + 12, 202, 156, 46);
        holdCards[(size_t) i]->setBounds (x + 12, 274, 156, 46);
        learn[(size_t) i]->setBounds (x + 12, 378, 156, 34);
    }
    defaults.setBounds (688, 506, 104, 34);
    mapDown.setBounds (466, 554, 36, 32);
    mapCard.setBounds (508, 550, 120, 40);
    mapUp.setBounds (634, 554, 36, 32);

    expTargetCard.setBounds (844, 116, 170, 40);
    for (int i = 0; i < jet::ExpCount; ++i)
        targetPills[(size_t) i]->setBounds (844 + (i % 4) * 96, 180 + (i / 4) * 32, 90, 26);
    expSlider.setBounds (840, 302, 388, 24);
    calButton.setBounds (844, 334, 380, 44);
    for (int i = 0; i < 3; ++i)
    {
        curvePills[(size_t) i]->setBounds (844 + i * 129, 442, 123, 30);
        smoothPills[(size_t) i]->setBounds (844 + i * 129, 502, 123, 30);
    }
    learnExp.setBounds (844, 546, 186, 34);
    learnKeep.setBounds (1038, 546, 186, 34);

    pebble.setBounds (36, 628, 92, 92);
    for (int i = 0; i < 2; ++i)
    {
        inputPills[(size_t) i]->setBounds (150 + i * 128, 650, 122, 28);
        keeperPills[(size_t) i]->setBounds (432 + i * 96, 650, 90, 28);
    }
    exportButton.setBounds (714, 644, 170, 44);
}

void PedalPage::timerCallback()
{
    if (! isVisible()) return;
    auto& s = proc.pedals();
    deviceText = proc.pedalsDirect() ? "READING YOUR FOOTSWITCH DIRECTLY " + dotChar + " READY"
                                     : (s.footDevice == "none" ? String ("THROUGH ABLETON ONLY") : String ("NO FOOTSWITCH FOUND, PLUG ONE IN"));
    const int learning = proc.learningWhat();
    for (int i = 0; i < 4; ++i)
    {
        tapCards[(size_t) i]->setText (proc.footJobTap (i));
        holdCards[(size_t) i]->setText (proc.footJobHold (i));
        learn[(size_t) i]->set (learning == i ? "PRESS IT NOW" : "LEARN", {}, learning == i ? fam::col::teal : fam::col::line);
    }
    const int t = jlimit (0, jet::ExpCount - 1, (int) std::lround (proc.get ("expTarget")));
    expTargetCard.setText (jet::expTargetNames()[(size_t) t]);
    for (int i = 0; i < jet::ExpCount; ++i) targetPills[(size_t) i]->set (jet::expTargetNames()[(size_t) i], i == t, 10.0f);
    if (! expSlider.isMouseButtonDown()) expSlider.setValue (proc.uiExp.load());
    calButton.set (proc.isCalibrating() ? "HEEL DOWN, THEN ROCK TO TOE" : "CALIBRATE HEEL AND TOE", {}, proc.isCalibrating() ? fam::col::teal : fam::col::line);
    calNote = proc.isCalibrating() ? "Listening for six seconds. Rock the pedal all the way, both ways."
                                   : "Fixes a pedal that doesn't reach 0 or 127, or one that runs backwards.";
    static const char* curves[3] = { "LINEAR", "SLOW START", "FAST START" };
    static const char* smooths[3] = { "OFF", "LOW", "HIGH" };
    for (int i = 0; i < 3; ++i)
    {
        curvePills[(size_t) i]->set (curves[i], s.curve == i, 11.0f);
        smoothPills[(size_t) i]->set (smooths[i], s.expSmooth == i, 11.0f);
    }
    learnExp.set (learning == JetstreamProcessor::LearnExpression ? "MOVE THE PEDAL NOW" : "LEARN EXPRESSION " + dotChar + " CC " + String (s.expCC), {},
                  learning == JetstreamProcessor::LearnExpression ? fam::col::teal : fam::col::line);
    learnKeep.set (learning == JetstreamProcessor::LearnKeeper ? "PRESS IT NOW" : (s.keeperCC >= 0 ? "KEEPER SWITCH " + dotChar + " CC " + String (s.keeperCC) : String ("LEARN A KEEPER SWITCH")), {},
                   learning == JetstreamProcessor::LearnKeeper ? fam::col::teal : fam::col::line);
    mapCard.setText (s.pedalMapChannel == 0 ? "OFF" : "CHANNEL " + String (s.pedalMapChannel), s.pedalMapChannel == 0 ? fam::col::dim : fam::col::ink);
    inputPills[0]->set ("READ DIRECTLY", s.footDevice != "none", 11.0f);
    inputPills[1]->set ("ABLETON ONLY", s.footDevice == "none", 11.0f);
    keeperPills[0]->set ("LIVE", s.keeperLive, 11.0f);
    keeperPills[1]->set ("STUDIO", ! s.keeperLive, 11.0f);
    if (proc.lastKeeper() != lastKept) { lastKept = proc.lastKeeper(); pebble.react (fam::Pebble::Kept); }
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
    // a footswitch press or a Keeper gets a hop too
    const int fp = proc.uiFootPulse.load(), kp = proc.uiKeepPulse.load();
    if (fp != lastFoot || kp != lastKeep) { hop = 1.0f; lastFoot = fp; lastKeep = kp; }
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
    message = t - proc.uiMessageAt < 2.5 ? proc.uiMessage : String();
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

    // what the last footswitch press did, for a moment
    if (message.isNotEmpty())
    {
        g.setColour (ff::teal);
        g.setFont (ff::font (12.0f, 0.06f));
        g.drawFittedText (message, { 868, 18, 118, getHeight() - 36 }, Justification::centredLeft, 3, 1.0f);
    }

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
      bypass (p),
      feet (p, [this] { pedalPage.setVisible (true); pedalPage.toFront (false); }),
      pedalPage (p, [this] { pedalPage.setVisible (false); })
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
                                                       &recDrive, &autoLevel, &drive, &tone, &dry, &master, &width, &air, &strip, &bypass, &feet })
        addAndMakeVisible (c);
    addChildComponent (pedalPage);

    setSize (kWidth, kHeight);
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
    presets.setBounds (176, 20, 520, 40);
    inputMode.setBounds (720, 24, 210, 32);
    learn.setBounds (940, 24, 100, 32);
    sensitivity.setBounds (1050, 22, 190, 36);

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
    feet.setBounds (20, 644, 1220, 144);
    pedalPage.setBounds (getLocalBounds());
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

    static const char* groupNames[] = { "BLUR", "BLUR", "BLUR", "BLUR", "SYNTH", "SYNTH", "GLITCH", "GLITCH", "SWELL", "SWELL" };
    static const char* variantNames[] = { "0", "-1", "+1", "\xc2\xb1" "1", "A", "B", "A", "B", "A", "B" };
    groupCard.setValue (groupNames[voice]);
    variantCard.setValue (String::fromUTF8 (variantNames[voice]));

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
    if (proc.uiBpm.load() > 0.0f)
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

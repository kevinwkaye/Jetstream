#pragma once

#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

// Flip Fella palette, shared across all of Kevin's builds.
namespace ff
{
const juce::Colour bg        { 0xff050505 };
const juce::Colour face      { 0xff0d0d0d };
const juce::Colour housing   { 0xff060606 };
const juce::Colour card      { 0xff1b1b1b };
const juce::Colour cardHi    { 0xff252525 };
const juce::Colour cardLow   { 0xff141414 };
const juce::Colour ink       { 0xfff3f0e8 };
const juce::Colour dim       { 0xffa29e95 };
const juce::Colour faint     { 0xff5a5852 };
const juce::Colour line      { 0xff3a3a3a };
const juce::Colour ring      { 0xff1a1a1a };
const juce::Colour pin       { 0xff3c3c3c };
const juce::Colour red       { 0xffe8392f };
const juce::Colour teal      { 0xff3fd6c8 }; // sampler A
const juce::Colour coral     { 0xffff7a59 }; // sampler B
const juce::Colour blob      { 0xff4d5bff };
const juce::Colour blobShade { 0xff2329a8 };
const juce::Colour eye       { 0xffffffff };
const juce::Colour faceInk   { 0xff0b0b12 };

juce::Font font (float height, float kerning = 0.04f);
void label (juce::Graphics&, const juce::String& text, juce::Rectangle<int> r,
            juce::Justification j = juce::Justification::centredLeft, juce::Colour c = dim, float size = 13.0f);
} // namespace ff

namespace ui
{
// A split-flap card that flips like the Karlsson clock when its value changes.
class FlipCard : public juce::Component
{
public:
    void setValue (const juce::String& v);
    void setTextHeight (float h) { textHeight = h; }
    void advance (double dt);
    void paint (juce::Graphics&) override;

private:
    void drawHalf (juce::Graphics&, juce::Rectangle<float> card, const juce::String& text, bool top, float scale, bool shade);
    juce::String current, previous;
    float progress = 1.0f;
    float textHeight = 42.0f;
};

// Rounded pill buttons bound to a choice parameter.
class Pills : public juce::Component
{
public:
    Pills (juce::AudioProcessorValueTreeState&, const juce::String& paramId, juce::StringArray labels, float textSize = 14.0f);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cell (int i) const;
    juce::StringArray labels;
    int selected = 0;
    float textSize;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// A single pill that turns ink-white when its switch is on (SYNC, AUTO LEVEL, HOLD).
class ToggleChip : public juce::Component
{
public:
    ToggleChip (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& text, float textSize = 13.0f);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::String text;
    float textSize;
    bool on = false;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

class FlipLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
};

// A knob drawn as a small clock face, with its name above and value below.
class Dial : public juce::Component
{
public:
    Dial (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption, bool redHand = false);
    void setModulated (float position, bool active);
    void setValueText (const juce::String& t) { if (t != valueText) { valueText = t; repaint(); } }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider slider;
    juce::RangedAudioParameter* param = nullptr;
    juce::String caption, valueText;
    bool red = false;
    float modPos = 0.0f;
    bool modActive = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// A slider with its name on the left and value on the right.
class LabelledSlider : public juce::Component
{
public:
    LabelledSlider (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& caption);
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::Slider slider;

private:
    juce::RangedAudioParameter* param = nullptr;
    juce::String caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// The hero control: the pedal's Voice dial with its five groups round the rim.
class VoiceDial : public juce::Component
{
public:
    VoiceDial (JetstreamProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    int currentStop() const;

private:
    juce::Point<float> centre() const;
    float scale() const;
    int stopAt (juce::Point<float>) const;
    void pick (int stop);
    JetstreamProcessor& proc;
    juce::RangedAudioParameter *voiceParam = nullptr, *slotParam = nullptr;
    int dragStop = -1;
    float dragStart = 0.0f;
};

// The A and B footswitches. They capture notes in Manual mode and show each channel's level.
class SamplerButton : public juce::Component
{
public:
    SamplerButton (JetstreamProcessor&, int channel);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void set (bool);
    JetstreamProcessor& proc;
    int channel;
    juce::RangedAudioParameter* param = nullptr;
    bool pressed = false;
};

// The pedal's indicator lights: A, input, B and Mod.
class Lights : public juce::Component
{
public:
    explicit Lights (JetstreamProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;

private:
    JetstreamProcessor& proc;
};

// REC DIM / REC DRV: record a knob movement, loop it, move the knob to stop.
class RecButton : public juce::Component
{
public:
    RecButton (JetstreamProcessor&, int target, const juce::String& text);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    JetstreamProcessor& proc;
    int target;
    juce::String text;
};

class LearnButton : public juce::Component
{
public:
    explicit LearnButton (JetstreamProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { proc.startLearn(); }

private:
    JetstreamProcessor& proc;
};

// A / LIVE / B, like Murmur. Hold A or B to save into it.
class PresetToggle : public juce::Component, private juce::Timer
{
public:
    PresetToggle (JetstreamProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    juce::Rectangle<float> cell (int i) const;
    JetstreamProcessor& proc;
    int pressedCell = -1;
    double downAt = 0, savedFlashUntil = 0;
    int savedCell = -1;
    bool saved = false;
};

class BypassButton : public juce::Component
{
public:
    explicit BypassButton (JetstreamProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    JetstreamProcessor& proc;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    bool bypassed = false;
};

// Pebble and the Glitch B memory: six cells, Hold and Clear.
class MemoryStrip : public juce::Component
{
public:
    explicit MemoryStrip (JetstreamProcessor& p);
    void update (double now, double dt);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void paintPebble (juce::Graphics&, juce::Rectangle<float> area);
    juce::Rectangle<float> cellBounds (int i) const;
    JetstreamProcessor& proc;
    ToggleChip hold;
    juce::TextButton clear { "CLEAR" };
    float level = 0, envA = 0, envB = 0, look = 0, mouth = 0, hop = 0;
    bool asleep = false, blinking = false, held = false, bypassed = false;
    double lastSound = 0, nextBlink = 2.0, blinkUntil = 0, now = 0;
    int lastCaptures = 0, filled = 0;
    float flash[6] = {};
    juce::String status, detail;
};
} // namespace ui

class JetstreamEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit JetstreamEditor (JetstreamProcessor&);
    ~JetstreamEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void section (juce::Graphics&, juce::Rectangle<int>) const;

    JetstreamProcessor& proc;
    ui::FlipLookAndFeel lnf;
    double lastTick = 0;

    // header
    ui::Pills inputMode;
    ui::LearnButton learn;
    ui::LabelledSlider sensitivity;
    ui::PresetToggle presets;

    // samplers
    ui::FlipCard triggerCard;
    ui::Pills trigger, fsMode;
    ui::Dial envShape, envTime;
    ui::SamplerButton buttonA, buttonB;

    // voice
    ui::FlipCard groupCard, variantCard;
    ui::VoiceDial voiceDial;
    ui::Lights lights;

    // motion
    ui::Dial dimension, lfoDepth, lfoRate, lfoShape;
    ui::Pills dimSource;
    ui::ToggleChip sync;
    ui::RecButton recDim, recDrive;

    // output
    ui::ToggleChip autoLevel;
    ui::Dial drive, tone, dry, master;
    ui::LabelledSlider width, air;

    ui::MemoryStrip strip;
    ui::BypassButton bypass;

    juce::String dimMeaning;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JetstreamEditor)
};

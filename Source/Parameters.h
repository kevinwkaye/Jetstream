// Builds the host parameter list from the one shared list in engine/ParamSpec.h,
// with readable values for the knobs that aren't plain percentages.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "engine/ParamSpec.h"

namespace jet
{
inline juce::String envTimeText (float v)
{
    if (EnvTime::zone (v) == EnvTime::Repeat) return "Repeat";
    if (EnvTime::zone (v) == EnvTime::Infinite) return "Infinite";
    const float s = EnvTime::seconds (v);
    return s < 1.0f ? juce::String (juce::roundToInt (s * 1000.0f)) + " ms" : juce::String (s, s < 10.0f ? 1 : 0) + " s";
}

inline juce::String envShapeText (float v)
{
    static const char* const n[kNumShapes] = { "Decay", "Triangle", "Square", "Swell", "Rise", "Random" };
    return n[ShapeKnob::from (v).shape];
}

inline juce::String lfoRateText (float v)
{
    const float hz = lfoHz (v);
    return hz < 1.0f ? juce::String (hz, 2) + " Hz" : juce::String (hz, 1) + " Hz";
}

inline juce::String toneText (float v)
{
    if (v >= 0.995f) return "Open";
    return juce::String (1.5f * std::pow (13.0f, v), 1) + " kHz";
}

// A parameter's value as the panel shows it
inline juce::String valueText (const juce::String& id, float v)
{
    if (id == "envTime") return envTimeText (v);
    if (id == "envShape") return envShapeText (v);
    if (id == "lfoRate") return lfoRateText (v);
    if (id == "tone") return toneText (v);
    if (id == "master") return std::abs (v) < 0.05f ? juce::String ("0 dB") : juce::String (v, 1) + " dB";
    return juce::String (juce::roundToInt (v * 100.0f)) + "%";
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    for (auto& s : paramSpecs())
    {
        const ParameterID id { s.id, 1 };
        const String sid (s.id);
        switch (s.type)
        {
            case PType::Float:
                l.add (std::make_unique<AudioParameterFloat> (id, s.name, NormalisableRange<float> (s.lo, s.hi), s.def,
                           AudioParameterFloatAttributes().withStringFromValueFunction ([sid] (float v, int) { return valueText (sid, v); })));
                break;
            case PType::Int:
                l.add (std::make_unique<AudioParameterInt> (id, s.name, (int) s.lo, (int) s.hi, (int) s.def));
                break;
            case PType::Bool:
                l.add (std::make_unique<AudioParameterBool> (id, s.name, s.def > 0.5f));
                break;
            case PType::Choice:
            {
                StringArray c;
                for (auto* n : s.choices) c.add (n);
                l.add (std::make_unique<AudioParameterChoice> (id, s.name, c, (int) s.def));
                break;
            }
        }
    }
    return l;
}
} // namespace jet

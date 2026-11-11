#pragma once

#include "engine/JetEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// Short names for the panel. The parameter list itself lives in engine/ParamSpec.h.
namespace ParamIDs
{
inline constexpr auto inputMode = "inputMode";
inline constexpr auto sensitivity = "sensitivity";
inline constexpr auto trigger = "trigger";
inline constexpr auto fsMode = "fsMode";
inline constexpr auto envShape = "envShape";
inline constexpr auto envTime = "envTime";
inline constexpr auto voice = "voice";
inline constexpr auto dimension = "dimension";
inline constexpr auto lfoDepth = "lfoDepth";
inline constexpr auto lfoRate = "lfoRate";
inline constexpr auto lfoShape = "lfoShape";
inline constexpr auto dimSource = "dimSource";
inline constexpr auto sync = "sync";
inline constexpr auto drive = "drive";
inline constexpr auto tone = "tone";
inline constexpr auto dry = "dry";
inline constexpr auto master = "master";
inline constexpr auto autoLevel = "autoLevel";
inline constexpr auto width = "width";
inline constexpr auto air = "air";
inline constexpr auto hold = "hold";
inline constexpr auto bypass = "bypass";
} // namespace ParamIDs

namespace names
{
inline const juce::StringArray voices { "Blur 0", "Blur -1", "Blur +1", "Blur +/-1", "Synth A", "Synth B", "Glitch A", "Glitch B", "Swell A", "Swell B" };
inline const juce::StringArray envShapes { "Decay", "Triangle", "Square", "Swell", "Rise", "Random" };
inline const juce::StringArray lfoShapes { "Saw down", "Triangle", "Square", "Sine", "Saw up", "Random" };
// Tempo-synced lengths, slowest first, and their length in beats
inline const juce::StringArray divisions { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
inline constexpr float divisionBeats[8] = { 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f };

inline int divisionIndex (float v01) { return juce::jlimit (0, 7, juce::roundToInt (v01 * 7.0f)); }
inline float lfoHz (float v01) { return 0.02f * std::pow (2.0f, v01 * 9.0f); } // 0.02 .. 10 Hz

inline juce::String envTimeText (float v)
{
    using jet::EnvTime;
    if (EnvTime::zone (v) == EnvTime::Repeat) return "Repeat";
    if (EnvTime::zone (v) == EnvTime::Infinite) return "Infinite";
    const float s = EnvTime::seconds (v);
    return s < 1.0f ? juce::String (juce::roundToInt (s * 1000.0f)) + " ms" : juce::String (s, s < 10.0f ? 1 : 0) + " s";
}
} // namespace names

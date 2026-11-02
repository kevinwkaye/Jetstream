#pragma once

#include "engine/JetEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// Every control on the Jetstream panel. All are automatable, MIDI-mappable in
// Ableton and saved with the set.
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
inline constexpr auto footA = "footA";
inline constexpr auto footB = "footB";
inline constexpr auto presetSlot = "presetSlot";
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

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    using P = AudioParameterFloat;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pctText = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; });
    auto knob = [&] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<P> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def, pctText));
    };
    auto knobWith = [&] (const char* id, const char* name, float def, std::function<String (float, int)> text)
    {
        layout.add (std::make_unique<P> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def,
                                         AudioParameterFloatAttributes().withStringFromValueFunction (std::move (text))));
    };
    auto toggle = [&] (const char* id, const char* name, bool def) { layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def)); };
    auto choice = [&] (const char* id, const char* name, StringArray items, int def) { layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, def)); };

    // ---- samplers ----
    choice (ParamIDs::inputMode, "Input", { "Guitar", "Synth", "Bass" }, 0);
    knob (ParamIDs::sensitivity, "Sensitivity", 0.6f);
    choice (ParamIDs::trigger, "Trigger Mode", { "Poly", "Mono", "Manual" }, 0);
    choice (ParamIDs::fsMode, "Footswitch Mode", { "Toggle", "Latch", "Momentary" }, 0);
    knobWith (ParamIDs::envShape, "Env Shape", 0.6f, [] (float v, int)
    {
        return names::envShapes[jet::ShapeKnob::from (v).shape];
    });
    knobWith (ParamIDs::envTime, "Env Time", 0.62f, [] (float v, int) { return names::envTimeText (v); });

    // ---- voice ----
    choice (ParamIDs::voice, "Voice", names::voices, 2);
    knob (ParamIDs::dimension, "Dimension", 0.5f);

    // ---- motion ----
    knob (ParamIDs::lfoDepth, "LFO Depth", 0.2f);
    knobWith (ParamIDs::lfoRate, "LFO Rate", 0.4f, [] (float v, int)
    {
        const float hz = names::lfoHz (v);
        return hz < 1.0f ? String (hz, 2) + " Hz" : String (hz, 1) + " Hz";
    });
    choice (ParamIDs::lfoShape, "LFO Shape", names::lfoShapes, 1);
    choice (ParamIDs::dimSource, "Dimension Source", { "LFO", "Envelope", "Off" }, 0);
    toggle (ParamIDs::sync, "Tempo Sync", false);

    // ---- output ----
    knob (ParamIDs::drive, "Drive", 0.15f);
    knobWith (ParamIDs::tone, "Tone", 0.85f, [] (float v, int)
    {
        if (v >= 0.995f) return String ("Open");
        return String (1.5f * std::pow (13.0f, v), 1) + " kHz";
    });
    knob (ParamIDs::dry, "Dry", 0.5f);
    layout.add (std::make_unique<P> (ParameterID { ParamIDs::master, 1 }, "Master", NormalisableRange<float> (-24.0f, 12.0f), 0.0f,
                                     AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                     {
                                         return std::abs (v) < 0.05f ? String ("0 dB") : String (v, 1) + " dB";
                                     })));
    toggle (ParamIDs::autoLevel, "Auto Level", true);
    knob (ParamIDs::width, "Width", 0.8f);
    knob (ParamIDs::air, "Air", 0.45f);

    // ---- memory and switches ----
    toggle (ParamIDs::hold, "Hold Memory", false);
    toggle (ParamIDs::bypass, "Bypass", false);
    toggle (ParamIDs::footA, "Footswitch A", false);
    toggle (ParamIDs::footB, "Footswitch B", false);
    choice (ParamIDs::presetSlot, "Preset", { "A", "Live", "B" }, 1);

    return layout;
}

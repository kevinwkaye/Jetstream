// Jetstream's single parameter list, factory presets and expression mapping.
// Plain C++17 with no JUCE, so the plugin, the presets and the offline tests all share
// exactly one definition of every parameter. IDs are the ones v0.1 used, so old sets load.
#pragma once

#include "JetEngine.h"
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace jet
{
enum class PType { Float, Int, Bool, Choice };

struct ParamSpec
{
    const char* id;
    const char* name;
    PType type;
    float lo, hi, def;
    std::vector<const char*> choices {};
};

enum ExpTarget
{
    ExpDimension = 0, ExpEnvTime, ExpEnvShape, ExpDrive, ExpDry, ExpLfoDepth, ExpLfoRate, ExpTone, ExpWidth, ExpAir, ExpCount
};

inline const std::vector<const char*>& expTargetNames()
{
    static const std::vector<const char*> n { "DIMENSION", "ENV TIME", "ENV SHAPE", "DRIVE", "DRY", "LFO DEPTH", "LFO RATE",
                                              "TONE", "WIDTH", "AIR" };
    return n;
}

inline const char* expTargetParam (int t)
{
    static const char* const ids[ExpCount] = { "dimension", "envTime", "envShape", "drive", "dry", "lfoDepth", "lfoRate",
                                               "tone", "width", "air" };
    return ids[t < 0 || t >= ExpCount ? 0 : t];
}

inline const std::vector<const char*>& voiceNames()
{
    static const std::vector<const char*> n { "Blur 0", "Blur -1", "Blur +1", "Blur +/-1", "Synth A", "Synth B",
                                              "Glitch A", "Glitch B", "Swell A", "Swell B" };
    return n;
}

inline const std::vector<ParamSpec>& paramSpecs()
{
    using P = PType;
    static const std::vector<ParamSpec> s {
        // Samplers
        { "inputMode", "Input", P::Choice, 0, 2, 0, { "Guitar", "Synth", "Bass" } },
        { "sensitivity", "Sensitivity", P::Float, 0, 1, 0.6f },
        { "trigger", "Trigger Mode", P::Choice, 0, 2, 0, { "Poly", "Mono", "Manual" } },
        { "fsMode", "Footswitch Mode", P::Choice, 0, 2, 0, { "Toggle", "Latch", "Momentary" } },
        { "envShape", "Env Shape", P::Float, 0, 1, 0.6f },
        { "envTime", "Env Time", P::Float, 0, 1, 0.62f },

        // Voice
        { "voice", "Voice", P::Choice, 0, 9, 0, voiceNames() },
        { "dimension", "Dimension", P::Float, 0, 1, 0.5f },

        // Motion
        { "lfoDepth", "LFO Depth", P::Float, 0, 1, 0.2f },
        { "lfoRate", "LFO Rate", P::Float, 0, 1, 0.4f },
        { "lfoShape", "LFO Shape", P::Choice, 0, 5, 1, { "Saw down", "Triangle", "Square", "Sine", "Saw up", "Random" } },
        { "dimSource", "Dimension Source", P::Choice, 0, 2, 0, { "LFO", "Envelope", "Off" } },
        { "sync", "Tempo Sync", P::Bool, 0, 1, 0 },

        // Output
        { "drive", "Drive", P::Float, 0, 1, 0.15f },
        { "tone", "Tone", P::Float, 0, 1, 0.85f },
        { "dry", "Dry", P::Float, 0, 1, 0.5f },
        { "master", "Master", P::Float, -24, 12, 0 },
        { "autoLevel", "Auto Level", P::Bool, 0, 1, 1 },
        { "width", "Width", P::Float, 0, 1, 0.8f },
        { "air", "Air", P::Float, 0, 1, 0.45f },

        // Expression
        { "expTarget", "Expression target", P::Choice, 0, (float) (ExpCount - 1), 0, expTargetNames() },
        { "expLo", "Expression heel", P::Float, 0, 1, 0.2f },
        { "expHi", "Expression toe", P::Float, 0, 1, 0.9f },
        { "exp", "Expression", P::Float, 0, 1, 0.0f },
        { "expOn", "Expression active", P::Bool, 0, 1, 0 },

        // Memory and switches
        { "hold", "Hold Memory", P::Bool, 0, 1, 0 },
        { "bypass", "Bypass", P::Bool, 0, 1, 0 },
        { "tails", "Tails", P::Bool, 0, 1, 1 },
    };
    return s;
}

inline const ParamSpec* findSpec (const char* id)
{
    for (auto& s : paramSpecs())
        if (std::strcmp (s.id, id) == 0) return &s;
    return nullptr;
}

// ---- how the plain values read ----
inline float lfoHz (float v01) { return 0.02f * std::pow (2.0f, clampf (v01, 0.0f, 1.0f) * 9.0f); } // 0.02 .. 10 Hz
inline float lfoKnobFor (float hz) { return std::log2 (hz / 0.02f) / 9.0f; }
inline float envTimeFor (float seconds) { return EnvTime::kInfinite * std::log (seconds / 0.05f) / std::log (400.0f); }
inline float envShapeFor (int shape, float rateOffset = 0.5f) { return ((float) shape + rateOffset - 0.5f) / (float) (kNumShapes - 1); }
inline constexpr float kEnvInfinite = 0.92f, kEnvRepeat = 0.97f;

// Tempo-synced lengths, slowest first, and their length in beats
inline const char* const kDivisions[8] = { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32" };
inline constexpr float kDivisionBeats[8] = { 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f };
inline int divisionIndex (float v01) { return std::clamp ((int) std::lround (v01 * 7.0f), 0, 7); }

// Writes one parameter (plain value: choice index, 0/1 for bools) into the engine's params
// no allocation: safe on the audio thread
inline void setParam (EngineParams& p, const char* idc, float v)
{
    struct Id { const char* s; bool operator== (const char* o) const { return std::strcmp (s, o) == 0; } } id { idc };
    const int iv = (int) std::lround (v);
    const bool bv = v > 0.5f;
    if      (id == "inputMode") p.inputMode = iv;
    else if (id == "sensitivity") p.sensitivity = v;
    else if (id == "trigger") p.trigger = iv;
    else if (id == "fsMode") p.fsMode = iv;
    else if (id == "envShape") p.envShape = v;
    else if (id == "envTime") p.envTime = v;
    else if (id == "voice") p.voice = std::clamp (iv, 0, kNumVoices - 1);
    else if (id == "dimension") p.dimension = v;
    else if (id == "lfoDepth") p.lfoDepth = v;
    else if (id == "lfoRate") p.lfoRateHz = lfoHz (v);
    else if (id == "lfoShape") p.lfoShape = iv;
    else if (id == "dimSource") p.dimSource = iv;
    else if (id == "drive") p.drive = v;
    else if (id == "tone") p.tone = v;
    else if (id == "dry") p.dry = v;
    else if (id == "master") p.masterDb = v;
    else if (id == "autoLevel") p.autoLevel = bv;
    else if (id == "width") p.width = v;
    else if (id == "air") p.air = v;
    else if (id == "hold") p.hold = bv;
    else if (id == "bypass") p.bypass = bv;
    else if (id == "tails") p.tails = bv;
}

inline void setParam (EngineParams& p, const std::string& id, float v) { setParam (p, id.c_str(), v); }

// The expression pedal moves one control between the preset's heel and toe settings,
// both on the control's own 0..1 scale.
inline float expressionValue (float heel, float toe, float pedal) { return heel + (toe - heel) * std::clamp (pedal, 0.0f, 1.0f); }

inline void applyExpression (EngineParams& p, int target, float heel, float toe, float pedal)
{
    const float v = expressionValue (heel, toe, pedal);
    switch (target)
    {
        case ExpDimension: p.dimension = v; break;
        case ExpEnvTime: p.envTime = v; break;
        case ExpEnvShape: p.envShape = v; break;
        case ExpDrive: p.drive = v; break;
        case ExpDry: p.dry = v; break;
        case ExpLfoDepth: p.lfoDepth = v; break;
        case ExpLfoRate: p.lfoRateHz = lfoHz (v); break;
        case ExpTone: p.tone = v; break;
        case ExpWidth: p.width = v; break;
        case ExpAir: p.air = v; break;
        default: break;
    }
}

//==============================================================================
// Factory presets. Each one starts from the defaults, sets what it needs, and says what
// the expression pedal does and one thing to try. The first ten follow the sounds in
// Hologram's own "10 Beautiful Ambient Post-Rock Sounds" walkthrough.
struct Preset
{
    std::string name;
    std::string hint;
    std::vector<std::pair<std::string, float>> values;
};

inline const std::vector<Preset>& factoryPresets()
{
    using V = std::vector<std::pair<std::string, float>>;
    auto exp = [] (int target, float heel, float toe) -> V { return { { "expTarget", (float) target }, { "expLo", heel }, { "expHi", toe } }; };
    auto join = [] (V a, const V& b) { a.insert (a.end(), b.begin(), b.end()); return a; };
    const float swell = envShapeFor (ShapeSwell), decay = envShapeFor (ShapeDecay), tri = envShapeFor (ShapeTriangle);
    const float square = envShapeFor (ShapeSquare), rise = envShapeFor (ShapeRise), rnd = envShapeFor (ShapeRandom);
    enum { Saw = 0, Tri, Sq, Sine, Up, Rand };

    static const std::vector<Preset> p {
        { "INIT", "Start here: every note swells into a held copy of itself.",
          join ({ { "voice", VBlur0 } }, exp (ExpDimension, 0.2f, 0.9f)) },
        { "AMBIENT BED", "Play slow chords. Rock toe down to stretch the space.",
          join ({ { "voice", VBlur0 }, { "envShape", swell }, { "envTime", envTimeFor (6.0f) }, { "dimension", 0.55f },
                  { "lfoDepth", 0.15f }, { "lfoRate", lfoKnobFor (0.1f) }, { "lfoShape", Sine }, { "air", 0.6f } },
                exp (ExpDimension, 0.25f, 0.9f)) },
        { "ENDLESS PAD", "Each new chord takes over the last. Toe down brings the guitar back.",
          join ({ { "voice", VBlur0 }, { "trigger", 1 }, { "envShape", swell }, { "envTime", kEnvInfinite }, { "dimension", 0.5f },
                  { "air", 0.6f }, { "dry", 0.35f } }, exp (ExpDry, 0.2f, 0.75f)) },
        { "SHIMMER", "Hold notes and let the octave rise in behind them.",
          join ({ { "voice", VBlurUp }, { "envShape", swell }, { "envTime", envTimeFor (5.0f) }, { "dimension", 0.7f },
                  { "air", 0.7f }, { "lfoDepth", 0.1f }, { "lfoRate", lfoKnobFor (0.15f) }, { "lfoShape", Sine } },
                exp (ExpAir, 0.3f, 1.0f)) },
        { "SUB DRONE", "Tap A on a low note to hold a drone. Double tap A to let it go.",
          join ({ { "voice", VBlurDown }, { "trigger", 2 }, { "fsMode", 1 }, { "envShape", swell }, { "envTime", kEnvInfinite },
                  { "dimension", 0.35f }, { "air", 0.5f } }, exp (ExpDimension, 0.1f, 0.7f)) },
        { "OCTAVE CATHEDRAL", "Single notes become a choir an octave either side.",
          join ({ { "voice", VBlurBoth }, { "envShape", swell }, { "envTime", envTimeFor (6.0f) }, { "dimension", 0.6f },
                  { "air", 0.8f }, { "width", 1.0f } }, exp (ExpAir, 0.4f, 1.0f)) },
        { "TIDE", "Every note pulses in and out. Turn Sync on to lock it to the song.",
          join ({ { "voice", VBlur0 }, { "envShape", envShapeFor (ShapeTriangle, 0.35f) }, { "envTime", kEnvRepeat },
                  { "dimension", 0.5f }, { "air", 0.5f } }, exp (ExpEnvShape, tri - 0.08f, tri + 0.09f)) },
        { "FILTER LEAD", "Single notes turn into a square lead with a slow filter sweep.",
          join ({ { "voice", VSynthA }, { "trigger", 1 }, { "envShape", square }, { "envTime", kEnvInfinite }, { "dimension", 0.45f },
                  { "lfoDepth", 0.5f }, { "lfoRate", lfoKnobFor (0.25f) }, { "lfoShape", Sine }, { "air", 0.35f } },
                exp (ExpDimension, 0.1f, 0.9f)) },
        { "ENVELOPE FILTER", "Every note opens the filter and closes it again. Dig in.",
          join ({ { "voice", VSynthB }, { "envShape", decay }, { "envTime", envTimeFor (1.5f) }, { "dimension", 0.15f },
                  { "dimSource", 1 }, { "lfoDepth", 0.7f }, { "lfoRate", lfoKnobFor (1.6f) }, { "lfoShape", Saw }, { "air", 0.3f } },
                exp (ExpLfoDepth, 0.2f, 1.0f)) },
        { "EIGHTIES PAD", "Chords bloom into wide, detuned saws.",
          join ({ { "voice", VSynthB }, { "envShape", swell }, { "envTime", envTimeFor (5.0f) }, { "dimension", 0.55f },
                  { "lfoDepth", 0.1f }, { "lfoRate", lfoKnobFor (0.12f) }, { "lfoShape", Sine }, { "air", 0.6f }, { "width", 1.0f } },
                exp (ExpDimension, 0.25f, 0.8f)) },
        { "SQUARE STAB", "Short square blips under every note. Rock to lengthen them.",
          join ({ { "voice", VSynthA }, { "envShape", decay }, { "envTime", envTimeFor (0.5f) }, { "dimension", 0.6f },
                  { "lfoDepth", 0.0f }, { "drive", 0.2f }, { "air", 0.3f } }, exp (ExpEnvTime, envTimeFor (0.2f), envTimeFor (2.0f))) },
        { "STUTTER", "Hold A to stutter what you just played. Let go to stop.",
          join ({ { "voice", VGlitchA }, { "trigger", 2 }, { "fsMode", 2 }, { "envShape", square }, { "envTime", kEnvInfinite },
                  { "dimension", 0.5f }, { "lfoDepth", 0.0f }, { "air", 0.25f } }, exp (ExpDimension, 0.0f, 1.0f)) },
        { "GLITCH RAIN", "Play a phrase and it breaks into scattered loops.",
          join ({ { "voice", VGlitchA }, { "envShape", decay }, { "envTime", envTimeFor (1.0f) }, { "dimension", 0.3f },
                  { "lfoDepth", 0.6f }, { "lfoRate", lfoKnobFor (4.0f) }, { "lfoShape", Rand }, { "air", 0.4f } },
                exp (ExpLfoDepth, 0.0f, 1.0f)) },
        { "SELF REMIX", "Play for half a minute, tap C to hold the memory, keep playing.",
          join ({ { "voice", VGlitchB }, { "envShape", square }, { "envTime", envTimeFor (1.0f) }, { "dimension", 0.5f },
                  { "lfoDepth", 0.8f }, { "lfoRate", lfoKnobFor (0.6f) }, { "lfoShape", Rand }, { "dry", 0.4f }, { "air", 0.3f } },
                exp (ExpLfoRate, lfoKnobFor (0.2f), lfoKnobFor (3.0f))) },
        { "SWELL ECHOES", "Notes fade in with no attack and trail off into echoes.",
          join ({ { "voice", VSwellA }, { "envShape", rise }, { "envTime", envTimeFor (2.0f) }, { "dimension", 0.6f },
                  { "lfoDepth", 0.0f }, { "air", 0.7f } }, exp (ExpDimension, 0.3f, 0.95f)) },
        { "TAPE WARBLE", "Held notes drift out of tune like old tape.",
          join ({ { "voice", VBlur0 }, { "envShape", swell }, { "envTime", envTimeFor (5.0f) }, { "dimension", 0.12f },
                  { "lfoDepth", 0.22f }, { "lfoRate", lfoKnobFor (0.6f) }, { "lfoShape", Sine }, { "tone", 0.6f }, { "air", 0.45f } },
                exp (ExpLfoDepth, 0.05f, 0.5f)) },
        { "VIOLIN FUZZ", "Notes swell in through a bowed fuzz. Rock for more grit.",
          join ({ { "voice", VSwellB }, { "envShape", swell }, { "envTime", envTimeFor (1.5f) }, { "dimension", 0.4f },
                  { "lfoDepth", 0.0f }, { "drive", 0.3f }, { "tone", 0.7f }, { "air", 0.5f } }, exp (ExpDrive, 0.1f, 0.7f)) },
        { "BASS SUB BLOOM", "Bass: an octave below blooms under each note. Lows stay tight.",
          join ({ { "inputMode", 2 }, { "voice", VBlurDown }, { "envShape", swell }, { "envTime", envTimeFor (3.0f) },
                  { "dimension", 0.3f }, { "lfoDepth", 0.0f }, { "air", 0.3f } }, exp (ExpDry, 0.3f, 0.75f)) },
        { "BASS SYNTH", "Bass: every note becomes a filtered square. Dig in to open it.",
          join ({ { "inputMode", 2 }, { "voice", VSynthA }, { "trigger", 1 }, { "envShape", decay }, { "envTime", envTimeFor (1.2f) },
                  { "dimension", 0.25f }, { "dimSource", 1 }, { "lfoDepth", 0.45f }, { "lfoRate", lfoKnobFor (1.2f) },
                  { "lfoShape", Saw }, { "air", 0.15f }, { "dry", 0.4f } }, exp (ExpDimension, 0.1f, 0.7f)) },
        { "SYNTH PAD SHADOW", "Synth: each chord change leaves an octave-down shadow.",
          join ({ { "inputMode", 1 }, { "voice", VBlurDown }, { "trigger", 1 }, { "envShape", swell }, { "envTime", envTimeFor (8.0f) },
                  { "dimension", 0.55f }, { "air", 0.6f } }, exp (ExpDimension, 0.2f, 0.9f)) },
    };
    (void) rnd;
    return p;
}

// controls you play or set once, rather than per sound: presets leave them alone
inline bool isPerformanceParam (const std::string& id)
{
    return id == "exp" || id == "expOn" || id == "bypass" || id == "hold" || id == "sensitivity" || id == "sync"
        || id == "master" || id == "tails";
}

// Builds the full parameter set for a preset (every parameter, so nothing lingers from before)
inline std::vector<std::pair<std::string, float>> presetValues (const Preset& pr)
{
    std::vector<std::pair<std::string, float>> out;
    for (auto& s : paramSpecs())
    {
        const std::string id = s.id;
        if (isPerformanceParam (id)) continue;
        float v = s.def;
        for (auto& kv : pr.values) if (kv.first == id) v = kv.second;
        out.emplace_back (id, v);
    }
    return out;
}

inline EngineParams paramsFor (const Preset& pr)
{
    EngineParams p;
    for (auto& s : paramSpecs()) setParam (p, s.id, s.def);
    for (auto& kv : presetValues (pr)) setParam (p, kv.first, kv.second);
    p.repeatRateHz = -1.0f;
    return p;
}
} // namespace jet

#pragma once

#include "DspUtil.h"

// The pedal's six envelope (and LFO) shapes, in panel order:
// 0 falling ramp, 1 triangle, 2 square, 3 rounded swell, 4 rising ramp, 5 stepped random.
namespace jet
{
enum Shape { ShapeDecay = 0, ShapeTriangle, ShapeSquare, ShapeSwell, ShapeRise, ShapeRandom, kNumShapes };

inline float smoothstep (float x)
{
    x = clampf (x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Where each shape reaches its peak, as a fraction of its length.
// Everything before is the attack, everything after is the release.
inline float shapePeak (int shape, float edge)
{
    switch (shape)
    {
        case ShapeDecay:    return edge;
        case ShapeTriangle: return 0.5f;
        case ShapeSquare:   return edge;
        case ShapeSwell:    return 0.5f;
        case ShapeRise:     return 1.0f - edge;
        default:            return edge;
    }
}

// Unipolar shape value 0..1 at phase p (0..1). 'edge' is the softened edge
// length as a fraction of the shape, so square and ramps never click.
// The random shape is handled by the caller (it needs state).
inline float shapeValue (int shape, float p, float edge)
{
    p = clampf (p, 0.0f, 1.0f);
    edge = clampf (edge, 0.0005f, 0.25f);
    switch (shape)
    {
        case ShapeDecay:
            if (p < edge) return smoothstep (p / edge);
            return std::pow (1.0f - (p - edge) / (1.0f - edge), 1.6f);
        case ShapeTriangle:
            return smoothstep (p < 0.5f ? p * 2.0f : 2.0f - p * 2.0f);
        case ShapeSquare:
            if (p < edge) return smoothstep (p / edge);
            if (p > 1.0f - edge) return smoothstep ((1.0f - p) / edge);
            return 1.0f;
        case ShapeSwell:
            return 0.5f - 0.5f * std::cos (kTwoPi * p);
        case ShapeRise:
            if (p > 1.0f - edge) return smoothstep ((1.0f - p) / edge);
            return std::pow (p / (1.0f - edge), 1.4f);
        default:
            if (p < edge) return smoothstep (p / edge);
            if (p > 1.0f - edge) return smoothstep ((1.0f - p) / edge);
            return 1.0f;
    }
}

// Splits the Env Shape knob into a shape and, from where the knob sits between
// two shapes, the rate used by Repeat Waveform and the random steps (like the pedal).
struct ShapeKnob
{
    int shape = ShapeSwell;
    float rateHz = 1.0f;
    static ShapeKnob from (float knob)
    {
        ShapeKnob s;
        const float x = clampf (knob, 0.0f, 1.0f) * (float) (kNumShapes - 1);
        s.shape = (int) std::lround (x);
        const float offset = clampf (x - (float) s.shape + 0.5f, 0.0f, 1.0f); // 0..1 across the shape's zone
        s.rateHz = 0.25f * std::pow (2.0f, offset * 6.0f);                      // 0.25 Hz .. 16 Hz
        return s;
    }
};

// One sampler channel's volume envelope.
class Envelope
{
public:
    enum class Mode { Idle, OneShot, Sustain, Repeat };

    void prepare (float sampleRate)
    {
        fs = sampleRate;
        out.setTime (0.002f, fs);
        rng.s = (uint32_t) (0x51ED270Bu + (uint32_t) (sampleRate * 7.0f));
    }

    // durationSec: the whole shape's length (Env Time). shape/rate from ShapeKnob.
    void startOneShot (int shp, float durationSec, float randomRate)
    {
        begin (shp, durationSec, randomRate);
        mode = Mode::OneShot;
    }

    // Attack, then hold at full until release() is called (Infinite, manual footswitches)
    void startSustain (int shp, float durationSec, float randomRate)
    {
        begin (shp, durationSec, randomRate);
        mode = Mode::Sustain;
        holding = true;
    }

    // Loops the whole shape at rateHz, starting at phase
    void startRepeat (int shp, float rateHz, float startPhase)
    {
        begin (shp, 1.0f / std::max (0.05f, rateHz), rateHz * 2.0f);
        phase = startPhase - std::floor (startPhase);
        mode = Mode::Repeat;
    }

    // Let a sustaining envelope run its release part
    void release()
    {
        if (mode == Mode::Sustain && holding)
        {
            holding = false;
            releaseFrom = current;
            phase = std::max (phase, peak);
        }
        else if (mode == Mode::Repeat)
        {
            kill (0.25f);
        }
    }

    // Fade out quickly from wherever it is (Mono mode cancelling a note)
    void kill (float seconds)
    {
        floorLevel = current;
        floorStep = current / std::max (1.0f, seconds * fs);
        mode = Mode::Idle;
    }

    void setRepeatRate (float rateHz) { if (mode == Mode::Repeat) inc = std::max (0.05f, rateHz) / fs; }
    void setShape (int shp) { shape = shp; }

    bool isActive() const { return mode != Mode::Idle || floorLevel > 1.0e-4f; }
    bool isHolding() const { return mode == Mode::Sustain && holding; }
    float level() const { return current; }
    float getPhase() const { return phase; }

    float next()
    {
        float v = 0.0f;
        switch (mode)
        {
            case Mode::Idle: break;
            case Mode::OneShot:
                v = value (phase);
                phase += inc;
                if (phase >= 1.0f) { mode = Mode::Idle; phase = 1.0f; v = 0.0f; }
                break;
            case Mode::Sustain:
                if (holding)
                {
                    if (phase < peak) { v = value (phase); phase = std::min (peak, phase + inc); }
                    else v = shape == ShapeRandom ? randomValue() : 1.0f;
                }
                else
                {
                    // Decay, triangle and swell fall along their own curve. Square, rise and
                    // random have no natural fall, so they fade evenly over half the time.
                    const bool linear = shape == ShapeSquare || shape == ShapeRise || shape == ShapeRandom;
                    const float span = std::max (1.0e-4f, 1.0f - peak);
                    const float rel = linear ? 1.0f - (phase - peak) / span : value (phase);
                    v = std::min (rel, releaseFrom);
                    phase += linear ? span / (std::max (0.03f, 0.5f * dur) * fs) : inc;
                    if (phase >= 1.0f) { mode = Mode::Idle; v = 0.0f; }
                }
                break;
            case Mode::Repeat:
                v = value (phase);
                phase += inc;
                if (phase >= 1.0f) phase -= 1.0f;
                break;
        }

        // A retrigger or cancel never drops straight to silence: the old level glides down underneath
        if (floorLevel > 0.0f)
        {
            floorLevel = std::max (0.0f, floorLevel - floorStep);
            v = std::max (v, floorLevel);
        }
        current = out.next (v);
        return current;
    }

private:
    void begin (int shp, float durationSec, float randomRate)
    {
        if (current > 1.0e-4f)
        {
            floorLevel = current; // glide from the old level over 60 ms
            floorStep = current / (0.06f * fs);
        }
        shape = shp;
        dur = std::max (0.02f, durationSec);
        inc = 1.0f / (dur * fs);
        edge = clampf (0.012f / dur, 0.002f, 0.2f);
        peak = shapePeak (shape, edge);
        phase = 0.0f;
        holding = false;
        randRate = clampf (randomRate, 0.1f, 40.0f);
        randCount = 0.0f;
        randTarget = 0.6f + 0.4f * rng.uni();
        randSmooth.setTime (0.012f, fs);
    }

    float randomValue()
    {
        randCount += randRate / fs;
        if (randCount >= 1.0f) { randCount -= 1.0f; randTarget = 0.15f + 0.85f * rng.uni(); }
        return randSmooth.next (randTarget);
    }

    float value (float p)
    {
        if (shape == ShapeRandom)
            return randomValue() * shapeValue (ShapeSquare, p, edge);
        return shapeValue (shape, p, edge);
    }

    float fs = 48000.0f;
    Mode mode = Mode::Idle;
    int shape = ShapeSwell;
    float dur = 1.0f, inc = 0.0f, phase = 0.0f, edge = 0.01f, peak = 0.5f;
    bool holding = false;
    float releaseFrom = 1.0f, current = 0.0f;
    float floorLevel = 0.0f, floorStep = 0.0f;
    float randRate = 4.0f, randCount = 0.0f, randTarget = 1.0f;
    Smoother randSmooth, out;
    Rng rng;
};

// Bipolar LFO (-1..1) with the same six shapes, and a one-shot envelope mode
// that runs one cycle per new note (the pedal's envelope generator for Dimension).
class Lfo
{
public:
    void prepare (float sampleRate)
    {
        fs = sampleRate;
        smooth.setTime (0.004f, fs);
        rng.s = 0x1234567u;
    }

    void retrigger() { phase = 0.0f; oneShotDone = false; held = 0.6f + 0.4f * rng.uni(); }

    // Free running: bipolar. One-shot: unipolar 0..1, rests at 0 after one cycle.
    float next (int shape, float rateHz, bool oneShot)
    {
        const float inc = clampf (rateHz, 0.005f, 40.0f) / fs;
        float v;
        if (oneShot)
        {
            if (oneShotDone) v = 0.0f;
            else
            {
                v = shape == ShapeRandom ? held * shapeValue (ShapeSquare, phase, 0.05f)
                                         : shapeValue (shape, phase, 0.02f);
                phase += inc;
                if (phase >= 1.0f) { phase = 1.0f; oneShotDone = true; }
            }
        }
        else
        {
            switch (shape)
            {
                case ShapeDecay:    v = 1.0f - 2.0f * phase; break;
                case ShapeTriangle: v = phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f; break;
                case ShapeSquare:   v = phase < 0.5f ? 1.0f : -1.0f; break;
                case ShapeSwell:    v = std::sin (kTwoPi * phase); break;
                case ShapeRise:     v = 2.0f * phase - 1.0f; break;
                default:            v = held * 2.0f - 1.0f; break;
            }
            phase += inc;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                held = rng.uni();
            }
        }
        return smooth.next (v); // softens the square and saw jumps
    }

    float getPhase() const { return phase; }
    void setPhase (float p) { phase = p - std::floor (p); }

private:
    float fs = 48000.0f, phase = 0.0f, held = 0.5f;
    bool oneShotDone = true;
    Smoother smooth;
    Rng rng;
};
} // namespace jet

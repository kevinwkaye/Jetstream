#pragma once

#include "Spectral.h"

// The building blocks behind the voices: Synth A and B oscillators, seamless loop
// playback for Glitch A and B, and Swell B's fuzz.
namespace jet
{
// ---------------------------------------------------------------------------------
// Synth: finds the notes in a captured note's partials and plays them on band-limited
// oscillators. A is hard and digital, B is airy and chorused. Dimension sets the low-pass.
// Like the pedal, it samples once and then stops following you.
// ---------------------------------------------------------------------------------
class SynthOsc
{
public:
    static constexpr int kMaxNotes = 3;

    void prepare (float sampleRate)
    {
        fs = sampleRate;
        for (auto& f : filt) f.reset();
        level.setTime (0.05f, fs);
        cutoff.setTime (0.03f, fs);
        vib = 0.0f;
        has = false;
        for (auto& n : note) n = Note();
    }

    // A MIDI note sets the pitch exactly. Otherwise the fundamentals come from the partials.
    void setFromVoice (const Voice& v, float captureRms, int midiNote, bool bass)
    {
        float freqs[kMaxNotes] = {}, amps[kMaxNotes] = {};
        int count = 0;

        if (midiNote >= 0)
        {
            freqs[0] = 440.0f * std::pow (2.0f, (float) (midiNote - 69) / 12.0f);
            amps[0] = 1.0f;
            count = 1;
        }
        else
        {
            const float lo = bass ? 27.0f : 60.0f, hi = bass ? 520.0f : 1500.0f;
            const float toHz = fs / kTwoPi;
            float top = 0.0f;
            for (int i = 0; i < v.np; ++i) top = std::max (top, v.p[(size_t) i].amp);
            if (top <= 0.0f) { has = false; return; }

            // strongest first (Analyser builds them in that order)
            auto harmonicOf = [] (float f0, float f) // is f a harmonic (2..10) of f0?
            {
                const float r = f / f0, nearest = std::round (r);
                return nearest >= 2.0f && nearest <= 10.0f && std::fabs (r - nearest) < 0.025f * nearest;
            };
            for (int i = 0; i < v.np && count < kMaxNotes; ++i)
            {
                const auto& p = v.p[(size_t) i];
                if (p.amp < top * 0.16f) break;
                float f = p.omega * toHz;
                if (f < lo * 0.5f || f > hi * 2.0f) continue;

                // a quieter partial an octave below is the real fundamental
                for (int j = 0; j < v.np; ++j)
                {
                    const float g = v.p[(size_t) j].omega * toHz;
                    if (g >= lo && std::fabs (f / g - 2.0f) < 0.04f && v.p[(size_t) j].amp > top * 0.06f) { f = g; break; }
                }
                if (f < lo || f > hi) continue;

                bool skip = false;
                for (int k = 0; k < count; ++k)
                {
                    if (std::fabs (std::log2 (f / freqs[k])) < 0.03f || harmonicOf (freqs[k], f)) { skip = true; break; }
                    if (harmonicOf (f, freqs[k])) { freqs[k] = f; skip = true; break; }
                }
                if (! skip) { freqs[count] = f; amps[count] = p.amp; ++count; }
            }
            float t = 1.0e-9f;
            for (int k = 0; k < count; ++k) t = std::max (t, amps[k]);
            for (int k = 0; k < count; ++k) amps[k] = std::sqrt (amps[k] / t);
        }

        float sum = 0.0f;
        for (int i = 0; i < kMaxNotes; ++i)
        {
            if (i < count) { note[i].freq = freqs[i]; note[i].amp = amps[i]; sum += amps[i] * amps[i]; }
            else note[i].amp = 0.0f;
        }
        // Matches the captured note's loudness (sawtooth and square RMS sit near 0.7)
        targetLevel = count > 0 ? clampf (captureRms * 2.4f, 0.002f, 0.8f) / std::sqrt (std::max (1.0f, sum)) : 0.0f;
        has = count > 0;
    }

    bool hasNotes() const { return has; }
    float noteHz (int i) const { return note[i].amp > 0.0f ? note[i].freq : 0.0f; }

    // type 0 = Synth A, 1 = Synth B. dim 0..1 sets the low-pass cutoff.
    Stereo next (int type, float dim)
    {
        if (++ctl >= 16)
        {
            ctl = 0;
            const float fc = cutoff.next (90.0f * std::pow (2.0f, clampf (dim, 0.0f, 1.0f) * 7.6f));
            const float q = type == 0 ? 1.25f : 0.72f;
            filt[0].set (fc, q, fs);
            filt[1].set (fc, q, fs);
        }
        const float lvl = level.next (has ? targetLevel : 0.0f);
        vib += 0.21f / fs;
        if (vib >= 1.0f) vib -= 1.0f;
        const float vibCents = type == 1 ? 3.5f * std::sin (kTwoPi * vib) : 0.0f;

        Stereo o;
        for (int i = 0; i < kMaxNotes; ++i)
        {
            auto& nt = note[i];
            if (nt.amp <= 0.0f && nt.env <= 1.0e-4f) continue;
            nt.env += (std::max (nt.amp, 0.0f) - nt.env) * 0.002f;
            if (type == 0)
            {
                // a square lead with a little saw for edge
                const float inc = nt.freq / fs;
                const float s = 0.62f * square (nt.ph[0], inc) + 0.38f * saw (nt.ph[0], inc);
                advance (nt.ph[0], inc);
                o.l += s * nt.env;
                o.r += s * nt.env;
            }
            else
            {
                // three detuned saws across the stereo field, gently moving
                static const float detune[3] = { -8.0f, 0.0f, 8.0f };
                static const float pan[3] = { -0.8f, 0.0f, 0.8f };
                for (int d = 0; d < 3; ++d)
                {
                    const float cents = detune[d] + vibCents * (d == 1 ? -0.5f : (d == 0 ? 1.0f : -1.0f));
                    const float inc = nt.freq * std::pow (2.0f, cents / 1200.0f) / fs;
                    const float s = 0.45f * saw (nt.ph[d], inc);
                    advance (nt.ph[d], inc);
                    o += panMono (s * nt.env, pan[d]);
                }
            }
        }
        return { filt[0].lp (o.l) * lvl, filt[1].lp (o.r) * lvl };
    }

private:
    struct Note { float freq = 220.0f, amp = 0.0f, env = 0.0f, ph[3] = { 0.0f, 0.31f, 0.67f }; };

    static float blep (float t, float dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    static float saw (float ph, float inc) { return 2.0f * ph - 1.0f - blep (ph, inc); }
    static float square (float ph, float inc)
    {
        float v = ph < 0.5f ? 1.0f : -1.0f;
        v += blep (ph, inc);
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        v -= blep (p2, inc);
        return v;
    }
    static void advance (float& ph, float inc) { ph += inc; if (ph >= 1.0f) ph -= 1.0f; }

    float fs = 48000.0f, targetLevel = 0.0f, vib = 0.0f;
    bool has = false;
    int ctl = 0;
    Note note[kMaxNotes];
    Svf filt[2];
    Smoother level, cutoff;
};

// ---------------------------------------------------------------------------------
// Loops: each loop closes on the audio that led into it. The buffer holds a pre-roll
// before 'start'; the end is trimmed (up to 10 ms) to where the waveform best matches
// that pre-roll, and the tail fades into it with equal gain. No click, no level bump.
// (Backtrack v0.2 lessons.)
// ---------------------------------------------------------------------------------
inline int matchedLoopEnd (const float* buf, int start, int wantEnd, int searchBack, int matchLen)
{
    matchLen = std::min (matchLen, start);
    if (matchLen < 8) return wantEnd;
    double refE = 0.0;
    for (int i = 0; i < matchLen; ++i) refE += (double) buf[start - matchLen + i] * buf[start - matchLen + i];
    if (refE < 1.0e-12) return wantEnd;

    auto score = [&] (int e)
    {
        double xy = 0.0, yy = 0.0;
        for (int i = 0; i < matchLen; ++i)
        {
            const double x = buf[start - matchLen + i], y = buf[e - matchLen + i];
            xy += x * y; yy += y * y;
        }
        return yy > 1.0e-12 ? xy / std::sqrt (refE * yy) : -1.0;
    };

    const int lo = std::max (start + 2 * matchLen, wantEnd - searchBack);
    int best = wantEnd;
    double bestScore = -2.0;
    for (int e = wantEnd; e >= lo; e -= 4) // coarse
    {
        const double s = score (e);
        if (s > bestScore) { bestScore = s; best = e; }
    }
    const int c = best;
    for (int e = std::min (wantEnd, c + 3); e >= std::max (lo, c - 3); --e) // refine
    {
        const double s = score (e);
        if (s > bestScore) { bestScore = s; best = e; }
    }
    return best;
}

struct LoopReader
{
    const float* buf = nullptr;
    int start = 0, end = 0, xf = 0, pos = 0, pendingEnd = -1;
    float gain = 0.0f, gainStep = 0.0f, target = 0.0f;

    // buf[start - preRoll .. start) must hold the audio before the loop
    void begin (const float* b, int loopStart, int loopEnd, int fadeSamples, float fadeInSamples)
    {
        buf = b;
        start = loopStart;
        end = std::max (loopStart + 16, loopEnd);
        xf = std::min ({ fadeSamples, (end - start) / 3, start });
        pos = start;
        pendingEnd = -1;
        target = 1.0f;
        gainStep = 1.0f / std::max (1.0f, fadeInSamples);
    }
    void stop (float fadeOutSamples) { target = 0.0f; gainStep = 1.0f / std::max (1.0f, fadeOutSamples); }
    void setEnd (int newEnd) { if (newEnd != end) pendingEnd = std::max (start + 16, newEnd); } // at the loop point
    bool active() const { return buf != nullptr && (gain > 0.0f || target > 0.0f); }

    float next()
    {
        if (buf == nullptr) return 0.0f;
        gain += clampf (target - gain, -gainStep, gainStep);
        if (gain <= 0.0f && target <= 0.0f) { buf = nullptr; return 0.0f; }

        float v = buf[pos];
        const int fadeStart = end - xf;
        if (pos >= fadeStart && xf > 0)
        {
            const float t = (float) (pos - fadeStart + 1) / (float) (xf + 1);
            const float g = raisedCos (t);
            v = v * (1.0f - g) + buf[pos - (end - start)] * g; // into the pre-roll before start
        }
        if (++pos >= end)
        {
            pos = start;
            if (pendingEnd > 0)
            {
                end = pendingEnd;
                xf = std::min ({ xf, (end - start) / 3, start });
                pendingEnd = -1;
            }
        }
        return v * raisedCos (gain);
    }
};

// ---------------------------------------------------------------------------------
// Swell B: a violin-like fuzz, 2x oversampled, darkened, and gated so silence stays silent
// ---------------------------------------------------------------------------------
class Fuzz
{
public:
    void prepare (float sampleRate)
    {
        fs = sampleRate;
        os.reset();
        for (auto& f : lp) { f.reset(); f.setCutoff (5200.0f, fs * 2.0f); }
        hp.reset();
        hp.setCutoff (90.0f, fs);
        gate.setTime (0.012f, fs);
    }

    // level: the detector's envelope, play: your playing level
    float process (float x, float level, float play)
    {
        x -= hp.process (x); // tighten the lows before the clipping
        float a, b;
        os.upsample (x, a, b);
        auto shape = [] (float v) { return std::tanh (14.0f * v + 0.18f) - 0.17808f; };
        a = lp[1].process (lp[0].process (shape (a)));
        b = lp[1].process (lp[0].process (shape (b)));
        const float y = os.downsample (a, b);
        const float open = clampf ((20.0f * std::log10 ((level + 1.0e-9f) / std::max (1.0e-6f, play)) + 34.0f) / 12.0f, 0.0f, 1.0f);
        return y * 0.16f * gate.next (open);
    }

private:
    float fs = 48000.0f;
    Oversampler2x os;
    OnePole lp[2], hp;
    Smoother gate;
};
} // namespace jet

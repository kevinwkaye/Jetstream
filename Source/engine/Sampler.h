#pragma once

#include "Envelope.h"

// One of the pedal's two sampling channels (A or B). It records each new note
// from its attack, then rebuilds it as a frozen spectrum (Blur), a synth (Synth)
// or a short loop (Glitch A). Swell and Glitch B are handled by the engine.
namespace jet
{
enum Voice { VBlur0 = 0, VBlurDown, VBlurUp, VBlurBoth, VSynthA, VSynthB, VGlitchA, VGlitchB, VSwellA, VSwellB, kNumVoices };
enum Group { GroupBlur = 0, GroupSynth, GroupGlitchA, GroupGlitchB, GroupSwell };

inline int groupOf (int v)
{
    if (v <= VBlurBoth) return GroupBlur;
    if (v <= VSynthB) return GroupSynth;
    if (v == VGlitchA) return GroupGlitchA;
    if (v == VGlitchB) return GroupGlitchB;
    return GroupSwell;
}

// ---------------------------------------------------------------------------------
// Spectral freeze: keeps a note's spectrum and plays it forever with fresh random
// phases each frame, so the sound sustains without its attack or decay, smooth as glass.
// ---------------------------------------------------------------------------------
class FreezeVoice
{
public:
    void prepare (float sampleRate)
    {
        fs = sampleRate;
        n = sampleRate > 60000.0f ? 16384 : 8192;
        hop = n / 4;
        fft.init (n);
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) n);
        base.assign ((size_t) n / 2 + 1, 0.0f);
        mags = base;
        work.resize ((size_t) n);
        for (int s = 0; s < 2; ++s)
        {
            side[s].ola.assign ((size_t) n, 0.0f);
            side[s].read = hop;
            side[s].rng.s = 0xA511E9B3u + (uint32_t) s * 0x9E3779B9u;
        }
        has = false;
    }

    int frameSize() const { return n; }
    int hopSize() const { return hop; }
    int samplesNeeded (int offset) const { return offset + n + hop; } // two overlapping frames

    // Analyse 'raw' starting at 'start': averages two frames' magnitudes.
    void analyse (const float* raw, int start)
    {
        std::fill (base.begin(), base.end(), 0.0f);
        double energy = 0.0;
        for (int f = 0; f < 2; ++f)
        {
            const float* x = raw + start + f * hop;
            for (int i = 0; i < n; ++i)
            {
                work[(size_t) i] = { x[i] * window[(size_t) i], 0.0f };
                energy += (double) x[i] * x[i];
            }
            fft.run (work.data(), false);
            for (int k = 0; k <= n / 2; ++k)
                base[(size_t) k] += 0.5f * std::abs (work[(size_t) k]);
        }
        captureRms = (float) std::sqrt (energy / (2.0 * n));
        base[0] = 0.0f; // no DC
        rebuild();
        has = true;
    }

    // Octave layers for the -1, +1 and +/-1 Blur voices
    void setOctaves (bool down, bool up)
    {
        if (down == octDown && up == octUp) return;
        octDown = down;
        octUp = up;
        if (has) rebuild();
    }

    const std::vector<float>& spectrum() const { return base; }
    float rms() const { return captureRms; }
    bool hasSpectrum() const { return has; }

    Stereo next()
    {
        Stereo o;
        for (int s = 0; s < 2; ++s)
        {
            auto& sd = side[s];
            if (sd.read >= hop)
            {
                std::copy (sd.ola.begin() + hop, sd.ola.end(), sd.ola.begin());
                std::fill (sd.ola.end() - hop, sd.ola.end(), 0.0f);
                if (has) synthFrame (sd);
                sd.read = 0;
            }
            (s == 0 ? o.l : o.r) = sd.ola[(size_t) sd.read++];
        }
        return o;
    }

private:
    struct Side
    {
        std::vector<float> ola;
        int read = 0;
        Rng rng;
    };

    void rebuild()
    {
        const int half = n / 2;
        for (int k = 0; k <= half; ++k)
        {
            float p = base[(size_t) k] * base[(size_t) k];
            if (octUp)
            {
                const float src = (float) k * 0.5f;
                const int i0 = (int) src;
                const float fr = src - (float) i0;
                const float m = lerpf (base[(size_t) i0], base[(size_t) std::min (half, i0 + 1)], fr);
                p += 0.42f * m * m;
            }
            if (octDown && 2 * k + 1 <= half)
            {
                const float m = std::sqrt (base[(size_t) (2 * k)] * base[(size_t) (2 * k)] + base[(size_t) (2 * k + 1)] * base[(size_t) (2 * k + 1)]);
                p += 0.55f * m * m;
            }
            mags[(size_t) k] = std::sqrt (p);
        }
        const float layers = 1.0f + (octUp ? 0.42f : 0.0f) + (octDown ? 0.55f : 0.0f);
        gain = (1.0f / 0.75f) / std::sqrt (layers); // keeps the level steady as octaves are added
    }

    void synthFrame (Side& sd)
    {
        const int half = n / 2;
        work[0] = { 0.0f, 0.0f };
        for (int k = 1; k < half; ++k)
        {
            const float ph = kPi * sd.rng.next();
            const auto c = std::polar (mags[(size_t) k], ph);
            work[(size_t) k] = c;
            work[(size_t) (n - k)] = std::conj (c);
        }
        work[(size_t) half] = { 0.0f, 0.0f };
        fft.run (work.data(), true);
        for (int i = 0; i < n; ++i)
            sd.ola[(size_t) i] += work[(size_t) i].real() * window[(size_t) i] * gain;
    }

    float fs = 48000.0f, gain = 1.333f, captureRms = 0.0f;
    int n = 8192, hop = 2048;
    bool has = false, octDown = false, octUp = false;
    Fft fft;
    std::vector<float> window, base, mags;
    std::vector<std::complex<float>> work;
    Side side[2];
};

// ---------------------------------------------------------------------------------
// Synth: finds the notes in the captured spectrum and plays them on oscillators.
// A is hard and digital, B is airy and chorused. Dimension sets the low-pass.
// ---------------------------------------------------------------------------------
class SynthVoice
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
    }

    // Picks up to three notes from the spectrum (fundamentals, not their harmonics)
    void setFromSpectrum (const std::vector<float>& mag, int fftSize, float rms, int midiNote, bool bass)
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
            const int half = fftSize / 2;
            const float binHz = fs / (float) fftSize;
            const float lo = bass ? 28.0f : 55.0f, hi = bass ? 1000.0f : 2200.0f;
            struct Peak { float f, m; };
            std::vector<Peak> peaks;
            float maxM = 0.0f;
            for (int k = 2; k < half - 1; ++k)
            {
                const float m = mag[(size_t) k];
                if (m > mag[(size_t) k - 1] && m >= mag[(size_t) k + 1])
                {
                    const float a = mag[(size_t) k - 1], c = mag[(size_t) k + 1];
                    const float d = 0.5f * (a - c) / std::max (1.0e-9f, a - 2.0f * m + c);
                    const float f = ((float) k + clampf (d, -0.5f, 0.5f)) * binHz;
                    if (f >= lo && f <= hi) { peaks.push_back ({ f, m }); maxM = std::max (maxM, m); }
                }
            }
            std::sort (peaks.begin(), peaks.end(), [] (const Peak& a, const Peak& b) { return a.m > b.m; });
            auto related = [] (float a, float b) // is b a harmonic of a?
            {
                const float r = b / a;
                const float nearest = std::round (r);
                return nearest >= 1.0f && nearest <= 10.0f && std::fabs (r - nearest) < 0.03f * nearest;
            };
            for (const auto& p : peaks)
            {
                if (p.m < maxM * 0.18f || count == kMaxNotes) break;
                bool skip = false;
                for (int i = 0; i < count; ++i)
                {
                    if (related (freqs[i], p.f)) { skip = true; break; }
                    if (related (p.f, freqs[i])) { freqs[i] = p.f; skip = true; break; } // found the fundamental
                }
                if (! skip) { freqs[count] = p.f; amps[count] = p.m; ++count; }
            }
            float top = 1.0e-9f;
            for (int i = 0; i < count; ++i) top = std::max (top, amps[i]);
            for (int i = 0; i < count; ++i) amps[i] = std::sqrt (amps[i] / top);
        }

        for (int i = 0; i < kMaxNotes; ++i)
        {
            note[i].freq = i < count ? freqs[i] : note[i].freq;
            note[i].amp = i < count ? amps[i] : 0.0f;
        }
        float sum = 0.0f;
        for (int i = 0; i < count; ++i) sum += note[i].amp;
        targetLevel = count > 0 ? clampf (rms * 3.2f, 0.02f, 0.6f) / std::max (1.0f, sum) : 0.0f;
        has = count > 0;
    }

    bool hasNotes() const { return has; }

    // type 0 = Synth A, 1 = Synth B. dim 0..1 sets the low-pass cutoff.
    Stereo next (int type, float dim)
    {
        if (++ctl >= 16)
        {
            ctl = 0;
            const float fc = cutoff.next (60.0f * std::pow (2.0f, clampf (dim, 0.0f, 1.0f) * 8.2f));
            const float q = type == 0 ? 1.1f : 0.75f;
            filt[0].set (fc, q, fs);
            filt[1].set (fc, q, fs);
        }
        const float lvl = level.next (has ? targetLevel : 0.0f);
        vib += 0.23f / fs;
        if (vib >= 1.0f) vib -= 1.0f;
        const float vibCents = type == 1 ? 3.0f * std::sin (kTwoPi * vib) : 0.0f;

        Stereo o;
        for (int i = 0; i < kMaxNotes; ++i)
        {
            auto& nt = note[i];
            if (nt.amp <= 0.0f && nt.env <= 1.0e-4f) continue;
            nt.env += ((nt.amp > 0.0f ? nt.amp : 0.0f) - nt.env) * 0.002f;
            if (type == 0)
            {
                const float inc = nt.freq / fs;
                const float s = 0.5f * saw (nt.ph[0], inc) + 0.5f * square (nt.ph[0], inc);
                advance (nt.ph[0], inc);
                o.l += s * nt.env;
                o.r += s * nt.env;
            }
            else
            {
                static const float detune[3] = { -9.0f, 0.0f, 9.0f };
                static const float pan[3] = { -0.75f, 0.0f, 0.75f };
                for (int d = 0; d < 3; ++d)
                {
                    const float inc = nt.freq * std::pow (2.0f, (detune[d] + vibCents * (d == 1 ? -1.0f : 1.0f)) / 1200.0f) / fs;
                    const float s = 0.42f * saw (nt.ph[d], inc);
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
// Plays a buffer as a seamless loop: the last few milliseconds crossfade into the start.
// ---------------------------------------------------------------------------------
struct LoopReader
{
    const float* buf = nullptr;
    int len = 0, xf = 0, pos = 0, nextLen = 0;
    float gain = 0.0f, gainStep = 0.0f, target = 0.0f;

    void start (const float* b, int length, int fadeSamples, float fadeIn)
    {
        buf = b;
        len = nextLen = std::max (16, length);
        xf = std::min (fadeSamples, len / 4);
        pos = xf;
        target = 1.0f;
        gainStep = 1.0f / std::max (1.0f, fadeIn);
    }
    void stop (float fadeOut) { target = 0.0f; gainStep = 1.0f / std::max (1.0f, fadeOut); }
    void setLength (int length) { nextLen = std::max (16, length); } // takes effect at the loop point
    bool active() const { return buf != nullptr && (gain > 0.0f || target > 0.0f); }

    float next()
    {
        if (buf == nullptr) return 0.0f;
        gain += clampf (target - gain, -gainStep, gainStep);
        if (gain <= 0.0f && target <= 0.0f) { buf = nullptr; return 0.0f; }

        float v;
        const int fadeStart = len - xf;
        if (pos >= fadeStart)
        {
            const float t = (float) (pos - fadeStart) / (float) std::max (1, xf);
            const float a = std::cos (t * kPi * 0.5f), b = std::sin (t * kPi * 0.5f);
            v = buf[pos] * a + buf[pos - fadeStart] * b;
        }
        else
        {
            v = buf[pos];
        }
        if (++pos >= len)
        {
            pos = xf;
            if (nextLen != len)
            {
                len = nextLen;
                xf = std::min (xf, len / 4);
                if (pos >= len - xf) pos = 0;
            }
        }
        return v * gain;
    }
};

// What the engine asks for when a channel's sound starts
struct EnvRequest
{
    Envelope::Mode mode = Envelope::Mode::OneShot;
    int shape = ShapeSwell;
    float duration = 1.0f, randomRate = 4.0f, repeatRate = 1.0f, phase = 0.0f;
};

class Channel
{
public:
    static constexpr float kRawSeconds = 0.5f;
    static constexpr float kFreezeOffset = 0.035f; // skip the pick attack, like the pedal's Blur
    static constexpr int kGlitchLengths = 4;

    void prepare (float sampleRate, uint32_t seed)
    {
        fs = sampleRate;
        rawLen = (int) (kRawSeconds * fs);
        raw.assign ((size_t) rawLen + 8, 0.0f);
        glitchBuf[0].assign ((size_t) rawLen + 8, 0.0f);
        glitchBuf[1].assign ((size_t) rawLen + 8, 0.0f);
        env.prepare (fs);
        freeze.prepare (fs);
        synth.prepare (fs);
        rawFill = 0;
        rawActive = false;
        haveFreeze = haveSynth = haveGlitch = false;
        startPending = false;
        rng.s = seed;
        (void) rng.next();
    }

    static int glitchLength (int index, float sampleRate)
    {
        static const float ms[kGlitchLengths] = { 45.0f, 90.0f, 180.0f, 360.0f };
        return (int) (ms[std::min (std::max (index, 0), kGlitchLengths - 1)] * 0.001f * sampleRate);
    }

    // A new note: start recording it. 'pre' samples before the attack come from 'preRoll'.
    void trigger (const float* preRoll, int pre, int midiNote, const EnvRequest& req, bool startNow)
    {
        pre = std::min (pre, rawLen / 8);
        for (int i = 0; i < pre; ++i) raw[(size_t) i] = preRoll[i];
        rawFill = pre;
        preLen = pre;
        rawActive = true;
        note = midiNote;
        haveFreeze = haveSynth = haveGlitch = false;
        pending = req;
        startPending = true;
        if (startNow) startEnvelope();
    }

    // Footswitch / Mono cancel / Glitch B
    void startEnvelope()
    {
        startPending = false;
        switch (pending.mode)
        {
            case Envelope::Mode::Sustain: env.startSustain (pending.shape, pending.duration, pending.randomRate); break;
            case Envelope::Mode::Repeat:  env.startRepeat (pending.shape, pending.repeatRate, pending.phase); break;
            default:                      env.startOneShot (pending.shape, pending.duration, pending.randomRate); break;
        }
    }

    void release() { startPending = false; env.release(); }
    void kill (float seconds) { startPending = false; env.kill (seconds); }
    Envelope& envelope() { return env; }
    const Envelope& envelope() const { return env; }

    // Feed the incoming audio (mono). Builds content as soon as enough has been heard.
    void feed (float x, int group, int voice, int glitchIndex, bool bass)
    {
        if (rawActive)
        {
            raw[(size_t) rawFill++] = x;
            if (rawFill >= rawLen) rawActive = false;
        }
        if (rawFill == 0) return;
        ensureContent (group, voice, glitchIndex, bass);
    }

    // Call when the voice changes: rebuilds the held note in the new voice
    void ensureContent (int group, int voice, int glitchIndex, bool bass)
    {
        if (group == GroupBlur || group == GroupSynth)
        {
            const int offset = preLen + (int) (kFreezeOffset * fs);
            if (! haveFreeze && rawFill >= std::min (rawLen, freeze.samplesNeeded (offset)))
            {
                const int start = std::max (0, std::min (offset, rawLen - freeze.samplesNeeded (0)));
                freeze.analyse (raw.data(), start);
                haveFreeze = true;
            }
            freeze.setOctaves (voice == VBlurDown || voice == VBlurBoth, voice == VBlurUp || voice == VBlurBoth);
            if (group == GroupSynth && haveFreeze && ! haveSynth)
            {
                synth.setFromSpectrum (freeze.spectrum(), freeze.frameSize(), freeze.rms(), note, bass);
                haveSynth = true;
            }
            if (startPending && haveFreeze) startEnvelope();
        }
        else if (group == GroupGlitchA)
        {
            const int len = glitchLength (glitchIndex, fs);
            if (! haveGlitch && rawFill >= std::min (rawLen, preLen + len))
            {
                cur ^= 1;
                std::copy (raw.begin(), raw.begin() + rawFill, glitchBuf[cur].begin());
                glitchFill = rawFill;
                if (loop.active()) old = loop; // crossfade from the previous loop
                old.stop (0.015f * fs);
                loop = LoopReader();
                loop.start (glitchBuf[cur].data() + preLen, std::min (len, glitchFill - preLen), (int) (0.006f * fs), 0.004f * fs);
                haveGlitch = true;
            }
            if (haveGlitch)
            {
                // Longer lengths can only use what has been recorded so far
                const int avail = (rawActive ? rawFill : glitchFill) - preLen;
                if (! rawActive && glitchFill < rawFill) // keep copying while the note is still being heard
                    glitchFill = rawFill;
                loop.setLength (std::min (len, std::max (16, avail)));
            }
            if (startPending && haveGlitch) startEnvelope();
        }
        else if (startPending)
        {
            startEnvelope();
        }

        // While recording continues, keep the glitch copy topped up
        if (group == GroupGlitchA && haveGlitch && rawActive)
        {
            glitchBuf[cur][(size_t) rawFill - 1] = raw[(size_t) rawFill - 1];
            glitchFill = rawFill;
        }
    }

    // The channel's content (before its envelope). Swell and Glitch B pass their own source in.
    Stereo source (int group, int voice, float dim, float external)
    {
        switch (group)
        {
            case GroupBlur:
                return haveFreeze ? freeze.next() : Stereo {};
            case GroupSynth:
                if (! haveFreeze) return {};
                return synth.next (voice == VSynthA ? 0 : 1, dim);
            case GroupGlitchA:
            {
                const float v = loop.next() + old.next();
                return { v, v };
            }
            default:
                return { external, external };
        }
    }

    float nextEnv() { return env.next(); }
    bool active() const { return env.isActive() || startPending; }
    bool waiting() const { return startPending; }

    // Recorded note for Glitch B's memory
    const float* rawData() const { return raw.data() + preLen; }
    int rawSamples() const { return std::max (0, rawFill - preLen); }
    bool recording() const { return rawActive; }

private:
    float fs = 48000.0f;
    std::vector<float> raw;
    int rawLen = 0, rawFill = 0, preLen = 0, note = -1;
    bool rawActive = false;
    bool haveFreeze = false, haveSynth = false, haveGlitch = false, startPending = false;
    EnvRequest pending;
    Envelope env;
    FreezeVoice freeze;
    SynthVoice synth;
    std::vector<float> glitchBuf[2];
    int cur = 0, glitchFill = 0;
    LoopReader loop, old;
    Rng rng;
};
} // namespace jet

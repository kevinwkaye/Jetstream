#pragma once

// Small, dependency-free DSP helpers for the Jetstream engine.
// Everything in Source/engine is plain C++17 so it can be tested without JUCE.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace jet
{
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clampf (float x, float lo, float hi) { return std::min (std::max (x, lo), hi); }
inline float lerpf (float a, float b, float t) { return a + (b - a) * t; }
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

// Transparent below 0.85, gently limits above. Keeps feedback paths from running away.
inline float softClip (float x)
{
    const float ax = std::fabs (x);
    if (ax <= 0.85f)
        return x;
    const float s = x > 0.0f ? 1.0f : -1.0f;
    return s * (0.85f + 0.15f * std::tanh ((ax - 0.85f) / 0.15f));
}

// The output ceiling: untouched below -2.5 dBFS, never above -1 dBFS.
inline float softCeiling (float x)
{
    constexpr float knee = 0.75f, room = 0.141f; // 0.75 + 0.141 = 0.891 (-1 dBFS)
    const float ax = std::fabs (x);
    if (ax <= knee) return x;
    const float s = x > 0.0f ? 1.0f : -1.0f;
    return s * (knee + room * std::tanh ((ax - knee) / room));
}

// Raised cosine 0..1, for fades with no corners
inline float raisedCos (float t)
{
    t = std::min (std::max (t, 0.0f), 1.0f);
    return 0.5f - 0.5f * std::cos (3.14159265358979f * t);
}

struct Stereo
{
    float l = 0.0f, r = 0.0f;
    Stereo operator+ (Stereo o) const { return { l + o.l, r + o.r }; }
    Stereo operator* (float g) const { return { l * g, r * g }; }
    Stereo& operator+= (Stereo o) { l += o.l; r += o.r; return *this; }
};

// Equal-power pan, pan in [-1, 1]
inline Stereo panMono (float v, float pan)
{
    const float a = (clampf (pan, -1.0f, 1.0f) + 1.0f) * (kPi * 0.25f);
    return { v * std::cos (a) * 1.41421356f, v * std::sin (a) * 1.41421356f };
}

struct OnePole
{
    float z = 0.0f, a = 0.0f;
    void setCutoff (float fc, float fs) { a = std::exp (-kTwoPi * clampf (fc, 1.0f, fs * 0.49f) / fs); }
    float process (float x) { z = x + a * (z - x); return z; }
    void reset() { z = 0.0f; }
};

// Glides a control value towards its target so nothing zips.
struct Smoother
{
    float v = 0.0f, k = 1.0f;
    bool primed = false;
    void setTime (float seconds, float fs) { k = 1.0f - std::exp (-1.0f / std::max (1.0f, seconds * fs)); }
    float next (float target)
    {
        if (! primed) { v = target; primed = true; }
        v += (target - v) * k;
        return v;
    }
};

struct Rng
{
    uint32_t s = 0x9E3779B9u;
    float next() // -1..1
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) (s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    float uni() { return next() * 0.5f + 0.5f; } // 0..1
};

// Zero-delay state variable filter (Simper). Low-pass output.
struct Svf
{
    float ic1 = 0, ic2 = 0, g = 0, k = 1.4f, a1 = 0, a2 = 0, a3 = 0;
    void set (float fc, float q, float fs)
    {
        g = std::tan (kPi * clampf (fc, 10.0f, fs * 0.45f) / fs);
        k = 1.0f / std::max (0.3f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    float lp (float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }
    void reset() { ic1 = ic2 = 0; }
};

// Power-of-two circular delay line with cubic fractional reads.
// tap(1) returns the most recently pushed sample.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void init (int sizePow2) { buf.assign ((size_t) sizePow2, 0.0f); mask = sizePow2 - 1; w = 0; }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
    int size() const { return mask + 1; }
    void push (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
    float at (int back) const { return buf[(size_t) ((w - back) & mask)]; }

    float tap (float delay) const
    {
        delay = clampf (delay, 2.0f, (float) (mask - 3));
        const int d = (int) delay;
        const float f = delay - (float) d;
        const float y0 = at (d - 1), y1 = at (d), y2 = at (d + 1), y3 = at (d + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
};

inline int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place iterative radix-2 FFT. Twiddles are built once per size.
class Fft
{
public:
    void init (int n)
    {
        size = n;
        tw.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
            tw[(size_t) i] = std::polar (1.0f, -kTwoPi * (float) i / (float) n);
        rev.resize ((size_t) n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    int getSize() const { return size; }

    void run (std::complex<float>* x, bool inverse) const
    {
        const int n = size;
        for (int i = 0; i < n; ++i)
            if (i < rev[(size_t) i]) std::swap (x[i], x[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    auto w = tw[(size_t) (j * step)];
                    if (inverse) w = std::conj (w);
                    const auto u = x[i + j], v = x[i + j + half] * w;
                    x[i + j] = u + v;
                    x[i + j + half] = u - v;
                }
        }
        if (inverse)
        {
            const float s = 1.0f / (float) n;
            for (int i = 0; i < n; ++i) x[i] *= s;
        }
    }

private:
    int size = 0;
    std::vector<std::complex<float>> tw;
    std::vector<int> rev;
};
// 2x oversampling with a 47-tap Kaiser halfband filter (stopband about 80 dB down).
// Used around the drive and the Swell B fuzz so their harmonics don't fold back.
// Adds kLatency input samples of delay.
class Oversampler2x
{
public:
    static constexpr int kSide = 12;            // non-zero taps each side
    static constexpr int kLatency = 2 * kSide - 1; // up + down, in input samples

    Oversampler2x()
    {
        auto bessel0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };
        const double beta = 8.0, half = 2.0 * kSide; // taps run -(2*kSide-1)..(2*kSide-1)
        double total = 0.0;
        for (int i = 0; i < kSide; ++i)
        {
            const double k = 2.0 * i + 1.0;
            const double sinc = std::sin (3.14159265358979 * k * 0.5) / (3.14159265358979 * k * 0.5);
            const double r = k / half;
            const double w = bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel0 (beta);
            c[i] = (float) (0.5 * sinc * w);
            total += 2.0 * c[i];
        }
        const float norm = (float) (0.5 / total); // DC gain exactly 1
        for (float& v : c) v *= norm;
        reset();
    }

    void reset()
    {
        std::fill (std::begin (up), std::end (up), 0.0f);
        std::fill (std::begin (dn), std::end (dn), 0.0f);
        upPos = dnPos = 0;
    }

    // One input sample -> two samples at twice the rate
    void upsample (float x, float& a, float& b)
    {
        up[upPos] = x;
        up[upPos + kHist] = x;
        const float* h = up + upPos + 1; // h[kHist-1] is the newest sample
        const int centre = kHist - 1 - kSide;
        a = h[centre];
        float acc = 0.0f;
        for (int i = 0; i < kSide; ++i)
            acc += c[i] * (h[centre - i] + h[centre + 1 + i]);
        b = 2.0f * acc;
        upPos = (upPos + 1) % kHist;
    }

    // Two samples at twice the rate -> one output sample
    float downsample (float a, float b)
    {
        dn[dnPos] = a; dn[dnPos + kDown] = a;
        dnPos = (dnPos + 1) % kDown;
        dn[dnPos] = b; dn[dnPos + kDown] = b;
        dnPos = (dnPos + 1) % kDown;
        const float* h = dn + dnPos; // h[kDown-1] newest
        const int centre = kDown - 1 - (2 * kSide - 1);
        float acc = 0.5f * h[centre];
        for (int i = 0; i < kSide; ++i)
            acc += c[i] * (h[centre - (2 * i + 1)] + h[centre + (2 * i + 1)]);
        return acc;
    }

private:
    static constexpr int kHist = 2 * kSide + 1;
    static constexpr int kDown = 4 * kSide;
    float c[kSide];
    float up[2 * kHist];
    float dn[2 * kDown];
    int upPos = 0, dnPos = 0;
};
// 4x oversampling: two halfband stages. Latency rounds to a whole input sample.
class Oversampler4x
{
public:
    static constexpr int kLatency = 35; // 23 at 1x, (23 + 1) / 2 at 2x
    void reset() { a.reset(); b.reset(); dB.reset(); dA.reset(); held = 0.0f; }

    template <typename Fn>
    float process (float x, Fn&& shape)
    {
        float u0, u1;
        a.upsample (x, u0, u1);
        // one sample of delay at 2x keeps the total latency a whole number of input samples
        const float d = held; held = u1; u1 = u0; u0 = d;
        float q[4];
        b.upsample (u0, q[0], q[1]);
        b.upsample (u1, q[2], q[3]);
        for (float& v : q) v = shape (v);
        const float d0 = dB.downsample (q[0], q[1]);
        const float d1 = dB.downsample (q[2], q[3]);
        return dA.downsample (d0, d1);
    }

private:
    Oversampler2x a, b, dB, dA;
    float held = 0.0f;
};
// RBJ biquad (transposed direct form II)
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (bool highPass, float fc, float q, float fs)
    {
        const float w = kTwoPi * clampf (fc, 5.0f, fs * 0.45f) / fs, cw = std::cos (w), al = std::sin (w) / (2.0f * q);
        const float a0 = 1.0f + al;
        if (highPass) { b0 = (1.0f + cw) * 0.5f / a0; b1 = -(1.0f + cw) / a0; }
        else          { b0 = (1.0f - cw) * 0.5f / a0; b1 = (1.0f - cw) / a0; }
        b2 = b0; a1 = -2.0f * cw / a0; a2 = (1.0f - al) / a0;
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// Linkwitz-Riley 4th order (two Butterworth stages): flat when the bands are summed
struct LR4
{
    Biquad s[2];
    void set (bool highPass, float fc, float fs) { for (auto& b : s) { b.set (highPass, fc, 0.70710678f, fs); b.reset(); } }
    float process (float x) { return s[1].process (s[0].process (x)); }
};
} // namespace jet

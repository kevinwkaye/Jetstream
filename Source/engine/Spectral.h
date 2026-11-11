#pragma once

// From Afterglow (shared family code). A caught note becomes a set of partials (each a measured frequency
// and level) plus a soft bed of the note's own breath. The partials ring as pure,
// phase-continuous sines, so a held note keeps its exact pitch and timbre with no loop
// point, no smear and no frame-rate grain. Each partial drifts very slightly in level
// and sits at its own place in the stereo field, which keeps the sustain alive.
//
// Analysis: two overlapping FFT frames give each peak's true frequency from the phase
// change between them. Synthesis: every partial writes its window's spectral shape into
// one stereo spectrum per frame (inverse FFT synthesis), so a hundred partials cost
// little more than one.
//
// Plain C++17, no JUCE.

#include "DspUtil.h"
#include <array>
#include <complex>

namespace jet
{
using cfloat = std::complex<float>;

constexpr int kMaxPartials = 128;

struct Partial
{
    float omega = 0.0f;       // radians per sample, detune included
    float amp = 0.0f;         // sine amplitude
    cfloat zL { 1, 0 }, zR { 1, 0 }, rot { 1, 0 };   // phase now (left, right) and per-frame turn
    float wobPh = 0.0f, wobStep = 0.0f;               // slow level drift
    std::array<cfloat, 3> resL {}, resR {}, resRot {}; // the resonator's partials
    int resDir = 0;                                   // +1 upper, -1 lower, 0 not set up
};

// The sound of one layer
struct Voice
{
    std::array<Partial, kMaxPartials> p;
    int np = 0;
    std::vector<float> noise;   // breath and body: magnitude envelope per analysis bin
    int noiseBins = 0;
    float noiseBinHz = 10.0f;
    float noiseScale = 0.0f;    // turns analysis magnitudes into synthesis magnitudes

    void reserve (int maxBins) { noise.assign ((size_t) maxBins, 0.0f); }

    void copyFrom (const Voice& o)
    {
        np = o.np;
        std::copy (o.p.begin(), o.p.begin() + o.np, p.begin());
        noiseBins = std::min (o.noiseBins, (int) noise.size());
        std::copy (o.noise.begin(), o.noise.begin() + noiseBins, noise.begin());
        noiseBinHz = o.noiseBinHz;
        noiseScale = o.noiseScale;
    }

    float noiseAt (float hz) const
    {
        const float x = hz / noiseBinHz;
        const int i = (int) x;
        if (i < 0 || i + 1 >= noiseBins) return 0.0f;
        return lerpf (noise[(size_t) i], noise[(size_t) i + 1], x - (float) i);
    }
};

// ---------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------
class Analyser
{
public:
    void prepare (float sampleRate, int size, int synthSize, int synthHop)
    {
        fs = sampleRate;
        n = size;
        half = n / 2;
        dh = n / 16;
        ns = synthSize;
        hopS = synthHop;
        fft.init (n);
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) n);
        buf.assign ((size_t) n, {});
        mag.assign ((size_t) half + 1, 0.0f);
        phA.assign ((size_t) half + 1, 0.0f);
        phB.assign ((size_t) half + 1, 0.0f);
        flo.assign ((size_t) half + 1, 0.0f);
        tmp.assign ((size_t) half + 1, 0.0f);
        pre.assign ((size_t) half + 2, 0.0);
        peaks.reserve ((size_t) half);
    }

    int size() const { return n; }
    int span() const { return n + dh; }
    float binHz() const { return fs / (float) n; }
    const std::vector<float>& magnitudes() const { return mag; }

    // amplitude of a sine whose peak is at bin k (Hann window)
    float ampAtBin (float bin) const
    {
        const int k = (int) std::lround (bin);
        float m = 0.0f;
        for (int d = -1; d <= 1; ++d)
            if (k + d > 0 && k + d < half) m = std::max (m, mag[(size_t) (k + d)]);
        return 4.0f * m / (float) n;
    }

    // Reads n + n/16 samples of history from start. Returns the frame's RMS.
    template <typename Read>
    float measure (Read&& sampleAt, int64_t start)
    {
        // both frames in one complex FFT: frame A real, frame B imaginary
        double energy = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float a = sampleAt (start + i), b = sampleAt (start + dh + i);
            energy += (double) a * a;
            buf[(size_t) i] = { a * window[(size_t) i], b * window[(size_t) i] };
        }
        fft.run (buf.data(), false);
        for (int k = 0; k <= half; ++k)
        {
            const cfloat x = buf[(size_t) k], y = std::conj (buf[(size_t) ((n - k) & (n - 1))]);
            const cfloat A = 0.5f * (x + y);
            const cfloat B = cfloat (0.0f, -0.5f) * (x - y);
            mag[(size_t) k] = 0.5f * (std::abs (A) + std::abs (B));
            phA[(size_t) k] = std::arg (A);
            phB[(size_t) k] = std::arg (B);
        }
        return (float) std::sqrt (energy / n);
    }

    // Turns the measured frames into a voice. Returns false if nothing tonal or noisy was found.
    bool build (Voice& v, Rng& rng, float noiseGain)
    {
        const int lo = std::max (2, (int) (24.0f / binHz())), hi = std::min (half - 6, (int) (std::min (18500.0f, fs * 0.45f) / binHz()));

        // local floor: a moving average with the peaks clipped off, run twice
        smooth (mag, flo, [] (int k) { return 4 + k / 24; });
        for (int k = 0; k <= half; ++k) tmp[(size_t) k] = std::min (mag[(size_t) k], 2.0f * flo[(size_t) k]);
        smooth (tmp, flo, [] (int k) { return 4 + k / 24; });

        float top = 0.0f;
        for (int k = lo; k <= hi; ++k) top = std::max (top, mag[(size_t) k]);
        if (top <= 1.0e-9f) return false;
        const float gate = top * 0.0005f; // 66 dB under the loudest partial

        peaks.clear();
        for (int k = lo; k <= hi; ++k)
        {
            const float m = mag[(size_t) k];
            if (m < gate || m < 1.8f * flo[(size_t) k]) continue;
            if (m <= mag[(size_t) k - 1] || m < mag[(size_t) k + 1] || m < mag[(size_t) k - 2] || m < mag[(size_t) k + 2]) continue;
            peaks.push_back (k);
        }
        std::sort (peaks.begin(), peaks.end(), [this] (int a, int b) { return mag[(size_t) a] > mag[(size_t) b]; });
        if ((int) peaks.size() > kMaxPartials) peaks.resize ((size_t) kMaxPartials);

        v.np = 0;
        const float binW = kTwoPi / (float) n;
        for (int k : peaks)
        {
            // level: parabola through the log magnitudes
            const float a = std::log (mag[(size_t) k - 1] + 1.0e-12f), b = std::log (mag[(size_t) k] + 1.0e-12f), c = std::log (mag[(size_t) k + 1] + 1.0e-12f);
            const float den = a - 2.0f * b + c;
            const float off = std::fabs (den) > 1.0e-9f ? clampf (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
            const float peakMag = std::exp (b - 0.25f * (a - c) * off);

            // frequency: from the phase turn between the two frames
            float dphi = phB[(size_t) k] - phA[(size_t) k] - binW * (float) k * (float) dh;
            dphi -= kTwoPi * std::round (dphi / kTwoPi);
            float omega = binW * (float) k + dphi / (float) dh;
            if (std::fabs (omega - binW * ((float) k + off)) > 1.2f * binW) omega = binW * ((float) k + off);

            auto& p = v.p[(size_t) v.np++];
            p.omega = omega * std::pow (2.0f, rng.next() * 1.1f / 1200.0f); // a hair of detune, like a second string
            p.amp = 4.0f * peakMag / (float) n;
            const float ph = kTwoPi * rng.uni();
            const float spread = 1.05f * rng.next(); // up to 60 degrees between the sides keeps mono solid
            p.zL = std::polar (1.0f, ph);
            p.zR = std::polar (1.0f, ph + spread);
            p.rot = std::polar (1.0f, p.omega * (float) hopS);
            p.wobPh = kTwoPi * rng.uni();
            p.wobStep = kTwoPi * (0.07f + 0.28f * rng.uni()) * (float) hopS / fs;
            p.resDir = 0;
        }

        // breath and body: what's left once the partials are taken out, smoothed
        for (int k = 0; k <= half; ++k) tmp[(size_t) k] = mag[(size_t) k];
        for (int k : peaks)
            for (int d = -3; d <= 3; ++d)
            {
                const int j = k + d;
                if (j > 0 && j < half) tmp[(size_t) j] = std::min (tmp[(size_t) j], flo[(size_t) j]);
            }
        smooth (tmp, flo, [] (int k) { return 2 + k / 32; });
        v.noiseBins = std::min (half + 1, (int) v.noise.size());
        for (int k = 0; k < v.noiseBins; ++k) v.noise[(size_t) k] = (k >= lo && k <= hi) ? flo[(size_t) k] : 0.0f;
        v.noiseBinHz = binHz();
        // a windowed-noise magnitude here becomes the same loudness in the synthesis
        v.noiseScale = noiseGain * std::sqrt (32.0f * (float) ns / (float) n);
        return v.np > 0 || noiseGain > 0.0f;
    }

private:
    template <typename Width>
    void smooth (const std::vector<float>& in, std::vector<float>& out, Width&& width)
    {
        pre[0] = 0.0;
        for (int k = 0; k <= half; ++k) pre[(size_t) k + 1] = pre[(size_t) k] + in[(size_t) k];
        for (int k = 0; k <= half; ++k)
        {
            const int w = width (k);
            const int a = std::max (0, k - w), b = std::min (half, k + w);
            out[(size_t) k] = (float) ((pre[(size_t) b + 1] - pre[(size_t) a]) / (double) (b - a + 1));
        }
    }

    float fs = 48000.0f;
    int n = 4096, half = 2048, dh = 256, ns = 4096, hopS = 512;
    Fft fft;
    std::vector<float> window, mag, phA, phB, flo, tmp;
    std::vector<double> pre;
    std::vector<cfloat> buf;
    std::vector<int> peaks;
};

// ---------------------------------------------------------------------------------
// Synthesis: one stereo spectrum per frame, inverse FFT, windowed overlap-add
// ---------------------------------------------------------------------------------
class Synth
{
public:
    static constexpr int kTaps = 9;     // the Hann window's spectral shape, 4 bins either side
    static constexpr int kFrac = 64;    // fractional bin steps in the table

    void prepare (float sampleRate, int size)
    {
        fs = sampleRate;
        n = size;
        hop = n / 8;
        half = n / 2;
        fft.init (n);
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) n);

        // W(d) = sum over the frame of w[t] e^(-i 2 pi d t / n), for d = tap - 4 - frac.
        // With w = 0.5 - 0.25 (e^(i2pi t/n) + e^(-i2pi t/n)) it is three geometric sums.
        const double pi = 3.14159265358979323846;
        auto D = [&] (double x)
        {
            if (std::fabs (x) < 1.0e-9) return std::complex<double> ((double) n, 0.0);
            return (1.0 - std::polar (1.0, -2.0 * pi * x)) / (1.0 - std::polar (1.0, -2.0 * pi * x / n));
        };
        for (int q = 0; q <= kFrac; ++q)
            for (int t = 0; t < kTaps; ++t)
            {
                const double d = (double) (t - kTaps / 2) - (double) q / kFrac;
                const auto s = 0.5 * D (d) - 0.25 * D (d - 1.0) - 0.25 * D (d + 1.0);
                kernel[(size_t) q][(size_t) t] = cfloat ((float) s.real(), (float) s.imag());
            }

        specL.assign ((size_t) n, {});
        specR.assign ((size_t) n, {});
        noiseL.assign ((size_t) half + 1, 0.0f);
        noiseR.assign ((size_t) half + 1, 0.0f);
        olaL.assign ((size_t) n, 0.0f);
        olaR.assign ((size_t) n, 0.0f);
        readPos = hop;
        anyNoise = false;
    }

    int size() const { return n; }
    int hopSize() const { return hop; }
    float binHz() const { return fs / (float) n; }

    void beginFrame()
    {
        std::fill (specL.begin(), specL.end(), cfloat {});
        std::fill (specR.begin(), specR.end(), cfloat {});
        std::fill (noiseL.begin(), noiseL.end(), 0.0f);
        std::fill (noiseR.begin(), noiseR.end(), 0.0f);
        anyNoise = false;
    }

    // a sine of the given frequency, with complex amplitudes for each side
    inline void addSine (float omega, cfloat cl, cfloat cr)
    {
        const float b = omega * (float) n / kTwoPi;
        if (b < 1.0f || b > (float) half - 6.0f) return;
        const int kb = (int) b;
        const int q = (int) ((b - (float) kb) * kFrac + 0.5f);
        const auto& k = kernel[(size_t) q];
        const int base = kb - kTaps / 2;
        const int mask = n - 1;
        for (int t = 0; t < kTaps; ++t)
        {
            const int idx = (base + t) & mask;
            specL[(size_t) idx] += cl * k[(size_t) t];
            specR[(size_t) idx] += cr * k[(size_t) t];
        }
    }

    // breath and body: noise power per synthesis bin
    inline void addNoisePower (int k, float pl, float pr)
    {
        noiseL[(size_t) k] += pl;
        noiseR[(size_t) k] += pr;
        anyNoise = true;
    }

    // inverse FFT, window and overlap-add into the output
    void endFrame (Rng& rng)
    {
        if (anyNoise)
        {
            for (int k = 1; k < half; ++k)
            {
                if (noiseL[(size_t) k] > 0.0f) specL[(size_t) k] += std::polar (std::sqrt (noiseL[(size_t) k]), kTwoPi * rng.uni());
                if (noiseR[(size_t) k] > 0.0f) specR[(size_t) k] += std::polar (std::sqrt (noiseR[(size_t) k]), kTwoPi * rng.uni());
            }
        }
        constexpr float olaScale = 1.0f / 3.0f; // Hann squared at 8x overlap sums to 3
        fft.run (specL.data(), true);
        fft.run (specR.data(), true);
        for (int i = 0; i < n; ++i)
        {
            const float w = window[(size_t) i] * olaScale;
            olaL[(size_t) i] += specL[(size_t) i].real() * w;
            olaR[(size_t) i] += specR[(size_t) i].real() * w;
        }
    }

    bool frameDue() const { return readPos >= hop; }

    void shift()
    {
        std::copy (olaL.begin() + hop, olaL.end(), olaL.begin());
        std::copy (olaR.begin() + hop, olaR.end(), olaR.begin());
        std::fill (olaL.end() - hop, olaL.end(), 0.0f);
        std::fill (olaR.end() - hop, olaR.end(), 0.0f);
        readPos = 0;
    }

    Stereo next()
    {
        const Stereo s { olaL[(size_t) readPos], olaR[(size_t) readPos] };
        ++readPos;
        return s;
    }

    void clear()
    {
        std::fill (olaL.begin(), olaL.end(), 0.0f);
        std::fill (olaR.begin(), olaR.end(), 0.0f);
    }

private:
    float fs = 48000.0f;
    int n = 4096, hop = 512, half = 2048, readPos = 0;
    Fft fft;
    std::vector<float> window, noiseL, noiseR, olaL, olaR;
    std::vector<cfloat> specL, specR;
    std::array<std::array<cfloat, kTaps>, kFrac + 1> kernel {};
    bool anyNoise = false;
};
} // namespace jet

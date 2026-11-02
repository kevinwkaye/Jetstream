// Offline render test for the Jetstream engine (no JUCE needed).
// Plays a guitar-like phrase through every voice and mode, then checks for NaNs,
// runaway levels, clicks and silence. Pass a folder to also write WAV files.
//
// Usage: jetstream_render_test [output_folder]

#include "JetEngine.h"
#include <cstdio>
#include <functional>
#include <string>

using namespace jet;

static constexpr float kFs = 48000.0f;

// A few plucked notes and a chord, then silence
static void makeInput (std::vector<float>& l, std::vector<float>& r, int n, float lowest = 220.0f)
{
    l.assign ((size_t) n, 0.0f);
    r.assign ((size_t) n, 0.0f);
    const float ratios[6] = { 1.0f, 1.26f, 1.498f, 2.0f, 1.498f, 1.26f };
    for (int k = 0; k < 6; ++k)
    {
        const int s0 = (int) ((0.2f + k * 0.9f) * kFs);
        const bool chord = k == 3;
        for (int i = 0; i < (int) (1.6f * kFs) && s0 + i < n; ++i)
        {
            const float t = (float) i / kFs;
            const float e = std::exp (-t * 2.5f) * std::min (1.0f, t * 500.0f);
            float v = 0.0f;
            for (int c = 0; c < (chord ? 3 : 1); ++c)
            {
                const float f = lowest * ratios[k] * (c == 0 ? 1.0f : (c == 1 ? 1.26f : 1.498f));
                v += 0.22f * e * (std::sin (kTwoPi * f * t) + 0.45f * std::sin (kTwoPi * 2 * f * t) + 0.25f * std::sin (kTwoPi * 3 * f * t));
            }
            l[(size_t) (s0 + i)] += v;
            r[(size_t) (s0 + i)] += v;
        }
    }
}

static void writeWav (const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return;
    const uint32_t n = (uint32_t) l.size(), dataBytes = n * 4, sr = (uint32_t) kFs;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); u32 (16); u16 (1); u16 (2); u32 (sr); u32 (sr * 4); u16 (4); u16 (16);
    std::fwrite ("data", 1, 4, f); u32 (dataBytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        int16_t a = (int16_t) (clampf (l[i], -1, 1) * 32767.0f);
        int16_t b = (int16_t) (clampf (r[i], -1, 1) * 32767.0f);
        std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f);
    }
    std::fclose (f);
}

struct Result { float peak = 0, rms = 0, tailRms = 0, maxJump = 0, side = 0; bool nan = false; };

using Script = std::function<void (float t, EngineParams&, JetEngine&)>;

static Result render (const char* name, EngineParams p, const Script& script, const std::string& outDir, float seconds = 7.0f, float lowest = 220.0f)
{
    static JetEngine eng; // large: keep it off the stack
    eng.prepare (kFs);
    eng.clearMemory();
    const int n = (int) (seconds * kFs);
    std::vector<float> l, r;
    makeInput (l, r, n, lowest);

    const int block = 256;
    for (int s = 0; s < n; s += block)
    {
        const int len = std::min (block, n - s);
        if (script) script ((float) s / kFs, p, eng);
        eng.process (l.data() + s, r.data() + s, len, p);
    }

    Result res;
    double sum = 0, tail = 0, side = 0;
    const int tailStart = (int) (5.6f * kFs);
    for (int i = 0; i < n; ++i)
    {
        const float a = l[(size_t) i], b = r[(size_t) i];
        if (! std::isfinite (a) || ! std::isfinite (b)) res.nan = true;
        res.peak = std::max (res.peak, std::max (std::fabs (a), std::fabs (b)));
        sum += (double) a * a + (double) b * b;
        side += (double) (a - b) * (a - b);
        if (i >= tailStart) tail += (double) a * a + (double) b * b;
        if (i > 0)
        {
            // second difference: a click shows as a spike much larger than the signal's own curvature
            if (i > 1)
            {
                const float d2 = std::fabs (a - 2 * l[(size_t) i - 1] + l[(size_t) i - 2]);
                res.maxJump = std::max (res.maxJump, d2);
            }
        }
    }
    res.rms = (float) std::sqrt (sum / (2.0 * n));
    res.side = (float) std::sqrt (side / n);
    res.tailRms = (float) std::sqrt (tail / (2.0 * std::max (1, n - tailStart)));
    std::printf ("%-28s peak %.3f  rms %.4f  tail %.4f  side %.4f  jump %.3f%s\n", name, res.peak, res.rms, res.tailRms, res.side, res.maxJump, res.nan ? "  NaN!" : "");
    if (! outDir.empty()) writeWav (outDir + "/" + name + ".wav", l, r);
    return res;
}

int main (int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "";
    int failures = 0;
    auto expect = [&] (bool ok, const char* what, const char* name)
    {
        if (! ok) { std::printf ("  FAIL %s: %s\n", name, what); ++failures; }
    };
    auto check = [&] (const char* name, const Result& r, bool expectSound = true)
    {
        expect (! r.nan, "NaN in output", name);
        expect (r.peak <= 1.0f, "output over full scale", name);
        if (expectSound) expect (r.rms > 0.004f, "no effect heard", name);
        expect (r.maxJump < 0.35f, "click (sudden jump)", name);
    };

    EngineParams base;
    base.dry = 0.0f; // effect only, so silence means the effect did nothing
    base.envTime = 0.62f;
    base.sensitivity = 0.6f;

    static const char* names[kNumVoices] = { "blur-0", "blur-down", "blur-up", "blur-both", "synth-a", "synth-b", "glitch-a", "glitch-b", "swell-a", "swell-b" };
    for (int v = 0; v < kNumVoices; ++v)
    {
        auto p = base;
        p.voice = v;
        check (names[v], render (names[v], p, nullptr, out));
    }

    { auto p = base; p.voice = VBlur0; p.envTime = 0.92f; check ("blur-infinite", render ("blur-infinite", p, nullptr, out)); }
    {
        auto p = base; p.voice = VBlur0; p.envTime = 0.98f; p.envShape = 0.2f; p.trigger = 0;
        auto r = render ("blur-repeat-poly", p, nullptr, out);
        check ("blur-repeat-poly", r);
        expect (r.side > 0.002f, "A and B are not spread in stereo", "blur-repeat-poly");
    }
    { auto p = base; p.voice = VSynthB; p.trigger = 1; check ("synth-b-mono", render ("synth-b-mono", p, nullptr, out)); }
    { auto p = base; p.voice = VGlitchA; p.lfoDepth = 0.8f; p.lfoRateHz = 0.7f; check ("glitch-a-lfo", render ("glitch-a-lfo", p, nullptr, out)); }
    { auto p = base; p.voice = VSwellA; p.trigger = 1; check ("swell-a-mono", render ("swell-a-mono", p, nullptr, out)); }
    { auto p = base; p.voice = VBlurUp; p.inputMode = InputBass; check ("bass-blur-up", render ("bass-blur-up", p, nullptr, out, 7.0f, 55.0f)); }
    { auto p = base; p.voice = VBlur0; p.drive = 0.8f; p.tone = 0.4f; check ("blur-drive", render ("blur-drive", p, nullptr, out)); }
    {
        // Dimension swept by hand: the delay time glides, which must bend, not click
        auto p = base; p.voice = VBlur0;
        check ("blur-dimension-sweep", render ("blur-dimension-sweep", p, [] (float t, EngineParams& q, JetEngine&) { q.dimension = 0.5f + 0.45f * std::sin (t * 2.0f); }, out));
    }
    {
        // Manual: A held for two seconds, B toggled on and off
        auto p = base; p.voice = VBlur0; p.trigger = 2; p.fsMode = 0;
        check ("manual-toggle", render ("manual-toggle", p, [] (float t, EngineParams& q, JetEngine&)
        {
            q.footA = (t > 0.25f && t < 0.3f) || (t > 2.6f && t < 2.65f);
            q.footB = (t > 1.15f && t < 1.2f) || (t > 4.0f && t < 4.05f);
        }, out));
    }
    {
        // Glitch B: fills memory, then Hold locks it and Dimension scrolls through the blocks
        auto p = base; p.voice = VGlitchB; p.envTime = 0.97f;
        int filled = 0;
        auto r = render ("glitch-b-memory", p, [&] (float t, EngineParams& q, JetEngine& e)
        {
            q.hold = t > 4.0f;
            if (t > 4.0f) q.dimension = std::fmod (t * 0.5f, 1.0f);
            filled = 0;
            for (int b = 0; b < JetEngine::kBlocks; ++b) filled += e.uiFilled[b].load() ? 1 : 0;
        }, out);
        check ("glitch-b-memory", r);
        expect (filled >= 3, "memory did not fill", "glitch-b-memory");
        std::printf ("  memory cells filled: %d\n", filled);
    }
    {
        // Bypass with tails: the dry signal returns at unity and nothing explodes
        auto p = base; p.voice = VSwellA; p.dimension = 0.8f;
        check ("bypass-tails", render ("bypass-tails", p, [] (float t, EngineParams& q, JetEngine&) { q.bypass = t > 3.0f; }, out), true);
    }
    {
        // MIDI notes trigger the samplers too
        auto p = base; p.voice = VSynthA; p.sensitivity = 0.0f; // deaf to audio
        auto r = render ("midi-triggered", p, [] (float t, EngineParams&, JetEngine& e)
        {
            static int last = -1;
            const int step = (int) (t / 0.9f);
            if (step != last && t > 0.25f) { e.noteOn (57 + step * 2, 0.8f); last = step; }
            if (t < 0.01f) last = -1;
        }, out);
        check ("midi-triggered", r);
    }
    {
        // Learn finds a sensitivity from the phrase
        auto p = base;
        float result = -1.0f;
        render ("learn", p, [&] (float t, EngineParams&, JetEngine& e)
        {
            if (t < 0.01f) e.startLearn();
            result = e.uiLearnResult.load();
        }, out);
        std::printf ("  learned sensitivity: %.2f\n", result);
        expect (result >= 0.0f && result <= 1.0f, "learn did not settle", "learn");
    }

    std::printf (failures == 0 ? "All scenarios passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

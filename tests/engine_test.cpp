// Jetstream engine test (no JUCE). Every check has a pass mark; the build fails if any miss.
//
//   pitch    Blur holds a note within 3 cents, at least 18 dB pure, steady within 3 dB
//   octaves  Blur +1 adds the octave in tune; Synth plays the caught note (or the MIDI note)
//   level    every voice answers plucked, quiet (-30 dB), pad (no attack) and late-start
//            playing no more than 20 dB under the input
//   clicks   bypass, voice switches, retriggers, Mono hand-offs and Glitch seams, scored
//            against the test tone's own curvature (Backtrack lesson)
//   seams    Glitch A loops wobble under 1 dB, measured over whole cycles
//   output   never above -1 dBFS, silence stays silent, bass lows pass untouched,
//            drive aliasing stays well down
//   memory   Glitch B fills, Hold locks it, Learn settles
//
// Usage: jetstream_engine_test [wav_folder]

#include "ParamSpec.h"
#include <cstdio>
#include <functional>
#include <string>

using namespace jet;

static constexpr float kFs = 48000.0f;
static int failures = 0;
static std::string wavDir;

static void expect (bool ok, const char* name, const char* what)
{
    if (! ok) { std::printf ("  FAIL %s: %s\n", name, what); ++failures; }
}

// ------------------------------------------------------------------------------------
// Test signals
// ------------------------------------------------------------------------------------
using Sig = std::vector<float>;

static void addPluck (Sig& s, float at, float f, float amp, float decay = 2.5f, int harmonics = 3, float dur = 2.5f)
{
    const int s0 = (int) (at * kFs);
    for (int i = 0; i < (int) (dur * kFs) && s0 + i < (int) s.size(); ++i)
    {
        const float t = (float) i / kFs;
        const float e = std::exp (-t * decay) * std::min (1.0f, t * 500.0f);
        float v = 0.0f;
        for (int h = 1; h <= harmonics; ++h)
            v += std::sin (kTwoPi * f * (float) h * t) / (float) (h * h);
        s[(size_t) (s0 + i)] += amp * e * v;
    }
}

static Sig phrase (float seconds, float lowest = 220.0f, float gain = 1.0f, float startAt = 0.2f)
{
    Sig s ((size_t) (seconds * kFs), 0.0f);
    const float ratios[6] = { 1.0f, 1.26f, 1.498f, 2.0f, 1.498f, 1.26f };
    for (int k = 0; k < 6; ++k)
    {
        const float at = startAt + (float) k * 0.9f;
        if (at > seconds - 0.5f) break;
        if (k == 3)
        {
            addPluck (s, at, lowest, 0.3f * gain);
            addPluck (s, at, lowest * 1.26f, 0.25f * gain);
            addPluck (s, at, lowest * 1.498f, 0.25f * gain);
        }
        else addPluck (s, at, lowest * ratios[k], 0.45f * gain);
    }
    return s;
}

// a synth pad: chords that swell in over 300 ms and change with no attack
static Sig pad (float seconds)
{
    Sig s ((size_t) (seconds * kFs), 0.0f);
    const float chords[3][3] = { { 220.0f, 277.2f, 329.6f }, { 196.0f, 246.9f, 293.7f }, { 174.6f, 220.0f, 261.6f } };
    const float len = 1.6f;
    for (int i = 0; i < (int) s.size(); ++i)
    {
        const float t = (float) i / kFs;
        const int c = std::min (2, (int) (t / len));
        const float local = t - (float) c * len;
        const float fadeIn = c == 0 ? std::min (1.0f, t / 0.3f) : 1.0f;
        const float xf = std::min (1.0f, local / 0.15f); // chords blend into each other
        float v = 0.0f;
        for (int n = 0; n < 3; ++n)
        {
            v += xf * std::sin (kTwoPi * chords[c][n] * t);
            if (c > 0) v += (1.0f - xf) * std::sin (kTwoPi * chords[c - 1][n] * t);
        }
        s[(size_t) i] = 0.12f * fadeIn * v;
    }
    return s;
}

// a steady sine, faded in so its own onset can't read as a click
static Sig tone (float seconds, float f, float amp)
{
    Sig s ((size_t) (seconds * kFs), 0.0f);
    for (int i = 0; i < (int) s.size(); ++i)
    {
        const float t = (float) i / kFs;
        s[(size_t) i] = amp * raisedCos (t / 0.05f) * std::sin (kTwoPi * f * t);
    }
    return s;
}

static void writeWav (const std::string& path, const Sig& l, const Sig& r)
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
        int16_t a = (int16_t) (clampf (l[i], -1, 1) * 32767.0f), b = (int16_t) (clampf (r[i], -1, 1) * 32767.0f);
        std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f);
    }
    std::fclose (f);
}

// ------------------------------------------------------------------------------------
// Rendering
// ------------------------------------------------------------------------------------
struct Out { Sig l, r; };
using Script = std::function<void (float t, EngineParams&, JetEngine&)>;

static EngineParams quietBase()
{
    EngineParams p;
    p.dry = 0.0f;       // effect only, so silence means the effect did nothing
    p.air = 0.0f;
    p.dimension = 0.15f;
    p.lfoDepth = 0.0f;
    p.envTime = 0.62f;
    return p;
}

static Out render (const char* name, EngineParams p, const Sig& in, const Script& script = nullptr)
{
    static JetEngine eng; // large: keep it off the stack
    eng.prepare (kFs);
    eng.clearMemory();
    Out o { in, in };
    const int block = 128;
    for (int s = 0; s < (int) in.size(); s += block)
    {
        const int len = std::min (block, (int) in.size() - s);
        if (script) script ((float) s / kFs, p, eng);
        eng.process (o.l.data() + s, o.r.data() + s, len, p);
    }
    if (! wavDir.empty()) writeWav (wavDir + "/" + name + ".wav", o.l, o.r);
    return o;
}

static float rms (const Sig& s, float from, float to)
{
    const int a = (int) (from * kFs), b = std::min ((int) s.size(), (int) (to * kFs));
    double e = 0.0;
    for (int i = a; i < b; ++i) e += (double) s[(size_t) i] * s[(size_t) i];
    return (float) std::sqrt (e / std::max (1, b - a));
}
static float db (float x) { return 20.0f * std::log10 (x + 1.0e-12f); }
static float peak (const Out& o)
{
    float pk = 0.0f;
    for (size_t i = 0; i < o.l.size(); ++i) pk = std::max (pk, std::max (std::fabs (o.l[i]), std::fabs (o.r[i])));
    return pk;
}
static bool finite (const Out& o)
{
    for (size_t i = 0; i < o.l.size(); ++i) if (! std::isfinite (o.l[i]) || ! std::isfinite (o.r[i])) return false;
    return true;
}

// ------------------------------------------------------------------------------------
// Spectrum analysis
// ------------------------------------------------------------------------------------
struct Spectrum
{
    std::vector<float> mag;
    float binHz = 1.0f;
};

static Spectrum spectrum (const Sig& s, float from, float to)
{
    const int a = (int) (from * kFs), len = (int) ((to - from) * kFs);
    const int n = nextPow2 (len) * 4; // zero padding for fine frequency reading
    Fft fft; fft.init (n);
    std::vector<std::complex<float>> buf ((size_t) n);
    for (int i = 0; i < len; ++i)
    {
        const float w = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) len);
        buf[(size_t) i] = { s[(size_t) (a + i)] * w, 0.0f };
    }
    fft.run (buf.data(), false);
    Spectrum sp;
    sp.binHz = kFs / (float) n;
    sp.mag.resize ((size_t) n / 2);
    for (int k = 0; k < n / 2; ++k) sp.mag[(size_t) k] = std::abs (buf[(size_t) k]);
    return sp;
}

// strongest peak between lo and hi Hz, read to a fraction of a bin
static float peakHz (const Spectrum& sp, float lo, float hi, float* level = nullptr)
{
    const int a = std::max (1, (int) (lo / sp.binHz)), b = std::min ((int) sp.mag.size() - 2, (int) (hi / sp.binHz));
    int best = a;
    for (int k = a; k <= b; ++k) if (sp.mag[(size_t) k] > sp.mag[(size_t) best]) best = k;
    const float y0 = std::log (sp.mag[(size_t) best - 1] + 1e-12f), y1 = std::log (sp.mag[(size_t) best] + 1e-12f), y2 = std::log (sp.mag[(size_t) best + 1] + 1e-12f);
    const float d = 0.5f * (y0 - y2) / (y0 - 2.0f * y1 + y2);
    if (level) *level = sp.mag[(size_t) best];
    return ((float) best + clampf (d, -0.5f, 0.5f)) * sp.binHz;
}
static float cents (float f, float ref) { return 1200.0f * std::log2 (f / ref); }

// energy near the harmonics of f0 against everything else
static float purityDb (const Spectrum& sp, float f0)
{
    double on = 0.0, all = 0.0;
    for (size_t k = 1; k < sp.mag.size(); ++k)
    {
        const float hz = (float) k * sp.binHz;
        if (hz < 40.0f || hz > 12000.0f) continue;
        const double e = (double) sp.mag[k] * sp.mag[k];
        all += e;
        const float h = std::round (hz / f0);
        if (h >= 0.5f && std::fabs (hz - h * f0) < std::max (4.0f, 0.012f * h * f0)) on += e;
    }
    return 10.0f * (float) std::log10 (on / std::max (1e-30, all - on));
}

// level movement over windows of whole cycles
static float wobbleDb (const Sig& s, float from, float to, float f0, int cycles = 10)
{
    const int win = (int) std::round ((float) cycles * kFs / f0);
    float lo = 1e9f, hi = -1e9f;
    for (int a = (int) (from * kFs); a + win <= (int) (to * kFs); a += win / 2)
    {
        double e = 0.0;
        for (int i = 0; i < win; ++i) e += (double) s[(size_t) (a + i)] * s[(size_t) (a + i)];
        const float d = 10.0f * (float) std::log10 (e / win + 1e-30);
        lo = std::min (lo, d); hi = std::max (hi, d);
    }
    return hi - lo;
}

// largest second difference against the test tone's own: amplitude * (2 pi f / fs)^2
static float clickScore (const Sig& s, float from, float to, float f, float amp)
{
    const float ref = amp * std::pow (kTwoPi * f / kFs, 2.0f);
    float worst = 0.0f;
    for (int i = std::max (2, (int) (from * kFs)); i < std::min ((int) s.size(), (int) (to * kFs)); ++i)
        worst = std::max (worst, std::fabs (s[(size_t) i] - 2.0f * s[(size_t) i - 1] + s[(size_t) i - 2]));
    return worst / std::max (1e-9f, ref);
}

// ------------------------------------------------------------------------------------
int main (int argc, char** argv)
{
    if (argc > 1) wavDir = argv[1];
    static const char* names[kNumVoices] = { "blur-0", "blur-down", "blur-up", "blur-both", "synth-a", "synth-b", "glitch-a", "glitch-b", "swell-a", "swell-b" };

    // ---- Blur holds a note: pitch, purity, steadiness ----
    std::printf ("Held notes (Blur, Infinite)\n");
    struct Held { const char* name; float f; int input; float measureHz; };
    const Held held[] = { { "guitar-a3", 220.0f, InputGuitar, 220.0f }, { "guitar-low-e", 82.41f, InputGuitar, 82.41f },
                          { "guitar-high-e", 659.3f, InputGuitar, 659.3f }, { "bass-low-e", 41.2f, InputBass, 164.8f } };
    for (const auto& h : held)
    {
        auto p = quietBase();
        p.voice = VBlur0;
        p.envTime = 0.92f; // Infinite
        p.inputMode = h.input;
        p.envShape = 0.6f; // swell
        Sig in ((size_t) (5.0f * kFs), 0.0f);
        addPluck (in, 0.2f, h.f, 0.5f, 2.5f, 5, 1.2f);
        auto o = render ((std::string ("held-") + h.name).c_str(), p, in);
        // after the pluck has died away, only the frozen note is left (bass: read the 4th harmonic,
        // because Bass mode keeps the fundamental dry)
        const auto sp = spectrum (o.l, 2.0f, 4.5f);
        const float f = peakHz (sp, h.measureHz * 0.94f, h.measureHz * 1.06f);
        const float c = cents (f, h.measureHz);
        const float pure = purityDb (sp, h.input == InputBass ? 41.2f : h.f);
        const float wob = wobbleDb (o.l, 2.0f, 4.5f, h.measureHz, 20);
        std::printf ("  %-16s %+6.2f cents  purity %5.1f dB  wobble %4.2f dB  level %6.1f dB\n", h.name, c, pure, wob, db (rms (o.l, 2.0f, 4.5f)));
        expect (std::fabs (c) <= 3.0f, h.name, "frozen note more than 3 cents out");
        expect (pure >= 18.0f, h.name, "frozen note under 18 dB pure");
        expect (wob <= 3.0f, h.name, "frozen note level wobbles more than 3 dB");
        expect (rms (o.l, 2.0f, 4.5f) > 0.01f, h.name, "frozen note too quiet");
    }

    // ---- Blur +1 and -1 add their octaves in tune ----
    for (int v : { (int) VBlurUp, (int) VBlurDown })
    {
        auto p = quietBase();
        p.voice = v; p.envTime = 0.92f;
        Sig in ((size_t) (5.0f * kFs), 0.0f);
        addPluck (in, 0.2f, 220.0f, 0.5f, 2.5f, 3, 1.2f);
        auto o = render (names[v], p, in);
        const auto sp = spectrum (o.l, 2.0f, 4.5f);
        const float target = v == VBlurUp ? 440.0f : 110.0f;
        float lvlOct = 0.0f, lvlBase = 0.0f;
        const float f = peakHz (sp, target * 0.97f, target * 1.03f, &lvlOct);
        peakHz (sp, 215.0f, 225.0f, &lvlBase);
        const float rel = db (lvlOct / lvlBase);
        std::printf ("  %-16s octave %+6.2f cents, %5.1f dB against the note\n", names[v], cents (f, target), rel);
        expect (std::fabs (cents (f, target)) <= 3.0f, names[v], "octave layer out of tune");
        expect (rel > -12.0f, names[v], "octave layer too quiet");
    }

    // ---- Synth plays the caught note, or the MIDI note exactly ----
    std::printf ("Synth pitch\n");
    {
        auto p = quietBase();
        p.voice = VSynthA; p.envTime = 0.92f; p.dimension = 0.9f;
        Sig in ((size_t) (4.0f * kFs), 0.0f);
        addPluck (in, 0.2f, 196.0f, 0.5f, 2.5f, 4, 1.0f);
        auto o = render ("synth-from-audio", p, in);
        const float f = peakHz (spectrum (o.l, 1.5f, 3.5f), 180.0f, 212.0f);
        std::printf ("  from audio       %+6.2f cents\n", cents (f, 196.0f));
        expect (std::fabs (cents (f, 196.0f)) <= 5.0f, "synth-from-audio", "Synth note more than 5 cents out");

        Sig in2 ((size_t) (4.0f * kFs), 0.0f);
        addPluck (in2, 0.2f, 233.0f, 0.5f, 2.5f, 4, 1.0f); // played sharp; MIDI says A3
        auto o2 = render ("synth-from-midi", p, in2, [] (float t, EngineParams&, JetEngine& e) { if (t >= 0.2f && t < 0.2f + 128.0f / kFs) e.noteOn (57); });
        const float f2 = peakHz (spectrum (o2.l, 1.5f, 3.5f), 200.0f, 245.0f);
        std::printf ("  from MIDI        %+6.2f cents\n", cents (f2, 220.0f));
        expect (std::fabs (cents (f2, 220.0f)) <= 1.0f, "synth-from-midi", "MIDI note not exact");
    }

    // ---- every voice answers soft, pad and late playing ----
    std::printf ("Level against the input (wet only, dB)\n             pluck   quiet     pad    late\n");
    for (int v = 0; v < kNumVoices; ++v)
    {
        float rel[4];
        for (int kind = 0; kind < 4; ++kind)
        {
            auto p = quietBase();
            p.voice = v;
            p.envTime = 0.75f;
            Sig in;
            float from = 0.3f, to = 5.0f;
            switch (kind)
            {
                case 0: in = phrase (5.5f); break;
                case 1: in = phrase (5.5f, 220.0f, 0.0316f); break;
                case 2: in = pad (5.0f); p.inputMode = InputSynth; from = 0.5f; to = 4.8f; break;
                default: in = phrase (7.5f, 220.0f, 1.0f, 2.5f); from = 2.6f; to = 7.3f; break;
            }
            const float inRms = rms (in, from, to);
            auto o = render ((std::string (names[v]) + "-" + std::to_string (kind)).c_str(), p, in);
            rel[kind] = db (rms (o.l, from, to) / inRms);
            expect (finite (o), names[v], "NaN in output");
            expect (peak (o) <= 0.892f, names[v], "output over -1 dBFS");
        }
        std::printf ("  %-10s %6.1f  %6.1f  %6.1f  %6.1f\n", names[v], rel[0], rel[1], rel[2], rel[3]);
        static const char* kinds[4] = { "plucked input lost", "quiet input lost", "pad input lost", "late start lost" };
        for (int k = 0; k < 4; ++k) expect (rel[k] > -20.0f, names[v], kinds[k]);
        expect (rel[0] < 8.0f, names[v], "much louder than the input");
    }

    // ---- clicks ----
    std::printf ("Clicks (worst curvature against the test tone's own, pass under the mark)\n");
    {
        const float f = 220.0f, a = 0.3f;
        auto report = [&] (const char* name, const Out& o, float from, float to, float mark)
        {
            const float sc = std::max (clickScore (o.l, from, to, f, a), clickScore (o.r, from, to, f, a));
            std::printf ("  %-22s %5.2f (mark %.0f)\n", name, sc, mark);
            expect (sc < mark, name, "click");
        };
        {
            auto p = quietBase(); p.voice = VBlur0; p.envTime = 0.92f; p.dry = 0.5f;
            auto o = render ("click-bypass", p, tone (5.0f, f, a), [] (float t, EngineParams& q, JetEngine&) { q.bypass = (t > 2.0f && t < 3.0f) || t > 4.0f; });
            report ("bypass on and off", o, 1.0f, 5.0f, 4.0f);
        }
        {
            auto p = quietBase(); p.voice = VBlur0; p.envTime = 0.92f; p.dry = 0.5f;
            auto o = render ("click-voices", p, tone (6.0f, f, a), [] (float t, EngineParams& q, JetEngine&)
            {
                q.voice = t < 1.5f ? VBlur0 : t < 2.5f ? VBlurUp : t < 3.5f ? VSwellA : t < 4.5f ? VGlitchA : VBlurDown;
            });
            report ("voice switches", o, 1.0f, 6.0f, 8.0f);
        }
        {
            Sig in ((size_t) (5.0f * kFs), 0.0f);
            for (int k = 0; k < 8; ++k) addPluck (in, 0.2f + 0.55f * (float) k, f, a, 1.0f, 1, 1.5f);
            for (int trig = 0; trig < 2; ++trig)
            {
                auto p = quietBase(); p.voice = VBlur0; p.envTime = 0.92f; p.trigger = trig;
                auto o = render (trig == 0 ? "click-poly" : "click-mono", p, in);
                report (trig == 0 ? "retrigger, Poly" : "hand-off, Mono", o, 0.3f, 5.0f, 6.0f);
            }
        }
        {
            // one capture by footswitch in Manual, so only one loop plays
            auto p = quietBase(); p.voice = VGlitchA; p.envTime = 0.92f; p.dimension = 0.6f; p.trigger = 2;
            Sig in = tone (4.0f, f, a);
            bool pressed = false;
            auto o = render ("click-glitch", p, in, [&] (float t, EngineParams&, JetEngine& e) { if (t >= 1.0f && ! pressed) { pressed = true; e.footswitch (0, true); } });
            report ("Glitch A seams", o, 1.0f, 4.0f, 4.0f);
            const float wob = wobbleDb (o.l, 2.4f, 3.9f, f, 10);
            std::printf ("  Glitch A seam wobble   %4.2f dB (mark 1)\n", wob);
            expect (wob < 1.0f, "glitch-seam", "loop level wobbles over 1 dB");
        }
    }

    // ---- output stage ----
    std::printf ("Output\n");
    {
        auto p = quietBase(); p.voice = VBlurBoth; p.envTime = 0.92f; p.drive = 1.0f; p.autoLevel = false; p.dry = 0.6f;
        p.dimension = 1.0f; p.air = 1.0f; p.masterDb = 12.0f;
        Sig in = phrase (6.0f, 220.0f, 2.0f);
        auto o = render ("hot", p, in);
        std::printf ("  hottest peak       %6.2f dBFS (mark -1)\n", db (peak (o)));
        expect (peak (o) <= 0.892f, "hot", "output over -1 dBFS");
        expect (finite (o), "hot", "NaN");
    }
    {
        auto p = quietBase(); p.voice = VSwellA; p.dimension = 1.0f; p.air = 1.0f; p.dry = 0.5f;
        auto o = render ("silence", p, Sig ((size_t) (3.0f * kFs), 0.0f));
        std::printf ("  silence            %6.1f dBFS\n", db (peak (o)));
        expect (peak (o) < 1.0e-5f, "silence", "output when nothing is played");
    }
    {
        // Bass mode: the lows pass at unity even with the effect fully wet
        auto p = quietBase(); p.inputMode = InputBass; p.voice = VSynthA;
        Sig in = tone (3.0f, 45.0f, 0.4f);
        auto o = render ("bass-lows", p, in);
        const float diff = db (rms (o.l, 1.0f, 2.8f) / rms (in, 1.0f, 2.8f));
        std::printf ("  bass lows          %+6.2f dB (mark 0.5)\n", diff);
        expect (std::fabs (diff) < 0.5f, "bass-lows", "Bass mode doesn't keep the lows at unity");
    }
    {
        // Drive at full on a 5 kHz tone: everything that isn't a harmonic is aliasing
        auto p = quietBase(); p.voice = VBlur0; p.drive = 1.0f; p.dry = 0.5f; p.tone = 1.0f; p.bypass = true;
        p.bypass = false; p.trigger = 2; // Manual and untouched: dry only, through the drive
        auto o = render ("drive-alias", p, tone (2.0f, 5000.0f, 0.5f));
        const auto sp = spectrum (o.l, 0.5f, 1.8f);
        double harm = 0, other = 0;
        for (size_t k = 1; k < sp.mag.size(); ++k)
        {
            const float hz = (float) k * sp.binHz;
            if (hz < 30.0f || hz > 20000.0f) continue; // the audible band
            const double e = (double) sp.mag[k] * sp.mag[k];
            const float h = std::round (hz / 5000.0f);
            if (h >= 1.0f && std::fabs (hz - h * 5000.0f) < 30.0f) harm += e; else other += e;
        }
        const float alias = 10.0f * (float) std::log10 (other / harm);
        std::printf ("  drive aliasing     %6.1f dB (mark -40)\n", alias);
        expect (alias < -40.0f, "drive-alias", "drive aliasing too loud");
    }

    // ---- Glitch B memory, Hold and Learn ----
    std::printf ("Memory and Learn\n");
    {
        auto p = quietBase(); p.voice = VGlitchB; p.envTime = 0.97f;
        int filledBeforeHold = 0, filledEnd = 0, writesAfterHold = 0;
        Sig in = phrase (9.0f);
        for (int k = 0; k < 6; ++k) addPluck (in, 5.5f + 0.5f * (float) k, 330.0f, 0.4f);
        auto o = render ("glitch-b-memory", p, in, [&] (float t, EngineParams& q, JetEngine& e)
        {
            q.hold = t > 5.0f;
            if (t > 5.0f) q.dimension = std::fmod (t * 0.5f, 1.0f);
            int filled = 0;
            for (int b = 0; b < JetEngine::kBlocks; ++b) filled += e.uiFilled[b].load() ? 1 : 0;
            if (t < 5.0f) filledBeforeHold = filled; else filledEnd = filled;
            if (t > 5.2f && e.uiWriting.load() >= 0) ++writesAfterHold;
        });
        std::printf ("  cells filled %d, writes while held %d\n", filledBeforeHold, writesAfterHold);
        expect (filledBeforeHold >= 3, "glitch-b", "memory did not fill");
        expect (writesAfterHold == 0 && filledEnd == filledBeforeHold, "glitch-b", "Hold did not lock the memory");
        expect (rms (o.l, 5.5f, 8.5f) > 0.005f, "glitch-b", "held memory silent");
    }
    {
        float loud = -1.0f, soft = -1.0f;
        // even, short notes with gaps between them; then the same with one ghost note 15 dB down
        auto even = [] ()
        {
            Sig s ((size_t) (5.0f * kFs), 0.0f);
            const float at[5] = { 0.2f, 0.8f, 1.4f, 2.0f, 3.7f }; // a long gap before the last
            for (int k = 0; k < 5; ++k) addPluck (s, at[k], 220.0f * (k % 2 ? 1.26f : 1.0f), 0.45f, 9.0f, 3, 0.5f);
            return s;
        };
        render ("learn-loud", quietBase(), even(), [&] (float t, EngineParams&, JetEngine& e) { if (t < 0.01f) e.startLearn(); loud = e.uiLearnResult.load(); });
        Sig mixed = even();
        addPluck (mixed, 3.2f, 300.0f, 0.08f, 9.0f, 3, 0.5f); // one ghost note in a gap
        render ("learn-ghost", quietBase(), mixed, [&] (float t, EngineParams&, JetEngine& e) { if (t < 0.01f) e.startLearn(); soft = e.uiLearnResult.load(); });
        std::printf ("  learned sensitivity: even playing %.2f, with a ghost note %.2f\n", loud, soft);
        expect (loud >= 0.0f && loud <= 1.0f, "learn", "Learn did not settle");
        expect (soft > loud, "learn", "Learn didn't open up for a soft note");
    }

    // ---- factory presets: each makes sound from the right input, wet only, and stays clean ----
    std::printf ("Factory presets (wet level against the input, peak)\n");
    {
        const auto& presets = factoryPresets();
        expect (presets.size() == 20, "presets", "expected 20 factory presets");
        for (size_t i = 0; i < presets.size(); ++i)
        {
            auto p = paramsFor (presets[i]);
            p.dry = 0.0f; // the effect alone
            Sig in;
            if (p.inputMode == InputBass) in = phrase (7.0f, 55.0f, 1.0f);
            else if (p.inputMode == InputSynth) in = pad (7.0f);
            else in = phrase (7.0f);
            if (p.voice == VGlitchB) // fill the memory first
                for (int k = 0; k < 4; ++k) addPluck (in, 4.4f + 0.6f * (float) k, 330.0f, 0.4f);
            bool pressed = false;
            const std::string nm = "preset-" + std::to_string (i + 1);
            auto o = render (nm.c_str(), p, in, [&] (float t, EngineParams&, JetEngine& e)
            {
                if (p.trigger == 2 && t >= 2.05f && ! pressed) { pressed = true; e.footswitch (0, true); } // Manual: press A on a note
            });
            const float lvl = db (rms (o.l, 1.0f, 6.8f) / std::max (1.0e-9f, rms (in, 1.0f, 6.8f)));
            std::printf ("  %2zu %-18s %6.1f dB  %6.1f dBFS\n", i + 1, presets[i].name.c_str(), lvl, db (peak (o)));
            expect (finite (o), presets[i].name.c_str(), "NaN");
            expect (lvl > -24.0f, presets[i].name.c_str(), "too quiet");
            expect (peak (o) <= 0.892f, presets[i].name.c_str(), "over -1 dBFS");
            expect (presets[i].hint.size() < 80, presets[i].name.c_str(), "hint too long for the panel");
        }
    }

    std::printf (failures == 0 ? "All checks passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

// CPU per instance at 48 kHz, for each release. Renders 20 s of a busy guitar part through
// the heaviest settings of each voice and reports the share of one core it needs.
#include "../Source/engine/ParamSpec.h"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <vector>

using namespace jet;

int main()
{
    const double sr = 48000.0;
    struct Case { const char* name; EngineParams p; };
    std::vector<Case> cases;
    auto add = [&] (const char* name, const char* preset, auto tweak)
    {
        for (auto& pr : factoryPresets())
            if (pr.name == preset) { auto p = paramsFor (pr); tweak (p); cases.push_back ({ name, p }); }
    };
    add ("Blur +/-1, Poly, long swells", "OCTAVE CATHEDRAL", [] (EngineParams&) {});
    add ("Blur 0, Infinite", "ENDLESS PAD", [] (EngineParams&) {});
    add ("Synth B, Poly, envelope filter", "ENVELOPE FILTER", [] (EngineParams&) {});
    add ("Glitch A, random LFO", "GLITCH RAIN", [] (EngineParams&) {});
    add ("Glitch B, scanning memory", "SELF REMIX", [] (EngineParams&) {});
    add ("Swell B, fuzz and drive", "VIOLIN FUZZ", [] (EngineParams& p) { p.drive = 0.7f; });
    add ("Bass, Synth A", "BASS SYNTH", [] (EngineParams&) {});

    std::printf ("%-36s %10s %10s\n", "Setting", "64 smp", "128 smp");
    static JetEngine e; // large: keep it off the stack
    for (auto& c : cases)
    {
        std::printf ("%-36s", c.name);
        for (int block : { 64, 128 })
        {
            e.prepare (sr);
            std::vector<float> l ((size_t) block), r ((size_t) block);
            const long total = (long) (20.0 * sr);
            double busy = 0;
            uint32_t seed = 1;
            for (long done = 0; done < total; done += block)
            {
                for (int i = 0; i < block; ++i)
                {
                    // a new plucked chord every 0.4 s, so the samplers are always catching
                    const long n = done + i;
                    const double t = (double) (n % (long) (0.4 * sr)) / sr;
                    const double f = 196.0 * std::pow (2.0, (double) ((n / (long) (0.4 * sr)) % 5) / 12.0);
                    seed = seed * 1664525u + 1013904223u;
                    const float v = (float) (0.35 * std::exp (-3.0 * t) * (std::sin (6.2831853 * f * n / sr) + 0.5 * std::sin (6.2831853 * 1.5 * f * n / sr)))
                                  + 1.0e-4f * ((float) (seed >> 9) / 8388608.0f - 1.0f);
                    l[(size_t) i] = r[(size_t) i] = v;
                }
                const auto a = std::chrono::steady_clock::now();
                e.process (l.data(), r.data(), block, c.p);
                busy += std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
            }
            std::printf (" %9.2f%%", 100.0 * busy / 20.0);
        }
        std::printf ("\n");
    }
    return 0;
}

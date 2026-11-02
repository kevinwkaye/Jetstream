#pragma once

#include "DspUtil.h"

// Spots the attack of each new note or chord, the way the pedal's calibrated
// dynamics tracking does. A fast envelope racing ahead of a slow one is an attack.
namespace jet
{
enum InputMode { InputGuitar = 0, InputSynth, InputBass };

class Detector
{
public:
    void prepare (float sampleRate)
    {
        fs = sampleRate;
        setMode (InputGuitar);
        reset();
    }

    void reset()
    {
        fast = slow = 0.0f;
        hp.reset(); lp.reset();
        sinceOnset = 1 << 30;
        armed = true;
    }

    void setMode (int m)
    {
        if (m == mode && hpCut > 0.0f) return;
        mode = m;
        switch (mode)
        {
            case InputSynth: hpCut = 50.0f;  lpCut = 9000.0f; fastRel = 0.030f; slowAtt = 0.060f; ratio = 1.55f; lockMs = 110.0f; break;
            case InputBass:  hpCut = 30.0f;  lpCut = 900.0f;  fastRel = 0.030f; slowAtt = 0.050f; ratio = 1.8f;  lockMs = 95.0f;  break;
            default:         hpCut = 70.0f;  lpCut = 7000.0f; fastRel = 0.015f; slowAtt = 0.035f; ratio = 1.9f;  lockMs = 75.0f;  break;
        }
        hp.setCutoff (hpCut, fs);
        lp.setCutoff (lpCut, fs);
        fastAtt = std::exp (-1.0f / (0.0006f * fs));
        fastRelC = std::exp (-1.0f / (fastRel * fs));
        slowAttC = std::exp (-1.0f / (slowAtt * fs));
        slowRelC = std::exp (-1.0f / (0.35f * fs));
    }

    // sensitivity 0..1 -> threshold -16 dB (deaf) .. -66 dB (very sensitive)
    void setSensitivity (float s) { threshold = dbToGain (-16.0f - 50.0f * clampf (s, 0.0f, 1.0f)); }

    // Returns > 0 (the attack's strength, 0..1) on the sample an attack is detected
    float process (float x)
    {
        const float lowCut = x - hp.process (x);
        const float band = lp.process (lowCut);
        const float a = std::fabs (band);

        fast = a > fast ? fastAtt * fast + (1.0f - fastAtt) * a : fastRelC * fast + (1.0f - fastRelC) * a;
        slow = fast > slow ? slowAttC * slow + (1.0f - slowAttC) * fast : slowRelC * slow + (1.0f - slowRelC) * fast;

        ++sinceOnset;
        const int lockSamples = (int) (lockMs * 0.001f * fs);
        if (! armed && sinceOnset > lockSamples && fast < slow * 1.15f)
            armed = true;

        if (armed && fast > threshold && fast > slow * ratio + 1.0e-5f)
        {
            armed = false;
            sinceOnset = 0;
            return clampf (fast / (threshold * 8.0f), 0.1f, 1.0f);
        }
        return 0.0f;
    }

    float level() const { return fast; }

private:
    float fs = 48000.0f;
    int mode = -1;
    float hpCut = 0.0f, lpCut = 8000.0f, fastRel = 0.015f, slowAtt = 0.035f, ratio = 1.9f, lockMs = 75.0f;
    float fastAtt = 0, fastRelC = 0, slowAttC = 0, slowRelC = 0;
    float fast = 0, slow = 0, threshold = 0.016f;
    OnePole hp, lp;
    int sinceOnset = 1 << 30;
    bool armed = true;
};
} // namespace jet

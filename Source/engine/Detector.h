#pragma once

#include "DspUtil.h"

// Spots the attack of each new note or chord, like the pedal's calibrated dynamics
// tracking. A fast envelope racing ahead of a slow one is an attack. Whether it's loud
// enough is judged against how loud you've been playing, never a fixed level, so soft
// passages and quiet guitars still trigger (see the DSP lessons from Speckle).
namespace jet
{
enum InputMode { InputGuitar = 0, InputSynth, InputBass };

class Detector
{
public:
    void prepare (float sampleRate)
    {
        fs = sampleRate;
        mode = -1;
        setMode (InputGuitar);
        reset();
    }

    // Detectors forget everything on prepare, or a loud passage before a render
    // would deafen them afterwards (Backtrack lesson).
    void reset()
    {
        fast = slow = play = 0.0f;
        hp.reset(); lp.reset();
        sinceOnset = 1 << 30;
        armed = true;
        quietFor = 0;
    }

    void setMode (int m)
    {
        if (m == mode) return;
        mode = m;
        switch (mode)
        {
            case InputSynth: hpCut = 50.0f; lpCut = 9000.0f; fastRel = 0.030f; slowAtt = 0.060f; ratio = 1.55f; lockMs = 110.0f; break;
            case InputBass:  hpCut = 30.0f; lpCut = 900.0f;  fastRel = 0.030f; slowAtt = 0.050f; ratio = 1.8f;  lockMs = 95.0f;  break;
            default:         hpCut = 70.0f; lpCut = 7000.0f; fastRel = 0.015f; slowAtt = 0.035f; ratio = 1.85f; lockMs = 75.0f;  break;
        }
        hp.setCutoff (hpCut, fs);
        lp.setCutoff (lpCut, fs);
        fastAtt = std::exp (-1.0f / (0.0006f * fs));
        fastRelC = std::exp (-1.0f / (fastRel * fs));
        slowAttC = std::exp (-1.0f / (slowAtt * fs));
        slowRelC = std::exp (-1.0f / (0.35f * fs));
        playAtt = std::exp (-1.0f / (0.05f * fs));
        playRel = std::exp (-1.0f / (6.0f * fs));
    }

    // Sensitivity 0..1. The gate sits this far under your playing level:
    // 0 = 9 dB (only strong attacks), 0.6 = 23 dB, 1 = 33 dB (every ghost note).
    void setSensitivity (float s)
    {
        s = clampf (s, 0.0f, 1.0f);
        gateRatio = dbToGain (-9.0f - 24.0f * s);
        floorLevel = dbToGain (-56.0f - 36.0f * s); // -56 .. -92 dBFS: below this nothing is a note
    }

    // Returns > 0 (the attack's strength, 0..1) on the sample an attack is detected
    float process (float x)
    {
        const float band = lp.process (x - hp.process (x));
        const float a = std::fabs (band);

        fast = a > fast ? fastAtt * fast + (1.0f - fastAtt) * a : fastRelC * fast + (1.0f - fastRelC) * a;
        slow = fast > slow ? slowAttC * slow + (1.0f - slowAttC) * fast : slowRelC * slow + (1.0f - slowRelC) * fast;

        // playing level: rises over 50 ms, falls over 6 s, and only follows real playing
        if (fast > floorLevel)
            play = fast > play ? playAtt * play + (1.0f - playAtt) * fast : playRel * play + (1.0f - playRel) * fast;
        quietFor = fast < std::max (floorLevel, play * 0.07f) ? quietFor + 1 : 0;

        ++sinceOnset;
        const int lockSamples = (int) (lockMs * 0.001f * fs);
        if (! armed && sinceOnset > lockSamples && fast < slow * 1.15f)
            armed = true;

        const float gate = std::max (floorLevel, play * gateRatio);
        if (armed && fast > gate && fast > slow * ratio + 1.0e-6f)
        {
            armed = false;
            sinceOnset = 0;
            if (play < fast * 0.5f) play = fast * 0.5f; // a first note teaches the level straight away
            return clampf (fast / std::max (1.0e-6f, play * 2.0f), 0.1f, 1.0f);
        }
        return 0.0f;
    }

    float level() const { return fast; }
    float playingLevel() const { return std::max (play, floorLevel); }
    bool sounding() const { return quietFor < (int) (0.05f * fs); }

private:
    float fs = 48000.0f;
    int mode = -1;
    float hpCut = 70.0f, lpCut = 7000.0f, fastRel = 0.015f, slowAtt = 0.035f, ratio = 1.85f, lockMs = 75.0f;
    float fastAtt = 0, fastRelC = 0, slowAttC = 0, slowRelC = 0, playAtt = 0, playRel = 0;
    float fast = 0, slow = 0, play = 0;
    float gateRatio = 0.07f, floorLevel = 1.0e-4f;
    OnePole hp, lp;
    int sinceOnset = 1 << 30, quietFor = 0;
    bool armed = true;
};
} // namespace jet

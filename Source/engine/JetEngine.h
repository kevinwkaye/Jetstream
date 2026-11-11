#pragma once

#include "Detector.h"
#include "Envelope.h"
#include "Voices.h"
#include <array>

// The whole Jetstream signal path, free of JUCE:
//
//   input -> note detection -> samplers A and B -> voice -> leveller -> stereo space
//         -> + dry -> drive and tone (2x oversampled, like the pedal's analogue output stage)
//         -> master -> soft ceiling
//
// Blur rebuilds each caught note as pure, phase-continuous sine partials (exact pitch, no
// grain), with octave layers that fade in behind it. Synth picks the notes out of those
// partials. Glitch loops close on matched audio. In Bass mode everything under 120 Hz
// bypasses the lot.
namespace jet
{
enum VoiceId { VBlur0 = 0, VBlurDown, VBlurUp, VBlurBoth, VSynthA, VSynthB, VGlitchA, VGlitchB, VSwellA, VSwellB, kNumVoices };
enum Group { GroupBlur = 0, GroupSynth, GroupGlitchA, GroupGlitchB, GroupSwell };

inline int groupOf (int v)
{
    if (v <= VBlurBoth) return GroupBlur;
    if (v <= VSynthB) return GroupSynth;
    if (v == VGlitchA) return GroupGlitchA;
    if (v == VGlitchB) return GroupGlitchB;
    return GroupSwell;
}

struct EngineParams
{
    int inputMode = InputGuitar;
    float sensitivity = 0.6f;
    int trigger = 0;       // 0 Poly, 1 Mono, 2 Manual
    int fsMode = 0;        // 0 Toggle, 1 Latch, 2 Momentary
    float envShape = 0.6f; // 0..1 across the six shapes
    float envTime = 0.5f;  // 0..1, top end holds Infinite then Repeat
    float repeatRateHz = -1.0f; // tempo-synced repeat rate, or -1 to use the shape knob
    int voice = VBlur0;
    float dimension = 0.5f;
    float dimOverride = -1.0f;   // recorded knob movement playing back
    float lfoDepth = 0.0f, lfoRateHz = 0.5f;
    int lfoShape = ShapeSwell;
    int dimSource = 0;     // 0 LFO, 1 envelope, 2 off
    float drive = 0.0f, driveOverride = -1.0f;
    float tone = 1.0f;
    float dry = 0.5f;
    float masterDb = 0.0f;
    bool autoLevel = true;
    float width = 0.8f;
    float air = 0.35f;
    bool hold = false;
    bool bypass = false;
    bool tails = true;
};

// Env Time knob zones
struct EnvTime
{
    enum Zone { Timed, Infinite, Repeat };
    static constexpr float kInfinite = 0.90f, kRepeat = 0.95f;
    static Zone zone (float t) { return t >= kRepeat ? Repeat : (t >= kInfinite ? Infinite : Timed); }
    static float seconds (float t) { return 0.05f * std::pow (400.0f, clampf (t / kInfinite, 0.0f, 1.0f)); } // 50 ms .. 20 s
};

struct EnvRequest
{
    Envelope::Mode mode = Envelope::Mode::OneShot;
    int shape = ShapeSwell;
    float duration = 1.0f, randomRate = 4.0f, repeatRate = 1.0f, phase = 0.0f;
};

class JetEngine
{
public:
    static constexpr int kBlocks = 6;
    static constexpr int kOverview = 8;
    static constexpr int kGlitchLengths = 4;
    static constexpr float kWetTrim = 0.8f;   // sustained sounds sit level with the dry signal
    static constexpr float kNoise = 0.35f;    // Blur's breath sits 9 dB under the note
    static constexpr float kPreRollSec = 0.012f;

    // ---- read by the panel ----
    std::atomic<float> uiInput { 0.0f }, uiEnvA { 0.0f }, uiEnvB { 0.0f }, uiDim { 0.5f }, uiWet { 0.0f };
    std::atomic<bool> uiFilled[kBlocks];
    std::atomic<float> uiOverview[kBlocks][kOverview];
    std::atomic<int> uiSelected { 0 }, uiWriting { -1 };
    std::atomic<int> uiCaptures { 0 };
    std::atomic<bool> uiLearning { false }, uiSustainA { false }, uiSustainB { false };
    std::atomic<float> uiLearnResult { -1.0f };
    std::atomic<float> uiNoteHz { 0.0f };

    JetEngine()
    {
        for (int b = 0; b < kBlocks; ++b)
        {
            uiFilled[b].store (false);
            for (int k = 0; k < kOverview; ++k) uiOverview[b][k].store (0.0f);
        }
    }

    static int latencySamples() { return Oversampler4x::kLatency; }

    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        const int base = fs > 60000.0f ? 8192 : 4096;
        anaNormal.prepare (fs, base, base, base / 8);
        anaBass.prepare (fs, base * 2, base, base / 8); // bass: twice the frequency detail
        hop = base / 8;

        hist.assign ((size_t) nextPow2 ((int) (1.5f * fs) + anaBass.span()), 0.0f);
        histMask = (int64_t) hist.size() - 1;
        clock = 0;

        detector.prepare (fs);
        lfo.prepare (fs);
        envGen.prepare (fs);
        fuzz.prepare (fs);
        rng.s = 0x2545F491u;

        preLen = (int) (kPreRollSec * fs);
        rawCap = preLen + (int) (0.5f * fs);
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = chans[c];
            ch.env.prepare (fs);
            ch.synth.prepare (fs, base);
            ch.osc.prepare (fs);
            ch.voice.reserve (base + 1);
            ch.oldVoice.reserve (base + 1);
            for (auto& r : ch.raw) r.assign ((size_t) rawCap + 8, 0.0f);
            ch.reset();
            ch.rng.s = 0x1F2E3D4Cu + (uint32_t) c * 0x9E3779B9u;
        }
        scratch.reserve (base + 1);

        memCap = rawCap;
        for (int b = 0; b < kBlocks; ++b)
        {
            if ((int) memory[b].size() != memCap + 8)
            {
                memory[b].assign ((size_t) memCap + 8, 0.0f);
                memLen[b] = 0;
                memPre[b] = 0;
                uiFilled[b].store (false);
            }
        }
        memPlay = memOld = LoopReader();
        memSel = -1;
        memPending = -1;

        // chord-change listener for pads (Afterglow lesson: linear magnitudes, 4096 points, every 80 ms)
        chgN = base;
        chgFft.init (chgN);
        chgWin.resize ((size_t) chgN);
        for (int i = 0; i < chgN; ++i) chgWin[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) chgN);
        chgBuf.assign ((size_t) chgN, {});
        chgNow.assign ((size_t) chgN / 2, 0.0f);
        chgPrev = chgRef = chgNow;
        chgEvery = (int) (0.08f * fs);
        chgCount = 0;
        havePrev = haveRef = false;

        const int maxDelay = nextPow2 ((int) (1.6f * fs));
        dl[0].init (maxDelay);
        dl[1].init (maxDelay);
        for (auto& f : dlDamp) f.reset();
        static const float fdnMs[4] = { 47.3f, 59.1f, 71.9f, 83.3f };
        for (int i = 0; i < 4; ++i)
        {
            fdn[i].init (nextPow2 ((int) (0.1f * fs)));
            fdnLen[i] = fdnMs[i] * 0.001f * fs;
            fdnDamp[i].reset();
            fdnDamp[i].setCutoff (6500.0f, fs);
        }
        static const float apMs[4] = { 5.3f, 7.9f, 6.1f, 8.7f };
        for (int i = 0; i < 4; ++i)
        {
            ap[i].init (nextPow2 ((int) (0.02f * fs)));
            apLen[i] = apMs[i] * 0.001f * fs;
        }

        for (auto* s : { &sDim, &sDrive, &sTone, &sDry, &sWet, &sMaster, &sWidth, &sAir, &sSend, &sDryGate })
            s->setTime (0.025f, fs);
        sDelay.setTime (0.12f, fs);
        sComp.setTime (0.05f, fs);
        sLevel.setTime (0.03f, fs);
        rmsAtt = std::exp (-1.0f / (0.08f * fs));
        rmsRel = std::exp (-1.0f / (0.4f * fs));
        rmsEnv = 0.0f;

        for (int c = 0; c < 2; ++c)
        {
            lowBand[c].set (false, 120.0f, fs);
            wetHp[c].set (true, 120.0f, fs);
            toneLp[c].reset();
            os[c].reset();
            adaaPrev[c] = 0.0;
            latL[c].init (64);
            latLow[c].init (64);
        }

        lastFoot[0] = lastFoot[1] = footDown[0] = footDown[1] = false;
        captureRequest[0] = captureRequest[1] = cancelRequest[0] = cancelRequest[1] = false;
        lastBypass = false;
        bypassMix = 0.0f;
        nextChannel = 0;
        appliedVoice = -1;
        dip = 1.0f;
        dipping = false;
        learning = false;
        externalNote = -1;
        driftPhase = 0.0f;
        dimNow = 0.5f;
    }

    // ---- control (audio thread unless noted) ----
    void noteOn (int midiNote) { externalNote = midiNote; }
    void startLearn() { learnRequest.store (true); }       // any thread
    void clearMemory() { clearRequest.store (true); }      // any thread

    // Footswitch A or B pressed or released. Manual mode captures; Poly and Mono sustain.
    void footswitch (int c, bool down) { footDown[c] = down; }

    // A quick capture of whatever is ringing now on channel c (any trigger mode)
    void captureNow (int c) { captureRequest[c] = true; }

    // Undo a capture that turned out to be half of the A+B combo
    void cancelCapture (int c) { cancelRequest[c] = true; }

    // Memory for saving with the set (message thread). Copies are racy but harmless.
    int memoryLength (int b) const { return memLen[b]; }
    int memoryPreRoll (int b) const { return memPre[b]; }
    const float* memoryData (int b) const { return memory[b].data(); }
    float sampleRate() const { return fs; }

    // Loads saved memory (message thread); the audio thread picks it up at the next block
    void loadMemory (const std::array<std::vector<float>, kBlocks>& blocks, const std::array<int, kBlocks>& preRolls, float savedRate)
    {
        if (loadReady.load()) return;
        for (int b = 0; b < kBlocks; ++b) { loadData[b] = blocks[b]; loadPre[b] = preRolls[b]; }
        loadRate = savedRate;
        loadReady.store (true);
    }

    bool channelActive (int c) const { return chans[c].env.isActive() || chans[c].waiting; }

    void process (float* L, float* R, int n, const EngineParams& p)
    {
        pollRequests();
        detector.setMode (p.inputMode);
        detector.setSensitivity (learning ? 1.0f : p.sensitivity);

        const auto shapeKnob = ShapeKnob::from (p.envShape);
        const auto zone = EnvTime::zone (p.envTime);
        const float envSecs = EnvTime::seconds (p.envTime);
        const float repeatRate = p.repeatRateHz > 0.0f ? p.repeatRateHz : shapeKnob.rateHz;
        const bool bass = p.inputMode == InputBass;
        const float dimKnob = p.dimOverride >= 0.0f ? p.dimOverride : p.dimension;
        const float driveKnob = p.driveOverride >= 0.0f ? p.driveOverride : p.drive;

        // Voice changes dip the wet signal for 40 ms and switch at the bottom, so nothing clicks
        if (appliedVoice < 0) { appliedVoice = p.voice; contentFor (p, bass); }
        if (p.voice != appliedVoice && ! dipping) dipping = true;

        const int group = groupOf (appliedVoice);
        const bool longDelay = appliedVoice == VBlurBoth;
        const bool sendDelay = group == GroupBlur || group == GroupSwell;

        // Dry: below 50% it fades up, above 75% the effect gives way to it (like the pedal)
        const float dryGain = clampf (p.dry * 2.0f, 0.0f, 1.0f);
        const float wetGain = p.dry > 0.75f ? clampf (1.0f - (p.dry - 0.75f) / 0.25f, 0.0f, 1.0f) : 1.0f;
        const bool glitchInterrupt = group == GroupGlitchB && p.dry < 0.15f;

        // Bypass: stop the samplers, let the tails ring out (or cut them)
        if (p.bypass && ! lastBypass)
            for (auto& ch : chans) ch.env.kill (0.25f);
        lastBypass = p.bypass;

        // Footswitches A and B
        for (int c = 0; c < 2; ++c)
        {
            if (cancelRequest[c]) { cancelRequest[c] = false; cancel (c); }
            if (captureRequest[c]) { captureRequest[c] = false; if (! p.bypass) manualCapture (c, p, group, shapeKnob, envSecs, bass); }
            if (footDown[c] != lastFoot[c])
            {
                footswitchChanged (c, footDown[c], p, group, shapeKnob, envSecs, bass);
                lastFoot[c] = footDown[c];
            }
        }

        // Shape and Repeat rate follow the knobs for sounds already playing
        for (auto& ch : chans)
        {
            ch.env.setShape (shapeKnob.shape);
            if (zone == EnvTime::Repeat) ch.env.setRepeatRate (repeatRate);
        }

        float peakIn = 0.0f, wetPeak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];

            // Bass: everything under 120 Hz stays dry and untouched (Linkwitz-Riley split,
            // so the bands sum flat). The effect is built from the full note, for its pitch,
            // and its own lows are taken out so they never double the dry ones.
            float lowL = 0.0f, lowR = 0.0f, hiL = inL, hiR = inR;
            const float lpL = lowBand[0].process (inL), lpR = lowBand[1].process (inR);
            if (bass)
            {
                lowL = lpL; lowR = lpR;
                hiL = inL - lowL;
                hiR = inR - lowR;
            }
            const float mono = 0.5f * (inL + inR);
            hist[(size_t) (clock & histMask)] = mono;
            peakIn = std::max (peakIn, std::fabs (mono));

            // ---- triggers ----
            const float hit = detector.process (mono);
            if (learning) learnListen (hit);
            if (! p.bypass && ! learning)
            {
                if (externalNote >= 0)
                    autoTrigger (p, group, shapeKnob, zone, envSecs, repeatRate, externalNote, false, bass);
                else if (hit > 0.0f)
                    autoTrigger (p, group, shapeKnob, zone, envSecs, repeatRate, -1, false, bass);
                else if (p.inputMode == InputSynth && ++chgCount >= chgEvery)
                {
                    chgCount = 0;
                    if (listenForChange()) autoTrigger (p, group, shapeKnob, zone, envSecs, repeatRate, -1, true, bass);
                }
            }
            externalNote = -1;

            // ---- Dimension and its modulation ----
            float dim = sDim.next (dimKnob);
            if (p.dimSource == 0 && p.lfoDepth > 0.0f)
                dim += p.lfoDepth * 0.5f * lfo.next (p.lfoShape, p.lfoRateHz, false);
            else if (p.dimSource == 1 && p.lfoDepth > 0.0f)
                dim += p.lfoDepth * envGen.next (p.lfoShape, p.lfoRateHz, true);
            dim = clampf (dim, 0.0f, 1.0f);
            dimNow = dim;
            const int glitchIdx = std::min (kGlitchLengths - 1, (int) (std::min (dim, 0.999f) * kGlitchLengths));

            // ---- the voice-change dip ----
            if (dipping)
            {
                dip -= 1.0f / (0.015f * fs);
                if (dip <= 0.0f)
                {
                    dip = 0.0f;
                    dipping = false;
                    appliedVoice = p.voice;
                    contentFor (p, bass);
                }
            }
            else if (dip < 1.0f)
            {
                dip = std::min (1.0f, dip + 1.0f / (0.025f * fs));
            }
            const int g = groupOf (appliedVoice);

            // ---- Glitch B memory ----
            if (memPending >= 0) memoryCapture (false);
            float memSample = 0.0f;
            if (g == GroupGlitchB)
            {
                // Dimension picks a cell. An empty cell falls back to the newest recording,
                // so a fresh set never sits silent while the memory fills.
                int sel = std::min (kBlocks - 1, (int) (dim * kBlocks));
                if (memLen[sel] <= 32 && memLast >= 0) sel = memLast;
                if (sel != memSel || memVersion[sel] != memPlayedVersion)
                    switchMemory (sel);
                memSample = memPlay.next() + memOld.next();
            }

            // ---- Swell source: the live signal, optionally fuzzed ----
            float swellSrc = mono;
            if (appliedVoice == VSwellB) swellSrc = fuzz.process (mono, detector.level(), detector.playingLevel());

            // ---- the two channels ----
            const float width = sWidth.next (p.width);
            Stereo wet;
            for (int c = 0; c < 2; ++c)
            {
                auto& ch = chans[c];
                recordRaw (ch, mono);
                if (ch.waiting && clock >= ch.analyseAt) runAnalysis (c, p, g, bass);
                if (g == GroupGlitchA) glitchTick (c, glitchIdx, p);

                const float e = ch.env.next();
                envNow[c] = e;

                // Blur keeps its synth running every hop so the overlap-add stays continuous
                Stereo s;
                if (g == GroupBlur)
                {
                    if (ch.synth.frameDue())
                    {
                        ch.synth.shift();
                        if (e > 1.0e-5f || ch.waiting || ch.oldGain > 0.0f) renderFrame (ch, appliedVoice);
                    }
                    s = ch.synth.next();
                }
                if (e <= 1.0e-5f) continue;

                switch (g)
                {
                    case GroupBlur: break;
                    case GroupSynth: s = ch.osc.next (appliedVoice == VSynthA ? 0 : 1, dim); break;
                    case GroupGlitchA: { const float v = ch.loop.next() + ch.oldLoop.next(); s = { v, v }; break; }
                    case GroupGlitchB: s = { memSample, memSample }; break;
                    default: s = { swellSrc, swellSrc }; break;
                }
                s = s * e;
                // A leans left, B leans right; Width runs from mono to a full split
                const float mid = 0.5f * (s.l + s.r), side = 0.5f * (s.l - s.r) * (0.4f + 0.6f * width);
                const Stereo pan = panMono (1.0f, (c == 0 ? -0.7f : 0.7f) * width);
                wet.l += (mid + side) * pan.l;
                wet.r += (mid - side) * pan.r;
            }
            wet = wet * (kWetTrim * raisedCos (dip));
            if (bass) wet = { wetHp[0].process (wet.l), wetHp[1].process (wet.r) };

            // ---- leveller: soft-knee RMS compressor so stacked Poly notes stay even ----
            wet = level (wet);

            // ---- stereo space: delay (Blur and Swell), then Air ----
            const float send = sSend.next (sendDelay ? 1.0f : 0.0f);
            wet = spaceDelay (wet, send, dim, longDelay, width);
            wet = airDiffuse (wet, sAir.next (p.air));
            wetPeak = std::max (wetPeak, std::max (std::fabs (wet.l), std::fabs (wet.r)));

            // ---- mix, then the output stage: drive and tone on everything, like the pedal ----
            const float dg = sDry.next (dryGain);
            const float wg = sWet.next (wetGain);
            const float gate = sDryGate.next (glitchInterrupt ? 1.0f - clampf (envNow[0] + envNow[1], 0.0f, 1.0f) : 1.0f);
            const float drv = sDrive.next (driveKnob);
            const float tn = sTone.next (p.tone);
            const float master = sMaster.next (dbToGain (p.masterDb));
            float mix[2] = { hiL * dg * gate + wet.l * wg, hiR * dg * gate + wet.r * wg };
            const float lowIn[2] = { lowL, lowR }, dryIn[2] = { inL, inR };

            // bypass crossfade (raised cosine, 20 ms)
            bypassMix += clampf ((p.bypass ? 1.0f : 0.0f) - bypassMix, -1.0f / (0.02f * fs), 1.0f / (0.02f * fs));
            const float bm = raisedCos (bypassMix);
            const float tailGain = p.tails ? 1.0f : 1.0f - bm;
            const float wetTail[2] = { wet.l * wg * tailGain, wet.r * wg * tailGain };

            float out[2];
            for (int c = 0; c < 2; ++c)
            {
                const float driven = outputStage (c, mix[c], drv, tn, p.autoLevel);
                latL[c].push (dryIn[c]);
                latLow[c].push (lowIn[c]);
                const float dryAligned = latL[c].at (Oversampler4x::kLatency + 1); // at(1) is the newest
                const float lowAligned = latLow[c].at (Oversampler4x::kLatency + 1);
                const float on = driven + lowAligned;
                const float off = dryAligned + wetTail[c];
                out[c] = softCeiling ((on * (1.0f - bm) + off * bm) * master);
            }
            L[i] = out[0];
            R[i] = out[1];
            ++clock;
        }

        uiInput.store (std::max (peakIn, uiInput.load() * 0.85f));
        uiWet.store (std::max (wetPeak, uiWet.load() * 0.85f));
        uiEnvA.store (envNow[0]);
        uiEnvB.store (envNow[1]);
        uiDim.store (dimNow);
        uiSelected.store (memSel < 0 ? 0 : memSel);
        uiSustainA.store (chans[0].env.isSustained());
        uiSustainB.store (chans[1].env.isSustained());
    }

private:
    // ---------------------------------------------------------------------------
    struct Channel
    {
        Envelope env;
        jet::Voice voice, oldVoice;
        bool hasVoice = false;
        float oldGain = 0.0f;
        float wBase = 1.0f, wUp = 0.0f, wDown = 0.0f; // octave layer weights, gliding
        Synth synth;
        SynthOsc osc;
        float captureRms = 0.0f;
        int midi = -1;

        std::array<std::vector<float>, 2> raw;
        int cur = 0, rawFill = 0;
        bool recording = false;
        LoopReader loop, oldLoop;
        bool hasLoop = false;
        int loopIdx = -1;

        bool waiting = false, startOnReady = false;
        int64_t analyseAt = 0, anaStart = 0;
        bool anaBass = false;
        EnvRequest req;
        Rng rng;

        void reset()
        {
            hasVoice = false; oldGain = 0.0f; rawFill = 0; recording = false;
            hasLoop = false; loopIdx = -1; waiting = false; startOnReady = false;
            loop = oldLoop = LoopReader();
            wBase = 1.0f; wUp = wDown = 0.0f;
            synth.clear();
        }
    };

    // ---------------------------------------------------------------------------
    // Triggers
    // ---------------------------------------------------------------------------
    EnvRequest makeRequest (const EngineParams& p, int group, const ShapeKnob& sk, EnvTime::Zone zone, float envSecs, float repeatRate, int c)
    {
        EnvRequest r;
        r.shape = sk.shape;
        r.randomRate = sk.rateHz;
        r.duration = group == GroupGlitchB && zone == EnvTime::Timed ? std::max (envSecs, 0.3f) : envSecs;
        r.repeatRate = repeatRate;
        if (zone == EnvTime::Repeat)
        {
            r.mode = Envelope::Mode::Repeat;
            const auto& other = chans[1 - c].env;
            // Poly: the two channels alternate, half a cycle apart. Mono: in step.
            r.phase = other.isActive() ? other.getPhase() + (p.trigger == 0 ? 0.5f : 0.0f) : 0.0f;
        }
        else if (zone == EnvTime::Infinite)
        {
            r.mode = Envelope::Mode::Sustain;
            r.duration = 2.0f; // the attack part of the shape, then hold
        }
        else
        {
            r.mode = Envelope::Mode::OneShot;
        }
        // Swell in Mono uses only the attack part of the shape, so fast playing stays clear
        if (group == GroupSwell && p.trigger == 1 && zone != EnvTime::Repeat)
            r.mode = Envelope::Mode::Sustain;
        return r;
    }

    int pickChannel()
    {
        // never steal a channel that's being held on its footswitch
        int c = nextChannel;
        if (chans[c].env.isSustained()) c = 1 - c;
        if (chans[c].env.isSustained()) return -1;
        nextChannel = 1 - c;
        return c;
    }

    void autoTrigger (const EngineParams& p, int group, const ShapeKnob& sk, EnvTime::Zone zone, float envSecs,
                      float repeatRate, int midiNote, bool fromChange, bool bass)
    {
        if (p.trigger == 2) return; // Manual: the footswitches decide
        envGen.retrigger();

        if (group == GroupGlitchB)
        {
            // Env Time sets how often a note is recorded and played back
            const float chance = zone == EnvTime::Timed ? 0.05f + 0.95f * std::pow (p.envTime / EnvTime::kInfinite, 1.5f) : 1.0f;
            if (rng.uni() > chance) return;
        }
        const int c = pickChannel();
        if (c < 0) return;
        startCapture (c, p, group, makeRequest (p, group, sk, zone, envSecs, repeatRate, c), midiNote, fromChange, bass);
    }

    // fromPast: the note is already ringing (chord change, footswitch), so analyse what's just been heard
    void startCapture (int c, const EngineParams& p, int group, const EnvRequest& req, int midiNote, bool fromPast, bool bass)
    {
        auto& ch = chans[c];
        ch.req = req;
        ch.midi = midiNote;
        ch.startOnReady = true;
        ++captures;
        uiCaptures.store (captures);

        // raw audio for the Glitch voices, with its pre-roll
        if (ch.loop.active()) { ch.oldLoop = ch.loop; ch.oldLoop.stop (0.012f * fs); }
        ch.loop = LoopReader();
        ch.hasLoop = false;
        ch.loopIdx = -1;
        ch.cur ^= 1;
        auto& buf = ch.raw[(size_t) ch.cur];
        const int past = fromPast ? std::min ((int) (0.36f * fs), (int) std::min<int64_t> (clock, (int64_t) (0.36f * fs))) : 0;
        const int64_t from = clock - preLen - past;
        for (int k = 0; k < preLen + past; ++k)
            buf[(size_t) k] = from + k >= 0 ? hist[(size_t) ((from + k) & histMask)] : 0.0f;
        ch.rawFill = preLen + past;
        ch.recording = ch.rawFill < rawCap;

        // spectral analysis for Blur and Synth
        auto& ana = bass ? anaBass : anaNormal;
        const float skipSec = midiNote >= 0 ? 0.012f : (p.inputMode == InputSynth ? 0.015f : (bass ? 0.045f : 0.035f));
        ch.anaBass = bass;
        if (fromPast) ch.anaStart = clock - ana.span();
        else ch.anaStart = clock + (int64_t) (skipSec * fs); // skip the pick attack, like the pedal's Blur
        ch.analyseAt = ch.anaStart + ana.span();
        ch.waiting = true;

        if (group == GroupGlitchB && ! p.hold) beginMemoryCapture (c);

        // Swell and Glitch B start straight away. Blur and Synth wait for their analysis,
        // Glitch A for its first loop.
        if (group == GroupSwell || group == GroupGlitchB) beginSound (c, p);
    }

    void beginSound (int c, const EngineParams& p)
    {
        auto& ch = chans[c];
        if (! ch.startOnReady) return;
        ch.startOnReady = false;
        if (p.trigger == 1) chans[1 - c].env.kill (0.12f); // Mono: the new note takes over without a gap
        switch (ch.req.mode)
        {
            case Envelope::Mode::Sustain: ch.env.startSustain (ch.req.shape, ch.req.duration, ch.req.randomRate); break;
            case Envelope::Mode::Repeat:  ch.env.startRepeat (ch.req.shape, ch.req.repeatRate, ch.req.phase); break;
            default:                      ch.env.startOneShot (ch.req.shape, ch.req.duration, ch.req.randomRate); break;
        }
        ch.env.setSustain (footDown[c] && p.trigger != 2);
    }

    void manualCapture (int c, const EngineParams& p, int group, const ShapeKnob& sk, float envSecs, bool bass)
    {
        EnvRequest r;
        r.shape = sk.shape;
        r.randomRate = sk.rateHz;
        // past the Infinite mark Env Time has no length, so the shape's attack takes 2 s like auto mode
        r.duration = EnvTime::zone (p.envTime) == EnvTime::Timed ? envSecs : 2.0f;
        r.mode = Envelope::Mode::Sustain;
        if (group == GroupSwell) { r.shape = ShapeSquare; r.duration = 0.06f; } // Swell: the way into the delay opens while held
        envGen.retrigger();
        startCapture (c, p, group, r, -1, group != GroupSwell && group != GroupGlitchB, bass);
    }

    void footswitchChanged (int c, bool down, const EngineParams& p, int group, const ShapeKnob& sk, float envSecs, bool bass)
    {
        auto& ch = chans[c];
        if (p.bypass) { ch.env.setSustain (false); return; }

        if (p.trigger != 2)
        {
            // Poly and Mono: holding A or B keeps that sound ringing until you let go
            ch.env.setSustain (down);
            return;
        }

        const double t = (double) clock / fs;
        if (group == GroupSwell || p.fsMode == 2) // Momentary
        {
            if (down) manualCapture (c, p, group, sk, envSecs, bass);
            else { ch.startOnReady = false; ch.env.release(); }
            return;
        }
        if (! down) return;

        const bool sounding = ch.env.isHolding() || ch.startOnReady || ch.waiting;
        if (p.fsMode == 0) // Toggle
        {
            if (sounding) { ch.startOnReady = false; ch.waiting = false; ch.env.release(); }
            else manualCapture (c, p, group, sk, envSecs, bass);
        }
        else // Latch: each press grabs a new note, a quick double press clears
        {
            if (t - lastPress[c] < 0.35) { ch.startOnReady = false; ch.waiting = false; ch.env.release(); }
            else manualCapture (c, p, group, sk, envSecs, bass);
            lastPress[c] = t;
        }
    }

    void cancel (int c)
    {
        auto& ch = chans[c];
        ch.startOnReady = false;
        ch.waiting = false;
        ch.env.kill (0.03f);
    }

    // ---------------------------------------------------------------------------
    // Content
    // ---------------------------------------------------------------------------
    void recordRaw (Channel& ch, float x)
    {
        if (! ch.recording) return;
        ch.raw[(size_t) ch.cur][(size_t) ch.rawFill++] = x;
        if (ch.rawFill >= rawCap) ch.recording = false;
    }

    void runAnalysis (int c, const EngineParams& p, int group, bool bass)
    {
        auto& ch = chans[c];
        ch.waiting = false;
        auto& ana = ch.anaBass ? anaBass : anaNormal;
        const float rms = ana.measure ([this] (int64_t i) { return i >= 0 ? hist[(size_t) (i & histMask)] : 0.0f; }, ch.anaStart);

        // judged against how loud you've been playing, never a fixed level
        const float floor = std::max (2.0e-6f, detector.playingLevel() * 0.02f);
        if (rms >= floor && ana.build (scratch, ch.rng, kNoise))
        {
            if (ch.hasVoice && (ch.env.level() > 1.0e-3f))
            {
                ch.oldVoice.copyFrom (ch.voice); // the old note fades out under the new one
                ch.oldGain = 1.0f;
            }
            ch.voice.copyFrom (scratch);
            setupOctaves (ch.voice);
            ch.hasVoice = true;
            ch.captureRms = rms;
            ch.osc.setFromVoice (ch.voice, rms, ch.midi, bass);
            if (ch.osc.hasNotes()) uiNoteHz.store (ch.osc.noteHz (0));
            // octave layers fade in behind the note ("Blur +1 lags behind your playing")
            ch.wUp = ch.wDown = 0.0f;
        }
        else if (! ch.hasVoice)
        {
            ch.startOnReady = false; // nothing there to hold
            return;
        }
        if (group == GroupBlur || group == GroupSynth) beginSound (c, p);
    }

    void setupOctaves (jet::Voice& v)
    {
        for (int i = 0; i < v.np; ++i)
        {
            auto& pt = v.p[(size_t) i];
            const float ratios[2] = { 2.0f, 0.5f };
            for (int j = 0; j < 2; ++j)
            {
                const float ph = kTwoPi * rng.uni();
                pt.resL[(size_t) j] = std::polar (1.0f, ph);
                pt.resR[(size_t) j] = std::polar (1.0f, ph + 0.9f * rng.next());
                pt.resRot[(size_t) j] = std::polar (1.0f, pt.omega * ratios[j] * (float) hop);
            }
        }
    }

    // When the voice changes, rebuild the held notes in the new voice
    void contentFor (const EngineParams& p, bool bass)
    {
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = chans[c];
            if (groupOf (appliedVoice) == GroupSynth && ch.hasVoice)
                ch.osc.setFromVoice (ch.voice, ch.captureRms, ch.midi, bass);
            if (ch.startOnReady && (groupOf (appliedVoice) == GroupSwell || groupOf (appliedVoice) == GroupGlitchB))
                beginSound (c, p);
            ch.loopIdx = -1; // Glitch A rebuilds its loop at the current length
        }
    }

    void renderFrame (Channel& ch, int voice)
    {
        float tb = 1.0f, tu = 0.0f, td = 0.0f;
        switch (voice)
        {
            case VBlurDown: tb = 0.82f; td = 0.72f; break;
            case VBlurUp:   tb = 0.82f; tu = 0.56f; break;
            case VBlurBoth: tb = 0.72f; tu = 0.48f; td = 0.6f; break;
            default: break;
        }
        const float norm = 1.0f / std::sqrt (tb * tb + tu * tu + td * td);
        tb *= norm; tu *= norm; td *= norm;
        const float k = 1.0f - std::exp (-(float) hop / (0.35f * fs)); // octaves glide in over 0.35 s
        ch.wBase += (tb - ch.wBase) * k;
        ch.wUp += (tu - ch.wUp) * k;
        ch.wDown += (td - ch.wDown) * k;
        if (ch.oldGain > 0.0f) ch.oldGain = std::max (0.0f, ch.oldGain - (float) hop / (0.12f * fs));

        ch.synth.beginFrame();
        if (ch.hasVoice) addVoice (ch, ch.voice, ch.wBase, ch.wUp, ch.wDown, true);
        if (ch.oldGain > 0.0f) addVoice (ch, ch.oldVoice, ch.wBase * ch.oldGain, 0.0f, 0.0f, false);
        ch.synth.endFrame (ch.rng);
    }

    void addVoice (Channel& ch, jet::Voice& v, float wb, float wu, float wd, bool withNoise)
    {
        const float nyq = kPi * 0.92f, lowest = kTwoPi * 22.0f / fs;
        for (int i = 0; i < v.np; ++i)
        {
            auto& pt = v.p[(size_t) i];
            pt.wobPh += pt.wobStep;
            if (pt.wobPh > kTwoPi) pt.wobPh -= kTwoPi;
            const float s = std::sin (pt.wobPh);
            const float wl = 1.0f + 0.045f * s, wr = 1.0f + 0.045f * std::sin (pt.wobPh + 1.9f);
            const float a = pt.amp * wb;
            ch.synth.addSine (pt.omega, pt.zL * (a * wl), pt.zR * (a * wr));
            if (wu > 1.0e-4f && pt.omega * 2.0f < nyq)
                ch.synth.addSine (pt.omega * 2.0f, pt.resL[0] * (pt.amp * wu * wr), pt.resR[0] * (pt.amp * wu * wl));
            if (wd > 1.0e-4f && pt.omega * 0.5f > lowest)
                ch.synth.addSine (pt.omega * 0.5f, pt.resL[1] * (pt.amp * wd), pt.resR[1] * (pt.amp * wd));
            pt.zL *= pt.rot; pt.zR *= pt.rot;
            pt.zL *= 1.5f - 0.5f * std::norm (pt.zL);
            pt.zR *= 1.5f - 0.5f * std::norm (pt.zR);
            for (int j = 0; j < 2; ++j)
            {
                pt.resL[(size_t) j] *= pt.resRot[(size_t) j];
                pt.resR[(size_t) j] *= pt.resRot[(size_t) j];
                pt.resL[(size_t) j] *= 1.5f - 0.5f * std::norm (pt.resL[(size_t) j]);
                pt.resR[(size_t) j] *= 1.5f - 0.5f * std::norm (pt.resR[(size_t) j]);
            }
        }
        if (withNoise && v.noiseBins > 0 && v.noiseScale > 0.0f)
        {
            const float binHz = ch.synth.binHz();
            const int last = ch.synth.size() / 2 - 1;
            for (int k = 1; k < last; ++k)
            {
                const float m = v.noiseAt ((float) k * binHz) * v.noiseScale * wb;
                if (m <= 0.0f) continue;
                ch.synth.addNoisePower (k, m * m, m * m);
            }
        }
    }

    static int glitchLength (int index, float sampleRate)
    {
        static const float ms[kGlitchLengths] = { 45.0f, 90.0f, 180.0f, 360.0f };
        return (int) (ms[std::min (std::max (index, 0), kGlitchLengths - 1)] * 0.001f * sampleRate);
    }

    void glitchTick (int c, int idx, const EngineParams& p)
    {
        auto& ch = chans[c];
        if (ch.loopIdx == idx && ch.hasLoop) return;
        const int len = glitchLength (idx, fs);
        if (ch.rawFill < preLen + len) return; // wait until a loop's worth of the note is in
        const float* buf = ch.raw[(size_t) ch.cur].data();
        const int end = matchedLoopEnd (buf, preLen, preLen + len, (int) (0.01f * fs), (int) (0.004f * fs));
        if (! ch.hasLoop)
        {
            ch.loop.begin (buf, preLen, end, (int) (0.006f * fs), 0.004f * fs);
            ch.hasLoop = true;
            beginSound (c, p);
        }
        else
        {
            ch.loop.setEnd (end);
        }
        ch.loopIdx = idx;
    }

    // ---------------------------------------------------------------------------
    // Glitch B memory: six cells, each a note recorded as a short loop
    // ---------------------------------------------------------------------------
    void beginMemoryCapture (int c)
    {
        if (memPending >= 0) memoryCapture (true);
        memPending = c;
        memTarget = (int) (rng.uni() * kBlocks) % kBlocks;
        uiWriting.store (memTarget);
    }

    void memoryCapture (bool force)
    {
        const auto& ch = chans[memPending];
        const int have = ch.rawFill - preLen;
        const int want = (int) (0.3f * fs);
        if (have >= want || (force && have >= (int) (0.08f * fs)) || (! ch.recording && have > 0))
        {
            const int len = std::min ({ have, want, memCap - preLen });
            const float* src = ch.raw[(size_t) ch.cur].data();
            std::copy (src, src + preLen + len, memory[memTarget].begin());
            memLen[memTarget] = preLen + len;
            memPre[memTarget] = preLen;
            memLast = memTarget;
            ++memVersion[memTarget];
            publishBlock (memTarget);
            memPending = -1;
            uiWriting.store (-1);
        }
        else if (force)
        {
            memPending = -1;
            uiWriting.store (-1);
        }
    }

    void publishBlock (int b)
    {
        uiFilled[b].store (memLen[b] > 32);
        const int s0 = memPre[b], len = std::max (0, memLen[b] - memPre[b]);
        for (int k = 0; k < kOverview; ++k)
        {
            float pk = 0.0f;
            const int a = s0 + len * k / kOverview, e = s0 + len * (k + 1) / kOverview;
            for (int i = a; i < e; ++i) pk = std::max (pk, std::fabs (memory[b][(size_t) i]));
            uiOverview[b][k].store (std::min (1.0f, pk * 2.5f));
        }
    }

    void switchMemory (int sel)
    {
        if (memPlay.active()) { memOld = memPlay; memOld.stop (0.02f * fs); }
        memPlay = LoopReader();
        memSel = sel;
        memPlayedVersion = memVersion[sel];
        if (memLen[sel] - memPre[sel] > 32)
        {
            const float* b = memory[sel].data();
            const int end = matchedLoopEnd (b, memPre[sel], memLen[sel], (int) (0.01f * fs), (int) (0.004f * fs));
            memPlay.begin (b, memPre[sel], end, (int) (0.008f * fs), 0.02f * fs);
        }
    }

    void pollRequests()
    {
        if (clearRequest.exchange (false))
        {
            for (int b = 0; b < kBlocks; ++b) { memLen[b] = 0; memPre[b] = 0; ++memVersion[b]; publishBlock (b); }
            memPending = -1;
            memLast = -1;
            memPlay.stop (0.02f * fs);
        }
        if (loadReady.load())
        {
            for (int b = 0; b < kBlocks; ++b)
            {
                // resample if the set was saved at another sample rate
                const auto& src = loadData[b];
                const float ratio = loadRate > 0.0f ? loadRate / fs : 1.0f;
                const int len = std::min ((int) ((float) src.size() / ratio), memCap);
                for (int i = 0; i < len; ++i)
                {
                    const float x = (float) i * ratio;
                    const int i0 = std::min ((int) x, (int) src.size() - 1);
                    const int i1 = std::min (i0 + 1, (int) src.size() - 1);
                    memory[b][(size_t) i] = lerpf (src[(size_t) i0], src[(size_t) i1], x - (float) i0);
                }
                memLen[b] = len;
                // older sets saved no pre-roll: use the first 12 ms as one
                memPre[b] = loadPre[b] > 0 ? std::min (len, (int) ((float) loadPre[b] / ratio)) : std::min (len / 4, preLen);
                ++memVersion[b];
                publishBlock (b);
                if (len > 32) memLast = b;
            }
            loadReady.store (false);
        }
        if (learnRequest.exchange (false))
        {
            learning = true;
            learnLeft = (int) (4.0f * fs);
            learnCount = 0;
            learnSoftest = 0.0f;
            learnWindow = 0;
            uiLearnResult.store (-1.0f);
        }
        uiLearning.store (learning);
    }

    // ---------------------------------------------------------------------------
    // Chord changes (Synth input): a steady new spectrum counts as a new note
    // ---------------------------------------------------------------------------
    bool listenForChange()
    {
        if (! detector.sounding()) { havePrev = false; return false; }
        for (int i = 0; i < chgN; ++i)
            chgBuf[(size_t) i] = { hist[(size_t) ((clock - chgN + i) & histMask)] * chgWin[(size_t) i], 0.0f };
        chgFft.run (chgBuf.data(), false);
        for (int k = 0; k < chgN / 2; ++k) chgNow[(size_t) k] = std::abs (chgBuf[(size_t) k]);
        auto similar = [&] (const std::vector<float>& a, const std::vector<float>& b)
        {
            double ab = 0, aa = 0, bb = 0;
            for (int k = 2; k < chgN / 2; ++k) { ab += (double) a[(size_t) k] * b[(size_t) k]; aa += (double) a[(size_t) k] * a[(size_t) k]; bb += (double) b[(size_t) k] * b[(size_t) k]; }
            return (float) (ab / std::sqrt (aa * bb + 1.0e-30));
        };
        const bool steady = havePrev && similar (chgNow, chgPrev) > 0.9f;
        const bool moved = ! haveRef || similar (chgNow, chgRef) < 0.72f;
        chgPrev = chgNow;
        havePrev = true;
        if (steady && moved && (double) (clock - lastChangeAt) > 0.25 * fs)
        {
            chgRef = chgNow;
            haveRef = true;
            lastChangeAt = clock;
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Learn: listens for four seconds and sets the sensitivity just under your softest note
    // ---------------------------------------------------------------------------
    void learnListen (float hit)
    {
        if (hit > 0.0f) { learnWindow = (int) (0.2f * fs); learnPeak = 0.0f; }
        if (learnWindow > 0)
        {
            learnPeak = std::max (learnPeak, detector.level());
            if (--learnWindow == 0 && learnPeak > 1.0e-6f)
            {
                const float rel = learnPeak / detector.playingLevel(); // 0 dB = as loud as you play
                learnSoftest = learnCount == 0 ? rel : std::min (learnSoftest, rel);
                ++learnCount;
            }
        }
        if (--learnLeft <= 0)
        {
            learning = false;
            if (learnCount > 0)
            {
                const float softDb = 20.0f * std::log10 (std::max (1.0e-4f, learnSoftest));
                // the gate lands 17 dB under your softest note
                uiLearnResult.store (clampf ((8.0f - softDb) / 24.0f, 0.0f, 1.0f));
            }
            else
            {
                uiLearnResult.store (-2.0f); // heard nothing
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Wet processing
    // ---------------------------------------------------------------------------
    Stereo level (Stereo in)
    {
        // RMS with an 80 ms attack: soft knee from -14 dBFS, 3:1 above it
        const float pw = 0.5f * (in.l * in.l + in.r * in.r);
        rmsEnv = pw > rmsEnv ? rmsAtt * rmsEnv + (1.0f - rmsAtt) * pw : rmsRel * rmsEnv + (1.0f - rmsRel) * pw;
        const float db = 10.0f * std::log10 (rmsEnv + 1.0e-12f);
        const float over = db + 14.0f, knee = 6.0f;
        float cut = 0.0f;
        if (over > knee * 0.5f) cut = over * (2.0f / 3.0f);
        else if (over > -knee * 0.5f) { const float x = over + knee * 0.5f; cut = (2.0f / 3.0f) * x * x / (2.0f * knee); }
        return in * sLevel.next (dbToGain (-cut));
    }

    Stereo spaceDelay (Stereo in, float send, float dim, bool longer, float width)
    {
        // Dimension: a few milliseconds (chorus and flange) up to long, dark, feeding-back repeats.
        // The time glides, so moving Dimension bends the pitch like the pedal.
        const float maxMs = longer ? 1350.0f : 675.0f;
        const float targetMs = 4.0f * std::pow (maxMs / 4.0f, dim);
        const float ms = sDelay.next (targetMs);
        driftPhase += 0.13f / fs;
        if (driftPhase >= 1.0f) driftPhase -= 1.0f;
        const float drift = 1.0f + 0.0035f * std::sin (kTwoPi * driftPhase);
        const float fb = clampf (0.12f + 0.62f * std::pow (dim, 0.8f), 0.0f, 0.78f);
        const float cut = 14000.0f * std::pow (2.0f, -2.0f * dim);
        if ((clock & 15) == 0) { dlDamp[0].setCutoff (cut, fs); dlDamp[1].setCutoff (cut, fs); }

        const float tL = ms * 0.001f * fs * drift;
        const float tR = ms * 0.001f * fs * (1.0f + 0.13f * width) / drift;
        const float yL = dl[0].tap (tL), yR = dl[1].tap (tR);
        const float cross = 0.22f * width;
        dl[0].push (softClip (in.l * send + dlDamp[0].process (yL * (1.0f - cross) + yR * cross) * fb));
        dl[1].push (softClip (in.r * send + dlDamp[1].process (yR * (1.0f - cross) + yL * cross) * fb));
        const float amt = (0.25f + 0.35f * dim) * send;
        return { in.l + yL * amt, in.r + yR * amt };
    }

    Stereo airDiffuse (Stereo in, float air)
    {
        // A soft, dark wash: two diffusers into a four-line feedback network
        auto allpass = [&] (int i, float x)
        {
            const float d = ap[i].tap (apLen[i]);
            const float v = x + 0.6f * d;
            ap[i].push (v);
            return d - 0.6f * v;
        };
        const float a = allpass (1, allpass (0, in.l));
        const float b = allpass (3, allpass (2, in.r));
        float y[4];
        for (int i = 0; i < 4; ++i) y[i] = fdn[i].tap (fdnLen[i]);
        const float h0 = 0.5f * (y[0] + y[1] + y[2] + y[3]);
        const float h1 = 0.5f * (y[0] - y[1] + y[2] - y[3]);
        const float h2 = 0.5f * (y[0] + y[1] - y[2] - y[3]);
        const float h3 = 0.5f * (y[0] - y[1] - y[2] + y[3]);
        const float g = 0.55f + 0.33f * air; // longer as you turn it up
        fdn[0].push (fdnDamp[0].process (h0 * g + a * 0.5f));
        fdn[1].push (fdnDamp[1].process (h1 * g + b * 0.5f));
        fdn[2].push (fdnDamp[2].process (h2 * g + a * 0.5f));
        fdn[3].push (fdnDamp[3].process (h3 * g + b * 0.5f));
        const float amt = air * 0.85f;
        return { in.l + (y[0] + y[2]) * amt, in.r + (y[1] + y[3]) * amt };
    }

    // Drive (over 10x gain, level-matched) at 4x, so its harmonics don't fold back, then Tone
    float outputStage (int c, float x, float drive, float tone, bool autoLevel)
    {
        const float g = 1.0f + 24.0f * drive * drive;
        const float comp = autoLevel ? 0.3f / std::tanh (0.3f * g) : 1.0f;
        const float amount = clampf (drive * 25.0f, 0.0f, 1.0f); // fully clean at zero
        // first-order antiderivative anti-aliasing on top of 4x: the tanh is averaged across
        // each step instead of sampled, which takes the folded harmonics down another ~20 dB
        double& x0 = adaaPrev[c];
        auto shape = [&] (float v)
        {
            const double x1 = v, dx = x1 - x0;
            const double mid = 0.5 * (x0 + x1);
            double t;
            if (std::fabs (dx) < 1.0e-5) t = std::tanh (g * mid);
            else t = (logCosh (g * x1) - logCosh (g * x0)) / (g * dx);
            x0 = x1;
            return (float) (mid + (t * comp - mid) * amount);
        };
        const float y = os[c].process (x, shape);
        if (tone < 0.995f)
        {
            if ((clock & 15) == 0) toneLp[c].setCutoff (1500.0f * std::pow (13.0f, tone), fs);
            return toneLp[c].process (y);
        }
        toneLp[c].z = y;
        return y;
    }

    static double logCosh (double z)
    {
        z = std::fabs (z);
        return z + std::log1p (std::exp (-2.0 * z)) - 0.69314718055994531;
    }
    double adaaPrev[2] = { 0.0, 0.0 };

    // ---------------------------------------------------------------------------
    float fs = 48000.0f;
    int hop = 512;
    Analyser anaNormal, anaBass;
    jet::Voice scratch;
    std::vector<float> hist;
    int64_t histMask = 0, clock = 0;
    Detector detector;
    Channel chans[2];
    Lfo lfo, envGen;
    Fuzz fuzz;
    Rng rng;
    int preLen = 576, rawCap = 0;
    int nextChannel = 0;
    float envNow[2] = { 0.0f, 0.0f };
    float dimNow = 0.5f;
    int appliedVoice = -1;
    float dip = 1.0f;
    bool dipping = false;
    bool footDown[2] = { false, false }, lastFoot[2] = { false, false };
    bool captureRequest[2] = { false, false }, cancelRequest[2] = { false, false };
    bool lastBypass = false;
    float bypassMix = 0.0f;
    double lastPress[2] = { -10.0, -10.0 };
    int externalNote = -1;
    int captures = 0;

    // chord changes
    Fft chgFft;
    int chgN = 4096, chgEvery = 3840, chgCount = 0;
    std::vector<float> chgWin, chgNow, chgPrev, chgRef;
    std::vector<std::complex<float>> chgBuf;
    bool havePrev = false, haveRef = false;
    int64_t lastChangeAt = -1000000;

    // Glitch B memory
    std::array<std::vector<float>, kBlocks> memory;
    int memLen[kBlocks] = {}, memPre[kBlocks] = {};
    uint32_t memVersion[kBlocks] = {};
    uint32_t memPlayedVersion = 0;
    int memCap = 0, memSel = -1, memPending = -1, memTarget = 0, memLast = -1;
    LoopReader memPlay, memOld;
    std::atomic<bool> clearRequest { false }, loadReady { false }, learnRequest { false };
    std::array<std::vector<float>, kBlocks> loadData;
    std::array<int, kBlocks> loadPre {};
    float loadRate = 48000.0f;

    // Learn
    bool learning = false;
    int learnLeft = 0, learnCount = 0, learnWindow = 0;
    float learnSoftest = 1.0f, learnPeak = 0.0f;

    // Space
    DelayLine dl[2];
    OnePole dlDamp[2];
    DelayLine fdn[4], ap[4];
    float fdnLen[4] = {}, apLen[4] = {};
    OnePole fdnDamp[4];
    float driftPhase = 0.0f;
    float rmsEnv = 0.0f, rmsAtt = 0.0f, rmsRel = 0.0f;

    // Output
    Oversampler4x os[2];
    OnePole toneLp[2];
    LR4 lowBand[2], wetHp[2];
    DelayLine latL[2], latLow[2];
    Smoother sDim, sDrive, sTone, sDry, sWet, sMaster, sWidth, sAir, sSend, sDryGate, sComp, sDelay, sLevel;
};
} // namespace jet

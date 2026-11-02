#pragma once

#include "Detector.h"
#include "Sampler.h"
#include <array>

// The whole Jetstream signal path, free of JUCE:
// input -> note detection -> samplers A and B -> voice -> stereo space -> drive and tone -> dry mix -> output
namespace jet
{
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
    float dry = 0.0f;
    float masterDb = 0.0f;
    bool autoLevel = true;
    float width = 0.8f;
    float air = 0.35f;
    bool hold = false;
    bool bypass = false;
    bool footA = false, footB = false;
};

// Env Time knob zones
struct EnvTime
{
    enum Zone { Timed, Infinite, Repeat };
    static constexpr float kInfinite = 0.90f, kRepeat = 0.95f;
    static Zone zone (float t) { return t >= kRepeat ? Repeat : (t >= kInfinite ? Infinite : Timed); }
    static float seconds (float t) { return 0.05f * std::pow (400.0f, clampf (t / kInfinite, 0.0f, 1.0f)); } // 50 ms .. 20 s
};

class JetEngine
{
public:
    static constexpr int kBlocks = 6;
    static constexpr int kOverview = 8;
    static constexpr float kWetTrim = 0.72f; // sustained sounds sit level with the dry signal

    // ---- read by the panel ----
    std::atomic<float> uiInput { 0.0f }, uiEnvA { 0.0f }, uiEnvB { 0.0f }, uiDim { 0.5f };
    std::atomic<bool> uiFilled[kBlocks];
    std::atomic<float> uiOverview[kBlocks][kOverview];
    std::atomic<int> uiSelected { 0 }, uiWriting { -1 };
    std::atomic<int> uiCaptures { 0 };
    std::atomic<bool> uiLearning { false };
    std::atomic<float> uiLearnResult { -1.0f };

    JetEngine()
    {
        for (int b = 0; b < kBlocks; ++b)
        {
            uiFilled[b].store (false);
            for (int k = 0; k < kOverview; ++k) uiOverview[b][k].store (0.0f);
        }
    }

    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        detector.prepare (fs);
        chans[0].prepare (fs, 0x1F2E3D4Cu);
        chans[1].prepare (fs, 0x5A6B7C8Du);
        lfo.prepare (fs);
        envGen.prepare (fs);

        preRoll.init (nextPow2 ((int) (0.06f * fs)));
        preCopy.assign ((size_t) preRoll.size(), 0.0f);
        preSamples = (int) (0.004f * fs);

        memCap = (int) (Channel::kRawSeconds * fs);
        for (int b = 0; b < kBlocks; ++b)
        {
            if ((int) memory[b].size() != memCap + 8)
            {
                memory[b].assign ((size_t) memCap + 8, 0.0f);
                memLen[b] = 0;
                uiFilled[b].store (false);
            }
        }
        memPlay = memOld = LoopReader();

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

        for (auto* s : { &sDim, &sDrive, &sTone, &sDry, &sWet, &sMaster, &sWidth, &sAir, &sSend, &sDryGate, &sComp })
            s->setTime (0.025f, fs);
        sDelay.setTime (0.12f, fs);
        sSwellGate.setTime (0.01f, fs);
        toneLp[0].reset(); toneLp[1].reset();
        lows[0].reset(); lows[1].reset(); lows2[0].reset(); lows2[1].reset();
        for (auto& f : fuzzLp) f.reset();
        for (auto& f : fuzzLp) f.setCutoff (5200.0f, fs);
        for (int c = 0; c < 2; ++c) { lows[c].setCutoff (120.0f, fs); lows2[c].setCutoff (120.0f, fs); }

        nextChannel = 0;
        lastFootA = lastFootB = false;
        lastBypass = false;
        memPending = -1;
        sampleClock = 0;
        driftPhase = 0.0f;
    }

    // ---- control from the plugin wrapper ----
    void noteOn (int midiNote, float velocity) { externalNote = midiNote; externalVel = velocity; }
    void startLearn() { learnRequest.store (true); }
    void clearMemory() { clearRequest.store (true); }

    // Memory for saving with the set (message thread). Copies are racy but harmless.
    int memoryLength (int b) const { return memLen[b]; }
    const float* memoryData (int b) const { return memory[b].data(); }
    float sampleRate() const { return fs; }

    // Loads saved memory (message thread); the audio thread picks it up at the next block
    void loadMemory (const std::array<std::vector<float>, kBlocks>& blocks, float savedRate)
    {
        if (loadReady.load()) return;
        for (int b = 0; b < kBlocks; ++b) loadData[b] = blocks[b];
        loadRate = savedRate;
        loadReady.store (true);
    }

    void process (float* L, float* R, int n, const EngineParams& p)
    {
        pollRequests();
        detector.setMode (p.inputMode);
        detector.setSensitivity (learning ? 1.0f : p.sensitivity);

        const int group = groupOf (p.voice);
        const auto shapeKnob = ShapeKnob::from (p.envShape);
        const auto zone = EnvTime::zone (p.envTime);
        const float envSecs = EnvTime::seconds (p.envTime);
        const float repeatRate = p.repeatRateHz > 0.0f ? p.repeatRateHz : shapeKnob.rateHz;
        const bool bass = p.inputMode == InputBass;
        const float dimKnob = p.dimOverride >= 0.0f ? p.dimOverride : p.dimension;
        const float driveKnob = p.driveOverride >= 0.0f ? p.driveOverride : p.drive;
        const bool longDelay = p.voice == VBlurBoth;
        const bool sendDelay = group == GroupBlur || group == GroupSwell;

        // Dry: up to 50% the effect stays full, above 75% it gives way to the dry signal (like the pedal)
        const float dryGain = p.bypass ? 1.0f : clampf (p.dry * 2.0f, 0.0f, 1.0f);
        const float wetGain = p.bypass ? 1.0f : (p.dry > 0.75f ? clampf (1.0f - (p.dry - 0.75f) / 0.25f, 0.0f, 1.0f) : 1.0f);
        const bool glitchInterrupt = group == GroupGlitchB && p.dry < 0.15f && ! p.bypass;

        // Bypass: stop the samplers but let the tails ring out
        if (p.bypass && ! lastBypass)
        {
            chans[0].kill (0.3f);
            chans[1].kill (0.3f);
        }
        lastBypass = p.bypass;

        // Footswitches (manual trigger mode), from the A and B buttons or a MIDI-mapped control
        if (p.footA != lastFootA) { footswitch (0, p.footA, p, group, shapeKnob, envSecs); lastFootA = p.footA; }
        if (p.footB != lastFootB) { footswitch (1, p.footB, p, group, shapeKnob, envSecs); lastFootB = p.footB; }

        // Repeat rate follows the knob live
        if (zone == EnvTime::Repeat)
            for (auto& c : chans) c.envelope().setRepeatRate (repeatRate);

        // Envelope shape follows the knob for sounds already playing
        for (auto& c : chans) c.envelope().setShape (shapeKnob.shape);

        const int glitchIdx = std::min (Channel::kGlitchLengths - 1, (int) (clampf (dimNow, 0.0f, 0.999f) * Channel::kGlitchLengths));
        if (group != lastGroup || p.voice != lastVoice)
        {
            for (auto& c : chans) c.ensureContent (group, p.voice, glitchIdx, bass);
            lastGroup = group;
            lastVoice = p.voice;
        }

        float peakIn = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];

            // Bass: everything under ~120 Hz stays dry and untouched
            float lowL = 0.0f, lowR = 0.0f, hiL = inL, hiR = inR;
            if (bass)
            {
                lowL = lows2[0].process (lows[0].process (inL));
                lowR = lows2[1].process (lows[1].process (inR));
                hiL = inL - lowL;
                hiR = inR - lowR;
            }
            const float mono = 0.5f * (hiL + hiR);
            preRoll.push (mono);
            peakIn = std::max (peakIn, std::fabs (mono));

            // ---- triggers ----
            const float hit = detector.process (mono);
            if (learning) learnListen (hit);
            if (! p.bypass && ! learning)
            {
                if (externalNote >= 0)
                {
                    autoTrigger (p, group, shapeKnob, zone, envSecs, repeatRate, externalNote);
                    externalNote = -1;
                }
                else if (hit > 0.0f)
                {
                    autoTrigger (p, group, shapeKnob, zone, envSecs, repeatRate, -1);
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

            // ---- Glitch B memory ----
            if (memPending >= 0) memoryCapture (false);
            float memSample = 0.0f;
            if (group == GroupGlitchB)
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
            if (p.voice == VSwellB) swellSrc = fuzz (mono);

            // ---- the two channels ----
            const int gi = std::min (Channel::kGlitchLengths - 1, (int) (std::min (dim, 0.999f) * Channel::kGlitchLengths));
            const float width = sWidth.next (p.width);
            Stereo wet;
            for (int c = 0; c < 2; ++c)
            {
                auto& ch = chans[c];
                ch.feed (mono, group, p.voice, gi, bass);
                const float e = ch.nextEnv();
                envNow[c] = e;
                if (e <= 1.0e-5f) continue;
                const float ext = group == GroupGlitchB ? memSample : swellSrc;
                const Stereo s = ch.source (group, p.voice, dim, ext) * e;
                const float mid = 0.5f * (s.l + s.r), side = 0.5f * (s.l - s.r) * width;
                const Stereo panned = panMono (1.0f, (c == 0 ? -0.65f : 0.65f) * width);
                wet.l += (mid + side) * panned.l;
                wet.r += (mid - side) * panned.r;
            }

            wet = wet * kWetTrim;

            // ---- stereo space: delay (Blur and Swell), then Air ----
            const float send = sSend.next (sendDelay ? 1.0f : 0.0f);
            wet = spaceDelay (wet, send, dim, longDelay, width);
            wet = airDiffuse (wet, sAir.next (p.air));

            // ---- drive and tone ----
            wet = driveTone (wet, sDrive.next (driveKnob), sTone.next (p.tone), p.autoLevel);

            // ---- mix ----
            const float dg = sDry.next (dryGain);
            const float wg = sWet.next (wetGain);
            const float gate = sDryGate.next (glitchInterrupt ? 1.0f - clampf (envNow[0] + envNow[1], 0.0f, 1.0f) : 1.0f);
            const float master = sMaster.next (dbToGain (p.masterDb));
            L[i] = softClip ((lowL + hiL * dg * gate + wet.l * wg) * master);
            R[i] = softClip ((lowR + hiR * dg * gate + wet.r * wg) * master);
            ++sampleClock;
        }

        uiInput.store (std::max (peakIn, uiInput.load() * 0.85f));
        uiEnvA.store (envNow[0]);
        uiEnvB.store (envNow[1]);
        uiDim.store (dimNow);
        uiSelected.store (memSel < 0 ? 0 : memSel);
    }

    bool channelActive (int c) const { return chans[c].active(); }

private:
    // ---------------------------------------------------------------------------
    EnvRequest makeRequest (const EngineParams& p, int group, const ShapeKnob& sk, EnvTime::Zone zone, float envSecs, float repeatRate, int c)
    {
        EnvRequest r;
        r.shape = sk.shape;
        r.randomRate = sk.rateHz;
        r.duration = group == GroupGlitchB && zone == EnvTime::Timed ? 1.0f : envSecs;
        r.repeatRate = repeatRate;
        if (zone == EnvTime::Repeat)
        {
            r.mode = Envelope::Mode::Repeat;
            const auto& other = chans[1 - c].envelope();
            // Poly: the two channels alternate (half a cycle apart). Mono: in step.
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

    void autoTrigger (const EngineParams& p, int group, const ShapeKnob& sk, EnvTime::Zone zone, float envSecs, float repeatRate, int midiNote)
    {
        if (p.trigger == 2) return; // Manual: the footswitches decide

        envGen.retrigger();

        if (group == GroupGlitchB)
        {
            // Env Time sets how often a note is recorded and played back
            const float chance = zone == EnvTime::Timed ? 0.05f + 0.95f * std::pow (p.envTime / EnvTime::kInfinite, 1.5f) : 1.0f;
            if (rng.uni() > chance) return;
            const int c = nextChannel;
            nextChannel ^= 1;
            startNote (c, p, group, sk, zone, envSecs, repeatRate, midiNote, true);
            if (! p.hold) beginMemoryCapture (c);
            return;
        }

        const int c = nextChannel;
        nextChannel ^= 1;
        if (p.trigger == 1) // Mono: each new note cancels the last
            chans[1 - c].kill (0.12f);
        startNote (c, p, group, sk, zone, envSecs, repeatRate, midiNote, group == GroupSwell);
    }

    void startNote (int c, const EngineParams& p, int group, const ShapeKnob& sk, EnvTime::Zone zone, float envSecs, float repeatRate, int midiNote, bool now)
    {
        const auto req = makeRequest (p, group, sk, zone, envSecs, repeatRate, c);
        const int pre = std::min (preSamples, preRoll.size() - 4);
        for (int k = 0; k < pre; ++k) preCopy[(size_t) k] = preRoll.at (pre - k);
        chans[c].trigger (preCopy.data(), pre, midiNote, req, now);
        ++captures;
        uiCaptures.store (captures);
    }

    void footswitch (int c, bool down, const EngineParams& p, int group, const ShapeKnob& sk, float envSecs)
    {
        if (p.trigger != 2 || p.bypass) return;
        auto& ch = chans[c];
        const double t = (double) sampleClock / fs;

        EnvRequest r;
        r.shape = sk.shape;
        r.randomRate = sk.rateHz;
        r.duration = envSecs;
        r.mode = Envelope::Mode::Sustain;

        auto capture = [&]
        {
            const int pre = std::min (preSamples, preRoll.size() - 4);
            for (int k = 0; k < pre; ++k) preCopy[(size_t) k] = preRoll.at (pre - k);
            const bool now = group == GroupSwell || group == GroupGlitchB;
            if (group == GroupSwell) // Swell's footswitches open the way into the delay, momentarily
            {
                r.shape = ShapeSquare;
                r.duration = 0.08f;
            }
            ch.trigger (preCopy.data(), pre, -1, r, now);
            if (group == GroupGlitchB && ! p.hold) beginMemoryCapture (c);
            envGen.retrigger();
        };

        if (group == GroupSwell || p.fsMode == 2) // Momentary
        {
            if (down) capture();
            else ch.release();
            return;
        }
        if (! down) return;

        if (p.fsMode == 0) // Toggle
        {
            if (ch.envelope().isHolding() || ch.waiting()) ch.release();
            else capture();
        }
        else // Latch: each press grabs a new note, a quick double press clears
        {
            if (t - lastPress[c] < 0.35) ch.release();
            else capture();
            lastPress[c] = t;
        }
    }

    // ---------------------------------------------------------------------------
    // Glitch B memory: six blocks, each a note recorded as a short loop
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
        const int have = ch.rawSamples();
        const int want = (int) (0.3f * fs);
        if (have >= want || (force && have >= (int) (0.08f * fs)) || (! ch.recording() && have > 0))
        {
            const int len = std::min ({ have, want, memCap });
            std::copy (ch.rawData(), ch.rawData() + len, memory[memTarget].begin());
            memLen[memTarget] = len;
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
        uiFilled[b].store (memLen[b] > 0);
        for (int k = 0; k < kOverview; ++k)
        {
            float pk = 0.0f;
            const int s0 = memLen[b] * k / kOverview, s1 = memLen[b] * (k + 1) / kOverview;
            for (int i = s0; i < s1; ++i) pk = std::max (pk, std::fabs (memory[b][(size_t) i]));
            uiOverview[b][k].store (std::min (1.0f, pk * 2.5f));
        }
    }

    void switchMemory (int sel)
    {
        if (memPlay.active()) { memOld = memPlay; memOld.stop (0.02f * fs); }
        memPlay = LoopReader();
        memSel = sel;
        memPlayedVersion = memVersion[sel];
        if (memLen[sel] > 32)
            memPlay.start (memory[sel].data(), memLen[sel], (int) (0.008f * fs), 0.02f * fs);
    }

    void pollRequests()
    {
        if (clearRequest.exchange (false))
        {
            for (int b = 0; b < kBlocks; ++b) { memLen[b] = 0; ++memVersion[b]; publishBlock (b); }
            memPending = -1;
            memLast = -1;
            memPlay.stop (0.02f * fs);
        }
        if (loadReady.load())
        {
            for (int b = 0; b < kBlocks; ++b)
            {
                // Resample if the set was saved at another sample rate
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
            learnSum = 0.0f;
            learnCount = 0;
            learnWindow = 0;
            uiLearnResult.store (-1.0f);
        }
        uiLearning.store (learning);
    }

    // ---------------------------------------------------------------------------
    // Learn: listens for four seconds and sets the sensitivity to suit how you play
    void learnListen (float hit)
    {
        if (hit > 0.0f) { learnWindow = (int) (0.2f * fs); learnPeak = 0.0f; }
        if (learnWindow > 0)
        {
            learnPeak = std::max (learnPeak, detector.level());
            if (--learnWindow == 0 && learnPeak > 1.0e-5f)
            {
                learnSum += 20.0f * std::log10 (learnPeak);
                ++learnCount;
            }
        }
        if (--learnLeft <= 0)
        {
            learning = false;
            if (learnCount > 0)
            {
                const float thresholdDb = learnSum / (float) learnCount - 18.0f;
                uiLearnResult.store (clampf ((-16.0f - thresholdDb) / 50.0f, 0.0f, 1.0f));
            }
            else
            {
                uiLearnResult.store (-2.0f); // heard nothing
            }
        }
    }

    // ---------------------------------------------------------------------------
    float fuzz (float x)
    {
        // Violin-like fuzz: hard drive, 2x oversampled, darkened, and gated so silence stays silent
        const float mid = 0.5f * (fuzzPrev + x);
        fuzzPrev = x;
        auto shape = [] (float v) { return std::tanh (12.0f * v + 0.2f) - 0.19738f; };
        float y = 0.5f * (shape (mid) + shape (x));
        y = fuzzLp[1].process (fuzzLp[0].process (y));
        const float lvl = detector.level();
        const float gate = sSwellGate.next (clampf ((20.0f * std::log10 (lvl + 1.0e-9f) + 64.0f) / 14.0f, 0.0f, 1.0f));
        return y * 0.17f * gate;
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
        if ((sampleClock & 15) == 0) { dlDamp[0].setCutoff (cut, fs); dlDamp[1].setCutoff (cut, fs); }

        const float tL = ms * 0.001f * fs * drift;
        const float tR = ms * 0.001f * fs * (1.0f + 0.13f * width) / drift;
        const float yL = dl[0].tap (tL), yR = dl[1].tap (tR);
        const float cross = 0.22f * width;
        dl[0].push (softClip (in.l * send + dlDamp[0].process (yL * (1.0f - cross) + yR * cross) * fb));
        dl[1].push (softClip (in.r * send + dlDamp[1].process (yR * (1.0f - cross) + yL * cross) * fb));
        const float amt = 0.25f + 0.35f * dim;
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

    Stereo driveTone (Stereo in, float drive, float tone, bool autoLevel)
    {
        const float g = 1.0f + 24.0f * drive * drive;
        const float comp = sComp.next (autoLevel ? 0.3f / std::tanh (0.3f * g) : 1.0f);
        auto shape = [g] (float v) { return std::tanh (g * v); };
        float out[2] = { in.l, in.r };
        for (int c = 0; c < 2; ++c)
        {
            if (drive > 0.001f)
            {
                const float mid = 0.5f * (drivePrev[c] + out[c]); // 2x oversampled
                drivePrev[c] = out[c];
                out[c] = 0.5f * (shape (mid) + shape (out[c])) * comp;
            }
            if (tone < 0.995f)
            {
                if ((sampleClock & 15) == 0) toneLp[c].setCutoff (1500.0f * std::pow (13.0f, tone), fs);
                out[c] = toneLp[c].process (out[c]);
            }
            else
            {
                toneLp[c].z = out[c];
            }
        }
        return { out[0], out[1] };
    }

    // ---------------------------------------------------------------------------
    float fs = 48000.0f;
    Detector detector;
    Channel chans[2];
    Lfo lfo, envGen;
    Rng rng;
    DelayLine preRoll;
    std::vector<float> preCopy;
    int preSamples = 192;
    int nextChannel = 0;
    float envNow[2] = { 0.0f, 0.0f };
    float dimNow = 0.5f;
    int lastGroup = -1, lastVoice = -1;
    bool lastFootA = false, lastFootB = false, lastBypass = false;
    double lastPress[2] = { -10.0, -10.0 };
    int64_t sampleClock = 0;
    int externalNote = -1;
    float externalVel = 0.0f;
    int captures = 0;

    // Glitch B memory
    std::array<std::vector<float>, kBlocks> memory;
    int memLen[kBlocks] = {};
    uint32_t memVersion[kBlocks] = {};
    uint32_t memPlayedVersion = 0;
    int memCap = 0, memSel = -1, memPending = -1, memTarget = 0, memLast = -1;
    LoopReader memPlay, memOld;
    std::atomic<bool> clearRequest { false }, loadReady { false }, learnRequest { false };
    std::array<std::vector<float>, kBlocks> loadData;
    float loadRate = 48000.0f;

    // Learn
    bool learning = false;
    int learnLeft = 0, learnCount = 0, learnWindow = 0;
    float learnSum = 0.0f, learnPeak = 0.0f;

    // Space
    DelayLine dl[2];
    OnePole dlDamp[2];
    DelayLine fdn[4], ap[4];
    float fdnLen[4] = {}, apLen[4] = {};
    OnePole fdnDamp[4];
    float driftPhase = 0.0f;

    // Output
    OnePole toneLp[2], lows[2], lows2[2], fuzzLp[2];
    float drivePrev[2] = { 0.0f, 0.0f }, fuzzPrev = 0.0f;
    Smoother sDim, sDrive, sTone, sDry, sWet, sMaster, sWidth, sAir, sSend, sDryGate, sComp, sDelay, sSwellGate;
};
} // namespace jet

#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;
using namespace jet;

// Several Jetstream copies can be open at once. The footswitches drive one of them: the
// one whose panel was last touched, or the next one along when C and D are tapped together.
namespace
{
SpinLock focusLock;
Array<JetstreamProcessor*> instances;
std::atomic<JetstreamProcessor*> focused { nullptr };

bool hasFocusPtr (const JetstreamProcessor* p)
{
    auto* f = focused.load();
    return f == nullptr || f == p;
}

thread_local bool fromDeviceNow = false;

void sendToFocused (const MidiMessage& m)
{
    const SpinLock::ScopedLockType l (focusLock);
    JetstreamProcessor* target = nullptr;
    for (auto* p : instances) if (p->isLearning()) target = p;
    if (target == nullptr) target = focused.load();
    if (target == nullptr && ! instances.isEmpty()) target = instances.getLast();
    if (target != nullptr) target->handleDirectMidi (m);
}

double nowSeconds() { return Time::getMillisecondCounterHiRes() * 0.001; }

constexpr int kTimerHz = 100;
constexpr size_t kRecordingLength = 10 * kTimerHz; // knob recordings run up to 10 seconds

bool isPerformance (const String& id) { return isPerformanceParam (id.toStdString()); }
} // namespace

JetstreamProcessor::JetstreamProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "JETSTREAM", createLayout())
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            raw[rp->getParameterID()] = apvts.getRawParameterValue (rp->getParameterID());
    for (auto& s : paramSpecs())
    {
        specRaw.push_back (apvts.getRawParameterValue (s.id));
        apvts.addParameterListener (s.id, &editWatch);
    }
    recs[RecDimension].paramId = "dimension";
    recs[RecDrive].paramId = "drive";

    settings.load();
    settingsStamp = PedalSettings::file().getLastModificationTime();
    direct->setSink (&sendToFocused);
    direct->setWanted (settings.footDevice);
    footswitches.onEvent = [this] (int sw, FootswitchLogic::Event e) { onFootEvent (sw, e); };
    loadUserPresets();
    {
        const SpinLock::ScopedLockType l (focusLock);
        instances.add (this);
    }
    focused.store (this);
    loadPreset (0);
    setLatencySamples (JetEngine::latencySamples());
    startTimerHz (kTimerHz);
}

JetstreamProcessor::~JetstreamProcessor()
{
    stopTimer();
    for (auto& s : paramSpecs()) apvts.removeParameterListener (s.id, &editWatch);
    const SpinLock::ScopedLockType l (focusLock);
    instances.removeAllInstancesOf (this);
    if (focused.load() == this) focused.store (instances.isEmpty() ? nullptr : instances.getLast());
}

void JetstreamProcessor::EditWatch::parameterChanged (const String& id, float)
{
    if (! owner.applyingPreset.load() && ! isPerformance (id)) owner.edited.store (true);
}

bool JetstreamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != AudioChannelSet::stereo()) return false;
    return in == AudioChannelSet::stereo() || in == AudioChannelSet::mono();
}

void JetstreamProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate);
    keeper.prepare (sampleRate);
    inCopy.setSize (2, jmax (1, samplesPerBlock));
    footBits.store (0);
    setLatencySamples (JetEngine::latencySamples());
}

bool JetstreamProcessor::hasFootFocus() const { return hasFocusPtr (this); }

//==============================================================================
// Audio
//==============================================================================
EngineParams JetstreamProcessor::readParams (int numSamples)
{
    EngineParams p;
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i) setParam (p, specs[i].id, specRaw[i]->load (std::memory_order_relaxed));

    // the expression pedal moves its target between the preset's heel and toe settings
    const int expTarget = jlimit (0, ExpCount - 1, (int) std::lround (get ("expTarget")));
    float pedal = get ("exp");
    const bool expActive = get ("expOn") > 0.5f;
    if (expActive)
    {
        // smoothing: off, a little (30 ms) or a lot (150 ms), so a stepped pedal never zips
        static const float times[3] = { 0.0f, 0.03f, 0.15f };
        const float t = times[jlimit (0, 2, settings.expSmooth)];
        const float a = t <= 0.0f ? 1.0f : 1.0f - std::exp (-(float) numSamples / (t * (float) currentSampleRate));
        expSmoothed += (pedal - expSmoothed) * a;
        pedal = expSmoothed;
        applyExpression (p, expTarget, get ("expLo"), get ("expHi"), pedal);
    }
    else expSmoothed = pedal;
    uiExp.store (pedal);

    p.dimOverride = recs[RecDimension].value.load();
    p.driveOverride = recs[RecDrive].value.load();

    // Free running like the pedal, or locked to the song's tempo
    float bpm = 0.0f;
    if (get ("sync") > 0.5f)
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm())
                    bpm = (float) *b;
    uiBpm.store (bpm);
    if (bpm > 0.0f)
    {
        const float beatHz = bpm / 60.0f;
        const float rateKnob = expActive && expTarget == ExpLfoRate ? expressionValue (get ("expLo"), get ("expHi"), pedal) : get ("lfoRate");
        p.lfoRateHz = beatHz / kDivisionBeats[divisionIndex (rateKnob)];
        // Repeat: where Env Shape sits between two shapes picks the note length
        const auto sk = ShapeKnob::from (p.envShape);
        const float offset = std::log2 (sk.rateHz / 0.25f) / 6.0f;
        p.repeatRateHz = beatHz / kDivisionBeats[divisionIndex (offset)];
    }
    else p.repeatRateHz = -1.0f;
    return p;
}

void JetstreamProcessor::setEngineFoot (int c, bool down)
{
    if (down) footBits.fetch_or (1 << c);
    else footBits.fetch_and (~(1 << c));
}

void JetstreamProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0 || getTotalNumOutputChannels() < 2) return;
    if (getTotalNumInputChannels() == 1) buffer.copyFrom (1, 0, buffer, 0, 0, n); // mono in feeds both sides

    // footswitches A and B, from the panel, the pedals or the original MIDI map
    const int bits = footBits.load();
    for (int c = 0; c < 2; ++c) engine.footswitch (c, (bits >> c) & 1);
    const int cap = captureBits.exchange (0), can = cancelBits.exchange (0);
    for (int c = 0; c < 2; ++c)
    {
        if (can & (1 << c)) engine.cancelCapture (c);
        if (cap & (1 << c)) engine.captureNow (c);
    }

    const auto p = readParams (n);
    if (inCopy.getNumSamples() < n) inCopy.setSize (2, n, false, false, true);
    inCopy.copyFrom (0, 0, buffer, 0, 0, n);
    inCopy.copyFrom (1, 0, buffer, 1, 0, n);

    // Process in pieces between MIDI events so MIDI-triggered notes land on time
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = jlimit (0, n, meta.samplePosition);
        if (at > pos) { engine.process (L + pos, R + pos, at - pos, p); pos = at; }
        handleMidi (meta.getMessage(), false);
    }
    if (pos < n) engine.process (L + pos, R + pos, n - pos, p);
    midi.clear();

    keeper.pushInput (inCopy.getReadPointer (0), inCopy.getReadPointer (1), n);
    keeper.pushOutput (L, R, n);
}

//==============================================================================
// MIDI: footswitches and expression first, then notes, then the original pedal map
//==============================================================================
void JetstreamProcessor::queue (int kind, int a)
{
    auto& f = fromDeviceNow ? directFifo : fifo;
    auto& buf = fromDeviceNow ? directPending : pending;
    const auto scope = f.write (1);
    if (scope.blockSize1 > 0) buf[(size_t) scope.startIndex1] = { kind, a };
}

void JetstreamProcessor::handleDirectMidi (const MidiMessage& m)
{
    const SpinLock::ScopedLockType l (directWriteLock);
    fromDeviceNow = true;
    handleMidi (m, true);
    fromDeviceNow = false;
}

void JetstreamProcessor::handleMidi (const MidiMessage& m, bool fromDevice)
{
    // notes from a track trigger the samplers with exact pitch
    if (m.isNoteOn() && ! fromDevice) { engine.noteOn (m.getNoteNumber()); return; }
    if (! m.isController()) return;
    const int ch = m.getChannel(), cc = m.getControllerNumber(), val = m.getControllerValue();

    if (learnTarget.load() != LearnNone)
    {
        if (val > 0) queue (Learned, ch * 128 + cc);
        return;
    }

    // Straight from the footswitch device any channel will do. Through Ableton the channel
    // must match, and when the device is read directly the copy through Ableton is ignored.
    if (fromDevice || ! pedalsDirect())
    {
        int foot = -1;
        for (int k = 0; k < 4 && foot < 0; ++k)
            if ((fromDevice || ch == settings.fsChannel[(size_t) k]) && cc == settings.fsCC[(size_t) k]) foot = k;
        const bool keep = foot < 0 && settings.keeperCC >= 0 && (fromDevice || ch == settings.keeperChannel) && cc == settings.keeperCC;
        const bool expression = foot < 0 && ! keep && (fromDevice || ch == settings.expChannel) && cc == settings.expCC;
        if (foot >= 0 || keep || expression)
        {
            if (! fromDevice)
            {
                hostFeedsPedals.store (true);
                const SpinLock::ScopedLockType l (focusLock);
                auto* f = focused.load();
                if (f != this && (f == nullptr || ! f->hostFeedsPedals.load())) focused.store (this);
                if (focused.load() != this) return;
            }
            if (foot >= 0) queue (val >= 64 ? FootPress : FootRelease, foot);
            else if (keep) { if (val >= 64) queue (KeepCC, 0); }
            else queue (Expression, val);
            return;
        }
    }

    // The original v0.1 chart, CC 80 to 83, only on the channel chosen on the Pedal page
    if (! fromDevice && settings.pedalMapChannel > 0 && ch == settings.pedalMapChannel)
    {
        if (cc == 80 || cc == 81) setEngineFoot (cc - 80, val >= 64);
        else if (cc == 82 && val >= 64) queue (MapBypass, 0);
        else if (cc == 83 && val >= 64) queue (MapHold, 0);
    }
}

//==============================================================================
// Panel helpers
//==============================================================================
void JetstreamProcessor::setNorm (const String& id, float v01)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v01));
        p->endChangeGesture();
    }
}

void JetstreamProcessor::setPlain (const String& id, float plain)
{
    if (auto* p = dynamic_cast<RangedAudioParameter*> (apvts.getParameter (id)))
        setNorm (id, p->convertTo0to1 (plain));
}

void JetstreamProcessor::togglePlain (const String& id) { setPlain (id, get (id.toRawUTF8()) > 0.5f ? 0.0f : 1.0f); }

void JetstreamProcessor::say (const String& s)
{
    uiMessage = s;
    uiMessageAt = nowSeconds();
}

void JetstreamProcessor::touchPanel() { focused.store (this); }

String JetstreamProcessor::expressionRangeText (bool toe) const
{
    const int t = jlimit (0, ExpCount - 1, (int) std::lround (get ("expTarget")));
    return valueText (expTargetParam (t), get (toe ? "expHi" : "expLo"));
}

int JetstreamProcessor::learnState() const
{
    if (engine.uiLearning.load()) return 1;
    if (nowSeconds() < learnFlashUntil) return learnFlash;
    return 0;
}

void JetstreamProcessor::timerCallback()
{
    const double now = nowSeconds();
    if (now >= nextSettingsCheck)
    {
        // settings changed by another copy or another Flip Fella plugin
        nextSettingsCheck = now + 1.0;
        const auto stamp = PedalSettings::file().getLastModificationTime();
        if (stamp != settingsStamp && learnTarget.load() == LearnNone && ! calibrating)
        {
            settingsStamp = stamp;
            settings.load();
            direct->setWanted (settings.footDevice);
        }
        const bool d = pedalsDirect();
        if (d != wasDirect)
        {
            wasDirect = d;
            if (d && hasFootFocus()) say ("FOOTSWITCHES READY");
        }
    }

    auto apply = [&] (const std::array<Pending, 512>& from, int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            const auto& q = from[(size_t) i];
            switch (q.kind)
            {
                case FootPress: footswitches.press (q.a, now); break;
                case FootRelease: footswitches.release (q.a, now); break;
                case KeepCC: keepNow(); break;
                case MapBypass: togglePlain ("bypass"); break;
                case MapHold: togglePlain ("hold"); break;
                case Expression:
                {
                    expMovedAt = now;
                    if (calibrating)
                    {
                        if (calFirst < 0) calFirst = q.a;
                        calLo = jmin (calLo, q.a);
                        calHi = jmax (calHi, q.a);
                        break;
                    }
                    if (get ("expOn") < 0.5f) setPlain ("expOn", 1.0f);
                    setPlain ("exp", settings.expressionValue (q.a));
                    break;
                }
                case Learned:
                {
                    const int ch = q.a / 128, cc = q.a % 128;
                    const int what = learnTarget.exchange (LearnNone);
                    if (what >= 0 && what < 4)
                    {
                        settings.fsChannel[(size_t) what] = ch;
                        settings.fsCC[(size_t) what] = cc;
                        say ("FOOTSWITCH " + String::charToString ((juce_wchar) ('A' + what)) + " IS CC " + String (cc));
                    }
                    else if (what == LearnKeeper) { settings.keeperChannel = ch; settings.keeperCC = cc; say ("KEEPER IS CC " + String (cc)); }
                    else if (what == LearnExpression) { settings.expChannel = ch; settings.expCC = cc; say ("EXPRESSION IS CC " + String (cc)); }
                    saveSettings();
                    break;
                }
                default: break;
            }
        }
    };
    {
        const auto scope = fifo.read (fifo.getNumReady());
        apply (pending, scope.startIndex1, scope.blockSize1);
        apply (pending, scope.startIndex2, scope.blockSize2);
    }
    {
        const auto scope = directFifo.read (directFifo.getNumReady());
        apply (directPending, scope.startIndex1, scope.blockSize1);
        apply (directPending, scope.startIndex2, scope.blockSize2);
    }

    footswitches.tick (now);

    // a two-switch tap acts once both feet are off, so holding on for a second can do something else
    if (comboABPending && ! footswitches.isDown (0) && ! footswitches.isDown (1))
    {
        comboABPending = false;
        togglePlain ("bypass");
        say (get ("bypass") > 0.5f ? (get ("tails") > 0.5f ? "BYPASSED, TAILS RING ON" : "BYPASSED") : "ON");
    }
    if (comboCDPending && ! footswitches.isDown (2) && ! footswitches.isDown (3))
    {
        comboCDPending = false;
        const SpinLock::ScopedLockType l (focusLock);
        const int i = instances.indexOf (this);
        auto* next = instances.isEmpty() ? nullptr : instances[(i + 1) % instances.size()];
        focused.store (next);
        if (next != nullptr) next->say (instances.size() > 1 ? "FOOTSWITCHES ARE HERE" : "ONLY ONE JETSTREAM OPEN");
    }

    int mask = 0;
    for (int k = 0; k < 4; ++k) if (footswitches.isDown (k)) mask |= 1 << k;
    uiFootDown.store (mask);

    if (calibrating && now > calibrateUntil)
    {
        calibrating = false;
        if (calHi - calLo > 20)
        {
            const bool startedLow = std::abs (calFirst - calLo) < std::abs (calFirst - calHi);
            settings.heel = startedLow ? calLo : calHi;
            settings.toe = startedLow ? calHi : calLo;
            saveSettings();
            say ("EXPRESSION CALIBRATED");
        }
        else say ("NO MOVEMENT, TRY AGAIN");
    }

    // sensitivity Learn finished
    const float learned = engine.uiLearnResult.exchange (-1.0f);
    if (learned >= 0.0f)
    {
        setPlain ("sensitivity", learned);
        learnFlash = 2;
        learnFlashUntil = now + 1.5;
        say ("SENSITIVITY SET TO " + String (roundToInt (learned * 100.0f)) + "%");
    }
    else if (learned < -1.5f)
    {
        learnFlash = 3;
        learnFlashUntil = now + 1.5;
        say ("HEARD NOTHING, TRY AGAIN");
    }

    // Knob recordings: up to 10 seconds, looped. Moving the knob again stops playback.
    for (auto& r : recs)
    {
        const float knob = get (r.paramId);
        if (r.state == RecRecording)
        {
            r.curve.push_back (knob);
            if (r.curve.size() >= kRecordingLength) { r.state = RecPlaying; r.play = 0; r.lastKnob = knob; }
        }
        else if (r.state == RecPlaying)
        {
            if (std::abs (knob - r.lastKnob) > 0.004f) { r.state = RecIdle; r.value.store (-1.0f); continue; }
            r.value.store (r.curve[r.play]);
            if (++r.play >= r.curve.size()) r.play = 0;
        }
    }
}

void JetstreamProcessor::toggleRecording (int target)
{
    auto& r = recs[(size_t) target];
    if (r.state == RecIdle) { r.curve.clear(); r.state = RecRecording; r.value.store (-1.0f); }
    else if (r.state == RecRecording)
    {
        r.state = r.curve.size() > 6 ? RecPlaying : RecIdle;
        r.play = 0;
        r.lastKnob = get (r.paramId);
    }
    else { r.state = RecIdle; r.value.store (-1.0f); }
}

float JetstreamProcessor::recordingProgress (int target) const
{
    const auto& r = recs[(size_t) target];
    if (r.state == RecRecording) return (float) r.curve.size() / (float) kRecordingLength;
    if (r.state == RecPlaying && ! r.curve.empty()) return (float) r.play / (float) r.curve.size();
    return 0.0f;
}

//==============================================================================
// Footswitches
//   A  tap: Sampler A (in Poly and Mono, catches what's ringing)   hold: sustain A
//   B  tap: Sampler B                                               hold: sustain B
//   C  tap: Hold Memory on and off                                  hold: bypass
//   D  tap: next preset                                             hold: previous preset
//   A+B tap: bypass
//   C+D tap: footswitches to the next Jetstream   held 1 s: Keeper
// In Manual, A and B are the pedal's own footswitches: Toggle, Latch and Momentary as set.
//==============================================================================
String JetstreamProcessor::footJobTap (int sw) const
{
    const bool manual = (int) std::lround (get ("trigger")) == 2;
    switch (sw)
    {
        case 0: return manual ? "SAMPLER A" : "CATCH A";
        case 1: return manual ? "SAMPLER B" : "CATCH B";
        case 2: return "HOLD MEMORY";
        case 3: return "NEXT PRESET";
        default: return {};
    }
}

String JetstreamProcessor::footJobHold (int sw) const
{
    const bool manual = (int) std::lround (get ("trigger")) == 2;
    const int fs = (int) std::lround (get ("fsMode"));
    switch (sw)
    {
        case 0: return manual ? (fs == 2 ? "PLAYS WHILE HELD" : "SAMPLER A") : "SUSTAIN A";
        case 1: return manual ? (fs == 2 ? "PLAYS WHILE HELD" : "SAMPLER B") : "SUSTAIN B";
        case 2: return "BYPASS";
        case 3: return "PREVIOUS PRESET";
        default: return {};
    }
}

void JetstreamProcessor::onFootEvent (int sw, FootswitchLogic::Event e)
{
    using E = FootswitchLogic;
    if (e == E::Down || e == E::ComboAB || e == E::ComboCD) uiFootPulse.fetch_add (1);
    const bool manual = (int) std::lround (get ("trigger")) == 2;

    switch (e)
    {
        case E::ComboAB:
            // the first switch's press already reached the engine: undo it, it was half of a pair
            for (int c = 0; c < 2; ++c)
                if (downFired[c])
                {
                    setEngineFoot (c, false);
                    if (manual) cancelBits.fetch_or (1 << c);
                    downFired[c] = false;
                }
            comboABPending = true;
            return;
        case E::ComboHoldAB: return; // a tap acts on release, so a long press does nothing extra
        case E::ComboCD: comboCDPending = true; return;
        case E::ComboHoldCD:
            comboCDPending = false;
            focused.store (this);
            keepNow();
            return;
        default: break;
    }

    switch (sw)
    {
        case 0:
        case 1:
        {
            const String name = sw == 0 ? "A" : "B";
            if (e == E::Down) { setEngineFoot (sw, true); downFired[sw] = true; }
            if (e == E::Tap)
            {
                setEngineFoot (sw, false);
                downFired[sw] = false;
                if (! manual) { captureBits.fetch_or (1 << sw); say ("CAUGHT " + name); }
            }
            if (e == E::HoldStart && ! manual) say ("SUSTAIN " + name + ": LET GO TO RELEASE");
            if (e == E::HoldEnd) { setEngineFoot (sw, false); downFired[sw] = false; }
            break;
        }
        case 2:
            if (e == E::Tap)
            {
                togglePlain ("hold");
                say (get ("hold") > 0.5f ? "MEMORY HELD: GLITCH B PLAYS IT BACK" : "MEMORY RECORDING AGAIN");
            }
            if (e == E::HoldStart)
            {
                togglePlain ("bypass");
                say (get ("bypass") > 0.5f ? "BYPASSED" : "ON");
            }
            break;
        case 3:
            if (e == E::Tap) stepPreset (1);
            if (e == E::HoldStart) stepPreset (-1);
            break;
        default: break;
    }
}

void JetstreamProcessor::footDown (int sw)
{
    focused.store (this);
    footswitches.press (sw, nowSeconds());
}

void JetstreamProcessor::footUp (int sw) { footswitches.release (sw, nowSeconds()); }

void JetstreamProcessor::setFootDevice (const String& device)
{
    settings.footDevice = device;
    saveSettings();
    direct->setWanted (device);
}

void JetstreamProcessor::setPedalMapChannel (int ch)
{
    settings.pedalMapChannel = jlimit (0, 16, ch);
    saveSettings();
    say (ch == 0 ? "ORIGINAL MIDI MAP OFF" : "ORIGINAL MIDI MAP ON CHANNEL " + String (ch));
}

void JetstreamProcessor::saveSettings()
{
    settings.save();
    settingsStamp = PedalSettings::file().getLastModificationTime();
}

void JetstreamProcessor::learnFootswitch (int sw) { learnTarget.store (sw); say ("PRESS FOOTSWITCH " + String::charToString ((juce_wchar) ('A' + sw))); }
void JetstreamProcessor::learnExpression() { learnTarget.store (LearnExpression); say ("MOVE THE EXPRESSION PEDAL"); }
void JetstreamProcessor::learnKeeper() { learnTarget.store (LearnKeeper); say ("PRESS YOUR KEEPER SWITCH"); }

void JetstreamProcessor::calibrateExpression()
{
    calibrating = true;
    calibrateUntil = nowSeconds() + 6.0;
    calLo = 127; calHi = 0; calFirst = -1;
    say ("HEEL DOWN, THEN ROCK TO TOE");
}

void JetstreamProcessor::resetFootswitches()
{
    settings.resetFootswitches();
    settings.expChannel = 16; settings.expCC = 11;
    settings.heel = 0; settings.toe = 127; settings.curve = 0; settings.expSmooth = 1;
    saveSettings();
    say ("FOOTSWITCH DEFAULTS RESTORED");
}

//==============================================================================
// Presets
//==============================================================================
File JetstreamProcessor::userPresetFolder()
{
    return File::getSpecialLocation (File::userDocumentsDirectory)
        .getChildFile ("Flip Fella Presets").getChildFile ("Jetstream");
}

void JetstreamProcessor::loadUserPresets()
{
    userPresets.clear();
    auto files = userPresetFolder().findChildFiles (File::findFiles, false, "*.xml");
    files.sort();
    userPresets.addArray (files);
}

String JetstreamProcessor::presetName (int i) const
{
    const auto& f = factoryPresets();
    if (i >= 0 && i < (int) f.size()) return f[(size_t) i].name;
    const int u = i - (int) f.size();
    return isPositiveAndBelow (u, userPresets.size()) ? userPresets[u].getFileNameWithoutExtension().toUpperCase() : String ("INIT");
}

String JetstreamProcessor::presetHint (int i) const
{
    const auto& f = factoryPresets();
    if (i >= 0 && i < (int) f.size()) return f[(size_t) i].hint;
    return "Your own preset.";
}

void JetstreamProcessor::loadPreset (int i)
{
    const int count = numPresets();
    if (count == 0) return;
    i = ((i % count) + count) % count;
    applyingPreset = true;
    const auto& f = factoryPresets();
    if (i < (int) f.size())
    {
        for (auto& kv : presetValues (f[(size_t) i])) setPlain (kv.first, kv.second);
    }
    else if (auto xml = parseXML (userPresets[i - (int) f.size()]))
    {
        for (auto& s : paramSpecs())
            if (! isPerformance (s.id) && xml->hasAttribute (s.id))
                setPlain (s.id, (float) xml->getDoubleAttribute (s.id));
    }
    applyingPreset = false;
    preset = i;
    edited = false;
    uiPresetPulse.fetch_add (1);
    say (String (i + 1).paddedLeft ('0', 2) + " " + presetName (i));
}

void JetstreamProcessor::stepPreset (int direction) { loadPreset (preset + direction); }

void JetstreamProcessor::saveUserPreset()
{
    auto folder = userPresetFolder();
    folder.createDirectory();
    int n = 1;
    while (folder.getChildFile ("USER " + String (n).paddedLeft ('0', 2) + ".xml").exists()) ++n;
    const auto file = folder.getChildFile ("USER " + String (n).paddedLeft ('0', 2) + ".xml");
    XmlElement xml ("JetstreamPreset");
    for (auto& s : paramSpecs())
        if (! isPerformance (s.id)) xml.setAttribute (s.id, get (s.id));
    if (xml.writeTo (file))
    {
        loadUserPresets();
        preset = (int) factoryPresets().size() + userPresets.indexOf (file);
        edited = false;
        uiPresetPulse.fetch_add (1);
        say ("SAVED AS " + file.getFileNameWithoutExtension());
    }
    else say ("COULDN'T SAVE THE PRESET");
}

// User A and B: two snapshots of the sound, kept with the set
ValueTree JetstreamProcessor::snapshot() const
{
    ValueTree t ("Snapshot");
    for (auto& s : paramSpecs())
        if (! isPerformance (s.id)) t.setProperty (s.id, get (s.id), nullptr);
    return t;
}

void JetstreamProcessor::applySnapshot (const ValueTree& t)
{
    applyingPreset = true;
    for (auto& s : paramSpecs())
        if (! isPerformance (s.id) && t.hasProperty (s.id))
            setPlain (s.id, (float) t.getProperty (s.id));
    applyingPreset = false;
    edited = true;
    uiPresetPulse.fetch_add (1);
}

void JetstreamProcessor::recallUser (int slot)
{
    slot = jlimit (0, 1, slot);
    if (! userSlots[slot].isValid()) { say (String ("USER ") + (slot == 0 ? "A" : "B") + " IS EMPTY: HOLD TO STORE"); return; }
    applySnapshot (userSlots[slot]);
    say (String ("USER ") + (slot == 0 ? "A" : "B"));
}

void JetstreamProcessor::storeUser (int slot)
{
    slot = jlimit (0, 1, slot);
    userSlots[slot] = snapshot();
    say (String ("STORED IN USER ") + (slot == 0 ? "A" : "B"));
}

bool JetstreamProcessor::hasUser (int slot) const { return userSlots[jlimit (0, 1, slot)].isValid(); }

//==============================================================================
// Keeper
//==============================================================================
void JetstreamProcessor::keepNow()
{
    DynamicObject::Ptr e = new DynamicObject();
    e->setProperty ("plugin", "Jetstream");
   #ifdef JucePlugin_VersionString
    e->setProperty ("version", JucePlugin_VersionString);
   #endif
    e->setProperty ("recreates", "Hologram Electronics Infinite Jets");
    e->setProperty ("time", Time::getCurrentTime().toISO8601 (true));
    e->setProperty ("context", settings.keeperLive ? "Live" : "Studio");
    e->setProperty ("preset", String (preset + 1).paddedLeft ('0', 2) + " " + presetName (preset) + (edited.load() ? " (edited)" : ""));
    e->setProperty ("tempo", uiBpm.load());
    e->setProperty ("voice", voiceNames()[(size_t) jlimit (0, kNumVoices - 1, (int) std::lround (get ("voice")))]);

    DynamicObject::Ptr exp = new DynamicObject();
    const auto& tn = expTargetNames();
    exp->setProperty ("target", tn[(size_t) jlimit (0, (int) tn.size() - 1, (int) std::lround (get ("expTarget")))]);
    exp->setProperty ("active", get ("expOn") > 0.5f);
    exp->setProperty ("position", uiExp.load());
    exp->setProperty ("movedSecondsAgo", nowSeconds() - expMovedAt);
    e->setProperty ("expression", var (exp.get()));

    int filled = 0;
    for (int b = 0; b < JetEngine::kBlocks; ++b) filled += engine.uiFilled[b].load() ? 1 : 0;
    e->setProperty ("memoryCellsFilled", filled);
    e->setProperty ("samplersPlaying", engine.uiCaptures.load());

    DynamicObject::Ptr prm = new DynamicObject();
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
        {
            DynamicObject::Ptr pv = new DynamicObject();
            pv->setProperty ("value", rp->convertFrom0to1 (rp->getValue()));
            pv->setProperty ("shown", rp->getCurrentValueAsText());
            prm->setProperty (rp->getParameterID(), var (pv.get()));
        }
    e->setProperty ("parameters", var (prm.get()));

    keeper.keep (e);
    uiKeepPulse.fetch_add (1);
    say ("KEPT");
}

File JetstreamProcessor::exportKeepers()
{
    const auto f = keeper.exportZip();
    if (f.existsAsFile()) { say ("EXPORTED TO DOCUMENTS"); f.revealToUser(); }
    else say ("NOTHING KEPT YET");
    return f;
}

void JetstreamProcessor::setKeeperLive (bool live)
{
    settings.keeperLive = live;
    saveSettings();
}

//==============================================================================
// State: every parameter, the preset, User A and B, the Glitch B memory (with the audio
// that led into each cell, so its loops close cleanly) and the knob recordings
//==============================================================================
void JetstreamProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto type = state.getChild (i).getType().toString();
        if (type == "Memory" || type == "Recordings" || type == "UserSlots" || type == "Snapshots") state.removeChild (i, nullptr);
    }
    state.setProperty ("preset", preset, nullptr);
    state.setProperty ("presetEdited", edited.load(), nullptr);

    ValueTree mem ("Memory");
    mem.setProperty ("rate", engine.sampleRate(), nullptr);
    for (int b = 0; b < JetEngine::kBlocks; ++b)
    {
        ValueTree blk ("Block");
        const int len = engine.memoryLength (b);
        if (len > 0)
        {
            MemoryBlock data ((size_t) len * sizeof (int16));
            auto* out = static_cast<int16*> (data.getData());
            const float* in = engine.memoryData (b);
            for (int i = 0; i < len; ++i) out[i] = (int16) jlimit (-32767, 32767, roundToInt (in[i] * 32767.0f));
            blk.setProperty ("data", data.toBase64Encoding(), nullptr);
            blk.setProperty ("preRoll", engine.memoryPreRoll (b), nullptr);
        }
        mem.appendChild (blk, nullptr);
    }
    state.appendChild (mem, nullptr);

    ValueTree slots ("UserSlots");
    for (int s = 0; s < 2; ++s)
    {
        ValueTree c = userSlots[s].isValid() ? userSlots[s].createCopy() : ValueTree ("Snapshot");
        c.setProperty ("empty", ! userSlots[s].isValid(), nullptr);
        slots.appendChild (c, nullptr);
    }
    state.appendChild (slots, nullptr);

    ValueTree recTree ("Recordings");
    recTree.setProperty ("hz", kTimerHz, nullptr);
    for (const auto& r : recs)
    {
        ValueTree c ("Curve");
        StringArray vals;
        for (float v : r.curve) vals.add (String (v, 3));
        c.setProperty ("values", vals.joinIntoString (","), nullptr);
        c.setProperty ("playing", r.state == RecPlaying, nullptr);
        recTree.appendChild (c, nullptr);
    }
    state.appendChild (recTree, nullptr);

    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void JetstreamProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto tree = ValueTree::fromXml (*xml);

    const auto mem = tree.getChildWithName ("Memory");
    if (mem.isValid())
    {
        std::array<std::vector<float>, JetEngine::kBlocks> blocks;
        std::array<int, JetEngine::kBlocks> pre {};
        for (int b = 0; b < JetEngine::kBlocks && b < mem.getNumChildren(); ++b)
        {
            MemoryBlock bytes;
            const auto blk = mem.getChild (b);
            if (bytes.fromBase64Encoding (blk.getProperty ("data").toString()))
            {
                const auto* in = static_cast<const int16*> (bytes.getData());
                const size_t len = bytes.getSize() / sizeof (int16);
                blocks[(size_t) b].resize (len);
                for (size_t i = 0; i < len; ++i) blocks[(size_t) b][i] = (float) in[i] / 32767.0f;
                pre[(size_t) b] = (int) blk.getProperty ("preRoll", 0); // v0.1 sets have none
            }
        }
        engine.loadMemory (blocks, pre, (float) (double) mem.getProperty ("rate", 48000.0));
        tree.removeChild (mem, nullptr);
    }

    for (int s = 0; s < 2; ++s) userSlots[s] = {};
    const auto slots = tree.getChildWithName ("UserSlots");
    if (slots.isValid())
        for (int s = 0; s < 2 && s < slots.getNumChildren(); ++s)
            if (! (bool) slots.getChild (s).getProperty ("empty", true)) userSlots[s] = slots.getChild (s).createCopy();
    // v0.1 kept A and B under Snapshots
    const auto old = tree.getChildWithName ("Snapshots");
    for (const auto& child : old)
    {
        const auto nm = child.getProperty ("name").toString();
        if (nm == "A" && ! userSlots[0].isValid()) userSlots[0] = child.createCopy();
        if (nm == "B" && ! userSlots[1].isValid()) userSlots[1] = child.createCopy();
    }
    tree.removeChild (slots, nullptr);
    tree.removeChild (old, nullptr);

    const auto recTree = tree.getChildWithName ("Recordings");
    if (recTree.isValid())
    {
        const int hz = (int) recTree.getProperty ("hz", 60);
        for (int i = 0; i < (int) recs.size() && i < recTree.getNumChildren(); ++i)
        {
            auto& r = recs[(size_t) i];
            const auto c = recTree.getChild (i);
            std::vector<float> curve;
            for (const auto& s : StringArray::fromTokens (c.getProperty ("values").toString(), ",", ""))
                if (s.isNotEmpty()) curve.push_back (s.getFloatValue());
            r.curve.clear();
            // recordings made at another timer rate keep their timing
            const size_t outLen = curve.empty() ? 0 : std::max<size_t> (1, (size_t) std::lround ((double) curve.size() * kTimerHz / hz));
            for (size_t k = 0; k < outLen; ++k) r.curve.push_back (curve[std::min (curve.size() - 1, k * curve.size() / outLen)]);
            r.state = (bool) c.getProperty ("playing") && ! r.curve.empty() ? (int) RecPlaying : (int) RecIdle;
            r.play = 0;
            r.value.store (-1.0f);
        }
        tree.removeChild (recTree, nullptr);
    }

    const int savedPreset = tree.getProperty ("preset", 0);
    const bool savedEdited = tree.getProperty ("presetEdited", false);
    applyingPreset = true;
    apvts.replaceState (tree);
    applyingPreset = false;
    preset = jlimit (0, jmax (0, numPresets() - 1), savedPreset);
    edited = savedEdited;
    uiPresetPulse.fetch_add (1);
    for (auto& r : recs) r.lastKnob = get (r.paramId);
}

AudioProcessorEditor* JetstreamProcessor::createEditor() { return new JetstreamEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new JetstreamProcessor(); }

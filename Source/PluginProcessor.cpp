#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

JetstreamProcessor::JetstreamProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "JetstreamState", createParameterLayout())
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            raw[rp->getParameterID()] = apvts.getRawParameterValue (rp->getParameterID());

    recs[RecDimension].paramId = ParamIDs::dimension;
    recs[RecDrive].paramId = ParamIDs::drive;
    startTimerHz (60);
}

JetstreamProcessor::~JetstreamProcessor()
{
    stopTimer();
}

void JetstreamProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
}

bool JetstreamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != AudioChannelSet::stereo())
        return false;
    return in == AudioChannelSet::stereo() || in == AudioChannelSet::mono();
}

jet::EngineParams JetstreamProcessor::readParams()
{
    jet::EngineParams p;
    p.inputMode = idx (ParamIDs::inputMode);
    p.sensitivity = f (ParamIDs::sensitivity);
    p.trigger = idx (ParamIDs::trigger);
    p.fsMode = idx (ParamIDs::fsMode);
    p.envShape = f (ParamIDs::envShape);
    p.envTime = f (ParamIDs::envTime);
    p.voice = jlimit (0, jet::kNumVoices - 1, idx (ParamIDs::voice));
    p.dimension = f (ParamIDs::dimension);
    p.dimOverride = recs[RecDimension].value.load();
    p.lfoDepth = f (ParamIDs::lfoDepth);
    p.lfoShape = idx (ParamIDs::lfoShape);
    p.dimSource = idx (ParamIDs::dimSource);
    p.drive = f (ParamIDs::drive);
    p.driveOverride = recs[RecDrive].value.load();
    p.tone = f (ParamIDs::tone);
    p.dry = f (ParamIDs::dry);
    p.masterDb = f (ParamIDs::master);
    p.autoLevel = on (ParamIDs::autoLevel);
    p.width = f (ParamIDs::width);
    p.air = f (ParamIDs::air);
    p.hold = on (ParamIDs::hold);
    p.bypass = on (ParamIDs::bypass);
    p.footA = on (ParamIDs::footA);
    p.footB = on (ParamIDs::footB);

    // Free running like the pedal, or locked to Ableton's tempo
    float bpm = 0.0f;
    if (on (ParamIDs::sync))
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto b = pos->getBpm())
                    bpm = (float) *b;
    uiBpm.store (bpm);

    if (bpm > 0.0f)
    {
        const float beatHz = bpm / 60.0f;
        p.lfoRateHz = beatHz / names::divisionBeats[names::divisionIndex (f (ParamIDs::lfoRate))];
        // Repeat Waveform: where Env Shape sits between two shapes picks the note length
        const auto sk = jet::ShapeKnob::from (p.envShape);
        const float offset = std::log2 (sk.rateHz / 0.25f) / 6.0f;
        p.repeatRateHz = beatHz / names::divisionBeats[names::divisionIndex (offset)];
    }
    else
    {
        p.lfoRateHz = names::lfoHz (f (ParamIDs::lfoRate));
        p.repeatRateHz = -1.0f;
    }
    return p;
}

void JetstreamProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0 || getTotalNumOutputChannels() < 2)
        return;

    if (getTotalNumInputChannels() == 1) // a mono input feeds both sides
        buffer.copyFrom (1, 0, buffer, 0, 0, n);

    const auto p = readParams();

    // Process in pieces between MIDI events so MIDI-triggered notes land on time
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = jlimit (0, n, meta.samplePosition);
        if (at > pos)
        {
            engine.process (L + pos, R + pos, at - pos, p);
            pos = at;
        }
        handleMidi (meta.getMessage());
    }
    if (pos < n)
        engine.process (L + pos, R + pos, n - pos, p);
}

// MIDI notes trigger the samplers. A few CCs work the footswitches without mapping.
void JetstreamProcessor::handleMidi (const MidiMessage& m)
{
    if (m.isNoteOn())
    {
        engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        return;
    }
    if (! m.isController())
        return;
    const bool down = m.getControllerValue() >= 64;
    switch (m.getControllerNumber())
    {
        case 80: queueValue (ParamIDs::footA, down ? 1.0f : 0.0f); break;
        case 81: queueValue (ParamIDs::footB, down ? 1.0f : 0.0f); break;
        case 82: if (down) queueValue (ParamIDs::bypass, on (ParamIDs::bypass) ? 0.0f : 1.0f); break;
        case 83: if (down) queueValue (ParamIDs::hold, on (ParamIDs::hold) ? 0.0f : 1.0f); break;
        default: break;
    }
}

void JetstreamProcessor::queueValue (const char* id, float plainValue)
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0)
        pending[(size_t) scope.startIndex1] = { id, plainValue };
}

void JetstreamProcessor::setNow (const String& id, float plainValue)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

// ---- knob recording ----
void JetstreamProcessor::toggleRecording (int target)
{
    auto& r = recs[(size_t) target];
    if (r.state == RecIdle)
    {
        r.curve.clear();
        r.state = RecRecording;
        r.value.store (-1.0f);
    }
    else if (r.state == RecRecording)
    {
        r.state = r.curve.size() > 6 ? RecPlaying : RecIdle;
        r.play = 0;
        r.lastKnob = f (r.paramId);
    }
    else
    {
        r.state = RecIdle;
        r.value.store (-1.0f);
    }
}

float JetstreamProcessor::recordingProgress (int target) const
{
    const auto& r = recs[(size_t) target];
    if (r.state == RecRecording) return (float) r.curve.size() / 600.0f;
    if (r.state == RecPlaying && ! r.curve.empty()) return (float) r.play / (float) r.curve.size();
    return 0.0f;
}

int JetstreamProcessor::learnState() const
{
    if (engine.uiLearning.load()) return 1;
    if (Time::getMillisecondCounterHiRes() * 0.001 < learnFlashUntil) return learnFlash;
    return 0;
}

// ---- message thread housekeeping ----
void JetstreamProcessor::timerCallback()
{
    // MIDI CC changes
    const auto scope = fifo.read (fifo.getNumReady());
    auto apply = [this] (int start, int size)
    {
        for (int i = start; i < start + size; ++i)
            setNow (pending[(size_t) i].id, pending[(size_t) i].value);
    };
    apply (scope.startIndex1, scope.blockSize1);
    apply (scope.startIndex2, scope.blockSize2);

    // Learn finished: set the sensitivity
    const float learned = engine.uiLearnResult.exchange (-1.0f);
    if (learned >= 0.0f)
    {
        setNow (ParamIDs::sensitivity, learned);
        learnFlash = 2;
        learnFlashUntil = Time::getMillisecondCounterHiRes() * 0.001 + 1.5;
    }
    else if (learned < -1.5f)
    {
        learnFlash = 3;
        learnFlashUntil = Time::getMillisecondCounterHiRes() * 0.001 + 1.5;
    }

    // Knob recordings: up to 10 seconds, looped. Moving the knob again stops playback.
    for (auto& r : recs)
    {
        const float knob = f (r.paramId);
        if (r.state == RecRecording)
        {
            r.curve.push_back (knob);
            if (r.curve.size() >= 600)
            {
                r.state = RecPlaying;
                r.play = 0;
                r.lastKnob = knob;
            }
        }
        else if (r.state == RecPlaying)
        {
            if (std::abs (knob - r.lastKnob) > 0.004f)
            {
                r.state = RecIdle;
                r.value.store (-1.0f);
                continue;
            }
            r.value.store (r.curve[r.play]);
            if (++r.play >= r.curve.size()) r.play = 0;
        }
    }

    // Preset toggle: leaving Live keeps your live settings, coming back restores them
    const int slot = idx (ParamIDs::presetSlot);
    if (slot != lastSlot)
    {
        auto store = apvts.state.getOrCreateChildWithName ("Snapshots", nullptr);
        if (lastSlot == 1)
        {
            for (int i = store.getNumChildren(); --i >= 0;)
                if (store.getChild (i).getProperty ("name").toString() == "Live")
                    store.removeChild (i, nullptr);
            auto live = snapshotOf();
            live.setProperty ("name", "Live", nullptr);
            store.appendChild (live, nullptr);
        }
        const char* name = slot == 0 ? "A" : (slot == 2 ? "B" : "Live");
        for (const auto& child : store)
            if (child.getProperty ("name").toString() == name)
            {
                applySnapshot (child);
                break;
            }
        lastSlot = slot;
    }
}

bool JetstreamProcessor::inSnapshot (const String& id)
{
    static const StringArray excluded { ParamIDs::presetSlot, ParamIDs::bypass, ParamIDs::footA, ParamIDs::footB, ParamIDs::hold };
    return ! excluded.contains (id);
}

ValueTree JetstreamProcessor::snapshotOf() const
{
    ValueTree t ("Snapshot");
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            if (inSnapshot (rp->getParameterID()))
                t.setProperty (Identifier (rp->getParameterID()), rp->convertFrom0to1 (rp->getValue()), nullptr);
    return t;
}

void JetstreamProcessor::applySnapshot (const ValueTree& t)
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
        {
            const Identifier id (rp->getParameterID());
            if (inSnapshot (rp->getParameterID()) && t.hasProperty (id))
                setNow (rp->getParameterID(), (float) t.getProperty (id));
        }
}

void JetstreamProcessor::savePresetSlot (int slot)
{
    if (slot != 0 && slot != 2)
        return;
    const String name = slot == 0 ? "A" : "B";
    auto store = apvts.state.getOrCreateChildWithName ("Snapshots", nullptr);
    for (int i = store.getNumChildren(); --i >= 0;)
        if (store.getChild (i).getProperty ("name").toString() == name)
            store.removeChild (i, nullptr);
    auto snap = snapshotOf();
    snap.setProperty ("name", name, nullptr);
    store.appendChild (snap, nullptr);
}

bool JetstreamProcessor::hasPresetSlot (int slot) const
{
    const String name = slot == 0 ? "A" : (slot == 2 ? "B" : "Live");
    const auto store = apvts.state.getChildWithName ("Snapshots");
    for (const auto& child : store)
        if (child.getProperty ("name").toString() == name)
            return true;
    return false;
}

AudioProcessorEditor* JetstreamProcessor::createEditor()
{
    return new JetstreamEditor (*this);
}

// ---- saving: parameters, presets, knob recordings and the Glitch B memory ----
void JetstreamProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();

    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto type = state.getChild (i).getType().toString();
        if (type == "Memory" || type == "Recordings")
            state.removeChild (i, nullptr);
    }

    ValueTree mem ("Memory");
    mem.setProperty ("rate", engine.sampleRate(), nullptr);
    for (int b = 0; b < jet::JetEngine::kBlocks; ++b)
    {
        ValueTree blk ("Block");
        const int len = engine.memoryLength (b);
        if (len > 0)
        {
            MemoryBlock data ((size_t) len * sizeof (int16));
            auto* out = static_cast<int16*> (data.getData());
            const float* in = engine.memoryData (b);
            for (int i = 0; i < len; ++i)
                out[i] = (int16) jlimit (-32767, 32767, roundToInt (in[i] * 32767.0f));
            blk.setProperty ("data", data.toBase64Encoding(), nullptr);
        }
        mem.appendChild (blk, nullptr);
    }
    state.appendChild (mem, nullptr);

    ValueTree recTree ("Recordings");
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

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void JetstreamProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = ValueTree::fromXml (*xml);

    const auto mem = tree.getChildWithName ("Memory");
    if (mem.isValid())
    {
        std::array<std::vector<float>, jet::JetEngine::kBlocks> blocks;
        for (int b = 0; b < jet::JetEngine::kBlocks && b < mem.getNumChildren(); ++b)
        {
            MemoryBlock bytes;
            if (bytes.fromBase64Encoding (mem.getChild (b).getProperty ("data").toString()))
            {
                const auto* in = static_cast<const int16*> (bytes.getData());
                const size_t len = bytes.getSize() / sizeof (int16);
                blocks[(size_t) b].resize (len);
                for (size_t i = 0; i < len; ++i)
                    blocks[(size_t) b][i] = (float) in[i] / 32767.0f;
            }
        }
        engine.loadMemory (blocks, (float) (double) mem.getProperty ("rate", 48000.0));
        tree.removeChild (mem, nullptr);
    }

    const auto recTree = tree.getChildWithName ("Recordings");
    if (recTree.isValid())
    {
        for (int i = 0; i < (int) recs.size() && i < recTree.getNumChildren(); ++i)
        {
            auto& r = recs[(size_t) i];
            const auto c = recTree.getChild (i);
            r.curve.clear();
            for (const auto& s : StringArray::fromTokens (c.getProperty ("values").toString(), ",", ""))
                if (s.isNotEmpty()) r.curve.push_back (s.getFloatValue());
            r.state = (bool) c.getProperty ("playing") && ! r.curve.empty() ? (int) RecPlaying : (int) RecIdle;
            r.play = 0;
            r.value.store (-1.0f);
        }
        tree.removeChild (recTree, nullptr);
    }

    apvts.replaceState (tree);
    lastSlot = idx (ParamIDs::presetSlot); // don't re-apply a preset on load
    for (auto& r : recs)
        r.lastKnob = f (r.paramId);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new JetstreamProcessor();
}

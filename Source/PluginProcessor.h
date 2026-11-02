#pragma once

#include "Params.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>

class JetstreamProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    JetstreamProcessor();
    ~JetstreamProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Jetstream"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    jet::JetEngine& getEngine() { return engine; }
    const jet::JetEngine& getEngine() const { return engine; }

    // ---- called from the panel (message thread) ----
    void startLearn() { engine.startLearn(); learnFlashUntil = 0; }
    int learnState() const; // 0 idle, 1 listening, 2 just set, 3 heard nothing
    void clearMemory() { engine.clearMemory(); }
    void savePresetSlot (int slot);
    bool hasPresetSlot (int slot) const;
    float currentBpm() const { return uiBpm.load(); }

    // Knob recording: like holding the pedal's bypass switch and turning Dimension or Drive
    enum RecTarget { RecDimension = 0, RecDrive = 1 };
    enum RecState { RecIdle = 0, RecRecording, RecPlaying };
    void toggleRecording (int target);
    int recordingState (int target) const { return recs[(size_t) target].state; }
    float recordingProgress (int target) const;

private:
    void timerCallback() override;
    jet::EngineParams readParams();
    void handleMidi (const juce::MidiMessage&);
    void setNow (const juce::String& id, float plainValue);
    juce::ValueTree snapshotOf() const;
    void applySnapshot (const juce::ValueTree&);
    static bool inSnapshot (const juce::String& id);

    jet::JetEngine engine;

    std::map<juce::String, std::atomic<float>*> raw;
    float f (const char* id) const { return raw.at (id)->load (std::memory_order_relaxed); }
    bool on (const char* id) const { return f (id) > 0.5f; }
    int idx (const char* id) const { return (int) std::lround (f (id)); }

    struct Recording
    {
        const char* paramId = nullptr;
        int state = RecIdle;
        std::vector<float> curve;
        size_t play = 0;
        float lastKnob = 0.0f;
        std::atomic<float> value { -1.0f };
    };
    std::array<Recording, 2> recs;

    // MIDI CC changes are made on the message thread
    struct Pending { const char* id = nullptr; float value = 0; };
    juce::AbstractFifo fifo { 256 };
    std::array<Pending, 256> pending;
    void queueValue (const char* id, float plainValue);

    std::atomic<float> uiBpm { 0.0f };
    int lastSlot = 1;
    double learnFlashUntil = 0;
    int learnFlash = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JetstreamProcessor)
};

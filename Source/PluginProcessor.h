#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "PedalControl.h"
#include "Keeper.h"
#include <map>

// Jetstream is played from four footswitches and an expression pedal, with an on-screen
// button for every footswitch job.
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

    // ---- the panel (message thread) ----
    void footDown (int sw);                 // on-screen footswitches behave exactly like the real ones
    void footUp (int sw);
    void setPlain (const juce::String& id, float plain);
    void setNorm (const juce::String& id, float v01);
    void togglePlain (const juce::String& id);
    float get (const char* id) const { return raw.at (id)->load (std::memory_order_relaxed); }
    void touchPanel();                      // touching the panel takes the footswitches
    juce::String expressionRangeText (bool toe) const;
    void pump() { timerCallback(); }        // tests: run the message-thread work now

    // sensitivity Learn (not footswitch Learn)
    void startLearn() { engine.startLearn(); learnFlashUntil = 0; }
    int learnState() const;                 // 0 idle, 1 listening, 2 just set, 3 heard nothing
    void clearMemory() { engine.clearMemory(); say ("MEMORY CLEARED"); }

    // presets: twenty factory ones, then your own
    int numPresets() const { return (int) jet::factoryPresets().size() + userPresets.size(); }
    juce::String presetName (int i) const;
    juce::String presetHint (int i) const;
    bool presetIsUser (int i) const { return i >= (int) jet::factoryPresets().size(); }
    void loadPreset (int i);
    void stepPreset (int direction);
    void saveUserPreset();
    int currentPreset() const { return preset; }
    bool presetEdited() const { return edited.load(); }
    static juce::File userPresetFolder();

    // User A and B: two quick snapshots kept with the set. Tap to recall, hold to store.
    void recallUser (int slot);
    void storeUser (int slot);
    bool hasUser (int slot) const;

    // Knob recording: like holding the pedal's bypass switch and turning Dimension or Drive
    enum RecTarget { RecDimension = 0, RecDrive = 1 };
    enum RecState { RecIdle = 0, RecRecording, RecPlaying };
    void toggleRecording (int target);
    int recordingState (int target) const { return recs[(size_t) target].state; }
    float recordingProgress (int target) const;

    // footswitch and expression setup
    PedalSettings& pedals() { return settings; }
    void saveSettings();
    void learnFootswitch (int sw);
    void learnExpression();
    void learnKeeper();
    void calibrateExpression();
    void resetFootswitches();
    void setFootDevice (const juce::String& device);
    void setPedalMapChannel (int ch);
    bool pedalsDirect() const { return direct->connected(); }
    juce::String footInputStatus() const { return direct->status(); }
    bool isLearning() const { return learnTarget.load() != LearnNone; }
    int learningWhat() const { return learnTarget.load(); }
    bool isCalibrating() const { return calibrating; }
    enum { LearnNone = -1, LearnExpression = 4, LearnKeeper = 5 };
    void handleDirectMidi (const juce::MidiMessage&);   // MIDI thread
    std::atomic<bool> hostFeedsPedals { false };
    bool hasFootFocus() const;

    // Keeper
    void keepNow();
    juce::File exportKeepers();
    void setKeeperLive (bool live);
    int lastKeeper() const { return keeper.lastNumber(); }

    // ---- what the panel and Pebble show ----
    std::atomic<int> uiFootPulse { 0 }, uiKeepPulse { 0 }, uiFootDown { 0 }, uiPresetPulse { 0 };
    std::atomic<float> uiExp { 0.0f }, uiBpm { 0.0f };
    juce::String uiMessage;
    double uiMessageAt = 0.0;
    juce::String footJobTap (int sw) const;
    juce::String footJobHold (int sw) const;
    void say (const juce::String&);

private:
    void timerCallback() override;
    jet::EngineParams readParams (int numSamples);
    void handleMidi (const juce::MidiMessage&, bool fromDevice);
    void onFootEvent (int sw, FootswitchLogic::Event);
    void loadUserPresets();
    juce::ValueTree snapshot() const;
    void applySnapshot (const juce::ValueTree&);

    jet::JetEngine engine;
    Keeper keeper;
    juce::AudioBuffer<float> inCopy;

    std::map<juce::String, std::atomic<float>*> raw;
    std::vector<std::atomic<float>*> specRaw;   // in paramSpecs() order, for the audio thread

    // footswitch A and B to the engine, applied at the start of the next block
    std::atomic<int> footBits { 0 };            // bit c: footswitch c is down
    std::atomic<int> captureBits { 0 }, cancelBits { 0 };
    void setEngineFoot (int c, bool down);

    // MIDI handed to the message thread
    struct Pending { int kind = 0; int a = 0; };
    enum PendingKind { FootPress, FootRelease, Expression, Learned, KeepCC, MapFoot, MapBypass, MapHold };
    juce::AbstractFifo fifo { 512 };
    std::array<Pending, 512> pending;
    juce::AbstractFifo directFifo { 512 };
    std::array<Pending, 512> directPending;
    juce::SpinLock directWriteLock;
    juce::SharedResourcePointer<DirectMidiInput> direct;
    void queue (int kind, int a);

    PedalSettings settings;
    juce::Time settingsStamp;
    double nextSettingsCheck = 0.0;
    bool wasDirect = false;
    FootswitchLogic footswitches;
    std::atomic<int> learnTarget { LearnNone };
    bool calibrating = false;
    double calibrateUntil = 0.0;
    int calLo = 127, calHi = 0, calFirst = -1;
    double expMovedAt = 0.0;
    float expSmoothed = 0.0f;
    double currentSampleRate = 48000.0;

    // footswitch state
    bool downFired[2] = { false, false };
    bool comboABPending = false, comboCDPending = false;

    // presets
    juce::Array<juce::File> userPresets;
    int preset = 0;
    std::atomic<bool> edited { false }, applyingPreset { false };
    struct EditWatch : juce::AudioProcessorValueTreeState::Listener
    {
        JetstreamProcessor& owner;
        explicit EditWatch (JetstreamProcessor& o) : owner (o) {}
        void parameterChanged (const juce::String& id, float) override;
    };
    EditWatch editWatch { *this };
    juce::ValueTree userSlots[2];

    // knob recordings
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

    double learnFlashUntil = 0;
    int learnFlash = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JetstreamProcessor)
};

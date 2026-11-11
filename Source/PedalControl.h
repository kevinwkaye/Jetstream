#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <functional>

// Footswitch and expression setup, shared by every Flip Fella plugin on this computer,
// so a footswitch set up once works in all of them. Nothing here names a brand:
// any MIDI footswitch with momentary switches and any expression input works.
//
// Defaults: footswitches A to D send momentary CC 20, 21, 22, 23 on channel 16,
// expression sends CC 11 on channel 16. Learn remaps any of them.
struct PedalSettings
{
    std::array<int, 4> fsChannel { 16, 16, 16, 16 };
    std::array<int, 4> fsCC { 20, 21, 22, 23 };
    int expChannel = 16, expCC = 11;
    int heel = 0, toe = 127;     // calibrated ends of the expression pedal; toe may be lower than heel
    int curve = 0;               // 0 linear, 1 slow start, 2 fast start
    int pedalMapChannel = 0;     // 0 off, else the channel that speaks the original pedal's own CC map
    int keeperChannel = 0, keeperCC = -1;  // a learned switch just for Keeper; off until learned
    bool keeperLive = false;     // Keepers are tagged Live or Studio
    juce::String footDevice;     // MIDI input read directly: empty = find the footswitch automatically, "none" = off, else a device name
    int prismLayout = 0;         // Prism Patch footswitches: 0 Modules (A to D switch slots 1 to 4), 1 Classic
    int backtrackLayout = 0;     // Backtrack footswitches: 0 Backtrack (A to D as on its panel), 1 Classic
    int expSmooth = 1;           // expression smoothing: 0 off, 1 low, 2 high

    void load();
    void save() const;
    void resetFootswitches();
    float expressionValue (int cc) const;    // 0..1 after calibration and curve
    static juce::File file();
};

// Turns plain press and release messages into taps, holds and two-switch combos.
// Runs on the message thread.
class FootswitchLogic
{
public:
    enum Event { Down, Tap, HoldStart, HoldEnd, ComboAB, ComboCD, ComboHoldAB, ComboHoldCD };
    std::function<void (int sw, Event)> onEvent;

    static constexpr double kHoldSeconds = 0.45, kComboWindow = 0.12, kComboHoldSeconds = 1.0;

    void press (int sw, double now);
    void release (int sw, double now);
    void tick (double now);
    bool isDown (int sw) const { return down[(size_t) sw]; }

private:
    std::array<bool, 4> down {}, held {}, combo {};
    std::array<double, 4> downAt {};
    std::array<double, 2> comboAt {};      // A+B and C+D
    std::array<bool, 2> comboHeld {};
};

// Reads the footswitch's MIDI device directly, so every copy of a plugin hears the
// footswitches without routing a MIDI track to it. One per process, shared by all copies.
// Messages arrive on a MIDI thread and go to the sink, which picks the copy in charge.
class DirectMidiInput : private juce::MidiInputCallback, private juce::Timer
{
public:
    DirectMidiInput();
    ~DirectMidiInput() override;

    using Sink = void (*) (const juce::MidiMessage&);
    void setSink (Sink s) { sink.store (s); }
    void setWanted (const juce::String& device);   // see PedalSettings::footDevice
    void refresh();

    bool connected() const { return isConnected.load(); }
    juce::String status() const;                   // a short line for the panel
    juce::StringArray openDevices() const;
    static juce::StringArray deviceNames();
    static bool looksLikeFootswitch (const juce::String& name);

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override { refresh(); }

    std::atomic<Sink> sink { nullptr };
    std::atomic<bool> isConnected { false };
    mutable juce::CriticalSection lock;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    juce::StringArray busy;
    juce::String wanted;
};

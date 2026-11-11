// Plugin-level checks: footswitch taps, holds and combos from plain 127/0 messages, the
// expression pedal, presets, and that the shared footswitch CCs never clash with anything.
#include "../Source/PluginProcessor.h"

static int failures = 0;
static void expect (bool ok, const char* what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++failures;
}

struct Rig
{
    JetstreamProcessor p;
    juce::AudioBuffer<float> b { 2, 256 };
    Rig() { p.prepareToPlay (48000.0, 256); p.pedals().resetFootswitches(); p.pedals().expChannel = 16; p.pedals().expCC = 11; p.pedals().pedalMapChannel = 0; p.setFootDevice ("none"); }
    void block (juce::MidiBuffer m = {})
    {
        b.clear();
        p.processBlock (b, m);
        p.pump();
    }
    void cc (int ch, int num, int val) { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::controllerEvent (ch, num, val), 0); block (m); }
    void wait (double seconds)
    {
        const double until = juce::Time::getMillisecondCounterHiRes() * 0.001 + seconds;
        while (juce::Time::getMillisecondCounterHiRes() * 0.001 < until) { block(); juce::Thread::sleep (5); }
    }
    float get (const char* id) { return p.get (id); }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::printf ("Footswitches\n");
    {
        Rig r;
        r.p.loadPreset (0);
        const int start = r.p.currentPreset();
        r.cc (16, 23, 127); r.wait (0.05); r.cc (16, 23, 0);           // tap D
        expect (r.p.currentPreset() == start + 1, "tap D: next preset");
        r.cc (16, 23, 127); r.wait (0.7); r.cc (16, 23, 0);            // hold D
        expect (r.p.currentPreset() == start, "hold D: previous preset");

        r.cc (16, 22, 127); r.wait (0.05); r.cc (16, 22, 0);           // tap C
        expect (r.get ("hold") > 0.5f, "tap C: Hold Memory on");
        r.cc (16, 22, 127); r.wait (0.05); r.cc (16, 22, 0);
        expect (r.get ("hold") < 0.5f, "tap C again: Hold Memory off");
        r.cc (16, 22, 127); r.wait (0.7); r.cc (16, 22, 0);            // hold C
        expect (r.get ("bypass") > 0.5f && r.get ("hold") < 0.5f, "hold C: bypass, memory untouched");
        r.cc (16, 22, 127); r.wait (0.7); r.cc (16, 22, 0);
        expect (r.get ("bypass") < 0.5f, "hold C again: on");

        r.cc (16, 20, 127); r.cc (16, 21, 127); r.wait (0.05); r.cc (16, 20, 0); r.cc (16, 21, 0); r.wait (0.02);
        expect (r.get ("bypass") > 0.5f, "A+B together: bypass");
        r.cc (16, 20, 127); r.cc (16, 21, 127); r.wait (0.05); r.cc (16, 20, 0); r.cc (16, 21, 0); r.wait (0.02);
        expect (r.get ("bypass") < 0.5f, "A+B again: on");

        const int kept = r.p.lastKeeper();
        r.cc (16, 22, 127); r.cc (16, 23, 127); r.wait (1.2); r.cc (16, 22, 0); r.cc (16, 23, 0);
        r.wait (0.5);
        expect (r.p.lastKeeper() > kept || r.p.uiKeepPulse.load() > 0, "C+D held 1 s: Keeper");
        expect (r.get ("hold") < 0.5f && r.p.currentPreset() == start, "C+D doesn't also act as C or D");
    }

    std::printf ("Expression\n");
    {
        Rig r;
        r.p.loadPreset (0); // INIT: Dimension 0.2 to 0.9
        r.cc (16, 11, 0); r.wait (0.05);
        expect (std::abs (r.get ("exp")) < 0.01f && r.get ("expOn") > 0.5f, "heel");
        r.cc (16, 11, 127); r.wait (0.05);
        expect (r.get ("exp") > 0.99f, "toe");
        expect (r.p.expressionRangeText (false) == "20%" && r.p.expressionRangeText (true) == "90%", "heel and toe read in the target's own units");
    }

    std::printf ("No clashes\n");
    for (int mapCh : { 0, 1 })
    {
        Rig r;
        r.p.pedals().pedalMapChannel = mapCh;
        r.p.loadPreset (2);
        std::map<juce::String, float> before;
        for (auto& s : jet::paramSpecs()) before[s.id] = r.get (s.id);
        // the shared CCs on the wrong channel, and every other CC on channel 16, change nothing
        for (int cc : { 20, 21, 22, 23, 11 }) { r.cc (1 + (mapCh == 1 ? 1 : 0), cc, 127); r.cc (2, cc, 0); }
        for (int cc : { 1, 7, 64, 80, 81, 82, 83, 100 }) { r.cc (16, cc, 127); r.cc (16, cc, 0); }
        r.wait (0.8);
        bool same = true;
        for (auto& s : jet::paramSpecs()) same = same && std::abs (before[s.id] - r.get (s.id)) < 1.0e-6f;
        expect (same && r.p.currentPreset() == 2, mapCh == 0 ? "original map off: stray CCs do nothing" : "original map on channel 1: other channels do nothing");
    }
    {
        Rig r;
        r.p.pedals().pedalMapChannel = 3;
        r.cc (3, 82, 127); r.cc (3, 82, 0); r.wait (0.05);
        expect (r.get ("bypass") > 0.5f, "original map: CC 82 on its channel toggles bypass");
        r.cc (3, 83, 127); r.cc (3, 83, 0); r.wait (0.05);
        expect (r.get ("hold") > 0.5f, "original map: CC 83 toggles Hold");
    }

    std::printf ("Presets and state\n");
    {
        Rig r;
        for (int i = 0; i < 20; ++i) { r.p.loadPreset (i); r.block(); }
        r.p.loadPreset (7);
        r.p.storeUser (0);
        r.p.loadPreset (0);
        r.p.recallUser (0);
        expect ((int) std::lround (r.get ("voice")) == jet::VSynthA, "User A recalls the stored sound");
        juce::MemoryBlock state;
        r.p.getStateInformation (state);
        Rig r2;
        r2.p.setStateInformation (state.getData(), (int) state.getSize());
        expect (r2.p.hasUser (0) && ! r2.p.hasUser (1), "User A saved with the set");
        expect (r2.p.currentPreset() == 0 && r2.p.presetEdited() && (int) std::lround (r2.get ("voice")) == jet::VSynthA, "preset, edited mark and sound restored");
        expect (r2.p.getLatencySamples() == jet::JetEngine::latencySamples(), "latency reported");
    }

    std::printf (failures == 0 ? "All plugin checks passed\n" : "%d plugin check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

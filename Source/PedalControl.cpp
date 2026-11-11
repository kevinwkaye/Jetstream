#include "PedalControl.h"

using namespace juce;

File PedalSettings::file()
{
    return File::getSpecialLocation (File::userApplicationDataDirectory)
        .getChildFile ("Kevin Kaye").getChildFile ("Flip Fella Pedals.xml");
}

void PedalSettings::load()
{
    auto xml = parseXML (file());
    if (xml == nullptr || ! xml->hasTagName ("Pedals")) return;
    for (int i = 0; i < 4; ++i)
    {
        fsChannel[(size_t) i] = jlimit (1, 16, xml->getIntAttribute ("fsChannel" + String (i), fsChannel[(size_t) i]));
        fsCC[(size_t) i] = jlimit (0, 127, xml->getIntAttribute ("fsCC" + String (i), fsCC[(size_t) i]));
    }
    expChannel = jlimit (1, 16, xml->getIntAttribute ("expChannel", expChannel));
    expCC = jlimit (0, 127, xml->getIntAttribute ("expCC", expCC));
    heel = jlimit (0, 127, xml->getIntAttribute ("heel", heel));
    toe = jlimit (0, 127, xml->getIntAttribute ("toe", toe));
    curve = jlimit (0, 2, xml->getIntAttribute ("curve", curve));
    pedalMapChannel = jlimit (0, 16, xml->getIntAttribute ("pedalMapChannel", pedalMapChannel));
    keeperChannel = jlimit (0, 16, xml->getIntAttribute ("keeperChannel", keeperChannel));
    keeperCC = jlimit (-1, 127, xml->getIntAttribute ("keeperCC", keeperCC));
    keeperLive = xml->getBoolAttribute ("keeperLive", keeperLive);
    prismLayout = jlimit (0, 1, xml->getIntAttribute ("prismLayout", prismLayout));
    backtrackLayout = jlimit (0, 1, xml->getIntAttribute ("backtrackLayout", backtrackLayout));
    expSmooth = jlimit (0, 2, xml->getIntAttribute ("expSmooth", expSmooth));
    footDevice = xml->getStringAttribute ("footDevice", footDevice);
}

void PedalSettings::save() const
{
    // start from what's on disk, so settings written by other Flip Fella plugins are kept
    std::unique_ptr<XmlElement> existing = parseXML (file());
    XmlElement xml ("Pedals");
    if (existing != nullptr && existing->hasTagName ("Pedals"))
        for (int i = 0; i < existing->getNumAttributes(); ++i)
            xml.setAttribute (existing->getAttributeName (i), existing->getAttributeValue (i));
    for (int i = 0; i < 4; ++i)
    {
        xml.setAttribute ("fsChannel" + String (i), fsChannel[(size_t) i]);
        xml.setAttribute ("fsCC" + String (i), fsCC[(size_t) i]);
    }
    xml.setAttribute ("expChannel", expChannel);
    xml.setAttribute ("expCC", expCC);
    xml.setAttribute ("heel", heel);
    xml.setAttribute ("toe", toe);
    xml.setAttribute ("curve", curve);
    xml.setAttribute ("pedalMapChannel", pedalMapChannel);
    xml.setAttribute ("keeperChannel", keeperChannel);
    xml.setAttribute ("keeperCC", keeperCC);
    xml.setAttribute ("keeperLive", keeperLive);
    xml.setAttribute ("prismLayout", prismLayout);
    xml.setAttribute ("backtrackLayout", backtrackLayout);
    xml.setAttribute ("expSmooth", expSmooth);
    xml.setAttribute ("footDevice", footDevice);
    file().getParentDirectory().createDirectory();
    xml.writeTo (file());
}

void PedalSettings::resetFootswitches()
{
    fsChannel = { 16, 16, 16, 16 };
    fsCC = { 20, 21, 22, 23 };
}

float PedalSettings::expressionValue (int cc) const
{
    // a reversed or short-range pedal is fixed by the calibrated heel and toe
    const float span = (float) (toe - heel);
    float v = std::abs (span) < 4.0f ? (float) cc / 127.0f : ((float) cc - (float) heel) / span;
    v = jlimit (0.0f, 1.0f, v);
    if (curve == 1) v = v * v;
    else if (curve == 2) v = std::sqrt (v);
    return v;
}

// ---------------------------------------------------------------------------------

void FootswitchLogic::press (int sw, double now)
{
    if (sw < 0 || sw > 3 || down[(size_t) sw]) return;
    down[(size_t) sw] = true;
    held[(size_t) sw] = false;
    combo[(size_t) sw] = false;
    downAt[(size_t) sw] = now;

    // A with B, or C with D, pressed together
    const int partner = sw ^ 1;
    if (down[(size_t) partner] && ! held[(size_t) partner] && now - downAt[(size_t) partner] < kComboWindow)
    {
        combo[(size_t) sw] = combo[(size_t) partner] = true;
        comboAt[(size_t) (sw / 2)] = now;
        comboHeld[(size_t) (sw / 2)] = false;
        if (onEvent) onEvent (sw < 2 ? 0 : 2, sw < 2 ? ComboAB : ComboCD);
        return;
    }
    if (onEvent) onEvent (sw, Down);
}

void FootswitchLogic::release (int sw, double now)
{
    if (sw < 0 || sw > 3 || ! down[(size_t) sw]) return;
    tick (now);
    down[(size_t) sw] = false;
    if (combo[(size_t) sw]) return;
    if (onEvent) onEvent (sw, held[(size_t) sw] ? HoldEnd : Tap);
}

void FootswitchLogic::tick (double now)
{
    // both switches of a combo still down after a second: the combo hold
    for (int pair = 0; pair < 2; ++pair)
    {
        const int a = pair * 2, b = a + 1;
        if (down[(size_t) a] && down[(size_t) b] && combo[(size_t) a] && combo[(size_t) b]
            && ! comboHeld[(size_t) pair] && now - comboAt[(size_t) pair] >= kComboHoldSeconds)
        {
            comboHeld[(size_t) pair] = true;
            if (onEvent) onEvent (a, pair == 0 ? ComboHoldAB : ComboHoldCD);
        }
    }
    for (int i = 0; i < 4; ++i)
        if (down[(size_t) i] && ! held[(size_t) i] && ! combo[(size_t) i] && now - downAt[(size_t) i] >= kHoldSeconds)
        {
            held[(size_t) i] = true;
            if (onEvent) onEvent (i, HoldStart);
        }
}

// =====================================================================================
// Direct MIDI input
// =====================================================================================
DirectMidiInput::DirectMidiInput()
{
    refresh();
    startTimer (2000); // picks up a footswitch plugged in later, or freed by another app
}

DirectMidiInput::~DirectMidiInput()
{
    stopTimer();
    const ScopedLock l (lock);
    for (auto& in : inputs) in->stop();
    inputs.clear();
}

bool DirectMidiInput::looksLikeFootswitch (const String& name)
{
    const auto n = name.toLowerCase();
    for (auto* skip : { "keyboard", "keys", "piano", "mixer", "pad", "smk", "launch", "maschine", "push", "loopmidi", "loopbe" })
        if (n.contains (skip)) return false;
    for (auto* want : { "footctrl", "foot ctrl", "footswitch", "foot switch", "foot controller", "chocolate", "m-vave", "mvave", "cuvave", "pedal" })
        if (n.contains (want)) return true;
    return false;
}

StringArray DirectMidiInput::deviceNames()
{
    StringArray names;
    for (const auto& d : MidiInput::getAvailableDevices()) names.add (d.name);
    return names;
}

void DirectMidiInput::setWanted (const String& device)
{
    {
        const ScopedLock l (lock);
        if (device == wanted) return;
        wanted = device;
    }
    refresh();
}

void DirectMidiInput::refresh()
{
    const auto available = MidiInput::getAvailableDevices();
    const ScopedLock l (lock);
    auto want = [this] (const String& name)
    {
        if (wanted == "none") return false;
        return wanted.isEmpty() ? looksLikeFootswitch (name) : name == wanted;
    };

    // close what's gone or no longer wanted
    for (int i = (int) inputs.size(); --i >= 0;)
    {
        bool present = false;
        for (const auto& d : available) present = present || d.identifier == inputs[(size_t) i]->getIdentifier();
        if (! present || ! want (inputs[(size_t) i]->getName()))
        {
            inputs[(size_t) i]->stop();
            inputs.erase (inputs.begin() + i);
        }
    }

    busy.clear();
    for (const auto& d : available)
    {
        if (! want (d.name)) continue;
        bool open = false;
        for (auto& in : inputs) open = open || in->getIdentifier() == d.identifier;
        if (open) continue;
        if (auto in = MidiInput::openDevice (d.identifier, this))
        {
            in->start();
            inputs.push_back (std::move (in));
        }
        else busy.add (d.name); // Windows lets one program hold a MIDI input at a time
    }
    isConnected.store (! inputs.empty());
}

String DirectMidiInput::status() const
{
    const ScopedLock l (lock);
    if (wanted == "none") return "Off: footswitch MIDI comes from Ableton";
    if (! inputs.empty())
    {
        StringArray names;
        for (auto& in : inputs) names.add (in->getName());
        return "Connected: " + names.joinIntoString (", ");
    }
    if (! busy.isEmpty()) return busy[0] + " is in use by Ableton. Turn its Track and Remote off in Ableton's MIDI settings";
    return wanted.isEmpty() ? "No footswitch found: plug it in, or pick it below"
                            : wanted + " isn't plugged in";
}

StringArray DirectMidiInput::openDevices() const
{
    const ScopedLock l (lock);
    StringArray names;
    for (auto& in : inputs) names.add (in->getName());
    return names;
}

void DirectMidiInput::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    if (auto s = sink.load()) s (m);
}

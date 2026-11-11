# Jetstream foot control plan

**Status, 11 November 2026: built in v0.2.** The table below is what shipped, with A and B tap to catch and hold to sustain in Poly and Mono. See the README.

Written 4 November 2026 from a cross-plugin audit. Build it in Jetstream's own thread.

## What happens today

- Footswitch A, Footswitch B, Bypass and Hold listen on CC 80 to 83 on any channel. They don't match the shared defaults, so footswitches A to D do nothing.
- No expression input.
- The Learn button learns input sensitivity, not MIDI. Keep it, but label the pedal Learn differently on the Pedal page so the two don't get confused.

## Footswitch defaults

| Footswitch | Tap | Hold |
| --- | --- | --- |
| A | Footswitch A | |
| B | Footswitch B | |
| C | Hold Memory on or off | Bypass |
| D | Next preset | Previous preset |
| A+B | Bypass | |
| C+D held 1 second | Keeper | |

A and B pass press and release straight through, so the Toggle, Latch and Momentary footswitch modes keep working as now. Move CC 80 to 83 behind the original pedal MIDI map option.

## Expression default

Dimension, for big sweeps of the resynth texture. Assignable to Drive, Dry, LFO Depth, Width or Air.

## How to build it

Prism Patch is the reference. Its foot control already follows the house rules, and its Pedal page design is already approved, so reuse it rather than drawing a new one.

1. **Shared module.** Copy `Source/PedalControl.h` and `Source/PedalControl.cpp` from kevinwkaye/prism--patch, unchanged apart from the ComboHold addition below. Settings live in the shared `Flip Fella Pedals.xml`, so a setup made in one plugin works in all of them.
2. **Combo hold.** Add a `ComboHoldAB` / `ComboHoldCD` event to `FootswitchLogic`: fire it when both switches of a combo stay down for 1 second. A combo tap keeps firing on press as now. Hold C+D for 1 second is reserved for Keeper in every plugin. If Prism Patch has already added this, copy its version.
3. **MIDI routing.** In `handleMidi`, offer every message to the pedal code first. A footswitch or expression message on its learned channel and CC is consumed there and goes no further. Notes, pitch bend and program changes carry on as before.
4. **Original pedal MIDI map.** Move the existing CC chart behind an "Original pedal MIDI map" channel setting on the Pedal page, off by default, exactly as Prism Patch does. This is what stops footswitches A to D and the expression pedal colliding with the chart.
5. **Pedal page.** Four footswitch flip cards showing Tap and Hold jobs, what each listens for, Learn and Defaults. An expression bar with target, Calibrate heel and toe, Learn, curve and smoothing.
6. **On-screen buttons.** Every footswitch job also has a button on the main page, so friends without pedals miss nothing.
7. **Pebble.** Taps its foot on a press, leans with the expression pedal.
8. **Keeper.** Hold C+D for 1 second runs Keeper. If Keeper isn't built in this plugin yet, reserve the combo and leave it silent.
9. **No brand names** anywhere in the UI. Use "Footswitches", "Footswitch A to D" and "Expression".
10. **README.** Copy Prism Patch's "Footswitches and expression (optional)" and "Getting footswitch MIDI to the plugin in Ableton" sections, with this plugin's table.
11. **Tests.** Tap, hold, A+B and C+D combos from plain 127/0 messages. A clash test: CC 20, 21, 22, 23 and 11 on channel 16 change nothing except the jobs in the table, with the original pedal map both off and on a different channel.

The defaults in the table are proposals based on the original pedal's footswitches. Kevin can change any of them before the build.

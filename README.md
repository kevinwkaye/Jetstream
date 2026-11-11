# Jetstream

A resynthesiser VST3 for Ableton Live, recreating the Hologram Electronics Infinite Jets pedal in the Flip Fella style, tuned for rich, smooth ambient post-rock.

## Install

1. Open the **builds** branch on GitHub and download `Jetstream-Windows.zip`.
2. Unzip it and copy `Jetstream.vst3` into `C:\Program Files\Common Files\VST3`.
3. In Ableton, open Preferences > Plug-Ins, turn on "Use VST3 Plug-In System Folders" and press Rescan.

`Jetstream-standalone.exe` in the same zip runs without Ableton if you want to try it on its own.

## How it plays

Jetstream listens for each new note or chord, records it on sampler A or B, then rebuilds it in the chosen voice.

| Control | What it does |
| --- | --- |
| Input: Guitar, Synth, Bass | Tunes note detection. Bass keeps everything under 120 Hz dry and tight |
| Learn, Sens | Learn listens for four seconds and sets the sensitivity to suit how hard you play |
| Trigger: Poly, Mono, Manual | Poly lets A and B overlap, Mono lets one sound at a time, Manual hands capture to the A and B buttons |
| Toggle, Latch, Moment | How A and B behave in Manual. Latch: double press to clear |
| Env Shape | Six volume shapes: decay, triangle, square, swell, rise, random. Between two shapes sets the Repeat and random rate |
| Env Time | 50 ms to 20 s, then Infinite (holds until the next note), then Repeat (loops the shape) |
| Voice | Blur 0, -1, +1, +/-1 (frozen, attack-free sustain with octaves), Synth A and B, Glitch A and B, Swell A and B, User A and B |
| Dimension | Space in Blur and Swell, filter in Synth, loop length in Glitch A, memory cell in Glitch B |
| LFO, Env, Off | What moves Dimension. Depth, Rate and Shape set the movement |
| Sync | Locks the LFO and Repeat Waveform to Ableton's tempo |
| Rec Dim, Rec Drv | Press, move the knob, press again: the movement loops for up to 10 s. Move the knob to stop it |
| Drive, Tone, Dry, Master | Output stage. Dry above 75% starts to fade the effect. Auto Level keeps Drive at a steady volume |
| Width, Air | Stereo spread of A and B, and a soft wash after the effect |
| Memory | Glitch B's six loops. Hold locks them, Clear empties them, click a cell to play it. They save with your set |
| User A, User B | The two User stops on the Voice dial. Hold one for a second to store the sound, tap it to recall. They save with your set |

MIDI notes on the track trigger the samplers with exact pitch. Every control can also be MIDI-mapped in Ableton.

## Footswitches and expression

Jetstream is played from four footswitches and an expression pedal. Every footswitch job also has a button on screen, so nothing needs pedals.

| Footswitch | Tap | Hold |
| --- | --- | --- |
| A | Sampler A. In Poly and Mono it catches whatever is ringing | Sustain A: the sound stops moving and rings until you let go |
| B | Sampler B, the same | Sustain B |
| C | Hold Memory on and off | Bypass |
| D | Next preset | Previous preset |
| A+B | Bypass | |
| C+D | Footswitches move to the next Jetstream | Held 1 s: Keeper |

In Manual, A and B are the pedal's own footswitches and follow Toggle, Latch or Momentary. A press starts the capture straight away, so a hold catches the first note. If the press turns out to be half of A+B, whatever it started is undone.

The footswitches send CC 20 to 23 on channel 16 by default and the expression pedal sends CC 11. **Learn** on the Pedal page remaps any of them, and **Calibrate** fixes a pedal that doesn't reach the ends or runs backwards. These settings are shared with every Flip Fella plugin on the computer.

Each preset chooses what the expression pedal moves and its heel and toe settings, shown under EXPRESSION. Pick another target on the Pedal page: Dimension, Env Time, Env Shape, Drive, Dry, LFO Depth, LFO Rate, Tone, Width or Air.

### Getting footswitch MIDI to the plugin in Ableton

Jetstream reads the footswitch's USB cable directly, so every copy works with no MIDI track. In Ableton's Link, Tempo and MIDI settings, switch Track, Sync and Remote off for the footswitch input so Live doesn't take the messages first. If you'd rather route it through Ableton, choose **ABLETON ONLY** on the Pedal page and send the footswitch to the track Jetstream sits on.

### Original MIDI map

Version 0.1 listened for CC 80 to 83 on any channel. That map now sits behind **ORIGINAL MIDI MAP** on the Pedal page, off by default, so it never collides with the footswitches. Pick a channel to turn it on: CC 80 and 81 press A and B, CC 82 toggles bypass and CC 83 toggles Hold.

## Presets

Twenty factory presets, each with a one-line **TRY THIS** hint and its own expression setting. The arrows and footswitch D step through them, and **SAVE** stores your own in Documents/Flip Fella Presets/Jetstream, after the factory ones.

1 INIT, 2 AMBIENT BED, 3 ENDLESS PAD, 4 SHIMMER, 5 SUB DRONE, 6 OCTAVE CATHEDRAL, 7 TIDE, 8 FILTER LEAD, 9 ENVELOPE FILTER, 10 EIGHTIES PAD, 11 SQUARE STAB, 12 STUTTER, 13 GLITCH RAIN, 14 SELF REMIX, 15 SWELL ECHOES, 16 TAPE WARBLE, 17 VIOLIN FUZZ, 18 BASS SUB BLOOM, 19 BASS SYNTH, 20 SYNTH PAD SHADOW.

## Sound

- Blur rebuilds each caught note as pure, phase-continuous sine partials, so a held note stays within 3 cents with no grain or wobble. Octave layers glide in behind it in tune.
- Synth picks the notes out of the captured sound, within 5 cents from audio and exact from MIDI.
- Glitch loops close on a matched point with the audio that led into them, so the wrap is silent. Glitch B memory keeps that lead-in when it's saved with the set.
- Drive runs at 4x oversampling with anti-derivative anti-aliasing, so it stays clean in the audible band.
- Bass mode splits at 120 Hz with a Linkwitz-Riley crossover, so the lows come through untouched and the bands sum flat.
- Triggers are judged against how loud you've been playing, so quiet playing still triggers.
- Voice changes, bypass, retriggers and Mono handovers all fade, so nothing clicks.
- The output has a soft ceiling at -1 dBFS.

`tests/engine_test.cpp` checks every one of these with a pass mark and renders all twenty presets. `tests/plugin_test.cpp` checks the footswitches, expression, presets and saved state. CI runs them on Windows and Linux ARM64 and reports CPU per instance with each build.

## Keeper

Hold C+D for a second, or press KEEPER, to keep the last 8 seconds with every setting. Keepers go to Documents/Flip Fella Keepers. Export them from the Pedal page.

## Building

GitHub Actions builds the Windows VST3 on every push to `main` and publishes it to the `builds` branch. To build locally you need CMake 3.22+ and a C++17 compiler; JUCE downloads automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

`engine_test` runs the sound checks (pass a folder to write WAV files), `plugin_test` the footswitch and preset checks, and `cpu_report` the CPU per instance at 48 kHz with 64 and 128 sample buffers.

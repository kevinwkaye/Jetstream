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
| A / Live / B | Hold A or B to save a preset, tap to recall. The User stops on the Voice dial do the same |

MIDI notes on the track trigger the samplers too. CC 80 and 81 press A and B, CC 82 toggles bypass and CC 83 toggles Hold. Every control can also be MIDI-mapped in Ableton.

## Building

GitHub Actions builds the Windows VST3 on every push to `main` and publishes it to the `builds` branch. To build locally you need CMake 3.22+ and a C++17 compiler; JUCE downloads automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

`jetstream_render_test` renders a guitar phrase through every voice and mode and fails on NaNs, clipping, clicks or silence. Pass a folder to write WAV files.

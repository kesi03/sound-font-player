# SF2 PLAYER

A Vibe FM firmware that plays SoundFont 2 (`.sf2`) files. Pick an instrument from the device menu,
browse fonts on host media, or upload one from the custom Editor page. It uses
[TinySoundFont](https://github.com/schellingb/TinySoundFont) as the synthesis engine.

## Features

- Keys, pads, MIDI and DAW notes play the selected preset immediately.
- A fallback demo voice plays when no SoundFont is loaded, so the instrument is never silent.
- Two static 16 MB font banks let a new font load while the current one keeps sounding.
- Device UI:
  - **SOUND** — instrument list, preset selection, volume, octave and transpose.
  - **FILES** — media browser for `.sf2` files, drag & drop support on plugin/browser hosts.
  - **CTRL** — three pages (F1 / F2): sound, FX 1 and FX 2; knobs 1-4 edit the four shown values.
  - **HELP** — HOME / F5 overview of every control.
  - F1 PREV, F2 NEXT (on CTRL: previous / next page), F3 AUDITION, F4 FILES / SOUND,
    F5 HELP, F6 ALL OFF.
  - K1 SOUND, K2 FILES, K3 CTRL, K4 AUDITION, K5 HELP, K6 ALL OFF.
  - Hold ENTER or DATA to turn on FOCUS; DATA then walks the on-screen controls.
- Editor page with `.sf2` upload, a preset dropdown, media-path loading and audition buttons.
- Twelve automatable parameters: `preset`, `volume`, `octave`, `transpose` and eight master FX controls.
- A 960 × 540 **stage**: a second, bigger picture in the plugin and Playground – a large preset browser,
  volume/octave/transpose sliders, master FX readouts, stereo meters and a live 25-key strip.

## Stage

Hosts with SDK 23 support open the stage in place of the device (`VIBE_STAGE_REPLACES`; press Esc / STAGE
to see the device again). Click or drag a preset row, drag a slider, or use the mouse wheel to step
presets – every element names the base control it stands for (encoder 1–4, F1/F2, DATA, the CTRL knobs).
The stage is only a view: the 480 × 320 display, keys, pads and encoders still do everything, and the
firmware sounds exactly the same when no host ever shows the stage.

The stage framebuffer lives in `firmware/sf2-player/main.c` (`vibe_stage` in `vibe_on_init`,
`vibe_on_stage` for drawing); `"stage": {"w": 960, "h": 540}` in `firmware.json` tells the host its size.
Stereo peak levels come from the audio flow through two single-writer values; nothing time-critical
runs in the stage callback.

## Master effects

A global effect chain runs after TinySoundFont (or the fallback demo voice) and before the final
soft clip, so it affects every preset. TinySoundFont does not implement SF2 chorus/reverb sends;
these are the firmware's own master effects.

Signal order: **drive -> chorus -> ping-pong delay -> stereo reverb -> tone -> tremolo**.

| Parameter | Default | What it does |
| --- | --- | --- |
| `drive` | 0 | Pre-gain into the final soft clipper (saturation). |
| `chorus` | 0.25 | Two LFO-modulated delay taps, one per channel. |
| `delay` | 0 | Wet level of a 375 ms cross-fed echo. |
| `feedback` | 0.35 | Echo repeats (scaled to a maximum of 0.85 to stay stable). |
| `reverb` | 0.32 | Wet level of a 4-comb / 2-allpass room per channel. |
| `decay` | 0.50 | Reverb feedback and damping (room length / brightness). |
| `tremolo` | 0 | LFO amplitude modulation, slightly offset between L and R. |
| `tone` | 1.00 | State-variable low-pass, 500 Hz ... 18 kHz (1 = open). |

All eight are normalised 0..1 parameters, available as named DAW parameters, sliders in the Editor,
and rows on the device's CTRL pages 2 and 3. The effects live in `firmware/sf2-player/fx.c` and use
static delay lines; nothing is allocated at run time.

## Build

The build needs Zig 0.13.0. The Vibe SDK is pinned to the same commit used by naminator.

```sh
sh scripts/fetch-sdk.sh
ZIG=/path/to/zig sh sdk/tools/build.sh ../firmware/sf2-player
```

Output: `sdk/build/sf2-player.wasm`, `sdk/build/sf2-player.zip` (includes firmware manifest,
icon and Editor page). Drop the `.zip` onto the Vibe FM VST window.

Validate:

```sh
node sdk/tools/vibe-check.mjs sdk/build/sf2-player.wasm \
  --manifest firmware/sf2-player/firmware.json --seconds 5
```

## Limits and implementation notes

- Maximum SoundFont size: **16 MB**. The file buffer and the two font arenas are static;
  the firmware uses no heap and no libc.
- Up to 256 presets and 64 simultaneous voices per font.
- The master FX chain adds about 280 KB of static delay-line memory and runs entirely in the audio flow.
- TinySoundFont is vendored unchanged in `firmware/sf2-player/vendor/tsf.h` (MIT license in
  `vendor/LICENSE.tsf`). `tsf_config.h` redirects its allocator and maths to static arenas and
  Vibe SDK DSP helpers; `tsf_port.c` is the only translation unit with `TSF_IMPLEMENTATION`.
- The audio flow owns all TinySoundFont rendering and note calls. The control flow only queues
  notes and publishes atomic state changes via the two-flow protocol.

## Editor protocol

Bytes sent with `vibe.send()`:

| Command | Payload | Meaning |
| --- | --- | --- |
| `0x10` | `len:u32`, `name_len:u8`, name | Begin upload |
| `0x11` | `offset:u32`, data | Upload chunk (2048 bytes recommended) |
| `0x12` | — | Commit and parse |
| `0x13` | — | Cancel |
| `0x20` | — | Request status |
| `0x21` | `index:u32` | Select preset |
| `0x22` | — | All notes off |
| `0x23` | `note:u8` | Audition a note |
| `0x30` | UTF-8 path | Load a font from host media |

Firmware replies start with `0x02`, followed by a status line, a `FONT:` line and one
`index:name` line per preset.

## Licenses

This firmware is Apache-2.0. TinySoundFont is MIT; see `firmware/sf2-player/vendor/LICENSE.tsf`.
The Vibe FM SDK is Apache-2.0 (see the SDK repository).

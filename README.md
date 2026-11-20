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
  - **CTRL** — parameter page.
  - **HELP** — HOME / F5 overview of every control.
  - F1 PREV, F2 NEXT, F3 AUDITION, F4 FILES, F5 HELP, F6 ALL OFF.
  - K1 SOUND, K2 FILES, K3 CTRL, K4 AUDITION, K5 HELP, K6 ALL OFF.
  - Hold ENTER or DATA to turn on FOCUS; DATA then walks the on-screen controls.
- Editor page with `.sf2` upload, a preset dropdown, media-path loading and audition buttons.
- Four automatable parameters: `preset`, `volume`, `octave`, `transpose`.

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

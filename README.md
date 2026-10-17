# Bandwidth Crusher

An audio effect inspired by "Bad Apple but only N kB can change per second" videos.
A short loop of audio plays on repeat, and the input keeps trying to rewrite it, but
only a limited number of **bytes per second** may change. Whatever can't be updated
keeps playing stale audio from earlier passes: smearing, ghosting, stutter and fizz.

Formats: **AU and VST3 for macOS** (Apple Silicon + Intel), plus a standalone app.

## Download and install

Grab `BandwidthCrusher-macOS.zip` from the
[latest build](https://github.com/bytebent/bandwidth-crusher/releases/tag/latest),
unzip it and follow `INSTALL.txt` (right-click `Install.command` → Open).

## Controls

| Group | Control | What it does |
|---|---|---|
| Bandwidth | Budget (0.5–1000 kB/s) | How many bytes of the loop may change per second. The main control. |
| | Bit depth (2–16) | Resolution of the stored loop. Fewer bits = more (cruder) samples per byte. |
| | Greedy ↔ Random | 0 fixes the worst errors first (clean); 1 picks at random (fizzy). |
| | Error threshold | Skip parts already this close to the input, saving budget. |
| Loop | Loop length (1 ms–4 s) or synced (1/64–4 bars) | Short = pitched buzz, long = smeary wash and ghosts. |
| | Memory | How much stale audio is left after 1 s. 100% = lingers forever. |
| Chunks | Chunk size (1–4096 samples) | Update single samples (hiss) or longer runs (stutters, fragments). |
| | Edge smoothing | Crossfades written chunks into the stale audio around them. |
| Output | Input / output gain, Dry/Wet | Output goes through a soft limiter. |
| Footer | Stereo budget | Shared between channels, or split evenly per channel. |
| | Reset when playback starts / Seed | Makes renders repeatable from the start of playback. |

The plugin adds 10 ms of latency, which it reports to the host, so Ableton compensates automatically.

## Repository layout

- `source/CrusherDSP.h` — the effect itself, real-time safe, no JUCE dependency
- `source/PluginProcessor.*`, `source/PluginEditor.*` — JUCE plugin wrapper
- `ui/index.html` — the interface (shown in a web view inside the plugin; also opens in a browser as a preview)
- `reference/bandwidth_fx.py` — the original Python version, used as the reference
- `tests/` — C++ vs Python comparison tests
- `tools/bandwidth_crusher.html` — the offline browser version with automation lanes

## Building

Every push to `main` builds the Mac plugin on GitHub Actions and publishes it as the
`latest` release. To build locally on a Mac (Xcode command line tools + CMake):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target BandwidthCrusher_AU BandwidthCrusher_VST3
```

## License note

Built with [JUCE](https://juce.com) 8, which is available under the AGPLv3 or a
commercial licence. Keeping this repository public and open source satisfies the
AGPL; if you ever want to sell the plugin as closed source, check JUCE's licence terms first.

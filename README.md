# NF Limiter V1.0 — Full Source Pack

**TRUE PEAK MASTERING LIMITER**  
**NF AUDIO TOOLS — BY NENNO FERNANDO**

This pack provides a JUCE/CMake implementation baseline, complete parameter/state model, lookahead limiter, stereo linking, adaptive release, three character modes, true-peak safety detector, meters, LUFS estimates, gain-reduction history, presets, manual menu, resizable UI and separated design assets.

## Controls and defaults

| Control | Range / choices | Default |
| --- | --- | --- |
| Gain | −12 to +24 dB | 0 dB |
| Ceiling | −12 to −0.1 dBTP | −1 dBTP |
| Release | 10–1000 ms | 150 ms |
| Auto Release | Off/On | On |
| Character | Clean/Punch/Loud | Clean |
| True Peak | Off/On | On |
| Oversampling | 1x/2x/4x/8x | 4x |
| Stereo Link | 0–100% | 100% |
| Bypass | Off/On | Off |

## Requested UI behavior

- Preset bar lists factory and user presets.
- SAVE opens a name dialog and stores a `.nflpreset` file.
- Three-line menu opens Portuguese/English manuals, preset folder, About and Reset Window Size.
- Bottom-right handle resizes the plugin between 860×560 and 1900×1250.
- Clicking the NF logo restores 1280×820.
- The host state saves every automatable parameter.

## Build

```bash
cmake -S . -B build -DJUCE_DIR=/absolute/path/to/JUCE
cmake --build build --config Release -j
```

Default: AU, VST3 and Standalone. Enable AAX only after configuring the Avid SDK:

```bash
cmake -S . -B build-aax -DJUCE_DIR=/path/to/JUCE -DNF_BUILD_AAX=ON
```

## Release-critical warning

The included limiter is a functional implementation baseline, not a claim of a certified commercial true-peak master. Before release, Claude must complete the items in `Docs/PROMPT_PARA_CLAUDE.txt`, including real oversampled true-peak detection compliant with the intended standard, click-free parameter smoothing, non-realtime oversampling reconfiguration, accurate EBU/ITU loudness metering, latency verification and exhaustive null/host tests.


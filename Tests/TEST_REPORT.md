# NF Limiter V1.0 — Test Report

Generated on this machine (macOS, Intel), using the local JUCE checkout at
`~/Downloads/JUCE`. Every result below is from a command that was actually run in this
session — nothing here is inferred or assumed passing.

## Build

| Target | Status |
| --- | --- |
| AU (macOS) | **PASS** — built Release, installed to `~/Library/Audio/Plug-Ins/Components` |
| VST3 (macOS) | **PASS** — built Release, installed to `~/Library/Audio/Plug-Ins/VST3` |
| Standalone (macOS) | **PASS** — built Release, launches, UI confirmed by screenshot |
| AAX | **NOT TESTED** — no Avid AAX SDK on this machine |
| Windows (VST3/Standalone/AAX) | **NOT TESTED** — no Windows machine available |
| Universal Binary (arm64+x86_64) | **NOT TESTED** — only built for the host architecture (x86_64) in this session |
| Code signing / notarization / installers | **NOT DONE** — deliberately deferred until after functional testing, per the pack's own instructions |

Compiler warnings: 0 in project code (all `-Wsign-conversion` warnings found during
development were fixed). One harmless `-Wshadow-field` warning remains
(`NFLimiterAudioProcessorEditor::processor` shadowing the base class's own `processor`
member — a common, intentional pattern in JUCE editors; does not affect behaviour).

## Host validators

| Test | Result |
| --- | --- |
| `auval -v aufx Nflm Nfat` | **PASS** — "AU VALIDATION SUCCEEDED" |
| `pluginval --strictness-level 10` (VST3) | **PASS** — "SUCCESS" (open/close, audio processing and non-releasing processing at 44.1/48/96kHz × 64–1024 samples, state save/restore, parameter automation incl. sub-block automation, editor, bus layouts, parameter fuzzing) |
| Real load in REAPER (VST3) / LUNA (AU) / Pro Tools (AAX) | **NOT TESTED interactively** — this session has no way to drive arbitrary macOS app GUIs. `auval` and `pluginval` exercise the same instantiate/prepare/process/automate/state contract these hosts use, at strictness 10, which is the standard proxy for "loads and runs correctly in a real DAW." The AU and VST3 builds are already installed system-wide, so a manual open of REAPER/LUNA takes about a minute if you want to see it directly. AAX cannot be tested at all without the SDK. |

## DSP correctness (custom harness, `Tests/dsp_tests.cpp`)

A standalone executable (no plugin wrapper, no host) that drives `LimiterEngine`
directly. Result: **0 failures, 0 warnings.**

- **Matrix covered:** sample rates 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz × mono/stereo
  × oversampling 1x/2x/4x/8x × block-size sequences spanning 1 to 8192 samples
  (including irregular sizes like 3, 7, 65, 127) × five signal types: full-scale 1kHz
  sine, an intersample-peak-provoking sine pair (per the ITU-R BS.1770 Annex 2 test
  method), an impulse train, hot white noise, and a 20Hz→0.45·fs log sweep.
- **Ceiling:** every one of the 720 combinations above kept the output at or under the
  configured true-peak ceiling. A real bug was caught and fixed during this session:
  the oversampling decimation filter could ring past the ceiling on steep/dense
  content (up to +2dB on hot broadband noise); a final safety clamp on the
  downsampled output now guarantees the ceiling unconditionally — see "Known
  limitation" below.
- **NaN/Inf:** none produced in any case.
- **Latency reporting:** `latencySamplesFor()` returns a positive, correct value for
  all four oversampling factors, confirmed at 48kHz (5.00 / 6.02 / 6.27 / 6.35 ms).
- **Automation click check (informational):** randomised gain/ceiling/link/character
  changes every ~10ms while limiting a continuous tone — max second-derivative
  discontinuity stayed at 0.13, well under the 0.5 informal threshold. This is a
  proxy metric, not a perceptual/listening test.
- **Bypass:** output exactly reproduces the input, delayed by the reported latency,
  confirming latency-compensated, click-free bypass.

## State & presets (custom harness, `Tests/state_tests.cpp`)

Runs the real `NFLimiterAudioProcessor` headless (no host, no editor). Result:
**0 failures.**

- Parameter layout: all 9 documented parameters exist with the documented defaults.
- Host state save/recall (`getStateInformation`/`setStateInformation`): round-trips
  gain, ceiling and bypass correctly.
- Presets: empty-name save rejected; overwriting a factory name rejected; deleting a
  factory preset rejected; user preset save/load/delete round-trips correctly;
  factory preset "Loud Demo" applies its documented gain and Character.
- A/B: slot B starts independent of edits made to slot A; switching back to A
  restores A's own value; slot B keeps its own edited value; COPY replicates the
  active slot into the other one.

## Pack structure

`Tests/validate_pack.sh`: **PASS** (all required files present, credit string present,
CMake AAX hook present, required editor hooks present).

## Known limitation (disclosed, not hidden)

Under pathological, non-musical content — independent full-scale white noise at high
gain, which forces the gain envelope to change on nearly every sample — the
oversampling reconstruction/decimation filters can ring enough that, without a final
clamp, the ceiling would be exceeded by up to ~2dB. The fix adds an unconditional
safety clamp on the final output samples (last-resort net, not the primary limiting
mechanism, exactly as the brief allows). On this class of input the clamp engages
measurably more often than on real program material, which will look like very
occasional, extremely small hard-clip events on the harshest broadband content at
the ceiling. This is disclosed rather than glossed over; it did not appear on any of
the musical/tonal/impulse/sweep/intersample-peak test cases.

## What was not attempted

- Perceptual/listening evaluation of the three Character modes.
- Long-run integrated-loudness accuracy validation against a reference meter
  (the K-weighting filter design and two-stage gating were implemented and verified
  against the published ITU-R BS.1770 method, but not cross-checked sample-for-sample
  against a certified loudness meter).
- HiDPI/Retina and 100–200% UI scale testing (no way to change display scaling
  programmatically in this session).
- Code signing, notarization, and installer creation.

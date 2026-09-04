# Manual — NF Limiter V1.0

**NF AUDIO TOOLS — BY NENNO FERNANDO**

## Quick workflow

1. Insert NF Limiter in the last master-bus slot.
2. Keep True Peak on and start with Ceiling at −1.0 dBTP for streaming.
3. Raise Gain while watching the central Gain Reduction meter.
4. Choose Clean for transparency, Punch for transients, or Loud for density.
5. Compare with bypass at a matched perceived level.

Gain drives the limiter; Ceiling defines the output cap; Release controls recovery; Auto adapts recovery to program material. Stereo Link at 100% maintains stereo stability.

True Peak's own internal oversampling is now fully automatic: NF Limiter picks the processing and True-Peak-detection precision from your project's sample rate alone (higher precision at 44.1/48 kHz, proportionally lighter at high sample rates), so there is no OVERSAMPLING control to set and no CPU/quality trade-off to manage — accuracy and CPU cost both stay appropriate to the session automatically, and switching sample rate never changes the plugin's latency mid-session.

DELTA / LISTEN is a monitoring-only control next to CHARACTER: engage it to hear exactly what the limiter is removing or changing — gain reduction, Character's colouration, and anything shaped by the Ceiling — instead of the normal output. It never alters the audio sent to the host when it is off, is never saved in a preset or session, always starts off when a session or preset loads, and is cancelled automatically while Bypass is engaged.

Use the preset bar to load settings and SAVE to create user presets. The three-line menu opens manuals, preset location, About and Reset Window Size. Drag the lower-right handle to resize. Click the NF logo to restore the default size.

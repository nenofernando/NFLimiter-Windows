# Pixel-exact assets from the approved concept

These PNGs are direct crops from `Assets/Reference/NF_Limiter_approved_concept.png`. They were not redrawn, recoloured or regenerated. The reference canvas is exactly 1536×1024.

Use `LAYOUT_MAP.json` as the authoritative geometry. Build the JUCE editor in a 1536×1024 design coordinate space, apply one uniform scale transform, and letterbox/center if the host aspect ratio differs. Do not independently stretch knobs, meters or panels.

The `*_complete.png` crops include their original panel context and are the strongest comparison references. Smaller crops define exact texture, border, typography, glow and spacing. For live controls, reproduce the appearance procedurally or prepare state strips from these pixels; do not use AI-generated substitutes.

At final QA, capture the plugin at 1536×1024 and overlay it at 50% opacity over the approved reference. Major borders, centers and labels should coincide.

#!/usr/bin/env bash
set -euo pipefail
r="$(cd "$(dirname "$0")/.."&&pwd)"
for f in CMakeLists.txt README.md Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/LimiterEngine.cpp Source/Metering.cpp Source/PresetManager.cpp Docs/MANUAL_PT.md Docs/MANUAL_EN.md Docs/PROMPT_PARA_CLAUDE.txt;do test -s "$r/$f"||{ echo "Missing $f";exit 1;};done
test "$(find "$r/Assets/SVG" -name '*.svg'|wc -l|tr -d ' ')" -ge 12
test "$(find "$r/Assets/PNG" -name '*.png'|wc -l|tr -d ' ')" -ge 12
grep -q 'BY NENNO FERNANDO' "$r/Docs/MANUAL_PT.md"
grep -q 'NF_BUILD_AAX' "$r/CMakeLists.txt"
grep -q 'Reset Window Size' "$r/Source/PluginEditor.cpp"
grep -q 'presets.save' "$r/Source/PluginEditor.cpp"
grep -q 'copyToOther' "$r/Source/PluginEditor.cpp"
echo "NF Limiter pack structure: PASS"

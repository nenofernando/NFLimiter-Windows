cmake_minimum_required(VERSION 3.22)
project(NFLimiter VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "JUCE source directory")
if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
  add_subdirectory("${JUCE_DIR}" JUCE)
else()
  find_package(JUCE CONFIG REQUIRED)
endif()
option(NF_BUILD_AAX "Build AAX when Avid SDK is configured" OFF)
set(NF_FORMATS AU VST3 Standalone)
if(NF_BUILD_AAX)
  list(APPEND NF_FORMATS AAX)
endif()
juce_add_plugin(NFLimiter
  COMPANY_NAME "NF Audio Tools"
  BUNDLE_ID "com.nfaudiotools.nflimiter"
  PLUGIN_MANUFACTURER_CODE Nfat
  PLUGIN_CODE Nflm
  FORMATS ${NF_FORMATS}
  PRODUCT_NAME "NF Limiter"
  DESCRIPTION "True Peak Mastering Limiter"
  IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(NFLimiter PRIVATE
  Source/PluginProcessor.cpp Source/PluginProcessor.h
  Source/PluginEditor.cpp Source/PluginEditor.h
  Source/LimiterEngine.cpp Source/LimiterEngine.h
  Source/Metering.cpp Source/Metering.h
  Source/PresetManager.cpp Source/PresetManager.h
  Source/Theme.h)
juce_generate_juce_header(NFLimiter)
target_compile_definitions(NFLimiter PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

# Manuals only: the chassis (header, logo, title, panels, footer) is drawn continuously
# in JUCE rather than pasted from bitmap crops, so no rectangular seam can appear behind
# any element — Assets/Exact_PNG/LAYOUT_MAP.json and colour-sampling from the reference
# image are used for geometry/texture only, never as literal overlaid images.
juce_add_binary_data(NFLimiterResources SOURCES
  Docs/MANUAL_PT.md
  Docs/MANUAL_EN.md)
set_target_properties(NFLimiterResources PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(NFLimiter PRIVATE juce::juce_audio_utils juce::juce_dsp NFLimiterResources
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(NF_BUILD_TESTS "Build the standalone DSP test harness" ON)
if(NF_BUILD_TESTS)
  add_executable(NFLimiterDspTests Tests/dsp_tests.cpp Source/LimiterEngine.cpp Source/Metering.cpp)
  target_compile_definitions(NFLimiterDspTests PRIVATE
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
  target_link_libraries(NFLimiterDspTests PRIVATE
    juce::juce_dsp juce::juce_audio_basics juce::juce_audio_formats juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

  add_executable(NFLimiterStateTests Tests/state_tests.cpp
    Source/PluginProcessor.cpp Source/LimiterEngine.cpp Source/Metering.cpp Source/PresetManager.cpp)
  target_compile_definitions(NFLimiterStateTests PRIVATE
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 NF_LIMITER_HEADLESS_TESTS=1)
  target_link_libraries(NFLimiterStateTests PRIVATE
    juce::juce_audio_processors juce::juce_dsp juce::juce_audio_utils
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()


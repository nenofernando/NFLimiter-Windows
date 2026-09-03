#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LimiterEngine.h"
#include "Metering.h"
#include "PresetManager.h"

class NFLimiterAudioProcessor final : public juce::AudioProcessor,
                                       private juce::AudioProcessorValueTreeState::Listener,
                                       private juce::AsyncUpdater
{
public:
    NFLimiterAudioProcessor();
    ~NFLimiterAudioProcessor() override;

    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "NF Limiter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout();

    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets;
    Metering metering;
    static constexpr int historyLength = 256;
    std::array<std::atomic<float>, historyLength> history {};
    std::atomic<int> historyWrite { 0 };

private:
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;

    // The history graph's time resolution must not depend on the host's arbitrary
    // block size (a host using large blocks would otherwise produce a choppy graph
    // with long flat segments) — processBlock slices the buffer into fixed small
    // chunks and calls the engine once per chunk, so a history point is pushed every
    // ~2-3ms of audio regardless of the host's own block size.
    static constexpr int historyChunkSamples = 128;
    float displayedGrForHistory = 0.0f; // audio-thread-only: short visual-only smoothing

    LimiterEngine limiter;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NFLimiterAudioProcessor)
};

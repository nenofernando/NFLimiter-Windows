#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "LimiterEngine.h"
#include "Metering.h"
#include "PresetManager.h"

class NFLimiterAudioProcessor final : public juce::AudioProcessor
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
    // 960 buckets at 5ms each ≈ 4.8s of real GAIN REDUCTION history (not a spectrum
    // analyser — this is gain reduction over time), same total span as the previous
    // 600 * 8ms — only the time resolution changed. Bucket duration is fixed in
    // *milliseconds*, converted to samples from the actual sample rate in
    // prepareToPlay(), so the represented time span stays ~5s regardless of whether
    // the host runs at 44.1kHz or 192kHz.
    static constexpr int historyLength = 960;
    std::array<std::atomic<float>, historyLength> history {};
    std::atomic<int> historyWrite { 0 };

private:
    // 5ms buckets (was 8ms) so narrow drum-hit-length gain-reduction transients get
    // their own bucket instead of being merged into a wider window.
    static constexpr double historyBucketMs = 5.0;
    int historyChunkSamples = 128; // recomputed from the real sample rate in prepareToPlay
    float displayedGrForHistory = 0.0f; // audio-thread-only: short visual-only smoothing

    LimiterEngine limiter;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NFLimiterAudioProcessor)
};

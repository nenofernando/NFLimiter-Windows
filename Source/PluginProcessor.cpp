#include "PluginProcessor.h"
#ifndef NF_LIMITER_HEADLESS_TESTS
 #include "PluginEditor.h"
#endif

namespace
{
    constexpr int kOversamplingFactors[] { 1, 2, 4, 8 };

    int oversamplingFactorFromChoice(float choiceValue)
    {
        return kOversamplingFactors[juce::jlimit(0, 3, juce::roundToInt(choiceValue))];
    }
}

NFLimiterAudioProcessor::NFLimiterAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "NF_LIMITER_STATE", layout()),
      presets(apvts)
{
    for (auto& v : history) v = 0.0f;
    apvts.addParameterListener("oversampling", this);
}

NFLimiterAudioProcessor::~NFLimiterAudioProcessor()
{
    apvts.removeParameterListener("oversampling", this);
    cancelPendingUpdate();
}

namespace
{
    auto oneDecimalWithSuffix(const char* suffix)
    {
        return [suffix](float v, int) { return juce::String(v, 1) + suffix; };
    }
    auto integerWithSuffix(const char* suffix)
    {
        return [suffix](float v, int) { return juce::String(juce::roundToInt(v)) + suffix; };
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NFLimiterAudioProcessor::layout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using C = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<F>("gain", "Gain", juce::NormalisableRange<float>(-12.0f, 24.0f), 0.0f,
                               juce::AudioParameterFloatAttributes().withStringFromValueFunction(oneDecimalWithSuffix(" dB"))));
    l.add(std::make_unique<F>("ceiling", "Ceiling", juce::NormalisableRange<float>(-12.0f, -0.1f), -1.0f,
                               juce::AudioParameterFloatAttributes().withStringFromValueFunction(oneDecimalWithSuffix(" dB"))));
    l.add(std::make_unique<F>("release", "Release", juce::NormalisableRange<float>(10.0f, 1000.0f, 0.0f, 0.4f), 150.0f,
                               juce::AudioParameterFloatAttributes().withStringFromValueFunction(integerWithSuffix(" ms"))));
    l.add(std::make_unique<B>("auto_release", "Auto Release", true));
    l.add(std::make_unique<C>("character", "Character", juce::StringArray { "Clean", "Punch", "Loud" }, 0));
    l.add(std::make_unique<B>("true_peak", "True Peak", true));
    l.add(std::make_unique<C>("oversampling", "Oversampling", juce::StringArray { "1x", "2x", "4x", "8x" }, 2));
    l.add(std::make_unique<F>("stereo_link", "Stereo Link", juce::NormalisableRange<float>(0.0f, 100.0f), 100.0f,
                               juce::AudioParameterFloatAttributes().withStringFromValueFunction(integerWithSuffix(" %"))));
    l.add(std::make_unique<B>("bypass", "Bypass", false));
    return l;
}

void NFLimiterAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int factor = oversamplingFactorFromChoice(apvts.getRawParameterValue("oversampling")->load());
    limiter.requestOversamplingFactor(factor);
    limiter.prepare(sampleRate, samplesPerBlock, getTotalNumOutputChannels());
    metering.prepare(sampleRate, getTotalNumOutputChannels());
    setLatencySamples(limiter.latencySamples());
}

void NFLimiterAudioProcessor::releaseResources()
{
    limiter.reset();
    metering.reset();
}

bool NFLimiterAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == l.getMainOutputChannelSet()
        && (l.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

void NFLimiterAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    metering.captureInput(buffer);

    auto raw = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };

    const int factor = oversamplingFactorFromChoice(raw("oversampling"));
    limiter.requestOversamplingFactor(factor);

    limiter.setParameters(raw("gain"), raw("ceiling"), raw("release"), raw("auto_release") > 0.5f,
                           (LimiterEngine::Character) juce::roundToInt(raw("character")),
                           raw("stereo_link"), raw("true_peak") > 0.5f, raw("bypass") > 0.5f);
    limiter.process(buffer);

    metering.captureOutput(buffer, limiter.gainReductionDb(), limiter.truePeakDb());
    history[(size_t) (historyWrite.fetch_add(1) % historyLength)] = limiter.gainReductionDb();
}

void NFLimiterAudioProcessor::parameterChanged(const juce::String& parameterID, float)
{
    if (parameterID == "oversampling") triggerAsyncUpdate();
}

void NFLimiterAudioProcessor::handleAsyncUpdate()
{
    // Runs on the message thread: safe place to renegotiate latency with the host
    // after an oversampling-factor change (the audio thread only flips an atomic).
    const int factor = oversamplingFactorFromChoice(apvts.getRawParameterValue("oversampling")->load());
    setLatencySamples(limiter.latencySamplesFor(factor));
}

void NFLimiterAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void NFLimiterAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

#ifndef NF_LIMITER_HEADLESS_TESTS
juce::AudioProcessorEditor* NFLimiterAudioProcessor::createEditor() { return new NFLimiterAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NFLimiterAudioProcessor(); }
#else
juce::AudioProcessorEditor* NFLimiterAudioProcessor::createEditor() { return nullptr; }
#endif

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
}

NFLimiterAudioProcessor::~NFLimiterAudioProcessor() = default;

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
    historyChunkSamples = juce::jmax(1, (int) std::round(sampleRate * (historyBucketMs / 1000.0)));
    displayedGrForHistory = 0.0f;
    normalOutputScratch.setSize(getTotalNumOutputChannels(), juce::jmax(1, samplesPerBlock), false, false, true);
    // DELTA/LISTEN always starts OFF on a fresh prepare (new instance, or a
    // sample-rate/config change) -- it is a monitoring toggle, never a saved value.
    setDeltaListenEnabled(false);
    limiter.setDeltaListenEnabled(false);
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

    const bool bypassedNow = raw("bypass") > 0.5f;
    limiter.setParameters(raw("gain"), raw("ceiling"), raw("release"), raw("auto_release") > 0.5f,
                           (LimiterEngine::Character) juce::roundToInt(raw("character")),
                           raw("stereo_link"), raw("true_peak") > 0.5f, bypassedNow);
    // DELTA/LISTEN: read once per block, same as every other parameter above, but this
    // one is deliberately NOT an APVTS parameter (see the field's own comment in
    // PluginProcessor.h) -- it is a monitoring toggle, not a sonic one.
    limiter.setDeltaListenEnabled(isDeltaListenEnabled());

    // Process in fixed ~8ms sub-chunks (block-size-invariant DSP — proven by the test
    // suite) purely so each GAIN REDUCTION HISTORY bucket holds the real minimum gain
    // seen in ITS OWN ~8ms window — a data point roughly every 8ms, independent of the
    // host's own (possibly much larger, or sample-rate-scaled) block size.
    const float releaseMsParam = raw("release");
    const float bucketMs = 1000.0f * (float) historyChunkSamples / (float) getSampleRate();
    const float releaseCoeff = std::exp(-bucketMs / juce::jmax(1.0f, releaseMsParam));

    float blockMinGrDb = 0.0f;
    // limiter.truePeakDb() is a snapshot overwritten on every process() call — with the
    // host block sliced into several ~5-8ms sub-chunks below, reading it only once
    // *after* the loop would silently report only the *last* sub-chunk's true peak,
    // dropping a genuinely higher peak that happened earlier in the same host block
    // (the exact bug behind PEAK reading higher than TRUE PEAK: PEAK is computed once
    // below over the *whole* buffer, so it never had this blind spot). Track the block-
    // wide max explicitly so both meters are measured over the same window.
    float blockMaxTruePeakDb = -100.0f;
    int pos = 0;
    const int totalSamples = buffer.getNumSamples();
    while (pos < totalSamples)
    {
        const int chunk = juce::jmin(historyChunkSamples, totalSamples - pos);
        juce::AudioBuffer<float> sub(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), pos, chunk);
        limiter.process(sub);
        blockMaxTruePeakDb = juce::jmax(blockMaxTruePeakDb, limiter.truePeakDb());

        // Accumulate the NORMAL (pre-DELTA/LISTEN) output for this sub-chunk into a
        // whole-block scratch buffer -- `sub` itself may already hold the Delta-mixed
        // monitoring signal by now (that's what actually reaches the host), so
        // metering below reads this snapshot instead, keeping Peak/True
        // Peak/LUFS/TRUE PEAK OVER accurate to the real master output regardless of
        // whether Listen is engaged. Plain sample copies into pre-allocated storage --
        // no allocation.
        auto normalView = limiter.normalOutputForMetering();
        for (int ch = 0; ch < normalView.getNumChannels(); ++ch)
            normalOutputScratch.copyFrom(ch, pos, normalView, ch, 0, chunk);

        // bucketGrDb: the real minimum (deepest) gain reduction the limiter applied
        // anywhere in this ~8ms bucket — never the input Gain, never input/output
        // Peak, only Decibels::gainToDecibels(appliedGain) from the limiter itself.
        float bucketGrDb = limiter.gainReductionDb();
        if (! std::isfinite(bucketGrDb)) bucketGrDb = 0.0f;
        blockMinGrDb = juce::jmin(blockMinGrDb, bucketGrDb);

        // Visual-only ballistics for the history graph (never affects audio or the
        // numeric GR readout, which reads Metering's own value): attack is immediate
        // so real transients are never smeared away or smoothed downward; release
        // follows the limiter's own Release time so the trace's decay looks like the
        // limiter's actual behaviour instead of an arbitrary fixed rate; it eases back
        // to 0dB rather than freezing on a stale reduction while Bypass is engaged.
        if (bypassedNow) displayedGrForHistory += 0.35f * (0.0f - displayedGrForHistory);
        else if (bucketGrDb < displayedGrForHistory) displayedGrForHistory = bucketGrDb;
        else displayedGrForHistory = bucketGrDb + releaseCoeff * (displayedGrForHistory - bucketGrDb);

        history[(size_t) (historyWrite.fetch_add(1) % historyLength)] = displayedGrForHistory;
        pos += chunk;
    }

    juce::AudioBuffer<float> normalWhole(normalOutputScratch.getArrayOfWritePointers(), buffer.getNumChannels(), totalSamples);
    metering.captureOutput(normalWhole, blockMinGrDb, blockMaxTruePeakDb, raw("ceiling"), bypassedNow);
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
    // DELTA/LISTEN is a monitoring toggle, never a saved value -- every session/preset
    // recall forces it back off, regardless of whatever state this instance was in
    // before the load. Only the atomic is touched here (setStateInformation() has no
    // guarantee of not running concurrently with the audio thread, unlike
    // prepareToPlay()); the next processBlock() forwards it into the engine safely,
    // the same as every other block.
    setDeltaListenEnabled(false);
}

#ifndef NF_LIMITER_HEADLESS_TESTS
juce::AudioProcessorEditor* NFLimiterAudioProcessor::createEditor() { return new NFLimiterAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NFLimiterAudioProcessor(); }
#else
juce::AudioProcessorEditor* NFLimiterAudioProcessor::createEditor() { return nullptr; }
#endif

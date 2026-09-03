#include "LimiterEngine.h"

void LimiterEngine::prepare(double sampleRate, int maximumBlockSize, int newNumChannels)
{
    baseSampleRate = sampleRate;
    maxBlockSize = maximumBlockSize;
    numChannels = juce::jlimit(1, 2, newNumChannels);
    lookaheadBaseSamples = juce::jmax(1, (int) std::round(baseSampleRate * ((double) kLookaheadMs / 1000.0)));

    for (int i = 0; i < kNumOsSlots; ++i)
    {
        const int factor = kOsFactors[i];
        lookaheadOsSamples[(size_t) i] = lookaheadBaseSamples * factor;

        if (factor == 1)
        {
            oversamplers[(size_t) i].reset();
            osLatencyBaseSamples[(size_t) i] = 0;
        }
        else
        {
            const int stages = (int) std::round(std::log2((double) factor));
            oversamplers[(size_t) i] = std::make_unique<juce::dsp::Oversampling<float>>(
                (size_t) numChannels, (size_t) stages,
                juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
            oversamplers[(size_t) i]->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
            osLatencyBaseSamples[(size_t) i] = (int) std::round(oversamplers[(size_t) i]->getLatencyInSamples());
        }
    }

    delayCapacity = lookaheadOsSamples[kNumOsSlots - 1] + 64;
    minRingCapacity = delayCapacity;

    for (auto& cs : channelState)
    {
        cs.delay.assign((size_t) delayCapacity, 0.0f);
        cs.dryDelay.assign((size_t) delayCapacity, 0.0f);
        cs.minIdxRing.assign((size_t) minRingCapacity, (juce::int64) 0);
        cs.minValRing.assign((size_t) minRingCapacity, 0.0f);
    }

    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    activeOsFactor.store(wanted);
    pendingOsFactor.store(wanted);

    inputGainSmoothed.reset(baseSampleRate, 0.02);
    ceilingSmoothed.reset(baseSampleRate, 0.02);
    stereoLinkSmoothed.reset(baseSampleRate, 0.02);
    bypassMix.reset(baseSampleRate, 0.006);
    inputGainSmoothed.setCurrentAndTargetValue(1.0f);
    ceilingSmoothed.setCurrentAndTargetValue(0.89125f);
    stereoLinkSmoothed.setCurrentAndTargetValue(1.0f);
    bypassMix.setCurrentAndTargetValue(0.0f);

    reset();
}

void LimiterEngine::reset()
{
    writePos = 0; readPos = 0;
    for (auto& cs : channelState)
    {
        std::fill(cs.delay.begin(), cs.delay.end(), 0.0f);
        std::fill(cs.dryDelay.begin(), cs.dryDelay.end(), 0.0f);
        cs.minHead = 0; cs.minCount = 0; cs.currentGain = 1.0f; cs.heldDigitalPeak = 0.0f;
    }
    for (auto& os : oversamplers) if (os) os->reset();
    currentGrDb.store(0.0f);
}

void LimiterEngine::switchToPendingFactorIfNeeded()
{
    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    if (wanted == activeOsFactor.load()) return;

    activeOsFactor.store(wanted);
    writePos = 0; readPos = 0;
    for (auto& cs : channelState)
    {
        std::fill(cs.delay.begin(), cs.delay.end(), 0.0f);
        std::fill(cs.dryDelay.begin(), cs.dryDelay.end(), 0.0f);
        cs.minHead = 0; cs.minCount = 0; cs.currentGain = 1.0f; cs.heldDigitalPeak = 0.0f;
    }
    const int idx = factorToIndex(wanted);
    if (oversamplers[(size_t) idx]) oversamplers[(size_t) idx]->reset();

    const double osRate = baseSampleRate * (double) wanted;
    inputGainSmoothed.reset(osRate, 0.02);
    ceilingSmoothed.reset(osRate, 0.02);
    stereoLinkSmoothed.reset(osRate, 0.02);
    bypassMix.reset(osRate, 0.006);
}

void LimiterEngine::setParameters(float inputGainDb, float ceilingDbTP, float releaseMsIn, bool autoReleaseIn,
                                   Character characterIn, float stereoLinkPercent, bool truePeakIn, bool bypassedIn)
{
    inputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb));
    ceilingSmoothed.setTargetValue(juce::Decibels::decibelsToGain(ceilingDbTP));
    stereoLinkSmoothed.setTargetValue(juce::jlimit(0.0f, 1.0f, stereoLinkPercent));
    releaseMs = juce::jlimit(10.0f, 1000.0f, releaseMsIn);
    autoRelease = autoReleaseIn;
    character = characterIn;
    truePeakEnabled = truePeakIn;
    bypassed = bypassedIn;
}

float LimiterEngine::computeReleaseCoeff(float target, double sampleRateAtFactor) const noexcept
{
    float effectiveRelease = releaseMs;
    if (autoRelease)
    {
        const float reductionDb = juce::jlimit(0.0f, 12.0f, -juce::Decibels::gainToDecibels(juce::jmax(target, 1.0e-6f)));
        effectiveRelease *= juce::jmap(reductionDb, 0.0f, 12.0f, 0.65f, 2.2f);
    }
    if (character == Character::punch) effectiveRelease *= 0.65f;
    else if (character == Character::loud) effectiveRelease *= 1.65f;
    effectiveRelease = juce::jmax(1.0f, effectiveRelease);
    return std::exp(-1.0f / (0.001f * effectiveRelease * (float) sampleRateAtFactor));
}

void LimiterEngine::applyCharacter(float& y) const noexcept
{
    if (character == Character::loud)
        y = std::tanh(y * 1.15f) / std::tanh(1.15f); // gentle density on limited peaks
    // clean: bit-transparent, no colouration. punch: transparent too — its identity is
    // entirely in the faster adaptive release above, which keeps transients snappier.
}

void LimiterEngine::pushMin(ChannelState& cs, juce::int64 idx, float value, int ringCapacity) noexcept
{
    while (cs.minCount > 0)
    {
        const int backPos = (cs.minHead + cs.minCount - 1) % ringCapacity;
        if (cs.minValRing[(size_t) backPos] >= value) --cs.minCount;
        else break;
    }
    const int pos = (cs.minHead + cs.minCount) % ringCapacity;
    cs.minValRing[(size_t) pos] = value;
    cs.minIdxRing[(size_t) pos] = idx;
    ++cs.minCount;
}

void LimiterEngine::popExpiredMin(ChannelState& cs, juce::int64 minAllowedIdx, int ringCapacity) noexcept
{
    while (cs.minCount > 0 && cs.minIdxRing[(size_t) cs.minHead] < minAllowedIdx)
    {
        cs.minHead = (cs.minHead + 1) % ringCapacity;
        --cs.minCount;
    }
}

int LimiterEngine::latencySamplesFor(int factor) const noexcept
{
    return lookaheadBaseSamples + osLatencyBaseSamples[(size_t) factorToIndex(factor)];
}

void LimiterEngine::process(juce::AudioBuffer<float>& buffer)
{
    switchToPendingFactorIfNeeded();

    const int factor = activeOsFactor.load();
    const int idx = factorToIndex(factor);
    const int chans = juce::jmin(numChannels, buffer.getNumChannels());

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::AudioBlock<float> osBlock = block;
    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        osBlock = oversamplers[(size_t) idx]->processSamplesUp(block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    const int lookaheadOs = lookaheadOsSamples[(size_t) idx];
    const double osSampleRate = baseSampleRate * (double) factor;
    float minGainThisBlock = 1.0f;
    float maxAbsOutThisBlock = 0.0f;

    for (int i = 0; i < osNumSamples; ++i)
    {
        const float gainNow = inputGainSmoothed.getNextValue();
        const float ceilNow = ceilingSmoothed.getNextValue();
        const float linkNow = stereoLinkSmoothed.getNextValue();
        // Bypass is a request, not an instant switch: crossfade the output between the
        // dry and wet paths over a few ms so toggling it never clicks.
        bypassMix.setTargetValue(bypassed ? 1.0f : 0.0f);
        const float mixToDry = bypassMix.getNextValue();

        float peak[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            float xRaw = osBlock.getChannelPointer((size_t) ch)[i];
            if (! std::isfinite(xRaw)) xRaw = 0.0f;
            cs.dryDelay[(size_t) (writePos % delayCapacity)] = xRaw; // untouched — Gain never applied here

            // Gain is applied exactly once, right here, only for the wet (limited) path.
            const float xGained = xRaw * gainNow;
            cs.delay[(size_t) (writePos % delayCapacity)] = xGained;

            const float instantAbs = std::abs(xGained);
            if ((writePos % factor) == 0) cs.heldDigitalPeak = instantAbs;
            peak[ch] = truePeakEnabled ? instantAbs : cs.heldDigitalPeak;
        }

        const float linkedMax = chans > 1 ? juce::jmax(peak[0], peak[1]) : peak[0];

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            const float detector = juce::jmap(linkNow, peak[ch], linkedMax);
            float target = bypassed ? 1.0f
                                     : (detector > ceilNow && detector > 1.0e-9f ? ceilNow / detector : 1.0f);
            target = juce::jlimit(0.0f, 1.0f, target);
            pushMin(cs, writePos, target, minRingCapacity);
        }

        ++writePos;

        if (writePos - readPos > lookaheadOs)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto& cs = channelState[(size_t) ch];
                popExpiredMin(cs, readPos, minRingCapacity);
                const float windowMin = cs.minCount > 0 ? cs.minValRing[(size_t) cs.minHead] : 1.0f;

                if (bypassed)
                {
                    // No stale gain reduction or ramping left over for when Bypass
                    // comes back off — the gain computer sits at rest, ready to go.
                    cs.currentGain = 1.0f;
                }
                else if (windowMin < cs.currentGain)
                    cs.currentGain = windowMin; // instant foresight-based attack (already ramped by the window)
                else
                    cs.currentGain = windowMin + computeReleaseCoeff(windowMin, osSampleRate) * (cs.currentGain - windowMin);

                float wet = cs.delay[(size_t) (readPos % delayCapacity)] * cs.currentGain;
                applyCharacter(wet);
                wet = juce::jlimit(-ceilNow, ceilNow, wet); // last-resort safety net, not the primary mechanism
                const float dry = cs.dryDelay[(size_t) (readPos % delayCapacity)];

                const float y = wet + mixToDry * (dry - wet);
                osBlock.getChannelPointer((size_t) ch)[i] = y;

                const float effectiveGain = cs.currentGain + mixToDry * (1.0f - cs.currentGain);
                minGainThisBlock = juce::jmin(minGainThisBlock, effectiveGain);
                maxAbsOutThisBlock = juce::jmax(maxAbsOutThisBlock, std::abs(y));
            }
            ++readPos;
        }
        else
        {
            for (int ch = 0; ch < chans; ++ch)
                osBlock.getChannelPointer((size_t) ch)[i] = 0.0f;
        }
    }

    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        oversamplers[(size_t) idx]->processSamplesDown(block);

    // Final safety net: the gain computer above guarantees the ceiling in the
    // oversampled domain, but the decimation (anti-imaging) filter used to get back to
    // the base rate can ring on steep/high-frequency gain modulation (dense broadband
    // material right at the ceiling) and overshoot by a fraction of a dB. This clamp on
    // the actual output samples is the true, unconditional ceiling guarantee — it is a
    // last-resort net, not the primary limiting mechanism, and is inaudible on program
    // material since it only ever trims filter ringing, not real gain reduction.
    // Bypassed is a true, unclamped pass-through, so this net is skipped while bypassed
    // (it would otherwise clip dry material that legitimately exceeds the ceiling).
    if (! bypassed)
    {
        const float ceilingNow = ceilingSmoothed.getCurrentValue();
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                d[i] = juce::jlimit(-ceilingNow, ceilingNow, d[i]);
        }
    }

    currentGrDb.store(juce::Decibels::gainToDecibels(minGainThisBlock, -60.0f));
    currentTruePeakDb.store(juce::Decibels::gainToDecibels(maxAbsOutThisBlock, -100.0f));
}

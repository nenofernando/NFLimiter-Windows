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

    // Unlike the wet path's lookahead ring (where read trails write by a bounded
    // lookaheadOs at all times, interleaved sample-by-sample), the dry path writes an
    // entire block and then reads it back in a second pass — so the ring must hold at
    // least one full block on top of the latency, or a large block wraps around and
    // clobbers samples before they're ever read.
    int maxTotalLatency = 0;
    for (int i = 0; i < kNumOsSlots; ++i)
        maxTotalLatency = juce::jmax(maxTotalLatency, lookaheadBaseSamples + osLatencyBaseSamples[(size_t) i]);
    dryDelayCapacity = maxTotalLatency + juce::jmax(1, maxBlockSize) + 64;

    for (auto& cs : channelState)
    {
        cs.delay.assign((size_t) delayCapacity, 0.0f);
        cs.dryBaseDelay.assign((size_t) dryDelayCapacity, 0.0f);
        cs.minIdxRing.assign((size_t) minRingCapacity, (juce::int64) 0);
        cs.minValRing.assign((size_t) minRingCapacity, 0.0f);
    }

    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    activeOsFactor.store(wanted);
    pendingOsFactor.store(wanted);

    inputGainSmoothed.reset(baseSampleRate, 0.02);
    ceilingSmoothed.reset(baseSampleRate, 0.02);
    stereoLinkSmoothed.reset(baseSampleRate, 0.02);
    truePeakBlend.reset(baseSampleRate, 0.02);
    // 6ms was too fast: with real program material, active gain reduction removed
    // almost instantly on toggle produced an audible thump/click on bass-heavy content.
    // 30ms still reads as an instant A/B switch but is long enough (well over one cycle
    // even at ~30Hz) to smooth over the wet/dry level jump.
    bypassMix.reset(baseSampleRate, 0.03); // base rate — the dry crossfade runs post-downsample
    inputGainSmoothed.setCurrentAndTargetValue(1.0f);
    ceilingSmoothed.setCurrentAndTargetValue(0.89125f);
    stereoLinkSmoothed.setCurrentAndTargetValue(1.0f);
    bypassMix.setCurrentAndTargetValue(0.0f);
    truePeakBlend.setCurrentAndTargetValue(1.0f);

    reset();
}

void LimiterEngine::reset()
{
    writePos = 0; readPos = 0; baseWritePos = 0;
    for (auto& cs : channelState)
    {
        std::fill(cs.delay.begin(), cs.delay.end(), 0.0f);
        std::fill(cs.dryBaseDelay.begin(), cs.dryBaseDelay.end(), 0.0f);
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
    writePos = 0; readPos = 0; baseWritePos = 0;
    for (auto& cs : channelState)
    {
        std::fill(cs.delay.begin(), cs.delay.end(), 0.0f);
        std::fill(cs.dryBaseDelay.begin(), cs.dryBaseDelay.end(), 0.0f);
        cs.minHead = 0; cs.minCount = 0; cs.currentGain = 1.0f; cs.heldDigitalPeak = 0.0f;
    }
    const int idx = factorToIndex(wanted);
    if (oversamplers[(size_t) idx]) oversamplers[(size_t) idx]->reset();

    const double osRate = baseSampleRate * (double) wanted;
    inputGainSmoothed.reset(osRate, 0.02);
    ceilingSmoothed.reset(osRate, 0.02);
    stereoLinkSmoothed.reset(osRate, 0.02);
    truePeakBlend.reset(osRate, 0.02);
    // bypassMix runs at the base rate and is unaffected by the oversampling factor.
}

void LimiterEngine::setParameters(float inputGainDb, float ceilingDbTP, float releaseMsIn, bool autoReleaseIn,
                                   Character characterIn, float stereoLinkPercent, bool truePeakIn, bool bypassedIn)
{
    inputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb));
    ceilingSmoothed.setTargetValue(juce::Decibels::decibelsToGain(ceilingDbTP));
    // stereoLinkPercent arrives as 0-100 (the "stereo_link" APVTS parameter's own
    // range, shown to the user as "N %"), but stereoLinkSmoothed is a normalised 0-1
    // ramp. This was previously clamped instead of divided, which silently collapsed
    // every non-zero value (1-100) to 1.0 — the knob only ever really moved between
    // "0%" and "100%", with everything in between behaving identically to 100%.
    stereoLinkSmoothed.setTargetValue(juce::jlimit(0.0f, 1.0f, stereoLinkPercent / 100.0f));
    truePeakBlend.setTargetValue(truePeakIn ? 1.0f : 0.0f);
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
    const int numBaseSamples = buffer.getNumSamples();

    // Capture the untouched dry signal at the BASE rate, before oversampling even
    // begins — a pure sample delay that never runs through the oversampling filters,
    // so bypass is bit-exact (not just gain/limiter-free). It will be read back
    // latencySamples() behind the input, so it lands in perfect sync with the wet path.
    for (int ch = 0; ch < chans; ++ch)
    {
        auto& cs = channelState[(size_t) ch];
        auto* d = buffer.getReadPointer(ch);
        for (int n = 0; n < numBaseSamples; ++n)
        {
            float raw = d[n];
            if (! std::isfinite(raw)) raw = 0.0f;
            cs.dryBaseDelay[(size_t) ((baseWritePos + n) % dryDelayCapacity)] = raw;
        }
    }

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::AudioBlock<float> osBlock = block;
    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        osBlock = oversamplers[(size_t) idx]->processSamplesUp(block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    const int lookaheadOs = lookaheadOsSamples[(size_t) idx];
    const double osSampleRate = baseSampleRate * (double) factor;
    float minGainThisBlock = 1.0f;
    float maxAbsWetOS = 0.0f; // oversampled (genuine true-peak) reading of the wet signal

    for (int i = 0; i < osNumSamples; ++i)
    {
        const float gainNow = inputGainSmoothed.getNextValue();
        const float ceilNow = ceilingSmoothed.getNextValue();
        const float linkNow = stereoLinkSmoothed.getNextValue();
        const float tpBlendNow = truePeakBlend.getNextValue();

        float peak[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            float xRaw = osBlock.getChannelPointer((size_t) ch)[i];
            if (! std::isfinite(xRaw)) xRaw = 0.0f;

            // Gain is applied exactly once, right here, only for the wet (limited) path.
            const float xGained = xRaw * gainNow;
            cs.delay[(size_t) (writePos % delayCapacity)] = xGained;

            const float instantAbs = std::abs(xGained);
            if ((writePos % factor) == 0) cs.heldDigitalPeak = instantAbs;
            // Blended rather than hard-switched: toggling True Peak mid-playback ramps
            // between the two detector sources instead of handing the lookahead window
            // a discontinuous target on a single sample (see truePeakBlend above).
            peak[ch] = cs.heldDigitalPeak + tpBlendNow * (instantAbs - cs.heldDigitalPeak);
        }

        // Stereo Link interpolates in the dB gain-reduction domain, not the linear peak
        // domain: each channel first gets its own fully-independent target, then (for
        // true stereo material) the two are blended toward whichever needs the larger
        // reduction. Blending dB values — not peaks — before converting back to a linear
        // gain is what keeps the link curve perceptually linear across 0-100%, and it's
        // mono-safe by construction: with one channel there is no second target to blend
        // toward, so linkNow has no effect at all, matching the pre-link behaviour exactly.
        float independentGrDb[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            const float independentTarget = (peak[ch] > ceilNow && peak[ch] > 1.0e-9f) ? ceilNow / peak[ch] : 1.0f;
            independentGrDb[ch] = juce::Decibels::gainToDecibels(juce::jlimit(0.0f, 1.0f, independentTarget), -100.0f);
        }
        const float linkedGrDb = chans > 1 ? juce::jmin(independentGrDb[0], independentGrDb[1]) : independentGrDb[0];

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            const float finalGrDb = juce::jmap(linkNow, independentGrDb[ch], linkedGrDb);
            float target = bypassed ? 1.0f : juce::Decibels::decibelsToGain(finalGrDb, -100.0f);
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
                // Last-resort safety net against decimation-filter ringing overshoot —
                // only meaningful when the gain computer is actually targeting this same
                // oversampled ceiling, i.e. True Peak is on. With it off, gain reduction
                // is deliberately driven by the decimated sample peak alone, so the true
                // (oversampled) peak is *expected* to run past the ceiling; clamping it
                // here would silently distort the signal and hide that legitimate
                // overshoot from both the ear and the meter. Blended by the same ramp as
                // the detector above (not a hard on/off switch) so toggling the button
                // fades the clamp in/out instead of stepping the sample value.
                const float clampedWet = juce::jlimit(-ceilNow, ceilNow, wet);
                wet += tpBlendNow * (clampedWet - wet);
                osBlock.getChannelPointer((size_t) ch)[i] = wet;

                minGainThisBlock = juce::jmin(minGainThisBlock, cs.currentGain);
                maxAbsWetOS = juce::jmax(maxAbsWetOS, std::abs(wet));
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

    // Dry/wet crossfade happens here, at the base rate, after the wet path has been
    // fully computed and downsampled — the dry sample never touches the oversampling
    // filters (see the capture loop at the top), so at mix=1 (fully bypassed) this is a
    // bit-exact pass-through, not an approximation. `buffer` holds the wet result now;
    // blend in the latency-aligned dry sample by however far bypassMix has ramped.
    const int totalLatency = latencySamplesFor(factor);
    float maxAbsOutThisBlock = 0.0f;
    float finalMix = 0.0f;
    for (int n = 0; n < numBaseSamples; ++n)
    {
        bypassMix.setTargetValue(bypassed ? 1.0f : 0.0f);
        const float mix = bypassMix.getNextValue();
        finalMix = mix;
        const juce::int64 srcAbs = baseWritePos + n - totalLatency;

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            auto* d = buffer.getWritePointer(ch);
            const float dry = srcAbs >= 0 ? cs.dryBaseDelay[(size_t) (srcAbs % dryDelayCapacity)] : 0.0f;
            const float y = d[n] + mix * (dry - d[n]);
            d[n] = y;
            maxAbsOutThisBlock = juce::jmax(maxAbsOutThisBlock, std::abs(y));
        }
    }
    baseWritePos += numBaseSamples;

    // While mostly/fully bypassed, gain reduction reads back toward 0dB in step with
    // the same crossfade, so the meter never lingers on a stale reduction value.
    const float reportedGain = minGainThisBlock + finalMix * (1.0f - minGainThisBlock);
    currentGrDb.store(juce::Decibels::gainToDecibels(reportedGain, -60.0f));

    // The True Peak meter (and the TRUE PEAK OVER indicator it feeds) always reports the
    // genuine oversampled/intersample peak, regardless of the True Peak toggle — that
    // toggle only decides whether this peak is allowed to *drive gain reduction*
    // ("True Peak Limiting"), never whether it's *measured* ("True Peak Meter"). With it
    // off, the reconstructed peak can legitimately exceed the ceiling, and OVER must
    // still be able to light up to say so. Bypassed always reports the real output.
    const float reportedPeakLinear = bypassed ? maxAbsOutThisBlock : juce::jmax(maxAbsWetOS, maxAbsOutThisBlock);
    currentTruePeakDb.store(juce::Decibels::gainToDecibels(reportedPeakLinear, -100.0f));
}

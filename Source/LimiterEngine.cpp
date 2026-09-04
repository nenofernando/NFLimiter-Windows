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

    dedicatedTpOversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        (size_t) numChannels, (size_t) kDedicatedTpStages,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    dedicatedTpOversampler->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
    dedicatedTpScratch.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);

    // FIR equiripple (linear-phase), not polyphase IIR: see the class-level comment on
    // dedicatedTpGainOversampler for why this filter family is required here.
    dedicatedTpGainOversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        (size_t) numChannels, (size_t) kDedicatedTpGainStages,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    dedicatedTpGainOversampler->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
    dedicatedTpGainScratch.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);

    // getLatencyInSamples() reports the round-trip up+down latency, but this analyzer
    // only ever calls processSamplesUp (feed-forward, never downsamples), so its real
    // delay is shorter and must be measured directly: an independent, throwaway
    // Oversampling instance (so it can't disturb the real member's filter state or care
    // about maximumBlockSize's own processing-size contract) is fed a single impulse,
    // and the delay is however far the reconstructed peak lands from where a
    // zero-delay filter would have put it. Measured once, here, never in process(). This
    // filter is linear-phase, so this single measurement (taken with a broadband
    // impulse) is exact at every frequency, not just the one the impulse happens to
    // emphasise -- unlike polyphase IIR, whose group delay varies with frequency.
    {
        juce::dsp::Oversampling<float> calibOs(1, (size_t) kDedicatedTpGainStages,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        const int calibN = 1024;
        calibOs.initProcessing((size_t) calibN);
        juce::AudioBuffer<float> calib(1, calibN);
        calib.clear();
        const int impulsePos = calibN / 4;
        calib.setSample(0, impulsePos, 1.0f);
        juce::dsp::AudioBlock<float> calibBlock(calib);
        auto calibUp = calibOs.processSamplesUp(calibBlock);
        int peakIdx = 0;
        float peakVal = 0.0f;
        auto* up0 = calibUp.getChannelPointer(0);
        for (size_t i = 0; i < calibUp.getNumSamples(); ++i)
        {
            const float v = std::abs(up0[i]);
            if (v > peakVal) { peakVal = v; peakIdx = (int) i; }
        }
        const double delayOsSamples = (double) peakIdx - (double) impulsePos * kGainAnalysisFactor;
        dedicatedTpGainLatencyOsSamples = juce::jmax(0, (int) std::round(delayOsSamples));
        dedicatedTpGainLatencyBaseSamples = juce::jmax(0, (int) std::round(delayOsSamples / (double) kGainAnalysisFactor));
    }
    inputGainRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);

    // Unlike the wet path's lookahead ring (where read trails write by a bounded
    // lookaheadOs at all times, interleaved sample-by-sample), the dry path writes an
    // entire block and then reads it back in a second pass — so the ring must hold at
    // least one full block on top of the latency, or a large block wraps around and
    // clobbers samples before they're ever read.
    int maxTotalLatency = 0;
    for (int i = 0; i < kNumOsSlots; ++i)
        maxTotalLatency = juce::jmax(maxTotalLatency, lookaheadBaseSamples + osLatencyBaseSamples[(size_t) i]);
    dryDelayCapacity = maxTotalLatency + juce::jmax(1, maxBlockSize) + 64;

    // The dedicated gain analyzer's own delay must fit inside the existing 5ms
    // lookahead, or a base sample's true-peak-aware gain decision would need audio that
    // has already been emitted -- verified here (and by the latency-compliance tests),
    // not assumed. If this ever trips, the fix is a longer lookahead, not silently
    // truncating the compensation.
    jassert(dedicatedTpGainLatencyBaseSamples <= lookaheadBaseSamples);

    // Rounded up to a power of two so every ring index below is a single AND against
    // tp8xPeakRingCapacityMask instead of an integer division: this ring is indexed
    // twice per dedicated-oversampled tick (write once per block, read once per main
    // loop tick), so at 192kHz/8x that is tens of thousands of indexing operations per
    // second per channel -- measured to be a significant, avoidable share of this
    // analyzer's CPU cost with a non-power-of-two modulus.
    const int gainFactorForRing = 1 << kDedicatedTpGainStages;
    const int minRingCapacityNeeded = dryDelayCapacity * gainFactorForRing;
    tp8xPeakRingCapacity = 1;
    while (tp8xPeakRingCapacity < minRingCapacityNeeded) tp8xPeakRingCapacity <<= 1;
    tp8xPeakRingMask = tp8xPeakRingCapacity - 1;

    for (auto& cs : channelState)
    {
        cs.delay.assign((size_t) delayCapacity, 0.0f);
        cs.dryBaseDelay.assign((size_t) dryDelayCapacity, 0.0f);
        cs.tp8xPeakRing.assign((size_t) tp8xPeakRingCapacity, 0.0f);
        cs.minIdxRing.assign((size_t) minRingCapacity, (juce::int64) 0);
        cs.minValRing.assign((size_t) minRingCapacity, 0.0f);
    }

    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    activeOsFactor.store(wanted);
    pendingOsFactor.store(wanted);

    inputGainSmoothed.reset(baseSampleRate, 0.02);
    inputGainSmoothedBaseRate.reset(baseSampleRate, 0.02);
    ceilingSmoothed.reset(baseSampleRate, 0.02);
    stereoLinkSmoothed.reset(baseSampleRate, 0.02);
    truePeakBlend.reset(baseSampleRate, 0.02);
    // 6ms was too fast: with real program material, active gain reduction removed
    // almost instantly on toggle produced an audible thump/click on bass-heavy content.
    // 30ms still reads as an instant A/B switch but is long enough (well over one cycle
    // even at ~30Hz) to smooth over the wet/dry level jump.
    bypassMix.reset(baseSampleRate, 0.03); // base rate — the dry crossfade runs post-downsample
    inputGainSmoothed.setCurrentAndTargetValue(1.0f);
    inputGainSmoothedBaseRate.setCurrentAndTargetValue(1.0f);
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
        std::fill(cs.tp8xPeakRing.begin(), cs.tp8xPeakRing.end(), 0.0f);
        cs.minHead = 0; cs.minCount = 0; cs.currentGain = 1.0f; cs.heldDigitalPeak = 0.0f;
    }
    for (auto& os : oversamplers) if (os) os->reset();
    if (dedicatedTpOversampler) dedicatedTpOversampler->reset();
    if (dedicatedTpGainOversampler) dedicatedTpGainOversampler->reset();
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
        std::fill(cs.tp8xPeakRing.begin(), cs.tp8xPeakRing.end(), 0.0f);
        cs.minHead = 0; cs.minCount = 0; cs.currentGain = 1.0f; cs.heldDigitalPeak = 0.0f;
    }
    const int idx = factorToIndex(wanted);
    if (oversamplers[(size_t) idx]) oversamplers[(size_t) idx]->reset();
    // The dedicated True Peak gain analyzer is fixed at 8x and deliberately untouched by
    // the OVERSAMPLING selector's own factor -- its precision must not change just
    // because the user picked a different quality factor. Its own filter state is
    // therefore deliberately NOT reset here: it is a continuous, streaming filter whose
    // internal state has nothing to do with baseWritePos's numbering, so resetting it on
    // every quality-factor switch would zero its delay line and manufacture a brief,
    // artificial discontinuity in an otherwise-unbroken reconstruction of the real,
    // continuous audio -- a protection gap the switch itself does not actually require.
    // cs.tp8xPeakRing IS cleared above (it is indexed by baseWritePos, which does
    // restart at 0), and the instantAbs fallback in process() already covers the brief
    // window where the delay-compensated ring read isn't valid yet after any reset of
    // that indexing -- so clearing the ring is sufficient; the filter itself does not
    // need to be, and keeping it running is strictly safer.

    const double osRate = baseSampleRate * (double) wanted;
    inputGainSmoothed.reset(osRate, 0.02);
    ceilingSmoothed.reset(osRate, 0.02);
    stereoLinkSmoothed.reset(osRate, 0.02);
    truePeakBlend.reset(osRate, 0.02);
    // bypassMix and inputGainSmoothedBaseRate run at the fixed base rate and are
    // unaffected by the oversampling factor.
}

void LimiterEngine::setParameters(float inputGainDb, float ceilingDbTP, float releaseMsIn, bool autoReleaseIn,
                                   Character characterIn, float stereoLinkPercent, bool truePeakIn, bool bypassedIn)
{
    inputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb));
    inputGainSmoothedBaseRate.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb));
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

    // Dedicated True Peak GAIN-DRIVING analysis: fixed 8x, feed-forward, decoupled from
    // the OVERSAMPLING selector's own factor -- so True Peak Limiting protects equally
    // at 1x/2x/4x/8x instead of only where the main quality oversampler happens to
    // provide enough intersample resolution on its own. Runs on the GAINED signal (post
    // input gain, the only stage between raw input and this analysis point) at a fixed
    // rate that never changes with `factor`. Skipped entirely while True Peak is fully
    // off (blend at rest on 0, current call and target both) to save CPU; only reads
    // pre-sized buffers either way, so it never allocates.
    const bool needsTpGainAnalysis = truePeakBlend.getTargetValue() > 1.0e-6f
                                    || truePeakBlend.getCurrentValue() > 1.0e-6f;
    if (needsTpGainAnalysis)
    {
        for (int n = 0; n < numBaseSamples; ++n)
            inputGainRampScratch[(size_t) n] = inputGainSmoothedBaseRate.getNextValue();

        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = buffer.getReadPointer(ch);
            auto* g = dedicatedTpGainScratch.getWritePointer(ch);
            for (int n = 0; n < numBaseSamples; ++n)
            {
                float raw = d[n];
                if (! std::isfinite(raw)) raw = 0.0f;
                g[n] = raw * inputGainRampScratch[(size_t) n];
            }
        }

        juce::dsp::AudioBlock<float> gainBlock(dedicatedTpGainScratch.getArrayOfWritePointers(),
                                                (size_t) chans, (size_t) numBaseSamples);
        auto gainOs = dedicatedTpGainOversampler->processSamplesUp(gainBlock);

        // Store every dedicated-oversampled tick's |value| individually (not a
        // per-base-sample max) so the main loop below can give each of its own ticks a
        // correspondingly fine slice when factor < 8, instead of collapsing all `factor`
        // sub-ticks of a base sample to one held value -- at factor==8 that would have
        // quietly thrown away the native per-tick resolution the old (coupled) code had.
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            auto* up = gainOs.getChannelPointer((size_t) ch);
            const juce::int64 baseOsIdx = baseWritePos * kGainAnalysisFactor;
            for (size_t k = 0; k < gainOs.getNumSamples(); ++k)
            {
                float v = std::abs(up[k]);
                if (! std::isfinite(v)) v = 0.0f;
                cs.tp8xPeakRing[(size_t) ((baseOsIdx + (juce::int64) k) & tp8xPeakRingMask)] = v;
            }
        }
    }
    else
    {
        // Keep the base-rate gain ramp in sync with the real (not-yet-consumed) target
        // so re-enabling True Peak later doesn't jump: advance the same number of steps
        // the loop above would have, without doing the per-sample work.
        inputGainSmoothedBaseRate.skip(numBaseSamples);
    }

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::AudioBlock<float> osBlock = block;
    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        osBlock = oversamplers[(size_t) idx]->processSamplesUp(block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    const int lookaheadOs = lookaheadOsSamples[(size_t) idx];
    const double osSampleRate = baseSampleRate * (double) factor;
    float minGainThisBlock = 1.0f;

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

            // The True-Peak-on candidate comes from the dedicated, fixed-8x,
            // feed-forward analyzer (decoupled from `factor`) rather than this main
            // factor's own `instantAbs`, so True Peak protection no longer depends on
            // which OVERSAMPLING quality factor is selected. True Peak off is completely
            // unaffected: heldDigitalPeak's own source and update timing are unchanged
            // above. `factor` always evenly divides kGainAnalysisFactor (1/2/4/8 divide
            // 8), so each of this base sample's `factor` main-loop ticks maps onto its
            // own equal-sized slice of the analyzer's 8 sub-samples for that base sample
            // -- at factor==8 that is one dedicated sample per tick (matching the native
            // per-tick resolution the old, coupled code had); at lower factors, this
            // tick's slice is reduced by max (never held constant across the whole base
            // sample), which is strictly finer than the old per-base-sample coupling.
            float tpOnCandidate = instantAbs;
            if (needsTpGainAnalysis)
            {
                const int nBase = i / factor;
                const int iWithinBase = i - nBase * factor;
                const int subPerTick = kGainAnalysisFactor / factor;
                const juce::int64 baseOsIdx = (baseWritePos + nBase) * kGainAnalysisFactor
                                             + iWithinBase * subPerTick - dedicatedTpGainLatencyOsSamples;
                // During the analyzer's own warm-up (the first ~39 base samples after
                // prepare()/reset()/a factor switch, before the delay-compensated ring
                // position becomes valid), baseOsIdx is negative: fall back to
                // instantAbs -- this factor's own immediate estimate, exactly what fed
                // the gain decision before this analyzer existed -- rather than 0.0f,
                // which would silently zero the True-Peak-on candidate for that whole
                // window (with truePeakBlend starting at 1.0, that suppressed real,
                // legitimate cold-start gain reduction; verified bit-identical to a
                // pre-decoupling reference with this fallback, diverging without it).
                float m = instantAbs;
                if (baseOsIdx >= 0)
                {
                    m = 0.0f;
                    for (int k = 0; k < subPerTick; ++k)
                        m = juce::jmax(m, cs.tp8xPeakRing[(size_t) ((baseOsIdx + k) & tp8xPeakRingMask)]);
                }
                tpOnCandidate = m;
            }
            // Blended rather than hard-switched: toggling True Peak mid-playback ramps
            // between the two detector sources instead of handing the lookahead window
            // a discontinuous target on a single sample (see truePeakBlend above).
            peak[ch] = cs.heldDigitalPeak + tpBlendNow * (tpOnCandidate - cs.heldDigitalPeak);
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
            d[n] = d[n] + mix * (dry - d[n]);
        }
    }
    baseWritePos += numBaseSamples;

    // While mostly/fully bypassed, gain reduction reads back toward 0dB in step with
    // the same crossfade, so the meter never lingers on a stale reduction value.
    const float reportedGain = minGainThisBlock + finalMix * (1.0f - minGainThisBlock);
    currentGrDb.store(juce::Decibels::gainToDecibels(reportedGain, -60.0f));

    // Dedicated True Peak detector: independently re-analyses this exact final output
    // — after Gain, limiting, Character, the Ceiling clamp and the dry/wet crossfade,
    // i.e. exactly what is about to reach the host — with its own fixed-factor (8x)
    // oversampler that never shares state with the OVERSAMPLING selector's own
    // oversamplers above. That decoupling is deliberate: metering precision must not
    // change just because the user picked a different processing factor. The True
    // Peak toggle still only ever decides whether this peak is allowed to *drive gain
    // reduction* ("True Peak Limiting") — this measurement ("True Peak Meter") always
    // reports the real reconstructed peak of whatever was actually produced, on or
    // off, bypassed or not. Copies into a pre-allocated scratch buffer first so this
    // can never touch what's actually sent to the host.
    for (int ch = 0; ch < chans; ++ch)
        dedicatedTpScratch.copyFrom(ch, 0, buffer, ch, 0, numBaseSamples);
    juce::dsp::AudioBlock<float> tpBlock(dedicatedTpScratch.getArrayOfWritePointers(), (size_t) chans, (size_t) numBaseSamples);
    auto tpUp = dedicatedTpOversampler->processSamplesUp(tpBlock);
    float maxAbsTp = 0.0f;
    for (size_t ch = 0; ch < (size_t) chans; ++ch)
    {
        auto* d = tpUp.getChannelPointer(ch);
        for (size_t i = 0; i < tpUp.getNumSamples(); ++i)
            if (std::isfinite(d[i])) maxAbsTp = juce::jmax(maxAbsTp, std::abs(d[i]));
    }
    currentTruePeakDb.store(juce::Decibels::gainToDecibels(maxAbsTp, -100.0f));
}

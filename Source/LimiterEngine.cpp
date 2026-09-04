#include "LimiterEngine.h"

void LimiterEngine::prepare(double sampleRate, int maximumBlockSize, int newNumChannels)
{
    baseSampleRate = sampleRate;
    maxBlockSize = maximumBlockSize;
    numChannels = juce::jlimit(1, 2, newNumChannels);
    lookaheadBaseSamples = juce::jmax(1, (int) std::round(baseSampleRate * ((double) kLookaheadMs / 1000.0)));
    // Matches the 0.02s ramp time used below for inputGainSmoothed/ceilingSmoothed/
    // stereoLinkSmoothed/truePeakBlend.
    rampSettleBaseSamples = juce::jmax(1, (int) std::round(baseSampleRate * 0.02));

    // Automatic oversampling: the DSP factor and the True Peak analyzer/meter factor
    // are chosen ONCE here, purely from the sample rate -- see tierForSampleRate() and
    // the class-level comment. No user control, no runtime switching, no crossfade.
    tierForSampleRate(sampleRate, dspFactor, tpFactor);

    lookaheadOsSamples = lookaheadBaseSamples * dspFactor;
    if (dspFactor == 1)
    {
        oversampler.reset();
        deltaRefOversampler.reset();
        osLatencyBaseSamples = 0;
    }
    else
    {
        const int stages = (int) std::round(std::log2((double) dspFactor));
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            (size_t) numChannels, (size_t) stages,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        oversampler->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
        osLatencyBaseSamples = (int) std::round(oversampler->getLatencyInSamples());

        // Identical construction to `oversampler` above -- same filter design, same
        // factor, same latency -- but its own separate instance/filter state, purely
        // so DELTA/LISTEN's reference can be round-tripped through the same linear
        // filtering without sharing (and corrupting) the main wet path's own
        // continuous filter state.
        deltaRefOversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            (size_t) numChannels, (size_t) stages,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        deltaRefOversampler->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
    }
    totalLatencyBaseSamples = lookaheadBaseSamples + osLatencyBaseSamples;
    deltaRefScratch.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);

    delayCapacity = lookaheadOsSamples + 64;
    minRingCapacity = delayCapacity;

    const int tpStages = (int) std::round(std::log2((double) tpFactor));
    dedicatedTpOversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        (size_t) numChannels, (size_t) tpStages,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    dedicatedTpOversampler->initProcessing((size_t) juce::jmax(1, maximumBlockSize));
    dedicatedTpScratch.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);

    // FIR equiripple (linear-phase), not polyphase IIR: see the class-level comment on
    // dedicatedTpGainOversampler for why this filter family is required here.
    dedicatedTpGainOversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        (size_t) numChannels, (size_t) tpStages,
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
        juce::dsp::Oversampling<float> calibOs(1, (size_t) tpStages,
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
        const double delayOsSamples = (double) peakIdx - (double) impulsePos * tpFactor;
        dedicatedTpGainLatencyOsSamples = juce::jmax(0, (int) std::round(delayOsSamples));
        dedicatedTpGainLatencyBaseSamples = juce::jmax(0, (int) std::round(delayOsSamples / (double) tpFactor));
    }
    inputGainRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);
    ceilRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 0.89125f);
    linkRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);
    tpBlendRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);

    // The dedicated gain analyzer's own delay must fit inside the existing 5ms
    // lookahead, or a base sample's true-peak-aware gain decision would need audio that
    // has already been emitted -- verified here (and by the latency-compliance tests),
    // not assumed. If this ever trips, the fix is a longer lookahead, not silently
    // truncating the compensation.
    jassert(dedicatedTpGainLatencyBaseSamples <= lookaheadBaseSamples);

    // dryDelayCapacity sized off this engine's one (fixed) latency: the dry path writes
    // an entire block and reads it back in a second pass, so the ring must hold at
    // least one full block on top of the latency, or a large block wraps around and
    // clobbers samples before they're ever read.
    dryDelayCapacity = totalLatencyBaseSamples + juce::jmax(1, maxBlockSize) + 64;

    // Rounded up to a power of two so every ring index below is a single AND against
    // tpGainPeakRingMask instead of an integer division: this ring is indexed twice per
    // dedicated-oversampled tick (write once per block, read once per main loop tick),
    // so at high sample rates that is tens of thousands of indexing operations per
    // second per channel -- measured to be a significant, avoidable share of this
    // analyzer's CPU cost with a non-power-of-two modulus.
    const int minRingCapacityNeeded = dryDelayCapacity * tpFactor;
    tpGainPeakRingCapacity = 1;
    while (tpGainPeakRingCapacity < minRingCapacityNeeded) tpGainPeakRingCapacity <<= 1;
    tpGainPeakRingMask = tpGainPeakRingCapacity - 1;

    for (auto& cs : channelState)
    {
        cs.dryBaseDelay.assign((size_t) dryDelayCapacity, 0.0f);
        cs.gainedRefDelay.assign((size_t) dryDelayCapacity, 0.0f);
        cs.tpGainPeakRing.assign((size_t) tpGainPeakRingCapacity, 0.0f);
        cs.path.delay.assign((size_t) delayCapacity, 0.0f);
        cs.path.minIdxRing.assign((size_t) minRingCapacity, (juce::int64) 0);
        cs.path.minValRing.assign((size_t) minRingCapacity, 0.0f);
        cs.path.minHead = 0; cs.path.minCount = 0; cs.path.currentGain = 1.0f; cs.path.heldDigitalPeak = 0.0f;
    }
    pathScratch.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);
    normalOutputSnapshot.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);
    lastProcessedNumSamples = 0;

    inputGainSmoothed.reset(baseSampleRate, 0.02);
    ceilingSmoothed.reset(baseSampleRate, 0.02);
    stereoLinkSmoothed.reset(baseSampleRate, 0.02);
    truePeakBlend.reset(baseSampleRate, 0.02);
    // 6ms was too fast: with real program material, active gain reduction removed
    // almost instantly on toggle produced an audible thump/click on bass-heavy content.
    // 30ms still reads as an instant A/B switch but is long enough (well over one cycle
    // even at ~30Hz) to smooth over the wet/dry level jump.
    bypassMix.reset(baseSampleRate, 0.03); // base rate — the dry crossfade runs post-downsample
    // 15ms: fast enough to read as an instant toggle, slow enough to never click.
    deltaMix.reset(baseSampleRate, 0.015);
    inputGainSmoothed.setCurrentAndTargetValue(1.0f);
    ceilingSmoothed.setCurrentAndTargetValue(0.89125f);
    stereoLinkSmoothed.setCurrentAndTargetValue(1.0f);
    bypassMix.setCurrentAndTargetValue(0.0f);
    truePeakBlend.setCurrentAndTargetValue(1.0f);
    // DELTA/LISTEN always starts OFF on prepare() -- a fresh instance, a sample-rate
    // change, or a new session must never inherit a previous "listening to Delta"
    // state. This is a monitoring toggle, not a parameter, so there is no saved value
    // to restore here in the first place.
    deltaMix.setCurrentAndTargetValue(0.0f);
    deltaListenEnabledLast = false;
    // The next setParameters() call snaps directly to its values instead of ramping
    // from these placeholders -- see the comment in setParameters() itself.
    parametersInitialized = false;

    reset();
}

void LimiterEngine::setDeltaListenEnabled(bool enabled) noexcept
{
    // Edge-triggered: PluginProcessor calls this every block with whatever the current
    // atomic says, same as setParameters(). Retargeting the smoother only on an actual
    // change (rather than every block) is essential -- calling setTargetValue() with
    // the same value repeatedly would keep resetting the ramp's own step calculation
    // before it ever finished, so Listen would never actually reach full strength.
    if (enabled != deltaListenEnabledLast)
    {
        deltaMix.setTargetValue(enabled ? 1.0f : 0.0f);
        deltaListenEnabledLast = enabled;
    }
}

void LimiterEngine::reset()
{
    baseWritePos = 0;
    writePos = 0;
    readPos = 0;

    for (auto& cs : channelState)
    {
        std::fill(cs.dryBaseDelay.begin(), cs.dryBaseDelay.end(), 0.0f);
        std::fill(cs.gainedRefDelay.begin(), cs.gainedRefDelay.end(), 0.0f);
        std::fill(cs.tpGainPeakRing.begin(), cs.tpGainPeakRing.end(), 0.0f);
        std::fill(cs.path.delay.begin(), cs.path.delay.end(), 0.0f);
        cs.path.minHead = 0; cs.path.minCount = 0; cs.path.currentGain = 1.0f; cs.path.heldDigitalPeak = 0.0f;
    }
    if (oversampler) oversampler->reset();
    if (deltaRefOversampler) deltaRefOversampler->reset();
    if (dedicatedTpOversampler) dedicatedTpOversampler->reset();
    if (dedicatedTpGainOversampler) dedicatedTpGainOversampler->reset();
    currentGrDb.store(0.0f);
}

void LimiterEngine::setParameters(float inputGainDb, float ceilingDbTP, float releaseMsIn, bool autoReleaseIn,
                                   Character characterIn, float stereoLinkPercent, bool truePeakIn, bool bypassedIn)
{
    const float gainTarget = juce::Decibels::decibelsToGain(inputGainDb);
    const float ceilingTarget = juce::Decibels::decibelsToGain(ceilingDbTP);
    // stereoLinkPercent arrives as 0-100 (the "stereo_link" APVTS parameter's own
    // range, shown to the user as "N %"), but stereoLinkSmoothed is a normalised 0-1
    // ramp. This was previously clamped instead of divided, which silently collapsed
    // every non-zero value (1-100) to 1.0 — the knob only ever really moved between
    // "0%" and "100%", with everything in between behaving identically to 100%.
    const float linkTarget = juce::jlimit(0.0f, 1.0f, stereoLinkPercent / 100.0f);
    const float tpTarget = truePeakIn ? 1.0f : 0.0f;

    if (! parametersInitialized)
    {
        // The FIRST setParameters() call after prepare()/construction represents this
        // instance's actual initial state -- either a restored session/preset's real
        // values, or a fresh instance's real defaults -- not a live, mid-playback
        // parameter change. prepare() has to seed the smoothers with SOME value before
        // any real parameters are known (in case audio is ever pulled before the host
        // calls setParameters at all), but ramping smoothly FROM that placeholder TO
        // the real restored value on every single session reopen was a genuine,
        // audible bug: any project saved with a non-default ceiling, gain, stereo
        // link, or True Peak state would spend the first 20ms of playback ramping in
        // from prepare()'s hardcoded defaults instead of already being at the
        // restored value. Snapping current AND target together here means recall is
        // instantaneous, exactly like a value that was already stable before playback
        // started -- there is nothing to smooth, because nothing was actually changing
        // from the listener's perspective.
        inputGainSmoothed.setCurrentAndTargetValue(gainTarget);
        ceilingSmoothed.setCurrentAndTargetValue(ceilingTarget);
        stereoLinkSmoothed.setCurrentAndTargetValue(linkTarget);
        truePeakBlend.setCurrentAndTargetValue(tpTarget);
        parametersInitialized = true;
    }
    else
    {
        inputGainSmoothed.setTargetValue(gainTarget);
        ceilingSmoothed.setTargetValue(ceilingTarget);
        stereoLinkSmoothed.setTargetValue(linkTarget);
        truePeakBlend.setTargetValue(tpTarget);
    }
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

void LimiterEngine::pushMin(PathState& ps, juce::int64 idx, float value, int ringCapacity) noexcept
{
    while (ps.minCount > 0)
    {
        const int backPos = (ps.minHead + ps.minCount - 1) % ringCapacity;
        if (ps.minValRing[(size_t) backPos] >= value) --ps.minCount;
        else break;
    }
    const int pos = (ps.minHead + ps.minCount) % ringCapacity;
    ps.minValRing[(size_t) pos] = value;
    ps.minIdxRing[(size_t) pos] = idx;
    ++ps.minCount;
}

void LimiterEngine::popExpiredMin(PathState& ps, juce::int64 minAllowedIdx, int ringCapacity) noexcept
{
    while (ps.minCount > 0 && ps.minIdxRing[(size_t) ps.minHead] < minAllowedIdx)
    {
        ps.minHead = (ps.minHead + 1) % ringCapacity;
        --ps.minCount;
    }
}

void LimiterEngine::process(juce::AudioBuffer<float>& buffer)
{
    const int chans = juce::jmin(numChannels, buffer.getNumChannels());
    const int numBaseSamples = buffer.getNumSamples();

    // Capture the untouched dry signal at the BASE rate, before oversampling even
    // begins — a pure sample delay that never runs through the oversampling filter, so
    // bypass is bit-exact (not just gain/limiter-free). It will be read back
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

    // Shared, per-base-sample parameter ramps, read by the per-tick loop below (which
    // repeats the same base-sample value across its own `dspFactor` sub-ticks).
    for (int n = 0; n < numBaseSamples; ++n)
    {
        inputGainRampScratch[(size_t) n] = inputGainSmoothed.getNextValue();
        ceilRampScratch[(size_t) n] = ceilingSmoothed.getNextValue();
        linkRampScratch[(size_t) n] = stereoLinkSmoothed.getNextValue();
        tpBlendRampScratch[(size_t) n] = truePeakBlend.getNextValue();
    }

    // Dedicated True Peak GAIN-DRIVING analysis: feed-forward, decoupled from the DSP
    // oversampler's own factor. Runs UNCONDITIONALLY, whether True Peak is on, off, or
    // mid-transition: this ring is read `dedicatedTpGainLatencyOsSamples` sub-samples
    // in the past below, so if it only ran while True Peak was on, re-enabling it after
    // any OFF period would read back stale data left over from the last time it ran (or
    // silence, if it never ran) for a short window right after activation -- a real,
    // measured protection gap at the exact moment True Peak turns on. Running it
    // continuously means the ring is always caught up to "now minus the analyzer's own
    // latency", at the cost of always paying this analyzer's own CPU (measured
    // separately in the benchmark) even while True Peak is fully off.
    {
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = buffer.getReadPointer(ch);
            auto* g = dedicatedTpGainScratch.getWritePointer(ch);
            for (int n = 0; n < numBaseSamples; ++n)
            {
                float raw = d[n];
                if (! std::isfinite(raw)) raw = 0.0f;
                const float gained = raw * inputGainRampScratch[(size_t) n];
                g[n] = gained;
                deltaRefScratch.setSample(ch, n, gained);
            }
        }

        juce::dsp::AudioBlock<float> gainBlock(dedicatedTpGainScratch.getArrayOfWritePointers(),
                                                (size_t) chans, (size_t) numBaseSamples);
        auto gainOs = dedicatedTpGainOversampler->processSamplesUp(gainBlock);

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            auto* up = gainOs.getChannelPointer((size_t) ch);
            const juce::int64 baseOsIdx = baseWritePos * tpFactor;
            for (size_t k = 0; k < gainOs.getNumSamples(); ++k)
            {
                float v = std::abs(up[k]);
                if (! std::isfinite(v)) v = 0.0f;
                cs.tpGainPeakRing[(size_t) ((baseOsIdx + (juce::int64) k) & tpGainPeakRingMask)] = v;
            }
        }
    }

    // DELTA/LISTEN's reference: round-trip the post-input-Gain, pre-limiting signal
    // through deltaRefOversampler -- up, then immediately back down, nothing done in
    // between -- so it carries the SAME always-present linear filtering/latency the
    // main wet path itself applies even with no gain reduction at all. Runs
    // unconditionally (like the dedicated True Peak analyzers above), so the
    // reference is never stale the instant Listen is engaged. At dspFactor==1 there is
    // no oversampling filter in the main path either, so deltaRefScratch (already
    // holding the gained signal) needs no round trip -- it already matches exactly.
    {
        if (dspFactor > 1 && deltaRefOversampler != nullptr)
        {
            juce::dsp::AudioBlock<float> refBlock(deltaRefScratch.getArrayOfWritePointers(), (size_t) chans, (size_t) numBaseSamples);
            deltaRefOversampler->processSamplesUp(refBlock);
            deltaRefOversampler->processSamplesDown(refBlock);
        }
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            auto* d = deltaRefScratch.getReadPointer(ch);
            for (int n = 0; n < numBaseSamples; ++n)
                cs.gainedRefDelay[(size_t) ((baseWritePos + n) % dryDelayCapacity)] = d[n];
        }
    }

    // truePeakEnabled is included (not just the ramp values) because activation uses
    // the analyzer's candidate at full strength from the very first tick (see the
    // max-combination below) rather than fading it in with tpBlendRamp -- so the ring
    // must be consulted from that same first tick, even in the edge case where this
    // whole block's tpBlendRamp values happen to still read as ~0.
    const bool needsTpGainAnalysis = truePeakEnabled || tpBlendRampScratch[0] > 1.0e-6f
                                    || tpBlendRampScratch[(size_t) numBaseSamples - 1] > 1.0e-6f;

    for (int ch = 0; ch < chans; ++ch)
        pathScratch.copyFrom(ch, 0, buffer, ch, 0, numBaseSamples);

    juce::dsp::AudioBlock<float> block(pathScratch.getArrayOfWritePointers(), (size_t) chans, (size_t) numBaseSamples);
    juce::dsp::AudioBlock<float> osBlock = block;
    if (dspFactor > 1 && oversampler != nullptr)
        osBlock = oversampler->processSamplesUp(block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    const double osSampleRate = baseSampleRate * (double) dspFactor;
    float minGainThisBlock = 1.0f;

    for (int i = 0; i < osNumSamples; ++i)
    {
        const int nBase = i / dspFactor;
        const float gainNow = inputGainRampScratch[(size_t) nBase];
        const float ceilNow = ceilRampScratch[(size_t) nBase];
        const float linkNow = linkRampScratch[(size_t) nBase];
        const float tpBlendNow = tpBlendRampScratch[(size_t) nBase];

        float peak[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& ps = channelState[(size_t) ch].path;
            float xRaw = osBlock.getChannelPointer((size_t) ch)[i];
            if (! std::isfinite(xRaw)) xRaw = 0.0f;

            // Gain is applied exactly once, right here, only for the wet (limited) path.
            const float xGained = xRaw * gainNow;
            ps.delay[(size_t) (writePos % delayCapacity)] = xGained;

            const float instantAbs = std::abs(xGained);
            if ((writePos % dspFactor) == 0) ps.heldDigitalPeak = instantAbs;

            // The True-Peak-on candidate comes from the dedicated feed-forward analyzer
            // (decoupled from dspFactor) rather than this path's own `instantAbs`.
            // dspFactor always evenly divides tpFactor (see tierForSampleRate: 4|8,
            // 2|4, 1|2), so each of this base sample's dspFactor ticks maps onto its
            // own equal-sized slice of the analyzer's tpFactor sub-samples for that
            // base sample.
            float tpOnCandidate = instantAbs;
            if (needsTpGainAnalysis)
            {
                const int iWithinBase = i - nBase * dspFactor;
                const int subPerTick = tpFactor / dspFactor;
                const juce::int64 baseOsIdx = (baseWritePos + nBase) * tpFactor
                                             + iWithinBase * subPerTick - dedicatedTpGainLatencyOsSamples;
                // During the analyzer's own warm-up (the first few base samples after
                // prepare()/reset(), before the delay-compensated ring position becomes
                // valid), baseOsIdx is negative: fall back to instantAbs rather than
                // 0.0f, which would silently zero the True-Peak-on candidate for that
                // window (with truePeakBlend at 1.0, that would suppress real,
                // legitimate cold-start gain reduction).
                float m = instantAbs;
                if (baseOsIdx >= 0)
                {
                    auto& sharedRing = channelState[(size_t) ch].tpGainPeakRing;
                    m = 0.0f;
                    for (int k = 0; k < subPerTick; ++k)
                        m = juce::jmax(m, sharedRing[(size_t) ((baseOsIdx + k) & tpGainPeakRingMask)]);
                }
                tpOnCandidate = m;
            }
            // Asymmetric by design (activating vs releasing protection are not the same
            // risk): a plain linear blend of the two detectors during ACTIVATION can
            // authorize LESS gain reduction than either detector alone would demand
            // whenever tpOnCandidate > heldDigitalPeak (the normal case, since real
            // intersample peaks usually exceed the plain sample peak) -- for any
            // tpBlendNow in (0,1) that produces a candidate strictly between the two,
            // under-protecting relative to the true peak for the whole ~20ms ramp.
            // Fix: while True Peak is enabled (ramping up OR already fully on), use the
            // MAX of the two detectors -- a monotonically-safe combination that can
            // never be smaller than either input, so it never authorizes less
            // protection than either alone would. This deliberately ignores the blend
            // ramp for the decision itself (protection engages at full strength the
            // instant the toggle flips ON); audible smoothness still comes from the
            // existing lookahead window and release envelope, exactly as for any other
            // ordinary change in program peak -- no separate ramp is needed for that.
            // While True Peak is disabled (ramping down toward OFF), the plain linear
            // blend remains: giving up protection gradually is inherently safe.
            peak[ch] = truePeakEnabled
                ? juce::jmax(ps.heldDigitalPeak, tpOnCandidate)
                : (ps.heldDigitalPeak + tpBlendNow * (tpOnCandidate - ps.heldDigitalPeak));
        }

        // Stereo Link interpolates in the dB gain-reduction domain, not the linear peak
        // domain: each channel first gets its own fully-independent target, then (for
        // true stereo material) the two are blended toward whichever needs the larger
        // reduction. Blending dB values — not peaks — before converting back to a linear
        // gain is what keeps the link curve perceptually linear across 0-100%, and it's
        // mono-safe by construction.
        float independentGrDb[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            const float independentTarget = (peak[ch] > ceilNow && peak[ch] > 1.0e-9f) ? ceilNow / peak[ch] : 1.0f;
            independentGrDb[ch] = juce::Decibels::gainToDecibels(juce::jlimit(0.0f, 1.0f, independentTarget), -100.0f);
        }
        const float linkedGrDb = chans > 1 ? juce::jmin(independentGrDb[0], independentGrDb[1]) : independentGrDb[0];

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& ps = channelState[(size_t) ch].path;
            const float finalGrDb = juce::jmap(linkNow, independentGrDb[ch], linkedGrDb);
            float target = bypassed ? 1.0f : juce::Decibels::decibelsToGain(finalGrDb, -100.0f);
            target = juce::jlimit(0.0f, 1.0f, target);
            pushMin(ps, writePos, target, minRingCapacity);
        }

        ++writePos;

        if (writePos - readPos > lookaheadOsSamples)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto& ps = channelState[(size_t) ch].path;
                popExpiredMin(ps, readPos, minRingCapacity);
                const float windowMin = ps.minCount > 0 ? ps.minValRing[(size_t) ps.minHead] : 1.0f;

                if (bypassed)
                    ps.currentGain = 1.0f;
                else if (windowMin < ps.currentGain)
                    ps.currentGain = windowMin; // instant foresight-based attack (already ramped by the window)
                else
                    ps.currentGain = windowMin + computeReleaseCoeff(windowMin, osSampleRate) * (ps.currentGain - windowMin);

                float wet = ps.delay[(size_t) (readPos % delayCapacity)] * ps.currentGain;
                applyCharacter(wet);
                // Last-resort safety net against decimation-filter ringing overshoot —
                // only meaningful when the gain computer is actually targeting this same
                // oversampled ceiling, i.e. True Peak is on. With it off, gain reduction
                // is deliberately driven by the decimated sample peak alone, so the true
                // (oversampled) peak is *expected* to run past the ceiling; clamping it
                // here would silently distort the signal and hide that legitimate
                // overshoot from both the ear and the meter.
                const float clampedWet = juce::jlimit(-ceilNow, ceilNow, wet);
                // Same asymmetry as the peak-candidate decision above: while activating,
                // this residual clamp engages at full strength immediately.
                const float clampBlend = truePeakEnabled ? 1.0f : tpBlendNow;
                wet += clampBlend * (clampedWet - wet);
                osBlock.getChannelPointer((size_t) ch)[i] = wet;

                minGainThisBlock = juce::jmin(minGainThisBlock, ps.currentGain);
            }
            ++readPos;
        }
        else
        {
            for (int ch = 0; ch < chans; ++ch)
                osBlock.getChannelPointer((size_t) ch)[i] = 0.0f;
        }
    }

    if (dspFactor > 1 && oversampler != nullptr)
        oversampler->processSamplesDown(block);

    for (int ch = 0; ch < chans; ++ch)
        buffer.copyFrom(ch, 0, pathScratch, ch, 0, numBaseSamples);

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
    // filter (see the capture loop at the top), so at mix=1 (fully bypassed) this is a
    // bit-exact pass-through, not an approximation. `buffer` holds the wet result now;
    // blend in the latency-aligned dry sample by however far bypassMix has ramped.
    float finalMix = 0.0f;
    for (int n = 0; n < numBaseSamples; ++n)
    {
        bypassMix.setTargetValue(bypassed ? 1.0f : 0.0f);
        const float mix = bypassMix.getNextValue();
        finalMix = mix;
        const juce::int64 srcAbs = baseWritePos + n - totalLatencyBaseSamples;

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
    // i.e. exactly what is about to reach the host — with its own oversampler that
    // never shares state with the DSP oversampler above. That decoupling is
    // deliberate: metering precision is set purely by sample rate, never by anything
    // else. This measurement ("True Peak Meter") always reports the real reconstructed
    // peak of whatever was actually produced, on or off, bypassed or not -- the True
    // Peak toggle only ever decides whether this peak drives gain reduction. Copies
    // into a pre-allocated scratch buffer first so this can never touch what's
    // actually sent to the host.
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

    // Snapshot the NORMAL final output (post Gain, limiting, Character, Ceiling clamp,
    // dry/wet crossfade -- exactly what the True Peak detector above just measured)
    // before any DELTA/LISTEN monitoring is applied: Peak/LUFS metering in
    // PluginProcessor reads THIS snapshot (see normalOutputForMetering()), not the
    // buffer that gets sent to the host, so DELTA/LISTEN can never produce a false
    // meter reading or a false TRUE PEAK OVER alert. Pre-allocated in prepare(); a
    // plain sample copy, no allocation.
    for (int ch = 0; ch < chans; ++ch)
        normalOutputSnapshot.copyFrom(ch, 0, buffer, ch, 0, numBaseSamples);
    lastProcessedNumSamples = numBaseSamples;

    // DELTA/LISTEN: a monitoring route only, never a sonic parameter. When engaged,
    // what's actually sent to the host becomes (aligned pre-limiter reference) minus
    // (this normal final output) -- everything the limiter removed or changed: gain
    // reduction, Character's colouration, and the ceiling clamp's own effect. The
    // reference was captured post-input-Gain / pre-limiting and already round-tripped
    // through deltaRefOversampler above (see gainedRefDelay's own comment for why),
    // which means it has ALREADY absorbed osLatencyBaseSamples of delay -- the same
    // amount the main wet path's own oversampling round trip absorbs. What remains to
    // align it with "normal" (delayed by the FULL totalLatencyBaseSamples =
    // lookaheadBaseSamples + osLatencyBaseSamples behind the input) is exactly
    // lookaheadBaseSamples, not the full total -- using the full total here would
    // double-count the oversampling latency and misalign the two signals by
    // osLatencyBaseSamples, reintroducing the same comb-filtering-style false Delta
    // this design otherwise avoids. Complementary raised-linear weights (old+new ==
    // 1.0, no equal-power bump) ramp over 15ms so toggling mid-playback never clicks.
    // deltaMix.getNextValue() is called exactly once per sample (not per channel), so
    // both channels always share the same instantaneous mix value. Bypass forces the
    // applied mix to 0 unconditionally (bypass cancels Delta) without disturbing the
    // ramp's own state, so Listen resumes exactly where it left off if bypass is later
    // released.
    for (int n = 0; n < numBaseSamples; ++n)
    {
        const float mix = deltaMix.getNextValue();
        const float appliedMix = bypassed ? 0.0f : mix;
        if (appliedMix <= 0.0f) continue; // skip the per-channel work entirely when Listen is fully off
        const juce::int64 srcAbs = baseWritePos - numBaseSamples + n - lookaheadBaseSamples;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& cs = channelState[(size_t) ch];
            auto* d = buffer.getWritePointer(ch);
            const float reference = srcAbs >= 0 ? cs.gainedRefDelay[(size_t) (srcAbs % dryDelayCapacity)] : 0.0f;
            const float normal = d[n];
            const float delta = reference - normal;
            d[n] = normal + appliedMix * (delta - normal);
        }
    }
}

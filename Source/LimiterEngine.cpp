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
        totalLatencyBaseSamples[(size_t) i] = lookaheadBaseSamples + osLatencyBaseSamples[(size_t) i];
    }

    // Fixed, worst-case latency across every factor -- reported to the host and never
    // changed afterwards (only recomputed here, in prepare(), on a genuine sample-rate/
    // block-size/channel-count change). Every factor's own shortfall against this figure
    // is made up with an internal, pure-delay padding stage (see runPath()), so every
    // factor's REAL physical latency is identical to the reported one at all times --
    // switching factors live never changes the host's plugin-delay-compensation.
    fixedLatencyBaseSamples = 0;
    for (int i = 0; i < kNumOsSlots; ++i)
        fixedLatencyBaseSamples = juce::jmax(fixedLatencyBaseSamples, totalLatencyBaseSamples[(size_t) i]);
    int maxPadding = 0;
    for (int i = 0; i < kNumOsSlots; ++i)
    {
        paddingSamplesForFactor[(size_t) i] = fixedLatencyBaseSamples - totalLatencyBaseSamples[(size_t) i];
        maxPadding = juce::jmax(maxPadding, paddingSamplesForFactor[(size_t) i]);
    }
    paddingRingCapacity = maxPadding + juce::jmax(1, maximumBlockSize) + 64;

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
    ceilRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 0.89125f);
    linkRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);
    tpBlendRampScratch.assign((size_t) juce::jmax(1, maximumBlockSize), 1.0f);

    // The dedicated gain analyzer's own delay must fit inside the existing 5ms
    // lookahead, or a base sample's true-peak-aware gain decision would need audio that
    // has already been emitted -- verified here (and by the latency-compliance tests),
    // not assumed. If this ever trips, the fix is a longer lookahead, not silently
    // truncating the compensation.
    jassert(dedicatedTpGainLatencyBaseSamples <= lookaheadBaseSamples);

    // dryDelayCapacity now sized off the fixed, factor-independent latency: the dry
    // path writes an entire block and reads it back in a second pass, so the ring must
    // hold at least one full block on top of the latency, or a large block wraps around
    // and clobbers samples before they're ever read.
    dryDelayCapacity = fixedLatencyBaseSamples + juce::jmax(1, maxBlockSize) + 64;

    // Rounded up to a power of two so every ring index below is a single AND against
    // tp8xPeakRingMask instead of an integer division: this ring is indexed twice per
    // dedicated-oversampled tick (write once per block, read once per main loop tick),
    // so at 192kHz/8x that is tens of thousands of indexing operations per second per
    // channel -- measured to be a significant, avoidable share of this analyzer's CPU
    // cost with a non-power-of-two modulus.
    const int gainFactorForRing = 1 << kDedicatedTpGainStages;
    const int minRingCapacityNeeded = dryDelayCapacity * gainFactorForRing;
    tp8xPeakRingCapacity = 1;
    while (tp8xPeakRingCapacity < minRingCapacityNeeded) tp8xPeakRingCapacity <<= 1;
    tp8xPeakRingMask = tp8xPeakRingCapacity - 1;

    for (auto& cs : channelState)
    {
        cs.dryBaseDelay.assign((size_t) dryDelayCapacity, 0.0f);
        cs.tp8xPeakRing.assign((size_t) tp8xPeakRingCapacity, 0.0f);
        for (auto& p : cs.path)
        {
            p.delay.assign((size_t) delayCapacity, 0.0f);
            p.minIdxRing.assign((size_t) minRingCapacity, (juce::int64) 0);
            p.minValRing.assign((size_t) minRingCapacity, 0.0f);
            p.paddingRing.assign((size_t) paddingRingCapacity, 0.0f);
            p.minHead = 0; p.minCount = 0; p.currentGain = 1.0f; p.heldDigitalPeak = 0.0f;
        }
    }
    for (auto& b : pathScratch) b.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);
    for (auto& b : pathOut) b.setSize(numChannels, juce::jmax(1, maximumBlockSize), false, false, true);

    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    pendingOsFactor.store(wanted);
    activePathFactor.store(wanted);
    activePathIdx = 0;
    warmingPathIdx = -1;
    crossfadeActive = false;
    crossfadeSamplesDone = 0;
    // A short, fixed crossfade -- a few ms is enough to be inaudible as a transition
    // while staying short relative to the lookahead warm-up time it follows.
    crossfadeTotalSamples = juce::jmax(1, (int) std::round(baseSampleRate * 0.005));

    for (int p = 0; p < kNumPaths; ++p)
    {
        pathTiming[(size_t) p] = PathTiming{};
        pathTiming[(size_t) p].factor = wanted;
        pathTiming[(size_t) p].paddingSamples = paddingSamplesForFactor[(size_t) factorToIndex(wanted)];
    }
    pathTiming[(size_t) activePathIdx].inUse = true;

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
    // The next setParameters() call snaps directly to its values instead of ramping
    // from these placeholders -- see the comment in setParameters() itself.
    parametersInitialized = false;

    reset();
}

void LimiterEngine::reset()
{
    baseWritePos = 0;
    warmingPathIdx = -1;
    crossfadeActive = false;
    crossfadeSamplesDone = 0;
    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    activePathIdx = 0;
    activePathFactor.store(wanted);

    for (int p = 0; p < kNumPaths; ++p)
    {
        auto& pt = pathTiming[(size_t) p];
        pt.writePos = 0; pt.readPos = 0; pt.baseSamplesIngested = 0; pt.paddingWritePos = 0;
        pt.factor = wanted;
        pt.paddingSamples = paddingSamplesForFactor[(size_t) factorToIndex(wanted)];
        pt.inUse = (p == activePathIdx);
    }

    for (auto& cs : channelState)
    {
        std::fill(cs.dryBaseDelay.begin(), cs.dryBaseDelay.end(), 0.0f);
        std::fill(cs.tp8xPeakRing.begin(), cs.tp8xPeakRing.end(), 0.0f);
        for (auto& p : cs.path)
        {
            std::fill(p.delay.begin(), p.delay.end(), 0.0f);
            std::fill(p.paddingRing.begin(), p.paddingRing.end(), 0.0f);
            p.minHead = 0; p.minCount = 0; p.currentGain = 1.0f; p.heldDigitalPeak = 0.0f;
        }
    }
    for (auto& os : oversamplers) if (os) os->reset();
    if (dedicatedTpOversampler) dedicatedTpOversampler->reset();
    if (dedicatedTpGainOversampler) dedicatedTpGainOversampler->reset();
    currentGrDb.store(0.0f);
}

void LimiterEngine::startWarmup(int newFactor)
{
    warmingPathIdx = 1 - activePathIdx;
    auto& wt = pathTiming[(size_t) warmingPathIdx];
    wt.writePos = 0; wt.readPos = 0; wt.baseSamplesIngested = 0; wt.paddingWritePos = 0;
    wt.factor = newFactor;
    wt.paddingSamples = paddingSamplesForFactor[(size_t) factorToIndex(newFactor)];
    wt.inUse = true;
    crossfadeActive = false;
    crossfadeSamplesDone = 0;

    for (auto& cs : channelState)
    {
        auto& ps = cs.path[(size_t) warmingPathIdx];
        std::fill(ps.delay.begin(), ps.delay.end(), 0.0f);
        std::fill(ps.paddingRing.begin(), ps.paddingRing.end(), 0.0f);
        ps.minHead = 0; ps.minCount = 0; ps.currentGain = 1.0f; ps.heldDigitalPeak = 0.0f;
    }
    // Safe to reset this factor's own oversampler: it cannot currently be feeding the
    // active path (the active path's own factor is different, or this path would not
    // be warming), so no audio in flight through it is disturbed.
    const int idx = factorToIndex(newFactor);
    if (oversamplers[(size_t) idx]) oversamplers[(size_t) idx]->reset();
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

float LimiterEngine::runPath(PathTiming& timing, int pathIdx, const juce::AudioBuffer<float>& rawInput,
                              juce::AudioBuffer<float>& outWet, int chans, int numBaseSamples,
                              const float* gainRamp, const float* ceilRamp, const float* linkRamp, const float* tpBlendRamp)
{
    const int factor = timing.factor;
    const int idx = factorToIndex(factor);
    // truePeakEnabled is included (not just the ramp values) because activation now
    // uses the analyzer's candidate at full strength from the very first tick (see the
    // max-combination below) rather than fading it in with tpBlendRamp -- so the ring
    // must be consulted from that same first tick, even in the edge case where this
    // whole block's tpBlendRamp values happen to still read as ~0.
    const bool needsTpGainAnalysis = truePeakEnabled || tpBlendRamp[0] > 1.0e-6f || tpBlendRamp[numBaseSamples - 1] > 1.0e-6f;

    auto& scratch = pathScratch[(size_t) pathIdx];
    for (int ch = 0; ch < chans; ++ch)
        scratch.copyFrom(ch, 0, rawInput, ch, 0, numBaseSamples);

    juce::dsp::AudioBlock<float> block(scratch.getArrayOfWritePointers(), (size_t) chans, (size_t) numBaseSamples);
    juce::dsp::AudioBlock<float> osBlock = block;
    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        osBlock = oversamplers[(size_t) idx]->processSamplesUp(block);

    const int osNumSamples = (int) osBlock.getNumSamples();
    const int lookaheadOs = lookaheadOsSamples[(size_t) idx];
    const double osSampleRate = baseSampleRate * (double) factor;
    float minGainThisBlock = 1.0f;

    for (int i = 0; i < osNumSamples; ++i)
    {
        const int nBase = i / factor;
        const float gainNow = gainRamp[nBase];
        const float ceilNow = ceilRamp[nBase];
        const float linkNow = linkRamp[nBase];
        const float tpBlendNow = tpBlendRamp[nBase];

        float peak[2] { 0.0f, 0.0f };
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& ps = channelState[(size_t) ch].path[(size_t) pathIdx];
            float xRaw = osBlock.getChannelPointer((size_t) ch)[i];
            if (! std::isfinite(xRaw)) xRaw = 0.0f;

            // Gain is applied exactly once, right here, only for the wet (limited) path.
            const float xGained = xRaw * gainNow;
            ps.delay[(size_t) (timing.writePos % delayCapacity)] = xGained;

            const float instantAbs = std::abs(xGained);
            if ((timing.writePos % factor) == 0) ps.heldDigitalPeak = instantAbs;

            // The True-Peak-on candidate comes from the dedicated, fixed-8x,
            // feed-forward analyzer (decoupled from `factor`, and SHARED between both
            // paths -- it never needs duplicating since its own precision never depends
            // on which path/factor is reading it) rather than this path's own
            // `instantAbs`. `factor` always evenly divides kGainAnalysisFactor (1/2/4/8
            // divide 8), so each of this base sample's `factor` ticks maps onto its own
            // equal-sized slice of the analyzer's 8 sub-samples for that base sample.
            float tpOnCandidate = instantAbs;
            if (needsTpGainAnalysis)
            {
                const int iWithinBase = i - nBase * factor;
                const int subPerTick = kGainAnalysisFactor / factor;
                const juce::int64 baseOsIdx = (baseWritePos + nBase) * kGainAnalysisFactor
                                             + iWithinBase * subPerTick - dedicatedTpGainLatencyOsSamples;
                // During the analyzer's own warm-up (the first ~39 base samples after
                // prepare()/reset(), before the delay-compensated ring position becomes
                // valid), baseOsIdx is negative: fall back to instantAbs rather than
                // 0.0f, which would silently zero the True-Peak-on candidate for that
                // window (with truePeakBlend at 1.0, that would suppress real,
                // legitimate cold-start gain reduction).
                float m = instantAbs;
                if (baseOsIdx >= 0)
                {
                    auto& sharedRing = channelState[(size_t) ch].tp8xPeakRing;
                    m = 0.0f;
                    for (int k = 0; k < subPerTick; ++k)
                        m = juce::jmax(m, sharedRing[(size_t) ((baseOsIdx + k) & tp8xPeakRingMask)]);
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
            // Measured: this was the actual cause of a real overshoot (up to ~1.4dB)
            // whenever a factor switch's warm-up happened to land during that ramp.
            // Fix: while True Peak is enabled (ramping up OR already fully on), use the
            // MAX of the two detectors -- a monotonically-safe combination that can
            // never be smaller than either input, so it never authorizes less
            // protection than either alone would. This deliberately ignores the blend
            // ramp for the decision itself (protection engages at full strength the
            // instant the toggle flips ON); audible smoothness still comes from the
            // existing lookahead window and release envelope, exactly as for any other
            // ordinary change in program peak -- no separate ramp is needed for that.
            // While True Peak is disabled (ramping down toward OFF), the plain linear
            // blend remains: giving up protection gradually is inherently safe (it can
            // only ever move the target toward "less limiting", never past the ceiling
            // it was already meeting), so there is no monotonic-safety requirement on
            // that direction, and easing off gradually avoids a level/character jump
            // right at the moment the toggle flips OFF.
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
            auto& ps = channelState[(size_t) ch].path[(size_t) pathIdx];
            const float finalGrDb = juce::jmap(linkNow, independentGrDb[ch], linkedGrDb);
            float target = bypassed ? 1.0f : juce::Decibels::decibelsToGain(finalGrDb, -100.0f);
            target = juce::jlimit(0.0f, 1.0f, target);
            pushMin(ps, timing.writePos, target, minRingCapacity);
        }

        ++timing.writePos;

        if (timing.writePos - timing.readPos > lookaheadOs)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto& ps = channelState[(size_t) ch].path[(size_t) pathIdx];
                popExpiredMin(ps, timing.readPos, minRingCapacity);
                const float windowMin = ps.minCount > 0 ? ps.minValRing[(size_t) ps.minHead] : 1.0f;

                if (bypassed)
                    ps.currentGain = 1.0f;
                else if (windowMin < ps.currentGain)
                    ps.currentGain = windowMin; // instant foresight-based attack (already ramped by the window)
                else
                    ps.currentGain = windowMin + computeReleaseCoeff(windowMin, osSampleRate) * (ps.currentGain - windowMin);

                float wet = ps.delay[(size_t) (timing.readPos % delayCapacity)] * ps.currentGain;
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
                // this residual clamp engages at full strength immediately rather than
                // fading in over the blend ramp, since the gain computer above is
                // already fully protecting from this same instant (via the `max`
                // combination) -- leaving this last-resort net blended in more slowly
                // would reopen exactly the gap the peak-candidate fix just closed for
                // whatever fraction of overshoot the gain computer doesn't already
                // catch (decimation-filter ringing). While deactivating, it still fades
                // out with tpBlendNow, matching the gradual release of protection above.
                const float clampBlend = truePeakEnabled ? 1.0f : tpBlendNow;
                wet += clampBlend * (clampedWet - wet);
                osBlock.getChannelPointer((size_t) ch)[i] = wet;

                minGainThisBlock = juce::jmin(minGainThisBlock, ps.currentGain);
            }
            ++timing.readPos;
        }
        else
        {
            for (int ch = 0; ch < chans; ++ch)
                osBlock.getChannelPointer((size_t) ch)[i] = 0.0f;
        }
    }

    if (factor > 1 && oversamplers[(size_t) idx] != nullptr)
        oversamplers[(size_t) idx]->processSamplesDown(block);

    // Internal fixed-latency padding: a pure per-channel delay of
    // timing.paddingSamples base-rate samples, so this path's TOTAL physical latency
    // (lookahead + this factor's own filter latency + this padding) always equals
    // fixedLatencyBaseSamples, regardless of which factor is running. This is what
    // makes a same-instant crossfade between two different-factor paths valid: both
    // describe the exact same moment in the original input.
    for (int ch = 0; ch < chans; ++ch)
    {
        auto& ps = channelState[(size_t) ch].path[(size_t) pathIdx];
        auto* d = scratch.getWritePointer(ch);
        for (int n = 0; n < numBaseSamples; ++n)
        {
            const juce::int64 wp = timing.paddingWritePos + n;
            ps.paddingRing[(size_t) (wp % paddingRingCapacity)] = d[n];
            const juce::int64 rp = wp - timing.paddingSamples;
            outWet.setSample(ch, n, rp >= 0 ? ps.paddingRing[(size_t) (rp % paddingRingCapacity)] : 0.0f);
        }
    }
    timing.paddingWritePos += numBaseSamples;
    timing.baseSamplesIngested += numBaseSamples;

    return minGainThisBlock;
}

void LimiterEngine::process(juce::AudioBuffer<float>& buffer)
{
    const int chans = juce::jmin(numChannels, buffer.getNumChannels());
    const int numBaseSamples = buffer.getNumSamples();

    // Start (or redirect) a warm-up if the target factor has changed. A crossfade
    // already in progress is never interrupted -- the newest request is picked up the
    // instant that crossfade completes and warmingPathIdx returns to -1.
    const int wanted = juce::jlimit(1, 8, pendingOsFactor.load());
    if (warmingPathIdx < 0)
    {
        if (wanted != pathTiming[(size_t) activePathIdx].factor)
            startWarmup(wanted);
    }
    else if (! crossfadeActive && wanted != pathTiming[(size_t) warmingPathIdx].factor)
    {
        startWarmup(wanted);
    }

    // Capture the untouched dry signal at the BASE rate, before oversampling even
    // begins — a pure sample delay that never runs through the oversampling filters,
    // so bypass is bit-exact (not just gain/limiter-free). It will be read back
    // latencySamples() behind the input, so it lands in perfect sync with the wet path.
    // baseWritePos is global and factor-independent (latency is fixed), so it is never
    // reset by a factor switch -- bypass alignment stays perfectly continuous through one.
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

    // Shared, per-base-sample parameter ramps: read by BOTH paths' per-tick loops (each
    // repeats the same base-sample value across its own `factor` sub-ticks). These tick
    // at the base rate unconditionally now (never per oversampled tick, and never reset
    // by a factor switch) specifically so two paths running at different factors during
    // a crossfade can't desync a shared SmoothedValue by consuming it at different rates.
    for (int n = 0; n < numBaseSamples; ++n)
    {
        inputGainRampScratch[(size_t) n] = inputGainSmoothed.getNextValue();
        ceilRampScratch[(size_t) n] = ceilingSmoothed.getNextValue();
        linkRampScratch[(size_t) n] = stereoLinkSmoothed.getNextValue();
        tpBlendRampScratch[(size_t) n] = truePeakBlend.getNextValue();
    }

    // Dedicated True Peak GAIN-DRIVING analysis: fixed 8x, feed-forward, decoupled from
    // the OVERSAMPLING selector's own factor -- shared, unduplicated, between both
    // paths. Runs UNCONDITIONALLY, whether True Peak is on, off, or mid-transition:
    // this ring is read `dedicatedTpGainLatencyOsSamples` sub-samples in the past by
    // runPath(), so if it were only written while True Peak is (or was about to be)
    // on, re-enabling it after any OFF period would read back stale data left over
    // from the last time it ran (or silence, if it never ran) for a short window right
    // after activation -- a real, measured protection gap at the exact moment True
    // Peak turns on, independent of and in addition to the blend-ramp issue fixed
    // below. Running it continuously means the ring is always caught up to "now minus
    // the analyzer's own latency", so there is never a stale-data window to read from,
    // at the cost of always paying this analyzer's own CPU (measured separately in the
    // benchmark) even while True Peak is fully off.
    {
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

    // Run the active path (always fully protected, never reset mid-stream) and, if a
    // switch is in flight, the warming path in parallel -- both fed the same input.
    const float activeMinGain = runPath(pathTiming[(size_t) activePathIdx], activePathIdx, buffer,
                                         pathOut[(size_t) activePathIdx], chans, numBaseSamples,
                                         inputGainRampScratch.data(), ceilRampScratch.data(),
                                         linkRampScratch.data(), tpBlendRampScratch.data());
    float warmMinGain = 1.0f;
    // Local sample index, WITHIN this block, at which the warm path first becomes
    // ready to crossfade -- numBaseSamples (i.e. "not this block") unless it is
    // computed below. This must be exact to the sample, not just "ready somewhere in
    // this block": checking readiness only once per process() CALL (after running the
    // whole block) would let a large block start blending in a warm path that had not
    // actually finished warming yet for that same block's EARLIER samples -- a real,
    // block-size-DEPENDENT protection gap (confirmed: this was the exact cause of a
    // measured ~3.4dB overshoot and a null-test/block-invariance failure before this
    // fix). Comparing baseSamplesIngested from before vs after this call, and computed
    // once per process() call, is entirely equivalent to checking every sample: the
    // count only ever advances by whole samples, in order, so the exact threshold
    // sample is recoverable without a per-sample loop.
    int warmReadySampleIdx = numBaseSamples;
    if (warmingPathIdx >= 0)
    {
        const juce::int64 ingestedBefore = pathTiming[(size_t) warmingPathIdx].baseSamplesIngested;
        warmMinGain = runPath(pathTiming[(size_t) warmingPathIdx], warmingPathIdx, buffer,
                               pathOut[(size_t) warmingPathIdx], chans, numBaseSamples,
                               inputGainRampScratch.data(), ceilRampScratch.data(),
                               linkRampScratch.data(), tpBlendRampScratch.data());
        // The warm path becomes eligible for crossfade once its own lookahead window is
        // populated ENTIRELY with decisions made after any shared parameter ramp
        // (inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed, truePeakBlend) has
        // fully settled -- not merely once the ramp has settled. The min-window only
        // ever remembers the last lookaheadBaseSamples-worth of pushed decisions
        // (older ones are continuously popped as the window slides); waiting for just
        // rampSettleBaseSamples ingested samples still leaves the window's own content
        // partly built from ticks where the ramp was mid-transition, since the last
        // ramp tick and the readiness check would land in the same instant. Requiring
        // the ramp to settle AND THEN a full additional lookahead to elapse guarantees
        // every entry remaining in the window was pushed after the ramp already
        // reached its target (measured: this two-stage margin was needed -- summing
        // the two, not just taking their max, eliminated a real, reproducible overshoot
        // when a factor switch and a True Peak toggle land at the same moment; pure
        // factor switching alone was already correct with no extra margin at all).
        const juce::int64 readyThreshold = rampSettleBaseSamples + lookaheadBaseSamples + 1;
        if (! crossfadeActive)
        {
            const juce::int64 ingestedAfter = pathTiming[(size_t) warmingPathIdx].baseSamplesIngested;
            if (ingestedAfter >= readyThreshold)
                warmReadySampleIdx = (int) juce::jmax((juce::int64) 0, readyThreshold - ingestedBefore);
        }
        else
        {
            warmReadySampleIdx = 0; // already running, ready from the start of this block
        }
    }

    // Combine active + warm output into the actual output buffer, per base sample.
    // Raised-cosine COMPLEMENTARY weights (oldGain + newGain == 1.0 exactly, monotonic,
    // no equal-power centre bump): for two highly-correlated signal versions, a linear
    // combination with weights summing to 1 can never exceed the larger of the two
    // inputs' magnitude at any instant (triangle inequality), so this step cannot itself
    // create a new true peak, unlike an equal-power (sin/cos, sum-of-squares = 1) curve.
    bool crossfadeFinishedThisBlock = false;
    for (int n = 0; n < numBaseSamples; ++n)
    {
        float oldW = 1.0f, newW = 0.0f;
        if (! crossfadeActive && n >= warmReadySampleIdx)
        {
            crossfadeActive = true;
            crossfadeSamplesDone = 0;
        }
        if (crossfadeActive)
        {
            const float t = juce::jlimit(0.0f, 1.0f, (float) crossfadeSamplesDone / (float) crossfadeTotalSamples);
            oldW = 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * t));
            newW = 1.0f - oldW;
            ++crossfadeSamplesDone;
            if (crossfadeSamplesDone >= crossfadeTotalSamples) crossfadeFinishedThisBlock = true;
        }
        for (int ch = 0; ch < chans; ++ch)
        {
            const float activeSample = pathOut[(size_t) activePathIdx].getSample(ch, n);
            const float warmSample = warmingPathIdx >= 0 ? pathOut[(size_t) warmingPathIdx].getSample(ch, n) : 0.0f;
            buffer.setSample(ch, n, oldW * activeSample + newW * warmSample);
        }
    }
    const float minGainThisBlock = crossfadeActive ? juce::jmin(activeMinGain, warmMinGain) : activeMinGain;

    if (crossfadeFinishedThisBlock)
    {
        pathTiming[(size_t) activePathIdx].inUse = false;
        activePathIdx = warmingPathIdx;
        warmingPathIdx = -1;
        crossfadeActive = false;
        crossfadeSamplesDone = 0;
        activePathFactor.store(pathTiming[(size_t) activePathIdx].factor);
    }

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
    // fixedLatencyBaseSamples replaces the old per-factor latencySamplesFor(factor):
    // since it never changes, this alignment is never disturbed by a factor switch.
    float finalMix = 0.0f;
    for (int n = 0; n < numBaseSamples; ++n)
    {
        bypassMix.setTargetValue(bypassed ? 1.0f : 0.0f);
        const float mix = bypassMix.getNextValue();
        finalMix = mix;
        const juce::int64 srcAbs = baseWritePos + n - fixedLatencyBaseSamples;

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

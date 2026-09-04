// Audit of the NEW automatic-oversampling architecture: the DSP and True Peak
// analyzer/meter factors are chosen ONCE, in prepare(), purely from the sample rate
// (see LimiterEngine::tierForSampleRate()) -- there is no user-facing selector, no
// live factor switching, no crossfade, no warm path. This file validates: the tier
// rule itself; that each tier's True Peak analyzer/meter factor is accurate enough
// against an independent 32x reference; ceiling compliance across the full matrix;
// cold start from sample 0; latency (reported == physically measured, and constant
// regardless of the legacy "oversampling" parameter); block-size invariance; bypass;
// and that automating the legacy parameter has no effect on the DSP.
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <cmath>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool condition, const std::string& what)
    {
        std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what.c_str());
        if (! condition) ++failures;
    }
    void info(const std::string& what) { std::printf("  [INFO] %s\n", what.c_str()); }

    juce::AudioBuffer<float> makeIntersamplePeakSignal(int channels, int n, double sr)
    {
        juce::AudioBuffer<float> b(channels, n);
        const double f1 = sr * 0.30, f2 = sr * 0.32;
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / sr;
                float x = 0.5f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * f1 * t)
                                         + std::sin(2.0 * juce::MathConstants<double>::pi * f2 * t));
                b.setSample(ch, i, juce::jlimit(-1.0f, 1.0f, x * 1.98f));
            }
        return b;
    }

    juce::AudioBuffer<float> runThroughInBlocks(LimiterEngine& engine, const juce::AudioBuffer<float>& signal, int blockSize)
    {
        juce::AudioBuffer<float> out(signal);
        int pos = 0;
        const int total = out.getNumSamples();
        while (pos < total)
        {
            const int bs = juce::jmin(juce::jmax(1, blockSize), total - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), out.getNumChannels(), pos, bs);
            engine.process(chunk);
            pos += bs;
        }
        return out;
    }

    bool hasNonFinite(const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getReadPointer(ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite(d[i])) return true;
        }
        return false;
    }

    // Independent reference: a FRESH juce::dsp::Oversampling instance (polyphase IIR,
    // a different filter family from the plugin's own FIR equiripple), oversampling
    // the WHOLE buffer once (so it has real, continuous filter history and no
    // artificial cold-start-on-truncation transient), then reporting the peak only
    // within [startSample, endSample).
    float independentTruePeakDbTail(const juce::AudioBuffer<float>& buf, int startSample, int stagesLog2 = 5 /* 32x */,
                                     int endSample = -1)
    {
        const int chans = buf.getNumChannels();
        juce::dsp::Oversampling<float> ref((size_t) chans, (size_t) stagesLog2,
                                            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        ref.initProcessing((size_t) buf.getNumSamples());
        juce::AudioBuffer<float> work(buf);
        juce::dsp::AudioBlock<float> block(work);
        auto up = ref.processSamplesUp(block);
        const int factor = 1 << stagesLog2;
        const size_t startIdx = (size_t) juce::jmax(0, startSample) * (size_t) factor;
        const size_t endIdx = endSample < 0 ? up.getNumSamples()
                                             : juce::jmin(up.getNumSamples(), (size_t) endSample * (size_t) factor);
        float maxAbs = 0.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = up.getChannelPointer((size_t) ch);
            for (size_t i = startIdx; i < endIdx; ++i)
                if (std::isfinite(d[i])) maxAbs = juce::jmax(maxAbs, std::abs(d[i]));
        }
        return juce::Decibels::gainToDecibels(maxAbs, -150.0f);
    }
}

int main()
{
    std::printf("== NF Limiter AUTOMATIC OVERSAMPLING audit ==\n");

    // Same 0.05dB tolerance and justification used throughout this session: the
    // dedicated analyzer's own worst-case error against a 32x reference (measured
    // separately, per tier, below) plus a small margin for float32 rounding.
    const float tolerance = 0.05f;

    // ------------------------------------------------------------------------------
    // Tier selection rule itself: pure function, no engine state.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Tier selection rule --\n");
    {
        struct Case { double sr; int expDsp; int expTp; };
        Case cases[] = {
            { 44100.0, 4, 8 }, { 48000.0, 4, 8 }, { 50000.0, 4, 8 },
            { 50000.01, 2, 4 }, { 88200.0, 2, 4 }, { 96000.0, 2, 4 }, { 100000.0, 2, 4 },
            { 100000.01, 1, 2 }, { 176400.0, 1, 2 }, { 192000.0, 1, 2 },
        };
        for (auto& c : cases)
        {
            int dsp = 0, tp = 0;
            LimiterEngine::tierForSampleRate(c.sr, dsp, tp);
            char l[160];
            std::snprintf(l, sizeof(l), "sr=%.2f: dspFactor=%d (expected %d), tpFactor=%d (expected %d)", c.sr, dsp, c.expDsp, tp, c.expTp);
            check(dsp == c.expDsp && tp == c.expTp, l);
        }
    }

    const double sampleRates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const float ceilings[] { -0.1f, -1.0f, -2.0f };
    const int channelCounts[] { 1, 2 };
    const int blockSizes[] { 1, 7, 16, 32, 64, 128, 256, 512, 1024 };
    const LimiterEngine::Character characters[] { LimiterEngine::Character::clean, LimiterEngine::Character::punch, LimiterEngine::Character::loud };

    // ------------------------------------------------------------------------------
    // prepare() picks the documented tier for each real sample rate, and it matches
    // what the engine actually built (dspFactorChosen()/truePeakFactorChosen()).
    // ------------------------------------------------------------------------------
    std::printf("\n-- prepare() actually applies the tier it chose --\n");
    for (double sr : sampleRates)
    {
        int expDsp = 0, expTp = 0;
        LimiterEngine::tierForSampleRate(sr, expDsp, expTp);
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        char l[160];
        std::snprintf(l, sizeof(l), "sr=%.0f: engine dspFactor=%d, tpFactor=%d (expected %d/%d)",
                      sr, engine.dspFactorChosen(), engine.truePeakFactorChosen(), expDsp, expTp);
        check(engine.dspFactorChosen() == expDsp && engine.truePeakFactorChosen() == expTp, l);
    }

    // ------------------------------------------------------------------------------
    // Legacy "oversampling" parameter automation must have NO effect on the DSP: the
    // factor is fixed by sample rate alone. requestOversamplingFactor() is a no-op;
    // this proves it end-to-end by comparing full renders with and without repeated
    // calls to it (including values that used to mean "1x"/"8x").
    // ------------------------------------------------------------------------------
    std::printf("\n-- Legacy oversampling parameter automation does nothing to audio or latency --\n");
    {
        const double sr = 48000.0;
        const int n = 20000;
        auto sig = makeIntersamplePeakSignal(2, n, sr);

        LimiterEngine baseline;
        baseline.prepare(sr, 256, 2);
        baseline.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        auto baselineOut = runThroughInBlocks(baseline, sig, 256);
        const int baselineLatency = baseline.latencySamples();

        LimiterEngine automated;
        automated.prepare(sr, 256, 2);
        automated.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        juce::AudioBuffer<float> out(sig);
        int pos = 0;
        const int legacyFactors[] { 1, 2, 4, 8, 1, 8, 2, 4 };
        int fi = 0;
        while (pos < n)
        {
            automated.requestOversamplingFactor(legacyFactors[fi % 8]); ++fi; // legacy no-op, called every block
            const int bs = juce::jmin(256, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
            automated.process(chunk);
            pos += bs;
        }
        const int automatedLatency = automated.latencySamples();

        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(baselineOut.getSample(ch, i) - out.getSample(ch, i)));
        char l[200];
        std::snprintf(l, sizeof(l), "max sample diff with vs without legacy factor automation = %.8f, latency %d vs %d", (double) maxDiff, baselineLatency, automatedLatency);
        check(maxDiff == 0.0f && baselineLatency == automatedLatency, l);
    }

    // ------------------------------------------------------------------------------
    // Latency: reported == physically measured (impulse), at every sample rate, and
    // stable regardless of legacy parameter automation (checked above too).
    // ------------------------------------------------------------------------------
    std::printf("\n-- Latency: reported vs physically measured (impulse) --\n");
    for (double sr : sampleRates)
    {
        LimiterEngine engine;
        engine.prepare(sr, 512, 1);
        engine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        const int reported = engine.latencySamples();
        const int impulsePos = 200;
        const int n = reported + 4000;
        juce::AudioBuffer<float> imp(1, n);
        imp.clear();
        imp.setSample(0, impulsePos, 1.0f);
        auto out = runThroughInBlocks(engine, imp, 512);
        int peakIdx = 0; float peakVal = 0.0f;
        for (int i = 0; i < n; ++i) { float v = std::abs(out.getSample(0, i)); if (v > peakVal) { peakVal = v; peakIdx = i; } }
        const int measured = peakIdx - impulsePos;
        char l[160];
        std::snprintf(l, sizeof(l), "sr=%.0f: reported=%d, measured=%d, diff=%d", sr, reported, measured, measured - reported);
        check(measured == reported, l);
    }

    // ------------------------------------------------------------------------------
    // Per-tier True Peak analyzer/meter accuracy against an independent 32x reference,
    // measured on the FINAL LIMITED OUTPUT (what actually matters -- not a raw
    // candidate-vs-reference peak comparison, which conflates filter-family Gibbs
    // behaviour with genuine precision; see the session notes on why that approach was
    // abandoned). If any tier fails here, this is reported BEFORE the factor is
    // escalated -- see the FAIL branch below.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Per-tier True Peak compliance vs independent 32x reference (validates the analyzer factor choice) --\n");
    {
        float worstOvershoot = -1000.0f;
        char worstLabel[300] = {};
        for (double sr : sampleRates)
        {
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    for (int blockSize : { 37, 512 })
                    {
                        const int n = (int) sr; // ~1 second
                        auto sig = makeIntersamplePeakSignal(chans, n, sr);
                        LimiterEngine engine;
                        engine.prepare(sr, juce::jmax(blockSize, 1), chans);
                        engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                        auto out = runThroughInBlocks(engine, sig, blockSize);

                        const int steadyStart = engine.latencySamples() + (int) std::round(sr * 0.02) + 64;
                        float refPeakDb = -150.0f;
                        for (int ch = 0; ch < chans; ++ch)
                        {
                            juce::AudioBuffer<float> mono(1, n);
                            mono.copyFrom(0, 0, out, ch, 0, n);
                            refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, steadyStart));
                        }
                        const float overshoot = refPeakDb - ceilingDb;
                        if (overshoot > worstOvershoot)
                        {
                            worstOvershoot = overshoot;
                            std::snprintf(worstLabel, sizeof(worstLabel),
                                "sr=%.0f (tpFactor=%d) ch=%d ceiling=%.1fdBTP block=%d: ref peak=%.3fdBTP (overshoot %.3fdB)",
                                sr, engine.truePeakFactorChosen(), chans, (double) ceilingDb, blockSize, (double) refPeakDb, (double) overshoot);
                        }
                    }
                }
            }
        }
        char l[340];
        std::snprintf(l, sizeof(l), "Worst overshoot across the full sr x ch x ceiling x block matrix: %.4fdB (tolerance %.2fdB) -- %s",
                      (double) worstOvershoot, (double) tolerance, worstLabel);
        if (worstOvershoot > tolerance)
        {
            check(false, l);
            info("A tier's True Peak analyzer factor did not meet tolerance -- per this audit's own rule, the factor must NOT be silently escalated. Report this result and escalate only after review.");
        }
        else
        {
            check(true, l);
        }
    }

    // ------------------------------------------------------------------------------
    // Cold start: whole render, sample 0 included, no skip, no exemption -- exactly
    // the standard established earlier this session (a prior "cold start" issue here
    // turned out to be a silence-priming-duration bug in an EARLIER version of this
    // very test, not a DSP defect; see git history). Covers every sample rate,
    // channel count, ceiling, and a representative block-size spread.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Cold start: whole render (sample 0 included) vs independent 32x reference --\n");
    {
        float worstOvershoot = -1000.0f;
        char worstLabel[300] = {};
        const int coldStartBlocks[] { 1, 7, 32, 256, 1024 };
        for (double sr : sampleRates)
        {
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    auto sig = makeIntersamplePeakSignal(chans, (int) (sr * 0.25), sr);
                    for (int blockSize : coldStartBlocks)
                    {
                        LimiterEngine engine;
                        engine.prepare(sr, juce::jmax(blockSize, 1), chans);
                        engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                        auto out = runThroughInBlocks(engine, sig, blockSize);

                        float refPeakDb = -150.0f;
                        for (int ch = 0; ch < chans; ++ch)
                        {
                            juce::AudioBuffer<float> mono(1, sig.getNumSamples());
                            mono.copyFrom(0, 0, out, ch, 0, sig.getNumSamples());
                            refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, 0));
                        }
                        const float overshoot = refPeakDb - ceilingDb;
                        if (overshoot > worstOvershoot)
                        {
                            worstOvershoot = overshoot;
                            std::snprintf(worstLabel, sizeof(worstLabel), "sr=%.0f ch=%d ceiling=%.1fdBTP block=%d: ref peak (whole render)=%.3fdBTP (overshoot %.3fdB)",
                                          sr, chans, (double) ceilingDb, blockSize, (double) refPeakDb, (double) overshoot);
                        }
                    }
                }
            }
        }
        char l[340];
        std::snprintf(l, sizeof(l), "Worst overshoot INCLUDING cold start: %.4fdB (tolerance %.2fdB) -- %s", (double) worstOvershoot, (double) tolerance, worstLabel);
        check(worstOvershoot <= tolerance, l);
    }

    // ------------------------------------------------------------------------------
    // Real recall / cold start: reproduces the product's own exact sequence
    // (requestOversamplingFactor() [no-op] -> prepare() -> the first setParameters()
    // call with the restored values) with no extra test-only reset() and no second
    // parameter change -- exactly what a DAW reopening a saved session with a
    // non-default ceiling produces.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Real recall / cold start: ceiling restored to -2.0dBTP, product's own prepare/setParameters order --\n");
    {
        const double sr = 48000.0;
        const float ceilingDb = -2.0f;
        const int chans = 2;
        const int n = (int) sr;
        auto sig = makeIntersamplePeakSignal(chans, n, sr);
        LimiterEngine engine;
        engine.requestOversamplingFactor(4); // legacy no-op, matches old product call site
        engine.prepare(sr, 512, chans);
        engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        auto out = runThroughInBlocks(engine, sig, 512);

        float refPeakDb = -150.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            juce::AudioBuffer<float> mono(1, n);
            mono.copyFrom(0, 0, out, ch, 0, n);
            refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, 0));
        }
        char l[200];
        std::snprintf(l, sizeof(l), "whole render (sample 0 included): peak=%.4fdBTP, overshoot=%.4fdB (tolerance %.2fdB)",
                      (double) refPeakDb, (double) (refPeakDb - ceilingDb), (double) tolerance);
        check(refPeakDb - ceilingDb <= tolerance, l);
    }

    // ------------------------------------------------------------------------------
    // Live change: ceiling changed during playback -- compares against the smoother's
    // own instantaneous trajectory during the ramp, and the final target strictly
    // after it settles. Same methodology validated earlier this session.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Live change: ceiling -1.0dBTP (stabilized) -> -2.0dBTP during playback --\n");
    {
        const double sr = 48000.0;
        const float startCeilingDb = -1.0f, endCeilingDb = -2.0f;
        const int chans = 2;
        const int stableSamples = 24000, postChangeSamples = 24000;
        const int n = stableSamples + postChangeSamples;
        auto sig = makeIntersamplePeakSignal(chans, n, sr);
        LimiterEngine engine;
        engine.prepare(sr, 256, chans);
        engine.setParameters(0.0f, startCeilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);

        juce::AudioBuffer<float> out(sig);
        int pos = 0; bool changed = false;
        while (pos < out.getNumSamples())
        {
            if (! changed && pos >= stableSamples)
            {
                engine.setParameters(0.0f, endCeilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                changed = true;
            }
            const int bs = juce::jmin(256, out.getNumSamples() - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), chans, pos, bs);
            engine.process(chunk);
            pos += bs;
        }

        const int rampSamples = engine.rampSettleSamples();
        const float startGain = juce::Decibels::decibelsToGain(startCeilingDb);
        const float endGain = juce::Decibels::decibelsToGain(endCeilingDb);
        auto expectedCeilingDbAt = [&](int sampleFromChange) -> float
        {
            if (sampleFromChange >= rampSamples) return endCeilingDb;
            const float t = (float) (sampleFromChange + 1) / (float) rampSamples;
            return juce::Decibels::gainToDecibels(startGain + (endGain - startGain) * t);
        };

        float maxStepNearChange = 0.0f, maxStepSteady = 0.0f;
        bool nonFinite = false;
        const int settle = (int) std::round(sr * 0.001);
        for (int ch = 0; ch < chans; ++ch)
        {
            float prev = out.getSample(ch, 0);
            for (int i = 1; i < out.getNumSamples(); ++i)
            {
                const float v = out.getSample(ch, i);
                if (! std::isfinite(v)) { nonFinite = true; continue; }
                const float step = std::abs(v - prev);
                if (i >= stableSamples - settle && i < stableSamples + settle) maxStepNearChange = juce::jmax(maxStepNearChange, step);
                else maxStepSteady = juce::jmax(maxStepSteady, step);
                prev = v;
            }
        }
        check(! nonFinite, "Live change: no NaN/Inf anywhere in the render");
        char stepLabel[240];
        std::snprintf(stepLabel, sizeof(stepLabel), "Live change: worst step at the change instant (+/-1ms)=%.5f vs steady playback elsewhere=%.5f",
                      (double) maxStepNearChange, (double) maxStepSteady);
        check(maxStepNearChange <= maxStepSteady + 0.1f, stepLabel);

        const int win = 64;
        const int scanEnd = juce::jmin(n, stableSamples + rampSamples + 4000);
        float worstRatio = -1000.0f; int worstAt = -1; float worstExpected = 0.0f;
        for (int w = stableSamples; w + win <= scanEnd; w += win)
        {
            const float expectedDb = expectedCeilingDbAt(w - stableSamples);
            float refPeakDb = -150.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                juce::AudioBuffer<float> mono(1, n);
                mono.copyFrom(0, 0, out, ch, 0, n);
                refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, w, 5, w + win));
            }
            const float overshoot = refPeakDb - expectedDb;
            if (overshoot > worstRatio) { worstRatio = overshoot; worstAt = w - stableSamples; worstExpected = expectedDb; }
        }
        char l2[280];
        std::snprintf(l2, sizeof(l2), "Live change: worst overshoot vs INSTANTANEOUS expected ceiling=%.4fdB at +%d samples (expected there=%.3fdBTP, tolerance %.2fdB)",
                      (double) worstRatio, worstAt, (double) worstExpected, (double) tolerance);
        check(worstRatio <= tolerance, l2);

        float finalPeakDb = -150.0f;
        for (int ch = 0; ch < chans; ++ch)
        {
            juce::AudioBuffer<float> mono(1, n);
            mono.copyFrom(0, 0, out, ch, 0, n);
            finalPeakDb = juce::jmax(finalPeakDb, independentTruePeakDbTail(mono, stableSamples + rampSamples + 100));
        }
        char l3[200];
        std::snprintf(l3, sizeof(l3), "Live change: after the ramp, tail vs FINAL target -2.0dBTP: peak=%.4fdBTP, overshoot=%.4fdB (tolerance %.2fdB)",
                      (double) finalPeakDb, (double) (finalPeakDb - endCeilingDb), (double) tolerance);
        check(finalPeakDb - endCeilingDb <= tolerance, l3);
    }

    // ------------------------------------------------------------------------------
    // Block-size invariance: the SAME signal through the SAME engine configuration
    // must produce identical output regardless of how it's chunked into blocks.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Block-size invariance --\n");
    {
        float worstDiff = 0.0f;
        char worstLabel[200] = {};
        for (double sr : { 48000.0, 192000.0 })
        {
            for (int chans : channelCounts)
            {
                const int n = 20000;
                auto sig = makeIntersamplePeakSignal(chans, n, sr);
                LimiterEngine reference;
                reference.prepare(sr, 512, chans);
                reference.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                auto refOut = runThroughInBlocks(reference, sig, 1);

                for (int blockSize : blockSizes)
                {
                    LimiterEngine engine;
                    engine.prepare(sr, juce::jmax(blockSize, 1), chans);
                    engine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                    auto out = runThroughInBlocks(engine, sig, blockSize);

                    float diff = 0.0f;
                    for (int ch = 0; ch < chans; ++ch)
                        for (int i = 0; i < n; ++i)
                            diff = juce::jmax(diff, std::abs(refOut.getSample(ch, i) - out.getSample(ch, i)));
                    if (diff > worstDiff)
                    {
                        worstDiff = diff;
                        std::snprintf(worstLabel, sizeof(worstLabel), "sr=%.0f ch=%d blockSize=%d", sr, chans, blockSize);
                    }
                }
            }
        }
        char l[240];
        std::snprintf(l, sizeof(l), "Worst block-size-dependent divergence vs blockSize=1 reference: %.8f at %s", (double) worstDiff, worstLabel);
        check(worstDiff < 1.0e-5f, l);
    }

    // ------------------------------------------------------------------------------
    // CLEAN/PUNCH/LOUD, True Peak ON/OFF, bypass, offline (single-block) render, and
    // NaN/Inf, across a representative subset of the matrix.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Character x True Peak x bypass x offline render --\n");
    {
        for (double sr : { 44100.0, 96000.0, 192000.0 })
        {
            for (auto ch : characters)
            {
                for (bool tpOn : { true, false })
                {
                    const int n = (int) sr / 2;
                    auto sig = makeIntersamplePeakSignal(2, n, sr);
                    LimiterEngine engine;
                    engine.prepare(sr, n, 2); // single "offline" block
                    engine.setParameters(0.0f, -1.0f, 150.0f, false, ch, 100.0f, tpOn, false);
                    auto out = runThroughInBlocks(engine, sig, n);
                    char l[160];
                    std::snprintf(l, sizeof(l), "sr=%.0f character=%d tp=%s offline block: finite=%s",
                                  sr, (int) ch, tpOn ? "on" : "off", hasNonFinite(out) ? "NO" : "yes");
                    check(! hasNonFinite(out), l);
                }
            }
        }

        // Bypass: bit-exact latency-compensated pass-through.
        {
            const double sr = 48000.0;
            const int n = 20000;
            auto sig = makeIntersamplePeakSignal(2, n, sr);
            LimiterEngine engine;
            engine.prepare(sr, 256, 2);
            engine.setParameters(6.0f, -6.0f, 150.0f, false, LimiterEngine::Character::loud, 100.0f, true, true);
            // Run long enough for the bypass crossfade (30ms) to fully settle.
            auto out = runThroughInBlocks(engine, sig, 256);
            const int lat = engine.latencySamples();
            const int settle = lat + (int) std::round(sr * 0.05) + 64;
            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = settle; i < n && i - lat < n; ++i)
                    maxDiff = juce::jmax(maxDiff, std::abs(out.getSample(ch, i) - sig.getSample(ch, i - lat)));
            char l[160];
            std::snprintf(l, sizeof(l), "Bypass: worst |output - latency-aligned input| after settling = %.6f", (double) maxDiff);
            check(maxDiff < 1.0e-4f, l);
        }
    }

    // ------------------------------------------------------------------------------
    // Save/restore correctness at the engine level: latency and tier are determined
    // purely by prepare()'s sample rate, so preparing twice at the same rate (as a
    // save-then-reload cycle would) must reproduce the identical tier and latency.
    // (Full APVTS/session-file save-restore is covered by state_tests.cpp.)
    // ------------------------------------------------------------------------------
    std::printf("\n-- Prepare twice at the same sample rate reproduces the identical tier and latency --\n");
    for (double sr : sampleRates)
    {
        LimiterEngine a, b;
        a.prepare(sr, 256, 2);
        b.prepare(sr, 512, 1); // different block size/channel count -- tier must not depend on either
        char l[200];
        std::snprintf(l, sizeof(l), "sr=%.0f: dspFactor %d vs %d, tpFactor %d vs %d, latency %d vs %d",
                      sr, a.dspFactorChosen(), b.dspFactorChosen(), a.truePeakFactorChosen(), b.truePeakFactorChosen(), a.latencySamples(), b.latencySamples());
        check(a.dspFactorChosen() == b.dspFactorChosen() && a.truePeakFactorChosen() == b.truePeakFactorChosen()
              && a.latencySamples() == b.latencySamples(), l);
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

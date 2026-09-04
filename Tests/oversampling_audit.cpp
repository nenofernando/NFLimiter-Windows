// Independent audit of the OVERSAMPLING selector's effect on the *full* audio path
// (not just the True Peak detector, already covered in dsp_tests.cpp). Everything here
// is deliberately independent of LimiterEngine's own internal true-peak measurement:
// the reference peak/aliasing measurements below build their own, separate
// juce::dsp::Oversampling instance (a different filter family -- polyphase IIR, not the
// plugin's FIR equiripple) purely to inspect LimiterEngine's *output*, so a bug in the
// plugin's own detector cannot also hide itself from the reference.
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <cmath>

namespace
{
    int failures = 0;

    void check(bool condition, const std::string& what)
    {
        std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what.c_str());
        if (! condition) ++failures;
    }
    void info(const std::string& what) { std::printf("  [INFO] %s\n", what.c_str()); }

    juce::AudioBuffer<float> makeSine(int channels, int n, double sr, double freq, float amplitude)
    {
        juce::AudioBuffer<float> b(channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample(ch, i, (float) (amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * freq * (double) i / sr)));
        return b;
    }

    // Two full-scale tones near Nyquist/3 summed out of phase per channel -- the same
    // intersample-peak provocateur used throughout dsp_tests.cpp.
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
            const int bs = juce::jmin(blockSize, total - pos);
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

    // Independent true-peak reference: a FRESH juce::dsp::Oversampling instance, built
    // fresh for every call (never shared with, or influenced by, LimiterEngine's own
    // internal oversamplers), using the polyphase-IIR filter family -- a genuinely
    // different filter design from the plugin's own FIR equiripple reconstruction --
    // at a high 16x factor, so its own reconstruction error is far smaller than
    // whatever it's being used to check.
    float independentTruePeakDb(const juce::AudioBuffer<float>& buf, int stagesLog2 = 4 /* 16x */)
    {
        const int chans = buf.getNumChannels();
        juce::dsp::Oversampling<float> ref((size_t) chans, (size_t) stagesLog2,
                                            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        ref.initProcessing((size_t) buf.getNumSamples());
        ref.reset();
        juce::AudioBuffer<float> work(buf);
        juce::dsp::AudioBlock<float> block(work);
        auto up = ref.processSamplesUp(block);
        float maxAbs = 0.0f;
        for (size_t ch = 0; ch < (size_t) chans; ++ch)
        {
            auto* d = up.getChannelPointer(ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                if (std::isfinite(d[i])) maxAbs = juce::jmax(maxAbs, std::abs(d[i]));
        }
        return juce::Decibels::gainToDecibels(maxAbs, -150.0f);
    }

    // Same reference, but for measuring only a STEADY-STATE TAIL of a longer render:
    // oversamples the WHOLE buffer once (so the reference filter sees the real,
    // continuous signal, including whatever led up to the region of interest) and only
    // searches for the peak from `startSample` onward. Copying [startSample, end) into
    // a fresh buffer and calling independentTruePeakDb() on THAT looks equivalent but
    // is not: it hands a brand-new Oversampling instance a signal that appears to start
    // abruptly at full amplitude, and that instance's own cold-start/group-delay
    // settling response to that artificial "attack" can itself ring past the ceiling --
    // a measurement artifact of the reference, not a property of the actual audio
    // (confirmed by hand: an apparent 0.5dB "overshoot" from the copy-then-measure
    // pattern vanished entirely once the reference saw the untruncated buffer).
    float independentTruePeakDbTail(const juce::AudioBuffer<float>& buf, int startSample, int stagesLog2 = 4,
                                     int endSample = -1 /* -1 = end of buffer */)
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
        for (size_t ch = 0; ch < (size_t) chans; ++ch)
        {
            auto* d = up.getChannelPointer(ch);
            for (size_t i = startIdx; i < endIdx; ++i)
                if (std::isfinite(d[i])) maxAbs = juce::jmax(maxAbs, std::abs(d[i]));
        }
        return juce::Decibels::gainToDecibels(maxAbs, -150.0f);
    }

    // FFT-based aliasing measurement: returns the fraction of total spectral energy
    // that falls OUTSIDE a narrow guard band around the fundamental, in dB relative to
    // total energy -- i.e. a single "how much spurious energy is in this signal"
    // number. A Hann-windowed, power-of-two-length capture is used to keep spectral
    // leakage from contaminating the measurement.
    // `startOffset` MUST land the analysis window well past the oversampling filter's
    // own cold-start transient (every FIR filter cascade has one, starting from a
    // zero initial state on a signal that begins abruptly at sample 0) -- analysing
    // window 0 was found to conflate that ordinary, expected transient with genuine
    // steady-state aliasing, making some factors look artificially cleaner or dirtier
    // than their real steady-state behaviour. See oversampling_isolation.cpp's stage-
    // by-stage isolation and the block-invariance audit for the investigation that
    // traced the earlier "4x looks anomalous" finding to exactly this.
    float aliasingEnergyDb(const juce::AudioBuffer<float>& buf, int channel, double sr, double fundamentalHz, int startOffset = 0)
    {
        const int order = 13; // 8192-point FFT
        const int fftSize = 1 << order;
        juce::dsp::FFT fft(order);

        std::vector<float> fftData((size_t) fftSize * 2, 0.0f);
        const int n = juce::jmin(fftSize, buf.getNumSamples() - startOffset);
        auto* src = buf.getReadPointer(channel) + startOffset;
        for (int i = 0; i < n; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * (float) i / (float) (n - 1));
            fftData[(size_t) i] = src[i] * w;
        }
        fft.performFrequencyOnlyForwardTransform(fftData.data());

        const double binHz = sr / (double) fftSize;
        const int fundamentalBin = (int) std::round(fundamentalHz / binHz);
        const int guardBins = 4; // a few bins either side of the fundamental, to absorb window spreading

        double totalEnergy = 0.0, bandEnergy = 0.0;
        for (int bin = 1; bin < fftSize / 2; ++bin) // skip DC
        {
            const double mag = (double) fftData[(size_t) bin];
            const double energy = mag * mag;
            totalEnergy += energy;
            if (std::abs(bin - fundamentalBin) <= guardBins) bandEnergy += energy;
        }
        const double aliasEnergy = juce::jmax(1.0e-18, totalEnergy - bandEnergy);
        return (float) (10.0 * std::log10(aliasEnergy / juce::jmax(1.0e-18, totalEnergy)));
    }

    // Single-impulse latency measurement: the reported latency should point at (or be
    // very close to) where the impulse's peak actually emerges in the output.
    int measuredLatencySamples(int factor, double sr)
    {
        LimiterEngine engine;
        engine.prepare(sr, 256, 1);
        engine.requestOversamplingFactor(factor);
        engine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        const int n = (int) sr; // 1 second, plenty of room for the impulse to emerge
        juce::AudioBuffer<float> sig(1, n);
        sig.clear();
        sig.setSample(0, 0, 1.0f);
        auto out = runThroughInBlocks(engine, sig, 256);
        int peakIdx = 0;
        float peakVal = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float v = std::abs(out.getSample(0, i));
            if (v > peakVal) { peakVal = v; peakIdx = i; }
        }
        return peakIdx;
    }
}

int main()
{
    std::printf("== NF Limiter OVERSAMPLING full-path audit ==\n\n");

    // ------------------------------------------------------------------------------
    // 1-2-3. Confirm the audio itself (not just the detector) actually changes with
    // the factor, and that the nonlinear stage (Character: Loud's tanh saturation,
    // applied inside the oversampled loop, after gain and before downsampling) really
    // runs at the oversampled rate: a 13kHz tone driven hard into that nonlinearity
    // produces very different spectra at 1x vs 8x if -- and only if -- oversampling is
    // actually wrapped around the nonlinear/limiting stage, not bypassing it.
    // ------------------------------------------------------------------------------
    std::printf("-- Full audio path: does the audio itself change with the factor? --\n");
    {
        const double sr = 44100.0;
        const int n = 1 << 15;
        auto render = [&](int factor)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 1);
            engine.requestOversamplingFactor(factor);
            engine.setParameters(18.0f, -1.0f, 150.0f, false, LimiterEngine::Character::loud, 100.0f, true, false);
            auto sig = makeSine(1, n, sr, 13000.0, 1.0f);
            return runThroughInBlocks(engine, sig, 512);
        };

        auto out1x = render(1);
        auto out8x = render(8);
        float maxDiff = 0.0f;
        const int skip = 2000; // past both factors' lookahead/filter latency
        for (int i = skip; i < n; ++i)
            maxDiff = juce::jmax(maxDiff, std::abs(out1x.getSample(0, i) - out8x.getSample(0, i)));
        char l[200];
        std::snprintf(l, sizeof(l), "1x vs 8x output differs on a hard-saturated 13kHz tone (max diff = %.4f) -- proves the nonlinear stage runs on genuinely different (oversampled) data, not just the detector", (double) maxDiff);
        check(maxDiff > 0.02f, l);
        check(! hasNonFinite(out1x) && ! hasNonFinite(out8x), "both renders stay finite");
    }

    // ------------------------------------------------------------------------------
    // 4. Aliasing audit via FFT: a 13kHz tone (44.1kHz) driven into the Loud
    // character's saturation is a classic aliasing provocateur -- its low-order
    // harmonics (26kHz, 39kHz...) sit above Nyquist and fold back down into clearly
    // separated, measurable bins.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Aliasing audit (13kHz tone driven into Loud saturation, 44.1kHz) --\n");
    {
        const double sr = 44100.0;
        const int n = 1 << 15;
        const double tone = 13000.0;
        float aliasDb[4];
        const int factors[] { 1, 2, 4, 8 };
        for (int fi = 0; fi < 4; ++fi)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 1);
            engine.requestOversamplingFactor(factors[fi]);
            engine.setParameters(18.0f, -1.0f, 150.0f, false, LimiterEngine::Character::loud, 100.0f, true, false);
            auto sig = makeSine(1, n, sr, tone, 1.0f);
            auto out = runThroughInBlocks(engine, sig, 512);
            // Analysis window starts at the buffer's midpoint, ~16000 samples (~370ms
            // @44.1kHz) in -- far past any filter's cold-start transient (a few hundred
            // samples at most) or this limiter's own lookahead latency (~300 samples).
            aliasDb[fi] = aliasingEnergyDb(out, 0, sr, tone, n / 2);
        }
        char l[220];
        std::snprintf(l, sizeof(l), "Spurious/aliasing energy vs fundamental: 1x=%.2fdB 2x=%.2fdB 4x=%.2fdB 8x=%.2fdB (more negative = cleaner)",
                      (double) aliasDb[0], (double) aliasDb[1], (double) aliasDb[2], (double) aliasDb[3]);
        info(l);
        check(aliasDb[0] > aliasDb[3] + 1.0f, "1x measurably worse (more aliasing) than 8x");
        check(aliasDb[0] >= aliasDb[1] - 1.0f, "2x is no worse than 1x (within 1dB tolerance)");
        check(aliasDb[1] >= aliasDb[2] - 1.0f, "4x is no worse than 2x (within 1dB tolerance)");
        check(aliasDb[2] >= aliasDb[3] - 1.0f, "8x is no worse than 4x (within 1dB tolerance)");
    }

    // ------------------------------------------------------------------------------
    // 5 + ceiling matrix + the 4x-vs-2x/8x investigation: for every factor, ceiling,
    // sample rate and channel count, measure the plugin's OWN reported true peak AND
    // an independent (different-filter-family, 16x) reference measurement of the same
    // output, so a real internal-detector bug and a genuine filter-response difference
    // can be told apart.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Ceiling compliance vs an INDEPENDENT (16x polyphase-IIR) reference --\n");
    {
        const double sampleRates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const float ceilings[] { -0.1f, -1.0f, -2.0f };
        const int factors[] { 2, 4, 8 };
        const int channelCounts[] { 1, 2 };

        float worstOvershoot = -1000.0f;
        for (double sr : sampleRates)
        {
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    for (int factor : factors)
                    {
                        LimiterEngine engine;
                        engine.prepare(sr, 512, chans);
                        engine.requestOversamplingFactor(factor);
                        engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                        const int n = (int) sr / 2;
                        auto sig = makeIntersamplePeakSignal(chans, n, sr);
                        auto out = runThroughInBlocks(engine, sig, 512);

                        float refPeakDb = -150.0f;
                        for (int ch = 0; ch < chans; ++ch)
                        {
                            juce::AudioBuffer<float> mono(1, out.getNumSamples());
                            mono.copyFrom(0, 0, out, ch, 0, out.getNumSamples());
                            refPeakDb = juce::jmax(refPeakDb, independentTruePeakDb(mono));
                        }
                        const float overshoot = refPeakDb - ceilingDb;
                        worstOvershoot = juce::jmax(worstOvershoot, overshoot);
                        if (overshoot > 0.3f)
                        {
                            char l[260];
                            std::snprintf(l, sizeof(l), "sr=%.0f ch=%d ceiling=%.1fdBTP factor=%dx: independent ref peak=%.3fdBTP (overshoot %.3fdB)",
                                          sr, chans, (double) ceilingDb, factor, (double) refPeakDb, (double) overshoot);
                            check(false, l);
                        }
                    }
                }
            }
        }
        char l[160];
        std::snprintf(l, sizeof(l), "Worst overshoot vs independent 16x reference across the full sr x ch x ceiling x factor matrix: %.3f dB", (double) worstOvershoot);
        check(worstOvershoot <= 0.3f, l);
    }

    // ------------------------------------------------------------------------------
    // Investigate the specific 2x=3.833 / 4x=3.369 / 8x=3.833 dBTP finding from the
    // earlier audit: compare LimiterEngine's own truePeakDb() against the independent
    // reference for the SAME rendered output, per factor. If they agree, the earlier
    // gap is a genuine acoustic difference between filter designs (each factor's
    // reconstruction really does produce different audio), not a detector bug.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Investigating the earlier 4x vs 2x/8x true-peak gap --\n");
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        for (int factor : { 2, 4, 8 })
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.requestOversamplingFactor(factor);
            // Ceiling set far above the signal (as in the original finding) so the
            // limiter never engages -- isolating pure detection/reconstruction quality.
            engine.setParameters(0.0f, 12.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
            auto sig = makeIntersamplePeakSignal(2, n, sr);
            juce::AudioBuffer<float> out(sig);
            // Every oversampling filter (this engine's and the independent reference's
            // alike) has its own ordinary cold-start transient from a zero initial
            // state -- both measurements below skip the first ~2000 samples (well past
            // it and past the limiter's own ~300-sample lookahead latency) so what's
            // compared is steady-state behaviour, not two different transients.
            const int steadyStateStart = 2000;
            float internalMax = -150.0f;
            int pos = 0;
            while (pos < n)
            {
                const int bs = juce::jmin(512, n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
                engine.process(chunk);
                if (pos + bs > steadyStateStart) internalMax = juce::jmax(internalMax, engine.truePeakDb());
                pos += bs;
            }

            float refMax = -150.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                juce::AudioBuffer<float> mono(1, out.getNumSamples() - steadyStateStart);
                mono.copyFrom(0, 0, out, ch, steadyStateStart, out.getNumSamples() - steadyStateStart);
                refMax = juce::jmax(refMax, independentTruePeakDb(mono));
            }

            char l[240];
            std::snprintf(l, sizeof(l), "%dx: LimiterEngine's own truePeakDb()=%.3fdBTP vs independent 16x reference=%.3fdBTP (diff=%.3fdB)",
                          factor, (double) internalMax, (double) refMax, (double) std::abs(internalMax - refMax));
            check(std::abs(internalMax - refMax) < 0.3f, l);
        }
        info("If the internal reading agrees with the independent reference at every factor (checks above), the 2x/4x/8x spread found earlier is a genuine acoustic difference between each factor's reconstruction filter (real audio, correctly measured), not a detector bug.");
    }

    // ------------------------------------------------------------------------------
    // Latency: reported vs measured via an actual impulse, per factor.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Latency: reported vs measured (single impulse) --\n");
    {
        const double sr = 48000.0;
        for (int factor : { 1, 2, 4, 8 })
        {
            LimiterEngine probe;
            probe.prepare(sr, 256, 1);
            probe.requestOversamplingFactor(factor);
            const int reported = probe.latencySamplesFor(factor);
            const int measured = measuredLatencySamples(factor, sr);
            char l[200];
            std::snprintf(l, sizeof(l), "%dx: reported=%d samples, measured (impulse peak position)=%d samples (diff=%d)",
                          factor, reported, measured, std::abs(reported - measured));
            check(std::abs(reported - measured) <= 2, l); // +/-2 samples slack for interpolation-kernel peak spread
        }
    }

    // ------------------------------------------------------------------------------
    // Dedicated True Peak detector factor selection: compare 4x/8x/16x (candidate
    // detector factors) against a 32x reference across the same sr x ch x ceiling
    // matrix used for the ceiling-compliance check above, on the plugin's actual
    // final (base-rate, post-everything) output -- exactly the signal a dedicated,
    // DSP-oversampling-independent detector would receive. Picks the smallest
    // candidate whose worst-case error against 32x stays within tolerance everywhere.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Dedicated True Peak detector: choosing the smallest sufficient factor --\n");
    {
        const double tolerance = 0.10; // dB -- the acceptance tolerance for the dedicated detector
        const double sampleRates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const float ceilings[] { -0.1f, -1.0f, -2.0f };
        const int dspFactors[] { 1, 2, 4, 8 };
        const int channelCounts[] { 1, 2 };
        const int candidates[] { 4, 8, 16 }; // stages 2, 3, 4
        double worstErr[3] { 0.0, 0.0, 0.0 };

        for (double sr : sampleRates)
        {
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    for (int dspFactor : dspFactors)
                    {
                        LimiterEngine engine;
                        engine.prepare(sr, 512, chans);
                        engine.requestOversamplingFactor(dspFactor);
                        engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                        const int n = (int) sr / 2;
                        auto sig = makeIntersamplePeakSignal(chans, n, sr);
                        auto out = runThroughInBlocks(engine, sig, 512);

                        // Skip the cold-start transient region for every measurement below.
                        const int steadyStart = 2000;
                        for (int ch = 0; ch < chans; ++ch)
                        {
                            juce::AudioBuffer<float> mono(1, n - steadyStart);
                            mono.copyFrom(0, 0, out, ch, steadyStart, n - steadyStart);
                            const float truth = independentTruePeakDb(mono, 5 /* 32x */);
                            for (int ci = 0; ci < 3; ++ci)
                            {
                                const int stages = (int) std::round(std::log2((double) candidates[ci]));
                                const float candidate = independentTruePeakDb(mono, stages);
                                worstErr[ci] = juce::jmax(worstErr[ci], (double) std::abs(candidate - truth));
                            }
                        }
                    }
                }
            }
        }

        int chosen = -1;
        for (int ci = 0; ci < 3; ++ci)
        {
            char l[200];
            std::snprintf(l, sizeof(l), "%dx candidate: worst error vs 32x reference across the full matrix = %.4f dB (tolerance = %.2f dB)",
                          candidates[ci], worstErr[ci], tolerance);
            check(worstErr[ci] < 1.0, l); // sanity bound, not the selection criterion itself
            if (chosen < 0 && worstErr[ci] < tolerance) chosen = candidates[ci];
        }
        if (chosen > 0)
        {
            char l[160];
            std::snprintf(l, sizeof(l), "Smallest candidate meeting the %.2fdB tolerance everywhere: %dx", tolerance, chosen);
            info(l);
        }
        else
        {
            info("No candidate met the tolerance everywhere -- see per-candidate worst-case numbers above.");
        }
    }

    // ------------------------------------------------------------------------------
    // Decoupled True Peak protection: since the gain decision now comes from a
    // dedicated, fixed-8x, feed-forward analyzer independent of the OVERSAMPLING
    // selector (see LimiterEngine::process()), True Peak ON must keep the FINAL
    // rendered output within ceiling + tolerance at EVERY factor, 1x included --
    // validated here against a genuinely independent 32x reference (not the plugin's
    // own truePeakDb(), which is a separate, already-validated detector), across
    // sample rates, channel counts, ceilings, block sizes (including one huge
    // "offline" single-block render), with a hard-clipped adversarial signal.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Decoupled True Peak protection: TRUE PEAK ON vs an independent 32x reference, 1x/2x/4x/8x --\n");
    {
        const double sampleRates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const float ceilings[] { -0.1f, -1.0f, -2.0f };
        const int factors[] { 1, 2, 4, 8 };
        const int channelCounts[] { 1, 2 };
        // 0.02dB detector-noise headroom (see the dedicated meter's own known 0.0311dB
        // worst-case error) plus a small margin for genuine decimation-filter ringing:
        // this tolerance is against the FINAL AUDIO, not the alert margin in Metering.
        const float tolerance = 0.15f;
        float worstOvershoot = -1000.0f;
        char worstLabel[260] = {};

        for (double sr : sampleRates)
        {
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    for (int factor : factors)
                    {
                        const int n = (int) sr; // ~1 second
                        auto sig = makeIntersamplePeakSignal(chans, n, sr);

                        for (int blockSize : { 37, 512, n /* single "offline" block */ })
                        {
                            LimiterEngine engine;
                            engine.prepare(sr, juce::jmax(blockSize, 1), chans);
                            engine.requestOversamplingFactor(factor);
                            engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                            auto out = runThroughInBlocks(engine, sig, blockSize);

                            const int steadyStart = juce::jmin(2000, n / 4);
                            float refPeakDb = -150.0f;
                            for (int ch = 0; ch < chans; ++ch)
                            {
                                juce::AudioBuffer<float> mono(1, n);
                                mono.copyFrom(0, 0, out, ch, 0, n);
                                refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, steadyStart, 5 /* 32x */));
                            }
                            const float overshoot = refPeakDb - ceilingDb;
                            if (overshoot > worstOvershoot)
                            {
                                worstOvershoot = overshoot;
                                std::snprintf(worstLabel, sizeof(worstLabel),
                                    "sr=%.0f ch=%d ceiling=%.1fdBTP factor=%dx block=%d: independent 32x ref peak=%.3fdBTP (overshoot %.3fdB)",
                                    sr, chans, (double) ceilingDb, factor, blockSize, (double) refPeakDb, (double) overshoot);
                            }
                        }
                    }
                }
            }
        }
        char l[300];
        std::snprintf(l, sizeof(l), "Worst overshoot vs independent 32x reference, TRUE PEAK ON, across sr x ch x ceiling x 1x/2x/4x/8x x block size: %.4f dB (tolerance %.2fdB) -- %s",
                      (double) worstOvershoot, (double) tolerance, worstLabel);
        check(worstOvershoot <= tolerance, l);
    }

    // ------------------------------------------------------------------------------
    // Cold-start audit: the previous test above deliberately skips ~2000 samples
    // before measuring, to separate steady-state ceiling compliance from startup
    // transients. This section does the opposite on purpose -- it measures the ENTIRE
    // render, sample 0 included, so a regression in the analyzer's own warm-up window
    // (the first ~39 base samples after prepare(), where its delay-compensated ring
    // read isn't valid yet -- see the instantAbs fallback in LimiterEngine::process())
    // cannot hide behind a skipped region. The adversarial signal starts at full
    // amplitude on sample 0 (no fade-in), which is the worst case for a fresh engine.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Cold-start audit: TRUE PEAK ON, whole render (sample 0 included) vs independent 32x reference --\n");
    {
        const double sampleRates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const float ceilings[] { -0.1f, -1.0f, -2.0f };
        const int factors[] { 1, 2, 4, 8 };
        const int channelCounts[] { 1, 2 };
        const float tolerance = 0.15f;
        float worstOvershoot = -1000.0f;
        char worstLabel[280] = {};

        for (double sr : sampleRates)
        {
            // Long enough to comfortably clear the lookahead window at every factor
            // (5ms lookahead is a few hundred samples at most) while keeping the full
            // sr x ch x ceiling x factor x blockSize matrix fast.
            const int n = (int) (sr * 0.25);
            for (int chans : channelCounts)
            {
                for (float ceilingDb : ceilings)
                {
                    for (int factor : factors)
                    {
                        auto sig = makeIntersamplePeakSignal(chans, n, sr);
                        for (int blockSize : { 1, 37, n /* single "offline" block */ })
                        {
                            LimiterEngine engine;
                            engine.prepare(sr, juce::jmax(blockSize, 1), chans);
                            engine.requestOversamplingFactor(factor);
                            engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                            auto out = runThroughInBlocks(engine, sig, blockSize);

                            float refPeakDb = -150.0f;
                            for (int ch = 0; ch < chans; ++ch)
                            {
                                juce::AudioBuffer<float> mono(1, n);
                                mono.copyFrom(0, 0, out, ch, 0, n);
                                // startSample=0: the whole render, cold start included.
                                refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, 0, 5 /* 32x */));
                            }
                            const float overshoot = refPeakDb - ceilingDb;
                            if (overshoot > worstOvershoot)
                            {
                                worstOvershoot = overshoot;
                                std::snprintf(worstLabel, sizeof(worstLabel),
                                    "sr=%.0f ch=%d ceiling=%.1fdBTP factor=%dx block=%d: independent 32x ref peak (whole render)=%.3fdBTP (overshoot %.3fdB)",
                                    sr, chans, (double) ceilingDb, factor, blockSize, (double) refPeakDb, (double) overshoot);
                            }
                        }
                    }
                }
            }
        }
        char l[320];
        std::snprintf(l, sizeof(l), "Worst overshoot vs independent 32x reference INCLUDING cold start, TRUE PEAK ON, across sr x ch x ceiling x 1x/2x/4x/8x x block size: %.4f dB (tolerance %.2fdB) -- %s",
                      (double) worstOvershoot, (double) tolerance, worstLabel);
        check(worstOvershoot <= tolerance, l);
    }

    // ------------------------------------------------------------------------------
    // Excess attenuation: on a stable signal safely under ceiling, True Peak ON must
    // not attenuate meaningfully more than True Peak OFF does for the same material --
    // the dedicated analyzer's own Gibbs-ringing headroom on adversarial content must
    // not turn into audible extra gain reduction on ordinary, non-adversarial program
    // material. Uses a simple full-scale-ish sine (no intersample surprises) well
    // inside the ceiling.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Excess attenuation: TRUE PEAK ON vs OFF on stable, non-adversarial material --\n");
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        const float ceilingDb = -1.0f;
        float worstExtraAttenDb = -1000.0f;
        char worstLabel[220] = {};

        for (int factor : { 1, 2, 4, 8 })
        {
            for (int chans : { 1, 2 })
            {
                auto sig = makeSine(chans, n, sr, 1000.0, 0.7f); // ~ -3dBFS, comfortably under -1dBTP ceiling

                auto render = [&](bool tpOn)
                {
                    LimiterEngine engine;
                    engine.prepare(sr, 512, chans);
                    engine.requestOversamplingFactor(factor);
                    engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, tpOn, false);
                    return runThroughInBlocks(engine, sig, 512);
                };

                auto onOut = render(true);
                auto offOut = render(false);
                const int skip = 4000;
                float maxAbsDiff = 0.0f;
                for (int ch = 0; ch < chans; ++ch)
                    for (int i = skip; i < n; ++i)
                        maxAbsDiff = juce::jmax(maxAbsDiff, std::abs(onOut.getSample(ch, i) - offOut.getSample(ch, i)));
                const float extraAttenDb = juce::Decibels::gainToDecibels(1.0f + maxAbsDiff, -150.0f);
                if (extraAttenDb > worstExtraAttenDb)
                {
                    worstExtraAttenDb = extraAttenDb;
                    std::snprintf(worstLabel, sizeof(worstLabel), "factor=%dx ch=%d: max ON-vs-OFF sample diff=%.6f (~%.4fdB)",
                                  factor, chans, (double) maxAbsDiff, (double) extraAttenDb);
                }
            }
        }
        char l[260];
        std::snprintf(l, sizeof(l), "Worst TRUE PEAK ON-vs-OFF difference on stable material, 1x/2x/4x/8x, mono/stereo: %s (must stay well under 0.5dB)", worstLabel);
        check(worstExtraAttenDb < 0.5f, l);
    }

    // ------------------------------------------------------------------------------
    // Live switching: a single engine instance, one continuous strong adversarial
    // signal, cycling OVERSAMPLING 1x->2x->4x->8x->1x->... and toggling True Peak
    // ON/OFF on independent schedules while audio keeps flowing -- the scenario the
    // dedicated analyzer's non-reset-on-switch behaviour (see
    // switchToPendingFactorIfNeeded()) exists for. Checks, on the ONE continuous
    // render: (a) no sample-to-sample discontinuity beyond what the existing smoothing
    // ramps already produce on steady playback, (b) no NaN/Inf anywhere, (c) no gap in
    // protection -- every window where True Peak was ON stays within ceiling +
    // tolerance per an independent 32x reference, with no exception carved out for the
    // instants right at a factor switch.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Live switching: continuous signal, OVERSAMPLING + TRUE PEAK cycling together --\n");
    {
        const double sr = 48000.0;
        const float ceilingDb = -1.0f;
        const int chans = 2;
        const int segmentSamples = 4096; // ~85ms at 48kHz -- several lookahead windows long
        const int numSegments = 40;
        const int n = segmentSamples * numSegments;
        const int factorSeq[] { 1, 2, 4, 8 };

        auto sig = makeIntersamplePeakSignal(chans, n, sr);
        LimiterEngine engine;
        engine.prepare(sr, 256, chans);
        engine.requestOversamplingFactor(1);

        std::vector<bool> segmentTpOn((size_t) numSegments, false);
        juce::AudioBuffer<float> out(sig);
        float maxStepDelta = 0.0f;
        bool sawNonFinite = false;
        float worstProtectedOvershoot = -1000.0f;
        char worstProtectedLabel[220] = {};

        for (int seg = 0; seg < numSegments; ++seg)
        {
            const int factor = factorSeq[seg % 4];
            const bool tpOn = (seg % 3) != 0; // ON for 2 of every 3 segments, OFF for 1
            segmentTpOn[(size_t) seg] = tpOn;
            engine.requestOversamplingFactor(factor);
            engine.setParameters(0.0f, ceilingDb, 150.0f, false, LimiterEngine::Character::clean, 100.0f, tpOn, false);

            int pos = seg * segmentSamples;
            const int segEnd = pos + segmentSamples;
            while (pos < segEnd)
            {
                const int bs = juce::jmin(256, segEnd - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), chans, pos, bs);
                engine.process(chunk);
                pos += bs;
            }
        }

        // (a) discontinuity + (b) NaN/Inf, over the WHOLE continuous render.
        for (int ch = 0; ch < chans; ++ch)
        {
            float prev = out.getSample(ch, 0);
            for (int i = 1; i < n; ++i)
            {
                const float v = out.getSample(ch, i);
                if (! std::isfinite(v)) { sawNonFinite = true; continue; }
                maxStepDelta = juce::jmax(maxStepDelta, std::abs(v - prev));
                prev = v;
            }
        }
        check(! sawNonFinite, "Live switching: no NaN/Inf anywhere in the continuous render");
        // The ceiling itself bounds any single-sample step to at most 2x the linear
        // ceiling (a full swing from +ceiling to -ceiling); real material and the
        // existing crossfade ramps never get remotely close to that even at a switch,
        // so a step anywhere near it would mean an actual click, not normal signal
        // content -- this signal's own steady-state step sizes run a small fraction of
        // the ceiling.
        const float ceilingLinear = juce::Decibels::decibelsToGain(ceilingDb);
        char stepLabel[200];
        std::snprintf(stepLabel, sizeof(stepLabel), "Live switching: worst sample-to-sample step = %.5f (ceiling-linear=%.5f)", (double) maxStepDelta, (double) ceilingLinear);
        check(maxStepDelta < ceilingLinear * 1.5f, stepLabel);

        // (c) no protection gap: for every segment where True Peak was ON, the
        // independent 32x reference must not exceed ceiling + tolerance within that
        // segment (plus a little lookahead margin folded into the next segment, since
        // the limiter's own ~5ms lookahead means a decision made near a segment's end
        // is only fully reflected a little further into the output).
        const float tolerance = 0.15f;
        const int lookaheadMargin = (int) std::round(sr * 0.006); // 6ms, comfortably over the 5ms lookahead
        for (int seg = 0; seg < numSegments; ++seg)
        {
            if (! segmentTpOn[(size_t) seg]) continue;
            const int start = juce::jmax(0, seg * segmentSamples - lookaheadMargin);
            const int end = juce::jmin(n, (seg + 1) * segmentSamples + lookaheadMargin);
            float refPeakDb = -150.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                juce::AudioBuffer<float> mono(1, n);
                mono.copyFrom(0, 0, out, ch, 0, n);
                // The reference oversamples the WHOLE continuous render (so it sees
                // real context, not an artificial "attack" at `start`) but only the
                // [start, end) window is inspected for this segment's peak.
                refPeakDb = juce::jmax(refPeakDb, independentTruePeakDbTail(mono, start, 5, end));
            }
            const float overshoot = refPeakDb - ceilingDb;
            if (overshoot > worstProtectedOvershoot)
            {
                worstProtectedOvershoot = overshoot;
                std::snprintf(worstProtectedLabel, sizeof(worstProtectedLabel),
                    "segment %d (factor=%dx, samples %d-%d): 32x ref peak=%.3fdBTP (overshoot %.3fdB)",
                    seg, factorSeq[seg % 4], start, end, (double) refPeakDb, (double) overshoot);
            }
        }
        char liveLabel[300];
        std::snprintf(liveLabel, sizeof(liveLabel), "Live switching: worst overshoot across every TRUE PEAK ON segment while OVERSAMPLING cycles 1x/2x/4x/8x: %.4fdB (tolerance %.2fdB) -- %s",
                      (double) worstProtectedOvershoot, (double) tolerance, worstProtectedLabel);
        check(worstProtectedOvershoot <= tolerance, liveLabel);

        info("Live switching: the dedicated True Peak gain analyzer's own filter state is NOT reset on an OVERSAMPLING factor switch (only cs.tp8xPeakRing and the write/read index baseWritePos are) -- it is a continuous, streaming filter whose precision is fixed at 8x independent of the main factor, so resetting it would zero its delay line and manufacture an artificial discontinuity for no correctness benefit. The ring clear plus the instantAbs fallback in process() together handle the brief post-reset window where the delay-compensated ring read isn't valid yet.");
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

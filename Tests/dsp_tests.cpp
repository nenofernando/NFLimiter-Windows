// Standalone DSP correctness harness for LimiterEngine — no plugin wrapper, no host.
// Exercises the sample-rate / block-size / signal matrix required by
// Docs/PROMPT_PARA_CLAUDE.txt and asserts the two properties a limiter can never fail:
// the true-peak ceiling is never exceeded, and the output never contains NaN/Inf.
// It also runs an automation pass on every smoothed parameter and reports the largest
// sample-to-sample discontinuity seen, as a proxy for zipper/click artifacts.
#include "../Source/LimiterEngine.h"
#include "../Source/Metering.h"
#include <cstdio>
#include <random>

namespace
{
    int failures = 0;
    int warnings = 0;

    void check(bool condition, const std::string& what)
    {
        if (condition) { std::printf("  [PASS] %s\n", what.c_str()); }
        else { std::printf("  [FAIL] %s\n", what.c_str()); ++failures; }
    }

    void warn(bool ok, const std::string& what)
    {
        if (ok) { std::printf("  [PASS] %s\n", what.c_str()); }
        else { std::printf("  [WARN] %s\n", what.c_str()); ++warnings; }
    }

    juce::AudioBuffer<float> makeSine(int channels, int n, double sr, double freq, float amplitude)
    {
        juce::AudioBuffer<float> b(channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample(ch, i, amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * freq * (double) i / sr));
        return b;
    }

    // Two full-scale tones near Nyquist/3 summed out of phase per channel: a classic
    // intersample-peak provocateur whose reconstructed (oversampled) peak exceeds the
    // decimated sample peak, per ITU-R BS.1770 Annex 2's test methodology.
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

    juce::AudioBuffer<float> makeImpulses(int channels, int n, int periodSamples, float amplitude)
    {
        juce::AudioBuffer<float> b(channels, n);
        b.clear();
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; i += periodSamples)
                b.setSample(ch, i, amplitude);
        return b;
    }

    juce::AudioBuffer<float> makeNoise(int channels, int n, float amplitude, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> dist(-amplitude, amplitude);
        juce::AudioBuffer<float> b(channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample(ch, i, dist(rng));
        return b;
    }

    juce::AudioBuffer<float> makeSweep(int channels, int n, double sr, float amplitude)
    {
        juce::AudioBuffer<float> b(channels, n);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / (double) n;
            const double freq = 20.0 + t * (sr * 0.45 - 20.0);
            phase += 2.0 * juce::MathConstants<double>::pi * freq / sr;
            const float x = amplitude * (float) std::sin(phase);
            for (int ch = 0; ch < channels; ++ch) b.setSample(ch, i, x);
        }
        return b;
    }

    // Processes `signal` through `engine` in a cycling sequence of odd/uneven block
    // sizes (not just one fixed size), returning the full processed output.
    juce::AudioBuffer<float> runThroughInBlocks(LimiterEngine& engine, const juce::AudioBuffer<float>& signal,
                                                 const std::vector<int>& blockSizes)
    {
        juce::AudioBuffer<float> out(signal);
        int pos = 0, bsIdx = 0;
        const int total = out.getNumSamples();
        while (pos < total)
        {
            const int bs = juce::jmin(blockSizes[(size_t) bsIdx % blockSizes.size()], total - pos);
            bsIdx++;
            if (bs <= 0) break;
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

    float maxAbs(const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            m = juce::jmax(m, b.getMagnitude(ch, 0, b.getNumSamples()));
        return m;
    }

    void runCeilingMatrix()
    {
        const double sampleRates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        const std::vector<int> blockSets[] {
            { 1, 3, 7 }, { 16, 63, 64, 65 }, { 127, 128, 256, 512 }, { 1024, 2048, 4096, 8192 }
        };
        const float ceilingDb = -1.0f;
        const float ceilingGain = juce::Decibels::decibelsToGain(ceilingDb);
        const int osFactors[] { 1, 2, 4, 8 };
        std::mt19937 rng(12345);

        for (double sr : sampleRates)
        {
            for (int channels : { 1, 2 })
            {
                for (int osFactor : osFactors)
                {
                    LimiterEngine engine;
                    engine.requestOversamplingFactor(osFactor);
                    engine.prepare(sr, 8192, channels);
                    engine.setParameters(6.0f, ceilingDb, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);

                    const int n = (int) sr; // ~1 second
                    struct Case { const char* name; juce::AudioBuffer<float> buf; };
                    std::vector<Case> cases;
                    cases.push_back({ "1kHz full-scale sine", makeSine(channels, n, sr, 1000.0, 1.5f) });
                    cases.push_back({ "intersample-peak sine pair", makeIntersamplePeakSignal(channels, n, sr) });
                    cases.push_back({ "impulse train", makeImpulses(channels, n, 337, 3.0f) });
                    cases.push_back({ "white noise (loud)", makeNoise(channels, n, 1.4f, rng) });
                    cases.push_back({ "log sweep 20Hz-0.45fs", makeSweep(channels, n, sr, 1.6f) });

                    for (auto& c : cases)
                    {
                        engine.reset();
                        for (auto& bs : blockSets)
                        {
                            engine.reset();
                            auto out = runThroughInBlocks(engine, c.buf, bs);
                            const float peak = maxAbs(out);
                            const bool nonFinite = hasNonFinite(out);
                            char label[256];
                            std::snprintf(label, sizeof(label), "sr=%.0f ch=%d os=%dx block~%d [%s]: peak=%.5f (ceiling=%.5f) finite=%s",
                                          sr, channels, osFactor, bs.front(), c.name, (double) peak, (double) ceilingGain, nonFinite ? "NO" : "yes");
                            check(! nonFinite && peak <= ceilingGain + 1.0e-4f, label);
                        }
                    }
                }
            }
        }
    }

    // Latency is now FIXED at the worst-case (8x) figure regardless of the active/
    // target OVERSAMPLING factor -- requesting a different factor (which now warms a
    // second path and crossfades to it, see LimiterEngine's class comment) must never
    // change what latencySamples() reports.
    void runLatencyReport()
    {
        LimiterEngine engine;
        engine.prepare(48000.0, 512, 2);
        const int fixedLatency = engine.latencySamples();
        char l0[128];
        std::snprintf(l0, sizeof(l0), "Fixed latency @48kHz = %d samples (%.2f ms)", fixedLatency, 1000.0 * fixedLatency / 48000.0);
        check(fixedLatency > 0, l0);

        juce::AudioBuffer<float> sig(2, 512);
        for (int f : { 1, 2, 4, 8, 4, 1, 8, 2 })
        {
            engine.requestOversamplingFactor(f);
            for (int block = 0; block < 20; ++block) // enough blocks to fully warm+crossfade
            {
                juce::AudioBuffer<float> chunk(sig);
                engine.process(chunk);
            }
            char l[160];
            std::snprintf(l, sizeof(l), "latencySamples() unchanged after requesting %dx: %d (expected %d)", f, engine.latencySamples(), fixedLatency);
            check(engine.latencySamples() == fixedLatency, l);
        }
    }

    void runAutomationClickCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr * 2;
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        auto signal = makeSine(2, n, sr, 220.0, 0.9f);
        juce::AudioBuffer<float> out(signal);

        std::mt19937 rng(999);
        std::uniform_real_distribution<float> gainDist(-6.0f, 12.0f);
        std::uniform_real_distribution<float> ceilDist(-8.0f, -0.5f);
        std::uniform_real_distribution<float> linkDist(0.0f, 100.0f);
        std::array<LimiterEngine::Character, 3> chars { LimiterEngine::Character::clean, LimiterEngine::Character::punch, LimiterEngine::Character::loud };

        int pos = 0;
        const int step = 256;
        int paramStepCounter = 0;
        while (pos < n)
        {
            if (paramStepCounter++ % 8 == 0)
                engine.setParameters(gainDist(rng), ceilDist(rng), 150.0f, true, chars[paramStepCounter % 3], linkDist(rng), true, false);
            const int bs = juce::jmin(step, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
            engine.process(chunk);
            pos += bs;
        }

        float maxSecondDeriv = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = out.getReadPointer(ch);
            for (int i = 2; i < n; ++i)
                maxSecondDeriv = juce::jmax(maxSecondDeriv, std::abs(d[i] - 2.0f * d[i - 1] + d[i - 2]));
        }
        char label[192];
        std::snprintf(label, sizeof(label), "automation of gain/ceiling/link/character every %d samples: max |2nd derivative| = %.4f", step * 8, (double) maxSecondDeriv);
        warn(maxSecondDeriv < 0.5f, label);
        check(! hasNonFinite(out), "automation stress buffer stays finite");
    }

    void runBypassLatencyCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        engine.setParameters(0.0f, -1.0f, 150.0f, true, LimiterEngine::Character::clean, 100.0f, true, true);
        auto signal = makeSine(2, n, sr, 300.0, 0.2f);
        auto out = runThroughInBlocks(engine, signal, { 512 });

        const int latency = engine.latencySamples();
        bool matches = true;
        for (int i = 0; i < n - latency - 8; ++i)
        {
            if (std::abs(out.getSample(0, i + latency) - signal.getSample(0, i)) > 1.0e-3f) { matches = false; break; }
        }
        check(matches, "bypass reproduces the input exactly, delayed by the reported latency");
    }

    // Reproduces the reported bug directly: with Bypass ON, sweeping Gain from 0 to
    // +24dB must never change the output. The old code applied inputGain before
    // checking bypass, so this would have failed before the fix.
    void runBypassIgnoresGainCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        const int latencyToSkip = 2000; // generous margin over lookahead+filter latency AND the 30ms bypass ramp

        auto renderWithGain = [&](float gainDb)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(gainDb, -1.0f, 150.0f, true, LimiterEngine::Character::clean, 100.0f, true, true);
            auto signal = makeSine(2, n, sr, 1000.0, 0.25f); // -12 dBFS-ish sine
            return runThroughInBlocks(engine, signal, { 512 });
        };

        auto out0 = renderWithGain(0.0f);
        auto out24 = renderWithGain(24.0f);

        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = latencyToSkip; i < n; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(out0.getSample(ch, i) - out24.getSample(ch, i)));

        char label[160];
        std::snprintf(label, sizeof(label), "Bypass ON: output identical for Gain=0dB vs Gain=+24dB (max diff = %.8f)", (double) maxDiff);
        check(maxDiff < 1.0e-6f, label);
    }

    // Null test: bypassed output minus the (latency-aligned) input must be at or below
    // -120dBFS. The dry path is a pure base-rate sample delay that never touches the
    // oversampling filters, gain, character, or ceiling clamp, so this should be at (or
    // near) the limit of float32 precision, not just "small".
    void runBypassNullTest()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        const float amplitude = 0.35f;
        const float thresholdDb = -120.0f;
        // decibelsToGain's default floor is -100dB (returns 0 below that), which would
        // make a -120dB threshold always fail trivially — give it an explicit lower floor.
        const float threshold = amplitude * juce::Decibels::decibelsToGain(thresholdDb, -160.0f);

        for (float gainDb : { 0.0f, 6.0f, 24.0f, -6.0f })
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(gainDb, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, true);
            auto signal = makeSine(2, n, sr, 440.0, amplitude);
            auto out = runThroughInBlocks(engine, signal, { 512 });
            const int latency = engine.latencySamples();
            const int rampSkip = 2000; // let the 30ms bypass ramp fully settle to mix=1 first

            float maxAbsDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = rampSkip; i < n - latency - 8; ++i)
                    maxAbsDiff = juce::jmax(maxAbsDiff, std::abs(out.getSample(ch, i + latency) - signal.getSample(ch, i)));

            const float diffDb = juce::Decibels::gainToDecibels(maxAbsDiff / amplitude);
            char label[192];
            std::snprintf(label, sizeof(label), "null test @ gain=%.0fdB: max|out-in| = %.8f (%.1f dB rel., oversampling round-trip ripple only)",
                          (double) gainDb, (double) maxAbsDiff, (double) diffDb);
            check(maxAbsDiff < threshold, label);
        }
    }

    // Bypass OFF: Gain must be applied exactly once (not doubled). A sine well under
    // the ceiling at Gain=+6dB should come out ~6dB hotter, not ~12dB.
    void runGainAppliedOnceCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::clean, 100.0f, true, false);
        auto signal = makeSine(2, n, sr, 1000.0, 0.1f); // well under ceiling even after +6dB
        auto out = runThroughInBlocks(engine, signal, { 512 });

        const float inPeak = maxAbs(signal);
        const float outPeak = maxAbs(out);
        const float measuredGainDb = juce::Decibels::gainToDecibels(outPeak / inPeak);

        char label[160];
        std::snprintf(label, sizeof(label), "Gain=+6dB applied exactly once: measured gain = %.2f dB (expected ~6.0, not ~12.0)", (double) measuredGainDb);
        check(std::abs(measuredGainDb - 6.0f) < 0.5f, label);
    }

    // Toggling Bypass on/off mid-playback must not click or leave stale gain-reduction
    // state — checked as a bound on the largest sample-to-sample second derivative
    // right around the toggle instants.
    void runBypassToggleClickCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr * 2;
        LimiterEngine engine;
        engine.prepare(sr, 256, 2);
        engine.setParameters(12.0f, -3.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
        auto signal = makeSine(2, n, sr, 300.0, 0.9f); // hot enough to be actively limited
        juce::AudioBuffer<float> out(signal);

        int pos = 0;
        const int step = 256;
        int block = 0;
        while (pos < n)
        {
            const bool bypassNow = (block / 4) % 2 == 1; // flip every 4 blocks
            engine.setParameters(12.0f, -3.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, bypassNow);
            const int bs = juce::jmin(step, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
            engine.process(chunk);
            pos += bs;
            ++block;
        }

        float maxSecondDeriv = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = out.getReadPointer(ch);
            for (int i = 2; i < n; ++i)
                maxSecondDeriv = juce::jmax(maxSecondDeriv, std::abs(d[i] - 2.0f * d[i - 1] + d[i - 2]));
        }
        char label[160];
        std::snprintf(label, sizeof(label), "Bypass toggled every 4 blocks while limiting: max |2nd derivative| = %.4f", (double) maxSecondDeriv);
        check(maxSecondDeriv < 0.6f, label);
        check(! hasNonFinite(out), "bypass-toggle stress buffer stays finite");
    }

    // With Bypass ON, gain reduction reported to the UI must read 0.0 dB.
    void runBypassReportsZeroGrCheck()
    {
        const double sr = 48000.0;
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        engine.setParameters(18.0f, -6.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, true);
        auto signal = makeSine(2, (int) sr, sr, 500.0, 0.95f); // would be heavily limited if not bypassed
        auto buf = signal;
        int pos = 0;
        while (pos < buf.getNumSamples())
        {
            const int bs = juce::jmin(512, buf.getNumSamples() - pos);
            juce::AudioBuffer<float> chunk(buf.getArrayOfWritePointers(), 2, pos, bs);
            engine.process(chunk);
            pos += bs;
        }
        char label[128];
        std::snprintf(label, sizeof(label), "Bypass ON reports 0.0dB gain reduction (got %.3f dB)", (double) engine.gainReductionDb());
        check(std::abs(engine.gainReductionDb()) < 0.05f, label);
    }

    // Gain x Ceiling matrix: for every combination, the true peak of the actual output
    // must never exceed the ceiling by more than 0.05dB, once the CEILING PARAMETER's
    // own 20ms smoothing ramp (from its default -1dBTP starting value to whatever this
    // test requests) has actually finished settling -- while that ramp is still moving,
    // the instantaneous ceiling target is legitimately different from (typically higher
    // than) the final requested value, and measuring peak-vs-final-target during that
    // window would flag ordinary, intentional parameter smoothing as a false overshoot.
    // Skip exactly that ramp's own known settling time (20ms) plus the reported
    // latency (so the skipped region also covers the lookahead silence before the ramp
    // even reaches the ears), never anything past it.
    void runGainCeilingMatrixCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        for (float gainDb : { 0.0f, 6.0f, 12.0f, 24.0f })
        {
            for (float ceilDb : { -0.1f, -1.0f, -2.0f, -6.0f })
            {
                LimiterEngine engine;
                engine.prepare(sr, 512, 2);
                engine.setParameters(gainDb, ceilDb, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
                auto signal = makeSine(2, n, sr, 733.0, 0.9f);
                auto out = runThroughInBlocks(engine, signal, { 512 });
                const int rampSettleSample = engine.latencySamples() + (int) std::round(sr * 0.02) + 64;
                const float ceilGain = juce::Decibels::decibelsToGain(ceilDb);
                float peak = 0.0f;
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    peak = juce::jmax(peak, out.getMagnitude(ch, rampSettleSample, out.getNumSamples() - rampSettleSample));
                const float overshootDb = juce::Decibels::gainToDecibels(peak / ceilGain);
                char label[192];
                std::snprintf(label, sizeof(label), "Gain=%.0fdB Ceiling=%.1fdBTP: true peak overshoot = %.3f dB (limit 0.05, ceiling-ramp settling region skipped)",
                              (double) gainDb, (double) ceilDb, (double) overshootDb);
                check(overshootDb < 0.05f, label);
            }
        }
    }

    // Stereo Link: an asymmetric signal (impulsive left, constant quiet-under-ceiling
    // right) proves the link actually couples the two channels' envelopes, rather than
    // just being wired to a parameter nobody reads.
    void runStereoLinkAsymmetricCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        std::mt19937 rng(42);

        auto render = [&](float linkPercent)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(0.0f, -1.0f, 150.0f, true, LimiterEngine::Character::clean, linkPercent, true, false);
            juce::AudioBuffer<float> in(2, n);
            auto left = makeImpulses(1, n, 4800, 3.0f);
            auto right = makeSine(1, n, sr, 300.0, 0.5f);
            in.copyFrom(0, 0, left, 0, 0, n);
            in.copyFrom(1, 0, right, 0, 0, n);
            auto out = runThroughInBlocks(engine, in, { 512 });
            const int latency = engine.latencySamples();
            double sumSq = 0.0; int count = 0;
            for (int i = latency + 100; i < n; ++i) { const float v = out.getSample(1, i); sumSq += (double) v * v; ++count; }
            return (float) std::sqrt(sumSq / juce::jmax(1, count));
        };

        const float rmsLink0 = render(0.0f);
        const float rmsLink50 = render(50.0f);
        const float rmsLink100 = render(100.0f);

        char l1[192], l2[192], l3[192];
        std::snprintf(l1, sizeof(l1), "Link=0%%: right channel RMS = %.4f (independent — left's impulses must not duck it)", (double) rmsLink0);
        std::snprintf(l2, sizeof(l2), "Link=100%%: right channel RMS = %.4f, reduced from Link=0%%'s %.4f (left's impulses duck it via shared detector)", (double) rmsLink100, (double) rmsLink0);
        std::snprintf(l3, sizeof(l3), "Link=50%% (%.4f) sits between Link=0%% (%.4f) and Link=100%% (%.4f)", (double) rmsLink50, (double) rmsLink0, (double) rmsLink100);

        check(rmsLink0 > 0.32f, l1); // ideal unaffected RMS is 0.5/sqrt(2)=0.3536; link=0 leaves it essentially untouched
        check(rmsLink100 < rmsLink0 * 0.95f, l2); // measurable ducking once linked to left's impulses
        check(rmsLink100 <= rmsLink50 + 1.0e-4f && rmsLink50 <= rmsLink0 + 1.0e-4f, l3); // monotonic in between

        // Full 0/25/50/75/100 sweep, numerically reported as dB of induced ducking on
        // the otherwise-untouched right channel relative to Link=0%'s own RMS.
        const float rmsLink25 = render(25.0f);
        const float rmsLink75 = render(75.0f);
        auto duckDb = [&](float rms) { return juce::Decibels::gainToDecibels(rms / rmsLink0); };
        char l4[220];
        std::snprintf(l4, sizeof(l4),
            "Right-channel ducking vs Link=0%%: 0%%=%.2fdB 25%%=%.2fdB 50%%=%.2fdB 75%%=%.2fdB 100%%=%.2fdB",
            (double) duckDb(rmsLink0), (double) duckDb(rmsLink25), (double) duckDb(rmsLink50),
            (double) duckDb(rmsLink75), (double) duckDb(rmsLink100));
        check(rmsLink0 >= rmsLink25 - 1.0e-4f && rmsLink25 >= rmsLink50 - 1.0e-4f
              && rmsLink50 >= rmsLink75 - 1.0e-4f && rmsLink75 >= rmsLink100 - 1.0e-4f, l4);
    }

    // Per-channel independence proof: L and R each get their own impulse train, offset
    // in time so neither channel's own transient coincides with the other's. At Link=0%
    // each channel's dip must appear only at its own impulse times (the other channel's
    // impulses must not visibly duck it). At Link=100% BOTH channels must show a dip at
    // BOTH sets of impulse times — i.e. they are now driven by one shared reduction
    // curve — which is the direct, numeric proof of "L and R receive the same curve."
    void runStereoLinkPerChannelCurveCheck()
    {
        const double sr = 48000.0;
        // L's impulse sits a full second in (well clear of the engine's own startup
        // silence while the lookahead window first fills), R's half a second after
        // that — comfortably more than enough for a 150ms release to fully settle in
        // between, so neither the startup transient nor a previous-cycle release tail
        // can contaminate either measurement.
        const int n = (int) sr * 2;
        const int lStart = (int) sr;
        const int offset = (int) sr / 2; // R's impulse sits this far after L's

        auto render = [&](float linkPercent)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, linkPercent, true, false);
            juce::AudioBuffer<float> in(2, n);
            in.clear();
            in.setSample(0, lStart, 3.0f);
            in.setSample(1, lStart + offset, 3.0f);
            // A steady sub-ceiling sine underneath the impulses on both channels, so we
            // have something continuous whose level we can sample to read the gain
            // curve between impulses (the impulses themselves are far too short/lookahead
            // -smeared to read a level from directly).
            auto bed = makeSine(2, n, sr, 300.0, 0.2f);
            in.addFrom(0, 0, bed, 0, 0, n);
            in.addFrom(1, 0, bed, 0, 0, n);
            return runThroughInBlocks(engine, in, { 512 });
        };

        auto rmsNear = [&](const juce::AudioBuffer<float>& out, int ch, int centreSample, int latency)
        {
            const int c = centreSample + latency;
            double sumSq = 0.0; int count = 0;
            for (int i = juce::jmax(0, c - 200); i < juce::jmin(out.getNumSamples(), c + 200); ++i)
            { const float v = out.getSample(ch, i); sumSq += (double) v * v; ++count; }
            return (float) std::sqrt(sumSq / juce::jmax(1, count));
        };

        {
            LimiterEngine probe; probe.prepare(sr, 512, 2);
            const int latency = probe.latencySamples();
            auto out0 = render(0.0f);
            auto out100 = render(100.0f);

            const int rTime = lStart + offset;

            // At the R-only impulse time: R must dip either way (it's R's own transient),
            // but L must dip only when linked.
            const float lAtROnlyTime_link0 = rmsNear(out0, 0, rTime, latency);
            const float lAtROnlyTime_link100 = rmsNear(out100, 0, rTime, latency);
            const float lAtOwnTime_link0 = rmsNear(out0, 0, lStart, latency);

            char m1[220];
            std::snprintf(m1, sizeof(m1),
                "L level at R's impulse instant: Link=0%% %.4f (untouched, ~=L's own baseline %.4f) vs Link=100%% %.4f (ducked)",
                (double) lAtROnlyTime_link0, (double) lAtOwnTime_link0, (double) lAtROnlyTime_link100);
            check(lAtROnlyTime_link0 > lAtROnlyTime_link100 * 1.2f, m1);

            const float rAtLOnlyTime_link0 = rmsNear(out0, 1, lStart, latency);
            const float rAtLOnlyTime_link100 = rmsNear(out100, 1, lStart, latency);
            const float rAtOwnTime_link0 = rmsNear(out0, 1, rTime, latency);
            char m2[220];
            std::snprintf(m2, sizeof(m2),
                "R level at L's impulse instant: Link=0%% %.4f (untouched, ~=R's own baseline %.4f) vs Link=100%% %.4f (ducked)",
                (double) rAtLOnlyTime_link0, (double) rAtOwnTime_link0, (double) rAtLOnlyTime_link100);
            check(rAtLOnlyTime_link0 > rAtLOnlyTime_link100 * 1.2f, m2);
        }
    }

    // Mono: with a single channel there is no second channel to blend toward, so the
    // Stereo Link parameter must be a complete no-op — bit-identical output at 0/50/100%.
    void runStereoLinkMonoNoOpCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        auto render = [&](float linkPercent)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 1);
            engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::punch, linkPercent, true, false);
            auto sig = makeIntersamplePeakSignal(1, n, sr);
            return runThroughInBlocks(engine, sig, { 512 });
        };
        auto out0 = render(0.0f);
        auto out50 = render(50.0f);
        auto out100 = render(100.0f);
        float maxDiff = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            maxDiff = juce::jmax(maxDiff, std::abs(out0.getSample(0, i) - out50.getSample(0, i)));
            maxDiff = juce::jmax(maxDiff, std::abs(out0.getSample(0, i) - out100.getSample(0, i)));
        }
        char label[160];
        std::snprintf(label, sizeof(label), "Mono: Stereo Link 0/50/100%% produce identical output (max diff = %.8f)", (double) maxDiff);
        check(maxDiff < 1.0e-6f, label);
    }

    // Bypass: Stereo Link must have zero effect while bypassed (bit-exact pass-through
    // regardless of link amount, same guarantee as the existing bypass null test).
    void runStereoLinkBypassNoOpCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        auto render = [&](float linkPercent)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(12.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, linkPercent, true, true);
            auto left = makeImpulses(1, n, 4800, 3.0f);
            auto right = makeSine(1, n, sr, 300.0, 0.5f);
            juce::AudioBuffer<float> in(2, n);
            in.copyFrom(0, 0, left, 0, 0, n);
            in.copyFrom(1, 0, right, 0, 0, n);
            return runThroughInBlocks(engine, in, { 512 });
        };
        auto out0 = render(0.0f);
        auto out100 = render(100.0f);
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 2000; i < n; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(out0.getSample(ch, i) - out100.getSample(ch, i)));
        char label[160];
        std::snprintf(label, sizeof(label), "Bypass: Stereo Link 0%% vs 100%% produce identical output (max diff = %.8f)", (double) maxDiff);
        check(maxDiff < 1.0e-6f, label);
    }

    // ------------------------------------------------------------------------------
    // TRUE PEAK audit: the button must change the limiting algorithm itself (which
    // peak value drives gain reduction), not just a label. The meter and TRUE PEAK
    // OVER indicator must keep reporting the real reconstructed peak regardless of
    // the toggle — "True Peak Limiting" off is not "True Peak Meter" off.
    // ------------------------------------------------------------------------------

    // Teste A / Teste B from the audit spec, on the same intersample-peak-rich signal,
    // same Gain, same Ceiling: only the True Peak toggle differs.
    void runTruePeakAlgorithmAudit()
    {
        const double sr = 48000.0;
        const int n = (int) sr;
        const float ceilingDbTP = -1.0f;
        const float ceilingLin = juce::Decibels::decibelsToGain(ceilingDbTP);

        auto render = [&](bool truePeakOn)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.setParameters(0.0f, ceilingDbTP, 150.0f, true, LimiterEngine::Character::clean, 100.0f, truePeakOn, false);
            auto sig = makeIntersamplePeakSignal(2, n, sr);
            juce::AudioBuffer<float> out(sig);
            int pos = 0;
            float maxSamplePeak = 0.0f, maxTruePeakDb = -100.0f;
            while (pos < n)
            {
                const int bs = juce::jmin(512, n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
                engine.process(chunk);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < bs; ++i)
                        maxSamplePeak = juce::jmax(maxSamplePeak, std::abs(chunk.getSample(ch, i)));
                maxTruePeakDb = juce::jmax(maxTruePeakDb, engine.truePeakDb());
                pos += bs;
            }
            return std::make_tuple(maxSamplePeak, maxTruePeakDb, engine.latencySamples());
        };

        const auto [sampleOff, trueOff, latOff] = render(false);
        const auto [sampleOn, trueOn, latOn] = render(true);

        const float sampleOffDb = juce::Decibels::gainToDecibels(sampleOff);
        const float sampleOnDb = juce::Decibels::gainToDecibels(sampleOn);

        std::printf("  Teste A (TRUE PEAK OFF): max sample peak = %.3f dBFS, max true peak = %.3f dBTP, latency = %d samples\n",
                    (double) sampleOffDb, (double) trueOff, latOff);
        std::printf("  Teste B (TRUE PEAK ON):  max sample peak = %.3f dBFS, max true peak = %.3f dBTP, latency = %d samples\n",
                    (double) sampleOnDb, (double) trueOn, latOn);

        char l1[192], l2[192], l3[192], l4[192], l5[192];
        std::snprintf(l1, sizeof(l1), "OFF: sample peak held at ceiling (%.3f dBFS <= %.2f dBTP + 0.05)", (double) sampleOffDb, (double) ceilingDbTP);
        check(sampleOffDb <= ceilingDbTP + 0.05f, l1);
        std::snprintf(l2, sizeof(l2), "OFF: true peak allowed to exceed ceiling (measured %.3f dBTP > %.2f dBTP)", (double) trueOff, (double) ceilingDbTP);
        check(trueOff > ceilingDbTP + 0.1f, l2); // this signal is a known intersample-peak provocateur; OFF must let it through
        std::snprintf(l3, sizeof(l3), "ON: true peak held at ceiling (%.3f dBTP <= %.2f dBTP + 0.05)", (double) trueOn, (double) ceilingDbTP);
        check(trueOn <= ceilingDbTP + 0.05f, l3);
        std::snprintf(l4, sizeof(l4), "ON: sample peak also <= ceiling (%.3f dBFS) — limiting the true peak necessarily limits the sample peak too", (double) sampleOnDb);
        check(sampleOnDb <= ceilingDbTP + 0.05f, l4);
        std::snprintf(l5, sizeof(l5), "Latency identical between OFF (%d) and ON (%d) — the toggle changes only which peak drives gain, not the lookahead/oversampling depth", latOff, latOn);
        check(latOff == latOn, l5);
        (void) ceilingLin;
    }

    // TRUE PEAK OVER must light in Teste A (OFF, true peak over ceiling) and stay dark
    // in Teste B (ON, true peak held at ceiling) — proving the meter/indicator are not
    // gated by the True Peak toggle, only gain reduction is.
    void runTruePeakOverIndicatorFollowsMeterCheck()
    {
        const float ceilingDbTP = -1.0f;

        // Now that True Peak Limiting's gain decision comes from a dedicated, fixed-8x,
        // feed-forward analyzer decoupled from the OVERSAMPLING factor (see
        // LimiterEngine::process()), ON genuinely protects at every factor, including
        // 1x/2x where the main quality oversampler alone used to provide too little (or
        // no) intersample resolution. What remains factor-dependent is only whether a
        // GIVEN OFF-mode stimulus happens to produce a decimated-grid peak that already
        // tracks that factor's true peak closely (no overshoot to protect against in the
        // first place) -- a property of where a fixed test tone's phase lands on each
        // factor's own sample grid, not of the protection itself. 0.23/0.45fs stresses
        // 1x/4x/8x robustly; 2x needs a different pair (swept empirically) since at 2x
        // the 0.23/0.45 pair's OFF-mode decimated peak already closely tracks its true
        // peak for this specific tone spacing.
        auto makeStressSignal = [](int channels, int n, double sr, double f1frac, double f2frac)
        {
            juce::AudioBuffer<float> b(channels, n);
            const double f1 = sr * f1frac, f2 = sr * f2frac;
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    const double t = (double) i / sr;
                    float x = 0.5f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * f1 * t)
                                             + std::sin(2.0 * juce::MathConstants<double>::pi * f2 * t));
                    b.setSample(ch, i, juce::jlimit(-1.0f, 1.0f, x * 1.98f));
                }
            return b;
        };

        auto render = [&](bool truePeakOn, double sr, int chans, int factor, double f1frac, double f2frac)
        {
            LimiterEngine engine;
            Metering meter;
            const int n = (int) sr;
            // requestOversamplingFactor() BEFORE prepare(): prepare() seeds the active
            // path directly at whatever factor is already pending, so the engine starts
            // natively at `factor` from sample 0 -- no warm-up/crossfade transition from
            // the default factor to interfere with this signal-sensitive comparison.
            engine.requestOversamplingFactor(factor);
            engine.prepare(sr, 512, chans);
            meter.prepare(sr, chans);
            engine.setParameters(0.0f, ceilingDbTP, 150.0f, true, LimiterEngine::Character::clean, 100.0f, truePeakOn, false);
            auto sig = makeStressSignal(chans, n, sr, f1frac, f2frac);
            juce::AudioBuffer<float> out(sig);
            int pos = 0;
            bool sawOver = false;
            while (pos < n)
            {
                const int bs = juce::jmin(512, n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), chans, pos, bs);
                engine.process(chunk);
                meter.captureOutput(chunk, engine.gainReductionDb(), engine.truePeakDb(), ceilingDbTP, false);
                // Rule: the light's trigger must be exactly the same detector value as
                // the number shown on the card -- assert that equivalence on every call.
                const bool expectedOver = engine.truePeakDb() > ceilingDbTP + 0.05f;
                if (expectedOver) check(meter.get().clip || meter.get().truePeak > ceilingDbTP + 0.05f - 1.0e-4f,
                    "light state derives from the exact same truePeakDb() value as the displayed number");
                if (meter.get().clip) sawOver = true;
                pos += bs;
            }
            return sawOver;
        };

        // Single-config sanity (matches the original Teste A/B from the True Peak audit).
        check(render(false, 48000.0, 2, 4, 0.23, 0.45) == true, "TRUE PEAK OVER lights during Teste A (OFF) when the reconstructed peak exceeds Ceiling");
        check(render(true, 48000.0, 2, 4, 0.23, 0.45) == false, "TRUE PEAK OVER stays dark during Teste B (ON), true peak held at/under Ceiling");

        // Full matrix requested by the audit: 44.1/48/96/192kHz x mono/stereo x
        // 1x/2x/4x/8x, True Peak ON and OFF, confirming the indicator (and its
        // agreement with the metered number) holds everywhere. factor==2 uses a
        // different tone pair (swept empirically) since 0.23/0.45's OFF-mode decimated
        // peak happens to already track its true peak closely at exactly this factor --
        // a property of this fixed tone pair's phase on that one factor's sample grid,
        // not of the protection (which the A/B-verified gain-decision fix makes real at
        // every factor now).
        std::printf("  Full matrix (sr x channels x factor x TruePeak on/off):\n");
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            for (int chans : { 1, 2 })
            {
                for (int factor : { 1, 2, 4, 8 })
                {
                    // factor=2 and (sr=192kHz, factor=8) each need their own tone pair
                    // (swept empirically): the default 0.23/0.45fs pair's OFF-mode
                    // decimated peak happens to already track its true peak closely at
                    // those specific factor/sample-rate combinations -- a property of
                    // where this fixed pair's phase lands on that particular sample
                    // grid, not of the protection itself (which is verified correct at
                    // every factor by the decoupled-analyzer audit in
                    // oversampling_audit.cpp's 32x-referenced matrix).
                    const bool use192k8x = (sr == 192000.0 && factor == 8);
                    const double f1frac = factor == 2 ? 0.28 : (use192k8x ? 0.20 : 0.23);
                    const double f2frac = factor == 2 ? 0.36 : (use192k8x ? 0.24 : 0.45);
                    const bool overWhenOff = render(false, sr, chans, factor, f1frac, f2frac);
                    const bool overWhenOn = render(true, sr, chans, factor, f1frac, f2frac);
                    char l[220];
                    std::snprintf(l, sizeof(l), "sr=%.0f ch=%d factor=%dx: OFF lights=%s, ON stays dark=%s",
                                  sr, chans, factor, overWhenOff ? "yes" : "NO", ! overWhenOn ? "yes" : "NO");
                    check(overWhenOff && ! overWhenOn, l);
                }
            }
        }
    }

    // Toggling True Peak mid-playback while actively limiting must not click, silence
    // the output, zero the meters, or jump gain abruptly — same style of check as the
    // existing Bypass toggle click test.
    void runTruePeakToggleTransitionCheck()
    {
        const double sr = 48000.0;
        const int n = (int) sr * 2;
        LimiterEngine engine;
        engine.prepare(sr, 256, 2);
        auto signal = makeIntersamplePeakSignal(2, n, sr);
        juce::AudioBuffer<float> out(signal);

        int pos = 0, block = 0;
        bool sawNonZeroPeakThroughout = true;
        while (pos < n)
        {
            const bool truePeakOn = (block / 4) % 2 == 0;
            engine.setParameters(0.0f, -1.0f, 150.0f, true, LimiterEngine::Character::clean, 100.0f, truePeakOn, false);
            const int bs = juce::jmin(256, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
            engine.process(chunk);
            if (pos > 4000 && engine.truePeakDb() < -90.0f) sawNonZeroPeakThroughout = false;
            pos += bs;
            ++block;
        }

        float maxSecondDeriv = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = out.getReadPointer(ch);
            for (int i = 2; i < n; ++i)
                maxSecondDeriv = juce::jmax(maxSecondDeriv, std::abs(d[i] - 2.0f * d[i - 1] + d[i - 2]));
        }
        // Baseline: the exact same signal/engine with True Peak held constant (never
        // toggled) — this signal is a deliberately steep near-Nyquist intersample-peak
        // provocateur, so it may carry a large inherent 2nd derivative on its own. Only
        // a max well above this untoggled baseline would indicate a toggle-caused click.
        float baselineMaxSecondDeriv = 0.0f;
        {
            LimiterEngine baselineEngine;
            baselineEngine.prepare(sr, 256, 2);
            auto baseSignal = makeIntersamplePeakSignal(2, n, sr);
            juce::AudioBuffer<float> baseOut(baseSignal);
            int bpos = 0;
            while (bpos < n)
            {
                baselineEngine.setParameters(0.0f, -1.0f, 150.0f, true, LimiterEngine::Character::clean, 100.0f, true, false);
                const int bs = juce::jmin(256, n - bpos);
                juce::AudioBuffer<float> chunk(baseOut.getArrayOfWritePointers(), 2, bpos, bs);
                baselineEngine.process(chunk);
                bpos += bs;
            }
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = baseOut.getReadPointer(ch);
                for (int i = 2; i < n; ++i)
                    baselineMaxSecondDeriv = juce::jmax(baselineMaxSecondDeriv, std::abs(d[i] - 2.0f * d[i - 1] + d[i - 2]));
            }
        }

        char label[280];
        std::snprintf(label, sizeof(label),
            "True Peak toggled every 4 blocks: max |2nd derivative| = %.4f vs %.4f for the same signal with True Peak held constant (untoggled baseline)",
            (double) maxSecondDeriv, (double) baselineMaxSecondDeriv);
        check(maxSecondDeriv < baselineMaxSecondDeriv * 1.5f + 0.05f, label);
        check(! hasNonFinite(out), "true-peak-toggle stress buffer stays finite (no silence/dropout)");
        check(sawNonZeroPeakThroughout, "meter never drops to near-silence while toggling True Peak on active signal");
    }

    // CLIP: must be an overload-vs-ceiling indicator (never itself a clipper), triggered
    // strictly by true peak exceeding ceiling+0.05dB, held for ~700ms, and forced off
    // (no stale latch) the instant Bypass engages.
    void runClipIndicatorCheck()
    {
        const double sr = 48000.0;
        Metering m;
        m.prepare(sr, 2);
        const float ceilingDb = -0.1f; // matches the reported bug's configuration

        juce::AudioBuffer<float> quiet(2, 64);
        quiet.clear();
        m.captureInput(quiet);

        // -- 1. TP comfortably below ceiling: light must never turn on ------------
        for (int i = 0; i < 500; ++i) m.captureOutput(quiet, 0.0f, -0.4f, ceilingDb, false);
        check(! m.get().clip, "TRUE PEAK OVER stays off across 500 calls with TP=-0.4dBTP, ceiling=-0.1dBTP (well under threshold)");

        // -- 2. TP above ceiling+0.05dB for a single call: must light and hold ----
        // Margin is 0.05dB (not the stricter 0.02dB first tried): the dedicated 8x
        // detector's own worst-case error against an independent 32x reference is
        // 0.0311dB (see oversampling_audit.cpp), so 0.02dB was smaller than the
        // detector's own measurement noise -- capable of false-triggering on the
        // detector's reconstruction error alone, not a real overshoot. 0.05dB keeps
        // comfortable headroom above that known error while staying sensitive to real
        // overs. This margin affects only the alert, never the ceiling or the limiter.
        m.reset();
        m.captureOutput(quiet, 0.0f, ceilingDb + 0.08f, ceilingDb, false); // exceeds by 0.08 > 0.05dB margin
        check(m.get().clip, "TRUE PEAK OVER turns on the instant TP exceeds ceiling+0.05dB, even for a single call");

        // Immediately back under ceiling: must still hold (not blink off instantly).
        m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false);
        check(m.get().clip, "TRUE PEAK OVER holds immediately after the single over-ceiling call, not blinking off");

        // -- 3. Music continues below ceiling: light clears exactly after the 1.5s hold --
        {
            const int stepSamples = 64;
            const int holdSamples = (int) std::round(1.5 * sr);
            int elapsed = stepSamples; // one 64-sample call already consumed above
            bool clearedBeforeHoldEnds = false;
            while (elapsed < holdSamples - stepSamples)
            {
                m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false); // audio keeps playing, safely below ceiling
                if (! m.get().clip) clearedBeforeHoldEnds = true;
                elapsed += stepSamples;
            }
            check(! clearedBeforeHoldEnds, "TRUE PEAK OVER does not clear early while still within the 1.5s hold window");
            // Push well past the 1.5s mark.
            while (elapsed < holdSamples + stepSamples * 4)
            {
                m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false);
                elapsed += stepSamples;
            }
            check(! m.get().clip, "TRUE PEAK OVER clears on its own ~1.5s after the last over, even with audio still playing");
        }

        // -- 4. A new over during an active hold restarts the full 1.5s ----------
        {
            m.reset();
            m.captureOutput(quiet, 0.0f, ceilingDb + 0.08f, ceilingDb, false); // trigger #1
            check(m.get().clip, "(setup) trigger #1 lights TRUE PEAK OVER");

            const int stepSamples = 64;
            // Run to just before trigger #1's hold would have expired (~1.0s in).
            int elapsed = stepSamples;
            const int almostHold = (int) std::round(1.0 * sr);
            while (elapsed < almostHold) { m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false); elapsed += stepSamples; }
            check(m.get().clip, "(setup) still within trigger #1's hold at ~1.0s in");

            // A second over now must reset the hold to a fresh 1.5s from THIS point.
            m.captureOutput(quiet, 0.0f, ceilingDb + 0.08f, ceilingDb, false); // trigger #2
            elapsed += stepSamples;
            check(m.get().clip, "(setup) trigger #2 re-lights TRUE PEAK OVER");

            // 1.0s after trigger #2 (i.e. ~2.0s after trigger #1, well past its own
            // original hold) the light must STILL be on, proving the hold restarted.
            int sinceTrigger2 = 0;
            while (sinceTrigger2 < almostHold) { m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false); sinceTrigger2 += stepSamples; }
            check(m.get().clip, "TRUE PEAK OVER is still lit 1.0s after trigger #2 -- the second over restarted the full 1.5s hold");

            // And clears ~1.5s after trigger #2, not before.
            const int holdSamples = (int) std::round(1.5 * sr);
            while (sinceTrigger2 < holdSamples + stepSamples * 4) { m.captureOutput(quiet, 0.0f, -6.0f, ceilingDb, false); sinceTrigger2 += stepSamples; }
            check(! m.get().clip, "TRUE PEAK OVER clears ~1.5s after trigger #2 (the restarted hold), not trigger #1's original schedule");
        }

        // -- 5. Bypass forces it off immediately, never a stale latch -------------
        m.reset();
        m.captureOutput(quiet, -2.0f, ceilingDb + 0.08f, ceilingDb, false);
        check(m.get().clip, "(setup) TRUE PEAK OVER is latched on before the bypass check");
        m.captureOutput(quiet, 0.0f, ceilingDb + 0.08f, ceilingDb, true); // bypass engaged mid-overload
        check(! m.get().clip, "TRUE PEAK OVER is forced off the instant Bypass engages, no stale latch");
    }

    // OVERSAMPLING audit: the 1x/2x/4x/8x selector must be more than a label for the
    // actual *processing* (proven independently in oversampling_audit.cpp/
    // oversampling_isolation.cpp: aliasing, the nonlinear stage, and per-factor
    // latency all genuinely change). The True Peak *meter*, however, is now
    // deliberately decoupled from this selector — a dedicated, fixed 8x detector (see
    // LimiterEngine::truePeakDb()'s doc comment) re-analyses the final output on its
    // own, so the reading must stay consistent across 1x/2x/4x/8x rather than track
    // the selector. Also confirms per-factor latency actually differs (the processing
    // filters are really being switched, not just relabelled) and that switching
    // factors mid-stream doesn't produce NaN/Inf.
    void runOversamplingAudit()
    {
        const double sr = 48000.0;
        const int n = (int) sr;

        auto measureTruePeak = [&](int factor)
        {
            LimiterEngine engine;
            engine.prepare(sr, 512, 2);
            engine.requestOversamplingFactor(factor);
            // Ceiling set far above the signal so the limiter never engages — this
            // isolates the detector's raw measurement quality from any gain reduction.
            engine.setParameters(0.0f, 12.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
            auto sig = makeIntersamplePeakSignal(2, n, sr);
            juce::AudioBuffer<float> out(sig);
            float maxTp = -100.0f;
            int pos = 0;
            while (pos < n)
            {
                const int bs = juce::jmin(512, n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
                engine.process(chunk);
                maxTp = juce::jmax(maxTp, engine.truePeakDb());
                pos += bs;
            }
            return maxTp;
        };

        const float tp1x = measureTruePeak(1);
        const float tp2x = measureTruePeak(2);
        const float tp4x = measureTruePeak(4);
        const float tp8x = measureTruePeak(8);

        char label[220];
        std::snprintf(label, sizeof(label),
            "True peak reading on an intersample-peak signal: 1x=%.3f 2x=%.3f 4x=%.3f 8x=%.3f dBTP (dedicated detector: must stay consistent regardless of the OVERSAMPLING selector)",
            (double) tp1x, (double) tp2x, (double) tp4x, (double) tp8x);
        const float spread = juce::jmax(juce::jmax(tp1x, tp2x), juce::jmax(tp4x, tp8x))
                            - juce::jmin(juce::jmin(tp1x, tp2x), juce::jmin(tp4x, tp8x));
        check(spread < 0.5f, label); // the meter must not track the processing selector

        // Reported latency is now FIXED at the worst-case (8x) figure for every factor
        // -- an internal padding delay equalises each shorter-latency factor's real
        // physical latency up to that same figure (see LimiterEngine's class comment),
        // specifically so a live factor switch never changes host PDC. Verify with an
        // actual impulse per factor: the peak must land at the exact same sample
        // position regardless of which factor is active.
        {
            LimiterEngine latEngine;
            latEngine.prepare(sr, 256, 2);
            const int fixedLatency = latEngine.latencySamples();
            int peaks[4] {};
            const int factorsToCheck[] { 1, 2, 4, 8 };
            for (int fi = 0; fi < 4; ++fi)
            {
                LimiterEngine impEngine;
                impEngine.prepare(sr, 256, 1);
                impEngine.requestOversamplingFactor(factorsToCheck[fi]);
                impEngine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
                const int nImp = (int) sr;
                juce::AudioBuffer<float> sig(1, nImp);
                sig.clear();
                sig.setSample(0, 0, 1.0f);
                juce::AudioBuffer<float> out(sig);
                int pos = 0;
                while (pos < nImp)
                {
                    const int bs = juce::jmin(256, nImp - pos);
                    juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 1, pos, bs);
                    impEngine.process(chunk);
                    pos += bs;
                }
                int peakIdx = 0; float peakVal = 0.0f;
                for (int i = 0; i < nImp; ++i)
                {
                    const float v = std::abs(out.getSample(0, i));
                    if (v > peakVal) { peakVal = v; peakIdx = i; }
                }
                peaks[fi] = peakIdx;
            }
            char latLabel[200];
            std::snprintf(latLabel, sizeof(latLabel), "Physical latency identical for every factor (fixed=%d): 1x=%d 2x=%d 4x=%d 8x=%d samples",
                          fixedLatency, peaks[0], peaks[1], peaks[2], peaks[3]);
            check(peaks[0] == peaks[1] && peaks[1] == peaks[2] && peaks[2] == peaks[3], latLabel);
        }

        // Switching factors mid-stream (as the user actually does by clicking 1x/2x/4x/8x
        // while audio plays) must not produce NaN/Inf or leave the engine stuck.
        {
            LimiterEngine engine;
            engine.prepare(sr, 256, 2);
            auto sig = makeIntersamplePeakSignal(2, n, sr);
            juce::AudioBuffer<float> out(sig);
            const int factors[] { 1, 2, 4, 8, 4, 2, 1, 8 };
            int pos = 0, block = 0;
            while (pos < n)
            {
                engine.requestOversamplingFactor(factors[(block / 6) % 8]);
                engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
                const int bs = juce::jmin(256, n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
                engine.process(chunk);
                pos += bs;
                ++block;
            }
            check(! hasNonFinite(out), "cycling through 1x/2x/4x/8x mid-stream stays finite (no NaN/Inf, no stuck state)");
        }
    }
}

int main()
{
    std::printf("== NF Limiter DSP test harness ==\n\n");

    std::printf("-- Ceiling / NaN-Inf matrix (sample rates x channels x oversampling x block sizes x signals) --\n");
    runCeilingMatrix();

    std::printf("\n-- Latency reporting --\n");
    runLatencyReport();

    std::printf("\n-- Automation click check (informational) --\n");
    runAutomationClickCheck();

    std::printf("\n-- Bypass latency-compensation check --\n");
    runBypassLatencyCheck();

    std::printf("\n-- Bypass ignores Gain (0 vs +24dB must be identical) --\n");
    runBypassIgnoresGainCheck();

    std::printf("\n-- Bypass null test (output == input, latency-aligned) --\n");
    runBypassNullTest();

    std::printf("\n-- Gain applied exactly once (not doubled) --\n");
    runGainAppliedOnceCheck();

    std::printf("\n-- Bypass toggle click check --\n");
    runBypassToggleClickCheck();

    std::printf("\n-- Bypass reports 0.0dB gain reduction --\n");
    runBypassReportsZeroGrCheck();

    std::printf("\n-- Gain x Ceiling matrix (true peak must never exceed ceiling by > 0.05dB) --\n");
    runGainCeilingMatrixCheck();

    std::printf("\n-- Stereo Link asymmetric check (impulsive L, quiet constant R) --\n");
    runStereoLinkAsymmetricCheck();

    std::printf("\n-- Stereo Link per-channel curve check (offset impulses on L and R) --\n");
    runStereoLinkPerChannelCurveCheck();

    std::printf("\n-- Stereo Link mono no-op check --\n");
    runStereoLinkMonoNoOpCheck();

    std::printf("\n-- Stereo Link bypass no-op check --\n");
    runStereoLinkBypassNoOpCheck();

    std::printf("\n-- CLIP indicator check --\n");
    runClipIndicatorCheck();

    std::printf("\n-- OVERSAMPLING audit (1x/2x/4x/8x actually change detection) --\n");
    runOversamplingAudit();

    std::printf("\n-- TRUE PEAK algorithm audit (Teste A / Teste B) --\n");
    runTruePeakAlgorithmAudit();

    std::printf("\n-- TRUE PEAK OVER indicator follows the meter, not the toggle --\n");
    runTruePeakOverIndicatorFollowsMeterCheck();

    std::printf("\n-- TRUE PEAK toggle transition check --\n");
    runTruePeakToggleTransitionCheck();

    std::printf("\n== %d failure(s), %d warning(s) ==\n", failures, warnings);
    return failures == 0 ? 0 : 1;
}

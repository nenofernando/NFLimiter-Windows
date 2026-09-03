// Deeper isolation of the two open findings from oversampling_audit.cpp:
//   1. LimiterEngine's own True Peak reading sits ~0.37-0.42dB above an independent
//      16x reference at 2x and 8x, while 4x agrees closely.
//   2. 4x measured ~1.7dB worse aliasing than its neighbours (2x, 8x).
// This file isolates each stage of the signal path separately -- upsampling alone,
// the nonlinear stage alone (at the oversampled rate), downsampling alone, and the
// full chain -- to localise exactly where the 4x deviation originates, before any
// DSP change is made. No LimiterEngine internals are modified; this only observes.
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <cmath>

namespace
{
    int failures = 0;
    void note(const std::string& s) { std::printf("  %s\n", s.c_str()); }

    juce::AudioBuffer<float> makeSine(int n, double sr, double freq, float amplitude)
    {
        juce::AudioBuffer<float> b(1, n);
        for (int i = 0; i < n; ++i)
            b.setSample(0, i, (float) (amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * freq * (double) i / sr)));
        return b;
    }

    int stagesFor(int factor) { return (int) std::round(std::log2((double) factor)); }

    // Mirrors LimiterEngine's own applyCharacter() Loud formula exactly (Source/
    // LimiterEngine.cpp), so the "reference nonlinearity" below is bit-for-bit the
    // same transfer function, just evaluated at a much higher oversampling factor.
    inline float loudSaturate(float y) { return std::tanh(y * 1.15f) / std::tanh(1.15f); }

    // ---- Stage 1: pure filter round-trip, NO processing in between --------------
    // Up then immediately down through LimiterEngine's own filter family
    // (FIREquiripple, isMaximumQuality=true, integer latency), built exactly the way
    // LimiterEngine builds it. Any amplitude deviation here is 100% attributable to
    // the filter cascade itself -- there is no nonlinearity, no gain, no lookahead.
    struct RoundTripResult { float gainDb; int latency; };

    RoundTripResult pureRoundTrip(int factor, double freq, double sr)
    {
        const int n = 1 << 15;
        auto sig = makeSine(n, sr, freq, 1.0f);
        if (factor == 1)
        {
            // No filtering at all -- 0dB by construction, latency 0.
            return { 0.0f, 0 };
        }
        juce::dsp::Oversampling<float> os(1, (size_t) stagesFor(factor),
                                           juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        os.initProcessing((size_t) n);
        const int latency = (int) std::round(os.getLatencyInSamples());

        juce::AudioBuffer<float> work(sig);
        juce::dsp::AudioBlock<float> block(work);
        auto up = os.processSamplesUp(block);
        os.processSamplesDown(block);

        // Steady-state RMS ratio, well past the filter's own latency and the initial
        // transient, referenced against the (delay-compensated) input RMS.
        const int skip = juce::jmax(latency + 200, 500);
        double outSumSq = 0.0, inSumSq = 0.0;
        int count = 0;
        for (int i = skip; i < n; ++i)
        {
            const float o = work.getSample(0, i);
            const float in = sig.getSample(0, i - latency);
            outSumSq += (double) o * o;
            inSumSq += (double) in * in;
            ++count;
        }
        const double outRms = std::sqrt(outSumSq / juce::jmax(1, count));
        const double inRms = std::sqrt(inSumSq / juce::jmax(1, count));
        const float gainDb = (float) (20.0 * std::log10(juce::jmax(1.0e-12, outRms / juce::jmax(1.0e-12, inRms))));
        juce::ignoreUnused(up);
        return { gainDb, latency };
    }

    // ---- Stage 2: nonlinearity alone, at the oversampled rate, no lookahead -----
    // Up -> apply the exact Loud tanh formula sample-by-sample at the oversampled
    // rate -> down. No gain reduction, no ceiling clamp: purely "how much does THIS
    // factor's filter cascade let the tanh's harmonics alias back down".
    float nonlinearOnlyAliasResidualDb(int factor, int referenceFactor, double freq, double sr, float driveGain)
    {
        const int n = 1 << 15;
        auto sig = makeSine(n, sr, freq, 1.0f);

        auto render = [&](int f) -> juce::AudioBuffer<float>
        {
            juce::dsp::Oversampling<float> os(1, (size_t) stagesFor(f),
                                               juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
            os.initProcessing((size_t) n);
            juce::AudioBuffer<float> work(sig);
            juce::dsp::AudioBlock<float> block(work);
            auto up = os.processSamplesUp(block);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                auto* d = up.getChannelPointer(0);
                d[i] = loudSaturate(d[i] * driveGain);
            }
            os.processSamplesDown(block);
            return work;
        };

        auto testOut = render(factor);
        auto refOut = render(referenceFactor);

        const int latTest = (int) std::round(juce::dsp::Oversampling<float>(1, (size_t) stagesFor(factor),
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true).getLatencyInSamples());
        const int latRef = (int) std::round(juce::dsp::Oversampling<float>(1, (size_t) stagesFor(referenceFactor),
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true).getLatencyInSamples());

        const int skip = juce::jmax(latTest, latRef) + 300;
        double residualSumSq = 0.0, refSumSq = 0.0;
        int count = 0;
        for (int i = skip; i < n - 10; ++i)
        {
            const float a = testOut.getSample(0, i);
            const float b = refOut.getSample(0, i - latTest + latRef);
            const double diff = (double) a - (double) b;
            residualSumSq += diff * diff;
            refSumSq += (double) b * b;
            ++count;
        }
        const double residualDb = 10.0 * std::log10(juce::jmax(1.0e-18, residualSumSq) / juce::jmax(1.0e-18, refSumSq));
        return (float) residualDb;
    }
}

int main()
{
    std::printf("== NF Limiter OVERSAMPLING isolation audit (stage-by-stage) ==\n\n");

    // --------------------------------------------------------------------------
    // Isolation 1: pure filter cascade (upsampling immediately followed by
    // downsampling, zero processing in between). If 4x deviates here, the anomaly
    // is 100% in the filter cascade itself, unrelated to LimiterEngine's gain,
    // nonlinearity, lookahead, or True Peak detector.
    // --------------------------------------------------------------------------
    std::printf("-- Isolation 1: pure filter round-trip gain (no nonlinearity, no limiter at all) --\n");
    {
        const double freqs[] { 7000.0, 9000.0, 11000.0, 13000.0, 15000.0, 17000.0 };
        const double rates[] { 44100.0, 48000.0 };
        for (double sr : rates)
        {
            std::printf("  sample rate = %.0f Hz\n", sr);
            for (double f : freqs)
            {
                if (f >= sr * 0.5) continue; // above Nyquist, skip
                auto r1 = pureRoundTrip(1, f, sr);
                auto r2 = pureRoundTrip(2, f, sr);
                auto r4 = pureRoundTrip(4, f, sr);
                auto r8 = pureRoundTrip(8, f, sr);
                char l[220];
                std::snprintf(l, sizeof(l), "%.0fHz: 1x=%.3fdB 2x=%.3fdB 4x=%.3fdB 8x=%.3fdB (pure filter passband gain, 0dB=ideal)",
                              f, (double) r1.gainDb, (double) r2.gainDb, (double) r4.gainDb, (double) r8.gainDb);
                note(l);
            }
        }
    }

    // --------------------------------------------------------------------------
    // Isolation 2: nonlinearity alone (Loud tanh, no lookahead/ceiling), at each
    // factor, residual against a 32x rendering of the SAME nonlinearity (same
    // formula, near-ideal oversampling). This isolates "nonlinear stage at high
    // rate" + "downsampling" combined, excluding any lookahead/gain-reduction
    // logic entirely.
    // --------------------------------------------------------------------------
    std::printf("\n-- Isolation 2: nonlinearity-only residual vs 32x reference (same tanh formula) --\n");
    {
        const double freqs[] { 7000.0, 9000.0, 11000.0, 13000.0, 15000.0, 17000.0 };
        const double rates[] { 44100.0, 48000.0 };
        const float drives[] { 1.5f, 3.0f, 6.0f }; // ~3.5dB, ~9.5dB, ~15.6dB of drive into tanh
        for (double sr : rates)
        {
            for (float drive : drives)
            {
                std::printf("  sample rate = %.0f Hz, drive = %.1fx (~%.1fdB)\n", sr, (double) drive, (double) juce::Decibels::gainToDecibels(drive));
                for (double f : freqs)
                {
                    if (f >= sr * 0.5) continue;
                    const float r2 = nonlinearOnlyAliasResidualDb(2, 32, f, sr, drive);
                    const float r4 = nonlinearOnlyAliasResidualDb(4, 32, f, sr, drive);
                    const float r8 = nonlinearOnlyAliasResidualDb(8, 32, f, sr, drive);
                    char l[200];
                    std::snprintf(l, sizeof(l), "%.0fHz: 2x=%.2fdB 4x=%.2fdB 8x=%.2fdB (residual energy vs 32x reference, more negative = cleaner)",
                                  f, (double) r2, (double) r4, (double) r8);
                    note(l);
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // Isolation 3: does the *peak-reading* (not RMS gain) of a pure filter
    // round-trip on the actual two-tone intersample-peak test signal reproduce the
    // 2x/8x-vs-4x detector gap from oversampling_audit.cpp, with NO nonlinearity, NO
    // gain reduction, and NO LimiterEngine code involved at all? If so, the gap is a
    // peak-reconstruction/phase characteristic of the filter cascade on this specific
    // beat signal, not a bug in LimiterEngine's detector logic.
    // --------------------------------------------------------------------------
    std::printf("\n-- Isolation 3: pure filter PEAK reconstruction on the two-tone intersample-peak signal --\n");
    {
        const double sr = 48000.0;
        const int n = 1 << 15;
        const double f1 = sr * 0.30, f2 = sr * 0.32;
        juce::AudioBuffer<float> sig(1, n);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            float x = 0.5f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * f1 * t)
                                     + std::sin(2.0 * juce::MathConstants<double>::pi * f2 * t));
            sig.setSample(0, i, juce::jlimit(-1.0f, 1.0f, x * 1.98f));
        }

        auto peakOf = [&](int factor) -> float
        {
            juce::dsp::Oversampling<float> os(1, (size_t) stagesFor(factor),
                                               juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
            os.initProcessing((size_t) n);
            juce::AudioBuffer<float> work(sig);
            juce::dsp::AudioBlock<float> block(work);
            auto up = os.processSamplesUp(block);
            float maxAbs = 0.0f;
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                maxAbs = juce::jmax(maxAbs, std::abs(up.getChannelPointer(0)[i]));
            return juce::Decibels::gainToDecibels(maxAbs, -150.0f);
        };

        const float p2 = peakOf(2), p4 = peakOf(4), p8 = peakOf(8);
        char l[220];
        std::snprintf(l, sizeof(l), "Pure-filter (no LimiterEngine, no nonlinearity) reconstructed peak: 2x=%.3fdBTP 4x=%.3fdBTP 8x=%.3fdBTP",
                      (double) p2, (double) p4, (double) p8);
        note(l);
        note("Compare against oversampling_audit.cpp's LimiterEngine-measured gap: 2x=3.833 4x=3.369 8x=3.833 dBTP (same shape = filter-cascade origin; different shape = LimiterEngine-specific).");
    }

    std::printf("\n(This file only observes -- %d hard failure(s) from internal consistency checks, if any)\n", failures);
    return 0;
}

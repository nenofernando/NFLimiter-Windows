// Deterministic block-size invariance audit: the SAME continuous stream, processed
// once as a single huge block and again split into fixed-size chunks, must produce
// bit-for-bit (or numerically negligible) identical output for the same oversampling
// factor -- LimiterEngine's own state (delay rings, lookahead window, release
// smoothing) is designed to persist seamlessly across process() calls, so chunking
// must never change the result. Any divergence pinpoints a real bug in how process()
// handles a block boundary, separate from anything about the JUCE Oversampling filter
// itself (already ruled out in oversampling_isolation.cpp).
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <cmath>
#include <random>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& s) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", s.c_str()); if (! ok) ++failures; }
    void note(const std::string& s) { std::printf("  [INFO] %s\n", s.c_str()); }

    juce::AudioBuffer<float> makeTwoTone(int chans, int n, double sr)
    {
        juce::AudioBuffer<float> b(chans, n);
        const double f1 = sr * 0.30, f2 = sr * 0.32;
        for (int ch = 0; ch < chans; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / sr;
                float x = 0.5f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * f1 * t)
                                         + std::sin(2.0 * juce::MathConstants<double>::pi * f2 * t));
                b.setSample(ch, i, juce::jlimit(-1.0f, 1.0f, x * 1.98f));
            }
        return b;
    }

    juce::AudioBuffer<float> makeImpulse(int chans, int n)
    {
        juce::AudioBuffer<float> b(chans, n);
        b.clear();
        for (int ch = 0; ch < chans; ++ch) b.setSample(ch, 0, 1.0f);
        return b;
    }

    // Renders `sig` through a freshly-prepared LimiterEngine, feeding it in chunks of
    // exactly `blockSize` samples (the last chunk may be shorter). Deterministic
    // parameters throughout -- no randomised automation, autoRelease off, fixed
    // release -- so the only thing that can differ between two renders of the same
    // factor is the chunking itself.
    juce::AudioBuffer<float> render(int factor, int chans, double sr, const juce::AudioBuffer<float>& sig, int blockSize, bool trace)
    {
        LimiterEngine engine;
        const int n = sig.getNumSamples();
        engine.prepare(sr, juce::jmax(n, blockSize), chans);
        engine.requestOversamplingFactor(factor);
        engine.setParameters(6.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);

        juce::AudioBuffer<float> out(sig);
        int pos = 0, callIndex = 0;
        const int lookaheadBase = (int) std::round(sr * 0.005); // matches LimiterEngine::kLookaheadMs
        while (pos < n)
        {
            const int bs = juce::jmin(blockSize, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), chans, pos, bs);
            engine.process(chunk);
            if (trace && callIndex < 6)
            {
                std::printf("    [trace] call=%d inputNumSamples=%d factor=%dx lookaheadBaseSamples=%d lookaheadOsSamples=%d latencySamples=%d\n",
                            callIndex, bs, factor, lookaheadBase, lookaheadBase * factor, engine.latencySamples());
            }
            pos += bs;
            ++callIndex;
        }
        return out;
    }

    float maxAbsDiffLatencyAligned(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int latencyA, int latencyB, int skipEdge)
    {
        // Both should already share the same latency (same factor => same latency),
        // but accept different values defensively and align on the larger.
        const int shift = latencyB - latencyA;
        float maxDiff = 0.0f;
        const int n = juce::jmin(a.getNumSamples(), b.getNumSamples());
        for (int ch = 0; ch < juce::jmin(a.getNumChannels(), b.getNumChannels()); ++ch)
            for (int i = skipEdge; i < n - juce::jmax(0, shift) - skipEdge; ++i)
            {
                const int j = i + shift;
                if (j < 0 || j >= n) continue;
                maxDiff = juce::jmax(maxDiff, std::abs(a.getSample(ch, i) - b.getSample(ch, j)));
            }
        return maxDiff;
    }
}

int main()
{
    std::printf("== NF Limiter block-size invariance audit ==\n\n");

    const int blockSizes[] { 1, 7, 16, 31, 32, 63, 64, 127, 128, 255, 256, 511, 512, 1024 };
    const double sampleRates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int factors[] { 1, 2, 4, 8 };
    const int channelCounts[] { 1, 2 };

    std::mt19937 rng(12345);

    std::printf("-- Deterministic null test: single-block reference vs chunked renders (two-tone signal) --\n");
    float worstDiffOverall = 0.0f;
    int worstFactor = 0, worstBlockSize = 0; double worstSr = 0; int worstChans = 0;

    for (double sr : sampleRates)
    {
        for (int chans : channelCounts)
        {
            const int n = (int) sr; // 1 second
            auto sig = makeTwoTone(chans, n, sr);

            for (int factor : factors)
            {
                auto reference = render(factor, chans, sr, sig, n, false); // one single call
                LimiterEngine latEngine; latEngine.prepare(sr, n, chans); latEngine.requestOversamplingFactor(factor);
                const int latency = latEngine.latencySamplesFor(factor);

                float worstForThisConfig = 0.0f;
                int worstBs = 0;
                for (int bs : blockSizes)
                {
                    auto test = render(factor, chans, sr, sig, bs, false);
                    const float diff = maxAbsDiffLatencyAligned(reference, test, latency, latency, latency + 8);
                    if (diff > worstForThisConfig) { worstForThisConfig = diff; worstBs = bs; }
                }
                // One random block size too, per the audit's request.
                std::uniform_int_distribution<int> bsDist(1, 4096);
                const int randomBs = bsDist(rng);
                auto testRandom = render(factor, chans, sr, sig, randomBs, false);
                const float randomDiff = maxAbsDiffLatencyAligned(reference, testRandom, latency, latency, latency + 8);
                if (randomDiff > worstForThisConfig) { worstForThisConfig = randomDiff; worstBs = randomBs; }

                char l[240];
                std::snprintf(l, sizeof(l), "sr=%.0f ch=%d factor=%dx: worst null-test diff across all block sizes = %.8f (at blockSize=%d)",
                              sr, chans, factor, (double) worstForThisConfig, worstBs);
                check(worstForThisConfig < 1.0e-4f, l);

                if (worstForThisConfig > worstDiffOverall)
                {
                    worstDiffOverall = worstForThisConfig; worstFactor = factor; worstBlockSize = worstBs; worstSr = sr; worstChans = chans;
                }
            }
        }
    }
    char summary[220];
    std::snprintf(summary, sizeof(summary), "Overall worst block-size-dependent divergence: %.8f at sr=%.0f ch=%d factor=%dx blockSize=%d",
                  (double) worstDiffOverall, worstSr, worstChans, worstFactor, worstBlockSize);
    note(summary);

    // ----------------------------------------------------------------------------
    // If a real divergence was found, localise the first diverging sample using an
    // impulse (cleanest possible signal to pinpoint exactly where things go wrong)
    // at the worst offending configuration.
    // ----------------------------------------------------------------------------
    if (worstDiffOverall >= 1.0e-4f)
    {
        std::printf("\n-- Localising the first diverging sample (impulse, worst configuration) --\n");
        const int n = (int) worstSr;
        auto imp = makeImpulse(worstChans, n);
        auto ref = render(worstFactor, worstChans, worstSr, imp, n, true);
        auto test = render(worstFactor, worstChans, worstSr, imp, worstBlockSize, true);
        int firstDiverge = -1;
        for (int i = 0; i < n; ++i)
        {
            if (std::abs(ref.getSample(0, i) - test.getSample(0, i)) > 1.0e-6f) { firstDiverge = i; break; }
        }
        char l[160];
        std::snprintf(l, sizeof(l), "First diverging sample index (impulse response): %d", firstDiverge);
        note(l);
    }
    else
    {
        note("No divergence found above 1e-4 anywhere in the matrix -- skipping impulse localisation (nothing to localise).");
    }

    // ----------------------------------------------------------------------------
    // Oversampling-factor switch mid-stream must not corrupt state (separate from
    // block-size invariance, but part of the same "state preserved between chunks"
    // family of checks the audit asked for).
    // ----------------------------------------------------------------------------
    std::printf("\n-- Switching factor mid-stream does not leave stale/corrupted state --\n");
    {
        const double sr = 48000.0;
        const int n = (int) sr * 2;
        LimiterEngine engine;
        engine.prepare(sr, 512, 2);
        auto sig = makeTwoTone(2, n, sr);
        juce::AudioBuffer<float> out(sig);
        const int seq[] { 1, 2, 4, 8, 4, 2, 1, 8 };
        int pos = 0, block = 0;
        bool finite = true;
        while (pos < n)
        {
            engine.requestOversamplingFactor(seq[(block / 8) % 8]);
            engine.setParameters(6.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
            const int bs = juce::jmin(256, n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
            engine.process(chunk);
            for (int ch = 0; ch < 2 && finite; ++ch)
                for (int i = 0; i < bs; ++i)
                    if (! std::isfinite(chunk.getSample(ch, i))) finite = false;
            pos += bs;
            ++block;
        }
        check(finite, "cycling 1x/2x/4x/8x/4x/2x/1x/8x mid-stream stays finite throughout");
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

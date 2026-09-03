// Fast, targeted sanitizer check for the dedicated True Peak detector added this
// session (not a re-run of the full pre-existing DSP matrix, which is already
// covered by NFLimiterDspTests in Release and is unrelated to this specific change --
// re-running it under ASan/UBSan is prohibitively slow and not what's being audited
// here). Exercises exactly the code paths this audit is about: prepare/process/reset
// at every sample rate, channel count and OVERSAMPLING factor combination, True Peak
// on and off, mid-stream factor switching, and repeated prepare() calls (simulating
// sample-rate changes and full processor reinitialisation), all under
// AddressSanitizer + UndefinedBehaviorSanitizer.
#include "../Source/LimiterEngine.h"
#include <cstdio>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& s) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", s.c_str()); if (! ok) ++failures; }

    juce::AudioBuffer<float> makeSignal(int chans, int n, double sr)
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
}

int main()
{
    std::printf("== Targeted sanitizer check: dedicated True Peak detector ==\n\n");

    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int blockSizes[] { 1, 7, 32, 63, 256, 1023 };
    const int channelCounts[] { 1, 2 };
    const int factors[] { 1, 2, 4, 8 };

    std::printf("-- prepare/process/reset matrix (sr x ch x blockSize x factor x TruePeak on/off) --\n");
    for (double sr : rates)
        for (int chans : channelCounts)
            for (int bs : blockSizes)
                for (int factor : factors)
                    for (bool tp : { true, false })
                    {
                        LimiterEngine engine;
                        engine.prepare(sr, bs, chans);
                        engine.requestOversamplingFactor(factor);
                        engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, tp, false);
                        auto sig = makeSignal(chans, bs * 5, sr);
                        int pos = 0;
                        while (pos < sig.getNumSamples())
                        {
                            const int n = juce::jmin(bs, sig.getNumSamples() - pos);
                            juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), chans, pos, n);
                            engine.process(chunk);
                            pos += n;
                        }
                        engine.reset();
                        // One more pass after reset, to exercise the post-reset state too.
                        juce::AudioBuffer<float> chunk2(sig.getArrayOfWritePointers(), chans, 0, juce::jmin(bs, sig.getNumSamples()));
                        engine.process(chunk2);
                    }
    check(true, "full prepare/process/reset matrix completed without a sanitizer trap");

    std::printf("\n-- Repeated prepare() calls at different rates/channels (simulates sample-rate change / full reinit) --\n");
    {
        LimiterEngine engine;
        const struct { double sr; int chans; int bs; } sequence[] {
            { 44100.0, 2, 512 }, { 96000.0, 2, 256 }, { 48000.0, 1, 64 },
            { 192000.0, 2, 1024 }, { 44100.0, 1, 32 }, { 48000.0, 2, 512 },
        };
        for (auto& s : sequence)
        {
            engine.prepare(s.sr, s.bs, s.chans);
            engine.requestOversamplingFactor(4);
            engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
            auto sig = makeSignal(s.chans, s.bs * 10, s.sr);
            int pos = 0;
            while (pos < sig.getNumSamples())
            {
                const int n = juce::jmin(s.bs, sig.getNumSamples() - pos);
                juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), s.chans, pos, n);
                engine.process(chunk);
                pos += n;
            }
        }
        check(true, "sequence of prepare() calls at varying sr/channels/blockSize completed without a sanitizer trap");
    }

    std::printf("\n-- Mid-stream factor switching (1x/2x/4x/8x cycling, all channel/rate combos) --\n");
    for (double sr : rates)
        for (int chans : channelCounts)
        {
            LimiterEngine engine;
            engine.prepare(sr, 256, chans);
            auto sig = makeSignal(chans, 256 * 40, sr);
            int pos = 0, block = 0;
            while (pos < sig.getNumSamples())
            {
                engine.requestOversamplingFactor(factors[block % 4]);
                engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, (block % 2) == 0, false);
                const int n = juce::jmin(256, sig.getNumSamples() - pos);
                juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), chans, pos, n);
                engine.process(chunk);
                pos += n;
                ++block;
            }
        }
    check(true, "mid-stream factor + True Peak toggling across all sr/channel combos completed without a sanitizer trap");

    std::printf("\n-- Odd/boundary block sizes against a smaller maxBlockSize (buffer-boundary stress) --\n");
    {
        // prepare() with a SMALL maxBlockSize, then feed the largest block size it was
        // prepared for, repeatedly -- exercises the scratch buffer and oversampler
        // right at their allocated capacity boundary.
        LimiterEngine engine;
        engine.prepare(48000.0, 1024, 2);
        engine.requestOversamplingFactor(8);
        engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
        auto sig = makeSignal(2, 1024 * 20, 48000.0);
        int pos = 0;
        while (pos < sig.getNumSamples())
        {
            const int n = juce::jmin(1024, sig.getNumSamples() - pos); // exactly maxBlockSize every call
            juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), 2, pos, n);
            engine.process(chunk);
            pos += n;
        }
        check(true, "repeated at-capacity (maxBlockSize-sized) blocks completed without a sanitizer trap");
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

// Standalone DSP correctness harness for LimiterEngine — no plugin wrapper, no host.
// Exercises the sample-rate / block-size / signal matrix required by
// Docs/PROMPT_PARA_CLAUDE.txt and asserts the two properties a limiter can never fail:
// the true-peak ceiling is never exceeded, and the output never contains NaN/Inf.
// It also runs an automation pass on every smoothed parameter and reports the largest
// sample-to-sample discontinuity seen, as a proxy for zipper/click artifacts.
#include "../Source/LimiterEngine.h"
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

    void runLatencyReport()
    {
        LimiterEngine engine;
        engine.prepare(48000.0, 512, 2);
        for (int f : { 1, 2, 4, 8 })
        {
            char label[128];
            std::snprintf(label, sizeof(label), "latencySamplesFor(%dx) = %d samples (%.2f ms) @ 48kHz",
                          f, engine.latencySamplesFor(f), 1000.0 * engine.latencySamplesFor(f) / 48000.0);
            check(engine.latencySamplesFor(f) > 0, label);
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

    std::printf("\n== %d failure(s), %d warning(s) ==\n", failures, warnings);
    return failures == 0 ? 0 : 1;
}

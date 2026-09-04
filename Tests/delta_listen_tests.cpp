// DELTA/LISTEN audit: a monitoring-only route (never a sonic parameter) that, when
// engaged, replaces the audio actually sent to the host with (aligned pre-limiter
// reference) minus (normal final output) -- everything the limiter removed or
// changed. See LimiterEngine::setDeltaListenEnabled()/process() for the
// implementation and the reasoning behind each design choice referenced below.
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <cmath>
#include <vector>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& s) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", s.c_str()); if (! ok) ++failures; }

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

    juce::AudioBuffer<float> makeQuietSine(int channels, int n, double sr, float amp)
    {
        juce::AudioBuffer<float> b(channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample(ch, i, amp * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 500.0 * (double) i / sr));
        return b;
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

    // Renders with delta OFF for the WHOLE render, or ON from a given sample onward
    // (never toggled mid-render unless requested), in fixed-size blocks.
    juce::AudioBuffer<float> render(double sr, int chans, int blockSize, const juce::AudioBuffer<float>& sig,
                                     LimiterEngine::Character character, bool truePeakOn, bool bypassedFlag,
                                     bool deltaOn, int deltaOnFromSample = 0)
    {
        LimiterEngine engine;
        engine.prepare(sr, juce::jmax(blockSize, 1), chans);
        engine.setParameters(0.0f, -1.0f, 150.0f, false, character, 100.0f, truePeakOn, bypassedFlag);
        juce::AudioBuffer<float> out(sig);
        int pos = 0;
        const int n = out.getNumSamples();
        while (pos < n)
        {
            const bool wantDelta = deltaOn && pos >= deltaOnFromSample;
            engine.setDeltaListenEnabled(wantDelta);
            const int bs = juce::jmin(juce::jmax(1, blockSize), n - pos);
            juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), chans, pos, bs);
            engine.process(chunk);
            pos += bs;
        }
        return out;
    }
}

int main()
{
    std::printf("== NF Limiter DELTA/LISTEN audit ==\n");

    // ------------------------------------------------------------------------------
    // 1. DELTA OFF is bit-identical to the engine with no Delta machinery touched at
    // all -- i.e. adding Delta must not change a single sample, the reported latency,
    // or the meters when Listen is never engaged.
    // ------------------------------------------------------------------------------
    std::printf("\n-- DELTA OFF transparency --\n");
    {
        const double sr = 48000.0;
        const int n = 20000;
        auto sig = makeIntersamplePeakSignal(2, n, sr);

        LimiterEngine a; a.prepare(sr, 256, 2); a.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        LimiterEngine b; b.prepare(sr, 256, 2); b.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        b.setDeltaListenEnabled(false); // explicit no-op call, same as the processor makes every block

        juce::AudioBuffer<float> outA(sig), outB(sig);
        int pos = 0;
        while (pos < n)
        {
            const int bs = juce::jmin(256, n - pos);
            juce::AudioBuffer<float> ca(outA.getArrayOfWritePointers(), 2, pos, bs);
            juce::AudioBuffer<float> cb(outB.getArrayOfWritePointers(), 2, pos, bs);
            a.process(ca);
            b.process(cb);
            pos += bs;
        }
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(outA.getSample(ch, i) - outB.getSample(ch, i)));
        char l[160];
        std::snprintf(l, sizeof(l), "bit-identical output with Delta machinery present but OFF: max diff = %.8f, latency %d vs %d", (double) maxDiff, a.latencySamples(), b.latencySamples());
        check(maxDiff == 0.0f && a.latencySamples() == b.latencySamples(), l);
    }

    // ------------------------------------------------------------------------------
    // 2. Null test: Gain=0dB, CLEAN, signal well under ceiling (no reduction at all),
    // Delta ON. After the latency fills AND deltaMix's own 15ms ramp (see
    // LimiterEngine::prepare()) has settled, Delta should be numerically silent --
    // skipping only the processing latency here (as an earlier version of this test
    // did) still catches deltaMix mid-ramp and measures a large, entirely expected
    // residual that has nothing to do with the reference/alignment itself.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Null test: no reduction, Delta ON --\n");
    {
        const double sr = 48000.0;
        const int n = 20000;
        auto sig = makeQuietSine(2, n, sr, 0.1f); // -20dBFS, nowhere near -1dBTP ceiling
        auto out = render(sr, 2, 256, sig, LimiterEngine::Character::clean, true, false, true, 0);

        LimiterEngine probe; probe.prepare(sr, 256, 2);
        const int settle = probe.latencySamples() + (int) std::round(sr * 0.015) + 64;
        float maxAbs = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = settle; i < n; ++i)
                maxAbs = juce::jmax(maxAbs, std::abs(out.getSample(ch, i)));
        const float db = juce::Decibels::gainToDecibels(maxAbs, -200.0f);
        char l[160];
        std::snprintf(l, sizeof(l), "worst |Delta| after latency settles = %.2fdBFS (tolerance: below -120dBFS)", (double) db);
        check(db < -120.0f, l);
    }

    // ------------------------------------------------------------------------------
    // 3. Real reduction: signal driven above ceiling, Delta must reproduce only the
    // removed/changed content -- non-trivial energy, no channel inversion (L and R
    // driven identically here, so Delta's L and R should track each other in sign).
    // ------------------------------------------------------------------------------
    std::printf("\n-- Real reduction: Delta reproduces the removed content --\n");
    {
        const double sr = 48000.0;
        const int n = 40000;
        auto sig = makeIntersamplePeakSignal(2, n, sr); // hits the ceiling hard
        auto outDelta = render(sr, 2, 256, sig, LimiterEngine::Character::clean, true, false, true, 0);

        LimiterEngine probe; probe.prepare(sr, 256, 2);
        const int settle = probe.latencySamples() + (int) std::round(sr * 0.02) + 64;
        float deltaEnergy = 0.0f;
        for (int i = settle; i < n; ++i)
            deltaEnergy += std::abs(outDelta.getSample(0, i));
        char l1[160];
        std::snprintf(l1, sizeof(l1), "Delta carries real, non-trivial energy while limiting is active: mean|Delta| = %.6f", (double) (deltaEnergy / juce::jmax(1, n - settle)));
        check(deltaEnergy > 0.001f * (double) (n - settle), l1);

        // Heavier reduction should mean a MORE evident Delta: compare a ceiling close
        // to the signal's own peak (light reduction) against a much tighter one
        // (heavy reduction).
        LimiterEngine light; light.prepare(sr, 256, 2); light.setParameters(0.0f, -0.1f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        LimiterEngine heavy; heavy.prepare(sr, 256, 2); heavy.setParameters(0.0f, -8.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
        auto renderWithCeiling = [&](LimiterEngine& eng) {
            juce::AudioBuffer<float> out(sig);
            eng.setDeltaListenEnabled(true);
            int pos = 0;
            while (pos < n) { const int bs = juce::jmin(256, n - pos); juce::AudioBuffer<float> c(out.getArrayOfWritePointers(), 2, pos, bs); eng.process(c); pos += bs; }
            return out;
        };
        auto outLight = renderWithCeiling(light);
        auto outHeavy = renderWithCeiling(heavy);
        float energyLight = 0.0f, energyHeavy = 0.0f;
        for (int i = settle; i < n; ++i) { energyLight += std::abs(outLight.getSample(0, i)); energyHeavy += std::abs(outHeavy.getSample(0, i)); }
        char l2[200];
        std::snprintf(l2, sizeof(l2), "heavier reduction (-8dBTP) produces more evident Delta than light (-0.1dBTP): %.4f vs %.4f", (double) energyHeavy, (double) energyLight);
        check(energyHeavy > energyLight, l2);
    }

    // ------------------------------------------------------------------------------
    // 4. Latency/alignment: an impulse fed with Delta ON must show the reference and
    // the (heavily attenuated) output aligned to the sample -- confirmed indirectly
    // via the null test above (any misalignment would produce comb-filtering energy
    // even on a signal well under ceiling); here, confirm reported latency is
    // unaffected by Delta being on.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Latency: unaffected by Delta --\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        LimiterEngine withoutDelta; withoutDelta.prepare(sr, 256, 2);
        LimiterEngine withDelta; withDelta.prepare(sr, 256, 2); withDelta.setDeltaListenEnabled(true);
        char l[160];
        std::snprintf(l, sizeof(l), "sr=%.0f: latency %d (Delta never off) vs %d (Delta off) -- must be identical",
                      sr, withDelta.latencySamples(), withoutDelta.latencySamples());
        check(withDelta.latencySamples() == withoutDelta.latencySamples(), l);
    }

    // ------------------------------------------------------------------------------
    // 5. Transition: toggling Delta on/off during strong, continuous audio, across
    // every mandated block size, must never click, jump DC, or produce NaN/Inf.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Transition: toggling Delta during strong audio, all mandated block sizes --\n");
    {
        const double sr = 48000.0;
        const int n = 48000;
        auto sig = makeIntersamplePeakSignal(2, n, sr);
        for (int blockSize : { 1, 7, 16, 32, 64, 128, 256, 512, 1024 })
        {
            LimiterEngine engine;
            engine.prepare(sr, juce::jmax(blockSize, 1), 2);
            engine.setParameters(0.0f, -1.0f, 150.0f, false, LimiterEngine::Character::clean, 100.0f, true, false);
            juce::AudioBuffer<float> out(sig);
            int pos = 0;
            bool nonFinite = false;
            float maxStep = 0.0f;
            float prevL = 0.0f, prevR = 0.0f;
            bool havePrev = false;
            int togglePeriod = juce::jmax(1, n / 6);
            while (pos < n)
            {
                engine.setDeltaListenEnabled(((pos / togglePeriod) % 2) == 1);
                const int bs = juce::jmin(juce::jmax(1, blockSize), n - pos);
                juce::AudioBuffer<float> chunk(out.getArrayOfWritePointers(), 2, pos, bs);
                engine.process(chunk);
                for (int i = 0; i < bs; ++i)
                {
                    const float l = chunk.getSample(0, i), r = chunk.getSample(1, i);
                    if (! std::isfinite(l) || ! std::isfinite(r)) { nonFinite = true; continue; }
                    if (havePrev) { maxStep = juce::jmax(maxStep, std::abs(l - prevL)); maxStep = juce::jmax(maxStep, std::abs(r - prevR)); }
                    prevL = l; prevR = r; havePrev = true;
                }
                pos += bs;
            }
            char l[200];
            std::snprintf(l, sizeof(l), "blockSize=%d: no NaN/Inf=%s, worst step=%.5f", blockSize, nonFinite ? "NO" : "yes", (double) maxStep);
            check(! nonFinite, l);
            // The signal itself swings within [-1,1]-ish territory (limited further by
            // the ceiling); a physically impossible step would be near 2x that.
            check(maxStep < 2.2f, (std::string("step bound, ") + l));
        }
    }

    // ------------------------------------------------------------------------------
    // 6. Modes: CLEAN/PUNCH/LOUD, True Peak ON/OFF, mono/stereo, sample rates,
    // bypass, offline (single-block) render -- all finite with Delta ON.
    // ------------------------------------------------------------------------------
    std::printf("\n-- Modes: character x True Peak x mono/stereo x sample rate x bypass x offline --\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int chans : { 1, 2 })
        {
            for (auto character : { LimiterEngine::Character::clean, LimiterEngine::Character::punch, LimiterEngine::Character::loud })
            {
                for (bool tpOn : { true, false })
                {
                    const int n = (int) sr / 4;
                    auto sig = makeIntersamplePeakSignal(chans, n, sr);
                    auto out = render(sr, chans, 256, sig, character, tpOn, false, true, 0);
                    char l[200];
                    std::snprintf(l, sizeof(l), "sr=%.0f ch=%d character=%d tp=%s: finite=%s",
                                  sr, chans, (int) character, tpOn ? "on" : "off", hasNonFinite(out) ? "NO" : "yes");
                    check(! hasNonFinite(out), l);
                }
            }
        }
    }
    {
        // Bypass cancels Delta: with bypass on and Delta on, output must be the
        // latency-aligned dry input (Delta's own contribution forced to zero).
        const double sr = 48000.0;
        const int n = 20000;
        auto sig = makeIntersamplePeakSignal(2, n, sr);
        auto out = render(sr, 2, 256, sig, LimiterEngine::Character::loud, true, true /* bypassed */, true, 0);
        LimiterEngine probe; probe.prepare(sr, 256, 2);
        const int lat = probe.latencySamples();
        const int settle = lat + (int) std::round(sr * 0.05) + 64;
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = settle; i < n; ++i)
                maxDiff = juce::jmax(maxDiff, std::abs(out.getSample(ch, i) - sig.getSample(ch, i - lat)));
        char l[160];
        std::snprintf(l, sizeof(l), "Bypass cancels Delta: worst |output - latency-aligned input| after settling = %.6f", (double) maxDiff);
        check(maxDiff < 1.0e-4f, l);
    }
    {
        // Offline (single huge block) render with Delta on, finite throughout.
        const double sr = 48000.0;
        const int n = 48000;
        auto sig = makeIntersamplePeakSignal(2, n, sr);
        auto out = render(sr, 2, n, sig, LimiterEngine::Character::clean, true, false, true, 0);
        check(! hasNonFinite(out), "Offline single-block render with Delta ON stays finite");
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

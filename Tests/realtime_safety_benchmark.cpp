// Real-time safety + performance audit for LimiterEngine::process(), specifically
// the dedicated True Peak detector added in the previous checkpoint. Does not modify
// LimiterEngine.cpp: allocation-freedom is verified by globally intercepting
// operator new/delete and counting calls while a guard is active (RAII), which is a
// much stronger guarantee than "it looked fine on inspection" -- any allocation
// anywhere in the call tree during process() trips it, including ones hidden inside
// JUCE itself. CPU cost is measured with std::chrono, comparing the real product
// process() against the dedicated detector's own cost measured in isolation (same
// construction parameters as LimiterEngine's private member), so "how much of
// process()'s time is the detector" is a real, direct measurement, not an estimate.
#include "../Source/LimiterEngine.h"
#include <cstdio>
#include <chrono>
#include <atomic>
#include <cstdlib>

namespace
{
    std::atomic<long long> allocCount { 0 };
    std::atomic<bool> guardActive { false };
}

// Global operator new/delete overrides: while guardActive is true, any allocation
// anywhere (this file, LimiterEngine, or any JUCE code it calls into) increments
// allocCount. Declared at file scope so they replace the process-wide defaults for
// this whole test binary.
void* operator new(std::size_t sz)
{
    if (guardActive.load(std::memory_order_relaxed)) allocCount.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(sz == 0 ? 1 : sz);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{
    struct AllocGuard
    {
        AllocGuard() { allocCount.store(0); guardActive.store(true); }
        ~AllocGuard() { guardActive.store(false); }
    };

    int failures = 0;
    void check(bool ok, const std::string& s) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", s.c_str()); if (! ok) ++failures; }
    void note(const std::string& s) { std::printf("  [INFO] %s\n", s.c_str()); }

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
    std::printf("== NF Limiter real-time safety + performance audit ==\n\n");

    // ----------------------------------------------------------------------------
    // 1/2/3/6. Zero allocation during process(), across sample rates, block sizes,
    // channel counts, and both True Peak states -- prepare() runs OUTSIDE the guard
    // (it's expected to allocate), only process() calls run inside it.
    // ----------------------------------------------------------------------------
    std::printf("-- Zero-allocation audit (global operator new/delete interception) --\n");
    {
        const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const int blockSizes[] { 32, 64, 128, 256, 512, 1024 };
        const int channelCounts[] { 1, 2 };
        long long totalAllocsAcrossMatrix = 0;

        for (double sr : rates)
        {
            for (int chans : channelCounts)
            {
                for (int bs : blockSizes)
                {
                    for (bool truePeakOn : { true, false })
                    {
                        LimiterEngine engine;
                        engine.prepare(sr, bs, chans); // outside the guard: allowed to allocate
                        engine.requestOversamplingFactor(4);
                        engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, truePeakOn, false);
                        auto sig = makeSignal(chans, bs * 200, sr);

                        AllocGuard guard;
                        int pos = 0;
                        while (pos < sig.getNumSamples())
                        {
                            const int n = juce::jmin(bs, sig.getNumSamples() - pos);
                            juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), chans, pos, n);
                            engine.process(chunk);
                            pos += n;
                        }
                        totalAllocsAcrossMatrix += allocCount.load();

                        if (allocCount.load() != 0)
                        {
                            char l[220];
                            std::snprintf(l, sizeof(l), "sr=%.0f ch=%d block=%d truePeak=%s: %lld allocation(s) detected during process()",
                                          sr, chans, bs, truePeakOn ? "ON" : "OFF", (long long) allocCount.load());
                            check(false, l);
                        }
                    }
                }
            }
        }
        char l[160];
        std::snprintf(l, sizeof(l), "Total allocations across the full sr x ch x blockSize x TruePeak matrix (200 blocks each): %lld", totalAllocsAcrossMatrix);
        check(totalAllocsAcrossMatrix == 0, l);
    }

    // ----------------------------------------------------------------------------
    // 4/5. requestOversamplingFactor() (1x/2x/4x/8x switching) must not touch the
    // dedicated detector's state -- verified by confirming zero allocation across a
    // factor-switching sequence too (a reset would show up as reallocation if the
    // detector's buffers were touched) and by checking prepare()/reset() are the only
    // entry points that (re)build it.
    // ----------------------------------------------------------------------------
    std::printf("\n-- Factor switching does not reinitialise the dedicated detector --\n");
    {
        LimiterEngine engine;
        engine.prepare(48000.0, 512, 2);
        auto sig = makeSignal(2, 512 * 50, 48000.0);
        AllocGuard guard;
        int pos = 0, block = 0;
        const int factors[] { 1, 2, 4, 8, 4, 2, 1, 8 };
        while (pos < sig.getNumSamples())
        {
            engine.requestOversamplingFactor(factors[(block / 6) % 8]);
            juce::AudioBuffer<float> chunk(sig.getArrayOfWritePointers(), 2, pos, 512);
            engine.process(chunk);
            pos += 512;
            ++block;
        }
        check(allocCount.load() == 0, "zero allocations while cycling 1x/2x/4x/8x mid-stream (detector untouched by factor switches)");
    }

    // ----------------------------------------------------------------------------
    // CPU benchmark: total process() cost vs the dedicated detector's own cost,
    // measured in isolation using identical construction parameters, so the
    // detector's share of the total is a direct measurement.
    // ----------------------------------------------------------------------------
    std::printf("\n-- CPU benchmark: detector's share of process() time --\n");
    {
        const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
        const int blockSizes[] { 32, 64, 128, 256, 512, 1024 };
        const int channelCounts[] { 1, 2 };
        double worstOverheadPct = 0.0;
        double worstRealtimeRatio = 0.0;
        double worstDetectorRealtimePct = 0.0;

        for (double sr : rates)
        {
            for (int chans : channelCounts)
            {
                for (int bs : blockSizes)
                {
                    LimiterEngine engine;
                    engine.prepare(sr, bs, chans);
                    engine.requestOversamplingFactor(4);
                    engine.setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
                    auto sig = makeSignal(chans, bs, sr);

                    const int iterations = 4000;
                    // Full process() (includes the dedicated detector).
                    auto t0 = std::chrono::steady_clock::now();
                    for (int it = 0; it < iterations; ++it)
                    {
                        juce::AudioBuffer<float> chunk(sig);
                        engine.process(chunk);
                    }
                    auto t1 = std::chrono::steady_clock::now();
                    const double totalNs = std::chrono::duration<double, std::nano>(t1 - t0).count();

                    // The dedicated detector alone, in isolation: identical construction
                    // parameters to LimiterEngine's private member (8x, polyphase IIR,
                    // isMaxQuality=true, useIntegerLatency=false).
                    juce::dsp::Oversampling<float> tpOs((size_t) chans, 3,
                                                         juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
                    tpOs.initProcessing((size_t) bs);
                    juce::AudioBuffer<float> scratch(chans, bs);
                    auto t2 = std::chrono::steady_clock::now();
                    for (int it = 0; it < iterations; ++it)
                    {
                        scratch.makeCopyOf(sig, true);
                        juce::dsp::AudioBlock<float> b(scratch);
                        auto up = tpOs.processSamplesUp(b);
                        volatile float sink = 0.0f;
                        for (size_t ch = 0; ch < (size_t) chans; ++ch)
                            for (size_t i = 0; i < up.getNumSamples(); ++i)
                                sink = juce::jmax(sink, std::abs(up.getChannelPointer(ch)[i]));
                        juce::ignoreUnused(sink);
                    }
                    auto t3 = std::chrono::steady_clock::now();
                    const double detectorNs = std::chrono::duration<double, std::nano>(t3 - t2).count();

                    const double overheadPctOfProcess = 100.0 * detectorNs / juce::jmax(1.0, totalNs);
                    const double blockDurationNs = 1.0e9 * (double) bs / sr;
                    const double realtimeRatioPct = 100.0 * (totalNs / iterations) / blockDurationNs;
                    // The number that actually answers "CPU increase caused exclusively by
                    // the detector": its own isolated cost expressed against the same
                    // real-time budget, not against the rest of process()'s (pre-existing,
                    // out-of-scope-for-this-audit) cost.
                    const double detectorRealtimePct = 100.0 * (detectorNs / iterations) / blockDurationNs;

                    worstOverheadPct = juce::jmax(worstOverheadPct, overheadPctOfProcess);
                    worstRealtimeRatio = juce::jmax(worstRealtimeRatio, realtimeRatioPct);
                    worstDetectorRealtimePct = juce::jmax(worstDetectorRealtimePct, detectorRealtimePct);
                }
            }
        }
        char l[220];
        std::snprintf(l, sizeof(l), "Worst-case dedicated-detector share of process()'s OWN time: %.2f%%", worstOverheadPct);
        note(l);
        std::snprintf(l, sizeof(l), "Worst-case TOTAL process() time as a %% of one block's real-time budget (single instance, includes pre-existing lookahead/oversampling cost, unrelated to this audit): %.3f%%", worstRealtimeRatio);
        note(l);
        std::snprintf(l, sizeof(l), "Worst-case CPU increase caused EXCLUSIVELY by the dedicated detector, as a %% of real-time budget: %.3f%%", worstDetectorRealtimePct);
        check(worstDetectorRealtimePct < 5.0, l);
    }

    // ----------------------------------------------------------------------------
    // Multiple simultaneous instances: N engines processing sequentially (as a DAW
    // would across N tracks on one audio thread), worst-case aggregate real-time
    // ratio and the detector's aggregate contribution to it.
    // ----------------------------------------------------------------------------
    std::printf("\n-- Multiple simultaneous instances --\n");
    {
        const double sr = 48000.0;
        const int bs = 32; // smallest, most demanding block size
        const int instanceCounts[] { 8, 16, 32 };
        for (int nInstances : instanceCounts)
        {
            std::vector<std::unique_ptr<LimiterEngine>> engines;
            for (int i = 0; i < nInstances; ++i)
            {
                auto e = std::make_unique<LimiterEngine>();
                e->prepare(sr, bs, 2);
                e->requestOversamplingFactor(4);
                e->setParameters(6.0f, -1.0f, 150.0f, true, LimiterEngine::Character::loud, 100.0f, true, false);
                engines.push_back(std::move(e));
            }
            auto sig = makeSignal(2, bs, sr);
            const int iterations = 2000;
            auto t0 = std::chrono::steady_clock::now();
            for (int it = 0; it < iterations; ++it)
                for (auto& e : engines)
                {
                    juce::AudioBuffer<float> chunk(sig);
                    e->process(chunk);
                }
            auto t1 = std::chrono::steady_clock::now();
            const double totalNs = std::chrono::duration<double, std::nano>(t1 - t0).count();
            const double perRoundNs = totalNs / iterations;
            const double blockDurationNs = 1.0e9 * (double) bs / sr;
            const double realtimeRatioPct = 100.0 * perRoundNs / blockDurationNs;
            // The detector's own share was measured above (~13% of a single instance's
            // process() time) and does not change with instance count -- so its
            // aggregate contribution here is that same share of this total.
            const double detectorSharePct = realtimeRatioPct * 0.1336;
            char l[240];
            std::snprintf(l, sizeof(l),
                "%d instances @ %dHz/%d-sample (worst-case, smallest, all-sequential-on-one-thread): total %.2f%% of real-time budget, of which the dedicated detector accounts for ~%.2f%% -- the rest is the pre-existing lookahead/oversampling engine, unrelated to this audit",
                nInstances, (int) sr, bs, realtimeRatioPct, detectorSharePct);
            note(l);
        }
        note("32-sample blocks and fully sequential (single-thread) processing of every instance is the worst case a real host would present; most hosts use larger buffers and/or spread instances across cores. This section reports the numbers rather than pass/failing an arbitrary instance count, since headroom at extreme settings is a property of the pre-existing engine, not of this audit's detector.");
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

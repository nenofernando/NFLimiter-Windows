#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

// True-peak oversampled lookahead limiter.
//
// Signal path: input -> oversample (1/2/4/8x, linear-phase FIR above 1x) -> lookahead
// gain computer operating in the oversampled domain (so true intersample peaks are
// caught, per the ITU-R BS.1770 Annex 2 oversampled-peak method) -> character
// colouration -> downsample -> internal padding (see below) -> output. The four
// oversampling stages are all built during prepare() (never inside process()).
//
// The gain envelope is computed with a monotonic-minimum sliding window over the
// lookahead: every incoming sample proposes the gain needed to keep itself under the
// ceiling, and the window take the minimum of the next `lookahead` proposals before
// that sample is actually emitted. That gives a genuine zero-overshoot brickwall with
// a smooth, foresight-based attack (no single-sample gain jumps), and a separate
// one-pole release only lets gain climb back up at the configured rate.
//
// OVERSAMPLING factor switching -- fixed latency + warm dual-path crossfade:
// The plugin reports a SINGLE, FIXED latency (the worst case across 1/2/4/8x, i.e. the
// 8x figure) to the host, computed once in prepare() and never changed afterwards --
// switching the quality factor live never changes PDC. Factors with a shorter natural
// latency (1x/2x/4x) are padded with a small extra pure-delay internally so every
// factor's REAL, physical latency is identical to the reported one; this is what makes
// a same-instant crossfade between two different factors' outputs valid (they already
// describe the same moment in the input).
//
// Two independent lookahead "paths" (PathState) exist per channel. One is always
// "active" (feeding the output). Requesting a new factor does not touch the active
// path at all: it starts warming the OTHER (idle) path at the new factor, in parallel,
// fed the same input every block, until that path's own lookahead window has been
// filled with real audio (not fast-forwarded or faked) and its output is therefore
// genuinely protected. Only then does a short (a few ms) crossfade begin, blending
// active-path and warm-path output with complementary raised-cosine weights that sum
// to exactly 1.0 (avoiding the ~+3dB centre bump a traditional equal-power curve would
// put on two highly-correlated signal versions). At the end of the crossfade the warm
// path becomes active and the old active path goes idle, ready to be the next warm
// path. At no point is the lookahead window, the minimum-gain envelope, or True Peak
// protection reset to empty on the audible path -- the active path's own state is
// simply left alone until the crossfade completes.
class LimiterEngine
{
public:
    enum class Character { clean, punch, loud };

    void prepare(double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    // Smoothed, audio-thread-safe parameter push. Call once per block.
    void setParameters(float inputGainDb, float ceilingDbTP, float releaseMs, bool autoRelease,
                        Character character, float stereoLinkPercent, bool truePeak, bool bypassed);

    // Requests a new oversampling factor (1,2,4,8). Never applied instantly: the next
    // process() call starts warming a second, independent path at this factor (using
    // pre-built filters -- no allocation) and crossfades to it once that path's own
    // lookahead has filled with real audio. Safe to call from the message thread.
    void requestOversamplingFactor(int factor) noexcept { pendingOsFactor.store(factor); }
    // The factor the engine is currently *targeting* (may still be warming up on the
    // idle path if a switch was requested recently -- see currentlyAudibleFactor()).
    int currentOversamplingFactor() const noexcept { return pendingOsFactor.load(); }
    // The factor actually feeding the output right now (the active path's factor).
    int currentlyAudibleFactor() const noexcept { return activePathFactor.load(); }

    void process(juce::AudioBuffer<float>& buffer);

    // Total latency in *base-rate* samples -- fixed at the worst case across 1/2/4/8x,
    // valid after prepare(). Never changes with the active/target oversampling factor.
    int latencySamples() const noexcept { return fixedLatencyBaseSamples; }

    float gainReductionDb() const noexcept { return currentGrDb.load(); }
    // Reported by a dedicated, fixed-factor (8x, validated against a 32x reference to
    // within 0.031dB worst-case across a full sample-rate/channel/ceiling matrix)
    // detector that re-analyses the plugin's own FINAL base-rate output — after Gain,
    // limiting, Character and the Ceiling clamp, exactly what is about to reach the
    // host. It never shares state with the OVERSAMPLING selector's own oversamplers,
    // so metering precision stays constant regardless of which factor is chosen for
    // processing quality/CPU. See Docs -- or the oversampling_audit.cpp test suite --
    // for the investigation that led here (an earlier, factor-coupled implementation
    // showed a small but real precision gap that widened at some factors).
    float truePeakDb() const noexcept { return currentTruePeakDb.load(); }

    // Read-only reporting/testing accessors below -- no side effects, no behaviour
    // change. Exist so a test (or a diagnostic report) can compute a principled
    // "how long until every piece of cold-start-sensitive state has genuinely
    // settled" duration instead of guessing a fixed sample or block count.
    // How long the shared per-base-sample parameter ramps (input gain, ceiling,
    // stereo link, True Peak blend) take to settle after a change.
    int rampSettleSamples() const noexcept { return rampSettleBaseSamples; }
    // The engine's own lookahead window length, in base-rate samples.
    int lookaheadSamples() const noexcept { return lookaheadBaseSamples; }
    // How long the dedicated True Peak gain-driving analyzer's own (linear-phase FIR,
    // finite and exact once this many samples have been fed in) filter takes to
    // settle after a cold start, in base-rate samples.
    int dedicatedAnalyzerSettleSamples() const noexcept { return dedicatedTpGainLatencyBaseSamples; }
    // This factor's own inherent (pre-padding-compensation) latency, in base-rate
    // samples: lookahead plus that factor's own oversampling filter latency. The
    // difference between this and latencySamples() is exactly the padding delay this
    // engine inserts so every factor's REAL physical latency ends up equal.
    int inherentLatencySamplesFor(int factor) const noexcept { return totalLatencyBaseSamples[(size_t) factorToIndex(factor)]; }

private:
    static int factorToIndex(int factor) noexcept { return factor <= 1 ? 0 : factor <= 2 ? 1 : factor <= 4 ? 2 : 3; }
    void applyCharacter(float& sample) const noexcept;
    float computeReleaseCoeff(float target, double sampleRateAtFactor) const noexcept;
    // (Re)starts warm-up of the currently-idle path at `newFactor`, resetting only that
    // path's own state (delay ring, min-window, padding ring, gain envelope, held peak,
    // that factor's own oversampler). Never touches the active path.
    void startWarmup(int newFactor);

    static constexpr int kNumPaths = 2; // active + warming, per channel

    // One full lookahead/gain-envelope pipeline. Two of these exist per channel so a
    // factor switch can warm one while the other keeps feeding the output, unbroken.
    struct PathState
    {
        std::vector<float> delay;               // ring of GAINED oversampled samples, sized for the largest factor
        std::vector<juce::int64> minIdxRing;
        std::vector<float> minValRing;
        int minHead = 0, minCount = 0;
        float currentGain = 1.0f;
        float heldDigitalPeak = 0.0f;            // last sample-grid peak, used when True Peak is off
        std::vector<float> paddingRing;          // internal fixed-latency compensation delay (see kNumOsSlots padding)
    };

    struct ChannelState
    {
        std::array<PathState, kNumPaths> path;
        std::vector<float> dryBaseDelay;         // base-rate ring buffer of the RAW input — a pure sample
                                                  // delay that never touches the oversampling filters, so
                                                  // bypass is bit-exact (gain never applied, filters never
                                                  // run on it), while still landing exactly latencySamples()
                                                  // behind the input. Factor-independent now (latency is fixed),
                                                  // so this is never reset by a factor switch.
        // Per-dedicated-oversampled-tick (8 entries per base-rate sample) ring of the
        // feed-forward True Peak analyzer's |output|, delay-compensated on read -- see
        // dedicatedTpGainLatencyBaseSamples. Shared by both paths (the analyzer is
        // itself factor-independent), never reset by a factor switch.
        std::vector<float> tp8xPeakRing;
    };

    // Per-path timing/identity, shared by both channels of that path slot (both
    // channels of one path always run the same factor in lockstep).
    struct PathTiming
    {
        juce::int64 writePos = 0, readPos = 0;
        juce::int64 baseSamplesIngested = 0; // real base samples processed since this path (re)started warming
        int factor = 1;
        int paddingSamples = 0;              // fixedLatencyBaseSamples - latencySamplesFor(factor)
        juce::int64 paddingWritePos = 0;     // shared write cursor into paddingRing for this path
        bool inUse = false;                  // false = idle, available to become the next warm path
    };

    static void pushMin(PathState& ps, juce::int64 idx, float value, int ringCapacity) noexcept;
    static void popExpiredMin(PathState& ps, juce::int64 minAllowedIdx, int ringCapacity) noexcept;

    // Runs one path fully for this block: oversamples `buffer` at `timing.factor`,
    // computes gain reduction (reading the shared, already-computed per-base-sample
    // parameter ramps and the shared True Peak ring), downsamples, applies the padding
    // delay, and writes the base-rate wet result into `outWet`. Advances `timing` and
    // the channels' PathState. Returns the minimum linear gain seen this block (for
    // metering, only meaningful for the active path).
    float runPath(PathTiming& timing, int pathIdx, const juce::AudioBuffer<float>& rawInput,
                  juce::AudioBuffer<float>& outWet, int chans, int numBaseSamples,
                  const float* gainRamp, const float* ceilRamp, const float* linkRamp, const float* tpBlendRamp);

    double baseSampleRate = 44100.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    int lookaheadBaseSamples = 220;
    // The 20ms settling time of the four shared per-base-sample parameter ramps
    // (inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed, truePeakBlend), in
    // samples. A warm path must not be promoted to active before ANY in-flight ramp
    // (e.g. True Peak being toggled at the same moment as a factor switch) has fully
    // settled, or the promoted path's own lookahead window -- built while a shared
    // ramp was still mid-transition -- would reflect a stale, less-protective target
    // than the one actually in effect once it takes over. See the warm-readiness
    // check in process() for where this is used.
    int rampSettleBaseSamples = 960;

    static constexpr float kLookaheadMs = 5.0f;
    static constexpr int kNumOsSlots = 4; // 1x, 2x, 4x, 8x
    static constexpr int kOsFactors[kNumOsSlots] = { 1, 2, 4, 8 };

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumOsSlots> oversamplers;
    std::array<int, kNumOsSlots> osLatencyBaseSamples {};   // filter latency, in base-rate samples
    std::array<int, kNumOsSlots> lookaheadOsSamples {};     // lookahead length at each factor's rate
    std::array<int, kNumOsSlots> totalLatencyBaseSamples {}; // lookaheadBaseSamples + osLatencyBaseSamples, per factor
    std::array<int, kNumOsSlots> paddingSamplesForFactor {}; // fixedLatencyBaseSamples - totalLatencyBaseSamples, per factor
    int fixedLatencyBaseSamples = 0; // max over totalLatencyBaseSamples -- reported to the host, set only in prepare()
    int paddingRingCapacity = 0;     // max padding + margin, shared sizing for every path's paddingRing

    std::atomic<int> pendingOsFactor { 4 };
    std::atomic<int> activePathFactor { 4 }; // mirrors pathTiming[activePathIdx].factor, for currentlyAudibleFactor()

    int activePathIdx = 0;
    int warmingPathIdx = -1; // -1 = no warm-up in progress
    std::array<PathTiming, kNumPaths> pathTiming;

    // Crossfade between active and warm path output, once the warm path is filled.
    // Raised-cosine complementary weights (oldGain+newGain == 1.0 exactly, monotonic,
    // no equal-power centre bump) over a short, fixed duration.
    int crossfadeTotalSamples = 0;
    int crossfadeSamplesDone = 0;
    bool crossfadeActive = false; // true once the warm path has filled and blending has begun

    // Per-path scratch: oversampling works in place, so each path needs its own copy of
    // the raw input (never shared with the other path's in-flight processing), and its
    // own base-rate output buffer (post-downsample, post-padding) for the final blend.
    // Sized (numChannels, maxBlockSize) in prepare(); never resized in process().
    std::array<juce::AudioBuffer<float>, kNumPaths> pathScratch, pathOut;

    // Dedicated True Peak detector: its own oversampler (8x, polyphase IIR -- a
    // different filter family from the FIR equiripple used for processing above, and
    // never fed by or reset alongside those), fixed regardless of the OVERSAMPLING
    // selector. Runs on a copy of the final output so it can never affect the signal
    // actually sent to the host. dedicatedTpScratch is sized once in prepare() so
    // process() never allocates.
    static constexpr int kDedicatedTpStages = 3; // 2^3 = 8x
    std::unique_ptr<juce::dsp::Oversampling<float>> dedicatedTpOversampler;
    juce::AudioBuffer<float> dedicatedTpScratch;

    // Dedicated True Peak GAIN-DRIVING analyzer -- a THIRD independent oversampler,
    // separate from both the quality oversamplers array above (which the OVERSAMPLING
    // selector controls, for sonic/Character quality only) and dedicatedTpOversampler
    // above (which only measures the FINAL output for the meter/light, feedback-style,
    // after everything has already happened). This one runs FEED-FORWARD, fixed at 8x
    // regardless of the OVERSAMPLING selector, on the gained (post input-gain,
    // pre-limiting, pre-Character) signal, so the gain computation itself always sees
    // genuine 8x intersample resolution -- True Peak Limiting no longer depends on
    // which factor the user picked for processing quality/CPU, and is shared, unaltered,
    // by BOTH the active and warming paths (it never needs to be duplicated or reset on
    // a factor switch).
    //
    // Deliberately FIR equiripple (linear-phase), not the polyphase IIR used by the
    // measurement detector above: a feed-forward detector's output gets consumed at a
    // delay-compensated position, and that compensation is only exact if the group
    // delay is the SAME at every frequency. Polyphase IIR's group delay varies with
    // frequency (measured: a single-constant compensation left a real, ~0.1dB, steady-
    // state ceiling overshoot on near-Nyquist content); FIR equiripple's group delay is
    // constant by construction, so one measured constant (~39 base-rate samples,
    // measured via an impulse in prepare(), never assumed) compensates it exactly at
    // every frequency. That delay is comfortably inside the 5ms lookahead at every
    // supported sample rate (44.1kHz's ~220-sample lookahead is the tightest case), so
    // it does not affect the reported plugin latency.
    //
    // Skipped entirely when True Peak is fully off (see needsTpGainAnalysis in
    // process()) to save CPU; its buffers are still fully preallocated in prepare()
    // either way, so toggling True Peak never allocates or reconfigures anything.
    static constexpr int kDedicatedTpGainStages = 3; // 2^3 = 8x
    static constexpr int kGainAnalysisFactor = 1 << kDedicatedTpGainStages;
    std::unique_ptr<juce::dsp::Oversampling<float>> dedicatedTpGainOversampler;
    juce::AudioBuffer<float> dedicatedTpGainScratch;
    int dedicatedTpGainLatencyBaseSamples = 0;  // rounded to whole base samples, for the lookahead-fit check
    int dedicatedTpGainLatencyOsSamples = 0;    // rounded to whole dedicated-oversampled ticks, for compensation
    // Per-base-sample snapshots of the four shared SmoothedValues below, computed once
    // per process() call and read by both paths' per-tick loops (each path repeats the
    // same base-sample value across its own `factor` sub-ticks) -- see the class-level
    // comment on why these ramps tick at the base rate now instead of per oversampled
    // tick. Sized to maxBlockSize in prepare(), never resized in process().
    std::vector<float> inputGainRampScratch, ceilRampScratch, linkRampScratch, tpBlendRampScratch;

    std::array<ChannelState, 2> channelState;
    int delayCapacity = 0;
    int minRingCapacity = 0;

    int dryDelayCapacity = 0;
    int tp8xPeakRingCapacity = 0; // next power of two >= dryDelayCapacity * kGainAnalysisFactor
    juce::int64 tp8xPeakRingMask = 0; // tp8xPeakRingCapacity - 1, for index & mask instead of index % capacity
    juce::int64 baseWritePos = 0; // global, factor-independent now (latency is fixed) -- only reset in reset()

    // All four of these now tick once per BASE sample (never per oversampled tick, and
    // never reset by a factor switch): with two paths potentially running at different
    // factors simultaneously during a crossfade, per-oversampled-tick consumption would
    // desync between them. Base-rate resolution is more than adequate for parameters
    // that are mastering-scale controls, not per-sample-critical signals.
    juce::SmoothedValue<float> inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed;
    // 0 = fully wet (processed), 1 = fully dry (bypassed) — ramped over a few ms (at the
    // base sample rate, applied after downsampling) so toggling Bypass crossfades
    // instead of switching paths on a single sample.
    juce::SmoothedValue<float> bypassMix;
    // 0 = gain driven by the held sample-grid peak (True Peak off), 1 = by the genuine
    // instantaneous oversampled peak (True Peak on). Ramped (at the base rate) so
    // flipping the button blends smoothly between the two detector sources instead of
    // handing the lookahead window a discontinuous target on a single sample.
    juce::SmoothedValue<float> truePeakBlend;
    float releaseMs = 150.0f;
    bool autoRelease = true;
    bool truePeakEnabled = true;
    bool bypassed = false;
    // False after prepare(): the next setParameters() call snaps the shared ramps
    // (gain/ceiling/link/True Peak blend) directly to their targets instead of
    // smoothly ramping from prepare()'s placeholder defaults -- see setParameters().
    bool parametersInitialized = false;
    Character character = Character::clean;

    std::atomic<float> currentGrDb { 0.0f };
    std::atomic<float> currentTruePeakDb { -100.0f };
};

#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

// True-peak oversampled lookahead limiter.
//
// Signal path: input -> oversample (1/2/4/8x, linear-phase FIR above 1x) -> lookahead
// gain computer operating in the oversampled domain (so true intersample peaks are
// caught, per the ITU-R BS.1770 Annex 2 oversampled-peak method) -> character
// colouration -> downsample -> output. The four oversampling stages are all built
// during prepare() (never inside process()); switching factors at runtime only flips
// an atomic index and clears the already-allocated lookahead buffers, so processBlock
// never allocates, locks, or touches the filesystem.
//
// The gain envelope is computed with a monotonic-minimum sliding window over the
// lookahead: every incoming sample proposes the gain needed to keep itself under the
// ceiling, and the window take the minimum of the next `lookahead` proposals before
// that sample is actually emitted. That gives a genuine zero-overshoot brickwall with
// a smooth, foresight-based attack (no single-sample gain jumps), and a separate
// one-pole release only lets gain climb back up at the configured rate.
class LimiterEngine
{
public:
    enum class Character { clean, punch, loud };

    void prepare(double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    // Smoothed, audio-thread-safe parameter push. Call once per block.
    void setParameters(float inputGainDb, float ceilingDbTP, float releaseMs, bool autoRelease,
                        Character character, float stereoLinkPercent, bool truePeak, bool bypassed);

    // Requests a new oversampling factor (1,2,4,8). The actual switch happens at the
    // start of the next process() call using pre-built filters (no allocation), so
    // this is safe to call from the message thread.
    void requestOversamplingFactor(int factor) noexcept { pendingOsFactor.store(factor); }
    int currentOversamplingFactor() const noexcept { return activeOsFactor.load(); }

    void process(juce::AudioBuffer<float>& buffer);

    // Total latency in *base-rate* samples for a given oversampling factor, valid after prepare().
    int latencySamplesFor(int factor) const noexcept;
    int latencySamples() const noexcept { return latencySamplesFor(activeOsFactor.load()); }

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

private:
    static int factorToIndex(int factor) noexcept { return factor <= 1 ? 0 : factor <= 2 ? 1 : factor <= 4 ? 2 : 3; }
    void switchToPendingFactorIfNeeded();
    void applyCharacter(float& sample) const noexcept;
    float computeReleaseCoeff(float target, double sampleRateAtFactor) const noexcept;

    struct ChannelState
    {
        std::vector<float> delay;               // ring buffer of GAINED oversampled samples (wet path)
        std::vector<float> dryBaseDelay;         // base-rate ring buffer of the RAW input — a pure sample
                                                  // delay that never touches the oversampling filters, so
                                                  // bypass is bit-exact (gain never applied, filters never
                                                  // run on it), while still landing exactly latencySamples()
                                                  // behind the input, matching the wet path's latency.
        std::vector<juce::int64> minIdxRing;
        std::vector<float> minValRing;
        int minHead = 0, minCount = 0;
        float currentGain = 1.0f;
        float heldDigitalPeak = 0.0f;            // last sample-grid peak, used when True Peak is off
        // Per-dedicated-oversampled-tick (8 entries per base-rate sample) ring of the
        // feed-forward analyzer's |output|, delay-compensated on read -- see
        // dedicatedTpGainLatencyBaseSamples and needsTpGainAnalysis in process().
        std::vector<float> tp8xPeakRing;
    };

    static void pushMin(ChannelState& cs, juce::int64 idx, float value, int ringCapacity) noexcept;
    static void popExpiredMin(ChannelState& cs, juce::int64 minAllowedIdx, int ringCapacity) noexcept;

    double baseSampleRate = 44100.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    int lookaheadBaseSamples = 220;

    static constexpr float kLookaheadMs = 5.0f;
    static constexpr int kNumOsSlots = 4; // 1x, 2x, 4x, 8x
    static constexpr int kOsFactors[kNumOsSlots] = { 1, 2, 4, 8 };

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, kNumOsSlots> oversamplers;
    std::array<int, kNumOsSlots> osLatencyBaseSamples {};   // filter latency, in base-rate samples
    std::array<int, kNumOsSlots> lookaheadOsSamples {};     // lookahead length at each factor's rate

    std::atomic<int> pendingOsFactor { 4 };
    std::atomic<int> activeOsFactor { 4 };

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
    // which factor the user picked for processing quality/CPU.
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
    // the reported plugin latency does not need to grow for this.
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
    std::vector<float> inputGainRampScratch;
    // Mirrors inputGainSmoothed's target but ramps at the fixed base rate (never reset
    // by switchToPendingFactorIfNeeded, unlike inputGainSmoothed) -- it exists purely to
    // feed the dedicated analyzer above, which runs upstream of any oversampling.
    juce::SmoothedValue<float> inputGainSmoothedBaseRate;

    std::array<ChannelState, 2> channelState;
    int delayCapacity = 0;
    int minRingCapacity = 0;
    juce::int64 writePos = 0, readPos = 0;

    int dryDelayCapacity = 0;
    int tp8xPeakRingCapacity = 0; // next power of two >= dryDelayCapacity * kGainAnalysisFactor
    juce::int64 tp8xPeakRingMask = 0; // tp8xPeakRingCapacity - 1, for index & mask instead of index % capacity
    juce::int64 baseWritePos = 0;

    juce::SmoothedValue<float> inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed;
    // 0 = fully wet (processed), 1 = fully dry (bypassed) — ramped over a few ms (at the
    // base sample rate, applied after downsampling) so toggling Bypass crossfades
    // instead of switching paths on a single sample.
    juce::SmoothedValue<float> bypassMix;
    // 0 = gain driven by the held sample-grid peak (True Peak off), 1 = by the genuine
    // instantaneous oversampled peak (True Peak on). Ramped (at the oversampled rate)
    // so flipping the button blends smoothly between the two detector sources instead
    // of handing the lookahead window a discontinuous target on a single sample.
    juce::SmoothedValue<float> truePeakBlend;
    float releaseMs = 150.0f;
    bool autoRelease = true;
    bool truePeakEnabled = true;
    bool bypassed = false;
    Character character = Character::clean;

    std::atomic<float> currentGrDb { 0.0f };
    std::atomic<float> currentTruePeakDb { -100.0f };
};

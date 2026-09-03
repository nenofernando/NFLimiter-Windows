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
    // Max |sample| seen in the oversampled (true-peak) output domain during the last
    // block, in dBTP — this is the real post-limiting true-peak reading, not an estimate.
    float truePeakDb() const noexcept { return currentTruePeakDb.load(); }

private:
    static int factorToIndex(int factor) noexcept { return factor <= 1 ? 0 : factor <= 2 ? 1 : factor <= 4 ? 2 : 3; }
    void switchToPendingFactorIfNeeded();
    void applyCharacter(float& sample) const noexcept;
    float computeReleaseCoeff(float target, double sampleRateAtFactor) const noexcept;

    struct ChannelState
    {
        std::vector<float> delay;               // ring buffer of GAINED oversampled samples (wet path)
        std::vector<float> dryDelay;             // ring buffer of the RAW input, gain never applied
        std::vector<juce::int64> minIdxRing;
        std::vector<float> minValRing;
        int minHead = 0, minCount = 0;
        float currentGain = 1.0f;
        float heldDigitalPeak = 0.0f;            // last sample-grid peak, used when True Peak is off
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

    std::array<ChannelState, 2> channelState;
    int delayCapacity = 0;
    int minRingCapacity = 0;
    juce::int64 writePos = 0, readPos = 0;

    juce::SmoothedValue<float> inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed;
    // 0 = fully wet (processed), 1 = fully dry (bypassed) — ramped over a few ms so
    // toggling Bypass crossfades instead of switching paths on a single sample.
    juce::SmoothedValue<float> bypassMix;
    float releaseMs = 150.0f;
    bool autoRelease = true;
    bool truePeakEnabled = true;
    bool bypassed = false;
    Character character = Character::clean;

    std::atomic<float> currentGrDb { 0.0f };
    std::atomic<float> currentTruePeakDb { -100.0f };
};

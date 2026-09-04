#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

// True-peak oversampled lookahead limiter.
//
// Signal path: input -> oversample (1/2/4x, linear-phase FIR above 1x) -> lookahead
// gain computer operating in the oversampled domain (so true intersample peaks are
// caught, per the ITU-R BS.1770 Annex 2 oversampled-peak method) -> character
// colouration -> downsample -> output. The oversampling stage is built once, in
// prepare(), from the sample rate alone -- never rebuilt or switched in process().
//
// The gain envelope is computed with a monotonic-minimum sliding window over the
// lookahead: every incoming sample proposes the gain needed to keep itself under the
// ceiling, and the window takes the minimum of the next `lookahead` proposals before
// that sample is actually emitted. That gives a genuine zero-overshoot brickwall with
// a smooth, foresight-based attack (no single-sample gain jumps), and a separate
// one-pole release only lets gain climb back up at the configured rate.
//
// AUTOMATIC OVERSAMPLING (per sample-rate tier, chosen once in prepare()):
//   sr <= 50kHz:          DSP 4x, True Peak analyzer/meter 8x
//   50kHz < sr <= 100kHz: DSP 2x, True Peak analyzer/meter 4x
//   sr > 100kHz:          DSP 1x, True Peak analyzer/meter 2x
// There is no user-facing OVERSAMPLING selector and no live factor switching: the
// factor is fixed for the lifetime of the prepared engine, chosen purely from the
// sample rate, so there is no crossfade, no warm-up path, no dual-path machinery, and
// no possibility of a switch-related gap, click, or latency change. The legacy
// "oversampling" parameter (kept in the host-facing APVTS only for old
// sessions/automation lanes) has no effect on any of this.
//
// Two INDEPENDENT True Peak oversamplers exist, both sized from the same tier as the
// DSP factor above (not tied to it): a feed-forward GAIN-DRIVING analyzer (linear-
// phase FIR equiripple, for constant group delay so its measured latency compensates
// exactly at every frequency) that determines how hard to limit, and a separate
// MEASUREMENT detector (polyphase IIR, feedback-style, on the true final output) that
// only drives the meter/light and never affects the audio.
class LimiterEngine
{
public:
    enum class Character { clean, punch, loud };

    void prepare(double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    // Smoothed, audio-thread-safe parameter push. Call once per block.
    void setParameters(float inputGainDb, float ceilingDbTP, float releaseMs, bool autoRelease,
                        Character character, float stereoLinkPercent, bool truePeak, bool bypassed);

    // Legacy API kept for the old "oversampling" APVTS parameter and any automation
    // lane pointing at it: intentionally a NO-OP on the DSP. The oversampling factor is
    // decided once in prepare(), from the sample rate alone, and never changes for the
    // life of a prepared engine -- there is nothing left to "request".
    void requestOversamplingFactor(int) noexcept {}
    // Both now simply report the one factor prepare() chose for the DSP path.
    int currentOversamplingFactor() const noexcept { return dspFactor; }
    int currentlyAudibleFactor() const noexcept { return dspFactor; }

    void process(juce::AudioBuffer<float>& buffer);

    // Total latency in base-rate samples: lookahead plus the DSP oversampler's own
    // filter latency at the tier-selected factor. Fixed once prepare() runs; never
    // changes afterwards (there is only ever one factor to report).
    int latencySamples() const noexcept { return totalLatencyBaseSamples; }

    float gainReductionDb() const noexcept { return currentGrDb.load(); }
    // Reported by a dedicated detector (tier-selected factor -- 8x at <=50kHz, 4x at
    // 50-100kHz, 2x above -- each validated against a 32x reference) that re-analyses
    // the plugin's own FINAL base-rate output — after Gain, limiting, Character and the
    // Ceiling clamp, exactly what is about to reach the host. It never shares state
    // with the DSP oversampler, so metering precision is set purely by sample rate.
    float truePeakDb() const noexcept { return currentTruePeakDb.load(); }

    // Read-only reporting/testing accessors -- no side effects, no behaviour change.
    int rampSettleSamples() const noexcept { return rampSettleBaseSamples; }
    int lookaheadSamples() const noexcept { return lookaheadBaseSamples; }
    int dedicatedAnalyzerSettleSamples() const noexcept { return dedicatedTpGainLatencyBaseSamples; }
    // The DSP factor and True Peak analyzer factor prepare() selected for the current
    // sample rate -- for tests/reporting to confirm the tier logic picked correctly.
    int dspFactorChosen() const noexcept { return dspFactor; }
    int truePeakFactorChosen() const noexcept { return tpFactor; }

    // Pure function, exposed for testing: the sample-rate tier rule itself, with no
    // engine state involved.
    static void tierForSampleRate(double sampleRate, int& dspFactorOut, int& tpFactorOut) noexcept
    {
        if (sampleRate <= 50000.0)       { dspFactorOut = 4; tpFactorOut = 8; }
        else if (sampleRate <= 100000.0) { dspFactorOut = 2; tpFactorOut = 4; }
        else                              { dspFactorOut = 1; tpFactorOut = 2; }
    }

private:
    void applyCharacter(float& sample) const noexcept;
    float computeReleaseCoeff(float target, double sampleRateAtFactor) const noexcept;

    struct PathState
    {
        std::vector<float> delay;               // ring of GAINED oversampled samples
        std::vector<juce::int64> minIdxRing;
        std::vector<float> minValRing;
        int minHead = 0, minCount = 0;
        float currentGain = 1.0f;
        float heldDigitalPeak = 0.0f;            // last sample-grid peak, used when True Peak is off
    };

    struct ChannelState
    {
        PathState path;
        std::vector<float> dryBaseDelay;         // base-rate ring of the RAW input — a pure sample
                                                  // delay that never touches the oversampling filter, so
                                                  // bypass is bit-exact.
        // Per-dedicated-oversampled-tick ring of the feed-forward True Peak analyzer's
        // |output|, delay-compensated on read -- see dedicatedTpGainLatencyBaseSamples.
        std::vector<float> tpGainPeakRing;
    };

    static void pushMin(PathState& ps, juce::int64 idx, float value, int ringCapacity) noexcept;
    static void popExpiredMin(PathState& ps, juce::int64 minAllowedIdx, int ringCapacity) noexcept;

    double baseSampleRate = 44100.0;
    int maxBlockSize = 512;
    int numChannels = 2;
    int lookaheadBaseSamples = 220;
    // The 20ms settling time of the three shared per-base-sample parameter ramps
    // (inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed) and truePeakBlend.
    int rampSettleBaseSamples = 960;

    static constexpr float kLookaheadMs = 5.0f;

    // The ONE DSP factor chosen in prepare() from the sample rate (1, 2, or 4 -- see
    // tierForSampleRate()). Only this factor's oversampler is ever built.
    int dspFactor = 4;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler; // null when dspFactor == 1
    int osLatencyBaseSamples = 0;   // DSP oversampler's own filter latency, in base-rate samples
    int lookaheadOsSamples = 0;     // lookahead length at dspFactor's rate
    int totalLatencyBaseSamples = 0; // lookaheadBaseSamples + osLatencyBaseSamples -- reported to the host

    juce::int64 writePos = 0, readPos = 0;

    // Scratch: oversampling works in place, so a copy of the raw input is needed before
    // it's overwritten; sized (numChannels, maxBlockSize) in prepare(), never resized
    // in process().
    juce::AudioBuffer<float> pathScratch;

    // Dedicated True Peak MEASUREMENT detector: its own oversampler (polyphase IIR --
    // a different filter family from the FIR equiripple used for processing --
    // feedback-style, on a copy of the final output, so it can never affect what's
    // actually sent to the host). Factor selected per sample-rate tier (see tpFactor).
    std::unique_ptr<juce::dsp::Oversampling<float>> dedicatedTpOversampler;
    juce::AudioBuffer<float> dedicatedTpScratch;

    // Dedicated True Peak GAIN-DRIVING analyzer -- a SECOND independent oversampler,
    // separate from both the DSP oversampler above (processing/Character quality only)
    // and dedicatedTpOversampler above (feedback meter, after the fact). This one runs
    // FEED-FORWARD, on the gained (post input-gain, pre-limiting, pre-Character)
    // signal, so the gain computation itself always sees genuine intersample
    // resolution at the tier's own factor. Deliberately FIR equiripple (linear-phase,
    // constant group delay at every frequency) so one measured compensation constant
    // (via an impulse in prepare()) is exact everywhere -- a polyphase IIR's
    // frequency-varying group delay left a real, measured ~0.1dB steady-state
    // overshoot on near-Nyquist content when tried here previously.
    //
    // Runs UNCONDITIONALLY (not skipped while True Peak is off): if it only ran while
    // True Peak was on, its ring would go stale during any OFF period, and reading a
    // few hundred stale samples right after re-enabling True Peak reproduced a real,
    // measured protection gap -- see the OFF->ON activation fix below.
    int tpFactor = 8;
    std::unique_ptr<juce::dsp::Oversampling<float>> dedicatedTpGainOversampler;
    juce::AudioBuffer<float> dedicatedTpGainScratch;
    int dedicatedTpGainLatencyBaseSamples = 0;  // rounded to whole base samples
    int dedicatedTpGainLatencyOsSamples = 0;    // rounded to whole dedicated-oversampled ticks, for compensation

    // Per-base-sample snapshots of the shared SmoothedValues below, computed once per
    // process() call. Sized to maxBlockSize in prepare(), never resized in process().
    std::vector<float> inputGainRampScratch, ceilRampScratch, linkRampScratch, tpBlendRampScratch;

    std::array<ChannelState, 2> channelState;
    int delayCapacity = 0;
    int minRingCapacity = 0;

    int dryDelayCapacity = 0;
    int tpGainPeakRingCapacity = 0; // next power of two >= dryDelayCapacity * tpFactor
    juce::int64 tpGainPeakRingMask = 0;
    juce::int64 baseWritePos = 0; // reset only in reset()

    juce::SmoothedValue<float> inputGainSmoothed, ceilingSmoothed, stereoLinkSmoothed;
    // 0 = fully wet (processed), 1 = fully dry (bypassed) — ramped over a few ms (at the
    // base sample rate, applied after downsampling) so toggling Bypass crossfades
    // instead of switching paths on a single sample.
    juce::SmoothedValue<float> bypassMix;
    // 0 = gain driven by the held sample-grid peak (True Peak off), 1 = by the genuine
    // instantaneous oversampled peak (True Peak on). Ramped (at the base rate) so
    // flipping the button blends smoothly. See process()/the peak-candidate combination
    // for why activation (OFF->ON) does not simply follow this ramp linearly.
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

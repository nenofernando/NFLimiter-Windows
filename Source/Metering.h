#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

struct MeterSnapshot
{
    float inL = -100, inR = -100, outL = -100, outR = -100;
    float truePeak = -100, lufsM = -100, lufsI = -100, gainReduction = 0;
    bool clip = false;
};

// Real BS.1770 K-weighted loudness metering: a high-shelf (+4dB @ 1.5kHz) cascaded with
// a high-pass (38Hz, RLB) per channel, a 400ms/100ms-hop sliding window for momentary
// loudness, and full two-stage gating (absolute -70 LUFS, relative -10 LU) for
// integrated loudness. K-weighting coefficients follow the RBJ cookbook shelf/high-pass
// formulas parameterised by sample rate (the same method used by pyloudnorm/libebur128
// for arbitrary, non-48kHz rates).
class Metering
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset();
    void requestResetIntegrated() noexcept { resetIntegratedRequested.store(true); }

    void captureInput(const juce::AudioBuffer<float>& in);
    // ceilingDbTP: current ceiling, so CLIP is an overload-vs-ceiling indicator, not a
    // fixed 0dBTP check. bypassed: forces CLIP off and clears any held state, so a
    // stale clip from before Bypass was engaged can never keep glowing through it.
    void captureOutput(const juce::AudioBuffer<float>& out, float grDb, float truePeakDb,
                        float ceilingDbTP, bool bypassed);

    MeterSnapshot get() const;

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process(float x) noexcept { const float y = b0 * x + z1; z1 = b1 * x + z2 - a1 * y; z2 = b2 * x - a2 * y; return y; }
        void reset() noexcept { z1 = 0; z2 = 0; }
    };

    void designKWeighting(double sr);
    static float peakDb(const juce::AudioBuffer<float>&, int channel);

    double sampleRate = 44100.0;
    int channels = 2;
    std::array<Biquad, 2> shelf, highpass;

    int windowLenSamples = 17640; // 400ms @ 44.1kHz
    int hopLenSamples = 4410;     // 100ms @ 44.1kHz
    std::vector<float> ringSumSq;
    int ringPos = 0;
    double runningSum = 0.0;
    int samplesUntilHop = 0;

    static constexpr int kGatingCapacity = 108000; // 3 hours of 100ms gating blocks
    std::vector<float> gatingBlocks;
    int gatingHead = 0, gatingCount = 0;
    std::atomic<bool> resetIntegratedRequested { false };

    std::atomic<float> inL { -100 }, inR { -100 }, outL { -100 }, outR { -100 }, truePeak { -100 };
    std::atomic<float> momentaryLufs { -100 }, gr { 0 };
    std::atomic<bool> clipped { false };
    int clipHoldSamplesRemaining = 0; // audio-thread-only; get() just reads `clipped`
};

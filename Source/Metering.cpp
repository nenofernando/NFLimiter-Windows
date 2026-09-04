#include "Metering.h"

float Metering::peakDb(const juce::AudioBuffer<float>& b, int c)
{
    return c < b.getNumChannels() ? juce::Decibels::gainToDecibels(b.getMagnitude(c, 0, b.getNumSamples()), -100.0f) : -100.0f;
}

void Metering::designKWeighting(double sr)
{
    auto designShelf = [sr](Biquad& f)
    {
        const double G = 4.0, Q = 1.0 / std::sqrt(2.0), fc = 1500.0;
        const double A = std::pow(10.0, G / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * (fc / sr);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cw = std::cos(w0);
        const double sqrtA = std::sqrt(A);
        const double b0 = A * ((A + 1) + (A - 1) * cw + 2 * sqrtA * alpha);
        const double b1 = -2.0 * A * ((A - 1) + (A + 1) * cw);
        const double b2 = A * ((A + 1) + (A - 1) * cw - 2 * sqrtA * alpha);
        const double a0 = (A + 1) - (A - 1) * cw + 2 * sqrtA * alpha;
        const double a1 = 2.0 * ((A - 1) - (A + 1) * cw);
        const double a2 = (A + 1) - (A - 1) * cw - 2 * sqrtA * alpha;
        f.b0 = (float) (b0 / a0); f.b1 = (float) (b1 / a0); f.b2 = (float) (b2 / a0);
        f.a1 = (float) (a1 / a0); f.a2 = (float) (a2 / a0);
    };
    auto designHighpass = [sr](Biquad& f)
    {
        const double Q = 0.5, fc = 38.0;
        const double w0 = 2.0 * juce::MathConstants<double>::pi * (fc / sr);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double cw = std::cos(w0);
        const double b0 = (1.0 + cw) / 2.0;
        const double b1 = -(1.0 + cw);
        const double b2 = (1.0 + cw) / 2.0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        f.b0 = (float) (b0 / a0); f.b1 = (float) (b1 / a0); f.b2 = (float) (b2 / a0);
        f.a1 = (float) (a1 / a0); f.a2 = (float) (a2 / a0);
    };
    for (auto& f : shelf) designShelf(f);
    for (auto& f : highpass) designHighpass(f);
}

void Metering::prepare(double sr, int numCh)
{
    sampleRate = sr;
    channels = juce::jlimit(1, 2, numCh);
    designKWeighting(sr);
    windowLenSamples = juce::jmax(1, (int) std::round(sr * 0.4));
    hopLenSamples = juce::jmax(1, (int) std::round(sr * 0.1));
    ringSumSq.assign((size_t) windowLenSamples, 0.0f);
    gatingBlocks.assign((size_t) kGatingCapacity, 0.0f);
    reset();
}

void Metering::reset()
{
    for (auto& f : shelf) f.reset();
    for (auto& f : highpass) f.reset();
    std::fill(ringSumSq.begin(), ringSumSq.end(), 0.0f);
    ringPos = 0; runningSum = 0.0; samplesUntilHop = hopLenSamples;
    gatingHead = 0; gatingCount = 0;
    resetIntegratedRequested.store(false);
    inL = inR = outL = outR = truePeak = -100.0f;
    momentaryLufs = -100.0f; gr = 0.0f; clipped = false; clipHoldSamplesRemaining = 0;
}

void Metering::captureInput(const juce::AudioBuffer<float>& b)
{
    inL = peakDb(b, 0);
    inR = peakDb(b, juce::jmin(1, b.getNumChannels() - 1));
}

void Metering::captureOutput(const juce::AudioBuffer<float>& b, float grDb, float truePeakDbIn,
                              float ceilingDbTP, bool bypassed)
{
    if (resetIntegratedRequested.exchange(false))
    {
        std::fill(ringSumSq.begin(), ringSumSq.end(), 0.0f);
        ringPos = 0; runningSum = 0.0; samplesUntilHop = hopLenSamples;
        gatingHead = 0; gatingCount = 0;
    }

    outL = peakDb(b, 0);
    outR = peakDb(b, juce::jmin(1, b.getNumChannels() - 1));
    gr = grDb;

    // TRUE PEAK OVER is an overload-vs-ceiling indicator (never a clipper control): it
    // lights when the SAME dedicated-detector reading shown on the True Peak card
    // (truePeakDbIn, measured on the final post-limiter/post-ceiling/post-downsampling
    // output — see LimiterEngine::truePeakDb()) exceeds the *current* ceiling by more
    // than a hair, holds so a fast transient is actually seen, then clears on its own.
    // A new over during an active hold re-triggers the full hold (jmax below). Bypass
    // always wins: no leftover latch can glow through it. The margin is sized against
    // the dedicated 8x detector's own known worst-case error (0.0311dB vs an
    // independent 32x reference, see oversampling_audit.cpp) so the detector's own
    // reconstruction noise alone can never cross it. This margin affects only the
    // alert -- it never feeds the ceiling or the limiting engine.
    static constexpr float kClipMarginDb = 0.05f;
    static constexpr double kClipHoldSeconds = 1.5;
    if (bypassed)
    {
        clipHoldSamplesRemaining = 0;
        clipped = false;
        truePeak = truePeakDbIn; // no retained value survives Bypass
    }
    else
    {
        if (truePeakDbIn > ceilingDbTP + kClipMarginDb)
            clipHoldSamplesRemaining = juce::jmax(clipHoldSamplesRemaining, (int) std::round(kClipHoldSeconds * sampleRate));
        else
            clipHoldSamplesRemaining = juce::jmax(0, clipHoldSamplesRemaining - b.getNumSamples());
        clipped = clipHoldSamplesRemaining > 0;

        // The TRUE PEAK card shows the peak responsible for the current over while the
        // light is held (growing if a still-bigger peak arrives during the hold), then
        // resumes following the live measurement the instant the hold ends -- so the
        // number and the light never visually disagree about what triggered it.
        truePeak = clipped ? juce::jmax(truePeak.load(), truePeakDbIn) : truePeakDbIn;
    }

    const int n = b.getNumSamples();
    const int chans = juce::jmin(channels, b.getNumChannels());

    for (int i = 0; i < n; ++i)
    {
        double sumSq = 0.0;
        for (int ch = 0; ch < chans; ++ch)
        {
            float x = b.getSample(ch, i);
            x = shelf[(size_t) ch].process(x);
            x = highpass[(size_t) ch].process(x);
            sumSq += (double) x * (double) x;
        }

        runningSum -= ringSumSq[(size_t) ringPos];
        ringSumSq[(size_t) ringPos] = (float) sumSq;
        runningSum += sumSq;
        ringPos = (ringPos + 1) % windowLenSamples;

        const double meanSq = juce::jmax(0.0, runningSum) / (double) windowLenSamples;
        momentaryLufs.store((float) (-0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, meanSq))));

        if (--samplesUntilHop <= 0)
        {
            samplesUntilHop = hopLenSamples;
            gatingBlocks[(size_t) ((gatingHead + gatingCount) % kGatingCapacity)] = (float) meanSq;
            if (gatingCount < kGatingCapacity) ++gatingCount;
            else gatingHead = (gatingHead + 1) % kGatingCapacity;
        }
    }
}

MeterSnapshot Metering::get() const
{
    MeterSnapshot s;
    s.inL = inL.load(); s.inR = inR.load(); s.outL = outL.load(); s.outR = outR.load();
    s.truePeak = truePeak.load(); s.gainReduction = gr.load(); s.clip = clipped.load();
    s.lufsM = momentaryLufs.load();

    const int count = gatingCount;
    if (count == 0) { s.lufsI = -100.0f; return s; }

    double sum1 = 0.0; int n1 = 0;
    for (int i = 0; i < count; ++i)
    {
        const float meanSq = gatingBlocks[(size_t) ((gatingHead + i) % kGatingCapacity)];
        const double lk = -0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, (double) meanSq));
        if (lk >= -70.0) { sum1 += meanSq; ++n1; }
    }
    if (n1 == 0) { s.lufsI = -100.0f; return s; }

    const double ungatedMean = sum1 / n1;
    const double relThreshold = -0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, ungatedMean)) - 10.0;

    double sum2 = 0.0; int n2 = 0;
    for (int i = 0; i < count; ++i)
    {
        const float meanSq = gatingBlocks[(size_t) ((gatingHead + i) % kGatingCapacity)];
        const double lk = -0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, (double) meanSq));
        if (lk >= -70.0 && lk >= relThreshold) { sum2 += meanSq; ++n2; }
    }
    s.lufsI = n2 > 0 ? (float) (-0.691 + 10.0 * std::log10(juce::jmax(1.0e-12, sum2 / n2))) : -100.0f;
    return s;
}

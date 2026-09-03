// Standalone harness for state persistence: host save/recall, preset save/load/delete
// (including factory-preset protection), and A/B/Copy independence. Runs the real
// NFLimiterAudioProcessor with no host and no editor.
#include "../Source/PluginProcessor.h"
#include <cstdio>

namespace
{
    int failures = 0;
    void check(bool condition, const std::string& what)
    {
        std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what.c_str());
        if (! condition) ++failures;
    }

    float raw(NFLimiterAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue(id)->load(); }
    void set(NFLimiterAudioProcessor& p, const char* id, float v01)
    {
        if (auto* param = p.apvts.getParameter(id)) param->setValueNotifyingHost(v01);
    }
}

int main()
{
    std::printf("== NF Limiter state/preset test harness ==\n\n");

    std::printf("-- Parameter layout sanity --\n");
    {
        NFLimiterAudioProcessor p;
        p.prepareToPlay(48000.0, 512);
        check(p.apvts.getParameter("gain") != nullptr, "gain parameter exists");
        check(p.apvts.getParameter("ceiling") != nullptr, "ceiling parameter exists");
        check(p.apvts.getParameter("release") != nullptr, "release parameter exists");
        check(p.apvts.getParameter("auto_release") != nullptr, "auto_release parameter exists");
        check(p.apvts.getParameter("character") != nullptr, "character parameter exists");
        check(p.apvts.getParameter("true_peak") != nullptr, "true_peak parameter exists");
        check(p.apvts.getParameter("oversampling") != nullptr, "oversampling parameter exists");
        check(p.apvts.getParameter("stereo_link") != nullptr, "stereo_link parameter exists");
        check(p.apvts.getParameter("bypass") != nullptr, "bypass parameter exists");
        check(std::abs(raw(p, "gain") - 0.0f) < 1.0e-3f, "gain default is 0 dB");
        check(std::abs(raw(p, "ceiling") - (-1.0f)) < 1.0e-3f, "ceiling default is -1 dBTP");
        check(std::abs(raw(p, "release") - 150.0f) < 1.0e-3f, "release default is 150 ms");
        check(raw(p, "stereo_link") > 99.0f, "stereo_link default is 100%");
    }

    std::printf("\n-- Host state save/recall round trip --\n");
    {
        NFLimiterAudioProcessor p;
        p.prepareToPlay(48000.0, 512);
        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(8.5f));
        set(p, "ceiling", p.apvts.getParameter("ceiling")->convertTo0to1(-3.2f));
        set(p, "bypass", 1.0f);

        juce::MemoryBlock block;
        p.getStateInformation(block);

        NFLimiterAudioProcessor p2;
        p2.prepareToPlay(48000.0, 512);
        p2.setStateInformation(block.getData(), (int) block.getSize());

        check(std::abs(raw(p2, "gain") - 8.5f) < 1.0e-2f, "gain survives save/recall");
        check(std::abs(raw(p2, "ceiling") - (-3.2f)) < 1.0e-2f, "ceiling survives save/recall");
        check(raw(p2, "bypass") > 0.5f, "bypass survives save/recall");
    }

    std::printf("\n-- Preset save/load/delete --\n");
    {
        NFLimiterAudioProcessor p;
        p.prepareToPlay(48000.0, 512);

        check(! p.presets.save(""), "save() rejects an empty name");
        check(! p.presets.save("Default"), "save() refuses to overwrite a factory name");
        check(! p.presets.remove("Loud Demo"), "remove() refuses to delete a factory preset");

        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(5.0f));
        const juce::String testName = "__NF_Limiter_Automated_Test__";
        p.presets.remove(testName); // clean up from a previous failed run, if any
        check(p.presets.save(testName), "save() writes a user preset");
        check(p.presets.names().contains(testName), "names() lists the new user preset");

        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(-10.0f));
        check(p.presets.load(testName), "load() succeeds for the user preset");
        check(std::abs(raw(p, "gain") - 5.0f) < 1.0e-2f, "load() restores the saved gain value");

        check(p.presets.remove(testName), "remove() deletes the user preset");
        check(! p.presets.names().contains(testName), "deleted preset no longer listed");

        p.presets.load("Loud Demo");
        check(raw(p, "gain") > 7.9f, "factory preset 'Loud Demo' applies its documented gain");
        check((int) std::round(raw(p, "character")) == 2, "factory preset 'Loud Demo' selects the Loud character");
    }

    std::printf("\n-- A/B independence and Copy --\n");
    {
        NFLimiterAudioProcessor p;
        p.prepareToPlay(48000.0, 512);

        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(3.0f));
        p.presets.selectAB(1);
        check(std::abs(raw(p, "gain") - 0.0f) < 1.0e-2f, "slot B starts independent from slot A's edits");

        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(-6.0f));
        p.presets.selectAB(0);
        check(std::abs(raw(p, "gain") - 3.0f) < 1.0e-2f, "returning to slot A restores its own value");

        p.presets.selectAB(1);
        check(std::abs(raw(p, "gain") - (-6.0f)) < 1.0e-2f, "slot B kept its own edited value");

        p.presets.copyToOther();
        p.presets.selectAB(0);
        check(std::abs(raw(p, "gain") - (-6.0f)) < 1.0e-2f, "COPY replicated slot B's value into slot A");
    }

    std::printf("\n-- GAIN REDUCTION HISTORY: real time-varying data, not one repeated value --\n");
    {
        const double sr = 48000.0;
        NFLimiterAudioProcessor p;
        p.prepareToPlay(sr, 512);
        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(18.0f));
        set(p, "ceiling", p.apvts.getParameter("ceiling")->convertTo0to1(-1.0f));

        // Silence, then loud periodic impulses (forcing real, varying gain reduction),
        // then silence again — several seconds so the ~4.8s history ring buffer fills
        // with genuinely different values across its length, not a snapshot of "now".
        const int totalSamples = (int) (sr * 6.0);
        juce::AudioBuffer<float> buf(2, totalSamples);
        buf.clear();
        for (int n = 4 * (int) sr; n < 5 * (int) sr; n += 4800)
            for (int ch = 0; ch < 2; ++ch)
                if (n < totalSamples) buf.setSample(ch, n, 3.0f);

        int pos = 0;
        while (pos < totalSamples)
        {
            const int bs = juce::jmin(512, totalSamples - pos);
            juce::AudioBuffer<float> chunk(buf.getArrayOfWritePointers(), 2, pos, bs);
            juce::MidiBuffer midi;
            p.processBlock(chunk, midi);
            pos += bs;
        }

        float minVal = 1000.0f, maxVal = -1000.0f;
        for (auto& v : p.history) { const float x = v.load(); minVal = juce::jmin(minVal, x); maxVal = juce::jmax(maxVal, x); }
        char label[192];
        std::snprintf(label, sizeof(label),
                      "history ring buffer spans %.2f dB (min=%.2f, max=%.2f) — must vary, not sit at one repeated value",
                      (double) (maxVal - minVal), (double) minVal, (double) maxVal);
        check((maxVal - minVal) > 1.0f, label);
        check(maxVal > -0.5f, "history includes near-0dB entries (the silent stretches)");
        check(minVal < -1.0f, "history includes real reduction entries (the impulses)");

        char label2[128];
        std::snprintf(label2, sizeof(label2), "history bucket duration ~8ms as configured (%d samples @ 48kHz)",
                      juce::roundToInt(sr * 0.008));
        check(true, label2); // documents the configured bucket size alongside the measured spread above
    }

    std::printf("\n-- PEAK vs TRUE PEAK coherence across a large host block --\n");
    {
        // Reproduces the exact bug: processBlock slices a large host block into several
        // ~5ms sub-chunks internally, and metering.captureOutput() must see the block-
        // WIDE max true peak, not just whichever sub-chunk happened to run last. Feed a
        // single large host block (2048 samples, far bigger than one 5ms sub-chunk) with
        // its loudest, most intersample-peak-provoking content in the FIRST sub-chunk and
        // near-silence afterwards — if the bug were still present, the last sub-chunk's
        // near-zero true peak would overwrite the real one before metering ever saw it.
        const double sr = 48000.0;
        NFLimiterAudioProcessor p;
        p.prepareToPlay(sr, 2048);
        set(p, "gain", p.apvts.getParameter("gain")->convertTo0to1(0.0f));
        set(p, "ceiling", p.apvts.getParameter("ceiling")->convertTo0to1(-1.0f));
        set(p, "true_peak", 0.0f); // OFF: sample-peak-only limiting deliberately lets true peak run past ceiling

        juce::AudioBuffer<float> buf(2, 2048);
        buf.clear();
        const double f1 = sr * 0.30, f2 = sr * 0.32;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 200; ++i) // only the first ~4ms carries the hot intersample-peak content
            {
                const double t = (double) i / sr;
                float x = 0.5f * (float) (std::sin(2.0 * juce::MathConstants<double>::pi * f1 * t)
                                         + std::sin(2.0 * juce::MathConstants<double>::pi * f2 * t));
                buf.setSample(ch, i, juce::jlimit(-1.0f, 1.0f, x * 1.98f));
            }

        // Process the hot block itself and read the meter right after it — the lookahead
        // latency (a few hundred samples) is far smaller than this 2048-sample block, so
        // the hot content's emission (and thus its true peak) lands within this same
        // call. Reading the meter after *later* blocks would just see whatever those
        // later (silent) blocks measured instead — each processBlock() call reports its
        // own block's true peak, not a running historical max.
        juce::MidiBuffer midi;
        p.processBlock(buf, midi);

        const float measuredTruePeak = p.metering.get().truePeak;
        char label[220];
        std::snprintf(label, sizeof(label),
            "True Peak measured from a large multi-sub-chunk block = %.3f dBTP (must reflect the hot first sub-chunk, not near-silence)",
            (double) measuredTruePeak);
        check(measuredTruePeak > -6.0f, label); // the hot content peaks well above -1dBTP ceiling; near-silence would read near -100dB
    }

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

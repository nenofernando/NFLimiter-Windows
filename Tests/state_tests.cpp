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

    std::printf("\n== %d failure(s) ==\n", failures);
    return failures == 0 ? 0 : 1;
}

#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// .nflpreset = the plugin's ValueTree state, XML-serialised, tagged with a version
// number so future releases can migrate older presets without ever renaming a
// parameter ID that has already shipped. Factory presets are computed in code (never
// written as files) and can't be overwritten or deleted; user presets live under
// folder() as one *.nflpreset file per preset.
class PresetManager
{
public:
    explicit PresetManager(juce::AudioProcessorValueTreeState&);

    static constexpr int currentVersion = 1;
    static const juce::StringArray& factoryNames();

    juce::StringArray names() const;
    bool isFactory(const juce::String& name) const { return factoryNames().contains(name); }

    bool save(const juce::String& name);
    bool load(const juce::String& name);
    bool remove(const juce::String& name); // refuses factory names

    void loadFactory(int index);
    void selectAB(int slot);
    void copyToOther();
    int activeAB() const noexcept { return activeSlot; }

    juce::File folder() const;

private:
    static void migrate(juce::ValueTree& tree, int fromVersion);

    juce::AudioProcessorValueTreeState& state;
    juce::ValueTree ab[2];
    int activeSlot = 0;
};

#include "PresetManager.h"

static const juce::Identifier nfVersionProp("nfPresetVersion");

PresetManager::PresetManager(juce::AudioProcessorValueTreeState& s) : state(s)
{
    ab[0] = state.copyState();
    ab[1] = state.copyState();
}

const juce::StringArray& PresetManager::factoryNames()
{
    static const juce::StringArray n { "Default", "Clean Master", "Punch Bus", "Loud Demo" };
    return n;
}

juce::File PresetManager::folder() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("NF Audio Tools/NF Limiter/Presets");
}

juce::StringArray PresetManager::names() const
{
    juce::StringArray n = factoryNames();
    for (auto& f : folder().findChildFiles(juce::File::findFiles, false, "*.nflpreset"))
        n.addIfNotAlreadyThere(f.getFileNameWithoutExtension());
    return n;
}

bool PresetManager::save(const juce::String& name)
{
    const juce::String trimmed = name.trim();
    if (trimmed.isEmpty() || isFactory(trimmed)) return false;

    auto dir = folder();
    if (! dir.exists() && ! dir.createDirectory()) return false;

    auto tree = state.copyState();
    tree.setProperty(nfVersionProp, currentVersion, nullptr);
    if (auto xml = tree.createXml())
        return xml->writeTo(dir.getChildFile(juce::File::createLegalFileName(trimmed) + ".nflpreset"));
    return false;
}

void PresetManager::migrate(juce::ValueTree& tree, int fromVersion)
{
    // V1 is the first shipped schema: nothing to migrate yet. Future versions add
    // steps here, keyed off fromVersion, and must never rename an already-released
    // parameter ID — only add new ones with safe defaults.
    juce::ignoreUnused(tree, fromVersion);
}

bool PresetManager::load(const juce::String& name)
{
    const int factoryIndex = factoryNames().indexOf(name);
    if (factoryIndex >= 0) { loadFactory(factoryIndex); return true; }

    auto f = folder().getChildFile(juce::File::createLegalFileName(name) + ".nflpreset");
    if (auto xml = juce::XmlDocument::parse(f))
    {
        auto tree = juce::ValueTree::fromXml(*xml);
        if (! tree.isValid()) return false;
        const int fileVersion = (int) tree.getProperty(nfVersionProp, 0);
        if (fileVersion < currentVersion) migrate(tree, fileVersion);
        state.replaceState(tree);
        return true;
    }
    return false;
}

bool PresetManager::remove(const juce::String& name)
{
    if (isFactory(name)) return false;
    return folder().getChildFile(juce::File::createLegalFileName(name) + ".nflpreset").deleteFile();
}

void PresetManager::loadFactory(int i)
{
    auto set = [this](const char* id, float v)
    {
        if (auto* p = state.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(v));
    };
    // 0 Default, 1 Clean Master, 2 Punch Bus, 3 Loud Demo
    set("gain", i == 3 ? 8.0f : i == 2 ? 4.0f : 0.0f);
    set("ceiling", -1.0f);
    set("release", i == 2 ? 80.0f : 150.0f);
    set("auto_release", 1.0f);
    set("character", (float) juce::jlimit(0, 2, i - 1));
    set("true_peak", 1.0f);
    set("oversampling", 2.0f);
    set("stereo_link", 100.0f);
    set("bypass", 0.0f);
}

void PresetManager::selectAB(int slot)
{
    slot = juce::jlimit(0, 1, slot);
    if (slot == activeSlot) return;
    ab[activeSlot] = state.copyState();
    activeSlot = slot;
    if (ab[activeSlot].isValid()) state.replaceState(ab[activeSlot].createCopy());
}

void PresetManager::copyToOther()
{
    ab[activeSlot] = state.copyState();
    ab[1 - activeSlot] = ab[activeSlot].createCopy();
}

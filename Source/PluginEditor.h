#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

// The whole UI is authored in a fixed 1536x1024 design space — the exact pixel size of
// Assets/Reference/NF_Limiter_approved_concept.png and of Assets/Exact_PNG/LAYOUT_MAP.json
// — and a single uniform scale+letterbox transform maps it onto the actual (resizable)
// component bounds. Every rect below was measured directly from that reference image
// (either taken verbatim from LAYOUT_MAP.json, or pixel-detected for the meters/GR bar),
// so geometry is exact rather than approximated; only genuinely static, never-changing
// pieces (logo, title lockup, footer credit) are drawn from the literal approved pixels,
// since everything else must reflect live plugin state.
class NFLimiterAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NFLimiterAudioProcessorEditor(NFLimiterAudioProcessor&);
    ~NFLimiterAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct ChoiceButtonGroup
    {
        void bind(juce::AudioProcessorValueTreeState& state, const juce::String& paramID, std::vector<juce::TextButton*> btns);
        void refresh();
        juce::AudioProcessorValueTreeState* apvts = nullptr;
        juce::String id;
        std::vector<juce::TextButton*> buttons;
    };

    static constexpr int designW = 1536, designH = 1024;
    juce::AffineTransform designToScreen() const;
    juce::Rectangle<int> map(juce::Rectangle<float> designRect) const;

    // The single official centre axis for every centred element (title, subtitle, GR
    // block, TRUE PEAK label+button, ...): the GAIN REDUCTION panel's own centreX, in
    // design-space units. Because the whole layout is uniformly scaled as one unit
    // (designToScreen()), anything centred on this axis in design space stays centred
    // at every window size — there is no separate per-size correction anywhere.
    static float getMainProcessingAxisX();

    void timerCallback() override;
    void refreshPresetList();
    void stepPreset(int direction);
    void requestSavePreset();
    void showMainMenu();
    void showManualDialog(const juce::String& title, bool portuguese);

    void drawPanel(juce::Graphics&, juce::Rectangle<float>);
    void drawTitle(juce::Graphics&, juce::Rectangle<float>, const juce::String&, float fontSize = 15.0f);
    void drawMeterPair(juce::Graphics&, juce::Rectangle<float> barL, juce::Rectangle<float> barR,
                        float scaleX, bool scaleOnLeft, float dbL, float dbR,
                        juce::Colour top, juce::Colour bottom);
    void drawGainReductionPanel(juce::Graphics&);
    void drawHistoryPanel(juce::Graphics&, juce::Rectangle<float>);
    void drawInfoBox(juce::Graphics&, juce::Rectangle<float>, const juce::String& label,
                      const juce::String& value, juce::Colour valueColour);
    void drawGroupBox(juce::Graphics&, juce::Rectangle<float>);
    void drawPill(juce::Graphics&, juce::Rectangle<float>);

    NFLimiterAudioProcessor& processor;
    NFLookAndFeel look;

    juce::Slider gain, ceiling, release, link;
    juce::ToggleButton autoRelease { "AUTO" }, truePeak { "ON" };
    juce::TextButton bypass { "BYPASS" }, power { "" };
    juce::TextButton characterButtons[3] { juce::TextButton("CLEAN"), juce::TextButton("PUNCH"), juce::TextButton("LOUD") };
    // The OVERSAMPLING selector is gone from the UI: the factor is now chosen
    // automatically from the sample rate (LimiterEngine::tierForSampleRate()). The
    // "oversampling" APVTS parameter itself is untouched, kept only so old
    // sessions/automation lanes that reference it still load without error.
    ChoiceButtonGroup characterGroup;

    juce::ComboBox presets;
    juce::TextButton presetPrev { "<" }, presetNext { ">" }, savePreset { "SAVE" }, menu,
                     aButton { "A" }, bButton { "B" }, copyButton { "COPY" };

    juce::ComponentBoundsConstrainer constrainer;
    juce::ResizableCornerComponent resizer;

    // Passive warning light only — never a button. See Theme.h's
    // NFLookAndFeel::paintTruePeakOverIndicator for the on/off appearance.
    struct TruePeakOverIndicator final : public juce::Component, public juce::SettableTooltipClient
    {
        TruePeakOverIndicator()
        {
            setInterceptsMouseClicks(false, false);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            setTooltip("True Peak Over: acende quando a saida pos-limiter ultrapassa o Ceiling. / Lights when the post-limiter true peak exceeds the Ceiling.");
            setTitle("True Peak Over Indicator");
            setDescription("Post-limiter true-peak overshoot warning");
            setAccessible(true);
        }
        void setLit(bool shouldBeLit) { if (lit != shouldBeLit) { lit = shouldBeLit; repaint(); } }
        void paint(juce::Graphics& g) override { NFLookAndFeel::paintTruePeakOverIndicator(g, getLocalBounds().toFloat(), lit); }
        bool lit = false;
    };
    TruePeakOverIndicator truePeakOverIndicator;

    std::unique_ptr<SliderAttachment> gainA, ceilingA, releaseA, linkA;
    std::unique_ptr<ButtonAttachment> autoA, tpA, bypassA;

    MeterSnapshot snapshot;
    juce::Rectangle<int> logoBounds;

    // Exactly the 1536:1024 (1.5) design aspect ratio, just a bit smaller than before
    // so the plugin doesn't dominate the screen on first open.
    static constexpr int defaultW = 1080, defaultH = 720;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NFLimiterAudioProcessorEditor)
};

#include "PluginEditor.h"
#include "BinaryData.h"

// ---------------------------------------------------------------------------------
// Design-space geometry (1536x1024), measured directly from
// Assets/Reference/NF_Limiter_approved_concept.png: panel/control rects come verbatim
// from Assets/Exact_PNG/LAYOUT_MAP.json; meter bars and the gain-reduction bar were
// pixel-detected (colour thresholding) from the same reference image, so their
// coordinates are exact, not eyeballed.
namespace NFLayout
{
    using R = juce::Rectangle<float>;

    static const R header          { 0, 0, 1536, 116 };
    static const R logoText        { 25, 15, 235, 95 };
    static const R titleText       { 485, 10, 460, 100 };
    static const R footerRect      { 0, 969, 1536, 55 };

    // Preset bar + A/B/COPY come from LAYOUT_MAP.json (shrunk slightly to also fit SAVE
    // at the same size as A/B, since the approved crop itself has no SAVE button). MENU
    // sits alone in the far corner with a larger gap, per the reference's spirit of a
    // single settings affordance separate from the preset controls.
    static const R presetPill      { 965, 27, 215, 62 };
    static const R aRect           { 1185, 27, 62, 62 };
    static const R bRect           { 1252, 27, 62, 62 };
    static const R copyRect        { 1319, 27, 95, 62 };
    static const R saveRect        { 1422, 27, 62, 62 };
    static const R menuRect        { 1496, 27, 40, 62 };

    static const R leftPanel       { 28, 115, 320, 670 };
    static const R rightPanel      { 1188, 115, 320, 670 };
    static const R grPanel         { 345, 115, 655, 670 };

    // Left (input) — exact bar rects from colour-threshold detection.
    static const R inputTitle      { 28, 124, 320, 26 };
    static const R inLabelL        { 117, 152, 49, 20 };
    static const R inLabelR        { 210, 152, 49, 20 };
    static const R barInL          { 117, 177, 49, 315 };
    static const R barInR          { 210, 177, 49, 315 };
    static const R inScaleCol      { 44, 177, 68, 315 };
    static const R inValueRow      { 100, 496, 186, 22 };
    static const R gainTitle       { 28, 534, 320, 24 };
    static const R gainKnob        { 86, 550, 204, 204 };
    static const R gainValue       { 28, 758, 320, 26 };

    // Right (output) — mirrored, scale numbers on the right of the bars.
    static const R outputTitle     { 1188, 124, 320, 26 };
    static const R outLabelL       { 1266, 152, 49, 20 };
    static const R outLabelR       { 1367, 152, 49, 20 };
    static const R barOutL         { 1266, 177, 49, 315 };
    static const R barOutR         { 1367, 177, 49, 315 };
    static const R outScaleCol     { 1424, 177, 68, 315 };
    static const R outValueRow     { 1249, 496, 186, 22 };
    static const R ceilingTitle    { 1188, 534, 320, 24 };
    static const R ceilingKnob     { 1246, 550, 204, 204 };
    static const R ceilingValue    { 1188, 758, 320, 26 };

    // Gain-reduction panel.
    static const R grTitle         { 345, 124, 655, 26 };
    static const R grLabelCol      { 365, 208, 50, 291 };
    static const R grGridArea      { 420, 208, 565, 291 };
    static const R grBar           { 563, 208, 296, 291 };
    static const R grNumber        { 400, 503, 545, 78 };
    static const R grHistory       { 395, 585, 590, 155 };

    // Statistics column — exact LAYOUT_MAP rects.
    static const R peakBox         { 1010, 155, 160, 105 };
    static const R truePeakBox     { 1010, 265, 160, 105 };
    static const R lufsMBox        { 1010, 375, 160, 105 };
    static const R lufsIBox        { 1010, 485, 160, 105 };
    static const R clipBox         { 1010, 610, 160, 110 };

    // Bottom control bar.
    static const R releaseTitle    { 40, 796, 225, 24 };
    static const R releaseKnob     { 107, 826, 90, 90 };
    static const R releaseValue    { 40, 918, 225, 22 };
    static const R autoPill        { 77, 942, 150, 26 };

    static const R characterTitle  { 275, 806, 355, 24 };
    static const R characterBox    { 285, 838, 335, 90 };

    static const R truePeakTitle   { 625, 806, 155, 24 };
    static const R truePeakButton  { 647, 845, 110, 58 };

    static const R oversamplingTitle { 775, 806, 320, 24 };
    static const R oversamplingBox   { 785, 838, 300, 90 };

    static const R linkTitle       { 1090, 796, 185, 24 };
    static const R linkKnob        { 1137, 830, 90, 90 };
    static const R linkValue       { 1090, 928, 185, 26 };

    static const R bypassButton    { 1270, 800, 225, 55 };
    static const R powerButton     { 1270, 868, 225, 58 };
}

// --------------------------------------------------------------------- ChoiceGroup --

void NFLimiterAudioProcessorEditor::ChoiceButtonGroup::bind(juce::AudioProcessorValueTreeState& state,
                                                              const juce::String& paramID,
                                                              std::vector<juce::TextButton*> btns)
{
    apvts = &state; id = paramID; buttons = std::move(btns);
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        auto* btn = buttons[i];
        const int index = (int) i;
        btn->onClick = [this, index]
        {
            if (auto* p = apvts->getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1((float) index));
            refresh();
        };
    }
    refresh();
}

void NFLimiterAudioProcessorEditor::ChoiceButtonGroup::refresh()
{
    if (apvts == nullptr) return;
    const int idx = juce::roundToInt(apvts->getRawParameterValue(id)->load());
    for (size_t i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState((int) i == idx, juce::dontSendNotification);
}

// ------------------------------------------------------------------------ ctor/dtor --

NFLimiterAudioProcessorEditor::NFLimiterAudioProcessorEditor(NFLimiterAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p), resizer(this, &constrainer)
{
    setLookAndFeel(&look);
    setResizable(true, false);
    constrainer.setSizeLimits(860, 560, 1900, 1250);
    constrainer.setFixedAspectRatio((double) NFLimiterAudioProcessorEditor::designW / (double) NFLimiterAudioProcessorEditor::designH);
    setConstrainer(&constrainer);
    setSize(defaultW, defaultH);

    for (auto* s : { &gain, &ceiling, &release, &link })
    {
        addAndMakeVisible(*s);
        s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    }
    gain.setColour(juce::Slider::rotarySliderFillColourId, NFColour::knobCyan);
    ceiling.setColour(juce::Slider::rotarySliderFillColourId, NFColour::knobPurple);
    link.setColour(juce::Slider::rotarySliderFillColourId, NFColour::knobPurple);
    release.setColour(juce::Slider::rotarySliderFillColourId, NFColour::knobOrange);
    gain.setTooltip("Input gain applied before limiting, -12 to +24 dB.");
    ceiling.setTooltip("True-peak output ceiling, -12 to -0.1 dBTP.");
    release.setTooltip("Release time, 10-1000 ms (scaled automatically while AUTO is on).");
    link.setTooltip("Stereo link amount for the gain-reduction detector, 0-100%.");

    addAndMakeVisible(autoRelease);
    addAndMakeVisible(truePeak);
    autoRelease.setClickingTogglesState(true);
    truePeak.setClickingTogglesState(true);
    autoRelease.setTooltip("Automatically scale release time with the amount of gain reduction.");
    truePeak.setTooltip("Detect true (intersample) peaks instead of sample peaks only.");

    addAndMakeVisible(bypass);
    addAndMakeVisible(power);
    bypass.setClickingTogglesState(true);
    power.setClickingTogglesState(true);
    power.setButtonText(juce::String::fromUTF8("\xE2\x8F\xBB"));
    power.setComponentID("power");
    power.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    bypass.setTooltip("Bypass the limiter (latency-compensated, click-free).");
    power.setTooltip("Power: lit when the limiter is active (click to toggle bypass).");
    // Power reflects "is active" (NOT bypassed) rather than mirroring BYPASS directly,
    // so it lights up purple during normal operation, matching the approved concept.
    power.onClick = [this]
    {
        if (auto* param = processor.apvts.getParameter("bypass"))
            param->setValueNotifyingHost(param->convertTo0to1(bypass.getToggleState() ? 0.0f : 1.0f));
    };

    std::vector<juce::TextButton*> charBtns, osBtns;
    for (auto& b : characterButtons) { addAndMakeVisible(b); b.setClickingTogglesState(false); charBtns.push_back(&b); }
    for (auto& b : oversamplingButtons) { addAndMakeVisible(b); b.setClickingTogglesState(false); osBtns.push_back(&b); }
    characterGroup.bind(processor.apvts, "character", charBtns);
    oversamplingGroup.bind(processor.apvts, "oversampling", osBtns);
    characterButtons[0].setTooltip("Clean: bit-transparent, no colouration.");
    characterButtons[1].setTooltip("Punch: faster adaptive release keeps transients snappy.");
    characterButtons[2].setTooltip("Loud: adds gentle saturation density on limited peaks.");
    for (auto& b : oversamplingButtons) b.setTooltip("Oversampling factor for true-peak detection and processing quality.");

    addAndMakeVisible(presets);
    addAndMakeVisible(presetPrev);
    addAndMakeVisible(presetNext);
    addAndMakeVisible(savePreset);
    menu.setButtonText(juce::String::fromUTF8("\xE2\x98\xB0"));
    addAndMakeVisible(menu);
    addAndMakeVisible(aButton);
    addAndMakeVisible(bButton);
    addAndMakeVisible(copyButton);
    addAndMakeVisible(resizer);
    presetPrev.setTooltip("Previous preset");
    presetNext.setTooltip("Next preset");
    savePreset.setTooltip("Save current settings as a new user preset");
    menu.setTooltip("Manuals, preset folder, reset window size, about");
    aButton.setTooltip("A/B slot A");
    bButton.setTooltip("A/B slot B");
    copyButton.setTooltip("Copy the active A/B slot to the other one");
    aButton.setClickingTogglesState(false);
    bButton.setClickingTogglesState(false);

    gainA = std::make_unique<SliderAttachment>(processor.apvts, "gain", gain);
    ceilingA = std::make_unique<SliderAttachment>(processor.apvts, "ceiling", ceiling);
    releaseA = std::make_unique<SliderAttachment>(processor.apvts, "release", release);
    linkA = std::make_unique<SliderAttachment>(processor.apvts, "stereo_link", link);
    autoA = std::make_unique<ButtonAttachment>(processor.apvts, "auto_release", autoRelease);
    tpA = std::make_unique<ButtonAttachment>(processor.apvts, "true_peak", truePeak);
    bypassA = std::make_unique<ButtonAttachment>(processor.apvts, "bypass", bypass);

    refreshPresetList();
    presets.onChange = [this] { if (presets.getSelectedId() > 0) processor.presets.load(presets.getText()); };
    presetPrev.onClick = [this] { stepPreset(-1); };
    presetNext.onClick = [this] { stepPreset(1); };
    savePreset.onClick = [this] { requestSavePreset(); };
    menu.onClick = [this] { showMainMenu(); };
    aButton.onClick = [this] { processor.presets.selectAB(0); aButton.setToggleState(true, juce::dontSendNotification); bButton.setToggleState(false, juce::dontSendNotification); repaint(); };
    bButton.onClick = [this] { processor.presets.selectAB(1); bButton.setToggleState(true, juce::dontSendNotification); aButton.setToggleState(false, juce::dontSendNotification); repaint(); };
    copyButton.onClick = [this] { processor.presets.copyToOther(); };
    aButton.setToggleState(true, juce::dontSendNotification);

    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

NFLimiterAudioProcessorEditor::~NFLimiterAudioProcessorEditor() { setLookAndFeel(nullptr); }

// ------------------------------------------------------------------- design mapping --

juce::AffineTransform NFLimiterAudioProcessorEditor::designToScreen() const
{
    const float sx = (float) getWidth() / (float) designW;
    const float sy = (float) getHeight() / (float) designH;
    const float s = juce::jmin(sx, sy);
    const float offX = ((float) getWidth() - (float) designW * s) * 0.5f;
    const float offY = ((float) getHeight() - (float) designH * s) * 0.5f;
    return juce::AffineTransform::scale(s).translated(offX, offY);
}

juce::Rectangle<int> NFLimiterAudioProcessorEditor::map(juce::Rectangle<float> designRect) const
{
    return designRect.transformedBy(designToScreen()).getSmallestIntegerContainer();
}

// ----------------------------------------------------------------------- behaviour --

void NFLimiterAudioProcessorEditor::refreshPresetList()
{
    auto current = presets.getText();
    presets.clear();
    auto names = processor.presets.names();
    for (int i = 0; i < names.size(); ++i) presets.addItem(names[i], i + 1);
    const int idx = names.indexOf(current);
    presets.setSelectedId(idx >= 0 ? idx + 1 : 1, juce::dontSendNotification);
}

void NFLimiterAudioProcessorEditor::stepPreset(int direction)
{
    const int count = presets.getNumItems();
    if (count == 0) return;
    int idx = juce::jmax(0, presets.getSelectedItemIndex());
    idx = (idx + direction + count) % count;
    presets.setSelectedItemIndex(idx, juce::sendNotificationSync);
}

void NFLimiterAudioProcessorEditor::requestSavePreset()
{
    auto w = std::make_shared<juce::AlertWindow>("Save NF Limiter Preset", "Preset name:", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "My Preset");
    w->addButton("SAVE", 1);
    w->addButton("CANCEL", 0);
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result)
    {
        if (result != 1) return;
        const auto name = w->getTextEditorContents("name");
        if (processor.presets.save(name)) refreshPresetList();
        else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "NF Limiter",
                                                      "Could not save that preset name (empty, or a factory name).");
    }), true);
}

void NFLimiterAudioProcessorEditor::showManualDialog(const juce::String& title, const juce::String& text)
{
    auto editor = std::make_unique<juce::TextEditor>();
    editor->setMultiLine(true, true);
    editor->setReadOnly(true);
    editor->setCaretVisible(false);
    editor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0c0f16));
    editor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    editor->setFont(juce::Font(juce::FontOptions(14.0f)));
    editor->setText(text);
    editor->setSize(680, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(editor.release());
    o.dialogTitle = title;
    o.dialogBackgroundColour = juce::Colour(0xff0c0f16);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void NFLimiterAudioProcessorEditor::showMainMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader("NF LIMITER");
    m.addItem(1, "Manual - Portugues");
    m.addItem(2, "Manual - English");
    m.addSeparator();
    m.addItem(3, "Open Preset Folder");
    m.addItem(4, "Reset Window Size");
    m.addItem(5, "Reset Integrated Loudness (LUFS-I)");
    m.addSeparator();
    m.addItem(6, "About");
    m.showMenuAsync({}, [this](int r)
    {
        if (r == 1) showManualDialog("Manual - Portugues",
            juce::String::fromUTF8(BinaryData::MANUAL_PT_md, BinaryData::MANUAL_PT_mdSize));
        else if (r == 2) showManualDialog("Manual - English",
            juce::String::fromUTF8(BinaryData::MANUAL_EN_md, BinaryData::MANUAL_EN_mdSize));
        else if (r == 3)
        {
            auto folder = processor.presets.folder();
            if (! folder.exists()) folder.createDirectory();
            folder.startAsProcess();
        }
        else if (r == 4) setSize(defaultW, defaultH);
        else if (r == 5) processor.metering.requestResetIntegrated();
        else if (r == 6) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "NF Limiter V1.0",
                                                                  juce::String::fromUTF8("NF Audio Tools \xE2\x80\x94 By Nenno Fernando"));
    });
}

void NFLimiterAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    if (logoBounds.contains(e.getPosition())) setSize(defaultW, defaultH);
}

void NFLimiterAudioProcessorEditor::timerCallback()
{
    snapshot = processor.metering.get();
    characterGroup.refresh();
    oversamplingGroup.refresh();
    power.setToggleState(! bypass.getToggleState(), juce::dontSendNotification);
    repaint();
}

// ---------------------------------------------------------------- drawing helpers --

void NFLimiterAudioProcessorEditor::drawPanel(juce::Graphics& g, juce::Rectangle<float> b)
{
    // Elevated brushed-metal panel: base gradient, fine horizontal brushing, an inner
    // shadow hugging the edges (recessed feel), and a bright top bevel + dark bottom
    // bevel for a raised, silver-edged look matching the approved concept.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff23272e), b.getX(), b.getY(),
                                            juce::Colour(0xff121417), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle(b, 12.0f);

    {
        juce::Graphics::ScopedSaveState save(g);
        juce::Path clip; clip.addRoundedRectangle(b, 12.0f);
        g.reduceClipRegion(clip.getBounds().getSmallestIntegerContainer());
        g.setColour(juce::Colours::white.withAlpha(0.025f));
        for (float y = b.getY() + 4.0f; y < b.getBottom(); y += 3.0f)
            g.drawLine(b.getX(), y, b.getRight(), y, 1.0f);
    }

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.drawRoundedRectangle(b.reduced(3.0f), 10.0f, 5.0f);

    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.drawLine(b.getX() + 12.0f, b.getY() + 1.2f, b.getRight() - 12.0f, b.getY() + 1.2f, 1.2f);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawLine(b.getX() + 12.0f, b.getBottom() - 1.0f, b.getRight() - 12.0f, b.getBottom() - 1.0f, 1.0f);

    g.setColour(NFColour::panelBorder);
    g.drawRoundedRectangle(b.reduced(0.6f), 12.0f, 1.2f);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawRoundedRectangle(b.reduced(1.8f), 11.0f, 1.0f);
}

void NFLimiterAudioProcessorEditor::drawTitle(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, float fontSize)
{
    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)).withExtraKerningFactor(0.08f));
    g.drawText(text, r, juce::Justification::centred);
}

void NFLimiterAudioProcessorEditor::drawMeterPair(juce::Graphics& g, juce::Rectangle<float> barL, juce::Rectangle<float> barR,
                                                   float scaleX, bool scaleOnLeft, float dbL, float dbR,
                                                   juce::Colour top, juce::Colour bottom)
{
    const int steps[] { 0, -6, -12, -18, -24, -30, -36, -42, -48, -54, -60 };
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    for (int s : steps)
    {
        const float y = juce::jmap((float) s, 0.0f, -60.0f, barL.getY(), barL.getBottom());
        g.setColour(NFColour::textDim);
        auto textR = juce::Rectangle<float>(scaleX, y - 8.0f, 60.0f, 16.0f);
        g.drawText(juce::String(s), textR, scaleOnLeft ? juce::Justification::centredRight : juce::Justification::centredLeft);
    }

    auto drawBar = [&](juce::Rectangle<float> bar, float db)
    {
        g.setColour(NFColour::wellBg);
        g.fillRoundedRectangle(bar, 3.0f);

        const float h = juce::jmap(juce::jlimit(-60.0f, 0.0f, db), -60.0f, 0.0f, 0.0f, bar.getHeight());
        if (h > 0.5f)
        {
            auto filled = bar.withTop(bar.getBottom() - h);
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(filled.getSmallestIntegerContainer());
            g.setGradientFill(juce::ColourGradient(top, bar.getX(), bar.getY(), bottom, bar.getX(), bar.getBottom(), false));
            g.fillRoundedRectangle(bar, 3.0f);
        }
        g.setColour(NFColour::wellBg.withAlpha(0.9f));
        for (float y = bar.getBottom(); y > bar.getY(); y -= 8.0f)
            g.drawLine(bar.getX(), y, bar.getRight(), y, 1.0f);
        g.setColour(NFColour::wellBorder);
        g.drawRoundedRectangle(bar, 3.0f, 1.0f);
    };
    drawBar(barL, dbL);
    drawBar(barR, dbR);
}

void NFLimiterAudioProcessorEditor::drawGainReductionPanel(juce::Graphics& g)
{
    using namespace NFLayout;
    drawPanel(g, grPanel);

    // Every element below shares the panel's true horizontal centre — the scale
    // column sits outside this to the left and never enters the centring maths.
    const float centerX = grPanel.getCentreX();
    auto meterBounds = grBar;
    meterBounds.setX(centerX - meterBounds.getWidth() * 0.5f);
    auto numberBounds = grNumber;
    numberBounds.setX(meterBounds.getX());
    numberBounds.setWidth(meterBounds.getWidth());
    auto historyBounds = grHistory;
    historyBounds.setX(centerX - historyBounds.getWidth() * 0.5f);
    auto titleBounds = grTitle;
    titleBounds.setX(centerX - titleBounds.getWidth() * 0.5f);

    drawTitle(g, titleBounds, "GAIN REDUCTION", 17.0f);

    const int steps[] { 0, -4, -8, -12, -16, -20, -24 };
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    for (int s : steps)
    {
        const float y = juce::jmap((float) s, 0.0f, -24.0f, grGridArea.getY(), grGridArea.getBottom());
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.drawText(juce::String(s), grLabelCol.withY(y - 9.0f).withHeight(18.0f), juce::Justification::centredRight);
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.drawLine(grGridArea.getX(), y, grGridArea.getRight(), y, 1.0f);
    }
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.drawText("dB", grLabelCol.withY(grGridArea.getBottom() + 3.0f).withHeight(18.0f), juce::Justification::centredRight);

    // A real segmented LED-style ladder: every segment is always visible (dark grey
    // when unlit), and segments light up top-down in the fixed yellow/orange/red band
    // as gain reduction increases — never a plain empty box, never a permanent glow.
    g.setColour(NFColour::wellBg);
    g.fillRect(meterBounds);

    const float amount = juce::jlimit(0.0f, 24.0f, -snapshot.gainReduction);
    const int numSegments = 48;
    const float segGap = 1.5f;
    const float segH = (meterBounds.getHeight() - segGap * (float) (numSegments - 1)) / (float) numSegments;
    const int litCount = juce::roundToInt(juce::jmap(amount, 0.0f, 24.0f, 0.0f, (float) numSegments));

    for (int i = 0; i < numSegments; ++i)
    {
        const float segY = meterBounds.getY() + (float) i * (segH + segGap);
        juce::Rectangle<float> seg(meterBounds.getX(), segY, meterBounds.getWidth(), segH);
        if (i < litCount)
        {
            const float t = (float) i / (float) (numSegments - 1);
            g.setColour(NFColour::grYellow.interpolatedWith(NFColour::grRed, t));
        }
        else
        {
            g.setColour(juce::Colour(0xff222429));
        }
        g.fillRect(seg);
    }
    g.setColour(NFColour::wellBorder);
    g.drawRect(meterBounds, 1.0f);

    g.setFont(juce::Font(juce::FontOptions(42.0f, juce::Font::bold)));
    g.setColour(juce::Colour(0xffff5a3c));
    g.drawText(juce::String(snapshot.gainReduction, 1) + " dB",
               (int) numberBounds.getX(), (int) numberBounds.getY(), (int) numberBounds.getWidth(), (int) numberBounds.getHeight(),
               juce::Justification::centred);

    drawHistoryPanel(g, historyBounds);

   #if NF_DEBUG_GR_CENTER_GUIDES
    g.setColour(juce::Colours::red);
    g.drawLine(centerX, grPanel.getY(), centerX, grPanel.getBottom(), 1.0f);
   #endif
}

void NFLimiterAudioProcessorEditor::drawHistoryPanel(juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour(NFColour::wellBg);
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(NFColour::wellBorder);
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    auto labels = area.removeFromLeft(area.getWidth() * 0.06f);
    auto plot = area.reduced(5.0f, 5.0f);

    const int steps[] { 0, -6, -12, -18, -24 };
    g.setFont(juce::Font(juce::FontOptions(10.5f)));
    for (int s : steps)
    {
        const float y = juce::jmap((float) s, 0.0f, -24.0f, plot.getY(), plot.getBottom());
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.drawText(juce::String(s), labels.withY(y - 7.0f).withHeight(14.0f), juce::Justification::centredRight);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawLine(plot.getX(), y, plot.getRight(), y, 1.0f);
    }
    g.drawText("dB", labels.withY(plot.getBottom() + 1.0f).withHeight(14.0f), juce::Justification::centredRight);
    for (int i = 1; i < 6; ++i)
    {
        const float x = juce::jmap((float) i, 0.0f, 6.0f, plot.getX(), plot.getRight());
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.drawLine(x, plot.getY(), x, plot.getBottom(), 1.0f);
    }

    // Real data only: gainReductionDb() is published from the DSP's actual applied
    // gain (Decibels::gainToDecibels(appliedGain)) at a fixed, block-size-independent
    // cadence (see processBlock's chunking) — never a derived/approximated value, never
    // input gain or peak. Displayed range is clamped to -24..0dB for the plot only;
    // NaN/Inf entries are rejected rather than connected into the curve. The newest
    // point is at the right edge and scrolls left as it ages.
    const int start = processor.historyWrite.load();
    std::vector<juce::Point<float>> tracePts;
    tracePts.reserve((size_t) NFLimiterAudioProcessor::historyLength);
    for (int i = 0; i < NFLimiterAudioProcessor::historyLength; ++i)
    {
        float v = processor.history[(size_t) ((start + i) % NFLimiterAudioProcessor::historyLength)].load();
        if (! std::isfinite(v)) v = ! tracePts.empty() ? juce::jmap(tracePts.back().y, plot.getY(), plot.getBottom(), 0.0f, -24.0f) : 0.0f;
        const float displayGrDb = juce::jlimit(-24.0f, 0.0f, v);
        const float x = juce::jmap((float) i, 0.0f, (float) (NFLimiterAudioProcessor::historyLength - 1), plot.getX(), plot.getRight());
        const float y = juce::jmap(displayGrDb, 0.0f, -24.0f, plot.getY(), plot.getBottom());
        tracePts.push_back({ x, y });
    }

    juce::Path curve;
    curve.startNewSubPath(tracePts.front());
    for (auto& pt : tracePts) curve.lineTo(pt);

    // The filled area sits between the 0dB reference line (the plot's top edge) and
    // the curve — so at rest (curve hugging the top) the fill is naturally a hairline,
    // and it grows into the "fire" shape exactly in proportion to real reduction, with
    // no separate empty/idle special-case needed.
    juce::Path fill;
    fill.startNewSubPath(plot.getX(), plot.getY());
    fill.lineTo(tracePts.front());
    for (auto& pt : tracePts) fill.lineTo(pt);
    fill.lineTo(plot.getRight(), plot.getY());
    fill.closeSubPath();

    juce::ColourGradient fireGradient(juce::Colour(0xffff9a00), 0, plot.getY(),
                                       juce::Colour(0xffff2418), 0, plot.getBottom(), false);
    fireGradient.addColour(0.5, juce::Colour(0xffff5a00));
    g.setGradientFill(fireGradient);
    g.fillPath(fill);

    // Discreet glow behind the line, then a crisp orange stroke on top.
    g.setColour(juce::Colour(0xffff9a00).withAlpha(0.35f));
    g.strokePath(curve, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xffffaa2e));
    g.strokePath(curve, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void NFLimiterAudioProcessorEditor::drawInfoBox(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& label,
                                                 const juce::String& value, juce::Colour valueColour)
{
    // Recessed display: dark inset gradient, an inner shadow hugging the top edge, and
    // a faint bright rim just inside the border for a bit of glass-like depth.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff0d0f13), r.getX(), r.getY(),
                                            juce::Colour(0xff1c2027), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle(r, 7.0f);
    {
        juce::Graphics::ScopedSaveState save(g);
        juce::Path clip; clip.addRoundedRectangle(r, 7.0f);
        g.reduceClipRegion(clip.getBounds().getSmallestIntegerContainer());
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.55f), r.getX(), r.getY(),
                                                juce::Colours::black.withAlpha(0.0f), r.getX(), r.getY() + r.getHeight() * 0.4f, false));
        g.fillRect(r);
    }
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawRoundedRectangle(r, 7.0f, 1.2f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawRoundedRectangle(r.reduced(1.4f), 6.0f, 1.0f);

    auto inner = r.reduced(r.getWidth() * 0.08f, r.getHeight() * 0.1f);
    g.setColour(NFColour::textDim);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)).withExtraKerningFactor(0.05f));
    g.drawText(label, inner.removeFromTop(inner.getHeight() * 0.42f), juce::Justification::centred);
    g.setColour(valueColour);
    g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    g.drawText(value, inner, juce::Justification::centred);
}

void NFLimiterAudioProcessorEditor::drawGroupBox(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(juce::Colour(0xff0e1014));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(NFColour::panelBorder);
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
}

void NFLimiterAudioProcessorEditor::drawPill(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(juce::Colour(0xff0e1014));
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    g.setColour(NFColour::panelBorder);
    g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
}

// ------------------------------------------------------------------------- paint --

void NFLimiterAudioProcessorEditor::paint(juce::Graphics& g)
{
    using namespace NFLayout;
    g.fillAll(NFColour::graphite);

    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(designToScreen());

    // One continuous brushed-metal surface for the whole header — logo and title are
    // drawn directly on top of it (no pasted bitmaps), so there is no rectangle seam.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff20242b), 0, 0, juce::Colour(0xff121418), (float) designW, (float) header.getHeight(), false));
    g.fillRect(header);
    g.setColour(juce::Colours::white.withAlpha(0.03f));
    for (float y = 6.0f; y < header.getHeight(); y += 3.0f) g.drawLine(0, y, (float) designW, y, 1.0f);
    g.setColour(NFColour::panelBorder);
    g.drawLine(0, header.getBottom(), (float) designW, header.getBottom(), 1.2f);

    {
        auto nfR = juce::Rectangle<float>(logoText.getX(), logoText.getY(), logoText.getWidth(), logoText.getHeight() * 0.58f);
        // Inclined brushed-metal sheen: a diagonal gradient plus a bright highlight
        // streak, rather than a flat top-to-bottom fade.
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xfffafbfc), nfR.getX(), nfR.getY(),
                                                juce::Colour(0xff6f7680), nfR.getX() + nfR.getWidth() * 0.55f, nfR.getBottom(), false));
        g.setFont(juce::Font(juce::FontOptions(48.0f, juce::Font::bold)));
        g.drawText("NF", nfR, juce::Justification::centredLeft);
        {
            juce::Graphics::ScopedSaveState saveHighlight(g);
            juce::Path clip; clip.addRectangle(nfR);
            g.reduceClipRegion(clip.getBounds().getSmallestIntegerContainer());
            g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.0f), nfR.getX(), nfR.getY(),
                                                    juce::Colours::white.withAlpha(0.5f), nfR.getX() + nfR.getWidth() * 0.22f, nfR.getY() + nfR.getHeight() * 0.15f, false));
            g.drawText("NF", nfR, juce::Justification::centredLeft);
        }
        g.setColour(NFColour::textDim);
        g.setFont(juce::Font(juce::FontOptions(12.5f)).withExtraKerningFactor(0.22f));
        g.drawText("AUDIO TOOLS", juce::Rectangle<float>(logoText.getX(), nfR.getBottom() + 2.0f, logoText.getWidth(), 22.0f), juce::Justification::centredLeft);
    }
    logoBounds = map(logoText);

    {
        // "NF Limiter" and its subtitle share the GAIN REDUCTION panel's centreX
        // (not the full window's), so the whole vertical stack lines up.
        const float titleCenterX = grPanel.getCentreX();
        auto nameR = juce::Rectangle<float>(titleCenterX - titleText.getWidth() * 0.5f, titleText.getY(),
                                             titleText.getWidth(), titleText.getHeight() * 0.56f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff5f6f8), nameR.getX(), nameR.getY(),
                                                juce::Colour(0xff9aa0aa), nameR.getX(), nameR.getBottom(), false));
        g.setFont(juce::Font(juce::FontOptions(42.0f, juce::Font::bold)));
        g.drawText("NF Limiter", nameR, juce::Justification::centred);
        g.setColour(NFColour::textDim);
        g.setFont(juce::Font(juce::FontOptions(15.0f)).withExtraKerningFactor(0.24f));
        g.drawText("TRUE PEAK MASTERING LIMITER", nameR.withY(nameR.getBottom() + 2.0f).withHeight(24.0f), juce::Justification::centred);

       #if NF_DEBUG_GR_CENTER_GUIDES
        g.setColour(juce::Colours::red);
        g.drawLine(titleCenterX, 4.0f, titleCenterX, (float) (NFLayout::grPanel.getBottom()), 1.0f);
       #endif
    }

    drawPill(g, presetPill);
    drawPanel(g, leftPanel);
    drawPanel(g, rightPanel);
    drawGainReductionPanel(g);

    drawTitle(g, inputTitle, "INPUT");
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.setColour(NFColour::textDim);
    g.drawText("L", inLabelL, juce::Justification::centred);
    g.drawText("R", inLabelR, juce::Justification::centred);
    drawMeterPair(g, barInL, barInR, inScaleCol.getX(), true, snapshot.inL, snapshot.inR, NFColour::inputCyan, NFColour::inputGreen);
    g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    g.setColour(NFColour::inputCyan);
    g.drawText(juce::String(snapshot.inL, 1) + " dB", inValueRow.withWidth(inValueRow.getWidth() * 0.5f), juce::Justification::centred);
    g.setColour(NFColour::inputGreen);
    g.drawText(juce::String(snapshot.inR, 1) + " dB", inValueRow.withX(inValueRow.getCentreX()).withWidth(inValueRow.getWidth() * 0.5f), juce::Justification::centred);
    drawTitle(g, gainTitle, "GAIN");
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(processor.apvts.getRawParameterValue("gain")->load(), 1) + " dB", gainValue, juce::Justification::centred);

    drawTitle(g, outputTitle, "OUTPUT");
    g.setFont(juce::Font(juce::FontOptions(12.5f)));
    g.setColour(NFColour::textDim);
    g.drawText("L", outLabelL, juce::Justification::centred);
    g.drawText("R", outLabelR, juce::Justification::centred);
    drawMeterPair(g, barOutL, barOutR, outScaleCol.getX(), false, snapshot.outL, snapshot.outR, NFColour::outputPurple, NFColour::outputPink);
    g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    g.setColour(NFColour::outputPurple);
    g.drawText(juce::String(snapshot.outL, 1) + " dB", outValueRow.withWidth(outValueRow.getWidth() * 0.5f), juce::Justification::centred);
    g.setColour(NFColour::outputPink);
    g.drawText(juce::String(snapshot.outR, 1) + " dB", outValueRow.withX(outValueRow.getCentreX()).withWidth(outValueRow.getWidth() * 0.5f), juce::Justification::centred);
    drawTitle(g, ceilingTitle, "CEILING");
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(processor.apvts.getRawParameterValue("ceiling")->load(), 1) + " dBTP", ceilingValue, juce::Justification::centred);

    drawInfoBox(g, peakBox, "PEAK", juce::String(juce::jmax(snapshot.outL, snapshot.outR), 1) + " dBFS", juce::Colours::white);
    drawInfoBox(g, truePeakBox, "TRUE PEAK", juce::String(snapshot.truePeak, 1) + " dBTP", NFColour::outputPurple);
    drawInfoBox(g, lufsMBox, "LUFS-M", juce::String(snapshot.lufsM, 1), NFColour::inputGreen);
    drawInfoBox(g, lufsIBox, "LUFS-I", juce::String(snapshot.lufsI, 1), NFColour::inputGreen);

    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.drawLine(clipBox.getX() + 10.0f, clipBox.getY(), clipBox.getRight() - 10.0f, clipBox.getY(), 1.0f);
    auto clipArea = clipBox.withTrimmedTop(10.0f);
    g.setColour(NFColour::grRed);
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)).withExtraKerningFactor(0.08f));
    g.drawText("CLIP", clipArea.removeFromTop(clipArea.getHeight() * 0.42f), juce::Justification::centred);
    juce::Rectangle<float> led(26.0f, 26.0f);
    led.setCentre(clipArea.getCentreX(), clipArea.getY() + 22.0f);
    g.setColour(snapshot.clip ? NFColour::grRed : NFColour::grRed.withAlpha(0.25f));
    g.fillEllipse(led);
    if (snapshot.clip)
    {
        g.setColour(NFColour::grRed.withAlpha(0.35f));
        g.fillEllipse(led.expanded(8.0f));
    }
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawEllipse(led, 1.0f);

    drawGroupBox(g, characterBox);
    drawGroupBox(g, oversamplingBox);
    drawTitle(g, releaseTitle, "RELEASE", 14.0f);
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(juce::roundToInt(processor.apvts.getRawParameterValue("release")->load())) + " ms",
               releaseValue, juce::Justification::centred);
    drawTitle(g, characterTitle, "CHARACTER", 14.0f);
    drawTitle(g, truePeakTitle, "TRUE PEAK", 14.0f);
    drawTitle(g, oversamplingTitle, "OVERSAMPLING", 14.0f);
    drawTitle(g, linkTitle, "STEREO LINK", 14.0f);

    g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    g.setColour(juce::Colours::white);
    g.drawText(juce::String(juce::roundToInt(processor.apvts.getRawParameterValue("stereo_link")->load())) + " %",
               linkValue, juce::Justification::centred);

    g.setColour(juce::Colours::white.withAlpha(0.82f));
    g.setFont(juce::Font(juce::FontOptions(14.5f)).withExtraKerningFactor(0.18f));
    g.drawText(juce::String::fromUTF8("NF AUDIO TOOLS \xE2\x80\x94 BY NENNO FERNANDO"), footerRect, juce::Justification::centred);
}

// ---------------------------------------------------------------------- resized --

void NFLimiterAudioProcessorEditor::resized()
{
    using namespace NFLayout;

    presetPrev.setBounds(map({ presetPill.getX(), presetPill.getY(), presetPill.getHeight(), presetPill.getHeight() }));
    presetNext.setBounds(map({ presetPill.getRight() - presetPill.getHeight(), presetPill.getY(), presetPill.getHeight(), presetPill.getHeight() }));
    presets.setBounds(map(presetPill.reduced(presetPill.getHeight() * 0.9f, 0.0f)));
    aButton.setBounds(map(aRect));
    bButton.setBounds(map(bRect));
    copyButton.setBounds(map(copyRect));
    savePreset.setBounds(map(saveRect));
    menu.setBounds(map(menuRect));

    gain.setBounds(nfSquare(map(gainKnob)));
    ceiling.setBounds(nfSquare(map(ceilingKnob)));
    release.setBounds(nfSquare(map(releaseKnob)));
    link.setBounds(nfSquare(map(linkKnob)));
    autoRelease.setBounds(map(autoPill));

    truePeak.setBounds(map(truePeakButton));
    bypass.setBounds(map(bypassButton));
    power.setBounds(map(powerButton));

    {
        auto box = characterBox;
        const float pad = box.getWidth() * 0.03f, gap = box.getWidth() * 0.025f;
        auto inner = box.reduced(pad, box.getHeight() * 0.1f);
        const float w = (inner.getWidth() - gap * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i)
            characterButtons[i].setBounds(map(inner.withX(inner.getX() + i * (w + gap)).withWidth(w)));
    }
    {
        auto box = oversamplingBox;
        const float pad = box.getWidth() * 0.03f, gap = box.getWidth() * 0.02f;
        auto inner = box.reduced(pad, box.getHeight() * 0.1f);
        const float w = (inner.getWidth() - gap * 3.0f) / 4.0f;
        for (int i = 0; i < 4; ++i)
            oversamplingButtons[i].setBounds(map(inner.withX(inner.getX() + i * (w + gap)).withWidth(w)));
    }

    resizer.setBounds(getWidth() - 22, getHeight() - 22, 22, 22);
}

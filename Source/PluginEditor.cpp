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
    // Narrower than before and re-centred on the PEAK/TRUE PEAK/LUFS-M/LUFS-I info
    // column beneath it (peakBox, centred at x=1090) instead of spanning wider than
    // that column.
    static const R presetPill      { 995, 27, 190, 62 };
    static constexpr float presetArrowW = 34.0f;
    // A/B/COPY/SAVE shrunk to match the narrower preset pill, laid out sequentially
    // right after it with small consistent gaps; MENU keeps its own larger gap.
    static const R aRect           { 1201, 33, 50, 50 };
    static const R bRect           { 1259, 33, 50, 50 };
    static const R copyRect        { 1317, 33, 78, 50 };
    static const R saveRect        { 1403, 33, 50, 50 };
    static const R menuRect        { 1477, 33, 40, 50 };

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

    // Bottom control bar, laid out as one grid: every zone's title shares the same
    // titleBaselineY, every zone's main control shares the same controlCenterY (knobs
    // and buttons of different heights are centred on this same line rather than
    // top-aligned), and RELEASE/STEREO LINK's numeric readouts share valueBaselineY.
    static constexpr float titleBaselineY = 806.0f;
    static constexpr float controlCenterY = 883.0f;
    static constexpr float valueBaselineY = 920.0f;
    static constexpr float secondaryControlCenterY = 955.0f;
    static constexpr float minimumGroupGap = 12.0f;

    // Shifted right so the RELEASE knob shares the exact same centre X as the GAIN
    // knob above it (both at design-space x=188) instead of sitting 36px to its left.
    static const R releaseTitle    { 76, titleBaselineY, 225, 24 };
    static const R releaseKnob     { 143, controlCenterY - 45.0f, 90, 90 };
    static const R releaseValue    { 76, valueBaselineY, 225, 22 };
    static const R autoPill        { 113, secondaryControlCenterY - 13.0f, 150, 26 };

    // Left edge stays put (minimumGroupGap from RELEASE), width trimmed a bit further
    // so the gap to TRUE PEAK matches TRUE PEAK's gap to OVERSAMPLING on the other side.
    static const R characterTitle  { 313, titleBaselineY, 247, 24 };
    static const R characterBox    { 313, controlCenterY - 45.0f, 247, 90 };

    // Centred on the GAIN REDUCTION panel's own centreX (672.5), not on the group's
    // own arbitrary column position — same axis as the header title and the GR block.
    // This axis is fixed and must not move for any spacing adjustment.
    static const R truePeakTitle   { grPanel.getCentreX() - 155.0f * 0.5f, titleBaselineY, 155, 24 };
    static const R truePeakButton  { grPanel.getCentreX() - 110.0f * 0.5f, controlCenterY - 29.0f, 110, 58 };

    static const R oversamplingTitle { 775, titleBaselineY, 320, 24 };
    static const R oversamplingBox   { 785, controlCenterY - 45.0f, 300, 90 };

    // Nudged left ~30px from its original centre for better breathing room before BYPASS/POWER.
    static const R linkTitle       { 1060, titleBaselineY, 185, 24 };
    static const R linkKnob        { 1107, controlCenterY - 45.0f, 90, 90 };
    static const R linkValue       { 1060, valueBaselineY, 185, 26 };

    // Both re-centred on the CEILING knob above them (design x=1348) rather than on
    // their own original column, per explicit request.
    static const R powerButton     { ceilingKnob.getCentreX() - 112.5f, 868, 225, 58 };
    static const R bypassButton    { ceilingKnob.getCentreX() - 65.0f, 808.5f, 130, 38 };
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
    for (auto& b : characterButtons) { addAndMakeVisible(b); b.setClickingTogglesState(false); b.setComponentID("character"); charBtns.push_back(&b); }
    for (auto& b : oversamplingButtons) { addAndMakeVisible(b); b.setClickingTogglesState(false); b.setComponentID("oversampling"); osBtns.push_back(&b); }
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
    addAndMakeVisible(truePeakOverIndicator);
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

float NFLimiterAudioProcessorEditor::getMainProcessingAxisX() { return NFLayout::grPanel.getCentreX(); }

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
    // The dialog's callback fires later, asynchronously, whenever the user dismisses
    // it — by then the plugin editor may already have been closed/destroyed by the
    // host (a real crash seen with at least one host: the editor window can go away
    // while this dialog is still up). A SafePointer, not a raw `this`, makes that a
    // no-op instead of a use-after-free.
    juce::Component::SafePointer<NFLimiterAudioProcessorEditor> safeThis(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, w](int result)
    {
        if (result != 1 || safeThis == nullptr) return;
        const auto name = w->getTextEditorContents("name");
        if (safeThis->processor.presets.save(name)) safeThis->refreshPresetList();
        else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "NF Limiter",
                                                      "Could not save that preset name (empty, or a factory name).");
    }), true);
}

namespace ManualDoc
{
    struct Block
    {
        enum Kind { Heading, Paragraph, Bullet, Image } kind;
        juce::String text;
        juce::Image image;
    };

    // A single scrollable page mixing wrapped, wordwrapped text blocks with the two
    // real UI screenshots (Assets/Manual/overview.png, controls.png), laid out top to
    // bottom at a fixed content width. No markdown parsing at runtime: the manual's
    // structure is built directly in buildManualBlocks() below, in each language.
    struct Page final : public juce::Component
    {
        static constexpr int contentWidth = 620;
        static constexpr int margin = 20;
        std::vector<Block> blocks;

        int layout(juce::Graphics* g)
        {
            int y = margin;
            for (auto& b : blocks)
            {
                if (b.kind == Block::Image)
                {
                    const float scale = (float) contentWidth / (float) juce::jmax(1, b.image.getWidth());
                    const int h = (int) std::round(b.image.getHeight() * scale);
                    if (g != nullptr)
                    {
                        g->setColour(juce::Colour(0xff2a2f38));
                        g->drawRect(juce::Rectangle<int>(margin, y, contentWidth, h).toFloat().expanded(1.0f), 1.0f);
                        g->drawImage(b.image, (float) margin, (float) y, (float) contentWidth, (float) h,
                                     0, 0, b.image.getWidth(), b.image.getHeight());
                    }
                    y += h + 22;
                    continue;
                }

                const bool bullet = b.kind == Block::Bullet;
                const float indent = bullet ? 18.0f : 0.0f;
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::byWord);
                as.setLineSpacing(4.0f);
                if (b.kind == Block::Heading)
                {
                    as.append(b.text, juce::Font(juce::FontOptions(19.0f, juce::Font::bold)), juce::Colour(0xff5ec8ff));
                }
                else
                {
                    if (bullet) as.append(juce::String::fromUTF8("\xE2\x80\xA2  "),
                                           juce::Font(juce::FontOptions(14.5f)), juce::Colours::white.withAlpha(0.6f));
                    as.append(b.text, juce::Font(juce::FontOptions(14.5f)), juce::Colours::white.withAlpha(0.88f));
                }
                juce::TextLayout tl;
                tl.createLayout(as, (float) contentWidth - indent);
                if (g != nullptr) tl.draw(*g, juce::Rectangle<float>((float) margin + indent, (float) y,
                                                                      (float) contentWidth - indent, tl.getHeight()));
                y += (int) std::ceil(tl.getHeight()) + (b.kind == Block::Heading ? 10 : 12);
            }
            return y + margin;
        }

        void resized() override {}
        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff0c0f16));
            layout(&g);
        }

        void finalise()
        {
            setSize(contentWidth + margin * 2, layout(nullptr));
        }
    };

    static std::unique_ptr<Page> buildManualPage(bool portuguese)
    {
        auto overview = juce::ImageCache::getFromMemory(BinaryData::overview_png, BinaryData::overview_pngSize);
        auto controls = juce::ImageCache::getFromMemory(BinaryData::controls_png, BinaryData::controls_pngSize);

        auto page = std::make_unique<Page>();
        auto& bl = page->blocks;
        auto H = [&] (const char* t) { bl.push_back({ Block::Heading, juce::String::fromUTF8(t), {} }); };
        auto P = [&] (const char* t) { bl.push_back({ Block::Paragraph, juce::String::fromUTF8(t), {} }); };
        auto B = [&] (const char* t) { bl.push_back({ Block::Bullet, juce::String::fromUTF8(t), {} }); };
        auto I = [&] (juce::Image img) { bl.push_back({ Block::Image, {}, img }); };

        if (portuguese)
        {
            H("NF Limiter V1.0 — Manual");
            P("NF Audio Tools — By Nenno Fernando. Limitador de masterizacao com true peak sobreamostrado, medicao de loudness BS.1770 (LUFS-M/LUFS-I) e lookahead sem overshoot.");
            H("Visao geral da interface");
            I(overview);
            P("Da esquerda para a direita: medidores de Input, o painel central GAIN REDUCTION (com historico dos ultimos segundos), as leituras de Peak / True Peak / LUFS-M / LUFS-I e os medidores de Output. A barra superior traz o seletor de presets, A/B, COPY, SAVE e o menu de tres tracos.");
            H("Fluxo rapido");
            B("Insira o NF Limiter no ultimo slot do master.");
            B("Mantenha True Peak ligado e Ceiling em -1,0 dBTP para streaming como ponto inicial.");
            B("Aumente Gain observando o medidor central de Gain Reduction.");
            B("Escolha Clean para transparencia, Punch para preservar ataques ou Loud para maior densidade.");
            B("Compare sempre com Bypass em volume semelhante, para nao confundir volume com melhora real.");
            H("Controles inferiores");
            I(controls);
            P("RELEASE define a velocidade de recuperacao da reducao de ganho; AUTO adapta essa velocidade a intensidade e duracao dos picos. CHARACTER escolhe o comportamento do limitador: Clean (menor coloracao), Punch (recuperacao mais rapida, preserva impacto) ou Loud (maior densidade, saturacao suave controlada). TRUE PEAK protege picos reconstruidos entre amostras (intersample). OVERSAMPLING aumenta a precisao do true peak trocando por mais uso de CPU (1x/2x/4x/8x). STEREO LINK em 100% mantem a imagem estereo estavel; valores menores permitem acao parcialmente independente entre os canais. BYPASS compara o sinal processado com o original, preservando a mesma cadeia de audio (sem clique, com latencia compensada).");
            H("Medidores");
            P("INPUT e OUTPUT mostram os canais L/R em tempo real. GAIN REDUCTION mostra a atenuacao aplicada agora e seu historico recente. PEAK e TRUE PEAK exibem os valores maximos ja atingidos. LUFS-M indica o loudness momentaneo; LUFS-I acumula desde a abertura do plugin ou o ultimo reset (menu -> Reset LUFS). TRUE PEAK OVER e um indicador passivo (nao e um botao): acende brevemente sempre que a saida pos-limiter ultrapassa o Ceiling em mais de 0,05 dB.");
            H("Presets e menu");
            P("Escolha um preset na barra superior; use A/B para comparar duas configuracoes e COPY para copiar o estado atual entre elas. Clique SAVE para nomear e salvar um preset. O menu de tres tracos abre os manuais em PT/EN, a pasta de presets, About, Reset Window Size e Reset LUFS. A alca no canto inferior direito redimensiona a janela; clicar no logo NF volta ao tamanho padrao.");
            H("Suporte");
            P("NF Audio Tools — By Nenno Fernando. Versao 1.0.");
        }
        else
        {
            H("NF Limiter V1.0 — Manual");
            P("NF Audio Tools — By Nenno Fernando. A true-peak, oversampled, lookahead mastering limiter with BS.1770 loudness metering (LUFS-M/LUFS-I) and zero-overshoot brickwall limiting.");
            H("Interface overview");
            I(overview);
            P("Left to right: Input meters, the central GAIN REDUCTION panel (with a rolling history of the last few seconds), the Peak / True Peak / LUFS-M / LUFS-I readouts, and the Output meters. The top bar holds the preset selector, A/B, COPY, SAVE and the three-line menu.");
            H("Quick workflow");
            B("Insert NF Limiter in the last slot of the master bus.");
            B("Keep True Peak on and start with Ceiling at -1.0 dBTP for streaming.");
            B("Raise Gain while watching the central Gain Reduction meter.");
            B("Choose Clean for transparency, Punch to preserve transients, or Loud for extra density.");
            B("Always compare against Bypass at a matched perceived level, so you're judging quality, not just loudness.");
            H("Bottom controls");
            I(controls);
            P("RELEASE sets how fast gain reduction recovers; AUTO adapts that speed to the intensity and duration of the peaks. CHARACTER picks the limiter's behaviour: Clean (least colouration), Punch (faster recovery, preserves impact) or Loud (denser, gently saturated). TRUE PEAK protects reconstructed inter-sample peaks. OVERSAMPLING trades CPU for true-peak accuracy (1x/2x/4x/8x). STEREO LINK at 100% keeps the stereo image stable; lower values let the channels act more independently. BYPASS compares the processed signal against the original through the same audio chain, click-free and latency-compensated.");
            H("Meters");
            P("INPUT and OUTPUT show the L/R channels in real time. GAIN REDUCTION shows the attenuation applied right now plus its recent history. PEAK and TRUE PEAK show the highest values reached. LUFS-M is momentary loudness; LUFS-I accumulates since the plugin opened or the last reset (menu -> Reset LUFS). TRUE PEAK OVER is a passive indicator (never a button): it lights briefly whenever the post-limiter output exceeds the Ceiling by more than 0.05 dB.");
            H("Presets and menu");
            P("Pick a preset from the top bar; use A/B to compare two settings and COPY to copy the current state between them. Click SAVE to name and store a preset. The three-line menu opens the PT/EN manuals, the presets folder, About, Reset Window Size and Reset LUFS. Drag the lower-right handle to resize the window; click the NF logo to restore the default size.");
            H("Support");
            P("NF Audio Tools — By Nenno Fernando. Version 1.0.");
        }

        page->finalise();
        return page;
    }
}

void NFLimiterAudioProcessorEditor::showManualDialog(const juce::String& title, bool portuguese)
{
    auto page = ManualDoc::buildManualPage(portuguese);

    auto viewport = std::make_unique<juce::Viewport>();
    viewport->setViewedComponent(page.release(), true);
    viewport->setSize(720, 640);
    viewport->setScrollBarsShown(true, false);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(viewport.release());
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
    // Same lifetime hazard as the SAVE dialog above: this fires later, after the
    // editor may already be gone.
    juce::Component::SafePointer<NFLimiterAudioProcessorEditor> safeThis(this);
    m.showMenuAsync({}, [safeThis](int r)
    {
        if (safeThis == nullptr) return;
        if (r == 1) safeThis->showManualDialog("Manual - Portugues", true);
        else if (r == 2) safeThis->showManualDialog("Manual - English", false);
        else if (r == 3)
        {
            auto folder = safeThis->processor.presets.folder();
            if (! folder.exists()) folder.createDirectory();
            folder.startAsProcess();
        }
        else if (r == 4) safeThis->setSize(defaultW, defaultH);
        else if (r == 5) safeThis->processor.metering.requestResetIntegrated();
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
    truePeakOverIndicator.setLit(snapshot.clip);
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

    // Colour bands by depth (0dB at the top down to -24dB at the bottom), so red only
    // ever shows up where the curve actually dips deep, not across the whole graph:
    // 0..-6dB orange, -6..-12dB a stronger orange, -12..-24dB red. Stops are doubled up
    // just before each boundary so the transition is a near-hard band, not a smooth
    // blend across the full 24dB range.
    const juce::Colour bandOrange (0xffff9500), bandStrongOrange (0xffff6500), bandRed (0xffff3028);
    auto bandedGradient = [&](float topAlpha, float bottomAlpha)
    {
        juce::ColourGradient gr(bandOrange.withAlpha(topAlpha), 0, plot.getY(),
                                 bandRed.withAlpha(bottomAlpha), 0, plot.getBottom(), false);
        const float midAlpha = (topAlpha + bottomAlpha) * 0.5f;
        gr.addColour(0.2499, bandOrange.withAlpha(topAlpha));
        gr.addColour(0.25, bandStrongOrange.withAlpha(midAlpha));
        gr.addColour(0.4999, bandStrongOrange.withAlpha(midAlpha));
        gr.addColour(0.5, bandRed.withAlpha(bottomAlpha));
        return gr;
    };

    // Fill: low alpha throughout (0.18 near the 0dB line, fading to 0.06 near the
    // bottom/curve) — a discreet tint, never a solid opaque block.
    g.setGradientFill(bandedGradient(0.18f, 0.06f));
    g.fillPath(fill);

    // Discreet glow behind the line, then a crisp, depth-coloured stroke on top.
    g.setGradientFill(bandedGradient(0.35f, 0.35f));
    g.strokePath(curve, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setGradientFill(bandedGradient(1.0f, 1.0f));
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
    juce::ignoreUnused(minimumGroupGap); // documents the >=12px gap the constants above were spaced by
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
        g.drawLine(titleCenterX, 4.0f, titleCenterX, (float) (NFLayout::truePeakButton.getBottom() + 4.0f), 1.0f);
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

   #if NF_DEBUG_GR_CENTER_GUIDES
    g.setColour(juce::Colours::lime);
    g.drawLine(28.0f, titleBaselineY, 1508.0f, titleBaselineY, 1.0f);
    g.setColour(juce::Colours::cyan);
    g.drawLine(28.0f, controlCenterY, 1508.0f, controlCenterY, 1.0f);
    g.setColour(juce::Colours::yellow);
    g.drawLine(28.0f, valueBaselineY, 1508.0f, valueBaselineY, 1.0f);
   #endif

    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("v1.0", footerRect.reduced(14.0f, 0.0f), juce::Justification::centredRight);
}

// ---------------------------------------------------------------------- resized --

void NFLimiterAudioProcessorEditor::resized()
{
    using namespace NFLayout;

    presetPrev.setBounds(map({ presetPill.getX(), presetPill.getY(), presetArrowW, presetPill.getHeight() }));
    presetNext.setBounds(map({ presetPill.getRight() - presetArrowW, presetPill.getY(), presetArrowW, presetPill.getHeight() }));
    presets.setBounds(map(presetPill.reduced(presetArrowW, 0.0f)));
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
    truePeakOverIndicator.setBounds(map(clipBox.withTrimmedTop(10.0f)));

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

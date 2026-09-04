#pragma once
#include <JuceHeader.h>

// Palette matched against Assets/Reference/NF_Limiter_approved_concept.png and
// Assets/ASSET_MANIFEST.md.
namespace NFColour
{
    static const juce::Colour graphite       (0xff0b0d11);   // page background
    static const juce::Colour panelBg        (0xff14171c);   // brushed-metal panel fill
    static const juce::Colour panelBorder    (0xff35393f);
    static const juce::Colour wellBg         (0xff090b0f);   // meter/GR/history well background
    static const juce::Colour wellBorder     (0xff2a2e35);

    static const juce::Colour inputCyan      (0xff00e4ff);
    static const juce::Colour inputGreen     (0xff16ff8b);
    static const juce::Colour outputPurple   (0xff9a55ff);
    static const juce::Colour outputPink     (0xffff39d1);
    static const juce::Colour grYellow       (0xffffd21c);
    static const juce::Colour grOrange       (0xffff7918);
    static const juce::Colour grRed          (0xffff2525);

    static const juce::Colour accentBlue     (0xff3f7bff);
    static const juce::Colour accentPurple   (0xffb257ff);
    static const juce::Colour textDim        (0xffa7adb8);

    // Exact knob accent colours (per-control identity on the lit dots/arc/marker).
    static const juce::Colour knobCyan       (0xff20ddf5);
    static const juce::Colour knobPurple     (0xff9554ff);
    static const juce::Colour knobOrange     (0xffff8518);
}

// Always returns a centred square, so a rotary knob is never stretched into an oval
// when the plugin window is resized to a non-square-cell layout.
static inline juce::Rectangle<int> nfSquare(juce::Rectangle<int> r)
{
    const int side = juce::jmin(r.getWidth(), r.getHeight());
    return juce::Rectangle<int>(side, side).withCentre(r.getCentre());
}

class NFLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    NFLookAndFeel()
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::textColourId, juce::Colours::white);
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff15181e));
        setColour(juce::TextButton::buttonOnColourId, NFColour::accentBlue);
        setColour(juce::TextButton::textColourOffId, NFColour::textDim);
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ToggleButton::tickColourId, NFColour::accentBlue);
    }

    // Centres the preset name in the combo box instead of the default left-aligned
    // layout, so "Default" reads centred on the same axis as the PEAK/TRUE PEAK/
    // LUFS column beneath it rather than looking shifted left within its own pill.
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(1, 1, box.getWidth() - 2, box.getHeight() - 2);
        label.setJustificationType(juce::Justification::centred);
    }

    // No native background/outline/arrow: the dedicated prev/next buttons either side
    // are the click affordance already, so a drawn arrow would only collide with the
    // now-centred preset name.
    void drawComboBox(juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override {}

    // Layered knob render, built in this order: drop shadow, 21 indicator dots, two
    // metal bezel rings, a 64-slice radial brushed-aluminium body, a radial light/dark
    // gradient, a corner highlight/shadow pair, and a short rounded marker. The accent
    // colour (set per-control via rotarySliderFillColourId) drives only the lit dots,
    // the lit arc, and the marker — the body itself is identical for every knob.
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        // 1. Square bounds — a knob is never stretched into an oval.
        const float diameter = (float) juce::jmin(w, h);
        auto bounds = nfSquare({ x, y, w, h }).toFloat().reduced(diameter * 0.14f);
        const auto centre = bounds.getCentre();
        const float radius = bounds.getWidth() * 0.5f;
        const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
        const float angle = startAngle + pos * (endAngle - startAngle);

        // 2. Soft drop shadow, offset down.
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillEllipse(bounds.translated(0.0f, diameter * 0.018f));

        // 3. 21 indicator dots from -135 to +135 degrees; lit ones (up to the current
        // value) use the accent colour, the rest are dark grey.
        const int numDots = 21;
        for (int i = 0; i < numDots; ++i)
        {
            const float t = (float) i / (float) (numDots - 1);
            const float a = startAngle + t * (endAngle - startAngle);
            const auto p = centre.getPointOnCircumference(radius * 1.22f, a);
            const float dotR = radius * 0.045f;
            g.setColour(t <= pos ? accent : juce::Colour(0xff35383e));
            g.fillEllipse(juce::Rectangle<float>(dotR * 2.0f, dotR * 2.0f).withCentre(p));
        }

        // 4. Two metal bezel rings: a thin silver outer ring, then a black inner ring
        // with a discreet white highlight top-left.
        g.setColour(juce::Colour(0xffaeb3ba));
        g.drawEllipse(bounds, juce::jmax(1.0f, radius * 0.035f));
        auto innerRingBounds = bounds.reduced(radius * 0.09f);
        g.setColour(juce::Colour(0xff0a0b0d));
        g.fillEllipse(innerRingBounds);
        {
            juce::Graphics::ScopedSaveState save(g);
            juce::Path ringClip; ringClip.addEllipse(innerRingBounds);
            g.reduceClipRegion(ringClip.getBounds().getSmallestIntegerContainer());
            g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.22f),
                                                    innerRingBounds.getX(), innerRingBounds.getY(),
                                                    juce::Colours::white.withAlpha(0.0f),
                                                    innerRingBounds.getCentreX(), innerRingBounds.getCentreY(), true));
            g.fillEllipse(innerRingBounds);
        }

        // 5. Smooth metal body — a plain diagonal gradient, no radial slices/spokes.
        auto face = bounds.reduced(radius * 0.20f);
        const auto faceCentre = face.getCentre();
        const float faceRadius = face.getWidth() * 0.5f;

        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffcdd2d8),
                                                face.getX() + face.getWidth() * 0.2f, face.getY() + face.getHeight() * 0.15f,
                                                juce::Colour(0xff35383e),
                                                face.getRight() - face.getWidth() * 0.1f, face.getBottom(), false));
        g.fillEllipse(face);
        {
            juce::Graphics::ScopedSaveState save(g);
            juce::Path faceClip; faceClip.addEllipse(face);
            g.reduceClipRegion(faceClip.getBounds().getSmallestIntegerContainer());

            // Soft centre-to-edge shading, then a corner highlight and a corner shadow.
            g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.1f), faceCentre.x, faceCentre.y,
                                                    juce::Colours::black.withAlpha(0.22f), faceCentre.x, face.getY(), true));
            g.fillEllipse(face);
            g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.45f),
                                                    face.getX() + face.getWidth() * 0.18f, face.getY() + face.getHeight() * 0.16f,
                                                    juce::Colours::white.withAlpha(0.0f),
                                                    faceCentre.x, faceCentre.y, true));
            g.fillEllipse(face);
            g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.3f),
                                                    face.getRight() - face.getWidth() * 0.18f, face.getBottom() - face.getHeight() * 0.16f,
                                                    juce::Colours::black.withAlpha(0.0f),
                                                    faceCentre.x, faceCentre.y, true));
            g.fillEllipse(face);
        }

        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.drawEllipse(face, 1.2f);

        // 8-9. Marker: a short, thick, rounded white line from 42% to 76% of the
        // radius — never a line crossing the full centre.
        const auto markerStart = faceCentre.getPointOnCircumference(faceRadius * 0.42f, angle);
        const auto markerEnd = faceCentre.getPointOnCircumference(faceRadius * 0.76f, angle);
        juce::Path marker;
        marker.startNewSubPath(markerStart);
        marker.lineTo(markerEnd);
        g.setColour(juce::Colours::white);
        g.strokePath(marker, juce::PathStrokeType(juce::jmax(2.5f, radius * 0.075f),
                                                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& btn, bool highlighted, bool) override
    {
        auto bounds = btn.getLocalBounds().toFloat().reduced(1.0f);
        const bool on = btn.getToggleState();
        const float corner = bounds.getHeight() * 0.5f;
        const auto accent = btn.findColour(juce::ToggleButton::tickColourId);

        if (on)
        {
            g.setColour(accent.withAlpha(0.30f));
            g.fillRoundedRectangle(bounds.expanded(3.5f), corner + 3.0f);
            g.setColour(accent.withAlpha(0.20f));
            g.fillRoundedRectangle(bounds.expanded(1.5f), corner + 1.5f);
        }
        g.setColour(on ? accent : juce::Colour(0xff14171c));
        g.fillRoundedRectangle(bounds, corner);
        g.setColour(on ? juce::Colours::white.withAlpha(0.5f) : juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(bounds, corner, 1.0f);
        g.setColour(on ? juce::Colours::white : NFColour::textDim.withAlpha(highlighted ? 1.0f : 0.85f));
        g.setFont(juce::Font(juce::FontOptions(juce::jmax(11.0f, bounds.getHeight() * 0.42f), juce::Font::bold)));
        g.drawText(btn.getButtonText(), bounds.toNearestInt(), juce::Justification::centred);
    }

    // Below: a passive, non-interactive warning indicator — not a button, no hover/
    // pressed/toggle state, no clip control of any kind.
    static void paintTruePeakOverIndicator(juce::Graphics& g, juce::Rectangle<float> bounds, bool lit)
    {
        auto textArea = bounds.removeFromBottom(bounds.getHeight() * 0.54f);
        auto ledArea = bounds;
        const float d = juce::jmin(ledArea.getWidth(), ledArea.getHeight()) * 0.32f;
        juce::Rectangle<float> led(d, d);
        led.setCentre(ledArea.getCentreX(), ledArea.getCentreY());

        if (lit)
        {
            // Lit: a defined, elegant red disc with a small centre highlight for a
            // physical-LED look, and only a thin (2-3px), low-opacity glow at the rim
            // — deliberately no large soft radial halo around the component.
            g.setColour(juce::Colour(0xffe0221a).withAlpha(0.22f));
            g.fillEllipse(led.expanded(2.5f));
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xffff5a48), led.getCentreX(), led.getCentreY(),
                                                    juce::Colour(0xffb31410), led.getX(), led.getY(), true));
            g.fillEllipse(led);
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.fillEllipse(juce::Rectangle<float>(d * 0.14f, d * 0.14f).withCentre(led.getCentre()));
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.drawEllipse(led, 1.0f);
        }
        else
        {
            // Off: a dark, discreet, clearly-passive wine-red disc — no glow at all.
            g.setColour(juce::Colour(0xff2a0a0c));
            g.fillEllipse(led);
            g.setColour(juce::Colours::black.withAlpha(0.65f));
            g.drawEllipse(led, 1.0f);
        }

        g.setColour(lit ? juce::Colour(0xffd4392c) : NFColour::textDim.withAlpha(0.55f));
        g.setFont(juce::Font(juce::FontOptions(juce::jmax(8.5f, textArea.getHeight() * 0.30f), juce::Font::bold)).withExtraKerningFactor(0.04f));
        g.drawFittedText("TRUE PEAK\nOVER", textArea.toNearestInt(), juce::Justification::centred, 2);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& btn, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        auto bounds = btn.getLocalBounds().toFloat().reduced(1.0f);
        const bool on = btn.getToggleState();
        const float corner = juce::jmin(6.0f, bounds.getHeight() * 0.22f);

        if (btn.getComponentID() == "power")
        {
            // Deliberately understated: a dark purple housing with a thin purple rim,
            // never a large solid-bright rectangle — the glow is concentrated behind
            // the icon glyph itself, drawn afterwards in drawButtonText.
            g.setColour(juce::Colour(0xff1a1220));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(NFColour::accentPurple.withAlpha(on ? 0.28f : 0.10f));
            g.fillRoundedRectangle(bounds, corner);
            g.setColour(NFColour::accentPurple.withAlpha(on ? 0.85f : 0.35f));
            g.drawRoundedRectangle(bounds, corner, 1.3f);
            if (on)
            {
                const auto c = bounds.getCentre();
                g.setGradientFill(juce::ColourGradient(NFColour::accentPurple.withAlpha(0.6f), c.x, c.y,
                                                        NFColour::accentPurple.withAlpha(0.0f), c.x, bounds.getY(), true));
                g.fillEllipse(juce::Rectangle<float>(bounds.getHeight() * 1.6f, bounds.getHeight() * 1.6f).withCentre(c));
            }
            return;
        }

        if (btn.getComponentID() == "delta")
        {
            // DELTA/LISTEN is a monitoring toggle, deliberately never blue (True
            // Peak's own colour) so it can never be mistaken for a sonic control.
            // OFF: dark, discreet, with a small orange accent stripe hinting at what
            // the button will look like when engaged. ON: a dark-orange face with an
            // orange outline and a very subtle glow -- distinct but not alarming.
            if (on)
            {
                g.setColour(NFColour::knobOrange.withAlpha(0.20f));
                g.fillRoundedRectangle(bounds.expanded(2.5f), corner + 2.5f);
                g.setColour(juce::Colour(0xff5a3510));
                g.fillRoundedRectangle(bounds, corner);
                g.setColour(NFColour::knobOrange.withAlpha(0.85f));
                g.drawRoundedRectangle(bounds, corner, 1.3f);
            }
            else
            {
                g.setColour(juce::Colour(down ? 0xff1c2028 : 0xff15181e));
                g.fillRoundedRectangle(bounds, corner);
                g.setColour(juce::Colours::black.withAlpha(0.7f));
                g.drawRoundedRectangle(bounds, corner, 1.0f);
                g.setColour(NFColour::knobOrange.withAlpha(0.5f));
                g.fillRect(bounds.getX() + corner * 0.5f, bounds.getBottom() - 2.0f, bounds.getWidth() - corner, 2.0f);
                if (highlighted)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.06f));
                    g.fillRoundedRectangle(bounds, corner);
                }
            }
            return;
        }

        const auto accent = btn.findColour(juce::TextButton::buttonOnColourId);

        if (on)
        {
            g.setColour(accent.withAlpha(0.30f));
            g.fillRoundedRectangle(bounds.expanded(3.0f), corner + 3.0f);
            g.setColour(accent.withAlpha(0.18f));
            g.fillRoundedRectangle(bounds.expanded(1.2f), corner + 1.2f);
        }
        g.setColour(on ? accent : juce::Colour(down ? 0xff1c2028 : 0xff15181e));
        g.fillRoundedRectangle(bounds, corner);
        if (! on)
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawLine(bounds.getX() + corner, bounds.getY() + 1.0f, bounds.getRight() - corner, bounds.getY() + 1.0f, 1.0f);
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.drawLine(bounds.getX() + corner, bounds.getBottom() - 0.6f, bounds.getRight() - corner, bounds.getBottom() - 0.6f, 1.0f);
        }
        g.setColour(on ? juce::Colours::white.withAlpha(0.5f) : juce::Colours::black.withAlpha(0.7f));
        g.drawRoundedRectangle(bounds, corner, 1.0f);
        if (highlighted && ! on)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, corner);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& btn, bool, bool) override
    {
        g.setColour(btn.getToggleState() ? juce::Colours::white : NFColour::textDim);

        if (btn.getComponentID() == "character")
        {
            // CLEAN / PUNCH / LOUD share one axis, one width and must render at one
            // shared size, spelled out in full — never abbreviated or ellipsised. The
            // size is derived from the widest label (PUNCH) so all three match exactly,
            // regardless of which button is actually being painted right now.
            const float availableWidth = juce::jmax(1.0f, (float) btn.getWidth() - 10.0f);
            auto fits = [&] (float size)
            {
                return juce::GlyphArrangement::getStringWidth(juce::Font(juce::FontOptions(size, juce::Font::bold)), "PUNCH") <= availableWidth;
            };

            float fontSize = juce::jlimit(9.0f, 15.0f, (float) btn.getHeight() * 0.34f);
            while (fontSize > 8.0f && ! fits(fontSize)) fontSize -= 0.5f;

            float hScale = 1.0f;
            const float widestAtSize = juce::GlyphArrangement::getStringWidth(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)), "PUNCH");
            if (widestAtSize > availableWidth)
                hScale = juce::jmax(0.9f, availableWidth / widestAtSize);

            auto f = juce::Font(juce::FontOptions(fontSize, juce::Font::bold));
            f.setHorizontalScale(hScale);
            g.setFont(f);
            g.drawText(btn.getButtonText(), btn.getLocalBounds(), juce::Justification::centred, false);
            return;
        }

        if (btn.getComponentID() == "oversampling")
        {
            // Same row, same button height as CHARACTER — 1x/2x/4x/8x must read at the
            // identical point size as CLEAN/PUNCH/LOUD, not the generic per-button size.
            const float fontSize = juce::jlimit(9.0f, 15.0f, (float) btn.getHeight() * 0.34f);
            g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)));
            g.drawText(btn.getButtonText(), btn.getLocalBounds(), juce::Justification::centred, false);
            return;
        }

        const float fontSize = juce::jlimit(12.0f, 22.0f, (float) btn.getHeight() * 0.4f);
        g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::bold)));
        g.drawText(btn.getButtonText(), btn.getLocalBounds(), juce::Justification::centred);
    }
};

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "DonationLinks.h"
#include "SupportLinks.h"
#include "TempoSync.h"
#include "UpdateChecker.h"
#include "UiAssets.h"

#include <cmath>
#include <cstdint>

namespace
{
constexpr int contentWidth = 1200;
constexpr int contentHeight = 860;
constexpr int panelInset = 24;
constexpr int panelWidth = contentWidth + panelInset * 2;
constexpr int panelHeight = contentHeight + panelInset * 2;
constexpr float firstColumn = 142.0f;
constexpr float secondColumn = 428.0f;
constexpr float thirdColumn = 714.0f;
constexpr float ledColumn = 907.0f;
constexpr float outputColumn = 1058.0f;
constexpr float rightAreaX = 941.0f;
constexpr float modalOffsetX = (contentWidth - 1040.0f) * 0.5f;
constexpr float topHardwareY = 58.0f;
constexpr float pi = juce::MathConstants<float>::pi;
constexpr float rotaryStart = 1.25f * pi;
constexpr float rotaryEnd = 2.75f * pi;
const juce::Colour ink (0xffdce1d8);
constexpr float columnHaloRadius = 29.0f;
constexpr float upperHaloRadius = 54.0f;

struct KnobDescription
{
    const char* id;
    const char* name;
    float x;
    float y;
    const char* unit;
};

constexpr std::array<KnobDescription, 7> knobDescriptions {{
    { "vco_freq", "VCO Freq", firstColumn, 252.0f, "Hz" },
    { "lfo_mod",  "LFO mod", secondColumn, 252.0f, "%" },
    { "lfo_freq", "LFO freq", thirdColumn, 252.0f, "Hz" },
    { "master",   "Master",  outputColumn, 252.0f, "%" },
    { "mix",      "Mix",     firstColumn, 520.0f, "%" },
    { "delay",    "Delay",   secondColumn, 520.0f, "ms" },
    { "repeats",  "Repeats", thirdColumn, 520.0f, "%" }
}};

constexpr std::array<const char*, 4> buttonIds {{
    "lfo_rounded", "lfo_differentiated", "sync", "oscillation"
}};
constexpr std::array<const char*, 3> modeIds {{
    "lfo_tempo_sync", "delay_tempo_sync", "fx_post"
}};

juce::Font panelFont (float height, bool bold = false, float width = 1.0f)
{
    auto result = juce::Font ("Arial", height, bold ? juce::Font::bold : juce::Font::plain);
    result.setHorizontalScale (width);
    return result;
}

juce::Point<float> radialPoint (juce::Point<float> centre, float radius, float angle)
{
    return { centre.x + radius * std::sin (angle), centre.y - radius * std::cos (angle) };
}

juce::Rectangle<float> disc (juce::Point<float> centre, float radius)
{
    return { centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f };
}

void label (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds,
            float height = 22.0f, bool bold = false, float width = 1.0f)
{
    g.setColour (ink);
    g.setFont (panelFont (height, bold, width));
    g.drawText (text, bounds, juce::Justification::centred, false);
}

juce::Path scallopedDisc (juce::Point<float> centre, float radius, int lobes, float phase)
{
    juce::Path shape;
    constexpr int segments = 240;
    for (int i = 0; i <= segments; ++i)
    {
        const auto angle = juce::MathConstants<float>::twoPi * static_cast<float> (i) / segments;
        const auto r = radius * (0.947f + 0.053f * std::cos (static_cast<float> (lobes) * (angle - phase)));
        const auto point = radialPoint (centre, r, angle);
        if (i == 0)
            shape.startNewSubPath (point);
        else
            shape.lineTo (point);
    }
    shape.closeSubPath();
    return shape;
}

void metalDisc (juce::Graphics& g, juce::Point<float> centre, float radius,
                bool engraved, float brightness = 1.0f)
{
    // Angular reflections and fine circles reproduce the machined silver cap.
    for (int i = 0; i < 96; ++i)
    {
        const auto angle = static_cast<float> (i) * juce::MathConstants<float>::twoPi / 96.0f;
        const auto shade = juce::jlimit (0.18f, 0.98f,
            brightness * (0.70f + 0.23f * std::cos (2.0f * (angle - 0.78f))
                               + 0.05f * std::sin (angle * 5.0f)));
        juce::Path segment;
        segment.addPieSegment (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f,
                               angle, angle + juce::MathConstants<float>::twoPi / 96.0f + 0.003f, 0.0f);
        g.setColour (juce::Colour::fromFloatRGBA (shade * 0.99f, shade, shade * 0.97f, 1.0f));
        g.fillPath (segment);
    }

    if (engraved)
    {
        for (float r = 1.0f; r < radius - 1.0f; r += 1.12f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.36f));
            g.drawEllipse (disc (centre, r), 0.35f);
            g.setColour (juce::Colours::white.withAlpha (0.38f));
            g.drawEllipse (disc (centre, r + 0.42f), 0.32f);
        }
    }

    g.setColour (juce::Colour (0xfffafbf2).withAlpha (0.80f));
    g.drawEllipse (disc (centre, radius - 0.35f), 0.7f);
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.drawEllipse (disc (centre, radius + 0.3f), 0.75f);
}

void drawKnob (juce::Graphics& g, juce::Point<float> centre, float radius, float angle,
               bool keyboardFocus)
{
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    g.fillEllipse (disc (centre.translated (2.5f, 5.5f), radius + 4.0f));
    g.setColour (juce::Colours::black.withAlpha (0.52f));
    g.fillEllipse (disc (centre.translated (1.0f, 3.5f), radius + 1.5f));

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff424743), centre.x - radius, centre.y - radius,
                                            juce::Colour (0xff070909), centre.x + radius, centre.y + radius, false));
    g.fillEllipse (disc (centre, radius));
    g.setColour (juce::Colour (0xff585d57).withAlpha (0.6f));
    g.drawEllipse (disc (centre, radius - 0.7f), 0.8f);
    g.setColour (juce::Colours::black);
    g.fillEllipse (disc (centre, radius - 2.4f));

    const auto bodyRadius = radius - 3.3f;
    const auto body = scallopedDisc (centre.translated (0.0f, -1.6f), bodyRadius, 10, angle);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff434744), centre.x - radius, centre.y - radius,
                                            juce::Colour (0xff0b0d0c), centre.x + radius * 0.5f,
                                            centre.y + radius, false));
    g.fillPath (body);
    g.setColour (juce::Colour (0xff73766f).withAlpha (0.34f));
    g.strokePath (body, juce::PathStrokeType (0.9f));

    // Short highlights follow the scallops; the black body remains visibly fluted.
    for (int i = 0; i < 10; ++i)
    {
        const auto a = angle + static_cast<float> (i) * juce::MathConstants<float>::twoPi / 10.0f;
        juce::Path flute;
        flute.addCentredArc (centre.x, centre.y - 1.6f, bodyRadius * 0.965f, bodyRadius * 0.965f,
                             0.0f, a - 0.12f, a + 0.11f, true);
        const auto light = 0.10f + 0.13f * (0.5f + 0.5f * std::cos (a + 0.9f));
        g.setColour (juce::Colours::white.withAlpha (light));
        g.strokePath (flute, juce::PathStrokeType (1.3f));
    }

    const auto capRadius = radius * 0.59f;
    const auto capCentre = centre.translated (0.0f, -1.6f);
    g.setColour (juce::Colours::black);
    g.fillEllipse (disc (centre.translated (0.0f, -1.2f), capRadius + 2.1f));
    {
        juce::Graphics::ScopedSaveState capState (g);
        g.addTransform (juce::AffineTransform::rotation (angle, capCentre.x, capCentre.y));
        metalDisc (g, capCentre, capRadius, true);

        // An asymmetric reflection and a fine machining trace make rotation visible
        // even though the concentric grooves alone are rotationally symmetric.
        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.fillEllipse (capCentre.x - capRadius * 0.32f, capCentre.y - capRadius * 0.78f,
                       capRadius * 0.49f, capRadius * 0.14f);
        juce::Path machiningTrace;
        machiningTrace.addCentredArc (capCentre.x - 0.6f, capCentre.y + 0.4f,
                                      capRadius * 0.80f, capRadius * 0.80f,
                                      0.0f, -0.84f, -0.34f, true);
        g.setColour (juce::Colour (0xff434b43).withAlpha (0.48f));
        g.strokePath (machiningTrace, juce::PathStrokeType (0.58f));
    }

    const auto pointerStart = radialPoint (centre, radius * 0.69f, angle);
    const auto pointerEnd = radialPoint (centre, radius * 0.91f, angle);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawLine (pointerStart.x + 0.8f, pointerStart.y + 1.0f,
                pointerEnd.x + 0.8f, pointerEnd.y + 1.0f, radius * 0.102f);
    g.setColour (juce::Colour (0xfff4f4df));
    g.drawLine (pointerStart.x, pointerStart.y, pointerEnd.x, pointerEnd.y, radius * 0.075f);

    if (keyboardFocus)
    {
        g.setColour (ink.withAlpha (0.7f));
        g.drawEllipse (disc (centre, radius + 5.5f), 1.1f);
    }
}

void drawToggle (juce::Graphics& g, juce::Point<float> centre, float radius,
                 const juce::Drawable* artwork, bool keyboardFocus = false)
{
    if (artwork != nullptr)
    {
        // Map the SVG's full 112x112 coordinate system, not its content bounds.
        // Both states retain the same physical pivot at (56,56).
        const auto scale = radius / 31.0f;
        artwork->draw (g, 1.0f, juce::AffineTransform::scale (scale).translated (
            centre.x - 56.0f * scale, centre.y - 56.0f * scale));
    }
    if (keyboardFocus)
    {
        g.setColour (ink.withAlpha (0.7f));
        g.drawEllipse (disc (centre, radius + 5.0f), 1.0f);
    }
}
void drawOscillation (juce::Graphics& g, juce::Point<float> centre, float radius,
                      bool latched, bool held, bool keyboardFocus)
{
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillEllipse (disc (centre.translated (2.5f, 4.5f), radius + 2.0f));
    metalDisc (g, centre, radius, false, 0.98f);
    g.setColour (juce::Colour (0xff171a18));
    g.fillEllipse (disc (centre, radius * 0.83f));
    g.setColour (juce::Colour (0xffe3e6d8));
    g.drawEllipse (disc (centre, radius * 0.82f), 1.0f);

    // Latched state is a mechanical depression, never an illuminated ring.
    const auto capCentre = centre.translated (0.0f, latched || held ? 2.4f : -1.0f);
    const auto capRadius = radius * (latched || held ? 0.72f : 0.75f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (latched || held ? 0xffa2a79f : 0xffeef0e7),
                                            capCentre.x - capRadius, capCentre.y - capRadius,
                                            juce::Colour (0xff6d756e), capCentre.x + capRadius,
                                            capCentre.y + capRadius, false));
    g.fillEllipse (disc (capCentre, capRadius));
    g.setColour (juce::Colour (0xfff4f5eb).withAlpha (0.75f));
    g.drawEllipse (disc (capCentre, capRadius - 0.6f), 0.9f);
    for (int i = 0; i < 29; ++i)
    {
        const auto y = (static_cast<float> (i) / 28.0f * 2.0f - 1.0f) * (capRadius - 1.7f);
        const auto halfWidth = std::sqrt (juce::jmax (0.0f, capRadius * capRadius - y * y)) - 1.2f;
        g.setColour (juce::Colours::white.withAlpha (i % 3 == 0 ? 0.09f : 0.035f));
        g.drawLine (capCentre.x - halfWidth, capCentre.y + y,
                    capCentre.x + halfWidth, capCentre.y + y, 0.38f);
    }

    if (keyboardFocus)
    {
        g.setColour (ink.withAlpha (0.75f));
        g.drawEllipse (disc (centre, radius + 5.0f), 1.0f);
    }
}

void drawScale (juce::Graphics& g, juce::Point<float> centre)
{
    g.setColour (ink);
    for (int i = 0; i <= 27; ++i)
    {
        const auto a = rotaryStart + (rotaryEnd - rotaryStart) * static_cast<float> (i) / 27.0f;
        const bool major = i % 3 == 0;
        const auto inner = radialPoint (centre, major ? 66.0f : 70.0f, a);
        const auto outer = radialPoint (centre, 81.0f, a);
        g.drawLine (inner.x, inner.y, outer.x, outer.y, major ? 4.5f : 2.5f);
    }
    g.setFont (panelFont (15.0f, true, 1.15f));
    for (int i = 0; i < 10; ++i)
    {
        const auto a = rotaryStart + (rotaryEnd - rotaryStart) * static_cast<float> (i) / 9.0f;
        const auto point = radialPoint (centre, 96.0f, a);
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (point.x - 16.0f, point.y - 8.5f, 32.0f, 17.0f),
                    juce::Justification::centred, false);
    }
}

void drawJack (juce::Graphics& g, juce::Point<float> centre)
{
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (disc (centre.translated (1.5f, 3.5f), 40.5f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff454b42), centre.x - 30.0f, centre.y - 35.0f,
                                            juce::Colour (0xff090b09), centre.x + 30.0f, centre.y + 35.0f, false));
    g.fillEllipse (disc (centre, 38.5f));
    g.setColour (juce::Colour (0xff141713));
    g.fillEllipse (disc (centre, 33.5f));

    juce::Path nut;
    nut.addPolygon (centre, 6, 32.0f, 0.18f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff6f2d7), centre.x - 23.0f, centre.y - 25.0f,
                                            juce::Colour (0xff646a5c), centre.x + 25.0f, centre.y + 29.0f, false));
    g.fillPath (nut);
    g.setColour (juce::Colour (0xffe7e8d1));
    g.strokePath (nut, juce::PathStrokeType (1.1f));
    g.setColour (juce::Colour (0xff0b0e0b));
    g.fillEllipse (disc (centre, 21.5f));
    g.setColour (juce::Colour (0xffd1d5bd));
    g.drawEllipse (disc (centre, 21.2f), 1.4f);
    g.setColour (juce::Colour (0xff1f261d));
    g.drawEllipse (disc (centre.translated (0.0f, 2.0f), 17.5f), 1.0f);
    g.setColour (juce::Colour (0xffaeb69d).withAlpha (0.65f));
    g.drawLine (centre.x - 14.5f, centre.y + 12.0f, centre.x - 7.0f, centre.y + 17.5f, 1.2f);
}

void drawIndicator (juce::Graphics& g, juce::Point<float> centre, float brightness = 0.0f)
{
    if (brightness > 0.0f)
    {
        const juce::Colour blue (0xff2458d7);
        juce::ColourGradient halo (blue.withAlpha (brightness * 0.19f), centre.x, centre.y,
                                   blue.withAlpha (0.0f), centre.x + columnHaloRadius, centre.y, true);
        halo.addColour (0.27, blue.withAlpha (brightness * 0.16f));
        halo.addColour (0.52, blue.withAlpha (brightness * 0.085f));
        halo.addColour (0.77, blue.withAlpha (brightness * 0.025f));
        g.setGradientFill (halo);
        g.fillEllipse (disc (centre, columnHaloRadius));
    }
    g.setColour (juce::Colours::black);
    g.fillEllipse (disc (centre.translated (0.7f, 1.1f), 17.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc0c3b7), centre.x - 13.0f, centre.y - 13.0f,
                                            juce::Colour (0xff454b43), centre.x + 13.0f, centre.y + 13.0f, false));
    g.fillEllipse (disc (centre, 16.0f));
    g.setColour (juce::Colour (0xff161a17));
    g.fillEllipse (disc (centre, 13.1f));
    const auto lensColour = juce::Colour (0xff1d263e).interpolatedWith (juce::Colour (0xff367dff), brightness);
    g.setGradientFill (juce::ColourGradient (lensColour.darker (0.62f), centre.x, centre.y - 7.0f,
                                            lensColour, centre.x, centre.y + 6.0f, false));
    g.fillEllipse (disc (centre, 7.2f));
    g.setColour (juce::Colour (0xffe5f2fa).withAlpha (0.16f + brightness * 0.42f));
    g.fillEllipse (centre.x - 3.5f, centre.y - 4.0f, 4.0f, 2.5f);
}

void drawUpperIndicator (juce::Graphics& g, juce::Point<float> centre, float brightness = 0.0f)
{
    if (brightness > 0.0f)
    {
        const juce::Colour white (0xffe5edf5);
        juce::ColourGradient halo (white.withAlpha (brightness * 0.16f), centre.x, centre.y,
                                   white.withAlpha (0.0f), centre.x + upperHaloRadius, centre.y, true);
        halo.addColour (0.33, white.withAlpha (brightness * 0.13f));
        halo.addColour (0.58, white.withAlpha (brightness * 0.065f));
        halo.addColour (0.81, white.withAlpha (brightness * 0.018f));
        g.setGradientFill (halo);
        g.fillEllipse (disc (centre, upperHaloRadius));
    }
    g.setColour (juce::Colour (0xff090d09));
    g.fillEllipse (disc (centre.translated (0.7f, 2.0f), 37.0f));
    g.setColour (juce::Colour (0xff30372f));
    g.drawEllipse (disc (centre, 32.0f), 2.0f);
    const auto lit = juce::Colour (0xff303731).interpolatedWith (juce::Colour (0xffe8ecea), brightness);
    const auto edge = juce::Colour (0xff1b211c).interpolatedWith (juce::Colour (0xff7c8a90), brightness);
    g.setGradientFill (juce::ColourGradient (lit, centre.x - 8.0f, centre.y - 10.0f,
                                            edge, centre.x + 20.0f, centre.y + 27.0f, true));
    g.fillEllipse (disc (centre, 28.0f));
    g.setColour (juce::Colour (0xfff0f5f7).withAlpha (0.17f + brightness * 0.49f));
    g.fillEllipse (centre.x - 5.0f, centre.y - 8.5f, 5.5f, 7.5f);
}

void drawScrew (juce::Graphics& g, juce::Point<float> centre)
{
    g.setColour (juce::Colour (0xff090c09));
    g.fillEllipse (disc (centre.translated (0.6f, 1.0f), 8.6f));
    metalDisc (g, centre, 7.7f, false, 0.85f);
    g.setColour (juce::Colour (0xff272d24));
    g.drawLine (centre.x - 4.2f, centre.y + 2.8f, centre.x + 4.2f, centre.y - 2.8f, 1.5f);
}

void drawLogo (juce::Graphics& g, const juce::Drawable* artwork, juce::Rectangle<float> area)
{
    if (artwork != nullptr)
        artwork->drawWithin (g, area, juce::RectanglePlacement::centred, 1.0f);
}
void paintWoodFrame (juce::Graphics& g)
{
    constexpr float thickness = 16.0f;
    const auto w = static_cast<float> (panelWidth);
    const auto h = static_cast<float> (panelHeight);
    std::array<juce::Path, 4> rails;
    const auto quad = [] (juce::Path& path, juce::Point<float> a, juce::Point<float> b,
                         juce::Point<float> c, juce::Point<float> d)
    {
        path.startNewSubPath (a);
        path.lineTo (b);
        path.lineTo (c);
        path.lineTo (d);
        path.closeSubPath();
    };
    quad (rails[0], { 0.0f, 0.0f }, { w, 0.0f }, { w - thickness, thickness }, { thickness, thickness });
    quad (rails[1], { 0.0f, h }, { thickness, h - thickness }, { w - thickness, h - thickness }, { w, h });
    quad (rails[2], { 0.0f, 0.0f }, { thickness, thickness }, { thickness, h - thickness }, { 0.0f, h });
    quad (rails[3], { w, 0.0f }, { w, h }, { w - thickness, h - thickness }, { w - thickness, thickness });

    for (size_t rail = 0; rail < rails.size(); ++rail)
    {
        const bool horizontal = rail < 2;
        const bool reverse = rail == 1 || rail == 3;
        const auto outside = horizontal ? (reverse ? h : 0.0f) : (reverse ? w : 0.0f);
        const auto inside = outside + (reverse ? -thickness : thickness);
        juce::ColourGradient stain (juce::Colour (0xff30362f), horizontal ? 0.0f : outside,
                                     horizontal ? outside : 0.0f, juce::Colour (0xff0b0f0b),
                                     horizontal ? 0.0f : inside, horizontal ? inside : 0.0f, false);
        stain.addColour (0.24, juce::Colour (0xff232922));
        stain.addColour (0.74, juce::Colour (0xff151b14));
        g.setGradientFill (stain);
        g.fillPath (rails[rail]);

        juce::Graphics::ScopedSaveState railState (g);
        g.reduceClipRegion (rails[rail]);
        juce::Random grain (static_cast<juce::int64> (0x6c6d7a + rail * 31));
        for (int line = 0; line < 23; ++line)
        {
            const auto depth = 0.6f + static_cast<float> (line) * 0.67f;
            const auto axis = outside + (reverse ? -depth : depth);
            const auto bend = (grain.nextFloat() - 0.5f) * 3.2f;
            juce::Path fibre;
            if (horizontal)
            {
                fibre.startNewSubPath (-8.0f, axis);
                fibre.cubicTo (w * 0.23f, axis + bend, w * 0.63f, axis - bend * 0.65f,
                               w + 8.0f, axis + bend * 0.18f);
            }
            else
            {
                fibre.startNewSubPath (axis, -8.0f);
                fibre.cubicTo (axis + bend, h * 0.27f, axis - bend * 0.65f, h * 0.68f,
                               axis + bend * 0.18f, h + 8.0f);
            }
            g.setColour (line % 3 == 0 ? juce::Colour (0xff020602).withAlpha (0.28f)
                                      : juce::Colour (0xff697161).withAlpha (0.075f));
            g.strokePath (fibre, juce::PathStrokeType (line % 3 == 0 ? 0.65f : 0.40f));
        }
        for (int pore = 0; pore < 45; ++pore)
        {
            const auto along = grain.nextFloat() * (horizontal ? w : h);
            const auto depth = 1.5f + grain.nextFloat() * 13.0f;
            const auto axis = outside + (reverse ? -depth : depth);
            const auto length = 3.0f + grain.nextFloat() * 12.0f;
            g.setColour (juce::Colour (0xff000400).withAlpha (0.22f));
            if (horizontal)
                g.drawLine (along, axis, along + length, axis + 0.25f, 0.35f);
            else
                g.drawLine (axis, along, axis + 0.25f, along + length, 0.35f);
        }
    }

    // Fine bevels and diagonal joints keep the stained timber restrained and dark.
    g.setColour (juce::Colour (0xff50594a).withAlpha (0.50f));
    g.drawRect (0.7f, 0.7f, w - 1.4f, h - 1.4f, 1.2f);
    g.setColour (juce::Colour (0xff020502).withAlpha (0.90f));
    g.drawRect (thickness - 1.0f, thickness - 1.0f, w - thickness * 2.0f + 2.0f,
                h - thickness * 2.0f + 2.0f, 1.6f);
    g.setColour (juce::Colour (0xff606b57).withAlpha (0.18f));
    g.drawRect (thickness + 0.6f, thickness + 0.6f, w - thickness * 2.0f - 1.2f,
                h - thickness * 2.0f - 1.2f, 0.7f);
    g.setColour (juce::Colour (0xff000300).withAlpha (0.85f));
    g.drawLine (0.0f, 0.0f, thickness, thickness, 0.85f);
    g.drawLine (w, 0.0f, w - thickness, thickness, 0.85f);
    g.drawLine (0.0f, h, thickness, h - thickness, 0.85f);
    g.drawLine (w, h, w - thickness, h - thickness, 0.85f);
}

void paintPanel (juce::Graphics& g, const juce::Drawable* powerArtwork)
{
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff252925), 0.0f, 0.0f,
                                            juce::Colour (0xff121612), 340.0f, 860.0f, false));
    g.fillRect (-8.0f, -8.0f, contentWidth + 16.0f, contentHeight + 16.0f);

    // Fixed-seed horizontal brush grain is generated entirely with native Graphics.
    juce::Random random (0x2a4112);
    for (float y = 0.0f; y < contentHeight; y += 0.62f)
    {
        const auto light = random.nextFloat();
        g.setColour (juce::Colours::white.withAlpha (0.012f + light * 0.035f));
        g.drawLine (0.0f, y, static_cast<float> (contentWidth), y, 0.23f);
        const auto start = random.nextFloat() * (contentWidth - 190.0f);
        g.setColour (juce::Colours::black.withAlpha (0.08f + random.nextFloat() * 0.09f));
        g.drawLine (start, y + 0.23f, juce::jmin (static_cast<float> (contentWidth), start + 50.0f + random.nextFloat() * 300.0f),
                    y + 0.23f, 0.20f);
    }
    for (int i = 0; i < 9000; ++i)
    {
        const auto x = random.nextFloat() * contentWidth;
        const auto y = random.nextFloat() * contentHeight;
        g.setColour ((i % 2 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (0.035f));
        g.drawLine (x, y, x + 0.8f + random.nextFloat() * 2.6f, y, 0.23f);
    }
    juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.035f), outputColumn, 360.0f,
                                juce::Colours::white.withAlpha (0.0f), 440.0f, 420.0f, false);
    g.setGradientFill (sheen);
    g.fillRect (-8.0f, -8.0f, contentWidth + 16.0f, contentHeight + 16.0f);
    g.setColour (ink.withAlpha (0.86f));
    for (const auto frame : std::array<juce::Rectangle<float>, 6> {{
             { 13.0f, 106.0f, 829.0f, 241.0f },
             { 13.0f, 355.0f, 829.0f, 261.0f },
             { 13.0f, 625.0f, 576.0f, 208.0f },
             { 598.0f, 625.0f, 244.0f, 208.0f },
             { rightAreaX, 106.0f, 247.0f, 241.0f },
             { rightAreaX, 355.0f, 247.0f, 261.0f }
         }})
        g.drawRoundedRectangle (frame, 30.0f, 1.8f);
    g.drawRoundedRectangle (883.0f, 106.0f, 48.0f, 727.0f, 24.0f, 1.8f);

    label (g, "DC 9V",      { 93.0f, -6.0f, 97.0f, 25.0f }, 19.0f, true);
    label (g, "power",      { 375.0f, -6.0f, 107.0f, 26.0f }, 22.0f, true);
    label (g, "stereo out", { 643.0f, -6.0f, 142.0f, 26.0f }, 22.0f, true);
    label (g, "linear out", { 987.0f, -6.0f, 141.0f, 26.0f }, 22.0f, true);

    // DC socket and polarity mark are intentionally non-interactive panel details.
    {
        juce::Graphics::ScopedSaveState dcState (g);
        g.addTransform (juce::AffineTransform::translation (firstColumn - 112.0f, -12.0f));
        const juce::Point<float> dc (112.0f, 70.0f);
        g.setColour (juce::Colour (0xff0b0e0b));
        g.fillEllipse (disc (dc.translated (0.0f, 2.0f), 31.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff353b33), dc.x - 22.0f, dc.y - 25.0f,
                                                juce::Colour (0xff151a14), dc.x + 24.0f, dc.y + 27.0f, false));
        g.fillEllipse (disc (dc, 29.5f));
        g.setColour (juce::Colour (0xff050805));
        g.fillEllipse (disc (dc.translated (0.0f, -2.0f), 15.5f));
        g.setColour (juce::Colour (0xff909880));
        g.fillEllipse (dc.x - 2.6f, dc.y + 6.3f, 5.2f, 6.0f);
        g.setColour (ink);
        g.drawEllipse (82.0f, 28.0f, 9.0f, 9.0f, 0.9f);
        g.drawLine (84.5f, 32.5f, 88.5f, 32.5f, 0.9f);
        g.drawLine (86.5f, 30.5f, 86.5f, 34.5f, 0.9f);
        g.drawEllipse (130.0f, 28.0f, 9.0f, 9.0f, 0.9f);
        g.drawLine (132.5f, 32.5f, 136.5f, 32.5f, 0.9f);
        g.drawLine (91.0f, 32.5f, 102.0f, 32.5f, 0.9f);
        g.drawLine (117.0f, 32.5f, 130.0f, 32.5f, 0.9f);
        juce::Path polarity;
        polarity.addCentredArc (109.5f, 32.5f, 7.5f, 5.0f, 0.0f, 0.5f * pi, 1.5f * pi, true);
        g.strokePath (polarity, juce::PathStrokeType (0.9f));
        g.fillEllipse (107.5f, 30.5f, 4.0f, 4.0f);
        g.drawLine (109.5f, 32.5f, 117.0f, 32.5f, 0.9f);
    }

    drawToggle (g, { secondColumn, topHardwareY }, 28.0f, powerArtwork);
    drawJack (g, { thirdColumn, topHardwareY });
    drawJack (g, { outputColumn, topHardwareY });

    for (const auto& item : knobDescriptions)
    {
        label (g, item.name, { item.x - 100.0f, item.y < 300.0f ? 114.0f : 373.0f, 200.0f, 28.0f }, 22.0f);
        drawScale (g, { item.x, item.y });
    }
    label (g, "Oscillation", { outputColumn - 100.5f, 373.0f, 201.0f, 28.0f }, 22.0f);
    label (g, "Sync", { thirdColumn - 70.0f, 638.0f, 140.0f, 31.0f }, 23.0f);
    label (g, "Off",  { thirdColumn - 55.0f, 784.0f, 110.0f, 29.0f }, 22.0f);
    label (g, juce::String::fromUTF8 (u8"Сигнал 2М"), { firstColumn - 97.0f, 784.0f, 194.0f, 30.0f }, 24.0f);

    g.setColour (ink);
    g.drawLine (209.0f, 727.0f, 362.0f, 727.0f, 3.2f);
    juce::Path rounded;
    rounded.startNewSubPath (59.0f, 728.0f);
    rounded.cubicTo (62.0f, 718.0f, 66.0f, 716.0f, 71.0f, 723.0f);
    rounded.cubicTo (75.0f, 733.0f, 82.0f, 734.0f, 87.0f, 723.0f);
    rounded.cubicTo (90.0f, 718.0f, 93.0f, 716.0f, 99.0f, 716.0f);
    g.strokePath (rounded, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    juce::Path differentiated;
    differentiated.startNewSubPath (361.0f, 660.0f);
    differentiated.lineTo (361.0f, 647.0f);
    differentiated.lineTo (367.0f, 652.0f);
    differentiated.lineTo (374.0f, 653.0f);
    differentiated.lineTo (374.0f, 667.0f);
    differentiated.lineTo (381.0f, 660.0f);
    differentiated.lineTo (391.0f, 660.0f);
    differentiated.lineTo (391.0f, 647.0f);
    g.strokePath (differentiated, juce::PathStrokeType (2.2f), juce::AffineTransform::translation (52.0f, 0.0f));
    juce::Path square;
    square.startNewSubPath (360.0f, 804.0f);
    square.lineTo (360.0f, 790.0f);
    square.lineTo (375.0f, 790.0f);
    square.lineTo (375.0f, 803.0f);
    square.lineTo (390.0f, 803.0f);
    square.lineTo (390.0f, 790.0f);
    g.strokePath (square, juce::PathStrokeType (2.4f), juce::AffineTransform::translation (52.0f, 0.0f));
    drawScrew (g, { 23.0f, 20.0f });
    drawScrew (g, { contentWidth - 25.0f, 21.0f });
    drawScrew (g, { 24.0f, 838.0f });
    drawScrew (g, { contentWidth - 24.0f, 838.0f });
   #if defined (SIGNAL2M_SHOW_BETA)
    if (SIGNAL2M_SHOW_BETA != 0)
        label (g, "BETA", { 12.0f, 44.0f, 55.0f, 22.0f }, 17.0f, true);
   #elif defined (JucePlugin_VersionCode)
    if ((JucePlugin_VersionCode >> 16) == 0)
        label (g, "BETA", { 12.0f, 44.0f, 55.0f, 22.0f }, 17.0f, true);
   #elif defined (JucePlugin_VersionString)
    if (juce::String (JucePlugin_VersionString).getIntValue() == 0)
        label (g, "BETA", { 12.0f, 44.0f, 55.0f, 22.0f }, 17.0f, true);
   #endif
}
} // namespace

class Signal2MAudioProcessorEditor::PanelLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PanelLookAndFeel()
    {
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff252b24));
        setColour (juce::TooltipWindow::textColourId, ink);
        setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff70786a));
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff20261f));
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff535e4d));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto size = static_cast<float> (juce::jmin (width, height));
        const auto scale = size / 128.0f;
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::scale (scale).translated (
            static_cast<float> (x) + (static_cast<float> (width) - size) * 0.5f,
            static_cast<float> (y) + (static_cast<float> (height) - size) * 0.5f));
        drawKnob (g, { 64.0f, 64.0f }, 55.0f, startAngle + position * (endAngle - startAngle),
                  slider.hasKeyboardFocus (true));
    }

    juce::Font getSliderPopupFont (juce::Slider&) override
    {
        return panelFont (16.0f);
    }
};

class Signal2MAudioProcessorEditor::Knob final : public juce::Slider
{
public:
    Knob (Signal2MAudioProcessorEditor& editor, juce::RangedAudioParameter& parameterToUse,
          const KnobDescription& description)
        : parameter (parameterToUse), owner (editor), parameterId (description.id),
          tempoCapable (juce::String (description.id) == "lfo_freq" || juce::String (description.id) == "delay"),
          delayControl (juce::String (description.id) == "delay")
    {
        setName (description.name);
        setTitle (description.name);
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setRotaryParameters (rotaryStart, rotaryEnd, true);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setMouseDragSensitivity (260);
        setPopupDisplayEnabled (true, false, &editor);
        setPopupMenuEnabled (false);
        setScrollWheelEnabled (true);
        setWantsKeyboardFocus (true);
        setDoubleClickReturnValue (true, parameter.convertFrom0to1 (parameter.getDefaultValue()));
        // The attachment supplies the processor's formatting; append its physical unit.
        setTextValueSuffix (" " + juce::String (description.unit));
        refreshTooltip();
    }

    juce::String getTextFromValue (double value) override
    {
        if (tempoCapable && tempoEnabled)
        {
            const auto normalised = parameter.convertTo0to1 (static_cast<float> (value));
            return signal2m::tempo::selectedDivision (normalised, delayControl).label;
        }
        return juce::Slider::getTextFromValue (value);
    }

    void setTempoSyncEnabled (bool enabled)
    {
        enabled = enabled && tempoCapable;
        if (tempoEnabled == enabled)
            return;
        tempoEnabled = enabled;
        updateText(); // JUCE updates an already open popup here, without changing the parameter.
        refreshTooltip();
    }

    void valueChanged() override
    {
        refreshTooltip();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            owner.showParameterMenu (parameter);
        else
            juce::Slider::mouseDown (event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            juce::Slider::mouseUp (event);
    }

    juce::RangedAudioParameter& parameter;

private:
    void refreshTooltip()
    {
        juce::String description, selected;
        if (tempoEnabled)
        {
            description = juce::String::fromUTF8 (delayControl
                ? u8"Длительность задержки по темпу проекта."
                : u8"Длительность цикла низкочастотного генератора по темпу проекта.");
            selected = getTextFromValue (getValue());
            if (selected.endsWithChar ('T'))
                selected = selected.dropLastCharacters (1) + juce::String::fromUTF8 (u8" (триоль)");
        }
        else if (parameterId == "vco_freq")
        {
            description = juce::String::fromUTF8 (u8"Частота основного генератора.");
            selected = juce::String (getValue(), 1) + juce::String::fromUTF8 (u8" Гц");
        }
        else if (parameterId == "lfo_freq")
        {
            description = juce::String::fromUTF8 (u8"Частота низкочастотного генератора.");
            selected = juce::String (getValue(), 2) + juce::String::fromUTF8 (u8" Гц");
        }
        else if (parameterId == "delay")
        {
            description = juce::String::fromUTF8 (u8"Время между повторами эха.");
            selected = juce::String (getValue(), 1) + juce::String::fromUTF8 (u8" мс");
        }
        else
        {
            if (parameterId == "lfo_mod")
                description = juce::String::fromUTF8 (u8"Глубина модуляции частоты основного генератора.");
            else if (parameterId == "master")
                description = juce::String::fromUTF8 (u8"Общий уровень выходного звука.");
            else if (parameterId == "mix")
                description = juce::String::fromUTF8 (u8"Доля эха в выходном сигнале.");
            else
                description = juce::String::fromUTF8 (u8"Обратная связь задержки: больше значение — дольше звучат повторы.");
            selected = juce::String (getValue() * 100.0, 1) + " %";
        }
        setTooltip (description + juce::String::fromUTF8 (u8" Выбрано ") + selected + ".");
    }

    Signal2MAudioProcessorEditor& owner;
    const juce::String parameterId;
    const bool tempoCapable;
    const bool delayControl;
    bool tempoEnabled = false;
};

class Signal2MAudioProcessorEditor::HardwareButton final : public juce::Button
{
public:
    HardwareButton (Signal2MAudioProcessorEditor& editor, juce::RangedAudioParameter& parameterToUse,
                    juce::String name, bool isPushButton)
        : juce::Button (std::move (name)), parameter (parameterToUse), owner (editor), pushButton (isPushButton)
    {
        setClickingTogglesState (true);
        setWantsKeyboardFocus (true);
        setTitle (getName());
    }

    void paintButton (juce::Graphics& g, bool, bool down) override
    {
        const auto side = static_cast<float> (juce::jmin (getWidth(), getHeight()));
        const auto scale = side / 112.0f;
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::scale (scale).translated (
            (static_cast<float> (getWidth()) - side) * 0.5f,
            (static_cast<float> (getHeight()) - side) * 0.5f));
        if (pushButton)
            drawOscillation (g, { 56.0f, 56.0f }, 45.0f, getToggleState(), down, hasKeyboardFocus (true));
        else
            drawToggle (g, { 56.0f, 56.0f }, 31.0f, getToggleState() ? owner.toggleOnArtwork.get() : owner.toggleOffArtwork.get(), hasKeyboardFocus (true));
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            owner.showParameterMenu (parameter);
        else
            juce::Button::mouseDown (event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            juce::Button::mouseUp (event);
    }

    juce::RangedAudioParameter& parameter;

private:
    Signal2MAudioProcessorEditor& owner;
    const bool pushButton;
};

class Signal2MAudioProcessorEditor::ModeButton final : public juce::Button
{
public:
    ModeButton (Signal2MAudioProcessorEditor& editor, juce::RangedAudioParameter& parameterToUse,
                juce::String letterToUse, juce::String offDescription, juce::String onDescription)
        : juce::Button (parameterToUse.getName (64)), parameter (parameterToUse), owner (editor),
          letter (std::move (letterToUse)), offTooltip (std::move (offDescription)),
          onTooltip (std::move (onDescription))
    {
        setClickingTogglesState (true);
        setWantsKeyboardFocus (true);
        setTitle (getName());
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (offTooltip);
    }

    juce::String getTooltip() override
    {
        return getToggleState() ? onTooltip : offTooltip;
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        juce::Graphics::ScopedSaveState buttonState (g);
        g.addTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / 24.0f,
                                                     static_cast<float> (getHeight()) / 24.0f));
        const auto foreground = getToggleState() ? ink : ink.withAlpha (0.50f);
        g.setColour (ink.withAlpha (down ? 0.10f : highlighted ? 0.055f : 0.018f));
        g.fillRoundedRectangle (0.7f, 0.7f, 22.6f, 22.6f, 4.3f);
        g.setColour (foreground);
        g.drawRoundedRectangle (0.9f, 0.9f, 22.2f, 22.2f, 4.1f,
                                hasKeyboardFocus (true) ? 1.9f : 1.3f);
        g.setFont (panelFont (16.5f, true));
        g.drawText (letter, juce::Rectangle<float> (0.0f, 0.0f, 24.0f, 24.0f),
                    juce::Justification::centred, false);
    }

    void buttonStateChanged() override
    {
        setTooltip (getToggleState() ? onTooltip : offTooltip);
        // ButtonAttachment also calls this for automation. Read the actual button
        // states so manual toggles refresh even before the attachment writes APVTS.
        owner.refreshTempoPresentation();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            owner.showParameterMenu (parameter);
        else
            juce::Button::mouseDown (event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            juce::Button::mouseUp (event);
    }

    juce::RangedAudioParameter& parameter;

private:
    Signal2MAudioProcessorEditor& owner;
    const juce::String letter, offTooltip, onTooltip;
};

class Signal2MAudioProcessorEditor::LogoButton final : public juce::Button
{
public:
    explicit LogoButton (Signal2MAudioProcessorEditor& editor) : juce::Button ("LMZ"), owner (editor)
    {
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (juce::String::fromUTF8 (u8"Сведения о синтезаторах, поддержка проекта и проверка обновлений."));
    }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        juce::Graphics::ScopedSaveState logoState (g);
        g.addTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / 215.0f,
                                                     static_cast<float> (getHeight()) / 208.0f));
        // This fixed SVG area always reserves its footer, including when no badge
        // is present. The logo's placement never changes with the update result.
        drawLogo (g, owner.logoArtwork.get(), { 9.0f, 8.0f, 197.0f, 168.0f });
        if (updateAvailable)
        {
            g.setColour (juce::Colour (0xffd45b52));
            g.setFont (panelFont (17.5f, true));
            g.drawText (juce::String::fromUTF8 (u8"Доступна новая версия"),
                        juce::Rectangle<float> (0.0f, 179.0f, 215.0f, 22.0f),
                        juce::Justification::centred, false);
        }
        if (hasKeyboardFocus (true))
        {
            g.setColour (ink.withAlpha (0.60f));
            g.drawRoundedRectangle (juce::Rectangle<float> (3.0f, 3.0f, 209.0f, 202.0f), 8.0f, 1.1f);
        }
    }

    void setUpdateAvailable (bool available)
    {
        if (updateAvailable == available)
            return;
        updateAvailable = available;
        repaint(); // Only this existing logo/badge region becomes dirty.
    }

    void clicked() override
    {
        owner.showSupportOverlay();
    }

private:
    Signal2MAudioProcessorEditor& owner;
    bool updateAvailable = false;
};

class Signal2MAudioProcessorEditor::SupportOverlay final : public juce::Component
{
    class LinkRow final : public juce::Button
    {
    public:
        explicit LinkRow (signal2m::support::Link linkToUse)
            : juce::Button (juce::String::fromUTF8 (linkToUse.label))
        {
            setDestination (juce::String::fromUTF8 (linkToUse.url != nullptr ? linkToUse.url : ""));
        }

        bool hasDestination() const noexcept { return destinationAvailable; }

        bool setDestination (const juce::String& url)
        {
            const auto valid = signal2m::update_detail::isSafeHttpUrl (url.toStdString());
            const auto accepted = valid ? url : juce::String();
            const auto changed = destinationText != accepted || destinationAvailable != valid;
            if (changed)
            {
                ++destinationGeneration;
                destinationText = accepted;
                destination = juce::URL (accepted);
                destinationAvailable = valid;
            }
            setWantsKeyboardFocus (valid);
            setMouseCursor (valid ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            if (changed)
                repaint();
            return changed;
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto sx = static_cast<float> (getWidth()) / 536.0f;
            const auto sy = static_cast<float> (getHeight()) / 60.0f;
            juce::Graphics::ScopedSaveState rowState (g);
            g.addTransform (juce::AffineTransform::scale (sx, sy));
            const auto active = hasDestination();
            const auto background = active ? juce::Colour (0xff273b49) : juce::Colour (0xff292f2a);
            g.setColour ((highlighted || down) && active ? background.brighter (0.13f) : background);
            g.fillRoundedRectangle (0.0f, 0.0f, 536.0f, 60.0f, 10.0f);
            g.setColour (active ? juce::Colour (0xff729ab3).withAlpha (0.52f)
                                : juce::Colour (0xff778173).withAlpha (0.22f));
            g.drawRoundedRectangle (0.6f, 0.6f, 534.8f, 58.8f, 9.4f, hasKeyboardFocus (true) ? 1.8f : 0.9f);
            g.setColour (active ? juce::Colour (0xffe4eef3) : juce::Colour (0xff9fa89b));
            g.setFont (panelFont (20.0f));
            g.drawText (getName(), 22.0f, 0.0f, 481.0f, 60.0f, juce::Justification::centredLeft, false);
            if (active)
            {
                juce::Path arrow;
                arrow.startNewSubPath (503.0f, 36.0f);
                arrow.lineTo (514.0f, 25.0f);
                arrow.startNewSubPath (503.0f, 25.0f);
                arrow.lineTo (514.0f, 25.0f);
                arrow.lineTo (514.0f, 36.0f);
                g.setColour (juce::Colour (0xffb5d3e3));
                g.strokePath (arrow, juce::PathStrokeType (1.5f));
            }
        }

        void clicked() override
        {
            if (pointerArmed && pointerGeneration == destinationGeneration && isShowing() && hasDestination())
            {
                // Only this explicit user action opens the configured real URL.
                // The overlay remains open and this Button owns the mouse-up.
                pointerArmed = false;
                const auto target = destination;
                target.launchInDefaultBrowser();
                return;
            }
            // A row withdrawn while its mouse-down was held remains owned and
            // safely consumes the release; it must not launch or close anything.
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            pointerGeneration = destinationGeneration;
            pointerArmed = event.mods.isLeftButtonDown();
            juce::Button::mouseDown (event);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            const juce::Component::SafePointer<LinkRow> safeThis (this);
            juce::Button::mouseUp (event);
            if (safeThis != nullptr)
                safeThis->pointerArmed = false;
        }

        void triggerClick() override
        {
            if (! isEnabled() || ! isShowing() || ! hasDestination())
                return;
            // Return-key/accessibility activations use an immediate target
            // snapshot rather than JUCE's queued command, which could outlive it.
            const auto generation = destinationGeneration;
            const auto target = destination;
            const juce::Component::SafePointer<LinkRow> safeThis (this);
            if (safeThis != nullptr && safeThis->destinationGeneration == generation
                && safeThis->isShowing() && safeThis->hasDestination())
                target.launchInDefaultBrowser();
        }

    private:
        juce::URL destination;
        juce::String destinationText;
        bool destinationAvailable = false;
        std::uint64_t destinationGeneration = 0;
        std::uint64_t pointerGeneration = 0;
        bool pointerArmed = false;
    };

    class UpdateCheckButton final : public juce::Button
    {
    public:
        explicit UpdateCheckButton (SupportOverlay& overlay)
            : juce::Button (juce::String::fromUTF8 (u8"Проверить обновления")), owner (overlay)
        {
            setWantsKeyboardFocus (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void setChecking (bool shouldBeChecking)
        {
            if (checking == shouldBeChecking)
                return;
            checking = shouldBeChecking;
            setMouseCursor (checking ? juce::MouseCursor::NormalCursor
                                     : juce::MouseCursor::PointingHandCursor);
            repaint();
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            juce::Graphics::ScopedSaveState buttonState (g);
            g.addTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / 384.0f,
                                                         static_cast<float> (getHeight()) / 48.0f));
            g.setColour (checking ? juce::Colour (0xff353e32)
                                  : highlighted || down ? juce::Colour (0xff5b6657) : juce::Colour (0xff465140));
            g.fillRoundedRectangle (0.0f, 0.0f, 384.0f, 48.0f, 9.0f);
            g.setColour (ink.withAlpha (hasKeyboardFocus (true) ? 0.80f : 0.24f));
            g.drawRoundedRectangle (0.7f, 0.7f, 382.6f, 46.6f, 8.3f, 1.0f);
            g.setColour (ink.withAlpha (checking ? 0.50f : 1.0f));
            g.setFont (panelFont (20.0f));
            g.drawText (getName(), juce::Rectangle<float> (0.0f, 0.0f, 384.0f, 48.0f),
                        juce::Justification::centred, false);
        }

        void clicked() override
        {
            // Keep receiving mouse events while checking, but never start twice.
            if (! checking)
                owner.startUpdateCheck();
        }

    private:
        SupportOverlay& owner;
        bool checking = false;
    };

    class UpdatesLinkButton final : public juce::Button
    {
    public:
        UpdatesLinkButton() : juce::Button (juce::String::fromUTF8 (u8"Перейти к обновлениям"))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setVisible (false);
        }

        void setDestination (const juce::String& pageUrl)
        {
            const juce::URL candidate (pageUrl);
            const auto scheme = candidate.getScheme().toLowerCase();
            available = (scheme == "http" || scheme == "https") && candidate.isWellFormed()
                && candidate.getDomain().isNotEmpty();
            destination = available ? candidate : juce::URL();
            setWantsKeyboardFocus (available);
            setVisible (available);
        }

        bool hasDestination() const noexcept { return available; }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            juce::Graphics::ScopedSaveState buttonState (g);
            g.addTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / 384.0f,
                                                         static_cast<float> (getHeight()) / 44.0f));
            g.setColour (highlighted || down ? juce::Colour (0xff304655) : juce::Colour (0xff273b49));
            g.fillRoundedRectangle (0.0f, 0.0f, 384.0f, 44.0f, 9.0f);
            g.setColour (juce::Colour (0xff729ab3).withAlpha (hasKeyboardFocus (true) ? 0.90f : 0.52f));
            g.drawRoundedRectangle (0.6f, 0.6f, 382.8f, 42.8f, 8.4f, 1.0f);
            g.setColour (juce::Colour (0xffe4eef3));
            g.setFont (panelFont (20.0f));
            g.drawText (getName(), juce::Rectangle<float> (0.0f, 0.0f, 384.0f, 44.0f),
                        juce::Justification::centred, false);
        }

        void clicked() override
        {
            if (available)
                destination.launchInDefaultBrowser();
        }

    private:
        juce::URL destination;
        bool available = false;
    };

    class CloseButton final : public juce::Button
    {
    public:
        explicit CloseButton (Signal2MAudioProcessorEditor& editor)
            : juce::Button (juce::String::fromUTF8 (u8"Закрыть")), owner (editor)
        {
            setWantsKeyboardFocus (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            juce::Graphics::ScopedSaveState buttonState (g);
            g.addTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / 212.0f,
                                                         static_cast<float> (getHeight()) / 48.0f));
            g.setColour (highlighted || down ? juce::Colour (0xff5b6657) : juce::Colour (0xff465140));
            g.fillRoundedRectangle (0.0f, 0.0f, 212.0f, 48.0f, 9.0f);
            g.setColour (ink.withAlpha (hasKeyboardFocus (true) ? 0.80f : 0.24f));
            g.drawRoundedRectangle (0.7f, 0.7f, 210.6f, 46.6f, 8.3f, 1.0f);
            label (g, getName(), { 0.0f, 0.0f, 212.0f, 48.0f }, 21.0f);
        }

        void clicked() override { owner.hideSupportOverlay(); }

    private:
        Signal2MAudioProcessorEditor& owner;
    };

public:
    explicit SupportOverlay (Signal2MAudioProcessorEditor& editor)
        : owner (editor), closeButton (editor), updateButton (*this)
    {
        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);
        setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
        setAlwaysOnTop (true);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            rows[i] = std::make_unique<LinkRow> (signal2m::support::links[i]);
            addChildComponent (*rows[i]);
            rows[i]->setVisible (i == 0 && rows[i]->hasDestination());
        }
        addAndMakeVisible (closeButton);
        addAndMakeVisible (updateButton);
        addChildComponent (updatesLink);
        refreshUpdateStatus();
        setVisible (false);
    }

    void startUpdateCheck()
    {
        // Manual requests enter here. Automatic startup requests have a separate
        // once-per-processor guard in the processor-owned checker.
        auto& checker = owner.audioProcessor.getUpdateChecker();
        if (checker.result().state == signal2m::UpdateState::checking)
        {
            refreshUpdateStatus();
            return;
        }
        checker.checkForUpdates();
        refreshUpdateStatus();
    }

    void refreshUpdateStatus()
    {
        refreshUpdateStatus (owner.audioProcessor.getUpdateChecker().result());
    }

    void refreshUpdateStatus (const signal2m::UpdateResult& result)
    {
        const auto installed = owner.audioProcessor.getUpdateChecker().installedVersion();
        if (currentUpdateState == result.state && latestVersion == result.latestVersion
            && updatePageUrl == result.pageUrl && installedVersion == installed)
            return;

        currentUpdateState = result.state;
        latestVersion = result.latestVersion;
        updatePageUrl = result.pageUrl;
        installedVersion = installed;
        switch (currentUpdateState)
        {
            case signal2m::UpdateState::idle:
                updateStatusText = juce::String::fromUTF8 (u8"Проверка обновлений ещё не выполнялась");
                break;
            case signal2m::UpdateState::checking:
                updateStatusText = juce::String::fromUTF8 (u8"Проверяем…");
                break;
            case signal2m::UpdateState::upToDate:
                updateStatusText = juce::String::fromUTF8 (u8"Обновлений нет");
                break;
            case signal2m::UpdateState::available:
                updateStatusText = juce::String::fromUTF8 (u8"Доступна версия ") + latestVersion;
                break;
            case signal2m::UpdateState::failed:
                updateStatusText = juce::String::fromUTF8 (u8"Не удалось проверить обновления. Попробуйте позже");
                break;
        }
        const auto linkHadFocus = updatesLink.hasKeyboardFocus (true);
        // A new request, failure or non-available result removes the old URL.
        updatesLink.setDestination (currentUpdateState == signal2m::UpdateState::available
                                     ? updatePageUrl : juce::String());
        updateButton.setChecking (currentUpdateState == signal2m::UpdateState::checking);
        if (linkHadFocus && ! updatesLink.hasDestination() && isShowing())
            updateButton.grabKeyboardFocus();
        repaint();
    }

    void refreshDonationLinks()
    {
        // Reading this processor-owned cache never starts network work. Slots
        // remain independent, including an empty first slot and a valid second.
        const auto result = owner.audioProcessor.getDonationLinks().result();
        const auto nextUrls = result.state == signal2m::DonationState::ready
                                ? result.urls : std::array<juce::String, 2>();
        if (donationUrls == nextUrls)
            return;
        donationUrls = nextUrls;
        bool changed = false;
        bool focusedRowRemoved = false;
        for (size_t i = 0; i < donationUrls.size(); ++i)
        {
            auto& row = *rows[i + 1];
            const auto hadFocus = row.hasKeyboardFocus (true);
            const auto wasVisible = row.isVisible();
            changed = row.setDestination (donationUrls[i]) || changed;
            const auto visible = row.hasDestination();
            focusedRowRemoved = focusedRowRemoved || (hadFocus && ! visible);
            row.setVisible (visible);
            changed = changed || wasVisible != visible;
        }
        if (focusedRowRemoved && isShowing())
            updateButton.grabKeyboardFocus();
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.70f));
        juce::Graphics::ScopedSaveState cardState (g);
        g.addTransform (juce::AffineTransform::scale (owner.panelScale).translated (
            owner.panelOrigin.x + modalOffsetX * owner.panelScale, owner.panelOrigin.y));
        const juce::Rectangle<float> card (220.0f, 50.0f, 600.0f, 760.0f);
        for (int i = 3; i > 0; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.10f));
            g.fillRoundedRectangle (card.expanded (static_cast<float> (i) * 3.0f).translated (0.0f, 4.0f), 23.0f);
        }
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff30382e), card.getX(), card.getY(),
                                                juce::Colour (0xff1c231c), card.getRight(), card.getBottom(), false));
        g.fillRoundedRectangle (card, 20.0f);
        g.setColour (ink.withAlpha (0.22f));
        g.drawRoundedRectangle (card.reduced (0.6f), 19.4f, 1.2f);
        drawLogo (g, owner.logoArtwork.get(), { 423.0f, 72.0f, 194.0f, 106.0f });
        label (g, juce::String::fromUTF8 (u8"Приобретайте синтезаторы LMZ"),
               { 244.0f, 190.0f, 552.0f, 36.0f }, 24.0f, true);
        if (rows[1]->isVisible() || rows[2]->isVisible())
            label (g, juce::String::fromUTF8 (u8"Поддержать проект"),
                   { 252.0f, 312.0f, 536.0f, 34.0f }, 22.0f, true);
        // Graphics renders the returned version as plain text, never markup.
        g.setColour (ink);
        g.setFont (panelFont (20.0f));
        g.drawFittedText (updateStatusText, { 252, 568, 536, 54 }, juce::Justification::centred, 2, 1.0f);
        label (g, juce::String::fromUTF8 (u8"Используемая версия: ") + installedVersion,
               { 252.0f, 632.0f, 536.0f, 26.0f }, 18.5f);
    }

    void resized() override
    {
        const auto place = [this] (juce::Component& child, juce::Rectangle<float> area)
        {
            child.setBounds (juce::Rectangle<float> (owner.panelOrigin.x + (area.getX() + modalOffsetX) * owner.panelScale,
                                                     owner.panelOrigin.y + area.getY() * owner.panelScale,
                                                     area.getWidth() * owner.panelScale,
                                                     area.getHeight() * owner.panelScale).toNearestInt());
        };
        place (*rows[0], { 252.0f, 238.0f, 536.0f, 60.0f });
        place (*rows[1], { 252.0f, 355.0f, 536.0f, 60.0f });
        place (*rows[2], { 252.0f, 427.0f, 536.0f, 60.0f });
        place (updateButton, { 328.0f, 510.0f, 384.0f, 48.0f });
        place (updatesLink, { 328.0f, 674.0f, 384.0f, 44.0f });
        place (closeButton, { 414.0f, 742.0f, 212.0f, 48.0f });
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // Hide only after this overlay has received the original mouse-down/up.
        // The child stays alive, so the event cannot click a synthesizer control.
        owner.hideSupportOverlay();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            owner.hideSupportOverlay();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::tabKey)
        {
            // Keep Tab traversal inside the visible card, including Shift+Tab.
            std::array<juce::Component*, 6> targets {};
            int count = 0;
            for (const auto& row : rows)
                if (row->isVisible() && row->hasDestination())
                    targets[static_cast<size_t> (count++)] = row.get();
            targets[static_cast<size_t> (count++)] = &updateButton;
            if (updatesLink.hasDestination())
                targets[static_cast<size_t> (count++)] = &updatesLink;
            targets[static_cast<size_t> (count++)] = &closeButton;
            int current = -1;
            for (int i = 0; i < count; ++i)
                if (targets[static_cast<size_t> (i)]->hasKeyboardFocus (true))
                    current = i;
            const auto backwards = key.getModifiers().isShiftDown();
            const auto next = current < 0 ? (backwards ? count - 1 : 0)
                                         : (current + (backwards ? count - 1 : 1)) % count;
            targets[static_cast<size_t> (next)]->grabKeyboardFocus();
            return true;
        }
        return false;
    }

private:
    Signal2MAudioProcessorEditor& owner;
    std::array<std::unique_ptr<LinkRow>, 3> rows;
    CloseButton closeButton;
    UpdateCheckButton updateButton;
    UpdatesLinkButton updatesLink;
    signal2m::UpdateState currentUpdateState = signal2m::UpdateState::idle;
    juce::String latestVersion, updatePageUrl, installedVersion, updateStatusText;
    std::array<juce::String, 2> donationUrls;
};
Signal2MAudioProcessorEditor::Signal2MAudioProcessorEditor (Signal2MAudioProcessor& processor)
    : juce::AudioProcessorEditor (processor), audioProcessor (processor),
      panelLookAndFeel (std::make_unique<PanelLookAndFeel>())
{
    setOpaque (true);
    setLookAndFeel (panelLookAndFeel.get());
    toggleOnArtwork = juce::Drawable::createFromImageData (BinaryData::toggleon_svg,
                                                          BinaryData::toggleon_svgSize);
    toggleOffArtwork = juce::Drawable::createFromImageData (BinaryData::toggleoff_svg,
                                                           BinaryData::toggleoff_svgSize);
    logoArtwork = juce::Drawable::createFromImageData (BinaryData::lmzlogo_svg,
                                                      BinaryData::lmzlogo_svgSize);
    jassert (toggleOnArtwork != nullptr && toggleOffArtwork != nullptr && logoArtwork != nullptr);

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const auto& description = knobDescriptions[i];
        auto* parameter = audioProcessor.parameters.getParameter (description.id);
        jassert (parameter != nullptr);
        knobs[i] = std::make_unique<Knob> (*this, *parameter, description);
        addAndMakeVisible (*knobs[i]);
        sliderAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            audioProcessor.parameters, description.id, *knobs[i]);
    }

    const std::array<juce::String, 4> names {{
        "LFO rounded waveform", "LFO differentiated waveform", "Sync", "Oscillation"
    }};
    const std::array<juce::String, 4> descriptions {{
        juce::String::fromUTF8 (u8"Выбор округлённой или импульсной формы низкочастотного генератора."),
        juce::String::fromUTF8 (u8"Выбор прямой или дифференцированной модуляции. Действует при отключённой округлённой форме."),
        juce::String::fromUTF8 (u8"Синхронизация основного генератора от низкочастотного."),
        juce::String::fromUTF8 (u8"Включает и выключает незатухающие повторы в петле задержки.")
    }};
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        auto* parameter = audioProcessor.parameters.getParameter (buttonIds[i]);
        jassert (parameter != nullptr);
        buttons[i] = std::make_unique<HardwareButton> (*this, *parameter, names[i], i == 3);
        buttons[i]->setTooltip (descriptions[i]);
        addAndMakeVisible (*buttons[i]);
        buttonAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            audioProcessor.parameters, buttonIds[i], *buttons[i]);
    }

    const std::array<const char*, 3> letters {{ "T", "T", "P" }};
    const std::array<juce::String, 3> offDescriptions {{
        juce::String::fromUTF8 (u8"Привязка цикла к темпу проекта выключена. Ручка регулирует частоту в герцах."),
        juce::String::fromUTF8 (u8"Привязка задержки к темпу проекта выключена. Ручка регулирует время в миллисекундах."),
        juce::String::fromUTF8 (u8"Режим сохранения хвоста выключен. Гейт закрывает весь выход, новая нота сбрасывает задержку.")
    }};
    const std::array<juce::String, 3> onDescriptions {{
        juce::String::fromUTF8 (u8"Привязка цикла к темпу проекта включена. Ручка выбирает прямые и триольные длительности."),
        juce::String::fromUTF8 (u8"Привязка задержки к темпу проекта включена. Ручка выбирает прямые и триольные длительности."),
        juce::String::fromUTF8 (u8"Режим сохранения хвоста включён. Гейт стоит перед задержкой, эхо и незатухающие повторы продолжаются между нотами.")
    }};
    for (size_t i = 0; i < modeButtons.size(); ++i)
    {
        auto* parameter = audioProcessor.parameters.getParameter (modeIds[i]);
        jassert (parameter != nullptr);
        modeButtons[i] = std::make_unique<ModeButton> (*this, *parameter, letters[i],
                                                      offDescriptions[i], onDescriptions[i]);
        addAndMakeVisible (*modeButtons[i]);
        modeAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            audioProcessor.parameters, modeIds[i], *modeButtons[i]);
    }
    refreshTempoPresentation();

    logoButton = std::make_unique<LogoButton> (*this);
    addAndMakeVisible (*logoButton);
    supportOverlay = std::make_unique<SupportOverlay> (*this);
    addChildComponent (*supportOverlay);

    // Generate the hardware artwork once at twice the reference resolution.
    // Controls follow APVTS; LEDs are painted separately from the DSP snapshot.
    panelArtwork = juce::Image (juce::Image::RGB, panelWidth * 2, panelHeight * 2, true);
    {
        juce::Graphics g (panelArtwork);
        g.addTransform (juce::AffineTransform::scale (2.0f));
        g.fillAll (juce::Colour (0xff0d110d));
        {
            juce::Graphics::ScopedSaveState panelState (g);
            g.addTransform (juce::AffineTransform::translation (static_cast<float> (panelInset),
                                                              static_cast<float> (panelInset)));
            paintPanel (g, toggleOnArtwork.get());
        }
        paintWoodFrame (g);
    }
    tooltips = std::make_unique<juce::TooltipWindow> (this, 650);
    setResizable (true, false);
    setResizeLimits (874, 636, 1872, 1362);
    getConstrainer()->setFixedAspectRatio (static_cast<double> (panelWidth) / panelHeight);
    setSize (1123, 817);
    // The checker outlives the editor and suppresses subsequent startup requests
    // when this same processor's editor is recreated.
    (void) audioProcessor.getUpdateChecker().checkOnStartup();
    timerCallback();
    startTimerHz (30);
}

Signal2MAudioProcessorEditor::~Signal2MAudioProcessorEditor()
{
    stopTimer();
    // Attachments must be gone before the corresponding controls.
    for (auto& attachment : sliderAttachments)
        attachment.reset();
    for (auto& attachment : buttonAttachments)
        attachment.reset();
    for (auto& attachment : modeAttachments)
        attachment.reset();
    tooltips.reset();
    activeHostMenu.reset();
    setLookAndFeel (nullptr);
}

void Signal2MAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0e0b));
    g.drawImage (panelArtwork, juce::Rectangle<float> (canvasOrigin.x, canvasOrigin.y,
                  static_cast<float> (panelWidth) * panelScale, static_cast<float> (panelHeight) * panelScale));
    juce::Graphics::ScopedSaveState ledState (g);
    g.addTransform (juce::AffineTransform::scale (panelScale).translated (panelOrigin.x, panelOrigin.y));
    drawUpperIndicator (g, { ledColumn, topHardwareY }, static_cast<float> (upperLedLevel) / 255.0f);
    for (int i = 0; i < 10; ++i)
    {
        const auto brightness = visualState.active ? (i == runningLedIndex ? 1.0f : 0.045f) : 0.0f;
        drawIndicator (g, { ledColumn, 174.0f + static_cast<float> (i) * 65.5f }, brightness);
    }
}

void Signal2MAudioProcessorEditor::repaintPanelArea (juce::Rectangle<float> area)
{
    repaint (juce::Rectangle<float> (panelOrigin.x + area.getX() * panelScale,
                                     panelOrigin.y + area.getY() * panelScale,
                                     area.getWidth() * panelScale,
                                     area.getHeight() * panelScale).getSmallestIntegerContainer().expanded (2));
}

void Signal2MAudioProcessorEditor::timerCallback()
{
    const auto updateResult = audioProcessor.getUpdateChecker().result();
    logoButton->setUpdateAvailable (updateResult.state == signal2m::UpdateState::available);
    if (supportOverlay->isVisible())
    {
        supportOverlay->refreshUpdateStatus (updateResult);
        supportOverlay->refreshDonationLinks();
    }
    // Read only the audio processor's published state. The GUI never advances an LFO
    // or derives time from timer intervals, wall clocks, or parameter values.
    const auto state = audioProcessor.getVisualState();
    const LedSnapshot next {
        std::isfinite (state.lfoPhase) ? juce::jlimit (0.0f, 1.0f, state.lfoPhase) : 0.0f,
        std::isfinite (state.lfoValue) ? juce::jlimit (-1.0f, 1.0f, state.lfoValue) : 0.0f,
        state.active
    };
    const auto nextUpperLevel = next.active ? juce::roundToInt ((next.lfoValue + 1.0f) * 127.5f) : 0;
    auto nextRunningIndex = -1;
    if (next.active)
    {
        auto shiftedPhase = next.lfoPhase + 0.5f;
        shiftedPhase -= std::floor (shiftedPhase);
        const auto step = juce::jlimit (0, 9, static_cast<int> (shiftedPhase * 10.0f));
        nextRunningIndex = 9 - step; // LEDs are numbered top to bottom: phase travels upwards.
    }

    const auto previousUpperLevel = upperLedLevel;
    const auto previousRunningIndex = runningLedIndex;
    const auto activityChanged = visualState.active != next.active;
    visualState = next;
    upperLedLevel = nextUpperLevel;
    runningLedIndex = nextRunningIndex;

    if (upperLedLevel != previousUpperLevel)
        repaintPanelArea (disc ({ ledColumn, topHardwareY }, upperHaloRadius + 1.0f));

    const auto repaintIndicator = [this] (int index)
    {
        if (index >= 0)
            repaintPanelArea (disc ({ ledColumn, 174.0f + static_cast<float> (index) * 65.5f }, columnHaloRadius + 1.0f));
    };
    if (activityChanged)
    {
        for (int i = 0; i < 10; ++i)
            repaintIndicator (i);
    }
    else if (runningLedIndex != previousRunningIndex)
    {
        repaintIndicator (previousRunningIndex);
        repaintIndicator (runningLedIndex);
    }
}

void Signal2MAudioProcessorEditor::resized()
{
    panelScale = juce::jmin (static_cast<float> (getWidth()) / panelWidth,
                            static_cast<float> (getHeight()) / panelHeight);
    canvasOrigin = { (static_cast<float> (getWidth()) - panelWidth * panelScale) * 0.5f,
                     (static_cast<float> (getHeight()) - panelHeight * panelScale) * 0.5f };
    panelOrigin = canvasOrigin + juce::Point<float> (panelInset * panelScale, panelInset * panelScale);
    const auto place = [this] (juce::Component& component, float centreX, float centreY, float size)
    {
        component.setBounds (juce::Rectangle<float> (panelOrigin.x + (centreX - size * 0.5f) * panelScale,
                                                    panelOrigin.y + (centreY - size * 0.5f) * panelScale,
                                                    size * panelScale, size * panelScale).toNearestIntEdges());
    };
    for (size_t i = 0; i < knobs.size(); ++i)
        place (*knobs[i], knobDescriptions[i].x, knobDescriptions[i].y, 128.0f);
    place (*buttons[0], firstColumn, 725.0f, 112.0f);
    place (*buttons[1], secondColumn, 725.0f, 112.0f);
    place (*buttons[2], thirdColumn, 725.0f, 112.0f);
    place (*buttons[3], outputColumn, 520.0f, 112.0f);
    place (*modeButtons[0], thirdColumn, 331.0f, 24.0f);
    place (*modeButtons[1], secondColumn, 598.0f, 24.0f);
    place (*modeButtons[2], firstColumn, 598.0f, 24.0f);
    logoButton->setBounds (juce::Rectangle<float> (panelOrigin.x + rightAreaX * panelScale,
                                                  panelOrigin.y + 625.0f * panelScale,
                                                  215.0f * panelScale,
                                                  208.0f * panelScale).toNearestInt());
    supportOverlay->setBounds (getLocalBounds());
}

void Signal2MAudioProcessorEditor::showSupportOverlay()
{
    if (supportOverlay->isVisible())
        return;
    supportOverlay->setBounds (getLocalBounds());
    (void) audioProcessor.getDonationLinks().refresh();
    supportOverlay->refreshDonationLinks();
    supportOverlay->refreshUpdateStatus();
    supportOverlay->setVisible (true);
    supportOverlay->toFront (false);
    supportOverlay->grabKeyboardFocus();
}

void Signal2MAudioProcessorEditor::hideSupportOverlay()
{
    if (! supportOverlay->isVisible())
        return;
    supportOverlay->setVisible (false);
    logoButton->grabKeyboardFocus();
}

void Signal2MAudioProcessorEditor::refreshTempoPresentation()
{
    if (knobs[2] != nullptr)
        knobs[2]->setTempoSyncEnabled (modeButtons[0] != nullptr && modeButtons[0]->getToggleState());
    if (knobs[5] != nullptr)
        knobs[5]->setTempoSyncEnabled (modeButtons[1] != nullptr && modeButtons[1]->getToggleState());
}

int Signal2MAudioProcessorEditor::getControlParameterIndex (juce::Component& component)
{
    for (const auto& knob : knobs)
        if (knob.get() == &component || knob->isParentOf (&component))
            return knob->parameter.getParameterIndex();
    for (const auto& button : buttons)
        if (button.get() == &component || button->isParentOf (&component))
            return button->parameter.getParameterIndex();
    for (const auto& button : modeButtons)
        if (button.get() == &component || button->isParentOf (&component))
            return button->parameter.getParameterIndex();
    return -1;
}

void Signal2MAudioProcessorEditor::showParameterMenu (juce::RangedAudioParameter& parameter)
{
    if (auto* context = getHostContext())
    {
        // JUCE 7.0.12's non-deprecated spelling of getContextMenuForParameterIndex.
        activeHostMenu = context->getContextMenuForParameter (&parameter);
        if (activeHostMenu != nullptr)
        {
            activeHostMenu->showNativeMenu (getMouseXYRelative());
            return;
        }
    }

    juce::PopupMenu menu;
    menu.addSectionHeader (parameter.getName (64));
    menu.addItem (1, juce::String::fromUTF8 (u8"Сбросить значение"));
    const auto safeThis = juce::Component::SafePointer<Signal2MAudioProcessorEditor> (this);
    auto* parameterPointer = &parameter;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safeThis, parameterPointer] (int result)
    {
        if (safeThis != nullptr && result == 1)
        {
            parameterPointer->beginChangeGesture();
            parameterPointer->setValueNotifyingHost (parameterPointer->getDefaultValue());
            parameterPointer->endChangeGesture();
        }
    });
}

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <memory>

class Signal2MAudioProcessor;

class Signal2MAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit Signal2MAudioProcessorEditor (Signal2MAudioProcessor&);
    ~Signal2MAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    int getControlParameterIndex (juce::Component&) override;

private:
    class PanelLookAndFeel;
    class Knob;
    class HardwareButton;
    class ModeButton;
    class LogoButton;
    class SupportOverlay;

    void showParameterMenu (juce::RangedAudioParameter&);
    void showSupportOverlay();
    void hideSupportOverlay();
    void timerCallback() override;
    void repaintPanelArea (juce::Rectangle<float>);
    void refreshTempoPresentation();

    struct LedSnapshot
    {
        float lfoPhase = 0.0f;
        float lfoValue = 0.0f;
        bool active = false;
    } visualState;
    int upperLedLevel = 0;
    int runningLedIndex = -1;

    Signal2MAudioProcessor& audioProcessor;
    std::unique_ptr<PanelLookAndFeel> panelLookAndFeel;
    std::unique_ptr<juce::Drawable> toggleOnArtwork;
    std::unique_ptr<juce::Drawable> toggleOffArtwork;
    std::unique_ptr<juce::Drawable> logoArtwork;
    std::array<std::unique_ptr<Knob>, 7> knobs;
    std::array<std::unique_ptr<HardwareButton>, 4> buttons;
    std::array<std::unique_ptr<ModeButton>, 3> modeButtons;
    std::unique_ptr<LogoButton> logoButton;
    std::unique_ptr<SupportOverlay> supportOverlay;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 7> sliderAttachments;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 4> buttonAttachments;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 3> modeAttachments;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    std::unique_ptr<juce::HostProvidedContextMenu> activeHostMenu;
    juce::Image panelArtwork;
    float panelScale = 1.0f;
    juce::Point<float> canvasOrigin;
    juce::Point<float> panelOrigin;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Signal2MAudioProcessorEditor)
};


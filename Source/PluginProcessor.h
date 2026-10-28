#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "NoteVoice.h"
#include <memory>

namespace signal2m { class UpdateChecker; class DonationLinks; }

class Signal2MAudioProcessor final : public juce::AudioProcessor
{
public:
    Signal2MAudioProcessor();
    ~Signal2MAudioProcessor() override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "LMZ Signal 2M"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Read-only display telemetry. The audio thread owns all oscillator timing;
    // the editor only polls these atomics and never drives the sound engine.
    struct VisualState
    {
        float lfoPhase = 0.0f;
        float lfoValue = 0.0f;
        bool active = false;
    };
    VisualState getVisualState() const noexcept;

    // Editor startup and manual update checks only. Never used by audio processing.
    signal2m::UpdateChecker& getUpdateChecker() noexcept;
    // Loaded only when the support card is opened; never used by audio processing.
    signal2m::DonationLinks& getDonationLinks() noexcept;
    double getCurrentTempoBpm() const noexcept { return currentTempoBpm.load(std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    std::unique_ptr<signal2m::UpdateChecker> updateChecker;
    std::unique_ptr<signal2m::DonationLinks> donationLinks;
    signal2m::Parameters snapshot() const noexcept;
    void handleMidi(const juce::MidiMessageMetadata&) noexcept;
    void publishVisualState() noexcept;
    signal2m::NoteVoice voice;
    std::atomic<float> visualPhase { 0.0f };
    std::atomic<float> visualLfo { 0.0f };
    std::atomic<bool> visualActive { false };
    std::atomic<double> currentTempoBpm { 120.0 };
    bool transportWasPlaying = false;
    struct RawParameters
    {
        const std::atomic<float>* vco = nullptr;
        const std::atomic<float>* depth = nullptr;
        const std::atomic<float>* lfo = nullptr;
        const std::atomic<float>* master = nullptr;
        const std::atomic<float>* mix = nullptr;
        const std::atomic<float>* delay = nullptr;
        const std::atomic<float>* repeats = nullptr;
        const std::atomic<float>* rounded = nullptr;
        const std::atomic<float>* differentiated = nullptr;
        const std::atomic<float>* sync = nullptr;
        const std::atomic<float>* oscillation = nullptr;
        const std::atomic<float>* lfoTempoSync = nullptr;
        const std::atomic<float>* delayTempoSync = nullptr;
        const std::atomic<float>* fxPost = nullptr;
        const juce::RangedAudioParameter* lfoDefinition = nullptr;
        const juce::RangedAudioParameter* delayDefinition = nullptr;
    } raw;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Signal2MAudioProcessor)
};

#pragma once

#include <cstddef>
#include <vector>

namespace signal2m
{
struct Parameters
{
    float vcoHz = 180.0f;
    float lfoHz = 2.0f;
    float lfoDepth = 0.25f;
    float master = 0.35f;
    float mix = 0.25f;
    float delayMs = 280.0f;
    float repeats = 0.4f;
    bool lfoRounded = true;
    bool lfoDifferentiated = false;
    bool sync = false;
    bool oscillation = false;
    bool lfoTempoSync = false;
    bool delayTempoSync = false;
    bool fxPost = false;
    // Internal note offset, not a DAW parameter. Applied before LFO and limits.
    float noteSemitones = 0.0f;
};

// A mono, continuously running instrument. No dependency on JUCE or the editor.
// prepare is called with processing stopped. reset is O(1) and may be used for
// retriggers on the audio thread. All processing/state calls are sequential.
class SynthEngine
{
public:
    void prepare(double sampleRate, int maximumBlockSize);
    void reset() noexcept;
    // Retrigger only the source. Delay samples, wet filters and feedback survive.
    // Its previous excitation is retained for a short generator crossfade.
    void resetGenerator() noexcept;
    void setParameters(const Parameters&) noexcept;
    void process(float* left, float* right, int numSamples) noexcept;
    // Gate the AC-coupled generator before both dry output and delay input.
    // retriggerGain blends the frozen excitation from resetGenerator(); zero
    // excitation and retrigger gains keep processing only the existing FX tail.
    float processGatedSample(double excitationGain, double retriggerGain = 0.0) noexcept;
    float getLfoPhase() const noexcept { return static_cast<float>(lfoPhase_); }
    float getLfoValue() const noexcept { return static_cast<float>(lastLfoValue_); }

private:
    struct Smoother
    {
        double current = 0.0;
        double target = 0.0;

        double next(double coefficient) noexcept;
        void snap() noexcept { current = target; }
    };

    static Parameters sanitise(const Parameters&) noexcept;
    void updateTargets() noexcept;
    double oscillator(double increment, double syncFraction) noexcept;
    double readDelay(double delaySamples) const noexcept;

    Parameters parameters_;
    double sampleRate_ = 44100.0;
    double inverseSampleRate_ = 1.0 / 44100.0;
    double controlCoefficient_ = 0.0;
    double delayCoefficient_ = 0.0;
    double lfoRoundedCoefficient_ = 0.0;
    double lfoDifferentiatorCoefficient_ = 0.0;
    double dryDcCoefficient_ = 0.0;
    double feedbackDcCoefficient_ = 0.0;
    Smoother vcoLogHz_, lfoLogHz_, depth_, master_, mix_, delaySamples_;
    Smoother repeats_, rounded_, differentiated_, oscillation_, delayFilter_;

    double vcoPhase_ = 0.0;
    double vcoBlepCarry_ = 0.0;
    double lfoPhase_ = 0.0;
    double roundedLfo_ = 0.0;
    double differentiatorLowpass_ = 0.0;
    double lastLfoValue_ = 0.0;
    double dryDcInput_ = 0.0;
    double dryDcOutput_ = 0.0;
    double delayLowpass1_ = 0.0;
    double delayLowpass2_ = 0.0;
    double feedbackDcInput_ = 0.0;
    double feedbackDcOutput_ = 0.0;
    double lastExcitation_ = 0.0;
    double retriggerExcitation_ = 0.0;
    std::vector<float> delayBuffer_;
    std::size_t delayWritePosition_ = 0;
    std::size_t delayValidSamples_ = 0;
};
} // namespace signal2m

#include "SynthEngine.h"

#include <algorithm>
#include <cmath>

namespace signal2m
{
namespace
{
constexpr double pi = 3.1415926535897932384626433832795;
constexpr double voiceGain = 0.28;
constexpr double wetGain = 0.70;
constexpr double maximumOutput = 0.98;

double finiteClamp(float value, double low, double high, double fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(static_cast<double>(value), low, high)
                                : fallback;
}

double removeDenormal(double value) noexcept
{
    return std::abs(value) < 1.0e-24 ? 0.0 : value;
}
} // namespace

double SynthEngine::Smoother::next(double coefficient) noexcept
{
    current = target + coefficient * (current - target);
    if (std::abs(current - target) < 1.0e-12)
        current = target;
    return current;
}

Parameters SynthEngine::sanitise(const Parameters& input) noexcept
{
    Parameters result = input;
    const Parameters defaults;
    result.vcoHz = static_cast<float>(finiteClamp(input.vcoHz, 10.0, 8000.0, defaults.vcoHz));
    result.lfoHz = static_cast<float>(finiteClamp(input.lfoHz, 0.1, 250.0, defaults.lfoHz));
    result.lfoDepth = static_cast<float>(finiteClamp(input.lfoDepth, 0.0, 1.0, defaults.lfoDepth));
    result.master = static_cast<float>(finiteClamp(input.master, 0.0, 1.0, defaults.master));
    result.mix = static_cast<float>(finiteClamp(input.mix, 0.0, 1.0, defaults.mix));
    result.delayMs = static_cast<float>(finiteClamp(input.delayMs, 35.0, 650.0, defaults.delayMs));
    result.repeats = static_cast<float>(finiteClamp(input.repeats, 0.0, 1.0, defaults.repeats));
    result.noteSemitones = static_cast<float>(finiteClamp(input.noteSemitones, -128.0, 128.0, 0.0));
    return result;
}

void SynthEngine::prepare(double sampleRate, int maximumBlockSize)
{
    // A sample-based engine does not need a block-sized scratch buffer. Hosts may
    // subsequently submit blocks both smaller and larger than this hint.
    static_cast<void>(maximumBlockSize);
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0
                    ? std::clamp(sampleRate, 8000.0, 768000.0) : 44100.0;
    inverseSampleRate_ = 1.0 / sampleRate_;
    controlCoefficient_ = std::exp(-inverseSampleRate_ / 0.015);
    delayCoefficient_ = std::exp(-inverseSampleRate_ / 0.045);
    lfoRoundedCoefficient_ = std::exp(-inverseSampleRate_ / 0.075); // R12/C5.
    lfoDifferentiatorCoefficient_ = std::exp(-inverseSampleRate_ / 0.100); // C4/load approximation.
    dryDcCoefficient_ = std::exp(-2.0 * pi * 1.5 * inverseSampleRate_);
    feedbackDcCoefficient_ = std::exp(-2.0 * pi * 3.0 * inverseSampleRate_);

    // The only allocation in the engine. Two guard samples cover interpolation.
    delayBuffer_.assign(static_cast<std::size_t>(std::ceil(0.650 * sampleRate_)) + 4U, 0.0f);
    updateTargets();
    reset();
}

void SynthEngine::updateTargets() noexcept
{
    vcoLogHz_.target = std::log2(static_cast<double>(parameters_.vcoHz))
                     + static_cast<double>(parameters_.noteSemitones) / 12.0;
    lfoLogHz_.target = std::log2(parameters_.lfoHz);
    depth_.target = parameters_.lfoDepth;
    master_.target = parameters_.master;
    mix_.target = parameters_.mix;
    delaySamples_.target = parameters_.delayMs * 0.001 * sampleRate_;
    repeats_.target = parameters_.repeats;
    rounded_.target = parameters_.lfoRounded ? 1.0 : 0.0;
    differentiated_.target = parameters_.lfoDifferentiated ? 1.0 : 0.0;
    oscillation_.target = parameters_.oscillation ? 1.0 : 0.0;

    // A deliberately approximate PT2399/Deep Blue bandwidth: longer clock times
    // produce darker repeats. This is not an ADC, DAC or transistor simulation.
    const double position = (parameters_.delayMs - 35.0) / 615.0;
    const double cutoff = std::min(6500.0 - 4100.0 * position, sampleRate_ * 0.22);
    delayFilter_.target = std::exp(-2.0 * pi * cutoff * inverseSampleRate_);
}

void SynthEngine::setParameters(const Parameters& parameters) noexcept
{
    parameters_ = sanitise(parameters);
    updateTargets();
}

void SynthEngine::reset() noexcept
{
    // O(1) invalidation: a delay tap may only read samples written since reset.
    // Old bytes are overwritten naturally, with no buffer clearing on Note On.
    delayWritePosition_ = 0;
    delayValidSamples_ = 0;
    vcoPhase_ = vcoBlepCarry_ = lfoPhase_ = 0.0;
    roundedLfo_ = differentiatorLowpass_ = 0.0;
    dryDcInput_ = dryDcOutput_ = 0.0;
    delayLowpass1_ = delayLowpass2_ = 0.0;
    feedbackDcInput_ = feedbackDcOutput_ = 0.0;
    vcoLogHz_.snap();
    lfoLogHz_.snap();
    depth_.snap();
    master_.snap();
    mix_.snap();
    delaySamples_.snap();
    repeats_.snap();
    rounded_.snap();
    differentiated_.snap();
    oscillation_.snap();
    delayFilter_.snap();
    const double alternate = 1.0 - 0.5 * differentiated_.current;
    lastLfoValue_ = alternate * (1.0 - rounded_.current);
}

double SynthEngine::oscillator(double increment, double syncFraction) noexcept
{
    double result = 2.0 * vcoPhase_ - 1.0 + vcoBlepCarry_;
    vcoBlepCarry_ = 0.0;

    // Each discontinuity contributes the two halves of a polynomial BLEP.
    // Looking ahead by one phase increment also handles a fractional hard sync
    // reset, with its actual jump height rather than an assumed full saw wrap.
    const auto addStep = [&](double jump, double fraction)
    {
        const double remaining = 1.0 - fraction;
        result += 0.5 * jump * remaining * remaining;
        vcoBlepCarry_ -= 0.5 * jump * fraction * fraction;
    };
    const auto advance = [&](double from, double duration)
    {
        const double endPhase = vcoPhase_ + increment * duration;
        if (endPhase >= 1.0)
        {
            const double fraction = from + (1.0 - vcoPhase_) / increment;
            addStep(-2.0, std::clamp(fraction, 0.0, 1.0));
            vcoPhase_ = endPhase - 1.0;
        }
        else
        {
            vcoPhase_ = endPhase;
        }
    };

    if (syncFraction >= 0.0)
    {
        advance(0.0, syncFraction);
        addStep(-2.0 * vcoPhase_, syncFraction);
        vcoPhase_ = 0.0;
        advance(syncFraction, 1.0 - syncFraction);
    }
    else
    {
        advance(0.0, 1.0);
    }
    return result;
}

double SynthEngine::readDelay(double delaySamples) const noexcept
{
    // Calculate integer lags first: wrapping a near-zero negative floating point
    // position can otherwise round to exactly the buffer length.
    const auto newerLag = static_cast<std::size_t>(delaySamples);
    const auto olderLag = newerLag + 1U;
    const double fraction = delaySamples - static_cast<double>(newerLag);
    const auto newerIndex = delayWritePosition_ >= newerLag
                            ? delayWritePosition_ - newerLag
                            : delayBuffer_.size() + delayWritePosition_ - newerLag;
    const auto olderIndex = newerIndex == 0 ? delayBuffer_.size() - 1U : newerIndex - 1U;
    const double newer = newerLag <= delayValidSamples_ ? delayBuffer_[newerIndex] : 0.0;
    const double older = olderLag <= delayValidSamples_ ? delayBuffer_[olderIndex] : 0.0;
    return newer + fraction * (older - newer);
}

void SynthEngine::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (delayBuffer_.empty())
    {
        std::fill_n(left, numSamples, 0.0f);
        std::fill_n(right, numSamples, 0.0f);
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const double lfoHz = std::exp2(lfoLogHz_.next(controlCoefficient_));
        const double lfoIncrement = lfoHz * inverseSampleRate_;
        const double square = lfoPhase_ < 0.5 ? 1.0 : -1.0;
        roundedLfo_ = removeDenormal(square + lfoRoundedCoefficient_ * (roundedLfo_ - square));
        differentiatorLowpass_ = removeDenormal(square + lfoDifferentiatorCoefficient_
                                                          * (differentiatorLowpass_ - square));
        const double differentiated = 0.5 * (square - differentiatorLowpass_);
        const double differentiateMix = differentiated_.next(controlCoefficient_);
        const double roundedMix = rounded_.next(controlCoefficient_);
        const double alternate = square + differentiateMix * (differentiated - square);
        // Weighted mixing keeps S1 completely bypassed at roundedMix == 1,
        // without cancellation roundoff depending on the unused branch.
        const double modulation = (1.0 - roundedMix) * alternate + roundedMix * roundedLfo_;
        lastLfoValue_ = modulation;
        // MFOS control voltage has inverse polarity: a high LFO lowers pitch.
        const double pitch = vcoLogHz_.next(controlCoefficient_)
                             - 5.0 * depth_.next(controlCoefficient_) * modulation;
        const double frequency = std::clamp(std::exp2(pitch), 0.5,
                                            std::min(20000.0, sampleRate_ * 0.40));

        double syncFraction = -1.0;
        const double nextEdge = lfoPhase_ < 0.5 ? 0.5 : 1.0;
        if (parameters_.sync && lfoPhase_ + lfoIncrement >= nextEdge)
            syncFraction = std::clamp((nextEdge - lfoPhase_) / lfoIncrement, 0.0, 1.0);
        const double raw = voiceGain * oscillator(frequency * inverseSampleRate_, syncFraction);
        lfoPhase_ += lfoIncrement;
        if (lfoPhase_ >= 1.0)
            lfoPhase_ -= 1.0;

        // AC coupling at the generator output also removes the mean introduced
        // by hard sync. This is upstream of the parallel delay and Master.
        const double dry = removeDenormal(dryDcCoefficient_ * (dryDcOutput_ + raw - dryDcInput_));
        dryDcInput_ = raw;
        dryDcOutput_ = dry;

        const double delayed = readDelay(delaySamples_.next(delayCoefficient_));
        const double filterCoefficient = delayFilter_.next(delayCoefficient_);
        delayLowpass1_ = removeDenormal(delayed + filterCoefficient * (delayLowpass1_ - delayed));
        delayLowpass2_ = removeDenormal(delayLowpass1_ + filterCoefficient
                                                          * (delayLowpass2_ - delayLowpass1_));
        const double wet = delayLowpass2_;
        const double feedback = removeDenormal(feedbackDcCoefficient_
                                            * (feedbackDcOutput_ + wet - feedbackDcInput_));
        feedbackDcInput_ = wet;
        feedbackDcOutput_ = feedback;

        const double sustain = oscillation_.next(controlCoefficient_);
        const double normalFeedback = 0.96 * repeats_.next(controlCoefficient_);
        const double feedbackGain = normalFeedback + sustain * (1.08 - normalFeedback);
        // With Oscillation, small signals grow while tanh bounds every write.
        // Feedback is AC coupled to prevent a silent saturated DC equilibrium.
        delayBuffer_[delayWritePosition_] = static_cast<float>(std::tanh(dry + feedbackGain * feedback));
        if (++delayWritePosition_ == delayBuffer_.size())
            delayWritePosition_ = 0;
        if (delayValidSamples_ < delayBuffer_.size())
            ++delayValidSamples_;

        // Deep Blue keeps its dry path and adds the wet return in parallel.
        const double output = master_.next(controlCoefficient_)
                            * (dry + wetGain * mix_.next(controlCoefficient_) * wet);
        const float sample = static_cast<float>(std::clamp(output, -maximumOutput, maximumOutput));
        left[i] = sample;
        right[i] = sample;
    }
}
} // namespace signal2m

#include "NoteVoice.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace signal2m
{
bool NoteVoice::validNote(int channel, int note) noexcept
{
    return channel >= 1 && channel <= 16 && note >= 0 && note <= 127;
}

void NoteVoice::prepare(double sampleRate, int maximumBlockSize)
{
    prepared_ = false;
    const double rate = std::isfinite(sampleRate) && sampleRate > 0.0
                        ? std::clamp(sampleRate, 8000.0, 768000.0) : 44100.0;
    engine_.prepare(rate, maximumBlockSize);
    attackSamples_ = std::max(1, static_cast<int>(std::floor(rate * 0.003)));
    releaseSamples_ = std::max(1, static_cast<int>(std::floor(rate * tailSeconds)));
    prepared_ = true;
    reset();
}

void NoteVoice::applyParameters() noexcept
{
    auto parameters = parameters_;
    parameters.noteSemitones = selectedNote_ >= 0 ? static_cast<float>(selectedNote_ - 60) : 0.0f;
    engine_.setParameters(parameters);
}

void NoteVoice::setParameters(const Parameters& parameters) noexcept
{
    parameters_ = parameters;
    applyParameters();
}

void NoteVoice::stopSound() noexcept
{
    active_ = releasing_ = false;
    selectedChannel_ = 0;
    selectedNote_ = -1;
    attackRemaining_ = releaseRemaining_ = 0;
    transitionStart_ = lastOutput_ = 0.0;
    applyParameters();
    engine_.reset();
}

void NoteVoice::reset() noexcept
{
    heldCount_ = 0;
    stopSound();
}

void NoteVoice::trigger(int channel, int note, bool bridgePreviousSample) noexcept
{
    selectedChannel_ = channel;
    selectedNote_ = note;
    applyParameters();
    engine_.reset();
    transitionStart_ = bridgePreviousSample ? lastOutput_ : 0.0;
    lastOutput_ = transitionStart_;
    attackRemaining_ = attackSamples_;
    releaseRemaining_ = 0;
    releasing_ = false;
    active_ = true;
}

void NoteVoice::noteOn(int channel, int note) noexcept
{
    if (!prepared_ || !validNote(channel, note))
        return;

    HeldNote entry { channel, note, 1 };
    for (std::size_t i = 0; i < heldCount_; ++i)
    {
        if (heldNotes_[i].channel == channel && heldNotes_[i].note == note)
        {
            entry.presses = heldNotes_[i].presses;
            if (entry.presses < std::numeric_limits<std::uint32_t>::max())
                ++entry.presses;
            for (std::size_t j = i + 1U; j < heldCount_; ++j)
                heldNotes_[j - 1U] = heldNotes_[j];
            --heldCount_;
            break;
        }
    }
    // There are only 16*128 possible valid pairs, so a new unique entry fits.
    heldNotes_[heldCount_++] = entry;
    trigger(channel, note, true);
}

void NoteVoice::beginRelease() noexcept
{
    if (active_ && !releasing_)
    {
        releaseRemaining_ = releaseSamples_;
        releasing_ = true;
    }
}

void NoteVoice::noteOff(int channel, int note) noexcept
{
    if (!validNote(channel, note))
        return;
    for (std::size_t i = 0; i < heldCount_; ++i)
    {
        if (heldNotes_[i].channel != channel || heldNotes_[i].note != note)
            continue;
        if (heldNotes_[i].presses > 1U)
        {
            --heldNotes_[i].presses;
            return;
        }
        for (std::size_t j = i + 1U; j < heldCount_; ++j)
            heldNotes_[j - 1U] = heldNotes_[j];
        --heldCount_;
        if (selectedChannel_ == channel && selectedNote_ == note)
        {
            if (heldCount_ > 0)
            {
                const auto& previous = heldNotes_[heldCount_ - 1U];
                trigger(previous.channel, previous.note, true);
            }
            else
            {
                beginRelease();
            }
        }
        return;
    }
}

void NoteVoice::removeHeldChannel(int channel) noexcept
{
    if (channel == 0)
    {
        heldCount_ = 0;
        return;
    }
    std::size_t remaining = 0;
    for (std::size_t i = 0; i < heldCount_; ++i)
        if (heldNotes_[i].channel != channel)
            heldNotes_[remaining++] = heldNotes_[i];
    heldCount_ = remaining;
}

void NoteVoice::allNotesOff(int channel) noexcept
{
    if (channel < 0 || channel > 16)
        return;
    removeHeldChannel(channel);
    if (active_ && (channel == 0 || selectedChannel_ == channel))
    {
        if (heldCount_ > 0)
        {
            const auto& previous = heldNotes_[heldCount_ - 1U];
            trigger(previous.channel, previous.note, true);
        }
        else
        {
            beginRelease();
        }
    }
}

void NoteVoice::allSoundOff(int channel) noexcept
{
    if (channel < 0 || channel > 16)
        return;
    removeHeldChannel(channel);
    if (active_ && (channel == 0 || selectedChannel_ == channel))
    {
        stopSound();
        if (heldCount_ > 0)
        {
            const auto& previous = heldNotes_[heldCount_ - 1U];
            trigger(previous.channel, previous.note, false);
        }
    }
}

void NoteVoice::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!active_)
    {
        std::fill_n(left, numSamples, 0.0f);
        std::fill_n(right, numSamples, 0.0f);
        return;
    }

    const int count = releasing_ ? std::min(numSamples, releaseRemaining_) : numSamples;
    engine_.process(left, right, count);
    for (int i = 0; i < count; ++i)
    {
        double sample = left[i];
        if (attackRemaining_ > 0)
        {
            const double position = static_cast<double>(attackSamples_ - attackRemaining_)
                                    / static_cast<double>(attackSamples_);
            sample = transitionStart_ + position * (sample - transitionStart_);
            --attackRemaining_;
        }
        if (releasing_)
        {
            --releaseRemaining_;
            sample *= static_cast<double>(releaseRemaining_) / static_cast<double>(releaseSamples_);
        }
        lastOutput_ = sample;
        left[i] = right[i] = static_cast<float>(sample);
    }
    if (releasing_ && releaseRemaining_ == 0)
        stopSound();
    if (count < numSamples)
    {
        std::fill_n(left + count, numSamples - count, 0.0f);
        std::fill_n(right + count, numSamples - count, 0.0f);
    }
}

float NoteVoice::getLfoPhase() const noexcept
{
    return active_ ? engine_.getLfoPhase() : 0.0f;
}

float NoteVoice::getLfoValue() const noexcept
{
    return active_ ? engine_.getLfoValue() : 0.0f;
}
} // namespace signal2m

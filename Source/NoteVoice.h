#pragma once

#include "SynthEngine.h"

#include <array>
#include <cstdint>

namespace signal2m
{
// MIDI-gated monophonic wrapper. MIDI channels are 1..16; channel 0 means
// all channels only for the two all-notes/sound methods. MIDI note 60 is neutral.
// Every method except prepare is safe for sequential calls on the audio thread.
class NoteVoice
{
public:
    static constexpr double tailSeconds = 0.005;

    void prepare(double sampleRate, int maximumBlockSize);
    void reset() noexcept;
    void setParameters(const Parameters&) noexcept;
    void noteOn(int channel, int note) noexcept;
    void noteOff(int channel, int note) noexcept;
    void allNotesOff(int channel = 0) noexcept;
    void allSoundOff(int channel = 0) noexcept;
    void process(float* left, float* right, int numSamples) noexcept;
    float getLfoPhase() const noexcept;
    float getLfoValue() const noexcept;
    // Note/short release activity. In Post, an FX tail can outlive this flag.
    bool isActive() const noexcept { return active_; }

private:
    struct HeldNote
    {
        int channel = 0;
        int note = 0;
        std::uint32_t presses = 0;
    };

    static bool validNote(int channel, int note) noexcept;
    void applyParameters() noexcept;
    void trigger(int channel, int note, bool bridgePreviousSample) noexcept;
    void beginRelease() noexcept;
    void finishPostRelease() noexcept;
    void stopSound() noexcept;
    void removeHeldChannel(int channel) noexcept;

    SynthEngine engine_;
    Parameters parameters_;
    std::array<HeldNote, 16 * 128> heldNotes_ {};
    std::size_t heldCount_ = 0;
    int selectedChannel_ = 0;
    int selectedNote_ = -1;
    int attackSamples_ = 1;
    int releaseSamples_ = 1;
    int attackRemaining_ = 0;
    int releaseRemaining_ = 0;
    int modeTailFadeRemaining_ = 0;
    int fxChannel_ = 0;
    double transitionStart_ = 0.0;
    double lastOutput_ = 0.0;
    bool prepared_ = false;
    bool active_ = false;
    bool releasing_ = false;
    bool fxRunning_ = false;
};
} // namespace signal2m

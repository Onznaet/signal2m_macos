#include "PluginProcessor.h"
#include "UpdateChecker.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void value(Signal2MAudioProcessor& processor, const char* id, float normalised)
{
    auto* parameter = processor.parameters.getParameter(id);
    require(parameter != nullptr, "Missing parameter");
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(normalised);
    parameter->endChangeGesture();
}
void physical(Signal2MAudioProcessor& processor, const char* id, float plain)
{
    auto* parameter = processor.parameters.getParameter(id);
    require(parameter != nullptr, "Missing physical parameter");
    value(processor, id, parameter->convertTo0to1(plain));
}
void process(Signal2MAudioProcessor& processor, juce::AudioBuffer<float>& buffer,
             juce::MidiBuffer& events)
{
    processor.processBlock(buffer, events);
    require(events.isEmpty(), "MIDI input must be consumed");
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            require(std::isfinite(buffer.getSample(channel, sample)), "Non-finite audio");
}
void render(Signal2MAudioProcessor& processor, juce::AudioBuffer<float>& buffer, int blocks)
{
    juce::MidiBuffer events;
    for (int block = 0; block < blocks; ++block)
        process(processor, buffer, events);
}
void note(Signal2MAudioProcessor& processor, juce::AudioBuffer<float>& buffer, int key, int channel = 1)
{
    juce::MidiBuffer events;
    events.addEvent(juce::MidiMessage::noteOn(channel, key, 1.0f), 0);
    process(processor, buffer, events);
}
void silent(const juce::AudioBuffer<float>& buffer, int from, int count)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int i = from; i < from + count; ++i)
            require(buffer.getSample(channel, i) == 0.0f, "Expected exact digital silence");
}
void same(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from = 0)
{
    require(a.getNumSamples() == b.getNumSamples(), "Different render lengths");
    for (int sample = from; sample < a.getNumSamples(); ++sample)
        require(std::abs(a.getSample(0, sample) - b.getSample(0, sample)) < 1.0e-6f,
                "Equivalent MIDI history/state did not reproduce audio");
}
double frequency(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    double first = -1.0, last = 0.0;
    int crossings = 0;
    for (int i = static_cast<int>(0.02 * sampleRate); i < buffer.getNumSamples(); ++i)
    {
        const double a = buffer.getSample(0, i - 1), b = buffer.getSample(0, i);
        if (a < 0.0 && b >= 0.0)
        {
            const double position = i - 1.0 + (-a) / (b - a);
            if (first < 0.0) first = position;
            last = position;
            ++crossings;
        }
    }
    require(crossings > 5, "Too few pitch crossings");
    return (crossings - 1) * sampleRate / (last - first);
}
struct PlayHead final : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        return info;
    }
};
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI initialise;
        Signal2MAudioProcessor first;
        require(first.getName() == "LMZ Signal 2M", "Expected branded host display name");
        const std::array<const char*, 11> ids = {"vco_freq", "lfo_mod", "lfo_freq", "master", "mix", "delay", "repeats", "lfo_rounded", "lfo_differentiated", "sync", "oscillation"};
        require(first.getParameters().size() == 11, "Expected exactly eleven active parameters");
        require(first.acceptsMidi() && !first.producesMidi(), "Expected MIDI input without MIDI output");
        require(first.getTailLengthSeconds() > 0.0 && first.getTailLengthSeconds() <= 0.005,
                "A note release should have only a finite short tail");
        require(first.getTotalNumInputChannels() == 0, "Instrument should have no audio input bus");
        require(first.getTotalNumOutputChannels() == 2, "Default output should be stereo");
        require(first.hasEditor(), "Panel editor missing");
        require(first.getUpdateChecker().installedVersion() == SIGNAL2M_EXPECTED_VERSION,
                "Update checker version must come from the build");
        require(first.getUpdateChecker().result().state == signal2m::UpdateState::idle,
                "Constructing a processor must not check for updates");
        for (std::size_t index = 0; index < ids.size(); ++index)
        {
            auto* parameter = first.parameters.getParameter(ids[index]);
            require(parameter != nullptr && parameter->isAutomatable(), "Active parameter not automatable");
            require(parameter->getName(128).isNotEmpty(), "Parameter name missing");
            require(index < 7 ? !parameter->isDiscrete() : parameter->isDiscrete() && parameter->getNumSteps() == 2,
                    "Wrong parameter type");
        }
        require(first.parameters.getParameter("power") == nullptr, "Power must be artwork");
        std::cout << "PASS stable eleven parameters, MIDI input, short tail and static power\n";

        juce::AudioBuffer<float> buffer(2, 512);
        first.prepareToPlay(44100.0, 128);
        render(first, buffer, 100);
        silent(buffer, 0, buffer.getNumSamples());
        require(!first.getVisualState().active, "Idle indicator telemetry must be inactive");
        value(first, "oscillation", 1.0f);
        render(first, buffer, 100);
        silent(buffer, 0, buffer.getNumSamples());
        std::cout << "PASS no-note idle is silent even with Oscillation\n";

        for (double rate : {44100.0, 48000.0})
        {
            first.prepareToPlay(rate, 64);
            juce::MidiBuffer events;
            events.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 17);
            events.addEvent(juce::MidiMessage::noteOff(1, 60), 200);
            process(first, buffer, events);
            silent(buffer, 0, 17);
            require(buffer.getRMSLevel(0, 18, 180) > 0.0001f, "MIDI note did not sound at its sample offset");
            const int silenceStart = 200 + static_cast<int>(std::ceil(rate * 0.005)) + 1;
            silent(buffer, silenceStart, buffer.getNumSamples() - silenceStart);
            require(!first.getVisualState().active, "Released voice kept LED active");
            render(first, buffer, 3);
            silent(buffer, 0, buffer.getNumSamples());
        }
        std::cout << "PASS sample-offset gate and full Oscillation cut after note-off at both rates\n";

        physical(first, "vco_freq", 220.0f);
        value(first, "lfo_mod", 0.0f);
        value(first, "mix", 0.0f);
        value(first, "oscillation", 0.0f);
        first.prepareToPlay(48000.0, 128);
        const float knobBeforeMidi = first.parameters.getRawParameterValue("vco_freq")->load();
        juce::AudioBuffer<float> pitched(2, 12000);
        note(first, pitched, 60);
        const double low = frequency(pitched, 48000.0);
        note(first, pitched, 72);
        const double high = frequency(pitched, 48000.0);
        require(std::abs(low - 220.0) < 1.0 && std::abs(high - 440.0) < 1.0,
                "MIDI octave did not transpose the carrier");
        require(std::abs(high / low - 2.0) < 0.01, "Octave ratio is not two");
        require(first.parameters.getRawParameterValue("vco_freq")->load() == knobBeforeMidi,
                "MIDI transposition must not overwrite the VCO knob");
        require(first.getVisualState().active, "Held note must publish active display state");
        require(first.getVisualState().lfoPhase >= 0.0f && first.getVisualState().lfoPhase < 1.0f,
                "Published LFO phase is outside its cycle");
        std::cout << "PASS MIDI60 neutral, octave transposition and DSP display telemetry\n";

        // A zero-sample host callback can carry release/panic events.
        juce::AudioBuffer<float> zero(2, 0);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allSoundOff(1), 0);
        process(first, zero, off);
        render(first, buffer, 1);
        silent(buffer, 0, buffer.getNumSamples());
        std::cout << "PASS panic event in zero-sample host callback\n";

        for (std::size_t index = 0; index < ids.size(); ++index)
            value(first, ids[index], index < 7 ? static_cast<float>(index + 1) / 9.0f : 1.0f);
        juce::MemoryBlock saved;
        first.getStateInformation(saved);
        require(saved.getSize() > 32, "Empty saved state");
        Signal2MAudioProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        for (const auto* id : ids)
            require(std::abs(first.parameters.getParameter(id)->getValue() - restored.parameters.getParameter(id)->getValue()) < 0.00001f,
                    "State roundtrip changed a parameter");
        first.prepareToPlay(48000.0, 256);
        restored.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> other(2, 512);
        render(first, buffer, 1);
        silent(buffer, 0, buffer.getNumSamples());
        note(first, buffer, 65);
        note(restored, other, 65);
        same(buffer, other);
        const char malformed[] = "invalid state";
        const auto previous = restored.parameters.getParameter("vco_freq")->getValue();
        restored.setStateInformation(malformed, sizeof(malformed));
        restored.setStateInformation(nullptr, 0);
        require(restored.parameters.getParameter("vco_freq")->getValue() == previous, "Malformed state changed parameters");
        std::cout << "PASS all eleven settings roundtrip, fresh-instance note and malformed-state handling\n";

        // Old delay feedback must not bleed into the next new note.
        physical(first, "delay", 35.0f);
        physical(first, "vco_freq", 220.0f);
        value(first, "lfo_mod", 0.0f);
        value(first, "mix", 1.0f);
        value(first, "repeats", 1.0f);
        value(first, "sync", 0.0f);
        first.getStateInformation(saved);
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        first.prepareToPlay(48000.0, 64);
        restored.prepareToPlay(48000.0, 64);
        note(first, buffer, 48);
        render(first, buffer, 150);
        juce::AudioBuffer<float> retriggered(2, 12000), clean(2, 12000);
        note(first, retriggered, 60);
        note(restored, clean, 60);
        same(retriggered, clean, 241); // Ignore the <=5ms de-click bridge.
        std::cout << "PASS new-note retrigger clears old delay history\n";

        for (double rate : {44100.0, 48000.0})
        {
            restored.prepareToPlay(rate, 64);
            note(restored, buffer, 60);
            for (const auto* id : ids)
                for (float position : {0.0f, 0.37f, 1.0f})
                {
                    value(restored, id, position);
                    render(restored, buffer, 8);
                }
            value(restored, "master", 0.0f);
            render(restored, buffer, 256);
            require(buffer.getRMSLevel(0, 0, 512) < 0.00001f, "Master zero should mute held output");
            restored.reset();
            render(restored, buffer, 1);
            silent(buffer, 0, buffer.getNumSamples());
        }
        std::cout << "PASS automation at two rates, Master mute and reset\n";

        value(first, "master", 0.5f);
        PlayHead playHead;
        first.setPlayHead(&playHead);
        first.prepareToPlay(48000.0, 128);
        note(first, buffer, 60);
        playHead.playing = false;
        render(first, buffer, 1);
        silent(buffer, 0, buffer.getNumSamples());
        note(first, buffer, 60);
        require(buffer.getRMSLevel(0, 0, 512) > 0.001f,
                "Keyboard preview while transport is stopped must work");
        first.setPlayHead(nullptr);
        std::cout << "PASS transport-stop panic and stopped-transport keyboard preview\n";
        require(first.getUpdateChecker().result().state == signal2m::UpdateState::idle,
                "Audio, prepare/reset and state restoration must not trigger update checks");
        std::cout << "PASS build version binding and no automatic checks during processor lifecycle\n";
        std::cout << "ALL PROCESSOR CHECKS PASSED\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}

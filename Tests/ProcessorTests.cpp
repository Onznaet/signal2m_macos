#include "PluginProcessor.h"
#include "UpdateChecker.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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
    bool available = true;
    juce::Optional<double> bpm;
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (!available) return {};
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setBpm(bpm);
        return info;
    }
};

void tempoAndPostChecks()
{
    const std::array<const char*, 3> modes { "lfo_tempo_sync", "delay_tempo_sync", "fx_post" };
    Signal2MAudioProcessor state;
    for (std::size_t index = 0; index < modes.size(); ++index)
    {
        const auto* id = modes[index];
        auto* parameter = state.parameters.getParameter(id);
        require(parameter && parameter->isAutomatable() && parameter->getNumSteps() == 2,
                "New mode must be an automatable boolean");
        require(parameter->getValue() == (index == 0 ? 0.0f : 1.0f),
                "Fresh defaults must be free LFO, synced Delay and Post FX");
        value(state, id, 1.0f);
    }
    juce::MemoryBlock allModes;
    state.getStateInformation(allModes);
    Signal2MAudioProcessor copy;
    copy.setStateInformation(allModes.getData(), static_cast<int>(allModes.getSize()));
    for (const auto* id : modes)
        require(copy.parameters.getParameter(id)->getValue() == 1.0f, "New mode did not roundtrip");
    auto legacy = state.parameters.copyState();
    for (const auto* id : modes)
        legacy.removeChild(legacy.getChildWithProperty("id", id), nullptr);
    juce::MemoryBlock oldState;
    if (auto xml = legacy.createXml()) juce::AudioProcessor::copyXmlToBinary(*xml, oldState);
    copy.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
    for (const auto* id : modes)
        require(copy.parameters.getParameter(id)->getValue() == 0.0f,
                "Loading old state retained a new mode from the existing instance");
    std::cout << "PASS new mode defaults/automation/state and legacy state into edited instance\n";

    for (const double rate : { 44100.0, 48000.0 })
    {
        Signal2MAudioProcessor timed;
        PlayHead clock;
        timed.setPlayHead(&clock);
        value(timed, "lfo_tempo_sync", 1.0f);
        value(timed, "lfo_freq", 10.0f / 25.0f); // quarter note
        value(timed, "lfo_mod", 0.0f);
        value(timed, "mix", 0.0f);
        timed.prepareToPlay(rate, 64);
        juce::AudioBuffer<float> tenth(2, static_cast<int>(rate / 10.0));
        const auto phase = [&](juce::Optional<double> bpm, double expected)
        {
            clock.bpm = bpm;
            note(timed, tenth, 60);
            require(std::abs(timed.getVisualState().lfoPhase - expected) < 0.0001,
                    "LFO cycle duration is not the selected musical duration at host BPM");
        };
        phase({}, 0.2); // initial missing tempo uses 120 BPM
        phase(60.0, 0.1);
        phase(180.0, 0.3);
        phase({}, 0.3);
        phase(std::numeric_limits<double>::quiet_NaN(), 0.3);
        phase(0.0, 0.3);
        require(timed.getCurrentTempoBpm() == 180.0, "Invalid/missing tempo must retain last valid BPM");
        value(timed, "lfo_freq", 11.0f / 25.0f); // quarter triplet
        phase(120.0, 0.3);
        value(timed, "lfo_freq", 0.0f); // eight whole notes, 16 seconds at 120 BPM
        phase(120.0, 0.00625);
        value(timed, "lfo_freq", 1.0f); // 1/512 triplet, 384 Hz at 120 BPM
        phase(120.0, 0.4);
        value(timed, "lfo_tempo_sync", 0.0f);
        physical(timed, "lfo_freq", 2.0f);
        phase(60.0, 0.2);
        phase(180.0, 0.2);
        timed.setPlayHead(nullptr);

        // A 10 ms note is shorter than its echo. In Post the delayed excitation
        // must still emerge at the actual quarter/whole/triplet time.
        for (const auto spec : std::array<std::array<double, 3>, 3> {{{120.0, 4.0 / 19.0, 0.5},
                                                                    {240.0, 5.0 / 19.0, 1.0 / 6.0},
                                                                    {120.0, 0.0, 2.0}}})
        {
            Signal2MAudioProcessor wet, dry;
            PlayHead wetClock, dryClock;
            wetClock.bpm = dryClock.bpm = spec[0];
            wet.setPlayHead(&wetClock); dry.setPlayHead(&dryClock);
            for (auto* processor : { &wet, &dry })
            {
                value(*processor, "fx_post", 1.0f);
                value(*processor, "delay_tempo_sync", 1.0f);
                value(*processor, "delay", static_cast<float>(spec[1]));
                value(*processor, "repeats", 0.0f);
                value(*processor, "lfo_mod", 0.0f);
                physical(*processor, "vco_freq", 220.0f);
                processor->prepareToPlay(rate, 64);
            }
            value(wet, "mix", 1.0f); value(dry, "mix", 0.0f);
            const int count = static_cast<int>((spec[2] + 0.08) * rate);
            juce::AudioBuffer<float> wetAudio(2, count), dryAudio(2, count);
            const auto play = [&](Signal2MAudioProcessor& processor, juce::AudioBuffer<float>& audio)
            {
                juce::MidiBuffer notes;
                notes.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
                notes.addEvent(juce::MidiMessage::noteOff(1, 60), static_cast<int>(0.01 * rate));
                process(processor, audio, notes);
            };
            play(wet, wetAudio); play(dry, dryAudio);
            const int before = static_cast<int>((spec[2] - 0.002) * rate);
            for (int i = 0; i < before; ++i)
                require(std::abs(wetAudio.getSample(0, i) - dryAudio.getSample(0, i)) < 1.0e-6f,
                        "Synced echo arrived before its musical duration");
            double echoEnergy = 0.0;
            for (int i = static_cast<int>(spec[2] * rate) + 2; i < count; ++i)
                echoEnergy += std::abs(wetAudio.getSample(0, i) - dryAudio.getSample(0, i));
            require(echoEnergy > 0.01, "Post echo did not survive note-off at selected musical duration");
            require(!wet.getVisualState().active, "Post echo must not pretend a MIDI note is held");
            require(wet.getTailLengthSeconds() > spec[2], "Post tail not advertised to the host");
            wet.setPlayHead(nullptr); dry.setPlayHead(nullptr);
        }
        std::cout << "PASS host BPM/straight/triplet/extreme/free timing and actual Post echo arrival sr=" << rate << '\n';
    }

    Signal2MAudioProcessor post;
    PlayHead clock;
    post.setPlayHead(&clock);
    value(post, "fx_post", 1.0f);
    value(post, "delay_tempo_sync", 0.0f);
    value(post, "mix", 1.0f);
    value(post, "oscillation", 1.0f);
    physical(post, "delay", 35.0f);
    post.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 4096);
    juce::MidiBuffer shortNote;
    shortNote.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    shortNote.addEvent(juce::MidiMessage::noteOff(1, 60), 400);
    process(post, audio, shortNote);
    require(!post.getVisualState().active && audio.getRMSLevel(0, 2000, 2000) > 0.0001f,
            "Post Oscillation did not continue after the note ended");
    require(std::isinf(post.getTailLengthSeconds()), "Post Oscillation must advertise an infinite host tail");
    clock.playing = false;
    render(post, audio, 1);
    silent(audio, 0, audio.getNumSamples());
    note(post, audio, 60);
    require(audio.getRMSLevel(0, 0, audio.getNumSamples()) > 0.001f, "Stopped host preview broke in Post");
    juce::AudioBuffer<float> empty(2, 0);
    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    process(post, empty, panic);
    render(post, audio, 1);
    silent(audio, 0, audio.getNumSamples());
    post.setPlayHead(nullptr);
    std::cout << "PASS Post Oscillation infinite host tail, transport stop, preview and zero-block panic\n";
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI initialise;
        Signal2MAudioProcessor first;
        require(first.getName() == "LMZ Signal 2M", "Expected branded host display name");
        const std::array<const char*, 11> ids = {"vco_freq", "lfo_mod", "lfo_freq", "master", "mix", "delay", "repeats", "lfo_rounded", "lfo_differentiated", "sync", "oscillation"};
        require(first.getParameters().size() == 14, "Expected eleven original controls plus three new modes");
        require(first.parameters.getParameter("lfo_tempo_sync")->getValue() == 0.0f
                && first.parameters.getParameter("delay_tempo_sync")->getValue() == 1.0f
                && first.parameters.getParameter("fx_post")->getValue() == 1.0f,
                "Unexpected 0.4 factory modes");
        // Retain the original eleven-control regression in its original mode.
        value(first, "delay_tempo_sync", 0.0f);
        value(first, "fx_post", 0.0f);
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
        tempoAndPostChecks();
        std::cout << "ALL PROCESSOR CHECKS PASSED\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}

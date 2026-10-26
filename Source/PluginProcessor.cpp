#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "UpdateChecker.h"
#include <algorithm>

namespace
{
juce::NormalisableRange<float> frequencyRange(float low, float high, float centre)
{
    juce::NormalisableRange<float> range(low, high);
    range.setSkewForCentre(centre);
    return range;
}

juce::AudioParameterFloatAttributes unit(const juce::String& name, int decimals)
{
    return juce::AudioParameterFloatAttributes().withLabel(name)
        .withStringFromValueFunction([decimals](float value, int) { return juce::String(value, decimals); })
        .withValueFromStringFunction([](const juce::String& text) { return text.getFloatValue(); });
}

juce::AudioParameterFloatAttributes percent()
{
    return juce::AudioParameterFloatAttributes().withLabel("%")
        .withStringFromValueFunction([](float value, int) { return juce::String(value * 100.0f, 1); })
        .withValueFromStringFunction([](const juce::String& text) { return text.getFloatValue() * 0.01f; });
}
}

Signal2MAudioProcessor::Signal2MAudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Signal2MState", createParameterLayout()),
      updateChecker(std::make_unique<signal2m::UpdateChecker>(JucePlugin_VersionString))
{
    raw.vco = parameters.getRawParameterValue("vco_freq");
    raw.depth = parameters.getRawParameterValue("lfo_mod");
    raw.lfo = parameters.getRawParameterValue("lfo_freq");
    raw.master = parameters.getRawParameterValue("master");
    raw.mix = parameters.getRawParameterValue("mix");
    raw.delay = parameters.getRawParameterValue("delay");
    raw.repeats = parameters.getRawParameterValue("repeats");
    raw.rounded = parameters.getRawParameterValue("lfo_rounded");
    raw.differentiated = parameters.getRawParameterValue("lfo_differentiated");
    raw.sync = parameters.getRawParameterValue("sync");
    raw.oscillation = parameters.getRawParameterValue("oscillation");
}

Signal2MAudioProcessor::~Signal2MAudioProcessor() = default;

signal2m::UpdateChecker& Signal2MAudioProcessor::getUpdateChecker() noexcept
{
    return *updateChecker;
}

juce::AudioProcessorValueTreeState::ParameterLayout Signal2MAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using ID = juce::ParameterID;
    layout.add(std::make_unique<Float>(ID{"vco_freq", 1}, "VCO Freq", frequencyRange(10.0f, 8000.0f, 200.0f), 180.0f, unit("Hz", 1)));
    layout.add(std::make_unique<Float>(ID{"lfo_mod", 1}, "LFO mod", juce::NormalisableRange<float>(0.0f, 1.0f), 0.25f, percent()));
    layout.add(std::make_unique<Float>(ID{"lfo_freq", 1}, "LFO freq", frequencyRange(0.1f, 250.0f, 4.0f), 2.0f, unit("Hz", 2)));
    layout.add(std::make_unique<Float>(ID{"master", 1}, "Master", juce::NormalisableRange<float>(0.0f, 1.0f), 0.35f, percent()));
    layout.add(std::make_unique<Float>(ID{"mix", 1}, "Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 0.25f, percent()));
    layout.add(std::make_unique<Float>(ID{"delay", 1}, "Delay", juce::NormalisableRange<float>(35.0f, 650.0f), 280.0f, unit("ms", 1)));
    layout.add(std::make_unique<Float>(ID{"repeats", 1}, "Repeats", juce::NormalisableRange<float>(0.0f, 1.0f), 0.4f, percent()));
    layout.add(std::make_unique<Bool>(ID{"lfo_rounded", 1}, "LFO rounded", true));
    layout.add(std::make_unique<Bool>(ID{"lfo_differentiated", 1}, "LFO edge shape", false));
    layout.add(std::make_unique<Bool>(ID{"sync", 1}, "Sync", false));
    layout.add(std::make_unique<Bool>(ID{"oscillation", 1}, "Oscillation", false));
    return layout;
}

signal2m::Parameters Signal2MAudioProcessor::snapshot() const noexcept
{
    signal2m::Parameters p;
    const auto read = [](const std::atomic<float>* value) { return value->load(std::memory_order_relaxed); };
    p.vcoHz = read(raw.vco); p.lfoDepth = read(raw.depth); p.lfoHz = read(raw.lfo);
    p.master = read(raw.master); p.mix = read(raw.mix); p.delayMs = read(raw.delay); p.repeats = read(raw.repeats);
    p.lfoRounded = read(raw.rounded) >= 0.5f; p.lfoDifferentiated = read(raw.differentiated) >= 0.5f;
    p.sync = read(raw.sync) >= 0.5f; p.oscillation = read(raw.oscillation) >= 0.5f;
    return p;
}

void Signal2MAudioProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    voice.setParameters(snapshot());
    voice.prepare(sampleRate, maximumExpectedSamplesPerBlock);
    transportWasPlaying = false;
    publishVisualState();
}

void Signal2MAudioProcessor::releaseResources() { reset(); }
void Signal2MAudioProcessor::reset()
{
    voice.reset();
    transportWasPlaying = false;
    publishVisualState();
}

Signal2MAudioProcessor::VisualState Signal2MAudioProcessor::getVisualState() const noexcept
{
    return { visualPhase.load(std::memory_order_relaxed),
             visualLfo.load(std::memory_order_relaxed),
             visualActive.load(std::memory_order_relaxed) };
}

void Signal2MAudioProcessor::publishVisualState() noexcept
{
    visualPhase.store(voice.getLfoPhase(), std::memory_order_relaxed);
    visualLfo.store(voice.getLfoValue(), std::memory_order_relaxed);
    visualActive.store(voice.isActive(), std::memory_order_relaxed);
}

bool Signal2MAudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && (layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

void Signal2MAudioProcessor::handleMidi(const juce::MidiMessageMetadata& metadata) noexcept
{
    // All supported messages are short and remain in MidiMessage's inline
    // storage. Skip SysEx rather than allocate on the audio thread.
    if (metadata.numBytes != 3)
        return;
    const auto message = metadata.getMessage();
    if (message.isNoteOn())
        voice.noteOn(message.getChannel(), message.getNoteNumber());
    else if (message.isNoteOff()) // Includes note-on with velocity zero.
        voice.noteOff(message.getChannel(), message.getNoteNumber());
    else if (message.isAllSoundOff())
        voice.allSoundOff(message.getChannel());
    else if (message.isAllNotesOff())
        voice.allNotesOff(message.getChannel());
}

void Signal2MAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(noDenormals);
    voice.setParameters(snapshot());

    // A transport stop must not leave a held sequencer note running. Preview
    // notes received while the host is stopped still work: only the transition
    // from playing to stopped clears previous notes, before new events below.
    if (const auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            const bool playing = position->getIsPlaying();
            if (transportWasPlaying && !playing)
                voice.allSoundOff();
            transportWasPlaying = playing;
        }

    const int samples = buffer.getNumSamples();
    buffer.clear();
    const auto render = [&](int from, int count)
    {
        if (count <= 0 || buffer.getNumChannels() == 0)
            return;
        auto* left = buffer.getWritePointer(0, from);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1, from) : left;
        voice.process(left, right, count);
    };

    // MIDI offsets are sample offsets in the current audio block. Split at
    // every event, preserving same-offset ordering and zero-sample callbacks.
    int cursor = 0;
    for (const auto metadata : midi)
    {
        const int eventSample = std::clamp(metadata.samplePosition, cursor, samples);
        render(cursor, eventSample - cursor);
        handleMidi(metadata);
        cursor = eventSample;
    }
    render(cursor, samples - cursor);
    midi.clear(); // This instrument consumes MIDI; it does not produce it.
    publishVisualState();
}

juce::AudioProcessorEditor* Signal2MAudioProcessor::createEditor()
{
    return new Signal2MAudioProcessorEditor(*this);
}

void Signal2MAudioProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destination);
}

void Signal2MAudioProcessor::setStateInformation(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return;
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(parameters.state.getType()))
        {
            auto restored = juce::ValueTree::fromXml(*xml);
            if (restored.isValid())
                parameters.replaceState(restored);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Signal2MAudioProcessor();
}

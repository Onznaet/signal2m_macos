#include "NoteVoice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace allocationProbe
{
bool active = false;
std::size_t count = 0;
}

#if defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define SIGNAL2M_NOTE_TEST_ASAN 1
 #endif
#endif

#if !defined(SIGNAL2M_NOTE_TEST_ASAN)
void* operator new(std::size_t size)
{
    if (allocationProbe::active)
        ++allocationProbe::count;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

namespace
{
using signal2m::NoteVoice;
using signal2m::Parameters;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Parameters dryParameters()
{
    Parameters p;
    p.vcoHz = 180.0f;
    p.master = 1.0f;
    p.mix = 0.0f;
    p.lfoDepth = 0.0f;
    return p;
}

std::vector<float> renderOn(NoteVoice& voice, int samples,
                             const std::vector<int>& blocks = { 257 })
{
    std::vector<float> output(static_cast<std::size_t>(samples));
    std::vector<float> right(static_cast<std::size_t>(*std::max_element(blocks.begin(), blocks.end())));
    std::size_t block = 0;
    int offset = 0;
    while (offset < samples)
    {
        const int count = std::min(blocks[block++ % blocks.size()], samples - offset);
        voice.process(output.data() + offset, right.data(), count);
        for (int i = 0; i < count; ++i)
        {
            const float sample = output[static_cast<std::size_t>(offset + i)];
            require(std::isfinite(sample) && std::abs(sample) <= 0.980001f,
                    "NoteVoice output must remain finite and bounded");
            require(sample == right[static_cast<std::size_t>(i)], "Both mono output channels must be populated");
        }
        const float phase = voice.getLfoPhase();
        const float value = voice.getLfoValue();
        require(std::isfinite(phase) && phase >= 0.0f && phase <= 1.0f, "LFO phase must be normalized");
        require(std::isfinite(value) && value >= -1.0f && value <= 1.0f, "LFO telemetry must expose a bounded actual waveform");
        offset += count;
    }
    return output;
}

std::vector<float> renderNote(Parameters p, int note, double rate, int samples,
                               const std::vector<int>& blocks = { 257 })
{
    NoteVoice voice;
    voice.setParameters(p);
    voice.prepare(rate, 64);
    voice.noteOn(1, note);
    return renderOn(voice, samples, blocks);
}

bool silence(const std::vector<float>& audio, std::size_t begin = 0)
{
    return std::all_of(audio.begin() + static_cast<std::ptrdiff_t>(begin), audio.end(),
                       [](float sample) { return sample == 0.0f; });
}

double rms(const std::vector<float>& audio, std::size_t begin = 0)
{
    double energy = 0.0;
    for (std::size_t i = begin; i < audio.size(); ++i)
        energy += static_cast<double>(audio[i]) * audio[i];
    return std::sqrt(energy / static_cast<double>(audio.size() - begin));
}

double difference(const std::vector<float>& a, const std::vector<float>& b, std::size_t begin = 0)
{
    require(a.size() == b.size(), "Comparison sizes must match");
    double energy = 0.0;
    for (std::size_t i = begin; i < a.size(); ++i)
    {
        const double delta = static_cast<double>(a[i]) - b[i];
        energy += delta * delta;
    }
    return std::sqrt(energy / static_cast<double>(a.size() - begin));
}

double frequency(const std::vector<float>& audio, double sampleRate, std::size_t begin)
{
    int crossings = 0;
    for (std::size_t i = std::max<std::size_t>(1, begin); i < audio.size(); ++i)
        if (audio[i - 1] <= 0.0f && audio[i] > 0.0f)
            ++crossings;
    return crossings * sampleRate / static_cast<double>(audio.size() - begin);
}

void testIdleGateLongNoteAndRelease()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        NoteVoice voice;
        auto p = dryParameters();
        p.mix = p.repeats = 1.0f;
        p.delayMs = 35.0f;
        p.oscillation = true;
        voice.setParameters(p);
        voice.prepare(rate, 32);
        require(!voice.isActive(), "Prepared synth must wait for MIDI");
        require(silence(renderOn(voice, static_cast<int>(rate))), "Idle must be exact silence even with Oscillation enabled");
        voice.noteOn(1, 60);
        require(voice.isActive(), "Note On must activate the voice");
        const auto held = renderOn(voice, static_cast<int>(5.0 * rate), { 1, 127, 2048 });
        require(held.front() == 0.0f, "Initial attack must start from silence");
        require(rms(held, static_cast<std::size_t>(4.0 * rate)) > 0.05,
                "A held note must keep sounding without further Note On events");
        voice.noteOff(1, 60);
        require(voice.isActive(), "The voice remains active during its short release");
        const int release = static_cast<int>(std::floor(rate * NoteVoice::tailSeconds));
        const auto tail = renderOn(voice, release + 1000, { 1, 19, 512 });
        require(silence(tail, static_cast<std::size_t>(release - 1)),
                "Note Off must mute the entire delay/Oscillation path within 5 ms");
        require(!voice.isActive(), "Release completion must deactivate the voice");
        require(silence(renderOn(voice, static_cast<int>(rate))), "Oscillation must not reappear while idle");
        require(voice.getLfoPhase() == 0.0f && voice.getLfoValue() == 0.0f,
                "Idle LFO telemetry must return to rest");
    }
}

void testRetriggerHistoryAndContinuity()
{
    Parameters p;
    p.vcoHz = 317.0f;
    p.lfoHz = 7.25f;
    p.lfoDepth = 0.7f;
    p.master = p.mix = p.repeats = 1.0f;
    p.delayMs = 137.0f;
    p.oscillation = p.sync = p.lfoDifferentiated = true;
    p.lfoRounded = false;
    const auto clean = renderNote(p, 60, 48000.0, 48000);

    NoteVoice voice;
    voice.prepare(48000.0, 64);
    voice.setParameters(p);
    voice.noteOn(3, 83);
    const auto dirty = renderOn(voice, 109039);
    const float preceding = dirty.back();
    voice.noteOn(1, 60);
    require(voice.getLfoPhase() == 0.0f, "Every new note must restart the LFO immediately");
    const auto retriggered = renderOn(voice, 48000);
    require(retriggered.front() == preceding, "Retrigger must bridge the preceding sample without a forced jump");
    require(difference(clean, retriggered, 145) == 0.0,
            "After the short bridge, retrigger must match fresh VCO, filters and delay history exactly");

    voice.allSoundOff();
    require(!voice.isActive(), "All Sound Off must stop immediately");
    voice.noteOn(1, 60);
    require(difference(clean, renderOn(voice, 48000)) == 0.0,
            "Triggers from silence must be repeatable including their complete delay train");
}

void testMidiPitchAndModulationOrder()
{
    const auto p = dryParameters();
    const auto neutral = renderNote(p, 60, 48000.0, 96000);
    const auto upper = renderNote(p, 72, 48000.0, 96000);
    const auto lower = renderNote(p, 48, 48000.0, 96000);
    const double neutralHz = frequency(neutral, 48000.0, 48000);
    require(std::abs(neutralHz - 180.0) < 1.1, "MIDI 60 must leave the VCO knob neutral");
    require(std::abs(frequency(upper, 48000.0, 48000) / neutralHz - 2.0) < 0.02,
            "Twelve MIDI semitones must double VCO frequency");
    require(std::abs(frequency(lower, 48000.0, 48000) / neutralHz - 0.5) < 0.02,
            "A lower octave must halve VCO frequency");

    auto wide = p;
    wide.vcoHz = 8000.0f;
    wide.lfoHz = 0.1f;
    wide.lfoDepth = 0.4f;
    wide.lfoRounded = wide.lfoDifferentiated = false;
    // +24 note semitones and the square LFO's -24 semitones cancel. Premature
    // clamping of the transposed knob to 8000 Hz would incorrectly give 2000 Hz.
    const auto cancelled = renderNote(wide, 84, 48000.0, 48000);
    require(std::abs(frequency(cancelled, 48000.0, 4800) - 8000.0) < 2.0,
            "Apply note and LFO modulation before the final safe-frequency clamp");
}

void testLfoAndDelayDoNotTranspose()
{
    auto p = dryParameters();
    p.lfoHz = 3.25f;
    p.lfoDepth = 0.5f;
    p.lfoRounded = false;
    p.lfoDifferentiated = true;
    NoteVoice a, b;
    a.prepare(48000.0, 64);
    b.prepare(48000.0, 64);
    a.setParameters(p);
    b.setParameters(p);
    a.noteOn(1, 60);
    b.noteOn(1, 84);
    renderOn(a, 15379);
    renderOn(b, 15379);
    require(a.getLfoPhase() == b.getLfoPhase() && a.getLfoValue() == b.getLfoValue(),
            "MIDI pitch must not transpose LFO timing or its selected waveform");

    p = dryParameters();
    p.delayMs = 137.0f;
    p.repeats = 0.0f;
    for (int note : { 48, 60, 84 })
    {
        p.mix = 0.0f;
        const auto dry = renderNote(p, note, 48000.0, 24000);
        p.mix = 1.0f;
        const auto wet = renderNote(p, note, 48000.0, 24000);
        std::size_t onset = 0;
        while (onset < dry.size() && std::abs(dry[onset] - wet[onset]) < 1.0e-5f)
            ++onset;
        require(std::abs(static_cast<double>(onset) / 48.0 - 137.0) < 1.0,
                "Delay time must remain fixed in milliseconds across MIDI pitches");
    }
}

void testOverlapChannelsAndRepeatedKeys()
{
    NoteVoice voice;
    voice.prepare(48000.0, 64);
    voice.setParameters(dryParameters());
    voice.noteOn(1, 60);
    renderOn(voice, 12000);
    voice.noteOn(2, 72);
    require(std::abs(frequency(renderOn(voice, 24000), 48000.0, 4800) - 360.0) < 3.0,
            "The last note must take monophonic priority across channels");
    voice.noteOff(1, 72);
    require(voice.isActive(), "An unrelated channel/key Note Off must not mute the selected voice");
    voice.noteOff(2, 72);
    require(std::abs(frequency(renderOn(voice, 24000), 48000.0, 4800) - 180.0) < 3.0,
            "Releasing the selected note must restore the last held note");
    voice.noteOff(1, 60);
    renderOn(voice, 1000);
    require(!voice.isActive(), "Releasing the final held note must finish the gate");

    voice.noteOn(1, 60);
    renderOn(voice, 1000);
    voice.noteOn(1, 60);
    require(voice.getLfoPhase() == 0.0f, "Repeated same-key Note On must retrigger");
    voice.noteOff(1, 60);
    renderOn(voice, 1000);
    require(voice.isActive(), "One Note Off must preserve a second matched same-key press");
    voice.noteOff(1, 60);
    renderOn(voice, 1000);
    require(!voice.isActive(), "Matched repeated-key releases must not leave a stuck note");

    voice.noteOn(1, 60);
    voice.noteOn(2, 60);
    voice.noteOff(1, 60);
    renderOn(voice, 1000);
    require(voice.isActive(), "Same pitch on another channel has independent ownership");
    voice.noteOff(2, 60);
    renderOn(voice, 1000);
    require(!voice.isActive(), "The final channel release must silence the shared pitch");
}

void testAllNotesAndSoundOff()
{
    NoteVoice voice;
    voice.prepare(48000.0, 64);
    voice.setParameters(dryParameters());
    voice.noteOn(1, 60);
    voice.noteOn(2, 72);
    renderOn(voice, 1000);
    voice.allNotesOff(1);
    require(voice.isActive(), "Channel All Notes Off must preserve another channel");
    voice.allNotesOff(2);
    require(voice.isActive(), "All Notes Off uses the normal short release");
    renderOn(voice, 240);
    require(!voice.isActive(), "All Notes Off must complete within the declared tail");

    voice.noteOn(1, 60);
    voice.noteOn(2, 72);
    renderOn(voice, 1000);
    voice.allSoundOff(2);
    require(voice.isActive(), "Channel All Sound Off may restore a note held on another channel");
    const auto restored = renderOn(voice, 24000);
    require(restored.front() == 0.0f, "All Sound Off must not carry the killed channel into its fallback attack");
    require(std::abs(frequency(restored, 48000.0, 4800) - 180.0) < 3.0, "Fallback after channel panic must use the remaining note");
    voice.allSoundOff();
    require(!voice.isActive() && silence(renderOn(voice, 1000)), "Global All Sound Off must mute immediately");
    voice.noteOn(16, 127);
    voice.allNotesOff();
    renderOn(voice, 1000);
    require(!voice.isActive(), "Global All Notes Off must include MIDI channel 16");
}

void testWaveformTelemetry()
{
    auto p = dryParameters();
    p.lfoHz = 2.0f;
    p.lfoRounded = false;
    NoteVoice square, rounded, differentiated;
    for (auto* voice : { &square, &rounded, &differentiated })
        voice->prepare(48000.0, 64);
    square.setParameters(p);
    p.lfoRounded = true;
    rounded.setParameters(p);
    p.lfoRounded = false;
    p.lfoDifferentiated = true;
    differentiated.setParameters(p);
    for (auto* voice : { &square, &rounded, &differentiated })
    {
        voice->noteOn(1, 60);
        renderOn(*voice, 4800);
        require(std::abs(voice->getLfoPhase() - 0.2f) < 1.0e-5f, "LFO phase must follow audio time");
    }
    require(square.getLfoValue() == 1.0f, "Square telemetry must show the selected high level");
    require(rounded.getLfoValue() > 0.7f && rounded.getLfoValue() < 0.8f,
            "Rounded telemetry must expose the RC trajectory, not an invented sine");
    require(differentiated.getLfoValue() > 0.15f && differentiated.getLfoValue() < 0.22f,
            "Differentiated telemetry must expose the decaying edge tail");
    renderOn(square, 9600);
    require(square.getLfoValue() == -1.0f, "Square telemetry must follow the falling edge");
}

void testBlocksResetAndEmptyInput()
{
    Parameters p;
    p.oscillation = p.sync = true;
    p.mix = 0.9f;
    p.delayMs = 35.0f;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const int samples = static_cast<int>(rate);
        const auto fixed = renderNote(p, 67, rate, samples, { 256 });
        const auto varied = renderNote(p, 67, rate, samples, { 1, 17, 127, 2048 });
        require(difference(fixed, varied) == 0.0, "Gate and DSP must be independent of block partitioning");
    }
    NoteVoice voice;
    voice.process(nullptr, nullptr, 0);
    voice.process(nullptr, nullptr, -1);
    voice.noteOn(1, 60);
    require(!voice.isActive(), "An unprepared voice must ignore Note On");
    voice.prepare(48000.0, 64);
    voice.setParameters(p);
    voice.noteOn(1, 67);
    const auto original = renderOn(voice, 12000);
    const float phase = voice.getLfoPhase(), value = voice.getLfoValue();
    voice.process(nullptr, nullptr, 0);
    voice.process(nullptr, nullptr, -3);
    require(voice.getLfoPhase() == phase && voice.getLfoValue() == value,
            "Zero-length processing must not advance any audible timeline");
    voice.reset();
    require(!voice.isActive() && silence(renderOn(voice, 1000)), "Reset must clear held keys and return to idle");
    voice.noteOn(1, 67);
    require(difference(original, renderOn(voice, 12000)) == 0.0, "Reset must retain parameters and reproduce a new trigger");
    voice.prepare(44100.0, 1);
    require(!voice.isActive(), "Reprepare must discard held-note state");
    voice.noteOn(1, 67);
    require(difference(renderOn(voice, 44100), renderNote(p, 67, 44100.0, 44100)) == 0.0,
            "Reprepare must preserve parameters with the new sample rate");
}

void testRealtimeStorageAndEventCapacity()
{
    NoteVoice voice;
    voice.prepare(48000.0, 64);
    auto p = dryParameters();
    p.mix = p.repeats = p.lfoDepth = 1.0f;
    p.oscillation = p.sync = true;
    voice.setParameters(p);
    std::array<float, 127> left {}, right {};
    allocationProbe::count = 0;
    allocationProbe::active = true;
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note <= 127; ++note)
            voice.noteOn(channel, note);
    voice.noteOn(1, 0); // Full capacity, existing pair moved to last priority.
    for (int i = 0; i < 2000; ++i)
    {
        const int channel = i % 16 + 1, note = i % 128;
        voice.noteOn(channel, note);
        p.delayMs = (i & 1) != 0 ? 35.0f : 650.0f;
        p.vcoHz = (i & 2) != 0 ? 10.0f : 8000.0f;
        voice.setParameters(p);
        voice.process(left.data(), right.data(), static_cast<int>(left.size()));
        voice.noteOff(channel, note);
        if (i % 73 == 0)
            voice.allNotesOff(channel);
        if (i % 127 == 0)
            voice.allSoundOff(channel);
        for (float sample : left)
            if (!std::isfinite(sample) || std::abs(sample) > 0.980001f)
            {
                allocationProbe::active = false;
                require(false, "Extreme note/parameter automation must stay finite");
            }
    }
    voice.allSoundOff();
    voice.process(left.data(), right.data(), static_cast<int>(left.size()));
    allocationProbe::active = false;
   #if !defined(SIGNAL2M_NOTE_TEST_ASAN)
    require(allocationProbe::count == 0, "Note events, reset, parameters and processing must not allocate");
   #else
    std::cout << "    ASan owns the allocator; the normal build probes realtime allocations.\n";
   #endif
    require(!voice.isActive(), "Panic must clear all held notes even after capacity stress");
    require(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0.0f; }),
            "No held-note capacity or duplicate sequence may survive a global panic");
    voice.noteOn(0, 60);
    voice.noteOn(17, 60);
    voice.noteOn(1, -1);
    voice.noteOn(1, 128);
    require(!voice.isActive(), "Invalid MIDI channels and keys must be ignored");
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "idle gate, long note and complete 5 ms release", testIdleGateLongNoteAndRelease },
        { "retrigger continuity, phase/filter/delay reset and repeatability", testRetriggerHistoryAndContinuity },
        { "neutral note, octave ratios and modulation-before-clamp", testMidiPitchAndModulationOrder },
        { "LFO rate and delay time independent of MIDI pitch", testLfoAndDelayDoNotTranspose },
        { "overlap, last-note priority, channels and repeated keys", testOverlapChannelsAndRepeatedKeys },
        { "channel/global All Notes Off and All Sound Off", testAllNotesAndSoundOff },
        { "actual selected LFO waveform telemetry", testWaveformTelemetry },
        { "sample rates, blocks, reset, reprepare and empty input", testBlocksResetAndEmptyInput },
        { "realtime storage, extreme events and full held-note capacity", testRealtimeStorageAndEventCapacity }
    };
    int failed = 0;
    for (const auto& test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception& error)
        {
            allocationProbe::active = false;
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failed == 0 ? "All NoteVoice behavioral checks passed.\n" : "NoteVoice behavioral checks failed.\n");
    return failed == 0 ? 0 : 1;
}

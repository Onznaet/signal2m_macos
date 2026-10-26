#include "SynthEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocationProbe
{
bool active = false;
std::size_t count = 0;
}

#if defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define SIGNAL2M_TEST_ASAN 1
 #endif
#endif

// The normal executable probes realtime allocations. ASan supplies its own
// new/delete interception, so that build instead checks the memory accesses.
#if !defined(SIGNAL2M_TEST_ASAN)
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
using signal2m::Parameters;
using signal2m::SynthEngine;

constexpr double pi = 3.1415926535897932384626433832795;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Parameters dryParameters()
{
    Parameters p;
    p.vcoHz = 220.0f;
    p.lfoDepth = 0.0f;
    p.master = 1.0f;
    p.mix = 0.0f;
    return p;
}

std::vector<float> renderOn(SynthEngine& engine, int sampleCount,
                            const std::vector<int>& blocks = { 257 })
{
    std::vector<float> result(static_cast<std::size_t>(sampleCount));
    const int largest = *std::max_element(blocks.begin(), blocks.end());
    std::vector<float> right(static_cast<std::size_t>(largest));
    int offset = 0;
    std::size_t blockIndex = 0;
    while (offset < sampleCount)
    {
        const int count = std::min(blocks[blockIndex++ % blocks.size()], sampleCount - offset);
        engine.process(result.data() + offset, right.data(), count);
        for (int i = 0; i < count; ++i)
        {
            const float value = result[static_cast<std::size_t>(offset + i)];
            require(std::isfinite(value), "Audio contains a non-finite sample");
            require(std::abs(value) <= 0.980001f, "Audio exceeds the documented output bound");
            require(value == right[static_cast<std::size_t>(i)], "Mono instrument must populate both channels");
        }
        offset += count;
    }
    return result;
}

std::vector<float> render(Parameters parameters, double sampleRate, double seconds,
                          const std::vector<int>& blocks = { 257 })
{
    SynthEngine engine;
    engine.prepare(sampleRate, 64);
    engine.setParameters(parameters);
    engine.reset();
    return renderOn(engine, static_cast<int>(std::lround(seconds * sampleRate)), blocks);
}

double rms(const std::vector<float>& audio, std::size_t begin = 0,
           std::size_t end = std::numeric_limits<std::size_t>::max())
{
    end = std::min(end, audio.size());
    double energy = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        energy += static_cast<double>(audio[i]) * audio[i];
    return std::sqrt(energy / static_cast<double>(end - begin));
}

double difference(const std::vector<float>& a, const std::vector<float>& b,
                  std::size_t begin = 0, std::size_t end = std::numeric_limits<std::size_t>::max())
{
    require(a.size() == b.size(), "Comparison lengths differ");
    end = std::min(end, a.size());
    double energy = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const double delta = static_cast<double>(a[i]) - b[i];
        energy += delta * delta;
    }
    return std::sqrt(energy / static_cast<double>(end - begin));
}

double risingCrossingFrequency(const std::vector<float>& audio, double sampleRate,
                               std::size_t begin, std::size_t end)
{
    int crossings = 0;
    for (std::size_t i = std::max<std::size_t>(1, begin); i < end; ++i)
        if (audio[i - 1] <= 0.0f && audio[i] > 0.0f)
            ++crossings;
    return crossings * sampleRate / static_cast<double>(end - begin);
}

std::size_t firstDifference(const std::vector<float>& a, const std::vector<float>& b,
                            double threshold = 1.0e-5)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(static_cast<double>(a[i]) - b[i]) > threshold)
            return i;
    return a.size();
}

// A measurement filter, independent of the engine's delay filtering. Four poles
// reject the new high-frequency VCO so old low-frequency loop energy can be heard.
std::vector<float> lowpassMeasurement(const std::vector<float>& audio, double sampleRate,
                                       double cutoff)
{
    const double coefficient = std::exp(-2.0 * pi * cutoff / sampleRate);
    std::array<double, 4> state {};
    std::vector<float> result(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i)
    {
        double sample = audio[i];
        for (double& pole : state)
        {
            pole = sample + coefficient * (pole - sample);
            sample = pole;
        }
        result[i] = static_cast<float>(sample);
    }
    return result;
}

void testGenerationAndPitch()
{
    auto p = dryParameters();
    const auto low = render(p, 48000.0, 2.0);
    require(rms(low, 4800) > 0.1, "Instrument must generate sustained sound without MIDI");
    const double fLow = risingCrossingFrequency(low, 48000.0, 4800, low.size());
    require(std::abs(fLow - 220.0) < 1.0, "VCO frequency does not follow its Hz setting");
    p.vcoHz = 880.0f;
    const auto high = render(p, 48000.0, 2.0);
    const double fHigh = risingCrossingFrequency(high, 48000.0, 4800, high.size());
    require(std::abs(fHigh / fLow - 4.0) < 0.02, "VCO frequency ratio must track its control");

    // A ramp has a strong second harmonic; this rejects a placeholder sine VCO.
    const auto harmonicAmplitude = [&](double frequency)
    {
        double real = 0.0, imaginary = 0.0;
        for (std::size_t i = 48000; i < low.size(); ++i)
        {
            const double angle = 2.0 * pi * frequency * static_cast<double>(i) / 48000.0;
            real += low[i] * std::cos(angle);
            imaginary += low[i] * std::sin(angle);
        }
        return std::hypot(real, imaginary);
    };
    const double ratio = harmonicAmplitude(440.0) / harmonicAmplitude(220.0);
    require(ratio > 0.35 && ratio < 0.65, "VCO must have a ramp/saw harmonic structure");
}

void testMasterAndSmoothing()
{
    auto p = dryParameters();
    const auto full = render(p, 48000.0, 0.5);
    p.master = 0.25f;
    const auto quiet = render(p, 48000.0, 0.5);
    require(std::abs(rms(quiet) / rms(full) - 0.25) < 1.0e-5, "Master must scale the full output");
    p.master = 0.0f;
    require(rms(render(p, 48000.0, 0.5)) == 0.0, "Master zero must silence the instrument");

    SynthEngine engine;
    engine.prepare(48000.0, 64);
    p.master = 1.0f;
    engine.setParameters(p);
    engine.reset();
    renderOn(engine, 12000);
    p.master = 0.0f;
    engine.setParameters(p);
    const auto fade = renderOn(engine, 12000, { 1, 19, 256 });
    require(rms(fade, 0, 96) > 0.04, "Master automation must be smoothed rather than jump to zero");
    require(rms(fade, 7200) < 1.0e-4, "Smoothed Master must reach silence promptly");
}

void testLfoControlsAndSwitches()
{
    auto p = dryParameters();
    p.vcoHz = 300.0f;
    p.lfoHz = 3.0f;
    p.lfoRounded = false;
    const auto unmodulated = render(p, 48000.0, 2.0);
    p.lfoDepth = 0.4f;
    const auto square = render(p, 48000.0, 2.0);
    require(difference(unmodulated, square, 4800) > 0.12, "LFO depth must modulate pitch");
    const double lower = risingCrossingFrequency(square, 48000.0, 32000, 38000);
    const double upper = risingCrossingFrequency(square, 48000.0, 40000, 46000);
    require(upper > 5.0 * lower, "Square LFO must alternate between distinct VCO pitches");

    p.lfoHz = 7.0f;
    const auto faster = render(p, 48000.0, 2.0);
    require(difference(square, faster, 4800) > 0.12, "LFO rate must change the modulation timeline");
    p.lfoHz = 3.0f;
    p.lfoDifferentiated = true;
    const auto differentiated = render(p, 48000.0, 2.0);
    require(difference(square, differentiated, 4800) > 0.10, "S1 must select square versus edge-tail modulation");
    p.lfoRounded = true;
    const auto rounded = render(p, 48000.0, 2.0);
    require(difference(rounded, square, 4800) > 0.10, "S2 must select the RC-rounded modulation branch");
    require(difference(rounded, differentiated, 4800) > 0.10, "Rounded and differentiated LFOs must differ");
    p.lfoDifferentiated = false;
    const auto roundedOtherS1 = render(p, 48000.0, 2.0);
    require(difference(rounded, roundedOtherS1) == 0.0, "S1 is bypassed when S2 selects its RC branch");

    p.lfoDepth = 0.0f;
    p.lfoHz = 137.0f;
    require(difference(unmodulated, render(p, 48000.0, 2.0)) == 0.0,
            "LFO shapes and rate must not affect the unsynced VCO at zero depth");
}

void testSyncOnBothEdges()
{
    auto p = dryParameters();
    p.vcoHz = 173.0f;
    p.lfoHz = 20.0f;
    const auto free = render(p, 48000.0, 1.0);
    p.sync = true;
    const auto synced = render(p, 48000.0, 1.0);
    require(difference(free, synced, 24000) > 0.1, "Sync must work independently of LFO modulation depth");

    // Both LFO edges are 1200 samples apart. A correctly reset ramp repeats at
    // this interval even though 173 Hz is not an integer multiple of 40 Hz.
    const auto periodicError = [](const std::vector<float>& audio)
    {
        double energy = 0.0;
        for (std::size_t i = 36000; i < audio.size(); ++i)
        {
            const double delta = audio[i] - audio[i - 1200];
            energy += delta * delta;
        }
        return std::sqrt(energy / 12000.0);
    };
    require(periodicError(synced) < 0.002, "Sync must reset on each leading and trailing LFO edge");
    require(periodicError(free) > 0.1, "Sync measurement must distinguish a free-running VCO");
}

void testDelayMixAndRepeats()
{
    for (float delay : { 90.0f, 260.0f })
    {
        auto p = dryParameters();
        p.delayMs = delay;
        p.repeats = 0.0f;
        const auto dry = render(p, 48000.0, 1.0);
        p.mix = 1.0f;
        const auto wet = render(p, 48000.0, 1.0);
        const std::size_t onset = firstDifference(dry, wet);
        const double onsetMs = 1000.0 * static_cast<double>(onset) / 48000.0;
        require(std::abs(onsetMs - delay) < 1.5, "First echo must arrive after the selected delay time");
        require(difference(dry, wet, onset + 240) > 0.05, "Mix must add an audible delayed signal");
    }

    auto p = dryParameters();
    p.delayMs = 137.0f;
    p.mix = 1.0f;
    p.repeats = 0.0f;
    const auto single = render(p, 48000.0, 1.5);
    p.repeats = 0.85f;
    const auto repeated = render(p, 48000.0, 1.5);
    const double onsetMs = 1000.0 * static_cast<double>(firstDifference(single, repeated)) / 48000.0;
    require(std::abs(onsetMs - 274.0) < 2.0, "Feedback must add a second traversal, not alter the first echo");
    require(difference(single, repeated, 24000) > 0.025, "Repeats must change the continuing echo train");
}

std::vector<float> renderHistory(bool oscillation, bool release)
{
    SynthEngine engine;
    engine.prepare(48000.0, 128);
    auto p = dryParameters();
    p.vcoHz = 173.0f;
    p.delayMs = 200.0f;
    p.mix = 1.0f;
    p.repeats = 0.0f;
    engine.setParameters(p);
    engine.reset();
    renderOn(engine, 96000);

    p.vcoHz = 7331.0f;
    p.oscillation = oscillation;
    engine.setParameters(p);
    auto history = renderOn(engine, 480000, { 31, 512, 7 });
    if (release)
    {
        p.oscillation = false;
        engine.setParameters(p);
        history = renderOn(engine, 144000, { 1, 127, 1024 });
    }
    return lowpassMeasurement(history, 48000.0, 300.0);
}

void testOscillationSustainAndRelease()
{
    // Changing source frequency lets the test distinguish historical echo from
    // a generator that simply keeps sounding. Repeats=0 is the negative control.
    const auto ordinary = renderHistory(false, false);
    const auto held = renderHistory(true, false);
    const auto released = renderHistory(true, true);
    const double ordinaryLate = rms(ordinary, 432000);
    const double heldEarly = rms(held, 48000, 96000);
    const double heldLate = rms(held, 432000);
    const double releasedLate = rms(released, 96000);
    require(heldLate > 0.012, "Oscillation must preserve audible low-frequency loop history");
    require(heldLate > ordinaryLate * 10.0, "Sustained history must exceed the continuously driven negative control");
    require(heldLate > heldEarly * 0.35, "Oscillation must not behave like an ordinary decaying tail");
    require(releasedLate < heldLate * 0.1, "Disabling Oscillation must restore Repeats-controlled release");
    std::cout << "    history RMS ordinary=" << ordinaryLate << " held=" << heldLate
              << " released=" << releasedLate << '\n';
}

void testSampleRatesBlocksInstancesAndReset()
{
    Parameters p;
    p.vcoHz = 431.0f;
    p.lfoHz = 7.3f;
    p.lfoDepth = 0.6f;
    p.lfoRounded = false;
    p.lfoDifferentiated = true;
    p.sync = true;
    p.repeats = 0.87f;
    p.mix = 0.8f;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto fixed = render(p, rate, 1.0, { 256 });
        const auto varied = render(p, rate, 1.0, { 1, 17, 127, 512, 2048 });
        require(difference(fixed, varied) == 0.0, "Processing must be independent of audio block boundaries");
        auto frequencyParameters = dryParameters();
        const auto pitch = render(frequencyParameters, rate, 1.5);
        require(std::abs(risingCrossingFrequency(pitch, rate, static_cast<std::size_t>(rate / 2), pitch.size()) - 220.0) < 1.1,
                "VCO timebase must follow the prepared sample rate");
    }

    SynthEngine a, b;
    a.prepare(48000.0, 32);
    b.prepare(48000.0, 512);
    a.setParameters(p);
    b.setParameters(p);
    a.reset();
    b.reset();
    const auto reference = renderOn(a, 24000);
    renderOn(a, 17039);
    const auto independent = renderOn(b, 24000);
    const double instanceDifference = difference(reference, independent);
    if (instanceDifference != 0.0)
    {
        const auto mismatch = firstDifference(reference, independent, 0.0);
        std::cout << "    instance difference=" << instanceDifference << " first mismatch=" << mismatch
                  << " a=" << reference[mismatch] << " b=" << independent[mismatch] << '\n';
    }
    require(instanceDifference == 0.0, "Instances must not share oscillator or delay state");
    a.reset();
    require(difference(reference, renderOn(a, 24000)) == 0.0, "Reset must clear phases, filters and the delay line deterministically");
    a.prepare(44100.0, 1);
    require(difference(renderOn(a, 44100), render(p, 44100.0, 1.0)) == 0.0,
            "Reprepare must rebuild sample-rate-dependent state and preserve parameter targets");
}

void testDelayWrapBoundaries()
{
    // Decimal milliseconds can land infinitesimally above an integer sample.
    // Exercise that position at every ring wrap, including the maximum delay.
    // Running this suite under ASan additionally checks the interpolation reads.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (float delay : { 35.0f, 90.0f, 137.0f, 280.0f, 650.0f })
        {
            auto p = dryParameters();
            p.delayMs = delay;
            p.mix = 1.0f;
            p.repeats = 0.81f;
            SynthEngine engine;
            engine.prepare(rate, 64);
            engine.setParameters(p);
            engine.reset();
            const int count = static_cast<int>(2.1 * rate);
            const auto first = renderOn(engine, count, { 1, 113, 1024 });
            engine.reset();
            const auto reset = renderOn(engine, count, { 512, 7 });
            require(difference(first, reset) == 0.0, "Delay interpolation must remain deterministic at ring boundaries");
        }
    }
}

void testAutomationRobustnessAndRealtimeStorage()
{
    SynthEngine engine;
    engine.prepare(48000.0, 32);
    std::array<float, 127> left {}, right {};
    Parameters p;
    p.oscillation = true;
    p.master = p.mix = p.repeats = p.lfoDepth = 1.0f;
    engine.setParameters(p);
    engine.reset();
    allocationProbe::count = 0;
    allocationProbe::active = true;
    for (int block = 0; block < 2000; ++block)
    {
        p.vcoHz = (block & 1) != 0 ? 10.0f : 8000.0f;
        p.lfoHz = (block & 2) != 0 ? 0.1f : 250.0f;
        p.delayMs = (block & 4) != 0 ? 35.0f : 650.0f;
        p.sync = (block & 8) != 0;
        p.lfoRounded = (block & 16) != 0;
        p.lfoDifferentiated = (block & 32) != 0;
        engine.setParameters(p);
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
        for (float value : left)
            if (!std::isfinite(value) || std::abs(value) > 0.980001f)
            {
                allocationProbe::active = false;
                require(false, "Extreme automated feedback must remain finite and bounded");
            }
    }
    allocationProbe::active = false;
   #if !defined(SIGNAL2M_TEST_ASAN)
    require(allocationProbe::count == 0, "setParameters/process must not allocate realtime storage");
   #else
    std::cout << "    ASan owns the allocator; allocation probe runs in the normal executable.\n";
   #endif

    p.vcoHz = std::numeric_limits<float>::quiet_NaN();
    p.lfoHz = std::numeric_limits<float>::infinity();
    p.lfoDepth = -100.0f;
    p.delayMs = -std::numeric_limits<float>::infinity();
    p.mix = std::numeric_limits<float>::quiet_NaN();
    p.master = std::numeric_limits<float>::infinity();
    p.repeats = 100.0f;
    engine.setParameters(p);
    const auto recovered = renderOn(engine, 48000);
    require(rms(recovered) > 0.0001, "Invalid parameter values must recover to a working finite model");

    for (double rate : { 44100.0, 48000.0 })
    {
        engine.prepare(rate, 16);
        p = dryParameters();
        p.vcoHz = 10.0f;
        p.lfoHz = 250.0f;
        p.lfoDepth = 1.0f;
        p.delayMs = 650.0f;
        p.repeats = p.mix = 1.0f;
        p.oscillation = p.sync = true;
        engine.setParameters(p);
        engine.reset();
        // Two minutes per sample rate: real feedback circulation, without
        // allocating a multi-minute capture or depending on wall-clock timers.
        const int blocks = static_cast<int>(std::ceil(120.0 * rate / left.size()));
        double lateEnergy = 0.0;
        for (int block = 0; block < blocks; ++block)
        {
            engine.process(left.data(), right.data(), static_cast<int>(left.size()));
            for (float value : left)
            {
                require(std::isfinite(value) && std::abs(value) <= 0.980001f,
                        "Long Oscillation run must remain finite and bounded");
                if (block >= blocks - 100)
                    lateEnergy += static_cast<double>(value) * value;
            }
        }
        require(lateEnergy > 1.0e-5, "Long Oscillation run must remain active");
    }
}

void testUnpreparedAndEmptyBlocks()
{
    SynthEngine engine;
    std::array<float, 8> left, right;
    left.fill(1.0f);
    right.fill(1.0f);
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    require(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0.0f; }),
            "An unprepared engine must return silence");
    engine.prepare(std::numeric_limits<double>::quiet_NaN(), 0);
    engine.process(left.data(), right.data(), 0);
    engine.process(left.data(), right.data(), -1);
    require(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0.0f; }),
            "Empty blocks must leave output storage untouched");
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "generation, VCO pitch and saw harmonics", testGenerationAndPitch },
        { "Master and continuous parameter smoothing", testMasterAndSmoothing },
        { "LFO rate/depth and physical S1/S2 branch selection", testLfoControlsAndSwitches },
        { "Sync on both LFO edges with modulation depth zero", testSyncOnBothEdges },
        { "Mix, delay arrival time and feedback traversal", testDelayMixAndRepeats },
        { "Oscillation historical sustain and release", testOscillationSustainAndRelease },
        { "sample rates, block sizes, instances and reset", testSampleRatesBlocksInstancesAndReset },
        { "fractional delay interpolation at ring boundaries", testDelayWrapBoundaries },
        { "automation, realtime allocations and long feedback stability", testAutomationRobustnessAndRealtimeStorage },
        { "unprepared engine and empty blocks", testUnpreparedAndEmptyBlocks }
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
            ++failed;
            allocationProbe::active = false;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failed == 0 ? "All DSP behavioral checks passed.\n" : "DSP behavioral checks failed.\n");
    return failed == 0 ? 0 : 1;
}

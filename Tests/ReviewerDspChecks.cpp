// Independent black-box behavioral checks. Product implementation is not duplicated.
#include "SynthEngine.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace
{
using signal2m::Parameters;
using signal2m::SynthEngine;
constexpr double pi = 3.14159265358979323846;
int failures = 0;
int assertions = 0;
void expect(bool condition, const std::string& description)
{
    ++assertions;
    std::cout << (condition ? "PASS " : "FAIL ") << description << '\n';
    if (!condition) ++failures;
}
struct Stats
{
    double rms = 0.0, peak = 0.0;
    bool finite = true;
};
Stats stats(const std::vector<float>& x, std::size_t start = 0)
{
    Stats result;
    double sum = 0.0;
    for (std::size_t i = start; i < x.size(); ++i)
    {
        result.finite = result.finite && std::isfinite(x[i]);
        result.peak = std::max(result.peak, std::abs(static_cast<double>(x[i])));
        sum += static_cast<double>(x[i]) * x[i];
    }
    result.rms = std::sqrt(sum / std::max<std::size_t>(1, x.size() - start));
    return result;
}
double difference(const std::vector<float>& a, const std::vector<float>& b, std::size_t begin = 0, std::size_t end = 0)
{
    end = end == 0 ? std::min(a.size(), b.size()) : std::min({end, a.size(), b.size()});
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const double value = static_cast<double>(a[i]) - b[i];
        sum += value * value;
    }
    return std::sqrt(sum / std::max<std::size_t>(1, end - begin));
}
std::vector<float> render(Parameters parameters, double sr, double seconds,
                          const std::vector<int>& chunks = {257}, int prepareHint = 257)
{
    SynthEngine engine;
    engine.setParameters(parameters);
    engine.prepare(sr, prepareHint);
    const auto count = static_cast<std::size_t>(std::llround(seconds * sr));
    std::vector<float> result(count), right(2048);
    std::size_t position = 0, chunk = 0;
    while (position < count)
    {
        const int size = static_cast<int>(std::min<std::size_t>(chunks[chunk++ % chunks.size()], count - position));
        if (right.size() < static_cast<std::size_t>(size)) right.resize(static_cast<std::size_t>(size));
        engine.process(result.data() + position, right.data(), size);
        for (int i = 0; i < size; ++i)
            if (result[position + static_cast<std::size_t>(i)] != right[static_cast<std::size_t>(i)])
            {
                expect(false, "left/right mono duplication");
                return result;
            }
        position += static_cast<std::size_t>(size);
    }
    return result;
}
Parameters dryParameters()
{
    Parameters p;
    p.vcoHz = 220.0f;
    p.lfoDepth = 0.0f;
    p.master = 0.7f;
    p.mix = 0.0f;
    p.repeats = 0.0f;
    return p;
}
double measuredFrequency(const std::vector<float>& x, double sr, double ignoreSeconds = 0.3)
{
    std::vector<std::size_t> crossings;
    for (std::size_t i = std::max<std::size_t>(1, static_cast<std::size_t>(sr * ignoreSeconds)); i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) crossings.push_back(i);
    if (crossings.size() < 3) return 0.0;
    return static_cast<double>(crossings.size() - 1) * sr
         / static_cast<double>(crossings.back() - crossings.front());
}
int modulationTransitions(const std::vector<float>& x, double sr)
{
    std::size_t lastCrossing = 0;
    bool haveLast = false;
    int state = -1, transitions = 0;
    for (std::size_t i = static_cast<std::size_t>(sr * 0.3); i < x.size(); ++i)
    {
        if (x[i - 1] > 0.0f || x[i] <= 0.0f) continue;
        if (haveLast)
        {
            const double frequency = sr / static_cast<double>(i - lastCrossing);
            const int newState = frequency < 145.0 ? 0 : (frequency > 275.0 ? 1 : -1);
            if (newState >= 0)
            {
                if (state >= 0 && newState != state) ++transitions;
                state = newState;
            }
        }
        lastCrossing = i;
        haveLast = true;
    }
    return transitions;
}
void generationAndControls(double sr)
{
    const std::string label = std::to_string(static_cast<int>(sr));
    auto p = dryParameters();
    for (const int block : {1, 17, 64, 257, 1024})
    {
        auto output = render(p, sr, 1.0, {block}, block);
        const auto s = stats(output);
        expect(s.finite && s.rms > 0.02 && s.peak <= 1.0,
               "generation without MIDI finite/nonzero sr=" + label + " block=" + std::to_string(block));
        expect(std::abs(measuredFrequency(output, sr) - 220.0) < 1.0,
               "physical VCO frequency sr=" + label + " block=" + std::to_string(block));
    }
    p.vcoHz = 120.0f;
    const auto low = render(p, sr, 1.0);
    p.vcoHz = 480.0f;
    const auto high = render(p, sr, 1.0);
    expect(std::abs(measuredFrequency(high, sr) / measuredFrequency(low, sr) - 4.0) < 0.025,
           "VCO knob changes measured pitch over two octaves sr=" + label);

    p = dryParameters();
    const auto unmodulated = render(p, sr, 4.0);
    p.vcoHz = 200.0f;
    p.lfoRounded = false;
    p.lfoDifferentiated = false;
    p.lfoDepth = 0.2f;
    p.lfoHz = 1.0f;
    const auto slow = render(p, sr, 4.0);
    p.lfoHz = 4.0f;
    const auto fast = render(p, sr, 4.0);
    const int slowTransitions = modulationTransitions(slow, sr);
    const int fastTransitions = modulationTransitions(fast, sr);
    std::cout << "MEASURE lfo-transitions sr=" << sr << " slow=" << slowTransitions << " fast=" << fastTransitions << '\n';
    expect(difference(unmodulated, slow) > 0.03, "LFO depth changes audio sr=" + label);
    expect(slowTransitions >= 6 && slowTransitions <= 8 && fastTransitions >= 28 && fastTransitions <= 31,
           "LFO rate changes physical modulation cadence sr=" + label);

    p = dryParameters();
    p.vcoHz = 333.0f;
    p.lfoDepth = 0.45f;
    p.lfoHz = 2.0f;
    p.lfoRounded = false;
    const auto square = render(p, sr, 2.0);
    p.lfoDifferentiated = true;
    const auto differentiated = render(p, sr, 2.0);
    p.lfoRounded = true;
    const auto roundedOne = render(p, sr, 2.0);
    p.lfoDifferentiated = false;
    const auto roundedTwo = render(p, sr, 2.0);
    expect(difference(square, differentiated) > 0.02 && difference(square, roundedOne) > 0.02
           && difference(differentiated, roundedOne) > 0.02,
           "S1/S2 produce square, differentiated, and RC families sr=" + label);
    expect(difference(roundedOne, roundedTwo) < 1.0e-7,
           "S1 is bypassed by S2 RC selection sr=" + label);

    p = dryParameters();
    p.vcoHz = 433.7f;
    p.lfoHz = 5.0f;
    const auto free = render(p, sr, 2.0);
    p.sync = true;
    const auto synced = render(p, sr, 2.0);
    const auto halfLfo = static_cast<std::size_t>(std::llround(sr * 0.1));
    std::vector<float> shiftedSync(synced.begin() + static_cast<std::ptrdiff_t>(halfLfo), synced.end());
    std::vector<float> shiftedFree(free.begin() + static_cast<std::ptrdiff_t>(halfLfo), free.end());
    const double syncPeriodError = difference(synced, shiftedSync, static_cast<std::size_t>(sr));
    const double freePeriodError = difference(free, shiftedFree, static_cast<std::size_t>(sr));
    expect(difference(free, synced) > 0.02, "Sync affects audio with LFO depth zero sr=" + label);
    expect(syncPeriodError < 0.002 && freePeriodError > 0.02,
           "Sync repeats VCO waveform at both LFO edges sr=" + label);
    expect(difference(free, synced, 0, halfLfo - 2) < 1.0e-7,
           "first Sync event occurs at a physical LFO edge sr=" + label);
}
void delayAndLevels(double sr)
{
    const std::string label = std::to_string(static_cast<int>(sr));
    auto p = dryParameters();
    p.vcoHz = 137.0f;
    for (const float milliseconds : {50.0f, 187.0f, 650.0f})
    {
        p.delayMs = milliseconds;
        p.mix = 0.0f;
        auto dry = render(p, sr, 1.8);
        p.mix = 1.0f;
        auto wet = render(p, sr, 1.8);
        std::size_t onset = wet.size();
        for (std::size_t i = 0; i < wet.size(); ++i)
            if (std::abs(wet[i] - dry[i]) > 1.0e-6f) { onset = i; break; }
        const double observedMs = 1000.0 * static_cast<double>(onset) / sr;
        std::cout << "MEASURE echo-onset sr=" << sr << " requested_ms=" << milliseconds << " observed_ms=" << observedMs << '\n';
        expect(std::abs(observedMs - milliseconds) <= 3.0 * 1000.0 / sr,
               "Delay sets physical first echo time sr=" + label + " ms=" + std::to_string(milliseconds));
        expect(difference(dry, wet, static_cast<std::size_t>(sr)) > 0.015,
               "Mix adds an audible echo sr=" + label + " ms=" + std::to_string(milliseconds));
    }
    p.delayMs = 187.0f;
    p.repeats = 0.0f;
    const auto noRepeat = render(p, sr, 2.0);
    p.repeats = 0.8f;
    const auto repeated = render(p, sr, 2.0);
    expect(difference(noRepeat, repeated, 0, static_cast<std::size_t>(sr * 0.374) - 3) < 1.0e-7,
           "Repeats leaves first pass unchanged before a full feedback round trip sr=" + label);
    expect(difference(noRepeat, repeated, static_cast<std::size_t>(sr * 0.5)) > 0.01,
           "Repeats changes later echoes sr=" + label);
    p.master = 0.8f;
    const auto loud = render(p, sr, 1.5);
    p.master = 0.2f;
    const auto quiet = render(p, sr, 1.5);
    const double levelRatio = stats(quiet).rms / stats(loud).rms;
    expect(std::abs(levelRatio - 0.25) < 0.002, "Master scales dry and echo together sr=" + label);
    p.master = 0.0f;
    expect(stats(render(p, sr, 0.5)).peak == 0.0, "Master zero gives silence sr=" + label);
}
void lifecycleAndInstances()
{
    auto p = dryParameters();
    p.lfoDepth = 0.8f; p.lfoHz = 13.7f; p.mix = 0.9f; p.repeats = 0.95f;
    p.sync = true; p.lfoRounded = false; p.lfoDifferentiated = true;
    const auto serial = render(p, 48000.0, 2.0, {1}, 1);
    const auto variable = render(p, 48000.0, 2.0, {17, 1, 1024, 257, 64, 511}, 17);
    expect(difference(serial, variable) < 1.0e-8,
           "block changes including blocks larger than prepare hint preserve sample timeline");
    SynthEngine engine;
    engine.setParameters(p); engine.prepare(44100.0, 64);
    std::vector<float> first(44100), second(44100), right(44100);
    engine.process(first.data(), right.data(), 44100);
    engine.reset();
    engine.process(second.data(), right.data(), 44100);
    expect(difference(first, second) == 0.0, "reset preserves settings and clears all history");
    engine.prepare(48000.0, 1024);
    engine.process(second.data(), right.data(), 44100);
    engine.prepare(44100.0, 17);
    engine.process(second.data(), right.data(), 44100);
    for (std::size_t i = 0; i < first.size(); ++i) if (first[i] != second[i]) { std::cout << "MEASURE first-reprepare-different=" << i << " a=" << first[i] << " b=" << second[i] << '\n'; break; }
    std::cout << "MEASURE reprepare-difference=" << difference(first, second) << " first=" << first[0] << " second=" << second[0] << '\n';
    expect(difference(first, second) == 0.0, "re-prepare at another SR then return resets history correctly");

    const auto expected = render(p, 48000.0, 2.0, {257});
    SynthEngine a, b;
    a.setParameters(p); a.prepare(48000.0, 257);
    auto other = dryParameters(); other.vcoHz = 71.0f; other.oscillation = true; other.mix = 1.0f;
    b.setParameters(other); b.prepare(44100.0, 17);
    std::vector<float> actual(expected.size()), bLeft(257), bRight(257), aRight(257);
    for (std::size_t at = 0; at < actual.size(); at += 257)
    {
        const int n = static_cast<int>(std::min<std::size_t>(257, actual.size() - at));
        a.process(actual.data() + at, aRight.data(), n);
        other.vcoHz = at % 2 == 0 ? 8000.0f : 10.0f;
        b.setParameters(other);
        if (at % (257 * 11) == 0) b.reset();
        b.process(bLeft.data(), bRight.data(), n);
    }
    expect(difference(expected, actual) == 0.0, "interleaved independently changed/reset instances remain isolated");
}
// Spectral energy around a former low note. Hann-windowed Goertzel bins avoid
// counting the continuously running high note as a sustained old echo.
double oldNoteBand(const std::vector<float>& input, double sr)
{
    if (input.empty()) return 0.0;
    std::vector<double> windowed(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
        windowed[i] = input[i] * (0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(input.size() - 1)));
    double sumPower = 0.0;
    for (int hz = 65; hz <= 135; ++hz)
    {
        const double coefficient = 2.0 * std::cos(2.0 * pi * hz / sr);
        double q1 = 0.0, q2 = 0.0;
        for (const double value : windowed)
        {
            const double q0 = value + coefficient * q1 - q2;
            q2 = q1; q1 = q0;
        }
        sumPower += std::max(0.0, q1 * q1 + q2 * q2 - coefficient * q1 * q2);
    }
    return 2.0 * std::sqrt(sumPower) / static_cast<double>(input.size());
}
struct TailResult { double early = 0.0, late = 0.0, released = 0.0; bool finite = true; };
TailResult tailHistory(double sr, bool oscillation)
{
    auto p = dryParameters();
    p.vcoHz = 100.0f; p.delayMs = 200.0f; p.master = 0.5f;
    p.repeats = 1.0f; p.oscillation = oscillation;
    SynthEngine dry, mixed;
    dry.setParameters(p); dry.prepare(sr, 257);
    p.mix = 1.0f; mixed.setParameters(p); mixed.prepare(sr, 257);
    std::vector<float> dryLeft(257), dryRight(257), mixedLeft(257), mixedRight(257);
    std::vector<float> early, late, released;
    const auto total = static_cast<std::size_t>(sr * 38.0);
    TailResult result;
    bool switchedPitch = false, switchedOff = false;
    for (std::size_t at = 0; at < total; )
    {
        std::size_t until = total;
        for (const double boundary : {4.0, 5.0, 6.0, 34.0, 35.0, 36.0, 37.0, 38.0})
        {
            const auto frame = static_cast<std::size_t>(std::llround(sr * boundary));
            if (frame > at) until = std::min(until, frame);
        }
        if (!switchedPitch && at >= static_cast<std::size_t>(sr * 4.0))
        {
            p.vcoHz = 8000.0f;
            p.mix = 0.0f; dry.setParameters(p);
            p.mix = 1.0f; mixed.setParameters(p);
            switchedPitch = true;
        }
        if (!switchedOff && at >= static_cast<std::size_t>(sr * 36.0))
        {
            p.oscillation = false; p.repeats = 0.0f;
            p.mix = 0.0f; dry.setParameters(p);
            p.mix = 1.0f; mixed.setParameters(p);
            switchedOff = true;
        }
        const int count = static_cast<int>(std::min<std::size_t>(257, until - at));
        dry.process(dryLeft.data(), dryRight.data(), count);
        mixed.process(mixedLeft.data(), mixedRight.data(), count);
        for (int i = 0; i < count; ++i)
        {
            const auto sample = at + static_cast<std::size_t>(i);
            result.finite = result.finite && std::isfinite(mixedLeft[static_cast<std::size_t>(i)])
                            && std::abs(mixedLeft[static_cast<std::size_t>(i)]) <= 1.0f;
            const float wet = mixedLeft[static_cast<std::size_t>(i)] - dryLeft[static_cast<std::size_t>(i)];
            if (sample >= static_cast<std::size_t>(sr * 5.0) && sample < static_cast<std::size_t>(sr * 6.0)) early.push_back(wet);
            if (sample >= static_cast<std::size_t>(sr * 34.0) && sample < static_cast<std::size_t>(sr * 35.0)) late.push_back(wet);
            if (sample >= static_cast<std::size_t>(sr * 37.0)) released.push_back(wet);
        }
        at += static_cast<std::size_t>(count);
    }
    result.early = oldNoteBand(early, sr);
    result.late = oldNoteBand(late, sr);
    result.released = oldNoteBand(released, sr);
    return result;
}
void feedbackAndRelease(double sr)
{
    const auto normal = tailHistory(sr, false);
    const auto sustained = tailHistory(sr, true);
    std::cout << "MEASURE old-note-band sr=" << sr << " normal=" << normal.early << ',' << normal.late
              << " oscillation=" << sustained.early << ',' << sustained.late << " released=" << sustained.released << '\n';
    const std::string label = std::to_string(static_cast<int>(sr));
    expect(normal.finite && sustained.finite, "38-second history/feedback renders remain finite sr=" + label);
    expect(normal.early > 0.001 && normal.late < normal.early * 0.02,
           "ordinary maximum Repeats still decays after the old source stops sr=" + label);
    expect(sustained.late > 0.001 && sustained.late > sustained.early * 0.15
           && sustained.late > normal.late * 10.0,
           "Oscillation retains a former note for 30 seconds after pitch change sr=" + label);
    expect(sustained.released < sustained.late * 0.02,
           "turning Oscillation off with Repeats zero releases former echo sr=" + label);

    auto p = dryParameters();
    p.vcoHz = 8000.0f; p.lfoHz = 250.0f; p.lfoDepth = 1.0f;
    p.sync = true; p.lfoRounded = false; p.lfoDifferentiated = true;
    p.master = 1.0f; p.mix = 1.0f; p.repeats = 1.0f; p.oscillation = true;
    SynthEngine engine;
    engine.setParameters(p); engine.prepare(sr, 1024);
    std::vector<float> left(1024), right(1024);
    bool finite = true;
    double peak = 0.0;
    const auto count = static_cast<std::size_t>(sr * 90.0);
    for (std::size_t at = 0, block = 0; at < count; ++block)
    {
        if (block % 17 == 0)
        {
            p.delayMs = block % 34 == 0 ? 35.0f : 650.0f;
            engine.setParameters(p);
        }
        const int size = static_cast<int>(std::min<std::size_t>(1024, count - at));
        engine.process(left.data(), right.data(), size);
        for (int i = 0; i < size; ++i)
        {
            finite = finite && std::isfinite(left[static_cast<std::size_t>(i)]) && std::isfinite(right[static_cast<std::size_t>(i)]);
            peak = std::max(peak, std::abs(static_cast<double>(left[static_cast<std::size_t>(i)])));
        }
        at += static_cast<std::size_t>(size);
    }
    std::cout << "MEASURE long-feedback sr=" << sr << " seconds=90 peak=" << peak << '\n';
    expect(finite && peak <= 1.0 && peak > 0.01,
           "90 seconds extreme feedback and Delay automation remain bounded/nonzero sr=" + label);
}
}
int main(int argc, char** argv)
{
    std::cout << std::setprecision(10);
    if (argc > 1 && std::string(argv[1]) == "lifecycle") { lifecycleAndInstances(); return failures == 0 ? 0 : 1; }
    for (const double sr : {44100.0, 48000.0})
    {
        generationAndControls(sr);
        delayAndLevels(sr);
        feedbackAndRelease(sr);
    }
    lifecycleAndInstances();
    auto invalid = dryParameters();
    invalid.vcoHz = std::numeric_limits<float>::quiet_NaN();
    invalid.lfoHz = std::numeric_limits<float>::infinity();
    invalid.delayMs = -100000.0f;
    invalid.repeats = std::numeric_limits<float>::quiet_NaN();
    invalid.oscillation = true;
    expect(stats(render(invalid, 48000.0, 2.0)).finite, "nonfinite/out-of-range control values are sanitised");
    std::cout << "RESULT assertions=" << assertions << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
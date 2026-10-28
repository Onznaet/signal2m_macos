#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace signal2m::tempo
{
// Durations are quarter-note beats, independent of the project's time signature.
// Adjacent pairs are straight and triplet; every next entry is faster.
struct Division
{
    double beats;
    const char* label;
};

inline constexpr std::array<Division, 26> divisions {{
    { 32.0, "8/1" }, { 64.0 / 3.0, "8/1T" },
    { 16.0, "4/1" }, { 32.0 / 3.0, "4/1T" },
    { 8.0, "2/1" }, { 16.0 / 3.0, "2/1T" },
    { 4.0, "1/1" }, { 8.0 / 3.0, "1/1T" },
    { 2.0, "1/2" }, { 4.0 / 3.0, "1/2T" },
    { 1.0, "1/4" }, { 2.0 / 3.0, "1/4T" },
    { 0.5, "1/8" }, { 1.0 / 3.0, "1/8T" },
    { 0.25, "1/16" }, { 1.0 / 6.0, "1/16T" },
    { 0.125, "1/32" }, { 1.0 / 12.0, "1/32T" },
    { 0.0625, "1/64" }, { 1.0 / 24.0, "1/64T" },
    { 0.03125, "1/128" }, { 1.0 / 48.0, "1/128T" },
    { 0.015625, "1/256" }, { 1.0 / 96.0, "1/256T" },
    { 0.0078125, "1/512" }, { 1.0 / 192.0, "1/512T" }
}};

inline constexpr std::size_t delayFirstDivision = 6;
inline constexpr double defaultBpm = 120.0;
inline constexpr double minimumBpm = 10.0;
inline constexpr double maximumBpm = 999.0;
inline constexpr double maximumDelaySeconds = 24.0;

inline double sanitiseBpm(double bpm, double fallback = defaultBpm) noexcept
{
    if (!std::isfinite(bpm) || bpm <= 0.0)
        bpm = std::isfinite(fallback) && fallback > 0.0 ? fallback : defaultBpm;
    return std::clamp(bpm, minimumBpm, maximumBpm);
}

inline std::size_t divisionIndex(float normalised, bool delay = false) noexcept
{
    const double unit = std::isfinite(normalised)
        ? std::clamp(static_cast<double>(normalised), 0.0, 1.0) : 0.0;
    const auto first = delay ? delayFirstDivision : std::size_t { 0 };
    const auto steps = divisions.size() - first - 1;
    return first + static_cast<std::size_t>(std::floor(unit * static_cast<double>(steps) + 0.5));
}

inline const Division& selectedDivision(float normalised, bool delay = false) noexcept
{
    return divisions[divisionIndex(normalised, delay)];
}

inline double lfoFrequency(float normalised, double bpm) noexcept
{
    return sanitiseBpm(bpm) / (60.0 * selectedDivision(normalised).beats);
}

inline double delayMilliseconds(float normalised, double bpm) noexcept
{
    return 60000.0 * selectedDivision(normalised, true).beats / sanitiseBpm(bpm);
}
} // namespace signal2m::tempo

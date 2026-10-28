#pragma once

#include <array>

namespace signal2m::support
{
struct Link
{
    const char* label;
    const char* url;
};

// UTF-8 labels and the fixed Telegram destination. DonationLinks supplies the
// two donation destinations; an empty or unsafe slot is hidden without a row.
inline constexpr std::array<Link, 3> links {{
    { u8"Telegram-канал LMZ", "https://t.me/lenmodzvuk" },
    { u8"Донат 1", "" },
    { u8"Донат 2", "" }
}};
} // namespace signal2m::support

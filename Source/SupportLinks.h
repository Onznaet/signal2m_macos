#pragma once

#include <array>

namespace signal2m::support
{
struct Link
{
    const char* label;
    const char* url;
};

// UTF-8 labels and explicitly configured destinations. Empty URL slots are
// placeholders and never launch a browser or invent payment information.
inline constexpr std::array<Link, 3> links {{
    { u8"Telegram-канал LMZ", "https://t.me/lenmodzvuk" },
    { u8"Донат 1 — ссылка появится позже", "" },
    { u8"Донат 2 — ссылка появится позже", "" }
}};
} // namespace signal2m::support

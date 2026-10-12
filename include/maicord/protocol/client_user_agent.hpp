#pragma once

namespace maicord {

// Windows desktop client user agent. Keep the HTTP header
// and browser_user_agent property on the same value.
inline constexpr char kClientUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "discord/1.0.9256 Chrome/148.0.7778.280 Electron/42.9.0 Safari/537.36";

} // namespace maicord

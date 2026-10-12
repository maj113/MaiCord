#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

// Per-platform presence (a presence's "client_status": desktop,
// mobile, web and embedded consoles), packed two bits per platform in one
// byte. An absent platform is offline; zero means no platform is online.
enum class ClientPlatform : u8 { desktop, mobile, web, embedded };
enum class ClientPlatformStatus : u8 { offline, online, idle, dnd };
inline constexpr u32 kClientPlatformCount = 4;

constexpr ClientPlatformStatus client_status_get(u8 packed, ClientPlatform platform) noexcept {
    return static_cast<ClientPlatformStatus>((packed >> (static_cast<u32>(platform) * 2u)) & 3u);
}

constexpr u8 client_status_with(u8 packed, ClientPlatform platform, ClientPlatformStatus status) noexcept {
    const u32 shift = static_cast<u32>(platform) * 2u;
    return static_cast<u8>((packed & ~(3u << shift)) | ((static_cast<u32>(status) & 3u) << shift));
}

// Mobile-only online, as the official client decides it: mobile is online and
// desktop is not online (web is ignored; desktop idle still counts as mobile).
// The phone replaces the dot only while the overall status is online; idle, do
// not disturb, streaming and offline keep their shapes. The official client also lets
// an online "vr" session block it; that key is not among the four kept here.
constexpr bool client_status_mobile_online(u8 packed) noexcept {
    return client_status_get(packed, ClientPlatform::mobile) == ClientPlatformStatus::online &&
           client_status_get(packed, ClientPlatform::desktop) != ClientPlatformStatus::online;
}

// Reads a client_status object. Unknown platforms and values are ignored (a
// future platform never rejects the presence); null or a non-object is empty.
// False only for a malformed object or a duplicated platform.
bool client_status_decode(JsonValue value, u8* packed) noexcept;

} // namespace maicord

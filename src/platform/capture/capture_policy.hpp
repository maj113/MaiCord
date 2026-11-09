#pragma once

// The decisions of screen capture that need no system: which windows the picker lists, how far a thumbnail is
// reduced, which frames a frame-rate cap admits, and UTF-8 names cut at a character. Shared by both systems' backends
// and tested directly (tests/platform/capture/capture_policy.cpp).

#include "maicord/core/base.hpp"

namespace maicord {

// What the picker knows of a top-level window.
struct CaptureWindowFacts {
    bool visible, cloaked, tool_window, owned, own_process, shell, minimized;
    u32 title_chars;
    u32 width, height; // the client area
};

// Listed: visible and not cloaked (another virtual desktop, a suspended app), a top-level application window (not a
// tool window, not owned by another window, not the desktop), titled, not MaiCord's own, and with a picture to show
// (a minimized window keeps its place: it is captured once restored).
constexpr bool capture_window_listed(const CaptureWindowFacts& window) noexcept {
    return window.visible && !window.cloaked && !window.tool_window && !window.owned && !window.own_process &&
           !window.shell && window.title_chars && (window.minimized || (window.width && window.height));
}

// The mip level of a width x height picture whose extent fits max_width x max_height (each level halves, rounding
// down, never below 1), or the last level when none fits.
constexpr u32 capture_thumbnail_level(u32 width, u32 height, u32 max_width, u32 max_height) noexcept {
    u32 level = 0;
    while ((width > max_width || height > max_height) && (width > 1 || height > 1)) {
        width = width > 1 ? width >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
        ++level;
    }
    return level;
}
constexpr u32 capture_level_extent(u32 extent, u32 level) noexcept {
    const u32 reduced = extent >> level;
    return reduced ? reduced : 1;
}

// A frame-rate cap: admits a frame when it comes at least one interval (with a quarter interval of slack, so a source
// at a multiple of the cap is not beaten down by jitter) after the last admitted slot, keeping the cadence across
// small delays and restarting it after a gap.
struct CapturePacer {
    u64 next_us;
};
constexpr bool capture_pacer_admit(CapturePacer* pacer, u64 timestamp_us, u32 max_fps) noexcept {
    if (!max_fps)
        return true;
    const u64 interval = 1'000'000u / max_fps;
    if (pacer->next_us && timestamp_us + interval / 4 < pacer->next_us)
        return false;
    pacer->next_us =
        pacer->next_us && timestamp_us < pacer->next_us + interval ? pacer->next_us + interval : timestamp_us + interval;
    return true;
}

// The bytes of `text` (`bytes` long) that fit in capacity - 1, ending on a whole UTF-8 sequence.
constexpr u32 capture_name_cut(const char* text, u32 bytes, u32 capacity) noexcept {
    if (!capacity)
        return 0;
    if (bytes < capacity)
        return bytes;
    u32 used = capacity - 1;
    while (used && (static_cast<u8>(text[used]) & 0xc0u) == 0x80u)
        --used;
    return used;
}

} // namespace maicord

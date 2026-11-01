#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class UiInputKind : u8 { key_down, system_key_down, character, system_character, primary_double_click, key_up, count };

struct UiInputEvent {
    UiInputKind kind;
    // Platform key code or text code unit. The router does not interpret it;
    // the platform/application handler owns translation and text assembly.
    u32 code;
    bool repeated = false;
};

using UiInputMask = u32;
constexpr UiInputMask ui_input_mask(UiInputKind kind) noexcept {
    return kind < UiInputKind::count ? 1u << static_cast<u32>(kind) : 0;
}
inline constexpr UiInputMask kUiInputAll = (1u << static_cast<u32>(UiInputKind::count)) - 1;
inline constexpr u32 kUiInputLayerLimit = 16;

enum class UiInputDisposition : u8 { ignored, handled, blocked, failed };
using UiInputHandler = UiInputDisposition (*)(void*, const UiInputEvent&) noexcept;

struct UiInputLayer {
    UiInputHandler handler;
    void* context;
    UiInputMask barrier;
    u32 id;
};

struct UiInputRouteResult {
    UiInputDisposition disposition;
    u32 layer_id;
    constexpr bool consumed() const noexcept { return disposition != UiInputDisposition::ignored; }
};

// Front to back. An ignored event stops at its layer's barrier. The barrier
// is captured before invoking that handler, so closing a layer cannot leak
// the same event into the newly exposed UI. No pointers are retained.
UiInputRouteResult ui_input_route(ConstSlice<UiInputLayer> layers, const UiInputEvent& event) noexcept;
// The owner records the key that closed an overlay. Consume its character,
// repeats and release before routing to newly exposed content. A fresh down
// recovers if the platform lost the old release across a focus transition.
bool ui_input_consume_key_tail(u32* held_key, const UiInputEvent& event) noexcept;

} // namespace maicord

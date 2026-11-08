#include "maicord/ui/core/input_router.hpp"

namespace maicord {
bool ui_input_consume_key_tail(u32* held_key, const UiInputEvent& event) noexcept {
    if (!held_key || !*held_key) return false;
    const u32 held = *held_key;
    if (event.kind == UiInputKind::key_up && event.code == held) { *held_key = 0; return true; }
    if (event.kind == UiInputKind::key_down && event.code == held && !event.repeated) { *held_key = 0; return false; }
    return (event.kind == UiInputKind::key_down && event.code == held) ||
        (event.kind == UiInputKind::character && (event.code == held || (held == '\r' && event.code == '\n')));
}

UiInputRouteResult ui_input_route(ConstSlice<UiInputLayer> layers, const UiInputEvent& event) noexcept {
    const auto mask = ui_input_mask(event.kind);
    for (u32 i = 0; i < layers.count; ++i) {
        const UiInputLayer layer = layers[i];
        const auto result = layer.handler ? layer.handler(layer.context, event) : UiInputDisposition::ignored;
        if (result != UiInputDisposition::ignored)
            return {result, layer.id};
        if (layer.barrier & mask)
            return {UiInputDisposition::blocked, layer.id};
    }
    return {UiInputDisposition::ignored, 0};
}

} // namespace maicord

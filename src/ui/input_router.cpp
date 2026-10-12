#include "maicord/ui/input_router.hpp"

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
    if (!mask || layers.count > kUiInputLayerLimit || (!layers.data && layers.count))
        return {UiInputDisposition::failed, 0};
    // Reject malformed routing frames before any callback has side effects.
    for (u32 i = 0; i < layers.count; ++i) {
        if ((layers[i].barrier & ~kUiInputAll) || !layers[i].id)
            return {UiInputDisposition::failed, layers[i].id};
    }
    for (u32 i = 0; i < layers.count; ++i) {
        const UiInputLayer layer = layers[i];
        const auto result = layer.handler ? layer.handler(layer.context, event) : UiInputDisposition::ignored;
        switch (result) {
        case UiInputDisposition::ignored:
            if (layer.barrier & mask) return {UiInputDisposition::blocked, layer.id};
            break;
        case UiInputDisposition::handled:
        case UiInputDisposition::blocked:
        case UiInputDisposition::failed:
            return {result, layer.id};
        default:
            return {UiInputDisposition::failed, layer.id};
        }
    }
    return {UiInputDisposition::ignored, 0};
}

} // namespace maicord

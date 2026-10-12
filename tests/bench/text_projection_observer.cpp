#include "text_projection_observer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::text_projection_bench {
namespace {
bool equal(const UiTextCommand& a, const UiTextCommand& b) noexcept {
    return utf8_equal(a.text, b.text) && a.owner.manager == b.owner.manager &&
        a.owner.handle.slot == b.owner.handle.slot && a.owner.handle.generation == b.owner.handle.generation &&
        ui_rect_equal(a.rectangle, b.rectangle) && ui_rect_equal(a.clip, b.clip) &&
        bytes_equal(&a.color, &b.color, sizeof(Color)) && a.style_flags == b.style_flags &&
        a.horizontal_alignment == b.horizontal_alignment && a.vertical_alignment == b.vertical_alignment &&
        a.wrapping == b.wrapping && a.overflow == b.overflow && a.face == b.face &&
        a.effect.kind == b.effect.kind && a.effect.count == b.effect.count &&
        a.effect.animate == b.effect.animate && a.effect.started_ms == b.effect.started_ms &&
        bytes_equal(a.effect.colors, b.effect.colors, sizeof(a.effect.colors)) &&
        a.styles.count == b.styles.count &&
        bytes_equal(a.styles.data, b.styles.data, a.styles.count * sizeof(TextStyleSpan));
}
}
__declspec(noinline) bool consume(void* context, const UiTextCommand& command, u32 ordinal) noexcept {
    auto& observation = *static_cast<Observation*>(context);
    if (ordinal != observation.count || (observation.expected &&
        (ordinal >= observation.expected->count || !equal(command, observation.expected->storage[ordinal])))) return false;
    // Intentionally cheap, identical synchronous work for both timed destinations.
    // Full production signature and exact setup comparison validate the payload.
    const u64 edge = command.text.bytes ? static_cast<u8>(command.text.data[0]) |
        (static_cast<u64>(static_cast<u8>(command.text.data[command.text.bytes - 1])) << 8) : 0;
    observation.hash = (observation.hash ^ ordinal ^ command.text.bytes ^
        (static_cast<u64>(command.style_flags) << 32) ^ edge) * 1099511628211ull;
    ++observation.count;
    return true;
}
__declspec(noinline) bool observe_stored(const UiTextCommandStream& stream, Observation* observation) noexcept {
    if (!observation) return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (!consume(observation, stream.storage[i], i)) return false;
    return true;
}
__declspec(noinline) u64 observe_preflight(const UiTextCommandStream* stream, const UiTextEmitter* emitter) noexcept {
    // Passing the complete stream across a non-LTCG boundary makes its command
    // storage externally observable; this phase intentionally has no glyph work.
    if ((stream == nullptr) == (emitter == nullptr)) return 0;
    return stream ? stream->signature ^ stream->count : emitter->signature ^ emitter->count;
}
}

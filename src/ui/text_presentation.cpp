#include "maicord/ui/text_presentation.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/text_style.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

// Word at a time: signatures are only compared within a session, so what
// matters is that every byte reaches them (the shift carries high bits down).
__forceinline u64 append_bytes(u64 signature, const void* source, u32 bytes) noexcept {
    const auto* input = static_cast<const u8*>(source);
    const u64 count = bytes;
    for (; bytes >= 8; bytes -= 8, input += 8) {
        u64 word;
        CopyMemory(&word, input, sizeof(word));
        signature = (signature ^ word) * 0x9E3779B97F4A7C15ull;
        signature ^= signature >> 29;
    }
    u64 tail = count << 56;
    for (u32 i = 0; i < bytes; ++i)
        tail ^= static_cast<u64>(input[i]) << (i * 8);
    signature = (signature ^ tail) * 0x9E3779B97F4A7C15ull;
    return signature ^ (signature >> 29);
}

u64 append_signature(u64 signature, const UiTextCommand& command, u32 index) noexcept {
    signature = hash64_mix(signature, index);
    signature = append_bytes(signature, command.text.data, command.text.bytes);
    signature = hash64_mix(signature, static_cast<u64>(reinterpret_cast<ULONG_PTR>(command.owner.manager)));
    signature = hash64_mix(signature, command.owner.handle.slot);
    signature = hash64_mix(signature, command.owner.handle.generation);
    signature = append_bytes(signature, &command.rectangle, sizeof(command.rectangle));
    signature = append_bytes(signature, &command.clip, sizeof(command.clip));
    signature = append_bytes(signature, &command.color, sizeof(command.color));
    signature = hash64_mix(signature, command.style_flags);
    signature = append_bytes(signature, command.styles.data, command.styles.count * sizeof(TextStyleSpan));
    signature = hash64_mix(signature, static_cast<u8>(command.face));
    for (u32 i = 0; i < 8; ++i)
        signature = hash64_mix(signature, command.effect.colors[i]);
    signature = hash64_mix(signature, command.effect.kind);
    signature = hash64_mix(signature, command.effect.count);
    signature = hash64_mix(signature, command.effect.animate);
    signature = hash64_mix(signature, command.effect.started_ms);
    signature = hash64_mix(signature, static_cast<u8>(command.horizontal_alignment));
    signature = hash64_mix(signature, static_cast<u8>(command.vertical_alignment));
    signature = hash64_mix(signature, static_cast<u8>(command.wrapping));
    return hash64_mix(signature, static_cast<u8>(command.overflow));
}

bool command_valid(const UiTextCommand& command) noexcept {
    return (!command.styles.count || command.styles.data) &&
        (!command.text.bytes || command.text.data) && command.owner.manager &&
        ui_widget_manager_resolve(*command.owner.manager, command.owner.handle) &&
        ui_rect_valid_or_empty(command.rectangle) && ui_rect_valid_or_empty(command.clip) &&
        command.horizontal_alignment <= UiTextAlignment::end &&
        command.vertical_alignment <= UiTextAlignment::end &&
        command.wrapping <= UiTextWrapping::no_wrap && command.overflow <= UiTextOverflow::ellipsis;
}
bool command_visible(const UiTextCommand& command) noexcept {
    return command.text.bytes && ui_rect_has_area(command.rectangle) && ui_rect_has_area(command.clip);
}

} // namespace

bool ui_text_command_stream_initialize(UiTextCommandStream* stream, Slice<UiTextCommand> storage,
                                       Slice<char> owned_text, Arena* owned_arena) noexcept {
    if (!stream || !storage.data || storage.count == 0 || (!owned_text.data && owned_text.count) ||
        (owned_arena && (!owned_arena->base || owned_text.data))) {
        return false;
    }
    stream->storage = storage;
    stream->owned_text = owned_text;
    stream->owned_bytes = 0;
    stream->owned_arena = owned_arena;
    stream->owned_mark = owned_arena ? arena_mark(owned_arena) : ArenaMark{};
    stream->signature = kHash64Seed;
    stream->count = 0;
    stream->rejected = 0;
    return true;
}

void ui_text_command_stream_reset(UiTextCommandStream* stream) noexcept {
    if (!stream) {
        return;
    }
    stream->signature = kHash64Seed;
    stream->owned_bytes = 0;
    if (stream->owned_arena)
        arena_rewind(stream->owned_arena, stream->owned_mark);
    stream->count = 0;
    stream->rejected = 0;
}

bool ui_text_command_stream_push_owned(UiTextCommandStream* stream, const UiTextCommand& command) noexcept {
    if (!command.text.bytes || !ui_rect_has_area(command.rectangle) || !ui_rect_has_area(command.clip))
        return ui_text_command_stream_push(stream, command);
    if (!stream || !command.text.data ||
        (!stream->owned_arena &&
         (!stream->owned_text.data || stream->owned_bytes > stream->owned_text.count ||
          command.text.bytes > stream->owned_text.count - stream->owned_bytes))) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    UiTextCommand retained = command;
    const auto mark = stream->owned_arena ? arena_mark(stream->owned_arena) : ArenaMark{};
    char* destination = stream->owned_arena ? arena_allocate<char>(stream->owned_arena, command.text.bytes)
                                            : stream->owned_text.data + stream->owned_bytes;
    if (!destination) {
        saturating_increment(stream->rejected);
        return false;
    }
    copy_bytes(destination, command.text.data, command.text.bytes);
    retained.text.data = destination;
    if (!ui_text_command_stream_push(stream, retained)) {
        if (stream->owned_arena)
            arena_rewind(stream->owned_arena, mark);
        return false;
    }
    stream->owned_bytes += command.text.bytes;
    return true;
}

bool ui_text_command_stream_push(UiTextCommandStream* stream, const UiTextCommand& command) noexcept {
    if (!stream || !stream->storage.data || stream->count > stream->storage.count ||
        !command_valid(command)) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    // Virtualized lists retain overscan rows whose text rectangle or inherited
    // clip can collapse to zero area at a viewport boundary. That is a valid
    // invisible command; one overscan label must not reject the whole stream.
    if (!command_visible(command)) {
        return true;
    }
    if (stream->count == stream->storage.count) {
        saturating_increment(stream->rejected);
        return false;
    }
    stream->signature = append_signature(stream->signature, command, stream->count);
    stream->storage[stream->count++] = command;
    return true;
}

bool ui_text_emitter_initialize(UiTextEmitter* emitter, UiTextConsumeFunction consume,
    void* context, u32 limit) noexcept {
    if (!emitter || !limit || limit > kUiTextEmitterMaximumCommands) return false;
    *emitter = {consume, context, kHash64Seed, 0, 0, limit};
    return true;
}
bool ui_text_output_valid(const UiTextOutput* output) noexcept {
    if (!output || (output->stream && output->emitter)) return false;
    if (output->stream) return output->stream->storage.data &&
        output->stream->count <= output->stream->storage.count;
    return output->emitter && output->emitter->limit &&
        output->emitter->limit <= kUiTextEmitterMaximumCommands &&
        output->emitter->count <= output->emitter->limit;
}
void ui_text_output_reset(UiTextOutput* output) noexcept {
    if (!output) return;
    if (output->stream) ui_text_command_stream_reset(output->stream);
    if (output->emitter) {
        output->emitter->signature = kHash64Seed;
        output->emitter->count = output->emitter->rejected = 0;
    }
}
void ui_text_output_reject(UiTextOutput* output) noexcept {
    if (!output) return;
    if (output->stream) { output->stream->count = 0; saturating_increment(output->stream->rejected); }
    if (output->emitter) { output->emitter->count = 0; saturating_increment(output->emitter->rejected); }
}
bool ui_text_output_push(UiTextOutput* output, const UiTextCommand& command) noexcept {
    if (!ui_text_output_valid(output)) return false;
    if (output->stream) return ui_text_command_stream_push(output->stream, command);
    auto& emitter = *output->emitter;
    if (!command_valid(command)) { saturating_increment(emitter.rejected); return false; }
    if (!command_visible(command)) return true;
    if (emitter.count == emitter.limit) { saturating_increment(emitter.rejected); return false; }
    // Hash before consuming: callbacks may generate glyphs, never retain or
    // modify a source command. Commit the ordinal only after consumption.
    const u64 signature = append_signature(emitter.signature, command, emitter.count);
    if (emitter.consume && !emitter.consume(emitter.context, command, emitter.count)) {
        saturating_increment(emitter.rejected); return false;
    }
    emitter.signature = signature;
    ++emitter.count;
    return true;
}
bool ui_text_output_push_owned(UiTextOutput* output, const UiTextCommand& command) noexcept {
    if (!ui_text_output_valid(output)) return false;
    return output->stream ? ui_text_command_stream_push_owned(output->stream, command) :
        ui_text_output_push(output, command);
}

} // namespace maicord

#include "maicord/ui/text/presentation.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/text/style.hpp"
#include <stddef.h>

namespace maicord {
namespace {

// The client compares the signature only in memory. Adjacent fields with no padding between them hash as one block.
// A block costs one multiply for each eight bytes. A field of its own costs one multiply or more.
u64 append_signature(u64 signature, const UiTextCommand& command, u32 index) noexcept {
    using C = UiTextCommand;
    static_assert(offsetof(C, clip) == offsetof(C, rectangle) + sizeof(UiRect) &&
                  offsetof(C, color) == offsetof(C, clip) + sizeof(UiRect) &&
                  offsetof(C, style_flags) == offsetof(C, color) + sizeof(Color) &&
                  offsetof(C, horizontal_alignment) == offsetof(C, style_flags) + 4 &&
                  offsetof(C, vertical_alignment) == offsetof(C, horizontal_alignment) + 1 &&
                  offsetof(C, wrapping) == offsetof(C, horizontal_alignment) + 2 &&
                  offsetof(C, overflow) == offsetof(C, horizontal_alignment) + 3 &&
                  offsetof(C, face) == offsetof(C, horizontal_alignment) + 4);
    static_assert(offsetof(UiTextEffect, kind) == sizeof(UiTextEffect::colors) &&
                  offsetof(UiTextEffect, count) == offsetof(UiTextEffect, kind) + 1 &&
                  offsetof(UiTextEffect, animate) == offsetof(UiTextEffect, kind) + 2 &&
                  offsetof(UiTextEffect, themed_styles) == offsetof(UiTextEffect, kind) + 3);
    constexpr u32 placement_bytes = offsetof(C, face) + 1 - offsetof(C, rectangle);
    constexpr u32 effect_bytes = offsetof(UiTextEffect, themed_styles) + 1;
    const UiWidgetHandle handle = command.owner.handle;
    signature = hash64_mix(signature, index);
    signature = hash64_words_inline(signature, command.text.data, command.text.bytes);
    signature = hash64_mix(signature, static_cast<u64>(reinterpret_cast<usize>(command.owner.manager)));
    signature = hash64_mix(signature, handle.slot | static_cast<u32>(handle.generation) << 16);
    // Rectangle, clip, colour, style flags, alignments, wrapping, overflow and face.
    signature = hash64_words_inline(signature, &command.rectangle, placement_bytes);
    signature = hash64_words_inline(signature, command.styles.data, command.styles.count * sizeof(TextStyleSpan));
    // Effect colours, kind, count, animation and the themed-styles flag.
    signature = hash64_words_inline(signature, command.effect.colors, effect_bytes);
    return hash64_mix(signature, command.effect.started_ms);
}

// The first check `command` fails, none when it is valid.
UiTextRejectCheck command_check(const UiTextCommand& command) noexcept {
    if (!command.owner.manager || !ui_widget_manager_resolve(*command.owner.manager, command.owner.handle))
        return UiTextRejectCheck::owner;
    if (!ui_rect_valid_or_empty(command.rectangle))
        return UiTextRejectCheck::rectangle;
    if (!ui_rect_valid_or_empty(command.clip))
        return UiTextRejectCheck::clip;
    return (!command.styles.count || command.styles.data) && (!command.text.bytes || command.text.data) &&
                   command.horizontal_alignment <= UiTextAlignment::end &&
                   command.vertical_alignment <= UiTextAlignment::end &&
                   command.wrapping <= TextWrapping::no_wrap && command.overflow <= TextOverflow::ellipsis
               ? UiTextRejectCheck::none
               : UiTextRejectCheck::fields;
}
// Counts a rejection and keeps what failed for diagnostics.
__declspec(noinline) bool reject(UiTextEmitter* emitter, const UiTextCommand& command,
                                 UiTextRejectCheck check) noexcept {
    saturating_increment(emitter->rejected);
    emitter->reject_check = check;
    emitter->reject_owner = command.owner;
    const UiRect& r = check == UiTextRejectCheck::clip ? command.clip : command.rectangle;
    emitter->reject_values[0] = check == UiTextRejectCheck::owner   ? command.owner.handle.slot
                                : check == UiTextRejectCheck::limit ? emitter->count
                                                                    : r.right >= r.left;
    emitter->reject_values[1] = check == UiTextRejectCheck::owner   ? command.owner.handle.generation
                                : check == UiTextRejectCheck::limit ? emitter->limit
                                                                    : r.bottom >= r.top;
    return false;
}
bool command_visible(const UiTextCommand& command) noexcept {
    return command.text.bytes &&
        minimum(command.rectangle.right, command.clip.right) > maximum(command.rectangle.left, command.clip.left) &&
        minimum(command.rectangle.bottom, command.clip.bottom) > maximum(command.rectangle.top, command.clip.top);
}

} // namespace

bool ui_text_command_stream_initialize(UiTextCommandStream* stream, const Slice<UiTextCommand>& storage,
                                       const Slice<char>& owned_text, Arena* owned_arena) noexcept {
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
    if (!command_visible(command))
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
        command_check(command) != UiTextRejectCheck::none) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    // Overscan labels outside their clip are valid but consume no storage,
    // signature work or renderer reservation.
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
    if (const auto check = command_check(command); check != UiTextRejectCheck::none)
        return reject(&emitter, command, check);
    if (!command_visible(command)) return true;
    if (emitter.count == emitter.limit) return reject(&emitter, command, UiTextRejectCheck::limit);
    // Hash before consuming: callbacks may generate glyphs, never retain or
    // modify a source command. Commit the ordinal only after consumption.
    const u64 signature = append_signature(emitter.signature, command, emitter.count);
    if (emitter.consume && !emitter.consume(emitter.context, command, emitter.count))
        return reject(&emitter, command, UiTextRejectCheck::consumer);
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

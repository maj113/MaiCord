#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/core/widget_manager.hpp"

namespace maicord {
enum class UiTextAlignment : u8 {
    start,
    center,
    end,
};

struct UiTextEffect {
    u32 colors[8];
    u8 kind, count;
    bool animate;
    // Not an effect: the command's spans carry theme-coloured styles (links, code plates, syntax) and the renderer
    // resolves them with the theme. It rides in the padding so UiTextCommand keeps its size.
    bool themed_styles;
    u64 started_ms;
};

struct UiTextCommand {
    Utf8View text;
    UiWidgetReference owner;
    UiRect rectangle;
    UiRect clip;
    Color color;
    u32 style_flags;
    UiTextAlignment horizontal_alignment;
    UiTextAlignment vertical_alignment;
    TextWrapping wrapping;
    TextOverflow overflow;
    UiTextFace face;
    UiTextEffect effect;
    ConstSlice<TextStyleSpan> styles;
};

struct UiTextCommandStream {
    Slice<UiTextCommand> storage;
    u64 signature;
    u32 count;
    u32 rejected;
    Slice<char> owned_text;
    u32 owned_bytes;
    Arena* owned_arena;
    ArenaMark owned_mark;
};

bool ui_text_command_stream_initialize(UiTextCommandStream* stream, const Slice<UiTextCommand>& storage,
                                       const Slice<char>& owned_text = {}, Arena* owned_arena = nullptr) noexcept;
void ui_text_command_stream_reset(UiTextCommandStream* stream) noexcept;
// Normal push borrows text. Owned push copies into the optional caller byte bank
// or arena; reset invalidates those copies and rewinds an arena to its entry mark.
bool ui_text_command_stream_push(UiTextCommandStream* stream, const UiTextCommand& command) noexcept;
bool ui_text_command_stream_push_owned(UiTextCommandStream* stream, const UiTextCommand& command) noexcept;

// Synchronous visitor: the command, text, styles and effect are borrowed only
// until consume returns. A null consumer validates/hashes without rendering.
using UiTextConsumeFunction = bool (*)(void*, const UiTextCommand&, u32 ordinal) noexcept;
inline constexpr u32 kUiTextEmitterMaximumCommands = 4096;
// The check a rejected command failed (diagnostics only).
enum class UiTextRejectCheck : u8 { none, owner, rectangle, clip, fields, limit, consumer };
struct UiTextEmitter {
    UiTextConsumeFunction consume;
    void* context;
    u64 signature;
    u32 count;
    u32 rejected;
    u32 limit;
    // The last rejection: its check, the command's owner and the values compared (the owner's slot and
    // generation, a rectangle's ordered axes, or count and limit).
    UiTextRejectCheck reject_check;
    UiWidgetReference reject_owner;
    u32 reject_values[2];
};
struct UiTextOutput {
    UiTextCommandStream* stream;
    UiTextEmitter* emitter;
};
bool ui_text_emitter_initialize(UiTextEmitter*, UiTextConsumeFunction consume = nullptr,
    void* context = nullptr, u32 limit = kUiTextEmitterMaximumCommands) noexcept;
bool ui_text_output_valid(const UiTextOutput*) noexcept;
void ui_text_output_reset(UiTextOutput*) noexcept;
void ui_text_output_reject(UiTextOutput*) noexcept;
bool ui_text_output_push(UiTextOutput*, const UiTextCommand&) noexcept;
bool ui_text_output_push_owned(UiTextOutput*, const UiTextCommand&) noexcept;
// Signature of the text emit(scene, output) publishes now, validated without
// rendering; 0 when it fails.
template <typename Scene, bool (*Emit)(const Scene&, UiTextOutput*) noexcept>
inline u64 ui_text_output_signature(const Scene& scene) noexcept {
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    return ui_text_emitter_initialize(&emitter) && Emit(scene, &output) ? emitter.signature : 0;
}

} // namespace maicord

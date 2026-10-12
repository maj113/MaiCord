#pragma once

#include "maicord/ui/message_text_presentation.hpp"

namespace maicord {

// A visible-range change can require one pass to measure, one to update the
// variable-height list, one to rematerialize the overscanned range, and one to
// seed measurements for newly materialized commands.  Rich/reply rows can add
// another round, so keep a small bounded margin rather than terminating a
// healthy account session at the former four-pass boundary.
inline constexpr u32 kMaximumMessageTextConvergencePasses = 8;

using MessageTextFinalize = bool (*)(void* context, const Theme& theme) noexcept;

enum MessageTextCoordinatorPassFlag : u8 {
    message_text_coordinator_scene_changed = 1u << 0,
    message_text_coordinator_command_changed = 1u << 1,
    message_text_coordinator_layout_changed = 1u << 2,
    message_text_coordinator_scene_dirty = 1u << 3,
    message_text_coordinator_projection_fixed = 1u << 4,
};

struct MessageTextCoordinatorPass {
    u64 scroll_units;
    u64 total_units;
    u32 first;
    u32 count;
    u32 command_count;
    u32 measurement_count;
    u32 leading_item_units[4];
    u8 flags;
};

struct MessageTextCoordinatorTrace {
    MessageTextCoordinatorPass passes[kMaximumMessageTextConvergencePasses];
    u8 count;
};

enum class MessageTextCoordinatorError : u8 {
    none,
    invalid_input,
    scene,
    projection,
    measurement,
    application,
    overflow,
    non_convergent,
    finalization,
};

struct MessageTextCoordinatorRequest {
    MessageScene* scene;
    MessageTextPresentation* presentation;
    UiTextMeasureCache* measure_cache;
    const ChatStateStore* state;
    const UserStore* users;
    const NavigationView* navigation;
    const Theme* theme;
    MessageTextFinalize finalize;
    void* finalize_context;
    u64 now_unix_seconds;
    u16 backend_revision;
    MessageTextCoordinatorTrace* trace;
};

struct MessageTextCoordinatorResult {
    MessageTextCoordinatorError error;
    MessageSceneError scene_error;
    MessageTextPresentationError presentation_error;
    u8 passes;
    bool finalized;

    constexpr bool ok() const noexcept {
        return error == MessageTextCoordinatorError::none;
    }
};

MessageTextCoordinatorResult message_text_coordinator_converge(
    const MessageTextCoordinatorRequest& request) noexcept;

static_assert(sizeof(MessageTextCoordinatorResult) == 5);
static_assert(sizeof(MessageTextCoordinatorPass) == 56);
static_assert(sizeof(MessageTextCoordinatorTrace) == 456);

} // namespace maicord

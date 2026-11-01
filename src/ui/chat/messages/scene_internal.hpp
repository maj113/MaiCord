#pragma once

#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/theme/styles.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::message_scene_detail {

const UiResolvedThemeStyles* theme_bank(const MessageScene* scene) noexcept;
const UiResolvedThemeStyles* theme_bank(const MessageScene& scene) noexcept;
const MessageSceneDisplay& display(const MessageScene* scene) noexcept;
const MessageSceneDisplay& display(const MessageScene& scene) noexcept;
__forceinline u64 hash_mix(u64 hash, u64 value) noexcept {
    return hash64_mix(hash, value);
}
bool plugin_folded(const MessageScene& scene, const MessageRecord& record) noexcept;
const MessageRichRecord* row_rich(const MessageScene& scene, const MessageRecord& record) noexcept;
const MessagePollRecord* row_poll(const MessageScene& scene, const MessageRecord& record) noexcept;
bool poll_state(const MessageScene& scene, const MessageRecord& record, MessageHandle handle,
                Slice<UiPollAnswer> answers, UiPollState* out) noexcept;
float content_trailing(bool accessories) noexcept;
float intro_row_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept;
bool set_resident_height(MessageScene* scene, u32 index, u32 units) noexcept;
float row_required_height(const MessageScene& scene, const MessageSceneRowWidget& row,
                          const MessageRichRecord* rich) noexcept;
const MessageRichRecord* snapshot_rich(const MessageScene& scene, u64 message_id) noexcept;
bool prepare_resident_measurements(MessageScene* scene, UiRect viewport) noexcept;
bool apply_row_specs(MessageScene* scene, MessageSceneRowWidget* row) noexcept;
void clear_materialization(MessageScene* scene) noexcept;
u64 focused_message(const MessageScene& scene) noexcept;
bool materialization_matches(const MessageScene& scene, u32 first, u32 count) noexcept;
bool refresh_row(MessageScene* scene, MessageSceneRowWidget* row, u32 source_index,
                 bool preserve_measurement, bool* data_changed) noexcept;
void rebase_retained_rows(MessageScene* scene, u32 first, u32 count) noexcept;
bool build_row(MessageScene* scene, u32 local, u32 source_index) noexcept;
template <typename Scene> const UiThemeMetrics& metrics(const Scene& scene) noexcept {
    return ui_theme_metrics(theme_bank(scene));
}
template <typename Scene> float message_em(const Scene& scene) noexcept {
    const auto* bank = theme_bank(scene);
    return bank && bank->ready ? bank->typography.body_size : 16.f;
}
template <typename Scene> float content_height(const Scene& scene) noexcept {
    return message_em(scene) * 1.3125f;
}
template <typename Scene> bool compact(const Scene& scene) noexcept {
    return display(scene).compact;
}
// Compact: the avatar column becomes the timestamp column (width by clock
// shape); the name starts one gap after it. Every compact row, grouped or
// not, owns that column so the hidden timestamp of a grouped row keeps the
// name aligned.
template <typename Scene> float avatar_width(const Scene& scene) noexcept {
    return compact(scene) ? message_em(scene) * message_scene_compact_clock_em(display(scene).clock)
                          : metrics(scene).message_avatar;
}
template <typename Scene> float avatar_gap(const Scene& scene) noexcept {
    return compact(scene) ? message_em(scene) * kMessageSceneCompactGapEm : metrics(scene).message_avatar_gap;
}
template <typename Scene> float reply_height(const Scene& scene) noexcept {
    return message_em(scene) * 1.375f;
}
template <typename Scene> float available_text_width(const Scene& scene, const UiRect& viewport) noexcept {
    return maximum(0.0f, ui_rect_width(viewport) - metrics(scene).message_left - avatar_width(scene) -
                             avatar_gap(scene) - metrics(scene).message_right);
}

template <typename Scene>
float snapshot_following_gap(const Scene& scene, const MessageRichRecord* outer) noexcept {
    // Rich-media gap between a snapshot and the outer message's own rich content.
    return outer && (outer->attachment_count || outer->embed_count || outer->sticker_count ||
                     outer->component_count || outer->reaction_count || outer->truncations)
               ? metrics(scene).message_rich_gap
               : 0.f;
}

} // namespace maicord::message_scene_detail

#include "maicord/ui/message_scene.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_rich_widgets.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "message_scene_measurements.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/model/invite_card_cache.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/theme_styles.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kMessageRootWidgetId = 1;
const UiResolvedThemeStyles* theme_bank(const MessageScene* scene) noexcept {
    return scene->theme_styles;
}
const UiResolvedThemeStyles* theme_bank(const MessageScene& scene) noexcept {
    return scene.theme_styles;
}
template <typename Scene> const UiThemeMetrics& metrics(const Scene& scene) noexcept {
    return ui_theme_metrics(theme_bank(scene));
}
template <typename Scene> float message_em(const Scene& scene) noexcept {
    const auto* bank = theme_bank(scene);
    return bank && bank->ready ? bank->typography.body_size : 16.f;
}
template <typename Scene> float author_height(const Scene& scene) noexcept {
    return message_em(scene) * 1.3125f;
}
template <typename Scene> float content_height(const Scene& scene) noexcept {
    return message_em(scene) * 1.3125f;
}
const MessageSceneDisplay& display(const MessageScene* scene) noexcept {
    return scene->display;
}
const MessageSceneDisplay& display(const MessageScene& scene) noexcept {
    return scene.display;
}
template <typename Scene> bool compact(const Scene& scene) noexcept {
    return display(scene).compact;
}
// Compact: the avatar column becomes the timestamp gutter so the content
// starts one hanging indent from the left padding of the row.
template <typename Scene> float avatar_width(const Scene& scene) noexcept {
    return compact(scene) ? maximum(0.0f, message_em(scene) * kMessageSceneCompactIndentEm -
                                              metrics(scene).message_avatar_gap)
                          : metrics(scene).message_avatar;
}
template <typename Scene> float avatar_height(const Scene& scene, bool grouped) noexcept {
    return grouped ? 0.0f : compact(scene) ? content_height(scene) : metrics(scene).message_avatar;
}
template <typename Scene> float group_top(const Scene& scene) noexcept {
    constexpr float spacing[]{0.0f, 0.0f, 4.0f, 8.0f, 16.0f, 24.0f};
    const u8 code = display(scene).group_spacing;
    if (code && code < 6)
        return spacing[code] + 1.0f;
    return compact(scene) ? 1.0f : metrics(scene).message_top;
}
template <typename Scene> float reply_height(const Scene& scene) noexcept {
    return message_em(scene) * 1.375f;
}
template <typename Scene> float snapshot_header(const Scene& scene) noexcept {
    return message_em(scene) * 1.5f;
}
template <typename Scene> float snapshot_padding(const Scene& scene) noexcept {
    return metrics(scene).message_rich_gap + 2.f;
}
template <typename Scene> float grouped_minimum(const Scene& scene) noexcept {
    return message_em(scene) * 1.375f;
}
template <typename Scene> float ungrouped_minimum(const Scene& scene) noexcept {
    if (compact(scene))
        return group_top(scene) + content_height(scene) + metrics(scene).message_bottom;
    return group_top(scene) +
           maximum(author_height(scene) + content_height(scene), metrics(scene).message_avatar) +
           metrics(scene).message_bottom;
}
MessageSceneResult scene_error(MessageScene* scene, MessageSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return MessageSceneResult{error, scene ? scene->first : 0, scene ? scene->count : 0, false};
}

template <typename Scene> float row_top_padding(const Scene& scene, bool grouped) noexcept {
    return grouped ? metrics(scene).message_grouped_top : group_top(scene);
}

template <typename Scene> float row_bottom_padding(const Scene& scene, bool grouped) noexcept {
    return grouped ? metrics(scene).message_grouped_bottom : metrics(scene).message_bottom;
}

template <typename Scene> float row_author_height(const Scene& scene, bool grouped) noexcept {
    return grouped || compact(scene) ? 0.0f : author_height(scene);
}

template <typename Scene>
UiLayoutStyle row_style(const Scene& scene, float height, bool grouped, float leading = 0) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::horizontal;
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.padding = UiInsets{metrics(scene).message_left, row_top_padding(scene, grouped) + leading,
                             metrics(scene).message_right, row_bottom_padding(scene, grouped)};
    style.gap = metrics(scene).message_avatar_gap;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

template <typename Scene>
UiLayoutStyle avatar_style(const Scene& scene, bool grouped, float reply_height) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(avatar_width(scene));
    style.height = ui_layout_fixed(avatar_height(scene, grouped));
    // The large avatar belongs to the current author, so it starts below the
    // reply preview that precedes the header.
    style.margin.top = grouped ? 0.0f : reply_height + 0.f;
    if (grouped) {
        style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size);
    }
    return style;
}

UiLayoutStyle body_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle region_style(float height, bool text) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size |
                                   (text ? ui_layout_node_text_content : 0));
    return style;
}

u64 hash_mix(u64 hash, u64 value) noexcept {
    return hash64_mix(hash, value);
}

u64 hash_mix_string(u64 hash, MessageRichString value) noexcept {
    hash = hash_mix(hash, value.offset);
    return hash_mix(hash, value.bytes);
}

u64 rich_layout_signature(const MessageRichRecord* rich) noexcept {
    if (!rich) {
        return 0;
    }
    u64 hash = kHash64Seed;
    hash = hash_mix(hash, rich->message_revision);
    hash = hash_mix(hash, rich->has_forwarded_snapshot);
    hash = hash_mix(hash, rich->forwarded_snapshot_child);
    if (rich->forwarded_snapshot_child) {
        hash = hash_mix_string(hash, rich->snapshot.content);
        hash = hash_mix(hash, rich->snapshot.flags);
        hash = hash_mix(hash, rich->snapshot.timestamp_ms);
        hash = hash_mix(hash, rich->snapshot.edited_timestamp_ms);
    }
    hash = hash_mix(hash, static_cast<u64>(rich->reference.state));
    hash = hash_mix(hash, rich->interaction.id);
    hash = hash_mix_string(hash, rich->interaction.user_name);
    hash = hash_mix_string(hash, rich->interaction.command_name);
    hash = hash_mix(hash, rich->reaction_count);
    hash = hash_mix(hash, rich->attachment_count);
    hash = hash_mix(hash, rich->embed_count);
    hash = hash_mix(hash, rich->sticker_count);
    hash = hash_mix(hash, rich->component_count);
    hash = hash_mix(hash, rich->truncations);
    hash = hash_mix(hash, rich->thread.id);
    hash = hash_mix(hash, (static_cast<u64>(rich->thread.message_count) << 32) | rich->thread.member_count);
    hash = hash_mix(hash, (rich->thread.archived ? 1u : 0u) | (rich->thread.locked ? 2u : 0u));
    hash = hash_mix_string(hash, rich->thread.name);
    for (u32 index = 0; index < rich->reaction_count; ++index) {
        const MessageRichReaction& value = rich->reactions[index];
        hash = hash_mix(hash, value.emoji_id);
        hash = hash_mix(hash, value.count);
        hash = hash_mix(hash, value.flags);
        hash = hash_mix_string(hash, value.name);
    }
    for (u32 index = 0; index < rich->attachment_count; ++index) {
        const MessageRichAttachment& value = rich->attachments[index];
        hash = hash_mix(hash, value.id);
        hash = hash_mix(hash, value.width);
        hash = hash_mix(hash, value.height);
        hash = hash_mix(hash, value.state);
        hash = hash_mix_string(hash, value.filename);
        hash = hash_mix_string(hash, value.title);
        hash = hash_mix_string(hash, value.description);
        hash = hash_mix_string(hash, value.content_type);
        hash = hash_mix_string(hash, value.url);
    }
    for (u32 index = 0; index < rich->embed_count; ++index) {
        const MessageRichEmbed& value = rich->embeds[index];
        hash = hash_mix_string(hash, value.author_name);
        hash = hash_mix_string(hash, value.title);
        hash = hash_mix_string(hash, value.description);
        hash = hash_mix_string(hash, value.url);
        hash = hash_mix(hash, value.field_count);
        hash = hash_mix_string(hash, value.footer_text);
        hash = hash_mix_string(hash, value.author_url);
        hash = hash_mix_string(hash, value.author_icon);
        hash = hash_mix_string(hash, value.footer_icon);
        hash = hash_mix_string(hash, value.provider_name);
        hash = hash_mix_string(hash, value.provider_url);
        hash = hash_mix(hash, value.timestamp_ms);
        hash = hash_mix(hash, static_cast<u64>(value.type));
        const MessageRichEmbedMedia* media_items[] = {&value.image, &value.thumbnail, &value.video};
        for (const MessageRichEmbedMedia* media : media_items) {
            hash = hash_mix(hash, media->width);
            hash = hash_mix(hash, media->height);
            hash = hash_mix(hash, media->flags);
            hash = hash_mix_string(hash, media->url);
            hash = hash_mix_string(hash, media->proxy_url);
        }
    }
    for (u32 index = 0; index < rich->embed_field_count; ++index) {
        const MessageRichEmbedField& value = rich->embed_fields[index];
        hash = hash_mix_string(hash, value.name);
        hash = hash_mix_string(hash, value.value);
        hash = hash_mix(hash, value.embed_index);
        hash = hash_mix(hash, value.inline_field);
    }
    for (u32 index = 0; index < rich->sticker_count; ++index) {
        hash = hash_mix(hash, rich->stickers[index].id);
        hash = hash_mix_string(hash, rich->stickers[index].name);
        hash = hash_mix(hash, rich->stickers[index].format_type);
    }
    for (u32 index = 0; index < rich->component_count; ++index) {
        const MessageRichComponent& value = rich->components[index];
        hash = hash_mix_string(hash, value.label);
        hash = hash_mix_string(hash, value.content);
        hash = hash_mix_string(hash, value.custom_id);
        hash = hash_mix_string(hash, value.url);
        hash = hash_mix(hash, value.id);
        hash = hash_mix(hash, value.parent);
        hash = hash_mix(hash, value.order);
        hash = hash_mix(hash, value.type);
        hash = hash_mix(hash, value.style);
        hash = hash_mix(hash, value.disabled);
        hash = hash_mix_string(hash, value.emoji_name);
        hash = hash_mix(hash, value.emoji_id);
        hash = hash_mix(hash, (static_cast<u32>(value.flags) << 16) | (value.min_values << 8) | value.max_values);
    }
    hash = hash_mix(hash, rich->system.target_user_id);
    hash = hash_mix_string(hash, rich->system.target_name);
    hash = hash_mix(hash, rich->system.call_ended_ms);
    hash = hash_mix(hash, (rich->system.call_present ? 1u : 0u) | (rich->system.call_ended ? 2u : 0u) |
                              (rich->system.call_participated ? 4u : 0u));
    return hash;
}

constexpr float kPluginHiddenRowHeight = 1.0f / static_cast<float>(kUiExtentUnitsPerDip);
// Collapsed and hidden plugin verdicts drop the row's rich content, polls and
// invite cards; the decoration cache answers without running plugin code.
bool plugin_folded(const MessageScene& scene, const MessageRecord& record) noexcept {
    return scene.plugin_decorations && message_scene_plugin_visibility(scene, record) != ui_plugin_render_normal;
}

const MessageRichRecord* row_rich(const MessageScene& scene, const MessageRecord& record) noexcept {
    if (plugin_folded(scene, record))
        return nullptr;
    const MessageRichStore* store = scene.timeline->rich_store;
    const MessageRichRecord* rich = store ? message_rich_store_find(*store, record.id) : nullptr;
    return rich && rich->message_revision == record.revision ? rich : nullptr;
}

const MessagePollRecord* row_poll(const MessageScene& scene, const MessageRecord& record) noexcept {
    if (!scene.polls || !scene.polls->store ||
        record.content_availability != MessageContentAvailability::available ||
        record.delivery != MessageDeliveryState::authoritative || plugin_folded(scene, record))
        return nullptr;
    const auto* poll = message_poll_store_find(*scene.polls->store, record.id);
    return poll && poll->channel_id == record.channel_id ? poll : nullptr;
}

bool unavailable_poll(const MessageScene& scene, const MessageRecord& record) noexcept {
    return record.content_availability == MessageContentAvailability::available &&
           record.delivery == MessageDeliveryState::authoritative &&
           (record.presentation_flags & message_presentation_has_poll) && !plugin_folded(scene, record) &&
           !row_poll(scene, record);
}

bool poll_state(const MessageScene& scene, const MessageRecord& record, MessageHandle handle,
                Slice<UiPollAnswer> answers, UiPollState* out) noexcept {
    const auto* poll = row_poll(scene, record);
    if (!poll || !answers.data || answers.count < poll->answer_count ||
        poll->answer_count > kUiPollMaximumAnswers)
        return false;
    UiPollState state{};
    state.owner = record.id;
    state.generation = handle.generation;
    state.revision = hash64_mix(poll->content_revision, poll->result_revision);
    if (!state.revision)
        state.revision = 1;
    state.content_revision = poll->content_revision;
    state.result_revision = poll->result_revision;
    state.question = message_poll_store_string(*scene.polls->store, poll->question);
    state.now_ms = scene.polls->now_ms;
    state.expiry_ms = poll->expiry_ms;
    state.expiry_known = (poll->fields & gateway_message_poll_field_expiry) && !poll->expiry_null;
    state.multiselect = poll->allow_multiselect;
    state.results_known = poll->results_known;
    state.finalized = poll->is_finalized;
    state.finalized_known = poll->results_known;
    state.supported = poll->layout_type == 1;
    state.total_votes_known = poll->results_known;
    bool mine_known = true, mine = false;
    for (u32 i = 0; i < poll->answer_count; ++i) {
        const auto& source = poll->answers[i];
        answers[i] = {source.id,
                      message_poll_store_string(*scene.polls->store, source.text),
                      message_poll_store_string(*scene.polls->store, source.emoji_name),
                      source.emoji_id,
                      source.count,
                      source.emoji_animated,
                      source.count_known,
                      source.me_voted,
                      source.me_known,
                      source.me_known && source.me_voted};
        state.total_votes += source.count_known ? source.count : 0;
        state.total_votes_known = state.total_votes_known && source.count_known;
        mine_known = mine_known && source.me_known;
        mine = mine || (source.me_known && source.me_voted);
    }
    state.answers = {answers.data, poll->answer_count};
    state.results = state.finalized || mine;
    state.can_vote = scene.polls->can_vote && mine_known;
    state.can_remove_vote = state.can_vote && mine;
    state.can_view_results = true;
    state.can_view_voters = scene.polls->can_view_voters;
    if (scene.polls->project_state) {
        UiPollState selected{};
        if (scene.polls->project_state(scene.polls->context, record.id, answers, &selected)) {
            if (selected.owner != record.id || selected.content_revision != poll->content_revision ||
                selected.answers.data != answers.data || selected.answers.count != poll->answer_count)
                return false;
            for (u32 i = 0; i < poll->answer_count; ++i)
                if (selected.answers[i].id != poll->answers[i].id)
                    return false;
            state = selected;
        }
    }
    *out = state;
    return true;
}

bool poll_height(const MessageScene& scene, const MessageRecord& record, MessageHandle handle, float width,
                 float* out) noexcept {
    *out = 0;
    if (!row_poll(scene, record)) {
        *out = unavailable_poll(scene, record) ? 64.f : 0;
        return true;
    }
    UiPollAnswer answers[kUiPollMaximumAnswers]{};
    UiPollState state{};
    UiPollLayout layout{};
    if (!scene.polls->measure ||
        !poll_state(scene, record, handle, {answers, kUiPollMaximumAnswers}, &state) ||
        !ui_poll_measure_layout(state, width, ui_theme_poll_style(scene.theme_styles), scene.polls->measure,
                                scene.polls->backend_revision, &layout))
        return false;
    *out = layout.height + metrics(scene).message_rich_gap;
    return true;
}

bool local_message_date(u64 timestamp_ms, u32* date) noexcept {
    constexpr u64 windows_to_unix_seconds = 11'644'473'600ull;
    constexpr u64 file_time_ticks_per_second = 10'000'000ull;
    const u64 seconds = timestamp_ms / 1'000ull;
    if (seconds > ~0ull - windows_to_unix_seconds ||
        seconds + windows_to_unix_seconds > ~0ull / file_time_ticks_per_second) {
        return false;
    }
    const u64 ticks = (seconds + windows_to_unix_seconds) * file_time_ticks_per_second;
    const FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&file_time, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return false;
    }
    *date = static_cast<u32>(local.wYear) * 512u + static_cast<u32>(local.wMonth) * 32u + local.wDay;
    return true;
}

struct MessageGroupDate {
    u64 timestamp_ms;
    u32 date;
    bool ready = false;
};

bool message_group_neighbors(const MessageScene& scene, const MessageRecord& previous,
                             const MessageRecord& current, MessageGroupDate* cached) noexcept {
    // System rows always stand alone and never start a group.
    if (message_type_is_system(previous.type) || message_type_is_system(current.type)) {
        if (cached) cached->ready = false;
        return false;
    }
    if (previous.channel_id != current.channel_id || previous.author_id == 0 ||
        previous.author_id != current.author_id || previous.timestamp_ms == 0 ||
        current.timestamp_ms == 0 || current.timestamp_ms < previous.timestamp_ms ||
        previous.delivery != current.delivery ||
        previous.content_availability != current.content_availability) {
        if (cached) cached->ready = false;
        return false;
    }
    const MessageRichRecord* current_rich = row_rich(scene, current);
    // A reply starts a group; a subsequent plain message may continue it.
    if (current_rich && message_rich_has_header(*current_rich)) {
        if (cached) cached->ready = false;
        return false;
    }
    u32 previous_date = 0;
    u32 current_date = 0;
    const bool previous_valid = cached && cached->ready && cached->timestamp_ms == previous.timestamp_ms
                                    ? (previous_date = cached->date, true)
                                    : local_message_date(previous.timestamp_ms, &previous_date);
    if (cached) cached->ready = false;
    if (!previous_valid || !local_message_date(current.timestamp_ms, &current_date))
        return false;
    if (cached) {
        cached->timestamp_ms = current.timestamp_ms;
        cached->date = current_date;
        cached->ready = true;
    }
    return previous_date == current_date;
}

bool message_group_neighbors(const MessageScene& scene, const MessageRecord& previous,
                             const MessageRecord& current) noexcept {
    return message_group_neighbors(scene, previous, current, nullptr);
}

bool row_is_grouped(const MessageScene& scene, const MessageRecord& record, const MessageRichRecord* rich,
                    u32 source_index) noexcept {
    if (scene.result_cards)
        return false;
    // System rows use the headerless (grouped) geometry: icon gutter, one line.
    if (message_type_is_system(record.type))
        return true;
    if (record.id == scene.unread_first_message_id || source_index == 0 || record.author_id == 0 ||
        record.timestamp_ms == 0 || (rich && message_rich_has_header(*rich))) {
        return false;
    }
    const MessageStore& store = *scene.timeline->store;
    const MessageRecord* previous =
        message_store_resolve(store, message_scene_source_handle(scene, source_index - 1u));
    if (!previous || !message_group_neighbors(scene, *previous, record)) {
        return false;
    }

    // The seven-minute boundary is measured from the first row of the current
    // group, not from the preceding message. Locate the homogeneous run, then
    // replay its group-start transitions. The scan is bounded by the retained
    // store's capacity, and only materialized rows ask for this projection.
    u32 run_first = source_index - 1u;
    while (run_first != 0) {
        const MessageRecord* left =
            message_store_resolve(store, message_scene_source_handle(scene, run_first - 1u));
        const MessageRecord* right =
            message_store_resolve(store, message_scene_source_handle(scene, run_first));
        if (!left || !right || right->id == scene.unread_first_message_id ||
            !message_group_neighbors(scene, *left, *right)) {
            break;
        }
        --run_first;
    }
    const MessageRecord* anchor =
        message_store_resolve(store, message_scene_source_handle(scene, run_first));
    if (!anchor) {
        return false;
    }
    u32 group_first = run_first;
    u64 anchor_timestamp = anchor->timestamp_ms;
    for (u32 index = run_first + 1u; index <= source_index; ++index) {
        const MessageRecord* item = message_store_resolve(store, message_scene_source_handle(scene, index));
        if (!item || item->timestamp_ms < anchor_timestamp) {
            return false;
        }
        if (item->timestamp_ms - anchor_timestamp >= kMessageGroupIntervalMilliseconds) {
            group_first = index;
            anchor_timestamp = item->timestamp_ms;
        }
    }
    return group_first != source_index;
}

template <typename Scene> float available_text_width(const Scene& scene, const UiRect& viewport) noexcept {
    return maximum(0.0f, ui_rect_width(viewport) - metrics(scene).message_left - avatar_width(scene) -
                             metrics(scene).message_avatar_gap - metrics(scene).message_right);
}

template <typename Scene>
float row_reply_height(const Scene& scene, const MessageSceneRowWidget& row,
                       const MessageRichRecord* rich) noexcept {
    if (!rich || !message_rich_has_header(*rich)) {
        return 0.0f;
    }
    return row.text_measured ? row.measured_reply_height : reply_height(scene);
}

template <typename Scene>
float desired_content_height(const Scene& scene, const MessageSceneRowWidget& row) noexcept {
    return row.text_measured ? row.measured_content_height : content_height(scene);
}

float unread_row_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return !row.forwarded_snapshot && row.message_id == scene.unread_first_message_id && row.message_id
               ? scene.unread_divider_style.height
               : 0;
}

// The channel-beginning block occupies the top of the first resident row.
float intro_index_height(const MessageScene& scene, u32 source_index) noexcept {
    return scene.owns_rows && !scene.result_cards && source_index == 0 ? scene.intro_height : 0.0f;
}
float intro_row_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return row.forwarded_snapshot ? 0.0f : intro_index_height(scene, row.source_index);
}
// Everything above the message body inside a row: intro, then unread divider.
float row_leading(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return intro_row_height(scene, row) + unread_row_height(scene, row);
}

float result_card_leading(const MessageScene& scene) noexcept {
    return scene.result_cards ? scene.result_card_style.heading_height : 0;
}
float result_card_trailing(const MessageScene& scene) noexcept {
    return scene.result_cards ? scene.result_card_style.action_height + scene.result_card_style.gap : 0;
}
bool set_resident_height(MessageScene* scene, u32 index, u32 units) noexcept {
    auto& timeline = *scene->timeline;
    if (!scene->source_order.count) {
        MessageTimelineInvalidation invalidation{};
        return message_timeline_set_item_height(&timeline, index, units, &invalidation);
    }
    // An immutable result projection may repeat model handles. Its variable
    // list has display indices, while the model retains canonical ID lookup.
    UiScrollAnchor anchor{};
    if (!timeline.follow_bottom && timeline.list->count &&
        !ui_variable_list_capture_anchor(*timeline.list, timeline.scroll_units, &anchor))
        return false;
    if (!ui_variable_list_set_height(timeline.list, index, units))
        return false;
    u64 restored = timeline.scroll_units;
    if (anchor.valid && !ui_variable_list_restore_anchor(*timeline.list, anchor, &restored))
        return false;
    return message_timeline_restore_scroll(&timeline, restored, timeline.follow_bottom);
}

float row_required_height(const MessageScene& scene, const MessageSceneRowWidget& row,
                          const MessageRichRecord* rich) noexcept {
    // A plugin-hidden timeline row keeps its list slot at one extent unit.
    if (row.redacted && scene.plugin_decorations)
        return kPluginHiddenRowHeight + intro_row_height(scene, row);
    if (row.forwarded_snapshot)
        return snapshot_header(scene) + 2.0f * snapshot_padding(scene) +
               desired_content_height(scene, row) + row.rich_layout.total();
    const float minimum_height = row.grouped ? grouped_minimum(scene) : ungrouped_minimum(scene);
    return row_leading(scene, row) + result_card_leading(scene) + result_card_trailing(scene) +
           maximum(minimum_height,
                   row_top_padding(scene, row.grouped) + row_bottom_padding(scene, row.grouped) +
                       row_reply_height(scene, row, rich) + row_author_height(scene, row.grouped) +
                       desired_content_height(scene, row) + row.rich_layout.total() + row.poll_height +
                       row.snapshot_height);
}

const MessageRichRecord* snapshot_rich(const MessageScene& scene, u64 message_id) noexcept {
    if (scene.plugin_decorations && g_ui_plugin_render_query) {
        const MessageStoreResult found = message_store_find(*scene.timeline->store, message_id);
        const MessageRecord* record = found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
        if (record && plugin_folded(scene, *record))
            return nullptr;
    }
    return scene.timeline->rich_store ? message_rich_store_snapshot(*scene.timeline->rich_store, message_id)
                                      : nullptr;
}

template <typename Scene>
float snapshot_following_gap(const Scene& scene, const MessageRichRecord* outer) noexcept {
    // Rich-media gap between a snapshot and the outer message's own rich content.
    return outer && (outer->attachment_count || outer->embed_count || outer->sticker_count ||
                     outer->component_count || outer->reaction_count || outer->truncations)
               ? metrics(scene).message_rich_gap
               : 0.f;
}

float snapshot_estimated_height(MessageScene* scene, MessageHandle handle,
                                const MessageRichRecord* snapshot, float width,
                                const MessageRichRecord* outer) noexcept {
    if (!snapshot)
        return 0.0f;
    auto* cached = scene->owns_rows && scene->timeline->store->capacity + handle.slot <
                                           scene->owned_measurements.capacity
                       ? message_scene_detail::measurement(scene, handle.slot, true)
                       : nullptr;
    const u64 key = hash_mix(hash_mix(rich_layout_signature(snapshot), ui_extent_units(width)),
                             scene->theme_geometry_revision);
    if (cached && cached->signature == key)
        return snapshot_header(scene) + 2 * snapshot_padding(scene) + cached->content_height +
               cached->rich.total() + snapshot_following_gap(scene, outer);
    return snapshot_header(scene) + 2 * snapshot_padding(scene) + content_height(scene) +
           message_rich_measure_layout(scene->timeline->rich_store, snapshot, width, scene->theme_styles)
               .total() +
           snapshot_following_gap(scene, outer);
}

bool prepare_resident_measurements(MessageScene* scene, UiRect viewport) noexcept {
    MessageTimeline& timeline = *scene->timeline;
    const MessageStore& store = *timeline.store;
    const u32 source_count = message_scene_source_count(*scene);
    const MessageRichStore* rich_store = timeline.rich_store;
    const float width = available_text_width(scene, viewport);
    u64 epoch = hash_mix(kHash64Seed, ui_extent_units(width));
    epoch = hash_mix(epoch, timeline.reset_generation);
    epoch = hash_mix(epoch, scene->theme_geometry_revision);
    epoch = hash_mix(epoch, scene->unread_revision);
    epoch = hash_mix(epoch, scene->result_card_revision);
    epoch = hash_mix(epoch, scene->source_order_revision);
    epoch = hash_mix(epoch, store.created);
    epoch = hash_mix(epoch, store.updated);
    epoch = hash_mix(epoch, store.deleted);
    epoch = hash_mix(epoch, store.count);
    epoch = hash_mix(epoch, source_count);
    epoch = hash_mix(epoch, timeline.list->structural_updates);
    epoch = hash_mix(epoch, timeline.list->generation);
    epoch = hash_mix(epoch, ui_extent_units(scene->intro_height));
    // Plugin verdicts can hide rows; remeasure when any decoration changes.
    if (scene->plugin_decorations)
        epoch = hash_mix(epoch, g_ui_plugin_render_revision);
    if (rich_store) {
        epoch = hash_mix(epoch, rich_store->updated);
        epoch = hash_mix(epoch, rich_store->deleted);
        epoch = hash_mix(epoch, rich_store->compactions);
    }
    if (scene->polls && scene->polls->store) {
        epoch = hash_mix(epoch, reinterpret_cast<usize>(scene->polls->store));
        epoch = hash_mix(epoch, scene->polls->store->content_revision);
        epoch = hash_mix(epoch, scene->polls->backend_revision);
    }
    // Store reset counters can repeat when switching channels.
    if (store.count) {
        epoch = hash_mix(epoch, store.records[store.order[0]].id);
        epoch = hash_mix(epoch, store.records[store.order[store.count - 1]].id);
    }
    if (scene->prepared_signature == epoch)
        return true;
    const u64 previous_unread = scene->unread_first_message_id;
    scene->unread_first_message_id = 0;
    if (scene->unread_cursor_known) {
        for (u32 index = 0; index < source_count; ++index) {
            const auto* record = message_store_resolve(store, message_scene_source_handle(*scene, index));
            if (record && record->channel_id == scene->unread_channel_id &&
                record->delivery == MessageDeliveryState::authoritative &&
                record->id > scene->unread_last_read_message_id) {
                scene->unread_first_message_id = record->id;
                break;
            }
        }
    }
    if (!scene->owns_rows && !previous_unread && !scene->unread_first_message_id) {
        scene->prepared_signature = epoch;
        return true;
    }
    UiScrollAnchor anchor{};
    if (source_count && !ui_variable_list_capture_anchor(*timeline.list, timeline.scroll_units, &anchor))
        return false;
    const MessageRecord* previous = nullptr;
    u64 group_start = 0;
    u64 previous_group_start = 0;
    MessageGroupDate group_date;
    for (u32 index = 0; index < source_count; ++index) {
        const MessageHandle handle = message_scene_source_handle(*scene, index);
        const MessageRecord* record = message_store_resolve(store, handle);
        if (!record)
            return false;
        // A plugin-hidden message keeps a one-unit slot and neither starts nor
        // continues a group, so its neighbours keep their own headers.
        if (scene->owns_rows && scene->plugin_decorations &&
            message_scene_plugin_visibility(*scene, *record) == ui_plugin_render_hidden) {
            if (!ui_variable_list_set_height(
                    timeline.list, index,
                    ui_extent_units(kPluginHiddenRowHeight + intro_index_height(*scene, index))))
                return false;
            continue;
        }
        const MessageRichRecord* rich = row_rich(*scene, *record);
        const bool divider = record->id == scene->unread_first_message_id;
        const bool neighbors = previous && message_group_neighbors(*scene, *previous, *record, &group_date);
        const bool system = !scene->result_cards && message_type_is_system(record->type);
        const bool grouped = system || (!scene->result_cards && !divider && neighbors &&
                                        record->timestamp_ms - group_start < kMessageGroupIntervalMilliseconds);
        const bool previous_divider = record->id == previous_unread;
        const bool previous_grouped =
            system || (!previous_divider && neighbors &&
                       record->timestamp_ms - previous_group_start < kMessageGroupIntervalMilliseconds);
        if (!grouped)
            group_start = record->timestamp_ms;
        if (!previous_grouped)
            previous_group_start = record->timestamp_ms;
        previous = record;
        if (!scene->owns_rows) {
            // Borrowed scenes keep their measured row heights in the timeline even
            // after the widgets leave the viewport. A read-state change must not
            // replace them with body estimates, which can clamp a valid scroll anchor.
            const float before = row_top_padding(scene, previous_grouped) +
                                 row_bottom_padding(scene, previous_grouped) +
                                 row_author_height(scene, previous_grouped) +
                                 (previous_divider ? scene->unread_prepared_height : 0);
            const float after = row_top_padding(scene, grouped) + row_bottom_padding(scene, grouped) +
                                row_author_height(scene, grouped) +
                                (divider ? scene->unread_divider_style.height : 0);
            const float height = maximum(
                (grouped ? grouped_minimum(scene) : ungrouped_minimum(scene)) +
                    (divider ? scene->unread_divider_style.height : 0),
                ui_extent_dips(ui_variable_list_item_units(*timeline.list, index)) + after - before);
            if (!ui_variable_list_set_height(timeline.list, index, ui_extent_units(height)))
                return false;
            continue;
        }
        auto* cached = message_scene_detail::measurement(scene, handle.slot);
        if (!cached)
            return false;
        u64 key = hash_mix(kHash64Seed, record->id);
        key = hash_mix(key, timeline.reset_generation);
        key = hash_mix(key, handle.generation);
        key = hash_mix(key, record->revision);
        key = hash_mix(key, rich_layout_signature(rich));
        key = hash_mix(key, ui_extent_units(width));
        key = hash_mix(key, scene->theme_geometry_revision);
        // Compact group starts carry the author name inside the body text.
        if (scene->display.compact)
            key = hash_mix(key, grouped ? 2 : 1);
        // Group headers/divider height do not change body wrapping. Preserve
        // the existing text/rich measurements when only read state changes.
        if (cached->signature != key) {
            *cached = {};
            cached->signature = key;
            cached->content_height = content_height(scene);
            cached->items = message_rich_estimate_item_layout(rich_store, rich, width, scene->theme_styles);
            cached->rich = rich ? message_rich_aggregate_layout(*rich, cached->items, width)
                                : MessageSceneRichLayout{};
            for (u32 i = 0; rich && i < rich->reaction_count; ++i)
                cached->reactions[i] = message_rich_reaction_button_width(rich->reactions[i]);
        }
        cached->grouped = grouped;
        const float reply = !rich || !message_rich_has_header(*rich) ? 0.0f
                            : cached->measured                       ? cached->reply_height
                                                                     : reply_height(scene);
        float poll_extent{};
        if (!poll_height(*scene, *record, handle, width, &poll_extent))
            return false;
        const float height =
            result_card_leading(*scene) + result_card_trailing(*scene) + intro_index_height(*scene, index) +
            (divider ? scene->unread_divider_style.height : 0) +
            maximum(grouped ? grouped_minimum(scene) : ungrouped_minimum(scene),
                    row_top_padding(scene, grouped) + row_bottom_padding(scene, grouped) +
                        row_author_height(scene, grouped) + reply + cached->content_height +
                        cached->rich.total() + poll_extent +
                        snapshot_estimated_height(scene, handle, snapshot_rich(*scene, record->id),
                                                  maximum(1.f, width - 2 * snapshot_padding(scene)), rich));
        if (!ui_variable_list_set_height(timeline.list, index, ui_extent_units(height)))
            return false;
    }
    u64 restored = timeline.scroll_units;
    if (anchor.valid && !ui_variable_list_restore_anchor(*timeline.list, anchor, &restored))
        return false;
    if (!message_timeline_restore_scroll(&timeline, restored, timeline.follow_bottom))
        return false;
    scene->unread_prepared_height = scene->unread_divider_style.height;
    scene->prepared_signature = epoch;
    saturating_increment(scene->preparation_passes);
    scene->prepared_rows = source_count;
    return true;
}

UiLayoutNode* mutable_layout_node(MessageScene* scene, UiWidgetHandle handle) noexcept {
    UiWidget* widget = ui_widget_manager_resolve(&scene->widgets, handle);
    return widget ? ui_layout_engine_resolve(&scene->widgets.layout, widget->layout) : nullptr;
}

bool set_fixed_height(MessageScene* scene, UiWidgetHandle handle, float height) noexcept {
    UiLayoutNode* node = mutable_layout_node(scene, handle);
    if (!node || !ui_extent_valid(height)) {
        return false;
    }
    node->spec.style.height = ui_layout_fixed(height);
    return true;
}

bool apply_row_specs(MessageScene* scene, MessageSceneRowWidget* row) noexcept {
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, row->message);
    if (!record || record->id != row->message_id || row->source_index >= scene->timeline->list->count) {
        return false;
    }
    const MessageRichRecord* rich = row_rich(*scene, *record);
    const float reply = row_reply_height(scene, *row, rich);
    const float resident_height =
        ui_extent_dips(ui_variable_list_item_units(*scene->timeline->list, row->source_index));
    const float author_height = row_author_height(scene, row->grouped);
    const float fixed = row_top_padding(scene, row->grouped) + row_bottom_padding(scene, row->grouped) +
                        reply + author_height + row->rich_layout.total() + row->poll_height +
                        row->snapshot_height + row_leading(*scene, *row) +
                        result_card_leading(*scene) + result_card_trailing(*scene);
    const float content = maximum(0.0f, resident_height - fixed);
    UiLayoutNode* row_node = mutable_layout_node(scene, row->widget);
    if (!row_node) {
        return false;
    }
    row_node->spec.style.height = ui_layout_fixed(resident_height);
    row_node->spec.style.gap = metrics(scene).message_avatar_gap;
    if (scene->plugin_decorations)
        if (auto* widget = ui_widget_manager_resolve(&scene->widgets, row->widget))
            widget->flags = row->redacted ? static_cast<u16>(widget->flags | ui_widget_block_input)
                                          : static_cast<u16>(widget->flags & ~ui_widget_block_input);
    row_node->spec.style.padding =
        UiInsets{metrics(scene).message_left,
                 row_top_padding(scene, row->grouped) + row_leading(*scene, *row) +
                     result_card_leading(*scene),
                 metrics(scene).message_right,
                 row_bottom_padding(scene, row->grouped) + result_card_trailing(*scene)};
    UiLayoutNode* avatar_node = mutable_layout_node(scene, row->avatar);
    if (!avatar_node) {
        return false;
    }
    avatar_node->spec.style.width = ui_layout_fixed(avatar_width(scene));
    avatar_node->spec.style.height = ui_layout_fixed(avatar_height(scene, row->grouped));
    avatar_node->spec.style.margin.top = row->grouped ? 0.0f : reply + 0.f;
    if (row->has_snapshot) {
        auto* child = const_cast<MessageSceneRowWidget*>(message_scene_snapshot_row(*scene, *row));
        const auto* snapshot = snapshot_rich(*scene, row->message_id);
        if (!child || !snapshot || !message_rich_widgets_apply(&scene->widgets, snapshot, child) ||
            !set_fixed_height(scene, child->widget,
                              row->snapshot_height - snapshot_following_gap(scene, rich)) ||
            !set_fixed_height(scene, child->reply, snapshot_header(scene)) ||
            !set_fixed_height(scene, child->content, desired_content_height(scene, *child)))
            return false;
    }
    return message_rich_widgets_apply(&scene->widgets, rich, row) &&
           set_fixed_height(scene, row->reply, reply) &&
           set_fixed_height(scene, row->author, author_height) &&
           set_fixed_height(scene, row->content, content);
}

void clear_materialization(MessageScene* scene) noexcept {
    ui_widget_manager_reset(&scene->widgets);
    scene->root = ui_widget_invalid_handle();
    scene->first = 0;
    scene->count = 0;
    scene->snapshot_count = 0;
    scene->active = false;
    scene->dirty = false;
}

u64 focused_message(const MessageScene& scene) noexcept {
    const MessageSceneRowWidget* focused = message_scene_resolve(scene, scene.widgets.focused);
    return focused ? focused->message_id : scene.focused_message_id;
}

bool row_identity_matches(const MessageScene& scene, const MessageSceneRowWidget& row,
                          u32 source_index) noexcept {
    const MessageHandle handle = message_scene_source_handle(scene, source_index);
    const MessageRecord* record = message_store_resolve(*scene.timeline->store, handle);
    const MessageRichRecord* rich = record ? row_rich(scene, *record) : nullptr;
    const auto* snapshot = record ? snapshot_rich(scene, record->id) : nullptr;
    const auto* child = message_scene_snapshot_row(scene, row);
    if (row.has_snapshot != (snapshot != nullptr) ||
        (snapshot && (!child || child->message_revision != record->revision ||
                      child->rich_signature != rich_layout_signature(snapshot) ||
                      child->measured_width != maximum(1.f, available_text_width(scene, scene.viewport) -
                                                                2 * snapshot_padding(scene)) ||
                      !message_rich_widgets_structure_matches(*child, snapshot))))
        return false;
    InviteLink invites[kMessageInviteCardMaximum];
    return record && row.source_index == source_index && row.message_id == record->id &&
           message_rich_widgets_structure_matches(
               row, rich, record && (row_poll(scene, *record) || unavailable_poll(scene, *record)),
               message_scene_invite_codes(scene, *record, {invites, kMessageInviteCardMaximum}));
}

bool materialization_matches(const MessageScene& scene, u32 first, u32 count) noexcept {
    if (!scene.active || scene.first != first || scene.count != count) {
        return false;
    }
    for (u32 local = 0; local < count; ++local) {
        if (!row_identity_matches(scene, scene.rows[local], first + local)) {
            return false;
        }
    }
    return true;
}

bool refresh_row(MessageScene* scene, MessageSceneRowWidget* row, u32 source_index,
                 bool preserve_measurement, bool* data_changed) noexcept {
    const MessageHandle handle = message_scene_source_handle(*scene, source_index);
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, handle);
    if (!record) {
        return false;
    }
    if (!preserve_measurement || row->message.slot != handle.slot ||
        row->message.generation != handle.generation || row->message_id != record->id ||
        row->message_revision != record->revision || record->revision == ~u32{0})
        row->invite_scan_negative = false;
    const MessageRichRecord* rich = row_rich(*scene, *record);
    const u64 signature = rich_layout_signature(rich);
    const float width = available_text_width(scene, scene->viewport);
    const auto* poll = row_poll(*scene, *record);
    const bool missing_poll = unavailable_poll(*scene, *record);
    const u32 poll_content = poll ? poll->content_revision : missing_poll ? ~u32{0} : 0;
    const u32 poll_result = poll ? poll->result_revision : 0;
    const bool poll_changed =
        row->poll_content_revision != poll_content || row->poll_result_revision != poll_result;
    if (!preserve_measurement || row->message_id != record->id || row->measured_width != width ||
        row->poll_content_revision != poll_content ||
        row->poll_backend_revision != (scene->polls ? scene->polls->backend_revision : 0)) {
        const float previous_poll_height = row->poll_height;
        if (!poll_height(*scene, *record, handle, width, &row->poll_height))
            return false;
        if (!scene->owns_rows && preserve_measurement && previous_poll_height != row->poll_height) {
            const float resident =
                ui_extent_dips(ui_variable_list_item_units(*scene->timeline->list, source_index));
            if (!set_resident_height(
                    scene, source_index,
                    ui_extent_units(maximum(1.f, resident + row->poll_height - previous_poll_height))))
                return false;
            scene->dirty = true;
        }
    }
    row->poll_content_revision = poll_content;
    row->poll_unavailable = missing_poll;
    row->poll_result_revision = poll_result;
    row->poll_backend_revision = scene->polls ? scene->polls->backend_revision : 0;
    const auto* cached = message_scene_detail::measurement(scene, handle.slot);
    const bool grouped = cached ? cached->grouped : row_is_grouped(*scene, *record, rich, source_index);
    const bool grouping_changed = row->grouped != grouped;
    const bool text_identity_changed = !preserve_measurement || row->message_id != record->id ||
                                       row->message_revision != record->revision ||
                                       row->rich_signature != signature || row->measured_width != width ||
                                       (scene->display.compact && grouping_changed);
    *data_changed = *data_changed || row->message.slot != handle.slot ||
                    row->message.generation != handle.generation ||
                    row->message_revision != record->revision || row->rich_signature != signature ||
                    grouping_changed || poll_changed;
    row->message = handle;
    row->message_id = record->id;
    row->message_revision = record->revision;
    row->source_index = source_index;
    row->rich_signature = signature;
    row->grouped = grouped;
    if (scene->plugin_decorations) {
        // Timeline rows reuse the redacted presentation for plugin-hidden rows.
        const bool hidden = message_scene_plugin_visibility(*scene, *record) == ui_plugin_render_hidden;
        *data_changed = *data_changed || row->redacted != hidden;
        row->redacted = hidden;
    }
    row->snapshot_height =
        snapshot_estimated_height(scene, handle, snapshot_rich(*scene, record->id),
                                  maximum(1.f, width - 2 * snapshot_padding(scene)), rich);
    if (const auto* child = message_scene_snapshot_row(*scene, *row))
        row->snapshot_height = row_required_height(*scene, *child, snapshot_rich(*scene, record->id)) +
                               snapshot_following_gap(scene, rich);
    const MessageSceneRichItemLayout estimated_items =
        cached ? cached->items
               : message_rich_estimate_item_layout(scene->timeline->rich_store, rich, width,
                                                   scene->theme_styles);
    const MessageSceneRichLayout estimated_rich =
        rich ? message_rich_aggregate_layout(*rich, estimated_items, width) : MessageSceneRichLayout{};
    if (text_identity_changed) {
        row->rich_layout = estimated_rich;
        row->rich_item_layout = estimated_items;
        row->measured_width = width;
        row->measured_reply_height = 0.0f;
        row->measured_content_height = content_height(scene);
        row->rich_layout_measured = false;
        row->text_measured = false;
        for (u32 index = 0; index < kGatewayMessageReactionMaximum; ++index) {
            row->reaction_widths[index] = rich && index < rich->reaction_count
                                              ? message_rich_reaction_button_width(rich->reactions[index])
                                              : 0.0f;
        }
    } else if (!row->rich_layout_measured) {
        row->rich_layout = estimated_rich;
        row->rich_item_layout = estimated_items;
    }
    if (cached)
        message_scene_detail::restore_measurement(row, *cached);
    return true;
}

UiWidgetResult add_region(MessageScene* scene, UiWidgetHandle parent, u32 id, const char* name,
                          float height, bool text) noexcept {
    u32 bytes = 0;
    while (name && name[bytes] != 0) {
        ++bytes;
    }
    return ui_widget_manager_add_prevalidated(
        &scene->widgets, parent,
        UiWidgetDesc{
            id, text ? UiWidgetKind::text : UiWidgetKind::container, ui_widget_enabled,
            UiLayoutNodeSpec{Utf8View{name, bytes}, region_style(height, text), UiSize{}, 0.0f, id}});
}

void rebase_retained_rows(MessageScene* scene, u32 first, u32 count) noexcept {
    if (!scene->active || scene->count == 0 || count == 0) {
        return;
    }
    const u32 old_first = scene->first;
    const u32 old_end = old_first + scene->count;
    const u32 new_end = first + count;
    const u32 overlap_first = maximum(old_first, first);
    const u32 overlap_end = minimum(old_end, new_end);
    if (overlap_first >= overlap_end) {
        for (u32 local = 0; local < count; ++local) {
            scene->rows[local] = MessageSceneRowWidget{};
        }
        return;
    }

    const u32 destination_first = overlap_first - first;
    const u32 destination_end = overlap_end - first;
    if (first >= old_first) {
        for (u32 source = overlap_first; source < overlap_end; ++source) {
            scene->rows[source - first] = scene->rows[source - old_first];
        }
    } else {
        for (u32 source = overlap_end; source != overlap_first; --source) {
            const u32 source_index = source - 1u;
            scene->rows[source_index - first] = scene->rows[source_index - old_first];
        }
    }
    for (u32 local = 0; local < destination_first; ++local) {
        scene->rows[local] = MessageSceneRowWidget{};
    }
    for (u32 local = destination_end; local < count; ++local) {
        scene->rows[local] = MessageSceneRowWidget{};
    }
}

bool build_row(MessageScene* scene, u32 local, u32 source_index) noexcept {
    const MessageSceneRowWidget retained = scene->rows[local];
    const bool preserve_measurement =
        scene->active && retained.message_id != 0 && retained.source_index == source_index;
    MessageSceneRowWidget row = preserve_measurement ? retained : MessageSceneRowWidget{};
    row.widget = ui_widget_invalid_handle();
    row.avatar = ui_widget_invalid_handle();
    row.body = ui_widget_invalid_handle();
    row.reply = ui_widget_invalid_handle();
    row.author = ui_widget_invalid_handle();
    row.content = ui_widget_invalid_handle();
    message_rich_widgets_reset_handles(&row);
    bool data_changed = false;
    if (!refresh_row(scene, &row, source_index, preserve_measurement, &data_changed)) {
        return false;
    }
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, row.message);
    const MessageRichRecord* rich = record ? row_rich(*scene, *record) : nullptr;
    const float height = row_required_height(*scene, row, rich);
    if (source_index > (~0u - 2u) / kMessageSceneWidgetsPerRow) {
        return false;
    }
    // Every materialized row owns one disjoint, fixed-stride ID range.  The
    // root owns ID 1, row N owns [2 + N * stride, 1 + (N + 1) * stride], and
    // rich children stay inside that range.  Validate the arithmetic once and
    // avoid an O(widget_count) duplicate scan for every widget in the row.
    const u32 id = 2u + source_index * kMessageSceneWidgetsPerRow;
    const UiWidgetResult root = ui_widget_manager_add_prevalidated(
        &scene->widgets, scene->root,
        UiWidgetDesc{
            id, UiWidgetKind::container,
            static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
            UiLayoutNodeSpec{utf8_literal("MessageRow"),
                             row_style(scene, height, row.grouped,
                                       row_leading(*scene, row) + result_card_leading(*scene)),
                             UiSize{}, 0.0f, id}});
    const UiWidgetResult avatar = ui_widget_manager_add_prevalidated(
        &scene->widgets, root.handle,
        UiWidgetDesc{id + 1u, UiWidgetKind::image, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Avatar"),
                                      avatar_style(scene, row.grouped, row_reply_height(scene, row, rich)),
                                      UiSize{}, 0.0f, id + 1u}});
    const UiWidgetResult body = ui_widget_manager_add_prevalidated(
        &scene->widgets, root.handle,
        UiWidgetDesc{id + 2u, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Body"), body_style(), UiSize{}, 0.0f, id + 2u}});
    if (!root.ok() || !avatar.ok() || !body.ok()) {
        return false;
    }
    const UiWidgetResult reply = add_region(scene, body.handle, id + 3u, "Reply", 0.0f, true);
    const UiWidgetResult author =
        add_region(scene, body.handle, id + 4u, "Author", row_author_height(scene, row.grouped), true);
    const UiWidgetResult content =
        add_region(scene, body.handle, id + 5u, "Content", content_height(scene), true);
    if (!reply.ok() || !author.ok() || !content.ok()) {
        return false;
    }
    row.widget = root.handle;
    row.avatar = avatar.handle;
    row.body = body.handle;
    row.reply = reply.handle;
    row.author = author.handle;
    row.content = content.handle;
    row.has_snapshot = false;
    const MessageRichRecord* snapshot = snapshot_rich(*scene, row.message_id);
    if (snapshot) {
        if (scene->snapshot_count >= scene->snapshot_rows.count) {
            if (!scene->owns_rows ||
                !stable_storage_reserve(&scene->owned_rows,
                                        scene->snapshot_storage_offset + scene->snapshot_count + 1u))
                return false;
            scene->snapshot_rows = {stable_storage_data<MessageSceneRowWidget>(scene->owned_rows) +
                                        scene->snapshot_storage_offset,
                                    scene->owned_rows.capacity - scene->snapshot_storage_offset};
        }
        row.has_snapshot = true;
        row.snapshot_slot = static_cast<u16>(scene->snapshot_count++);
        MessageSceneRowWidget child{};
        child.message = row.message;
        child.message_id = row.message_id;
        child.message_revision = row.message_revision;
        child.source_index = row.source_index;
        child.grouped = true;
        child.forwarded_snapshot = true;
        child.rich_signature = rich_layout_signature(snapshot);
        child.measured_width =
            maximum(1.f, available_text_width(scene, scene->viewport) - 2 * snapshot_padding(scene));
        child.measured_content_height = content_height(scene);
        child.rich_item_layout = message_rich_estimate_item_layout(
            scene->timeline->rich_store, snapshot, child.measured_width, scene->theme_styles);
        child.rich_layout =
            message_rich_aggregate_layout(*snapshot, child.rich_item_layout, child.measured_width);
        auto* cached =
            scene->owns_rows ? message_scene_detail::measurement(scene, row.message.slot, true) : nullptr;
        const u64 key =
            hash_mix(hash_mix(rich_layout_signature(snapshot), ui_extent_units(child.measured_width)),
                     scene->theme_geometry_revision);
        if (cached && cached->signature == key)
            message_scene_detail::restore_measurement(&child, *cached);
        else if (cached) {
            *cached = {};
            cached->signature = key;
            message_scene_detail::retain_measurement(scene, child);
        }
        const float following_gap = snapshot_following_gap(scene, rich);
        row.snapshot_height = row_required_height(*scene, child, snapshot) + following_gap;
        UiLayoutStyle snapshot_style = body_style();
        snapshot_style.height = ui_layout_fixed(row.snapshot_height - following_gap);
        snapshot_style.margin.bottom = following_gap;
        snapshot_style.padding = {snapshot_padding(scene), snapshot_padding(scene), snapshot_padding(scene),
                                  snapshot_padding(scene)};
        const u32 child_id = id + kMessageSceneWidgetsPerContent;
        const auto frame = ui_widget_manager_add_prevalidated(
            &scene->widgets, body.handle,
            {child_id,
             UiWidgetKind::container,
             ui_widget_enabled,
             {utf8_literal("ForwardedSnapshot"), snapshot_style, {}, 0, child_id}});
        if (!frame.ok())
            return false;
        child.widget = child.body = frame.handle;
        child.avatar = child.author = ui_widget_invalid_handle();
        const auto header =
            add_region(scene, frame.handle, child_id + 3u, "Forwarded", snapshot_header(scene), true);
        const auto child_content = add_region(scene, frame.handle, child_id + 5u, "ForwardedContent",
                                              desired_content_height(scene, child), true);
        if (!header.ok() || !child_content.ok())
            return false;
        child.reply = header.handle;
        child.content = child_content.handle;
        if (!message_rich_widgets_build(&scene->widgets, frame.handle, child_id, snapshot, &child))
            return false;
        scene->snapshot_rows[row.snapshot_slot] = child;
    }
    InviteLink invites[kMessageInviteCardMaximum];
    const u32 invite_count =
        record ? message_scene_invite_codes(*scene, *record, {invites, kMessageInviteCardMaximum}) : 0u;
    if (!message_rich_widgets_build(&scene->widgets, body.handle, id, rich, &row, invite_count)) {
        return false;
    }
    scene->rows[local] = row;
    return apply_row_specs(scene, &scene->rows[local]);
}

} // namespace

bool message_scene_initialize(MessageScene* scene, MessageTimeline* timeline,
                              Slice<MessageSceneRowWidget> row_storage, Slice<UiWidget> widget_storage,
                              Slice<UiLayoutNode> layout_storage) noexcept {
    if (!scene || !timeline || !timeline->store || !timeline->list || !row_storage.data ||
        row_storage.count == 0 || !widget_storage.data || !layout_storage.data ||
        widget_storage.count == 0 || layout_storage.count == 0) {
        return false;
    }
    *scene = MessageScene{};
    scene->rows = row_storage;
    scene->timeline = timeline;
    scene->root = ui_widget_invalid_handle();
    scene->unread_divider_style = kDefaultUiUnreadDividerStyle;
    scene->unread_prepared_height = kDefaultUiUnreadDividerStyle.height;
    return ui_widget_manager_initialize(&scene->widgets, widget_storage, layout_storage);
}

namespace {
void invalidate_geometry(MessageScene* scene) noexcept {
    scene->prepared_signature = 0;
    scene->dirty = true;
    if (scene->owned_measurements.arena.base)
        clear_bytes(scene->owned_measurements.arena.base,
                    scene->owned_measurements.capacity * scene->owned_measurements.element_bytes);
    for (u32 index = 0; index < scene->count; ++index) {
        auto& row = scene->rows[index];
        row.text_measured = false;
        row.rich_layout_measured = false;
        row.measured_width = -1;
        row.poll_content_revision = 0;
    }
    for (u32 index = 0; index < scene->snapshot_rows.count; ++index) {
        auto& row = scene->snapshot_rows[index];
        row.text_measured = false;
        row.rich_layout_measured = false;
        row.measured_width = -1;
    }
}
} // namespace

bool message_scene_bind_theme_styles(MessageScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    invalidate_geometry(scene);
    return true;
}

void message_scene_invalidate_geometry(MessageScene* scene) noexcept {
    if (scene)
        invalidate_geometry(scene);
}

MessageMediaPolicyResult message_scene_media_policy(const MessageScene& scene,
                                                    const MessageMediaOwner& owner) noexcept {
    return scene.media_policy.query ? scene.media_policy.query(scene.media_policy.context, owner)
                                    : MessageMediaPolicyResult{MessageExplicitFilter::show, true};
}

bool message_scene_set_media_policy(MessageScene* scene, const MessageMediaPolicy& policy) noexcept {
    if (!scene ||
        (scene->media_policy.context == policy.context && scene->media_policy.query == policy.query &&
         scene->media_policy.revision == policy.revision))
        return false;
    scene->media_policy = policy;
    invalidate_geometry(scene);
    return true;
}

MessageMediaObscure message_scene_media_obscure(const MessageScene& scene, const MessageMediaOwner& owner,
                                                u64 message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept {
    return message_scene_media_obscure(scene, message_scene_media_policy(scene, owner), message_id,
                                       reveal_key, spoiler, explicit_media);
}

MessageMediaObscure message_scene_media_obscure(const MessageScene& scene, MessageMediaPolicyResult policy,
                                                u64 message_id, u64 reveal_key, bool spoiler,
                                                bool explicit_media) noexcept {
    const MessageMediaObscure obscure = message_media_obscure(
        scene.media_reveals, reveal_key, spoiler, explicit_media, policy.filter,
        message_rich_display().spoilers == MessageSpoilerReveal::always, policy.explicit_reveal_allowed);
    if (obscure == MessageMediaObscure::blocked || !message_id || !scene.plugin_decorations ||
        !g_ui_plugin_render_query || !scene.timeline || !scene.timeline->store)
        return obscure;
    // Plugin media verdicts: hide replaces the preview like blocked media,
    // blur obscures it like explicit content until clicked.
    const MessageStoreResult found = message_store_find(*scene.timeline->store, message_id);
    const MessageRecord* record =
        found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
    UiPluginRenderDecoration decoration{};
    if (!record || !message_scene_plugin_decoration(scene, record->id, record->revision, &decoration))
        return obscure;
    if (decoration.media == ui_plugin_media_hide)
        return MessageMediaObscure::plugin_hidden;
    if (decoration.media == ui_plugin_media_blur && obscure == MessageMediaObscure::none &&
        !message_media_revealed(scene.media_reveals, reveal_key))
        return MessageMediaObscure::explicit_content;
    return obscure;
}

bool message_scene_reveal_media(MessageScene* scene, u64 reveal_key) noexcept {
    if (!scene || !message_media_reveal(&scene->media_reveals, reveal_key))
        return false;
    // Blocked notices and blur labels can differ in height from previews.
    invalidate_geometry(scene);
    return true;
}

bool message_scene_set_interaction(MessageScene* scene, const MessageSceneInteraction& interaction) noexcept {
    if (!scene)
        return false;
    const auto& current = scene->interaction;
    if (current.message_id == interaction.message_id && current.component_index == interaction.component_index &&
        current.pending == interaction.pending && current.failed == interaction.failed)
        return false;
    const bool notice_changed = current.failed != interaction.failed ||
                                (interaction.failed && current.message_id != interaction.message_id);
    scene->interaction = interaction;
    ++scene->interaction_revision;
    if (notice_changed)
        invalidate_geometry(scene);
    else
        scene->dirty = true;
    return true;
}

u32 message_scene_invite_codes(const MessageScene& scene, const MessageRecord& record,
                               Slice<InviteLink> output) noexcept {
    if (!scene.invite_cards || !scene.timeline || !scene.timeline->store || !output.data ||
        record.content_availability != MessageContentAvailability::available || record.content_bytes < 12 ||
        message_type_is_system(record.type) || plugin_folded(scene, record))
        return 0;
    const MessageStoreResult found = message_store_find(*scene.timeline->store, record.id);
    if (!found.ok())
        return 0;
    const Utf8View content = message_store_content(*scene.timeline->store, found.handle);
    return message_markdown_find_invites(
        content, {output.data, minimum<u32>(output.count, kMessageInviteCardMaximum)});
}

bool message_scene_set_content_gated(MessageScene* scene, bool gated) noexcept {
    if (!scene || scene->content_gated == gated)
        return false;
    scene->content_gated = gated;
    scene->dirty = true;
    return true;
}

bool message_scene_bind_invite_cards(MessageScene* scene, const InviteCardCache* cache) noexcept {
    if (!scene || scene->invite_cards == cache)
        return false;
    scene->invite_cards = cache;
    invalidate_geometry(scene);
    return true;
}

bool message_scene_bind_scheduled_events(MessageScene* scene, const ScheduledEventStore* events) noexcept {
    if (!scene || scene->scheduled_events == events)
        return false;
    scene->scheduled_events = events;
    return true;
}

bool message_scene_set_display(MessageScene* scene, MessageSceneDisplay value) noexcept {
    if (!scene || value.group_spacing >= 6 ||
        (scene->display.compact == value.compact && scene->display.group_spacing == value.group_spacing))
        return false;
    scene->display = value;
    invalidate_geometry(scene);
    return true;
}

bool message_scene_bind_poll_source(MessageScene* scene, const MessageScenePollBinding* binding) noexcept {
    if (!scene || (binding && (!binding->measure || !binding->backend_revision)))
        return false;
    if (scene->polls == binding)
        return true;
    scene->polls = binding;
    scene->prepared_signature = 0;
    scene->dirty = true;
    return true;
}

bool message_scene_poll_projection(const MessageScene& scene, const MessageSceneRowWidget& row,
                                   const Theme& theme, Slice<UiPollAnswer> answers,
                                   UiPollProjection* out) noexcept {
    if (!out || !scene.active || row.forwarded_snapshot || row.redacted || !scene.polls ||
        !scene.timeline || !scene.timeline->store || !ui_widget_manager_resolve(scene.widgets, row.poll))
        return false;
    const auto* record = message_store_resolve(*scene.timeline->store, row.message);
    const auto* node = message_scene_layout_node(scene, row.poll);
    const auto* poll = record ? row_poll(scene, *record) : nullptr;
    UiPollState state{};
    if (!record || record->id != row.message_id || record->revision != row.message_revision || !poll ||
        row.poll_content_revision != poll->content_revision ||
        row.poll_result_revision != poll->result_revision || !node ||
        !poll_state(scene, *record, row.message, answers, &state))
        return false;
    return ui_poll_controls_project(state, node->content_rectangle, ui_theme_poll_style(scene.theme_styles),
                                    theme, scene.polls->measure, scene.polls->backend_revision, out);
}

bool message_scene_initialize_owned(MessageScene* scene, MessageTimeline* timeline, u32 maximum_rows,
                                    u32 initial_rows, Slice<UiWidget> widget_storage,
                                    Slice<UiLayoutNode> layout_storage, bool defer_measurements) noexcept {
    if (!scene || scene->owned_rows.arena.base || maximum_rows == 0 || initial_rows == 0 ||
        initial_rows > maximum_rows) {
        return false;
    }
    StableStorage owned_rows{};
    if (maximum_rows > kInvalidUiWidgetSlot || maximum_rows > ~0u / 2u ||
        !stable_storage_initialize(&owned_rows, 2u * maximum_rows, initial_rows,
                                   sizeof(MessageSceneRowWidget), alignof(MessageSceneRowWidget), false)) {
        return false;
    }
    if (!message_scene_initialize(
            scene, timeline,
            Slice<MessageSceneRowWidget>{stable_storage_data<MessageSceneRowWidget>(owned_rows),
                                         owned_rows.capacity},
            widget_storage, layout_storage)) {
        stable_storage_release(&owned_rows);
        return false;
    }
    scene->owned_rows = owned_rows;
    scene->owns_rows = true;
    scene->snapshot_storage_offset = maximum_rows;
    if (!message_scene_detail::initialize_measurements(scene, defer_measurements)) {
        message_scene_release(scene);
        return false;
    }
    return true;
}

bool message_scene_reserve_measurements(MessageScene* scene) noexcept {
    return scene && scene->owned_measurements.arena.base && scene->timeline && scene->timeline->store &&
           stable_storage_reserve(&scene->owned_measurements, scene->timeline->store->capacity);
}

void message_scene_release(MessageScene* scene) noexcept {
    if (!scene) {
        return;
    }
    if (scene->owns_rows) {
        stable_storage_release(&scene->owned_rows);
    }
    stable_storage_release(&scene->owned_measurements);
    *scene = MessageScene{};
}

bool message_scene_reserve_rows(MessageScene* scene, u32 required_rows) noexcept {
    if (!scene || required_rows == 0) {
        return false;
    }
    if (required_rows <= scene->rows.count) {
        return true;
    }
    if (!scene->owns_rows || required_rows > scene->snapshot_storage_offset ||
        !stable_storage_grow(&scene->owned_rows, required_rows)) {
        return false;
    }
    scene->rows =
        Slice<MessageSceneRowWidget>{stable_storage_data<MessageSceneRowWidget>(scene->owned_rows),
                                     minimum(scene->snapshot_storage_offset, scene->owned_rows.capacity)};
    return true;
}

bool message_scene_set_highlight(MessageScene* scene, u64 message_id, float level) noexcept {
    if (!scene)
        return false;
    if (!message_id || level <= 0.0f) {
        message_id = 0;
        level = 0.0f;
    }
    level = minimum(level, 1.0f);
    if (scene->highlight_message_id == message_id && scene->highlight_level == level)
        return false;
    scene->highlight_message_id = message_id;
    scene->highlight_level = level;
    saturating_increment(scene->highlight_revision);
    return true;
}

bool message_scene_set_unread_cursor(MessageScene* scene, u64 channel_id, u64 last_read_message_id,
                                     bool known, bool* changed,
                                     const UiUnreadDividerStyle* style) noexcept {
    if (changed)
        *changed = false;
    if (!scene || !scene->timeline || !scene->timeline->store || (known && !channel_id) ||
        (style && !ui_unread_divider_style_valid(*style)))
        return false;
    const auto next_style = style ? *style : scene->unread_divider_style;
    const bool style_changed =
        next_style.height != scene->unread_divider_style.height ||
        next_style.horizontal_inset != scene->unread_divider_style.horizontal_inset ||
        next_style.label_width != scene->unread_divider_style.label_width ||
        next_style.label_gap != scene->unread_divider_style.label_gap ||
        next_style.line_thickness != scene->unread_divider_style.line_thickness;
    if (!known) {
        channel_id = 0;
        last_read_message_id = 0;
    }
    const bool updated = style_changed || scene->unread_cursor_known != known ||
                         scene->unread_channel_id != channel_id ||
                         scene->unread_last_read_message_id != last_read_message_id;
    if (!updated)
        return true;
    scene->unread_cursor_known = known;
    scene->unread_channel_id = channel_id;
    scene->unread_last_read_message_id = last_read_message_id;
    scene->unread_divider_style = next_style;
    saturating_increment(scene->unread_revision);
    scene->dirty = true;
    if (changed)
        *changed = true;
    return true;
}

bool message_scene_set_result_card_style(MessageScene* scene, const MessageResultCardStyle* style,
                                         bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!scene || !scene->timeline || !scene->owns_rows ||
        (style && !message_result_card_style_valid(*style)))
        return false;
    const auto next = style ? *style : MessageResultCardStyle{};
    if (scene->result_cards == (style != nullptr) &&
        scene->result_card_style.heading_height == next.heading_height &&
        scene->result_card_style.action_height == next.action_height &&
        scene->result_card_style.gap == next.gap &&
        scene->result_card_style.materialized_rows == next.materialized_rows)
        return true;
    scene->result_card_style = next;
    scene->result_cards = style != nullptr;
    saturating_increment(scene->result_card_revision);
    scene->dirty = true;
    if (changed)
        *changed = true;
    return true;
}

MessageHandle message_scene_source_handle(const MessageScene& scene, u32 index) noexcept {
    if (!scene.timeline || !scene.timeline->store)
        return {};
    if (scene.source_order.count)
        return index < scene.source_order.count ? scene.source_order[index] : MessageHandle{};
    return message_store_ordered_handle(*scene.timeline->store, index);
}
u32 message_scene_source_count(const MessageScene& scene) noexcept {
    return scene.source_order.count                  ? scene.source_order.count
           : scene.timeline && scene.timeline->store ? scene.timeline->store->count
                                                     : 0;
}
bool message_scene_set_source_order(MessageScene* scene, ConstSlice<MessageHandle> order,
                                    u32 revision) noexcept {
    if (!scene || !scene->timeline || !scene->timeline->store || (!order.data && order.count) ||
        (order.count && (!scene->result_cards || order.count != scene->timeline->list->count)))
        return false;
    if (scene->source_order.data == order.data && scene->source_order.count == order.count &&
        scene->source_order_revision == revision)
        return true;
    for (u32 i = 0; i < order.count; ++i) {
        if (!message_store_resolve(*scene->timeline->store, order[i]))
            return false;
    }
    scene->source_order = order;
    scene->source_order_revision = revision;
    scene->prepared_signature = 0;
    scene->dirty = true;
    return true;
}

namespace {
// Channel icon 68 px (margin-top 16), thread icon 64 px (margin-top 16),
// DM and group DM avatar 80 px (no margin).
float intro_icon_extent(MessageSceneIntroKind kind) noexcept {
    switch (kind) {
    case MessageSceneIntroKind::channel:
        return 68.0f;
    case MessageSceneIntroKind::thread:
        return 64.0f;
    case MessageSceneIntroKind::direct_message:
    case MessageSceneIntroKind::group_direct_message:
        return 80.0f;
    default:
        return 0.0f;
    }
}
float intro_icon_margin(MessageSceneIntroKind kind) noexcept {
    return kind == MessageSceneIntroKind::channel || kind == MessageSceneIntroKind::thread ? 16.0f : 0.0f;
}
float intro_text_width(const MessageScene& scene, float viewport_width) noexcept {
    (void)scene;
    return maximum(1.0f, viewport_width - 2.0f * kMessageSceneIntroMargin);
}
float intro_measure_text(MessageScene* scene, u16 item, Utf8View text, ConstSlice<TextStyleSpan> styles,
                         float width, float line, float em) noexcept {
    if (!text.bytes)
        return 0.0f;
    if (scene->intro_measure) {
        u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
        for (u32 index = 0; index < styles.count; ++index)
            signature = hash_mix(signature, (static_cast<u64>(styles[index].byte_offset) << 32) ^
                                                styles[index].byte_count ^ (u64{styles[index].size} << 56));
        UiTextMeasureValue value{};
        if (ui_text_measure_cache_measure(scene->intro_measure,
                                          {{0x494E54524F000000ull ^ scene->intro.channel_id, signature, 1, item, 0},
                                           text,
                                           styles,
                                           {width, kMaximumUiCoordinate},
                                           false},
                                          &value)
                .ok() &&
            (value.line_count || value.size.height > 0.0f))
            return value.line_count ? static_cast<float>(value.line_count) * line : maximum(line, value.size.height);
    }
    // No text backend (tests, first frame): about half an em per byte.
    const float per_line = maximum(1.0f, width / (em * 0.5f));
    const u32 lines = static_cast<u32>(static_cast<float>(text.bytes) / per_line) + 1u;
    return static_cast<float>(lines) * line;
}
void refresh_intro_measurement(MessageScene* scene, float viewport_width) noexcept {
    const MessageSceneIntro& intro = scene->intro;
    if (intro.kind == MessageSceneIntroKind::none || !scene->owns_rows || scene->result_cards ||
        !(viewport_width > 0.0f)) {
        scene->intro_height = 0.0f;
        scene->intro_header_height = scene->intro_subtitle_height = scene->intro_description_height = 0.0f;
        return;
    }
    const float width = intro_text_width(*scene, viewport_width);
    if (scene->intro_measured_width == width)
        return;
    const Utf8View header = message_scene_intro_header(intro);
    const Utf8View subtitle = message_scene_intro_subtitle(intro);
    const Utf8View description = message_scene_intro_description(intro);
    scene->intro_header_height = intro_measure_text(scene, 1, header, {&scene->intro_header_style, 1}, width,
                                                    kMessageSceneIntroHeaderLine, 32.0f);
    scene->intro_subtitle_height = intro_measure_text(scene, 2, subtitle, {&scene->intro_subtitle_style, 1},
                                                      width, kMessageSceneIntroSubtitleLine, 20.0f);
    scene->intro_description_height =
        intro_measure_text(scene, 3, description, {intro.description_spans, intro.description_span_count}, width,
                           kMessageSceneIntroDescriptionLine, 16.0f);
    float height = 2.0f * kMessageSceneIntroMargin + intro_icon_margin(intro.kind) + intro_icon_extent(intro.kind) +
                   scene->intro_subtitle_height + scene->intro_description_height;
    if (header.bytes)
        height += 2.0f * kMessageSceneIntroHeaderMargin + scene->intro_header_height;
    scene->intro_height = height;
    scene->intro_measured_width = width;
}
// The intro occupies the top of the first row, or with no rows the bottom of
// the viewport (an empty channel's beginning).
UiRect intro_area(const MessageScene& scene) noexcept {
    if (!scene.active || !(scene.intro_height > 0.0f))
        return {};
    if (scene.timeline && scene.timeline->list && scene.timeline->list->count == 0)
        return {scene.viewport.left, scene.viewport.bottom - scene.intro_height, scene.viewport.right,
                scene.viewport.bottom};
    for (u32 index = 0; index < scene.count; ++index) {
        const auto& row = scene.rows[index];
        if (row.source_index != 0 || row.forwarded_snapshot)
            continue;
        const auto* node = message_scene_layout_node(scene, row.widget);
        if (!node)
            return {};
        UiRect area = node->rectangle;
        area.bottom = minimum(area.bottom, area.top + scene.intro_height);
        return area;
    }
    return {};
}
} // namespace

namespace {
struct IntroWriter {
    MessageSceneIntro* intro;
    u32 used;
    // Appends whole UTF-8 sequences while they fit.
    void add(Utf8View text) noexcept {
        for (u32 offset = 0; text.data && offset < text.bytes;) {
            const Utf8DecodeResult decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
            if (!decoded.valid || decoded.width > kMessageSceneIntroTextBytes - used)
                return;
            copy_bytes(intro->text + used, text.data + offset, decoded.width);
            used += decoded.width;
            offset += decoded.width;
        }
    }
    void bold(u32 description_begin, Utf8View text) noexcept {
        const u32 begin = used;
        add(text);
        if (used > begin && intro->description_span_count < kMessageSceneIntroSpanMaximum)
            intro->description_spans[intro->description_span_count++] = {begin - description_begin, used - begin,
                                                                         text_style_bold, 0, {}};
    }
};
} // namespace

bool message_scene_intro_compose(MessageSceneIntro* intro, MessageSceneIntroKind kind, u64 channel_id,
                                 Utf8View name, Utf8View username, u64 user_id, bool locked) noexcept {
    if (!intro || kind == MessageSceneIntroKind::none || (!name.data && name.bytes) ||
        (!username.data && username.bytes))
        return false;
    *intro = MessageSceneIntro{};
    intro->kind = kind;
    intro->channel_id = channel_id;
    intro->locked = locked;
    IntroWriter out{intro, 0};
    // Header (channel welcome / thread name / conversation name).
    if (kind == MessageSceneIntroKind::channel) {
        out.add(utf8_literal("Welcome to #"));
        out.add(name);
        out.add(utf8_literal("!"));
    } else if (kind != MessageSceneIntroKind::no_history) {
        out.add(name);
    }
    intro->header_bytes = out.used;
    // Subtitle: a DM recipient's username.
    if (kind == MessageSceneIntroKind::direct_message) {
        out.add(username);
        intro->user_id = user_id;
    }
    intro->subtitle_bytes = out.used - intro->header_bytes;
    const u32 description = out.used;
    switch (kind) {
    case MessageSceneIntroKind::channel:
        out.add(utf8_literal("This is the start of the #"));
        out.add(name);
        if (locked) {
            out.add(utf8_literal(" "));
            out.bold(description, utf8_literal("private"));
        }
        out.add(utf8_literal(" channel."));
        break;
    case MessageSceneIntroKind::thread:
        if (locked)
            out.add(utf8_literal("@mention users to add them to this private thread."));
        break;
    case MessageSceneIntroKind::direct_message:
        out.add(utf8_literal("This is the beginning of your direct message history with "));
        out.bold(description, name);
        out.add(utf8_literal("."));
        break;
    case MessageSceneIntroKind::group_direct_message:
        out.add(utf8_literal("Welcome to the beginning of the "));
        out.bold(description, name);
        out.add(utf8_literal(" group."));
        break;
    case MessageSceneIntroKind::no_history: {
        out.add(utf8_literal("You do not have permission to view the message history of "));
        const u32 begin = out.used;
        out.add(utf8_literal("#"));
        out.add(name);
        if (out.used > begin && intro->description_span_count < kMessageSceneIntroSpanMaximum)
            intro->description_spans[intro->description_span_count++] = {begin - description, out.used - begin,
                                                                         text_style_bold, 0, {}};
        out.add(utf8_literal("."));
        break;
    }
    default:
        break;
    }
    intro->description_bytes = out.used - description;
    return true;
}

bool message_scene_set_intro(MessageScene* scene, const MessageSceneIntro* intro,
                             UiTextMeasureCache* measure) noexcept {
    if (!scene)
        return false;
    MessageSceneIntro next{};
    if (intro && intro->kind != MessageSceneIntroKind::none &&
        intro->header_bytes + intro->subtitle_bytes + intro->description_bytes <= kMessageSceneIntroTextBytes &&
        intro->description_span_count <= kMessageSceneIntroSpanMaximum)
        next = *intro;
    u64 signature = hash_mix(kHash64Seed, static_cast<u64>(next.kind) | (u64{next.locked} << 8));
    signature = hash_mix(signature, next.user_id);
    signature = hash_mix(signature, next.channel_id);
    const u32 text_bytes = next.header_bytes + next.subtitle_bytes + next.description_bytes;
    signature = hash_mix(signature, (u64{next.header_bytes} << 40) ^ (u64{next.subtitle_bytes} << 20) ^
                                        next.description_bytes);
    signature = hash64_counted_bytes(signature, next.text, text_bytes);
    signature = hash64_counted_bytes(signature, next.description_spans,
                                     next.description_span_count * static_cast<u32>(sizeof(TextStyleSpan)));
    scene->intro_measure = measure;
    if (signature == scene->intro_signature)
        return false;
    scene->intro = next;
    scene->intro_signature = signature;
    scene->intro_header_style = {0, next.header_bytes, text_style_bold, text_size_xxl, {}};
    scene->intro_subtitle_style = {0, next.subtitle_bytes, text_style_bold, text_size_lg, {}};
    // Measured (width and text) by the next update, before row extents.
    scene->intro_measured_width = 0.0f;
    scene->intro_height = 0.0f;
    scene->intro_header_height = scene->intro_subtitle_height = scene->intro_description_height = 0.0f;
    scene->dirty = true;
    return true;
}

bool message_scene_intro_geometry(const MessageScene& scene, MessageSceneIntroGeometry* geometry) noexcept {
    if (!geometry)
        return false;
    *geometry = {};
    const UiRect area = intro_area(scene);
    if (!ui_rect_has_area(area))
        return false;
    const MessageSceneIntro& intro = scene.intro;
    geometry->area = area;
    const float left = area.left + kMessageSceneIntroMargin;
    const float right = maximum(left + 1.0f, area.right - kMessageSceneIntroMargin);
    float top = area.top + kMessageSceneIntroMargin + intro_icon_margin(intro.kind);
    const float icon = intro_icon_extent(intro.kind);
    if (icon > 0.0f)
        geometry->icon = {left, top, left + icon, top + icon};
    top += icon;
    if (intro.header_bytes) {
        top += kMessageSceneIntroHeaderMargin;
        geometry->header = {left, top, right, top + scene.intro_header_height};
        top += scene.intro_header_height + kMessageSceneIntroHeaderMargin;
    }
    if (intro.subtitle_bytes) {
        geometry->subtitle = {left, top, right, top + scene.intro_subtitle_height};
        top += scene.intro_subtitle_height;
    }
    if (intro.description_bytes)
        geometry->description = {left, top, right, top + scene.intro_description_height};
    return true;
}

UiRect message_scene_unread_divider_rectangle(const MessageScene& scene,
                                              const MessageSceneRowWidget& row) noexcept {
    if (!scene.active || !scene.unread_cursor_known || row.forwarded_snapshot || !row.message_id ||
        row.message_id != scene.unread_first_message_id)
        return {};
    const auto* node = message_scene_layout_node(scene, row.widget);
    if (!node)
        return {};
    auto rectangle = node->rectangle;
    rectangle.top = minimum(rectangle.bottom, rectangle.top + intro_row_height(scene, row));
    rectangle.bottom = minimum(rectangle.bottom, rectangle.top + scene.unread_divider_style.height);
    return rectangle;
}

UiRect message_scene_message_rectangle(const MessageScene& scene,
                                       const MessageSceneRowWidget& row) noexcept {
    const auto* node = message_scene_layout_node(scene, row.widget);
    if (!node)
        return {};
    auto rectangle = node->rectangle;
    rectangle.top = minimum(rectangle.bottom, rectangle.top + intro_row_height(scene, row));
    const auto divider = message_scene_unread_divider_rectangle(scene, row);
    if (ui_rect_has_area(divider))
        rectangle.top = divider.bottom;
    return rectangle;
}

MessageSceneResult message_scene_update(MessageScene* scene, UiRect viewport) noexcept {
    if (!scene || !scene->timeline || !scene->timeline->store || !scene->timeline->list ||
        !scene->rows.data || !scene->widgets.storage.data || !ui_rect_valid_or_empty(viewport) ||
        message_scene_source_count(*scene) != scene->timeline->list->count) {
        return scene_error(scene, MessageSceneError::invalid_input);
    }
    if (!ui_rect_has_area(viewport)) {
        const bool changed =
            scene->active || scene->count != 0 || !ui_rect_equal(scene->viewport, viewport);
        clear_materialization(scene);
        scene->viewport = viewport;
        scene->source_count = message_scene_source_count(*scene);
        return MessageSceneResult{MessageSceneError::none, 0, 0, changed};
    }
    const u32 viewport_units = ui_extent_units(ui_rect_height(viewport));
    if (viewport_units == 0 || !message_timeline_set_viewport(scene->timeline, viewport_units)) {
        return scene_error(scene, MessageSceneError::model);
    }
    refresh_intro_measurement(scene, ui_rect_width(viewport));
    if (!prepare_resident_measurements(scene, viewport))
        return scene_error(scene, MessageSceneError::model);
    UiVariableRange range =
        scene->result_cards ? ui_variable_list_range(*scene->timeline->list, scene->timeline->scroll_units,
                                                     viewport_units, 0)
        : scene->owns_rows
            ? ui_variable_list_buffered_range(*scene->timeline->list, scene->timeline->scroll_units,
                                              viewport_units,
                                              ui_extent_units(kMessageScenePreparationMarginDips))
            : ui_variable_list_range(*scene->timeline->list, scene->timeline->scroll_units, viewport_units,
                                     kMessageSceneOverscanRows);
    if (scene->result_cards)
        range.count = minimum(range.count, scene->result_card_style.materialized_rows);
    if (!scene->owns_rows && scene->active && scene->dirty) {
        const u32 retained_end = scene->first + scene->count;
        const u32 desired_end = range.first + range.count;
        const u32 union_first = minimum(scene->first, range.first);
        const u32 union_end = maximum(retained_end, desired_end);
        const u32 union_count = union_end - union_first;
        if (union_count > scene->rows.count) {
            message_scene_reserve_rows(scene, union_count);
        }
        if (union_count <= scene->rows.count) {
            const u64 first_prefix = ui_variable_list_prefix_units(*scene->timeline->list, union_first);
            range = UiVariableRange{union_first, union_count,
                                    static_cast<i64>(first_prefix) -
                                        static_cast<i64>(scene->timeline->scroll_units)};
        }
    }
    if (range.count > scene->rows.count && !message_scene_reserve_rows(scene, range.count)) {
        clear_materialization(scene);
        scene->viewport = viewport;
        return scene_error(scene, MessageSceneError::capacity);
    }

    const bool retained =
        materialization_matches(*scene, range.first, range.count) &&
        (scene->snapshot_count == 0 || ui_rect_width(scene->viewport) == ui_rect_width(viewport));
    const bool viewport_changed = !ui_rect_equal(scene->viewport, viewport);
    scene->viewport = viewport;
    const bool was_dirty = scene->dirty;
    bool data_changed = false;
    if (retained) {
        for (u32 local = 0; local < range.count; ++local) {
            if (!refresh_row(scene, &scene->rows[local], range.first + local, true, &data_changed) ||
                !apply_row_specs(scene, &scene->rows[local])) {
                return scene_error(scene, MessageSceneError::model);
            }
        }
    } else {
        const u64 restore_focus = focused_message(*scene);
        rebase_retained_rows(scene, range.first, range.count);
        scene->snapshot_count = 0;
        ui_widget_manager_reset(&scene->widgets);
        const UiWidgetResult root = ui_widget_manager_add(
            &scene->widgets, ui_widget_invalid_handle(),
            UiWidgetDesc{kMessageRootWidgetId, UiWidgetKind::scroll_view, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("MessageTimeline"), ui_layout_scroll_root_style(), UiSize{}, 0.0f,
                                          kMessageRootWidgetId}});
        if (!root.ok()) {
            clear_materialization(scene);
            return scene_error(scene, MessageSceneError::widget);
        }
        scene->root = root.handle;
        for (u32 local = 0; local < range.count; ++local) {
            if (!build_row(scene, local, range.first + local)) {
                clear_materialization(scene);
                return scene_error(scene, MessageSceneError::widget);
            }
        }
        scene->first = range.first;
        scene->count = range.count;
        scene->focused_message_id = restore_focus;
        scene->active = true;
        saturating_increment(scene->rebuilds);
    }

    const float local_scroll = maximum(0.0f, -ui_signed_extent_dips(range.first_offset_units));
    // Short content sits against the bottom edge: rows are laid out below a
    // leading gap; scroll offsets are unaffected because such content has no
    // scroll range.
    const u64 total_units = scene->timeline->list->total_units;
    scene->lead_dips = scene->bottom_anchored && scene->timeline->list->count != 0 && total_units < viewport_units
                           ? ui_extent_dips(viewport_units - total_units)
                           : 0.0f;
    UiRect laid_out = viewport;
    laid_out.top = minimum(viewport.bottom - 1.0f, viewport.top + scene->lead_dips);
    const bool observed_change =
        scene->source_count != message_scene_source_count(*scene) ||
        scene->observed_created != scene->timeline->store->created ||
        scene->observed_updated != scene->timeline->store->updated ||
        scene->observed_deleted != scene->timeline->store->deleted ||
        scene->observed_height_updates != scene->timeline->list->height_updates ||
        scene->observed_structural_updates != scene->timeline->list->structural_updates;
    const bool scroll_changed = scene->observed_scroll_units != scene->timeline->scroll_units;
    const u32 rich_updated = scene->timeline->rich_store ? scene->timeline->rich_store->updated : 0;
    const bool paint_changed = scene->observed_rich_updated != rich_updated;
    if (retained && !viewport_changed && !data_changed && !observed_change && !scroll_changed &&
        !was_dirty) {
        // Paint-only rich metadata (such as an embed RGB color) must refresh
        // retained geometry without invalidating its measured text/layout.
        scene->observed_rich_updated = rich_updated;
        saturating_increment_if(paint_changed, scene->data_updates);
        return MessageSceneResult{MessageSceneError::none, range.first, range.count, paint_changed};
    }
    const UiWidget* root_widget = ui_widget_manager_resolve(scene->widgets, scene->root);
    if (!root_widget ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, root_widget->layout,
                                     UiPoint{0.0f, local_scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, laid_out).ok()) {
        clear_materialization(scene);
        return scene_error(scene, MessageSceneError::layout);
    }
    saturating_increment(scene->layout_passes);
    saturating_increment_if(retained && (data_changed || paint_changed), scene->data_updates);
    scene->first = range.first;
    scene->count = range.count;
    scene->source_count = message_scene_source_count(*scene);
    scene->observed_created = scene->timeline->store->created;
    scene->observed_updated = scene->timeline->store->updated;
    scene->observed_deleted = scene->timeline->store->deleted;
    scene->observed_rich_updated = rich_updated;
    scene->observed_height_updates = scene->timeline->list->height_updates;
    scene->observed_structural_updates = scene->timeline->list->structural_updates;
    scene->observed_scroll_units = scene->timeline->scroll_units;
    scene->dirty = false;
    if (scene->focused_message_id != 0) {
        message_scene_focus_message(scene, scene->focused_message_id);
    }
    return MessageSceneResult{MessageSceneError::none, range.first, range.count,
                              !retained || viewport_changed || data_changed || observed_change ||
                                  scroll_changed || was_dirty || paint_changed};
}

bool message_scene_scroll_by(MessageScene* scene, i64 delta_units, u64 bottom_tolerance_units,
                             bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_by(scene->timeline, delta_units, bottom_tolerance_units, changed)) {
        return false;
    }
    scene->dirty = scene->dirty || *changed;
    return true;
}

bool message_scene_scroll_to_start(MessageScene* scene, bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_to_start(scene->timeline, changed)) {
        return false;
    }
    scene->dirty = scene->dirty || *changed;
    return true;
}

bool message_scene_scroll_to_bottom(MessageScene* scene, bool* changed) noexcept {
    if (!scene || !scene->timeline || !changed ||
        !message_timeline_scroll_to_bottom(scene->timeline, changed)) {
        return false;
    }
    scene->dirty = scene->dirty || *changed;
    return true;
}

bool message_scene_set_text_metrics(MessageScene* scene, u32 source_index, float reply_height,
                                    float content_height, bool content_present,
                                    const MessageSceneRichLayout* rich_layout,
                                    const MessageSceneRichItemLayout* rich_item_layout,
                                    ConstSlice<float> reaction_widths, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!scene || !scene->timeline || !changed || !ui_extent_valid(reply_height) ||
        !ui_extent_valid(content_height) || (rich_layout && !message_rich_layout_valid(*rich_layout)) ||
        (rich_item_layout && !message_rich_item_layout_valid(*rich_item_layout)) ||
        (!reaction_widths.data && reaction_widths.count != 0) ||
        reaction_widths.count > kGatewayMessageReactionMaximum) {
        return false;
    }
    MessageSceneRowWidget* row = message_scene_find_row(scene, source_index);
    if (!row) {
        return false;
    }
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, row->message);
    if (!record || record->id != row->message_id) {
        return false;
    }
    const MessageRichRecord* rich = row_rich(*scene, *record);
    const float measured_reply = rich && message_rich_has_header(*rich) ? reply_height : 0.0f;
    const float measured_content = maximum(content_height, content_present ? 16.0f : 0.0f) + 5.0f;
    MessageSceneRichLayout measured_rich = rich_layout ? *rich_layout : row->rich_layout;
    const MessageSceneRichItemLayout measured_items =
        rich_item_layout ? *rich_item_layout : row->rich_item_layout;
    float intrinsic_reaction_widths[kGatewayMessageReactionMaximum]{};
    if (rich && reaction_widths.count >= rich->reaction_count) {
        for (u32 index = 0; index < rich->reaction_count; ++index) {
            const float label_width = reaction_widths[index];
            intrinsic_reaction_widths[index] =
                kDefaultMessageReactionLayoutMetrics.content_left_padding +
                kDefaultMessageReactionLayoutMetrics.content_right_padding + label_width +
                (rich->reactions[index].emoji_id != 0 ||
                         (scene->timeline->rich_store &&
                          regional_flag_sequence(message_rich_store_string(*scene->timeline->rich_store,
                                                                           rich->reactions[index].name)))
                     ? kDefaultMessageReactionLayoutMetrics.emoji_extent +
                           kDefaultMessageReactionLayoutMetrics.emoji_gap
                     : kDefaultMessageReactionLayoutMetrics.unicode_text_inset);
        }
        measured_rich.reactions = message_rich_reaction_layout_height(
            *rich, available_text_width(scene, scene->viewport),
            ConstSlice<float>{intrinsic_reaction_widths, rich->reaction_count});
    }
    if (rich && rich_layout && rich_item_layout &&
        !message_rich_layout_equal(
            measured_rich, message_rich_aggregate_layout(
                               *rich, measured_items, available_text_width(scene, scene->viewport),
                               ConstSlice<float>{intrinsic_reaction_widths, rich->reaction_count}))) {
        return false;
    }
    bool reaction_metrics_changed = false;
    for (u32 index = 0; index < row->reaction_button_count; ++index) {
        const float width = index < reaction_widths.count
                                ? intrinsic_reaction_widths[index]
                                : message_rich_reaction_button_width(rich->reactions[index]);
        reaction_metrics_changed = reaction_metrics_changed || row->reaction_widths[index] != width;
        row->reaction_widths[index] = width;
    }
    const bool metrics_changed = reaction_metrics_changed || !row->text_measured ||
                                 row->measured_reply_height != measured_reply ||
                                 row->measured_content_height != measured_content ||
                                 !message_rich_layout_equal(row->rich_layout, measured_rich) ||
                                 !message_rich_item_layout_equal(row->rich_item_layout, measured_items);
    row->measured_reply_height = measured_reply;
    row->measured_content_height = measured_content;
    row->rich_layout = measured_rich;
    row->rich_item_layout = measured_items;
    row->rich_layout_measured = row->rich_layout_measured || rich_layout != nullptr;
    row->measured_width = available_text_width(scene, scene->viewport);
    row->text_measured = true;
    message_scene_detail::retain_measurement(scene, *row);
    const u32 desired_units = ui_extent_units(row_required_height(*scene, *row, rich));
    const u32 resident_units = ui_variable_list_item_units(*scene->timeline->list, source_index);
    if (desired_units != resident_units) {
        if (!set_resident_height(scene, source_index, desired_units)) {
            return false;
        }
        scene->dirty = true;
        saturating_increment(scene->measurement_updates);
        *changed = true;
    } else {
        *changed = metrics_changed;
        scene->dirty = scene->dirty || metrics_changed;
    }
    return true;
}

UiWidgetHandle message_scene_rich_item_widget(const MessageSceneRowWidget& row,
                                              MessageSceneRichItemKind kind, u32 index) noexcept {
    if (kind == MessageSceneRichItemKind::poll)
        return index == 0 ? row.poll : ui_widget_invalid_handle();
    const u32 count = kind == MessageSceneRichItemKind::attachment  ? row.attachment_item_count
                      : kind == MessageSceneRichItemKind::embed     ? row.embed_item_count
                      : kind == MessageSceneRichItemKind::sticker   ? row.sticker_item_count
                      : kind == MessageSceneRichItemKind::component ? row.component_item_count
                      : kind == MessageSceneRichItemKind::invite    ? row.invite_item_count
                                                                    : row.notice_item_count;
    u32 slot = 0;
    if (index >= count || !message_rich_item_slot(kind, index, &slot)) {
        return ui_widget_invalid_handle();
    }
    return row.rich_items[slot];
}

const MessageSceneRowWidget* message_scene_snapshot_row(const MessageScene& scene,
                                                        const MessageSceneRowWidget& outer) noexcept {
    if (!outer.has_snapshot || outer.snapshot_slot >= scene.snapshot_count ||
        outer.snapshot_slot >= scene.snapshot_rows.count || !scene.snapshot_rows.data)
        return nullptr;
    const auto* child = &scene.snapshot_rows[outer.snapshot_slot];
    return child->forwarded_snapshot && child->message_id == outer.message_id &&
                   child->message_revision == outer.message_revision &&
                   child->message.generation == outer.message.generation
               ? child
               : nullptr;
}

bool message_scene_bind_snapshot_rows(MessageScene* scene, Slice<MessageSceneRowWidget> rows) noexcept {
    if (!scene || scene->active || scene->owns_rows || (!rows.data && rows.count) ||
        rows.count > kInvalidUiWidgetSlot)
        return false;
    scene->snapshot_rows = rows;
    return true;
}

bool message_scene_set_snapshot_text_metrics(MessageScene* scene, u32 source_index, float content_height,
                                             bool content_present,
                                             const MessageSceneRichLayout* rich_layout,
                                             const MessageSceneRichItemLayout* rich_item_layout,
                                             bool* changed) noexcept {
    if (changed)
        *changed = false;
    if (!scene || !changed || !ui_extent_valid(content_height) ||
        (rich_layout && !message_rich_layout_valid(*rich_layout)) ||
        (rich_item_layout && !message_rich_item_layout_valid(*rich_item_layout)))
        return false;
    auto* outer = message_scene_find_row(scene, source_index);
    auto* child =
        outer ? const_cast<MessageSceneRowWidget*>(message_scene_snapshot_row(*scene, *outer)) : nullptr;
    const auto* rich = child ? snapshot_rich(*scene, child->message_id) : nullptr;
    if (!child || !rich)
        return false;
    const float content = maximum(content_height, content_present ? 16.f : 0.f) + 5.f;
    const auto items = rich_item_layout ? *rich_item_layout : child->rich_item_layout;
    const auto layout = rich_layout ? *rich_layout : child->rich_layout;
    if (rich_layout && rich_item_layout &&
        !message_rich_layout_equal(layout,
                                   message_rich_aggregate_layout(*rich, items, child->measured_width)))
        return false;
    *changed = !child->text_measured || child->measured_content_height != content ||
               !message_rich_layout_equal(child->rich_layout, layout) ||
               !message_rich_item_layout_equal(child->rich_item_layout, items);
    child->measured_content_height = content;
    child->rich_layout = layout;
    child->rich_item_layout = items;
    child->rich_layout_measured = rich_layout != nullptr || child->rich_layout_measured;
    child->text_measured = true;
    message_scene_detail::retain_measurement(scene, *child);
    const auto* record = message_store_resolve(*scene->timeline->store, outer->message);
    if (!record)
        return false;
    outer->snapshot_height = row_required_height(*scene, *child, rich) +
                             snapshot_following_gap(scene, row_rich(*scene, *record));
    const u32 units = ui_extent_units(row_required_height(*scene, *outer, row_rich(*scene, *record)));
    if (units != ui_variable_list_item_units(*scene->timeline->list, source_index)) {
        if (!set_resident_height(scene, source_index, units))
            return false;
        *changed = true;
    }
    scene->dirty = scene->dirty || *changed;
    saturating_increment_if(*changed, scene->measurement_updates);
    return true;
}

const UiLayoutNode* message_scene_rich_item_layout_node(const MessageScene& scene,
                                                        const MessageSceneRowWidget& row,
                                                        MessageSceneRichItemKind kind, u32 index) noexcept {
    return message_scene_layout_node(scene, message_scene_rich_item_widget(row, kind, index));
}

const MessageSceneRowWidget* message_scene_resolve(const MessageScene& scene,
                                                   UiWidgetHandle widget) noexcept {
    if (!ui_widget_manager_resolve(scene.widgets, widget)) {
        return nullptr;
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const MessageSceneRowWidget& row = scene.rows[index];
        const UiWidgetHandle handles[] = {row.widget,   row.avatar,     row.body,        row.reply,
                                          row.author,   row.content,    row.attachments, row.embeds,
                                          row.stickers, row.components, row.reactions,   row.notice,
                                          row.poll,     row.thread};
        for (u32 handle_index = 0; handle_index < static_cast<u32>(sizeof(handles) / sizeof(handles[0]));
             ++handle_index) {
            if (handles[handle_index].slot == widget.slot &&
                handles[handle_index].generation == widget.generation) {
                return &row;
            }
        }
        for (u32 reaction = 0; reaction < row.reaction_button_count; ++reaction) {
            if (row.reaction_buttons[reaction].slot == widget.slot &&
                row.reaction_buttons[reaction].generation == widget.generation) {
                return &row;
            }
        }
        for (u32 rich_item = 0; rich_item < kMessageSceneRichItemMaximum; ++rich_item) {
            if (row.rich_items[rich_item].slot == widget.slot &&
                row.rich_items[rich_item].generation == widget.generation) {
                return &row;
            }
        }
        const auto* child = message_scene_snapshot_row(scene, row);
        if (child) {
            const UiWidgetHandle child_handles[]{child->widget,      child->reply,  child->content,
                                                 child->attachments, child->embeds, child->stickers,
                                                 child->components,  child->notice};
            for (const auto handle : child_handles)
                if (handle.slot == widget.slot && handle.generation == widget.generation)
                    return &row;
            for (const auto handle : child->rich_items)
                if (handle.slot == widget.slot && handle.generation == widget.generation)
                    return &row;
        }
    }
    return nullptr;
}

MessageSceneRowWidget* message_scene_find_row(MessageScene* scene, u32 source_index) noexcept {
    if (!scene || source_index < scene->first || source_index - scene->first >= scene->count) {
        return nullptr;
    }
    MessageSceneRowWidget* row = &scene->rows[source_index - scene->first];
    return row->source_index == source_index ? row : nullptr;
}

const MessageSceneRowWidget* message_scene_find_row(const MessageScene& scene, u32 source_index) noexcept {
    if (source_index < scene.first || source_index - scene.first >= scene.count) {
        return nullptr;
    }
    const MessageSceneRowWidget* row = &scene.rows[source_index - scene.first];
    return row->source_index == source_index ? row : nullptr;
}

const UiLayoutNode* message_scene_layout_node(const MessageScene& scene, UiWidgetHandle widget) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, widget);
}

MessageSceneHit message_scene_hit_test(const MessageScene& scene, float x, float y) noexcept {
    const UiWidgetHandle widget = ui_widget_manager_hit_test(scene.widgets, x, y);
    const MessageSceneRowWidget* row = message_scene_resolve(scene, widget);
    if (!row)
        return {};
    const auto divider = message_scene_unread_divider_rectangle(scene, *row);
    if (ui_rect_has_area(divider) && ui_rect_contains(divider, x, y))
        return {};
    // The channel-beginning block is not part of the first message.
    if (y < message_scene_message_rectangle(scene, *row).top)
        return {};
    // Rich children retain their own widget clipping during intrinsic-height
    // convergence; only the decoration strip adds a new hit exclusion.
    return MessageSceneHit{row->message_id, widget, row->source_index};
}

MessageReplyHit message_scene_reply_hit_test(const MessageScene& scene, float x, float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !scene.timeline->rich_store ||
        !ui_rect_contains(scene.viewport, x, y))
        return {};
    const auto hit = message_scene_hit_test(scene, x, y);
    const auto* row = hit.message_id ? message_scene_find_row(scene, hit.source_index) : nullptr;
    if (!row || row->message_id != hit.message_id)
        return {};
    const auto* preview = message_scene_layout_node(scene, row->reply);
    if (!preview ||
        !ui_rect_contains(ui_rect_intersection(preview->rectangle, preview->clip_rectangle), x, y))
        return {};
    const auto* record = message_store_resolve(*scene.timeline->store, row->message);
    const auto* rich = message_rich_store_find(*scene.timeline->rich_store, row->message_id);
    if (!record || record->id != row->message_id || !rich || rich->reference.message_id == 0 ||
        rich->reference.state == GatewayMessageReferenceState::none ||
        rich->reference.state == GatewayMessageReferenceState::deleted)
        return {};
    // A reference can retain its ID before its message has been downloaded.
    // The common navigation path fetches around that ID when it is not loaded.
    return {rich->reference.channel_id ? rich->reference.channel_id : record->channel_id,
            rich->reference.message_id};
}

bool message_scene_reaction_index(const MessageScene& scene, MessageSceneHit hit, u32* index) noexcept {
    if (!index || hit.message_id == 0 || !message_rich_display().reactions) {
        return false;
    }
    const MessageSceneRowWidget* row = message_scene_find_row(scene, hit.source_index);
    if (!row || row->message_id != hit.message_id ||
        !ui_widget_manager_resolve(scene.widgets, hit.widget)) {
        return false;
    }
    for (u32 reaction = 0; reaction < row->reaction_button_count; ++reaction) {
        if (row->reaction_buttons[reaction].slot == hit.widget.slot &&
            row->reaction_buttons[reaction].generation == hit.widget.generation) {
            *index = reaction;
            return true;
        }
    }
    return false;
}

bool message_scene_focus_message(MessageScene* scene, u64 message_id) noexcept {
    if (!scene) {
        return false;
    }
    if (message_id == 0) {
        scene->focused_message_id = 0;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    for (u32 index = 0; index < scene->count; ++index) {
        const MessageSceneRowWidget& row = scene->rows[index];
        if (row.message_id == message_id && ui_widget_manager_focus(&scene->widgets, row.widget)) {
            scene->focused_message_id = message_id;
            return true;
        }
    }
    return false;
}

} // namespace maicord

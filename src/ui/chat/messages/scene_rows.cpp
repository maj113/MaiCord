#include "scene_internal.hpp"
#include "maicord/ui/chat/messages/rich_widgets.hpp"
#include "scene_measurements.hpp"
#include "maicord/ui/chat/polls/controls.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord {
using namespace message_scene_detail;

namespace message_scene_detail {
namespace {
float author_height(const MessageScene& scene) noexcept {
    return message_em(scene) * 1.3125f;
}
float avatar_height(const MessageScene& scene, bool grouped) noexcept {
    return compact(scene) ? content_height(scene) : grouped ? 0.0f : metrics(scene).message_avatar;
}
float group_top(const MessageScene& scene) noexcept {
    static constexpr float spacing[]{0.0f, 0.0f, 4.0f, 8.0f, 16.0f, 24.0f};
    const u8 code = display(scene).group_spacing;
    if (code && code < 6)
        return spacing[code] + 1.0f;
    return compact(scene) ? 1.0f : metrics(scene).message_top;
}
float snapshot_header(const MessageScene& scene) noexcept {
    return message_em(scene) * 1.5f;
}
float snapshot_padding(const MessageScene& scene) noexcept {
    return metrics(scene).message_rich_gap + 2.f;
}
float grouped_minimum(const MessageScene& scene) noexcept {
    return message_em(scene) * 1.375f;
}
float ungrouped_minimum(const MessageScene& scene) noexcept {
    if (compact(scene))
        return group_top(scene) + content_height(scene) + metrics(scene).message_bottom;
    return group_top(scene) +
           maximum(author_height(scene) + content_height(scene), metrics(scene).message_avatar) +
           metrics(scene).message_bottom;
}
float row_top_padding(const MessageScene& scene, bool grouped) noexcept {
    return grouped ? metrics(scene).message_grouped_top : group_top(scene);
}

float row_bottom_padding(const MessageScene& scene, bool grouped) noexcept {
    return grouped ? metrics(scene).message_grouped_bottom : metrics(scene).message_bottom;
}

float row_author_height(const MessageScene& scene, bool grouped) noexcept {
    return grouped || compact(scene) ? 0.0f : author_height(scene);
}

UiLayoutStyle row_style(const MessageScene& scene, float height, bool grouped, float leading = 0) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::horizontal;
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.padding = UiInsets{metrics(scene).message_left, row_top_padding(scene, grouped) + leading,
                             metrics(scene).message_right, row_bottom_padding(scene, grouped)};
    style.gap = avatar_gap(scene);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle avatar_style(const MessageScene& scene, bool grouped, float reply_height) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(avatar_width(scene));
    style.height = ui_layout_fixed(avatar_height(scene, grouped));
    // The large avatar belongs to the current author, so it starts below the
    // reply preview that precedes the header.
    style.margin.top = grouped && !compact(scene) ? 0.0f : reply_height + 0.f;
    if (grouped && !compact(scene)) {
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

UiLayoutStyle region_style(float height) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size | ui_layout_node_text_content);
    return style;
}

} // namespace
namespace {
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
} // namespace
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

namespace {
bool unavailable_poll(const MessageScene& scene, const MessageRecord& record) noexcept {
    return record.content_availability == MessageContentAvailability::available &&
           record.delivery == MessageDeliveryState::authoritative &&
           (record.presentation_flags & message_presentation_has_poll) && !plugin_folded(scene, record) &&
           !row_poll(scene, record);
}

} // namespace
bool poll_state(const MessageScene& scene, const MessageRecord& record, MessageHandle handle,
                const Slice<UiPollAnswer>& answers, UiPollState* out) noexcept {
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

namespace {
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
    const UiPollStyle style = ui_theme_poll_style(scene.theme_styles);
    if (!scene.polls->measure ||
        !poll_state(scene, record, handle, {answers, kUiPollMaximumAnswers}, &state) ||
        !ui_poll_measure_layout(state, width, style, scene.polls->measure,
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
    // Delivery state is not a boundary: a pending or failed send groups exactly
    // as its confirmed form will.
    if (previous.channel_id != current.channel_id || previous.author_id == 0 ||
        previous.author_id != current.author_id || previous.timestamp_ms == 0 ||
        current.timestamp_ms == 0 || current.timestamp_ms < previous.timestamp_ms ||
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

float row_reply_height(const MessageScene& scene, const MessageSceneRowWidget& row,
                       const MessageRichRecord* rich) noexcept {
    if (!rich || !message_rich_has_header(*rich)) {
        return 0.0f;
    }
    return row.text_measured ? row.measured_reply_height : reply_height(scene);
}

float desired_content_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return row.text_measured ? row.measured_content_height : content_height(scene);
}

} // namespace
// Space after the message text: the accessories container's 2 DIP
// top padding when attachments, embeds, stickers, components, invites,
// reactions, a notice, a thread, a poll or a forwarded message follow; 5 DIPs
// under text alone.
float content_trailing(bool accessories) noexcept {
    return accessories ? 2.0f : 5.0f;
}

namespace {
float unread_row_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return !row.forwarded_snapshot && row.message_id == scene.unread_first_message_id && row.message_id
               ? scene.unread_divider_style.height
               : 0;
}

// The channel-beginning block occupies the top of the first resident row.
float intro_index_height(const MessageScene& scene, u32 source_index) noexcept {
    return scene.owns_rows && !scene.result_cards && source_index == 0 ? scene.intro_height : 0.0f;
}
} // namespace
float intro_row_height(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return row.forwarded_snapshot ? 0.0f : intro_index_height(scene, row.source_index);
}
namespace {
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
} // namespace
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

namespace {
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
        return snapshot_header(*scene) + 2 * snapshot_padding(*scene) + cached->content_height +
               cached->rich.total() + snapshot_following_gap(*scene, outer);
    return snapshot_header(*scene) + 2 * snapshot_padding(*scene) + content_height(*scene) +
           message_rich_measure_layout(scene->timeline->rich_store, snapshot, width, scene->theme_styles)
               .total() +
           snapshot_following_gap(*scene, outer);
}

} // namespace
bool prepare_resident_measurements(MessageScene* scene, const UiRect& viewport) noexcept {
    MessageTimeline& timeline = *scene->timeline;
    const MessageStore& store = *timeline.store;
    const u32 source_count = message_scene_source_count(*scene);
    const MessageRichStore* rich_store = timeline.rich_store;
    const float width = available_text_width(*scene, viewport);
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
            const float before = row_top_padding(*scene, previous_grouped) +
                                 row_bottom_padding(*scene, previous_grouped) +
                                 row_author_height(*scene, previous_grouped) +
                                 (previous_divider ? scene->unread_prepared_height : 0);
            const float after = row_top_padding(*scene, grouped) + row_bottom_padding(*scene, grouped) +
                                row_author_height(*scene, grouped) +
                                (divider ? scene->unread_divider_style.height : 0);
            const float height = maximum(
                (grouped ? grouped_minimum(*scene) : ungrouped_minimum(*scene)) +
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
            reset_value(cached);
            cached->signature = key;
            cached->content_height = content_height(*scene);
            if (rich) {
                MessageSceneRichItemLayout items;
                message_rich_fill_item_layout(rich_store, rich, width, scene->theme_styles, items);
                cached->rich = message_rich_aggregate_layout(*rich, items, width);
                message_scene_detail::store_items(scene, handle.slot, false, cached, items);
            }
            for (u32 i = 0; rich && i < rich->reaction_count; ++i)
                cached->reactions[i] = message_rich_reaction_button_width(rich->reactions[i]);
        }
        cached->grouped = grouped;
        const float reply = !rich || !message_rich_has_header(*rich) ? 0.0f
                            : cached->measured                       ? cached->reply_height
                                                                     : reply_height(*scene);
        float poll_extent{};
        if (!poll_height(*scene, *record, handle, width, &poll_extent))
            return false;
        const float height =
            result_card_leading(*scene) + result_card_trailing(*scene) + intro_index_height(*scene, index) +
            (divider ? scene->unread_divider_style.height : 0) +
            maximum(grouped ? grouped_minimum(*scene) : ungrouped_minimum(*scene),
                    row_top_padding(*scene, grouped) + row_bottom_padding(*scene, grouped) +
                        row_author_height(*scene, grouped) + reply + cached->content_height +
                        cached->rich.total() + poll_extent +
                        snapshot_estimated_height(scene, handle, snapshot_rich(*scene, record->id),
                                                  maximum(1.f, width - 2 * snapshot_padding(*scene)), rich));
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
    return true;
}

namespace {
UiLayoutNode* mutable_layout_node(MessageScene* scene, UiWidgetHandle handle) noexcept {
    return ui_widget_manager_layout_node(&scene->widgets, handle);
}

bool set_fixed_height(MessageScene* scene, UiWidgetHandle handle, float height) noexcept {
    UiLayoutNode* node = mutable_layout_node(scene, handle);
    if (!node || !ui_extent_valid(height)) {
        return false;
    }
    node->spec.style.height = ui_layout_fixed(height);
    return true;
}

} // namespace
bool apply_row_specs(MessageScene* scene, MessageSceneRowWidget* row) noexcept {
    const MessageRecord* record = message_store_resolve(*scene->timeline->store, row->message);
    if (!record || record->id != row->message_id || row->source_index >= scene->timeline->list->count) {
        return false;
    }
    const MessageRichRecord* rich = row_rich(*scene, *record);
    const float reply = row_reply_height(*scene, *row, rich);
    const float resident_height =
        ui_extent_dips(ui_variable_list_item_units(*scene->timeline->list, row->source_index));
    const float author_height = row_author_height(*scene, row->grouped);
    const float fixed = row_top_padding(*scene, row->grouped) + row_bottom_padding(*scene, row->grouped) +
                        reply + author_height + row->rich_layout.total() + row->poll_height +
                        row->snapshot_height + row_leading(*scene, *row) +
                        result_card_leading(*scene) + result_card_trailing(*scene);
    const float content = maximum(0.0f, resident_height - fixed);
    UiLayoutNode* row_node = mutable_layout_node(scene, row->widget);
    if (!row_node) {
        return false;
    }
    row_node->spec.style.height = ui_layout_fixed(resident_height);
    row_node->spec.style.gap = avatar_gap(*scene);
    if (scene->plugin_decorations)
        if (auto* widget = ui_widget_manager_resolve(&scene->widgets, row->widget))
            widget->flags = row->redacted ? static_cast<u16>(widget->flags | ui_widget_block_input)
                                          : static_cast<u16>(widget->flags & ~ui_widget_block_input);
    row_node->spec.style.padding =
        UiInsets{metrics(*scene).message_left,
                 row_top_padding(*scene, row->grouped) + row_leading(*scene, *row) +
                     result_card_leading(*scene),
                 metrics(*scene).message_right,
                 row_bottom_padding(*scene, row->grouped) + result_card_trailing(*scene)};
    UiLayoutNode* avatar_node = mutable_layout_node(scene, row->avatar);
    if (!avatar_node) {
        return false;
    }
    avatar_node->spec.style.width = ui_layout_fixed(avatar_width(*scene));
    avatar_node->spec.style.height = ui_layout_fixed(avatar_height(*scene, row->grouped));
    avatar_node->spec.style.margin.top = row->grouped && !compact(*scene) ? 0.0f : reply + 0.f;
    if (row->has_snapshot) {
        auto* child = const_cast<MessageSceneRowWidget*>(message_scene_snapshot_row(*scene, *row));
        const auto* snapshot = snapshot_rich(*scene, row->message_id);
        if (!child || !snapshot || !message_rich_widgets_apply(&scene->widgets, snapshot, child) ||
            !set_fixed_height(scene, child->widget,
                              row->snapshot_height - snapshot_following_gap(*scene, rich)) ||
            !set_fixed_height(scene, child->reply, snapshot_header(*scene)) ||
            !set_fixed_height(scene, child->content, desired_content_height(*scene, *child)))
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
    scene->scroll_dirty = false;
}

u64 focused_message(const MessageScene& scene) noexcept {
    const MessageSceneRowWidget* focused = message_scene_resolve(scene, scene.widgets.focused);
    return focused ? focused->message_id : scene.focused_message_id;
}

namespace {
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
    // A current negative scan result already proves the message has no invite links.
    const bool no_invites = record && row.invite_scan_negative && row.message_id == record->id &&
                            row.message_revision == record->revision && record->revision != ~u32{0};
    InviteLink invites[kMessageInviteCardMaximum];
    return record && row.source_index == source_index && row.message_id == record->id &&
           message_rich_widgets_structure_matches(
               row, rich, record && (row_poll(scene, *record) || unavailable_poll(scene, *record)),
               no_invites ? 0u
                          : message_scene_invite_codes(scene, *record, {invites, kMessageInviteCardMaximum}));
}

} // namespace
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
    const float width = available_text_width(*scene, scene->viewport);
    const auto* poll = row_poll(*scene, *record);
    const bool missing_poll = unavailable_poll(*scene, *record);
    const u32 poll_content = poll ? poll->content_revision : missing_poll ? ~u32{0} : 0;
    const u32 poll_result = poll ? poll->result_revision : 0;
    const bool poll_changed =
        row->poll_content_revision != poll_content || row->poll_result_revision != poll_result;
    if (!preserve_measurement || row->message_id != record->id || row->measured_width != width ||
        poll_changed || row->poll_backend_revision != (scene->polls ? scene->polls->backend_revision : 0)) {
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
    const auto* snapshot = snapshot_rich(*scene, record->id);
    row->snapshot_height = snapshot_estimated_height(scene, handle, snapshot,
                                                     maximum(1.f, width - 2 * snapshot_padding(*scene)), rich);
    if (const auto* child = message_scene_snapshot_row(*scene, *row))
        row->snapshot_height = row_required_height(*scene, *child, snapshot) + snapshot_following_gap(*scene, rich);
    if (text_identity_changed || !row->rich_layout_measured) {
        // A cached measurement overwrites both layouts below (restore_measurement).
        if (!cached) {
            message_rich_fill_item_layout(scene->timeline->rich_store, rich, width, scene->theme_styles,
                                          row->rich_item_layout);
            row->rich_layout = rich ? message_rich_aggregate_layout(*rich, row->rich_item_layout, width)
                                    : MessageSceneRichLayout{};
        }
        if (text_identity_changed) {
            row->measured_width = width;
            row->measured_reply_height = 0.0f;
            row->measured_content_height = content_height(*scene);
            row->rich_layout_measured = false;
            row->text_measured = false;
            for (u32 index = 0; index < kGatewayMessageReactionMaximum; ++index) {
                row->reaction_widths[index] = rich && index < rich->reaction_count
                                                  ? message_rich_reaction_button_width(rich->reactions[index])
                                                  : 0.0f;
            }
        }
    }
    if (cached)
        message_scene_detail::restore_measurement(*scene, row, *cached);
    return true;
}

namespace {
template <usize Count>
__forceinline UiWidgetResult add_region(MessageScene* scene, UiWidgetHandle parent, u32 id,
                                        const char (&name)[Count], float height) noexcept {
    return ui_widget_manager_add_keyed(&scene->widgets, parent, id, UiWidgetKind::text, ui_widget_enabled, name,
                                       Count - 1, region_style(height));
}

} // namespace
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
            reset_value<MessageSceneRowWidget>(&scene->rows[local]);
        }
        return;
    }

    const u32 destination_first = overlap_first - first;
    const u32 destination_end = overlap_end - first;
    if (first >= old_first) {
        for (u32 source = overlap_first; source < overlap_end; ++source) {
            copy_value(&scene->rows[source - first], scene->rows[source - old_first]);
        }
    } else {
        for (u32 source = overlap_end; source != overlap_first; --source) {
            const u32 source_index = source - 1u;
            copy_value(&scene->rows[source_index - first], scene->rows[source_index - old_first]);
        }
    }
    for (u32 local = 0; local < destination_first; ++local) {
        reset_value<MessageSceneRowWidget>(&scene->rows[local]);
    }
    for (u32 local = destination_end; local < count; ++local) {
        reset_value<MessageSceneRowWidget>(&scene->rows[local]);
    }
}

bool build_row(MessageScene* scene, u32 local, u32 source_index) noexcept {
    // The row is built where it lives: a failed build clears the whole
    // materialization, so a half-built row is never read.
    MessageSceneRowWidget& row = scene->rows[local];
    const bool preserve_measurement = scene->active && row.message_id != 0 && row.source_index == source_index;
    if (!preserve_measurement)
        reset_value(&row);
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
    const UiWidgetResult root = ui_widget_manager_add_keyed(
        &scene->widgets, scene->root, id, UiWidgetKind::container,
        static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable), "MessageRow",
        row_style(*scene, height, row.grouped, row_leading(*scene, row) + result_card_leading(*scene)));
    const UiWidgetResult avatar = ui_widget_manager_add_keyed(
        &scene->widgets, root.handle, id + 1u, UiWidgetKind::image, ui_widget_enabled, "Avatar",
        avatar_style(*scene, row.grouped, row_reply_height(*scene, row, rich)));
    const UiWidgetResult body = ui_widget_manager_add_keyed(&scene->widgets, root.handle, id + 2u,
                                                            UiWidgetKind::container, ui_widget_enabled, "Body",
                                                            body_style());
    if (!root.ok() || !avatar.ok() || !body.ok()) {
        return false;
    }
    const UiWidgetResult reply = add_region(scene, body.handle, id + 3u, "Reply", 0.0f);
    const UiWidgetResult author =
        add_region(scene, body.handle, id + 4u, "Author", row_author_height(*scene, row.grouped));
    const UiWidgetResult content = add_region(scene, body.handle, id + 5u, "Content", content_height(*scene));
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
        MessageSceneRowWidget& child = scene->snapshot_rows[row.snapshot_slot];
        reset_value(&child);
        child.message = row.message;
        child.message_id = row.message_id;
        child.message_revision = row.message_revision;
        child.source_index = row.source_index;
        child.grouped = true;
        child.forwarded_snapshot = true;
        child.rich_signature = rich_layout_signature(snapshot);
        child.measured_width =
            maximum(1.f, available_text_width(*scene, scene->viewport) - 2 * snapshot_padding(*scene));
        child.measured_content_height = content_height(*scene);
        message_rich_fill_item_layout(scene->timeline->rich_store, snapshot, child.measured_width,
                                      scene->theme_styles, child.rich_item_layout);
        child.rich_layout =
            message_rich_aggregate_layout(*snapshot, child.rich_item_layout, child.measured_width);
        auto* cached =
            scene->owns_rows ? message_scene_detail::measurement(scene, row.message.slot, true) : nullptr;
        const u64 key =
            hash_mix(hash_mix(rich_layout_signature(snapshot), ui_extent_units(child.measured_width)),
                     scene->theme_geometry_revision);
        if (cached && cached->signature == key)
            message_scene_detail::restore_measurement(*scene, &child, *cached);
        else if (cached) {
            reset_value(cached);
            cached->signature = key;
            message_scene_detail::retain_measurement(scene, child);
        }
        const float following_gap = snapshot_following_gap(*scene, rich);
        row.snapshot_height = row_required_height(*scene, child, snapshot) + following_gap;
        UiLayoutStyle snapshot_style = body_style();
        snapshot_style.height = ui_layout_fixed(row.snapshot_height - following_gap);
        snapshot_style.margin.bottom = following_gap;
        snapshot_style.padding = {snapshot_padding(*scene), snapshot_padding(*scene), snapshot_padding(*scene),
                                  snapshot_padding(*scene)};
        const u32 child_id = id + kMessageSceneWidgetsPerContent;
        const auto frame = ui_widget_manager_add_keyed(&scene->widgets, body.handle, child_id,
                                                       UiWidgetKind::container, ui_widget_enabled,
                                                       "ForwardedSnapshot", snapshot_style);
        if (!frame.ok())
            return false;
        child.widget = child.body = frame.handle;
        child.avatar = child.author = ui_widget_invalid_handle();
        const auto header = add_region(scene, frame.handle, child_id + 3u, "Forwarded", snapshot_header(*scene));
        const auto child_content = add_region(scene, frame.handle, child_id + 5u, "ForwardedContent",
                                              desired_content_height(*scene, child));
        if (!header.ok() || !child_content.ok())
            return false;
        child.reply = header.handle;
        child.content = child_content.handle;
        if (!message_rich_widgets_build(&scene->widgets, frame.handle, child_id, snapshot, &child))
            return false;
    }
    InviteLink invites[kMessageInviteCardMaximum];
    const u32 invite_count =
        record ? message_scene_invite_codes(*scene, *record, {invites, kMessageInviteCardMaximum}) : 0u;
    if (!message_rich_widgets_build(&scene->widgets, body.handle, id, rich, &row, invite_count)) {
        return false;
    }
    return apply_row_specs(scene, &row);
}
} // namespace message_scene_detail
} // namespace maicord

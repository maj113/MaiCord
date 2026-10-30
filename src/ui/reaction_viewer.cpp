#include "maicord/ui/reaction_viewer.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

inline ReactionViewerResult reject(ReactionViewerError error) noexcept {
    return ReactionViewerResult{error, false};
}

inline u8 format_count(u32 value, char* output, u32 capacity) noexcept {
    const u32 digits = decimal_digits(value);
    return digits > capacity ? u8{0} : static_cast<u8>(decimal_write(output, value));
}

inline bool range_in_page(const ReactionViewer& viewer, Utf8View value, u32 page_begin, u32 page_bytes,
                          u32* offset) noexcept {
    if ((!value.data && value.bytes != 0) || !viewer.text || page_begin > viewer.text_capacity ||
        page_bytes > viewer.text_capacity - page_begin) {
        return false;
    }
    if (value.bytes == 0) {
        *offset = 0;
        return true;
    }
    const char* begin = viewer.text + page_begin;
    const char* end = begin + page_bytes;
    if (value.data < begin || value.data > end || value.bytes > static_cast<u32>(end - value.data)) {
        return false;
    }
    *offset = static_cast<u32>(value.data - viewer.text);
    return true;
}


bool initialize_text_owner(ReactionViewer* viewer) noexcept {
    return ui_widget_manager_initialize_fill_root(
        &viewer->widgets, Slice<UiWidget>{viewer->widget_storage, 1},
        Slice<UiLayoutNode>{viewer->layout_storage, 1}, utf8_literal("ReactionViewer"),
        ui_layout_node_visible | ui_layout_node_allow_overlap, &viewer->text_owner);
}

} // namespace

bool reaction_viewer_initialize(ReactionViewer* viewer, Slice<ReactionViewerUser> users,
                                Slice<char> text) noexcept {
    if (!viewer || !users.data || users.count == 0 || users.count > kReactionViewerUserCapacity ||
        !text.data || text.count == 0 || text.count > kReactionViewerTextCapacity) {
        return false;
    }
    reset_value<ReactionViewer>(viewer);
    viewer->users = users.data;
    viewer->user_capacity = users.count;
    viewer->text = text.data;
    viewer->text_capacity = text.count;
    viewer->selected_aggregate = kReactionViewerNoIndex;
    viewer->hovered_user = kReactionViewerNoIndex;
    viewer->hovered_aggregate = kReactionViewerNoIndex;
    if (!initialize_text_owner(viewer)) {
        reset_value<ReactionViewer>(viewer);
        return false;
    }
    for (u32 index = 0; index < users.count; ++index) {
        reset_value<ReactionViewerUser>(&users[index]);
    }
    return true;
}

bool reaction_viewer_layout(ReactionViewer* viewer, UiRect viewport, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!viewer || !ui_rect_has_area(viewport)) {
        return false;
    }
    const float available_width = ui_rect_width(viewport) - kReactionViewerViewportMargin * 2.0f;
    const float available_height = ui_rect_height(viewport) - kReactionViewerViewportMargin * 2.0f;
    if (available_width < 280.0f || available_height < 240.0f) {
        return false;
    }
    const float width = minimum(kReactionViewerPreferredWidth, available_width);
    const float height = minimum(kReactionViewerPreferredHeight, available_height);
    const float left = viewport.left + (ui_rect_width(viewport) - width) * 0.5f;
    const float top = viewport.top + (ui_rect_height(viewport) - height) * 0.5f;
    const UiRect panel{left, top, left + width, top + height};
    const UiRect header{left, top, left + width, top + kReactionViewerHeaderHeight};
    const UiRect body{left, top + minimum(kReactionViewerBodyTop, height - 160.0f), left + width,
                      top + height - 20.0f};
    const UiRect close{panel.right - 48.0f, panel.top, panel.right - 8.0f, panel.top + 40.0f};
    const bool updated = !ui_rect_equal(viewer->viewport, viewport) ||
                         !ui_rect_equal(viewer->panel, panel) || !ui_rect_equal(viewer->body, body);
    viewer->viewport = viewport;
    viewer->panel = panel;
    viewer->header = header;
    viewer->body = body;
    viewer->close_button = close;
    if (viewer->active) {
        bool clamped = false;
        reaction_viewer_set_scroll(viewer, true, viewer->user_scroll, &clamped);
        reaction_viewer_set_scroll(viewer, false, viewer->aggregate_scroll, &clamped);
    }
    if (viewer->text_owner.manager &&
        !ui_widget_manager_layout_rectangle(&viewer->widgets, viewport).ok()) {
        return false;
    }
    saturating_increment_if(updated, viewer->revision);
    if (changed) {
        *changed = updated;
    }
    return true;
}

ReactionViewerResult reaction_viewer_open(ReactionViewer* viewer, u64 guild_id, u64 channel_id,
                                          u64 message_id, const MessageRichStore& rich_store,
                                          const MessageRichRecord& rich, UiRect viewport) noexcept {
    if (!viewer || !viewer->users || !viewer->text || channel_id == 0 || message_id == 0 ||
        !rich.occupied || rich.message_id != message_id) {
        return reject(ReactionViewerError::invalid_input);
    }
    if (rich.reaction_count == 0 || rich.reaction_count > kGatewayMessageReactionMaximum) {
        return reject(ReactionViewerError::no_reactions);
    }
    ReactionViewer candidate{};
    candidate.users = viewer->users;
    candidate.user_capacity = viewer->user_capacity;
    candidate.text = viewer->text;
    candidate.text_capacity = viewer->text_capacity;
    candidate.guild_id = guild_id;
    candidate.channel_id = channel_id;
    candidate.message_id = message_id;
    candidate.aggregate_count = rich.reaction_count;
    candidate.selected_aggregate = 0;
    candidate.hovered_user = kReactionViewerNoIndex;
    candidate.hovered_aggregate = kReactionViewerNoIndex;
    candidate.active = true;
    candidate.revision = viewer->revision;
    u32 name_used = 0;
    for (u32 index = 0; index < rich.reaction_count; ++index) {
        const MessageRichReaction& source = rich.reactions[index];
        const Utf8View name = message_rich_store_string(rich_store, source.name);
        if (!name.data || name.bytes == 0 || name.bytes > kMessageReactionMaximumEmojiNameBytes ||
            name.bytes > sizeof(candidate.aggregate_names) - name_used) {
            return reject(ReactionViewerError::capacity);
        }
        copy_bytes(candidate.aggregate_names + name_used, name.data, name.bytes);
        ReactionViewerAggregate& aggregate = candidate.aggregates[index];
        aggregate.emoji_id = source.emoji_id;
        aggregate.name_offset = name_used;
        aggregate.name_bytes = static_cast<u16>(name.bytes);
        aggregate.count = source.count;
        aggregate.type = source.normal_count != 0 ? MessageReactionType::normal
                                                  : MessageReactionType::burst;
        aggregate.query_count = source.normal_count != 0 ? source.normal_count : source.burst_count;
        aggregate.flags = (source.flags & 4u) != 0 ? 1u : 0u;
        aggregate.count_text_bytes = format_count(source.count, aggregate.count_text,
                                                  static_cast<u32>(sizeof(aggregate.count_text)));
        if (aggregate.query_count == 0 || aggregate.count_text_bytes == 0) {
            return reject(ReactionViewerError::invalid_input);
        }
        name_used += name.bytes;
    }
    candidate.expected_user_count = candidate.aggregates[0].query_count;
    if (!reaction_viewer_layout(&candidate, viewport)) {
        return reject(ReactionViewerError::layout);
    }
    saturating_increment(candidate.revision);
    *viewer = candidate;
    if (!initialize_text_owner(viewer) ||
        !ui_widget_manager_layout_rectangle(&viewer->widgets, viewport).ok()) {
        return reject(ReactionViewerError::layout);
    }
    return ReactionViewerResult{ReactionViewerError::none, true};
}

bool reaction_viewer_close(ReactionViewer* viewer, bool* changed) noexcept {
    if (changed) {
        *changed = false;
    }
    if (!viewer) {
        return false;
    }
    if (!viewer->active) {
        return true;
    }
    viewer->active = false;
    viewer->loading = false;
    viewer->hovered_user = kReactionViewerNoIndex;
    viewer->hovered_aggregate = kReactionViewerNoIndex;
    saturating_increment(viewer->revision);
    if (changed) {
        *changed = true;
    }
    return true;
}

ReactionViewerResult reaction_viewer_select_aggregate(ReactionViewer* viewer,
                                                      u32 aggregate_index) noexcept {
    if (!viewer || !viewer->active || aggregate_index >= viewer->aggregate_count) {
        return reject(ReactionViewerError::invalid_input);
    }
    if (viewer->selected_aggregate == aggregate_index) {
        return ReactionViewerResult{ReactionViewerError::none, false};
    }
    viewer->selected_aggregate = aggregate_index;
    viewer->user_count = 0;
    viewer->text_used = 0;
    viewer->after_user_id = 0;
    viewer->first_user = 0;
    viewer->user_scroll = 0;
    viewer->hovered_user = kReactionViewerNoIndex;
    viewer->expected_user_count = viewer->aggregates[aggregate_index].query_count;
    viewer->loading = false;
    viewer->complete = false;
    viewer->failed = false;
    viewer->truncated = false;
    saturating_increment(viewer->revision);
    return ReactionViewerResult{ReactionViewerError::none, true};
}

Slice<char> reaction_viewer_page_text_output(ReactionViewer& viewer) noexcept {
    return viewer.active && viewer.text && viewer.text_used <= viewer.text_capacity
               ? Slice<char>{viewer.text + viewer.text_used, viewer.text_capacity - viewer.text_used}
               : Slice<char>{};
}

ReactionViewerResult reaction_viewer_accept_page(ReactionViewer* viewer,
                                                 ConstSlice<ReactionUser> users,
                                                 u32 text_bytes) noexcept {
    if (!viewer || !viewer->active || !users.data || users.count > kReactionUsersMaximumPage ||
        viewer->selected_aggregate >= viewer->aggregate_count ||
        viewer->user_count > viewer->user_capacity ||
        users.count > viewer->user_capacity - viewer->user_count ||
        viewer->text_used > viewer->text_capacity ||
        text_bytes > viewer->text_capacity - viewer->text_used) {
        return reject(ReactionViewerError::capacity);
    }
    const u32 page_begin = viewer->text_used;
    for (u32 index = 0; index < users.count; ++index) {
        const ReactionUser& source = users[index];
        ReactionViewerUser destination{};
        destination.id = source.id;
        if (source.id == 0 || source.username.bytes == 0 || source.display_name.bytes == 0 ||
            source.username.bytes > 0xFFFFu || source.display_name.bytes > 0xFFFFu ||
            source.avatar_hash.bytes > sizeof(destination.avatar_hash) ||
            !range_in_page(*viewer, source.username, page_begin, text_bytes,
                           &destination.username_offset) ||
            !range_in_page(*viewer, source.display_name, page_begin, text_bytes,
                           &destination.display_name_offset)) {
            return reject(ReactionViewerError::stale_page);
        }
        destination.username_bytes = static_cast<u16>(source.username.bytes);
        destination.display_name_bytes = static_cast<u16>(source.display_name.bytes);
        destination.avatar_hash_bytes = static_cast<u8>(source.avatar_hash.bytes);
        destination.flags = source.bot ? reaction_viewer_user_bot : 0;
        copy_bytes(destination.avatar_hash, source.avatar_hash.data, source.avatar_hash.bytes);
        viewer->users[viewer->user_count + index] = destination;
    }
    viewer->text_used += text_bytes;
    viewer->user_count += users.count;
    viewer->after_user_id = users.count != 0 ? users[users.count - 1].id : viewer->after_user_id;
    viewer->loading = false;
    viewer->failed = false;
    viewer->complete = users.count < kReactionUsersMaximumPage ||
                       viewer->user_count >= viewer->expected_user_count ||
                       viewer->user_count >= viewer->user_capacity;
    viewer->truncated =
        viewer->user_count >= viewer->user_capacity && viewer->user_count < viewer->expected_user_count;
    saturating_increment(viewer->revision);
    return ReactionViewerResult{ReactionViewerError::none, true};
}

void reaction_viewer_set_loading(ReactionViewer* viewer, bool loading) noexcept {
    if (!viewer || !viewer->active || viewer->loading == loading) {
        return;
    }
    viewer->loading = loading;
    if (loading) {
        viewer->failed = false;
    }
    saturating_increment(viewer->revision);
}

void reaction_viewer_set_failed(ReactionViewer* viewer) noexcept {
    if (!viewer || !viewer->active) {
        return;
    }
    viewer->loading = false;
    viewer->failed = true;
    saturating_increment(viewer->revision);
}

bool reaction_viewer_request_more(ReactionViewer* viewer, bool* requested) noexcept {
    if (!viewer || !requested) {
        return false;
    }
    *requested = false;
    if (!viewer->active || viewer->loading || viewer->failed || viewer->complete) {
        return true;
    }
    if (viewer->user_count >= viewer->user_capacity) {
        viewer->complete = true;
        viewer->truncated = viewer->user_count < viewer->expected_user_count;
        saturating_increment(viewer->revision);
        return true;
    }
    const u32 visible = reaction_viewer_visible_user_count(*viewer);
    if (viewer->user_count == 0 || viewer->first_user + visible + 2 >= viewer->user_count) {
        *requested = true;
    }
    return true;
}

float reaction_viewer_scroll_maximum(const ReactionViewer& viewer, bool users) noexcept {
    const float content = users ? viewer.user_count * kReactionViewerUserRowHeight + 4.0f
                                : viewer.aggregate_count * kReactionViewerAggregateRowHeight;
    return maximum(0.0f, content - ui_rect_height(viewer.body));
}

bool reaction_viewer_set_scroll(ReactionViewer* viewer, bool users, float offset, bool* changed) noexcept {
    if (!viewer || !changed || !viewer->active || !ui_coordinate_valid(offset)) return false;
    float& position = users ? viewer->user_scroll : viewer->aggregate_scroll;
    const float next = minimum(maximum(0.0f, offset), reaction_viewer_scroll_maximum(*viewer, users));
    *changed = next != position;
    position = next;
    (users ? viewer->first_user : viewer->first_aggregate) =
        static_cast<u32>(next / (users ? kReactionViewerUserRowHeight : kReactionViewerAggregateRowHeight));
    if (*changed) {
        (users ? viewer->hovered_user : viewer->hovered_aggregate) = kReactionViewerNoIndex;
        saturating_increment(viewer->revision);
    }
    return true;
}

bool reaction_viewer_scroll_users(ReactionViewer* viewer, i32 rows, bool* changed) noexcept {
    return viewer && reaction_viewer_set_scroll(viewer, true,
        viewer->user_scroll + rows * kReactionViewerUserRowHeight, changed);
}

u32 reaction_viewer_visible_user_count(const ReactionViewer& viewer) noexcept {
    const float height = ui_rect_height(viewer.body) - 4.0f;
    return height > 0.0f ? maximum(1u, static_cast<u32>(height / kReactionViewerUserRowHeight)) : 0;
}

u32 reaction_viewer_visible_aggregate_count(const ReactionViewer& viewer) noexcept {
    const float height = ui_rect_height(viewer.body);
    return height > 0.0f ? maximum(1u, static_cast<u32>(height / kReactionViewerAggregateRowHeight)) : 0;
}

UiRect reaction_viewer_user_rectangle(const ReactionViewer& viewer, u32 user_index) noexcept {
    if (!viewer.active || user_index < viewer.first_user || user_index >= viewer.user_count) {
        return UiRect{};
    }
    const u32 visible_index = user_index - viewer.first_user;
    if (visible_index > reaction_viewer_visible_user_count(viewer) + 1) {
        return UiRect{};
    }
    const float narrow = ui_rect_width(viewer.panel) < 400.0f ? 112.0f : 138.0f;
    const float top =
        viewer.body.top + 4.0f + static_cast<float>(user_index) * kReactionViewerUserRowHeight - viewer.user_scroll;
    if (top >= viewer.body.bottom) return UiRect{};
    return UiRect{viewer.panel.left + narrow, top, viewer.panel.right - 24.0f,
                  top + kReactionViewerUserRowHeight - 8.0f};
}

UiRect reaction_viewer_aggregate_rectangle(const ReactionViewer& viewer, u32 aggregate_index) noexcept {
    if (!viewer.active || aggregate_index < viewer.first_aggregate ||
        aggregate_index >= viewer.aggregate_count) {
        return UiRect{};
    }
    const u32 visible_index = aggregate_index - viewer.first_aggregate;
    if (visible_index > reaction_viewer_visible_aggregate_count(viewer) + 1) {
        return UiRect{};
    }
    const float width = ui_rect_width(viewer.panel) < 400.0f ? 72.0f : 80.0f;
    const float top =
        viewer.body.top + static_cast<float>(aggregate_index) * kReactionViewerAggregateRowHeight - viewer.aggregate_scroll;
    if (top >= viewer.body.bottom) return UiRect{};
    return UiRect{viewer.panel.left + 24.0f, top, viewer.panel.left + 24.0f + width, top + 32.0f};
}

ReactionViewerHit reaction_viewer_hit_test(const ReactionViewer& viewer, float x, float y) noexcept {
    if (!viewer.active || !ui_rect_contains(viewer.viewport, x, y)) {
        return ReactionViewerHit{};
    }
    if (!ui_rect_contains(viewer.panel, x, y)) {
        return ReactionViewerHit{viewer.viewport, kReactionViewerNoIndex, ReactionViewerHitKind::outside};
    }
    if (ui_rect_contains(viewer.close_button, x, y)) {
        return ReactionViewerHit{viewer.close_button, kReactionViewerNoIndex, ReactionViewerHitKind::close};
    }
    if (!ui_rect_contains(viewer.body, x, y))
        return ReactionViewerHit{viewer.panel, kReactionViewerNoIndex, ReactionViewerHitKind::panel};
    for (u32 index = viewer.first_aggregate; index < viewer.aggregate_count; ++index) {
        const UiRect rectangle = reaction_viewer_aggregate_rectangle(viewer, index);
        if (!ui_rect_has_area(rectangle)) {
            break;
        }
        if (ui_rect_contains(rectangle, x, y)) {
            return ReactionViewerHit{rectangle, index, ReactionViewerHitKind::aggregate};
        }
    }
    for (u32 index = viewer.first_user; index < viewer.user_count; ++index) {
        const UiRect rectangle = reaction_viewer_user_rectangle(viewer, index);
        if (!ui_rect_has_area(rectangle)) {
            break;
        }
        if (ui_rect_contains(rectangle, x, y)) {
            return ReactionViewerHit{rectangle, index, ReactionViewerHitKind::user};
        }
    }
    return ReactionViewerHit{viewer.panel, kReactionViewerNoIndex, ReactionViewerHitKind::panel};
}

bool reaction_viewer_set_hover(ReactionViewer* viewer, float x, float y, bool* changed) noexcept {
    if (!viewer || !changed || !viewer->active) {
        return false;
    }
    const ReactionViewerHit hit = reaction_viewer_hit_test(*viewer, x, y);
    const u32 user = hit.kind == ReactionViewerHitKind::user ? hit.index : kReactionViewerNoIndex;
    const u32 aggregate = hit.kind == ReactionViewerHitKind::aggregate ? hit.index : kReactionViewerNoIndex;
    *changed = viewer->hovered_user != user || viewer->hovered_aggregate != aggregate;
    if (*changed) {
        viewer->hovered_user = user;
        viewer->hovered_aggregate = aggregate;
        saturating_increment(viewer->revision);
    }
    return true;
}

Utf8View reaction_viewer_aggregate_name(const ReactionViewer& viewer, u32 aggregate_index) noexcept {
    if (aggregate_index >= viewer.aggregate_count) {
        return Utf8View{};
    }
    const ReactionViewerAggregate& aggregate = viewer.aggregates[aggregate_index];
    return aggregate.name_offset + aggregate.name_bytes <= sizeof(viewer.aggregate_names)
               ? Utf8View{viewer.aggregate_names + aggregate.name_offset, aggregate.name_bytes}
               : Utf8View{};
}

Utf8View reaction_viewer_user_name(const ReactionViewer& viewer, const ReactionViewerUser& user) noexcept {
    return viewer.text && user.display_name_bytes != 0 &&
                   user.display_name_offset + user.display_name_bytes <= viewer.text_used
               ? Utf8View{viewer.text + user.display_name_offset, user.display_name_bytes}
               : Utf8View{};
}

Utf8View reaction_viewer_user_username(const ReactionViewer& viewer,
                                       const ReactionViewerUser& user) noexcept {
    return viewer.text && user.username_bytes != 0 &&
                   user.username_offset + user.username_bytes <= viewer.text_used
               ? Utf8View{viewer.text + user.username_offset, user.username_bytes}
               : Utf8View{};
}

Utf8View reaction_viewer_user_avatar_hash(const ReactionViewerUser& user) noexcept {
    return user.avatar_hash_bytes <= sizeof(user.avatar_hash)
               ? Utf8View{user.avatar_hash, user.avatar_hash_bytes}
               : Utf8View{};
}

} // namespace maicord

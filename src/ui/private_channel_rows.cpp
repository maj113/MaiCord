#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/activity_status.hpp"
#include "maicord/ui/profile_activity.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/model/private_channel_store.hpp"

namespace maicord {
namespace {
const PrivateChannelRecord* record(const ChatStateStore& state, u64 channel_id,
                                          const ChannelRecord** channel) noexcept {
    *channel = chat_state_store_find_channel(state, channel_id);
    return *channel && channel_is_private((*channel)->type) && state.private_channels
               ? private_channel_store_find(*state.private_channels, channel_id)
               : nullptr;
}
// The other member of a one-to-one conversation, or zero.
u64 private_row_peer(const UiPrivateRowSource& source, u64 channel_id) noexcept {
    const ChannelRecord* channel = nullptr;
    if (!source.state || !record(*source.state, channel_id, &channel) || channel->type != 1) return 0;
    u64 peer[2]{};
    return ui_private_row_group_members(*source.state, channel_id, peer) ? peer[0] : 0;
}
// The peer is online on mobile only; the caller knows the row is online.
bool mobile_peer(const UiPrivateRowSource& source, u64 channel_id) noexcept {
    return source.users &&
           client_status_mobile_online(user_store_client_status(*source.users, private_row_peer(source, channel_id)));
}
} // namespace

UiPresence ui_user_presence(const UiPrivateRowSource& source, u64 user) noexcept {
    const auto* relation = source.relationships && user ? relationship_store_find(*source.relationships, user) : nullptr;
    if (relation && relation->type == RelationshipType::friend_ && relation->presence != RelationshipPresence::unknown) {
        switch (relation->presence) {
        case RelationshipPresence::online: return UiPresence::online;
        case RelationshipPresence::idle: return UiPresence::idle;
        case RelationshipPresence::dnd: return UiPresence::busy;
        default: return UiPresence::offline;
        }
    }
    // Everyone else: the latest presence of any scope (a guild presence of an
    // open DM's recipient included) left its platforms on the user record.
    switch (source.users && user ? client_status_overall(user_store_client_status(*source.users, user))
                                 : ClientPlatformStatus::offline) {
    case ClientPlatformStatus::online: return UiPresence::online;
    case ClientPlatformStatus::idle: return UiPresence::idle;
    case ClientPlatformStatus::dnd: return UiPresence::busy;
    default: return UiPresence::offline;
    }
}
UiPrivateRowStatus ui_private_row_status(const UiPrivateRowSource& source, u64 channel_id) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* metadata = source.state ? record(*source.state, channel_id, &channel) : nullptr;
    if (metadata && channel->type == 3) {
        // As in the DM list: a group is online (and nothing else) while
        // any other member's own status is online.
        const auto recipients = private_channel_store_recipients(*source.state->private_channels, *metadata);
        for (u32 i = 0; i < recipients.count; ++i)
            if (recipients[i] != source.state->self_user_id &&
                ui_user_presence(source, recipients[i]) == UiPresence::online)
                return UiPrivateRowStatus::online;
        return UiPrivateRowStatus::none;
    }
    const u64 peer = private_row_peer(source, channel_id);
    if (!peer) return UiPrivateRowStatus::none;
    switch (ui_user_presence(source, peer)) {
    case UiPresence::online: return UiPrivateRowStatus::online;
    case UiPresence::idle: return UiPrivateRowStatus::idle;
    case UiPresence::busy: return UiPrivateRowStatus::dnd;
    default: return UiPrivateRowStatus::offline;
    }
}
bool ui_private_row_mobile(const UiPrivateRowSource& source, u64 channel_id) noexcept {
    return ui_private_row_status(source, channel_id) == UiPrivateRowStatus::online && mobile_peer(source, channel_id);
}
Utf8View ui_private_peer_activity(const UiPrivateRowSource& source, u64 channel_id, Slice<char> storage,
                                  UiActivityGlyph* glyph) noexcept {
    if (glyph) *glyph = UiActivityGlyph::none;
    const ChannelRecord* channel = nullptr;
    if (!source.state || !source.users || !storage.data || !record(*source.state, channel_id, &channel) ||
        channel->type != 1)
        return {};
    // The online peer's activity line (activity_status.hpp).
    u64 peer[2]{};
    if (!ui_private_row_group_members(*source.state, channel_id, peer) ||
        ui_private_row_status(source, channel_id) == UiPrivateRowStatus::offline)
        return {};
    char line[kAppearanceActivityBytes];
    UiActivityStatus status{};
    if (!ui_user_activity_status(*source.users, peer[0], user_appearance_find_activity(source.users->appearance, peer[0]),
                                 {line, sizeof(line)}, &status))
        return {};
    if (glyph) *glyph = status.glyph;
    u32 used = 0;
    if (status.emoji.bytes && status.emoji.bytes + 1u <= static_cast<u32>(storage.count)) {
        copy_bytes(storage.data, status.emoji.data, status.emoji.bytes);
        used = status.emoji.bytes;
        if (status.text.bytes) storage.data[used++] = ' ';
    }
    const u32 text = utf8_truncated_length(status.text, static_cast<u32>(storage.count) - used);
    copy_bytes(storage.data + used, status.text.data, text);
    return {storage.data, used + text};
}
Utf8View ui_private_row_subtitle(const UiPrivateRowSource& source, u64 channel_id, Slice<char> storage,
                                 UiActivityGlyph* glyph) noexcept {
    if (glyph) *glyph = UiActivityGlyph::none;
    const ChannelRecord* channel = nullptr;
    const auto* metadata = source.state ? record(*source.state, channel_id, &channel) : nullptr;
    if (!metadata) return {};
    if (source.typing && typing_store_channel_count(*source.typing, channel_id, source.state->self_user_id))
        return utf8_literal("Typing…");
    if (metadata->request_flags & private_request_spam) return utf8_literal("Possible spam");
    if (metadata->request_flags & private_request_pending) return utf8_literal("Message request");
    if (channel->type == 1) return ui_private_peer_activity(source, channel_id, storage, glyph);
    if (channel->type != 3 || !storage.data) return {};
    const auto recipients = private_channel_store_recipients(*source.state->private_channels, *metadata);
    u64 members = 1;
    for (u32 i = 0; i < recipients.count; ++i) members += recipients[i] != source.state->self_user_id;
    const Utf8View suffix = members == 1 ? utf8_literal(" member") : utf8_literal(" members");
    char digits[20];
    const u32 digit_count = decimal_write(digits, members);
    if (storage.count < digit_count + suffix.bytes) return {};
    copy_bytes(storage.data, digits, digit_count);
    copy_bytes(storage.data + digit_count, suffix.data, suffix.bytes);
    return {storage.data, digit_count + suffix.bytes};
}
u32 ui_private_row_group_members(const ChatStateStore& state, u64 channel_id, u64 output[2]) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* metadata = record(state, channel_id, &channel);
    if (!metadata) return 0;
    const auto recipients = private_channel_store_recipients(*state.private_channels, *metadata);
    u32 count = 0;
    for (u32 i = 0; i < recipients.count && count < 2; ++i)
        if (recipients[i] != state.self_user_id) output[count++] = recipients[i];
    return count;
}
UiRect ui_private_row_subtitle_rectangle(UiRect label) noexcept {
    const float middle = (label.top + label.bottom) * .5f;
    return {label.left, middle + 1.f, label.right, middle + 17.f};
}
UiRect ui_private_row_status_rectangle(UiRect avatar) noexcept {
    const float size = ui_rect_width(avatar) * .3125f;
    return {avatar.right - size, avatar.bottom - size, avatar.right, avatar.bottom};
}
UiRect ui_private_row_close_rectangle(UiRect row) noexcept {
    const float middle = (row.top + row.bottom) * .5f;
    return {row.right - 34.f, middle - 8.f, row.right - 18.f, middle + 8.f};
}
UiRect ui_private_row_group_avatar(UiRect avatar, u32 index) noexcept {
    const float size = ui_rect_width(avatar) * .625f;
    return index ? UiRect{avatar.right - size, avatar.bottom - size, avatar.right, avatar.bottom}
                 : UiRect{avatar.left, avatar.top, avatar.left + size, avatar.top + size};
}
Utf8View ui_private_row_custom_icon(const ChatStateStore& state, u64 channel_id) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* metadata = record(state, channel_id, &channel);
    if (!metadata || channel->type != 3 || !metadata->icon_bytes || metadata->icon_bytes > sizeof(metadata->icon))
        return {};
    const Utf8View hash{metadata->icon, metadata->icon_bytes};
    // Same hash-variant identity as avatars and guild icons.
    return user_avatar_variant(hash) ? hash : Utf8View{};
}
u8 ui_private_avatar_shape(const ChatStateStore* state, u64 channel_id, u8 type) noexcept {
    return type == 3 && state && ui_private_row_custom_icon(*state, channel_id).bytes ? 1 : type;
}
namespace {
UiPresence presence(UiPrivateRowStatus status) noexcept {
    return status == UiPrivateRowStatus::online ? UiPresence::online
           : status == UiPrivateRowStatus::idle ? UiPresence::idle
           : status == UiPrivateRowStatus::dnd  ? UiPresence::busy
                                                : UiPresence::offline;
}
bool ring(PaintStream* stream, UiRect r, Color color) noexcept {
    return paint_stream_push_rounded_border(stream, r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, Color{0, 0, 0, 0},
                                            color, ui_rect_width(r) * .5f + 2.f, 2);
}
// Cutouts repeat the row highlight so the ring matches the composited surface.
// Only a conversation row shows a group's online dot (not the header).
bool decorations(PaintStream* stream, const UiPrivateRowSource& source, u64 channel_id, u8 type, UiRect avatar,
                 Color surface, const Color* highlight, const Theme& theme, bool row) noexcept {
    if (type == 3) {
        // The generated icon: the front avatar's cut, then an online group's
        // dot at the usual corner.
        const auto front = ui_private_row_group_avatar(avatar, 1);
        if (!ring(stream, front, surface) || (highlight && !ring(stream, front, *highlight)))
            return false;
        if (!row || ui_private_row_status(source, channel_id) != UiPrivateRowStatus::online)
            return true;
        const auto dot = ui_private_row_status_rectangle(avatar);
        return ui_presence_paint(stream, dot, UiPresence::online, surface, theme) &&
               (!highlight || ring(stream, dot, *highlight));
    }
    const auto status = ui_private_row_status(source, channel_id);
    if (status == UiPrivateRowStatus::none || (!row && !private_row_peer(source, channel_id)))
        return true;
    const auto dot = ui_private_row_status_rectangle(avatar);
    if (status == UiPrivateRowStatus::online && mobile_peer(source, channel_id)) {
        const auto phone = ui_presence_mobile_geometry(dot, 2.f);
        return ui_presence_paint(stream, dot, UiPresence::online, surface, theme, true) &&
               (!highlight || paint_stream_push_rounded_border(stream, phone.cutout, Color{0, 0, 0, 0}, *highlight, phone.cutout_radius, 2));
    }
    return ui_presence_paint(stream, dot, presence(status), surface, theme) &&
           (!highlight || ring(stream, dot, *highlight));
}
} // namespace
bool ui_private_avatar_placeholder_paint(PaintStream* stream, u8 type, UiRect avatar, const Theme& theme) noexcept {
    if (type != 3)
        return paint_stream_push_rounded(stream, avatar, theme.avatar_placeholder, ui_rect_width(avatar) * .5f);
    for (u32 i = 0; i < 2; ++i) {
        const auto r = ui_private_row_group_avatar(avatar, i);
        if (!paint_stream_push_rounded(stream, r, theme.avatar_placeholder, ui_rect_width(r) * .5f))
            return false;
    }
    return true;
}
bool ui_private_rows_overlay_paint(const ShellNavigationScene& scene, UiRect clip, const UiPrivateRowSource& source,
                                   u64 selected_channel_id, const Theme& theme, PaintStream* stream) noexcept {
    if (!scene.active || !source.state || !ui_rect_has_area(clip)) return true;
    if (!paint_stream_push_clip(stream, clip)) return false;
    const auto* hovered = shell_navigation_scene_resolve(scene, scene.widgets.hovered);
    bool ok = true;
    for (u32 i = 0; ok && i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        if (!channel_is_private(row.view.type)) continue;
        const auto* layout = shell_navigation_scene_layout_node(scene, row);
        const auto* icon = ui_widget_manager_layout_node(scene.widgets, row.icon);
        if (!layout || !icon || !ui_rect_intersects(layout->rectangle, clip)) continue;
        const bool selected = selected_channel_id ? row.view.id == selected_channel_id : row.view.selected;
        const bool over = hovered == &row;
        const Color highlight = ui_background_hover_color(theme, selected ? theme.interactive : theme.surface_high,
                                                          selected ? .28f : .20f);
        ok = decorations(stream, source, row.view.id, ui_private_avatar_shape(source.state, row.view.id, row.view.type),
                         icon->rectangle, theme.base_lower,
                         selected || over ? &highlight : nullptr, theme, true);
        // The activity glyph leads the subtitle line (shell_text_presentation).
        UiActivityGlyph glyph = UiActivityGlyph::none;
        char subtitle[192];
        const auto* label = ui_widget_manager_layout_node(scene.widgets, row.label);
        if (ok && label && row.view.type == 1 &&
            ui_private_row_subtitle(source, row.view.id, {subtitle, sizeof(subtitle)}, &glyph).bytes &&
            glyph != UiActivityGlyph::none)
            ok = ui_icon_paint(stream, ui_activity_glyph_icon(glyph),
                               ui_activity_glyph_rectangle(ui_private_row_subtitle_rectangle(label->content_rectangle)),
                               theme.tokens.status.online);
        if (ok && over && !row.view.mention_count)
            ok = ui_icon_paint(stream, UiIcon::close, ui_private_row_close_rectangle(layout->rectangle),
                               theme.text_secondary);
    }
    return paint_stream_pop_clip(stream) && ok;
}
UiRect ui_private_header_avatar(const UiScene& scene) noexcept {
    const auto* title = ui_scene_node(scene, UiSemantic::title_bar);
    const auto* label = ui_scene_node(scene, UiSemantic::title_label);
    if (!title || !label) return {};
    const float middle = (title->rectangle.top + title->rectangle.bottom) * .5f;
    return {label->rectangle.left, middle - kUiPrivateHeaderAvatar * .5f,
            label->rectangle.left + kUiPrivateHeaderAvatar, middle + kUiPrivateHeaderAvatar * .5f};
}
bool ui_private_header_overlay_paint(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                     const Theme& theme, PaintStream* stream) noexcept {
    const auto* channel = source.state ? chat_state_store_find_channel(*source.state, channel_id) : nullptr;
    const auto avatar = ui_private_header_avatar(scene);
    if (!channel || !channel_is_private(channel->type) || !ui_rect_has_area(avatar)) return true;
    if (!decorations(stream, source, channel_id, ui_private_avatar_shape(source.state, channel_id, channel->type),
                     avatar, theme.base_lower, nullptr, theme, false))
        return false;
    char line[192];
    UiPrivateHeaderActivity activity{};
    return !ui_private_header_activity(scene, source, channel_id, {line, sizeof(line)}, &activity) ||
           activity.glyph == UiActivityGlyph::none ||
           ui_icon_paint(stream, ui_activity_glyph_icon(activity.glyph), activity.icon, theme.text_secondary);
}
namespace {
// The header's line: the first activity by the official priority (competing,
// streaming, playing, then the rest; rich presence, then the newer first),
// and "+N" for the other distinct activities while no custom status text
// takes the line. Custom status is never counted.
Utf8View header_line(const UiPrivateRowSource& source, u64 channel_id, Slice<char> storage,
                     UiActivityGlyph* glyph) noexcept {
    const Utf8View plain = ui_private_peer_activity(source, channel_id, storage, glyph);
    u64 peer[2]{};
    UserActivitySet set{};
    if (!plain.bytes || !ui_private_row_group_members(*source.state, channel_id, peer) ||
        !user_activity_find(source.users->activities, peer[0], &set))
        return plain;
    u8 order[kUserActivityMaximum];
    const u32 ordered = profile_activity_order(set, {order, kUserActivityMaximum});
    u32 count = 0;
    for (u32 i = 0; i < ordered; ++i)
        if (set.items[order[i]].type != UserActivityType::custom) order[count++] = order[i];
    if (!count) return plain;
    const auto* global = user_appearance_find(source.users->appearance, peer[0]);
    char text[kAppearanceActivityBytes];
    UiActivityStatus status{};
    if (!ui_activity_status(&set.items[order[0]], user_appearance_find_activity(source.users->appearance, peer[0]),
                            global && global->bot, {text, sizeof(text)}, &status))
        return plain;
    *glyph = status.glyph;
    u32 used = 0;
    const auto put = [&](Utf8View value) noexcept {
        const u32 bytes = utf8_truncated_length(value, static_cast<u32>(storage.count) - used);
        copy_bytes(storage.data + used, value.data, bytes);
        used += bytes;
    };
    if (status.emoji.bytes) {
        put(status.emoji);
        if (status.text.bytes) put(utf8_literal(" "));
    }
    put(status.text);
    if (count > 1 && status.text.data == text && !(global && global->bot)) {
        char digits[24] = {' ', '+'};
        put({digits, 2 + decimal_write(digits + 2, count - 1)});
    }
    return {storage.data, used};
}
} // namespace
bool ui_private_header_activity(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                Slice<char> storage, UiPrivateHeaderActivity* output) noexcept {
    reset_value(output);
    const auto* title = ui_scene_node(scene, UiSemantic::title_bar);
    const auto* label = ui_scene_node(scene, UiSemantic::title_label);
    const auto* channel = source.state ? chat_state_store_find_channel(*source.state, channel_id) : nullptr;
    if (!title || !label || !channel || channel->type != 1 || !source.measure || scene.view != UiShellView::private_channels)
        return false;
    output->line = header_line(source, channel_id, storage, &output->glyph);
    if (!output->line.bytes) return false;
    // The name as the header shows it (shell_text_presentation): after the avatar, one line.
    char name_storage[512];
    const Utf8View name = private_channel_title(*source.state, source.users, *channel, {name_storage, sizeof(name_storage)});
    const float left = label->rectangle.left + kUiPrivateHeaderAvatar + 8.f;
    const float room = label->rectangle.right - left;
    UiTextMeasureValue measured{};
    if (!name.bytes || room <= 0 ||
        !ui_text_measure_cache_measure(source.measure,
                                       {{0x484E414D, hash64_bytes(kHash64Seed, name.data, name.bytes), 0, 0, 0},
                                        name,
                                        {},
                                        {room, 64},
                                        true,
                                        UiTextFace::normal},
                                       &measured)
             .ok())
        return false;
    const float start = left + minimum(room, measured.size.width) + kUiPrivateHeaderActivityGap;
    if (label->rectangle.right - start < kUiPrivateHeaderActivityMinimum) return false;
    const UiRect line{start, title->rectangle.top, label->rectangle.right, title->rectangle.bottom};
    output->icon = output->glyph != UiActivityGlyph::none ? ui_activity_glyph_rectangle(line) : UiRect{};
    output->text = ui_activity_glyph_text(line, output->glyph);
    return ui_rect_width(output->text) > 0;
}
UiRect ui_private_member_crown_rectangle(UiRect label, float name_width) noexcept {
    const float middle = (label.top + label.bottom) * .5f, half = kUiPrivateMemberCrownExtent * .5f;
    const float room = ui_rect_width(label) - kUiPrivateMemberCrownExtent - kUiPrivateMemberCrownGap;
    const float left = label.left + maximum(0.f, minimum(name_width, room)) + kUiPrivateMemberCrownGap;
    return {left, middle - half, left + kUiPrivateMemberCrownExtent, middle + half};
}
bool ui_private_member_owner_paint(const MemberListScene& scene, const ChatStateStore& state, const Theme& theme,
                                   PaintStream* stream) noexcept {
    if (!scene.active || !scene.source || scene.source->guild_id || !state.private_channels) return true;
    const auto* channel = chat_state_store_find_channel(state, scene.source->list_id);
    const auto* record = channel && channel->type == 3
                             ? private_channel_store_find(*state.private_channels, channel->id)
                             : nullptr;
    if (!record || !record->owner_id) return true;
    if (!paint_stream_push_clip(stream, scene.viewport))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        if (!member_list_scene_row_resident(scene, i) || row.kind != MemberListRowKind::member ||
            row.user_id != record->owner_id)
            continue;
        const auto* label = member_list_scene_layout_node(scene, row.label);
        if (!label)
            continue;
        // The crown follows the name; an unmeasured name keeps it at the line's end.
        const auto line = label->content_rectangle;
        float name_width = ui_rect_width(line);
        if (scene.measure && scene.members && scene.users) {
            const auto name = member_list_row_label(row.source_row, *scene.members, *scene.users, state);
            UiTextMeasureValue measured{};
            if (name.bytes &&
                ui_text_measure_cache_measure(scene.measure,
                                              {{0x4e414d45, hash64_bytes(kHash64Seed, name.data, name.bytes), 0, 0x43, 0},
                                               name,
                                               {},
                                               {maximum(1.f, ui_rect_width(line)), 40},
                                               true,
                                               UiTextFace::normal},
                                              &measured)
                    .ok())
                name_width = measured.size.width;
        }
        ok = ui_icon_paint(stream, UiIcon::crown, ui_private_member_crown_rectangle(line, name_width),
                           theme.tokens.accent.warning);
    }
    return paint_stream_pop_clip(stream) && ok;
}
} // namespace maicord

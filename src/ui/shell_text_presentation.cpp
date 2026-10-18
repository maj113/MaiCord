#include "maicord/ui/badge.hpp"
#include "maicord/model/stage_instance_store.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/ui/role_color.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/ui/navigation_foreground.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/voice_panel.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/plugin_slot_surfaces.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"
#include "maicord/ui/composer_voice_bar.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"

namespace maicord {
namespace {

bool append_command(const UiWidgetManager& manager, UiWidgetHandle owner, Utf8View text,
                    const UiRect& rectangle, const UiRect& clip, const Color& color, UiTextOutput* stream,
                    UiTextAlignment horizontal_alignment = UiTextAlignment::start,
                    UiTextAlignment vertical_alignment = UiTextAlignment::start,
                    UiTextWrapping wrapping = UiTextWrapping::no_wrap,
                    UiTextOverflow overflow = UiTextOverflow::clip, u32 style_flags = text_style_none,
                    bool owned = false, UiTextFace face = UiTextFace::normal,
                    UiTextEffect effect = {}) noexcept {
    if (text.bytes == 0) {
        return true;
    }
    const UiTextCommand command{text,
                                UiWidgetReference{&manager, owner},
                                rectangle,
                                clip,
                                color,
                                style_flags,
                                horizontal_alignment,
                                vertical_alignment,
                                wrapping,
                                overflow,
                                face,
                                effect};
    return owned ? ui_text_output_push_owned(stream, command) : ui_text_output_push(stream, command);
}

// The server acronym: the first character of each word ("MaiCord Lab"
// -> "ML"). The length selects a smaller face for longer acronyms.
Utf8View guild_acronym(Utf8View name, Slice<char> storage, u32* characters) noexcept {
    u32 used = 0;
    *characters = 0;
    bool boundary = true;
    for (u32 offset = 0; offset < name.bytes && *characters < 6;) {
        const auto scalar = utf8_decode_one(name.data + offset, name.bytes - offset);
        if (!scalar.valid)
            break;
        const bool space = scalar.codepoint == ' ' || scalar.codepoint == '\t';
        if (boundary && !space) {
            if (used + scalar.width > storage.count)
                break;
            copy_bytes(storage.data + used, name.data + offset, scalar.width);
            used += scalar.width;
            ++*characters;
        }
        boundary = space;
        offset += scalar.width;
    }
    return {storage.data, used};
}

void reject_atomically(UiTextOutput* stream) noexcept {
    if (!stream) {
        return;
    }
    ui_text_output_reject(stream);
}

Color packed_role_color(u32 packed, const Theme& theme) noexcept {
    if (packed == 0) {
        return theme.text_primary;
    }
    return ui_role_color(packed);
}

// The member's enhanced role style (gradient or holographic name), when its
// colour role has one and still matches the row's published colour.
UiTextEffect member_role_effect(const MemberListScene& scene, const MemberListRow& row,
                                const Theme& theme) noexcept {
    if (!row.role_color || !scene.members || !scene.state || !scene.source)
        return {};
    const auto* member = guild_member_store_find(*scene.members, scene.source->guild_id, row.user_id);
    const auto* role = member && member->color_role_id
                           ? chat_state_store_find_role(*scene.state, scene.source->guild_id, member->color_role_id)
                           : nullptr;
    return role && role->color == row.role_color ? ui_role_gradient_effect(theme, *role) : UiTextEffect{};
}

} // namespace

bool shell_text_presentation_build_output(const UiScene& scene, const ChatStateStore& state,
                                          const NavigationView& navigation, const Theme& theme,
                                          const ShellNavigationScene& channel_scene,
                                          const MessageComposerScene* composer_scene,
                                          const MemberListScene* member_scene,
                                          const UiRect* composer_text_rectangle, UiTextOutput* stream,
                                          const UserStore* users,
                                          const GuildRailScene* guild_scene,
                                          u64 visual_selected_channel_id) noexcept {
    if (!ui_text_output_valid(stream) || scene.count == 0 ||
        navigation.channel_count > navigation.channel_capacity ||
        (navigation.channel_count != 0 && !navigation.channel_rows) ||
        (guild_scene && guild_scene->active &&
         (!guild_scene->rows.data || guild_scene->count > guild_scene->rows.count ||
          !ui_rect_has_area(guild_scene->viewport))) ||
        (channel_scene.active &&
         (!ui_rect_has_area(channel_scene.viewport) || channel_scene.count > channel_scene.rows.count))) {
        reject_atomically(stream);
        return false;
    }
    ui_text_output_reset(stream);
    const UiNode* title = ui_scene_resolve_semantic(scene, UiSemantic::title_bar);
    if (const auto* friends = ui_scene_resolve_semantic(scene, UiSemantic::friends_button)) {
        if (!append_command(scene.widgets, friends->widget, utf8_literal("Friends"),
                            ui_scene_content_rectangle(scene, UiSemantic::friends_button),
                            friends->rectangle, theme.text_primary, stream, UiTextAlignment::start,
                            UiTextAlignment::center)) {
            reject_atomically(stream);
            return false;
        }
    }
    const UiNode* title_label = ui_scene_resolve_semantic(scene, UiSemantic::title_label);
    const UiNode* channel_header = ui_scene_resolve_semantic(scene, UiSemantic::channel_header);
    const UiNode* channel_navigation = ui_scene_resolve_semantic(scene, UiSemantic::channel_navigation);
    if (!title) {
        reject_atomically(stream);
        return false;
    }

    Utf8View title_text = scene.view == UiShellView::friends ? utf8_literal("Friends")
                          : state.home_selected              ? utf8_literal("Direct messages")
                                                             : utf8_literal("MaiCord");
    char private_title[512];
    const ChannelRecord* selected_channel =
        chat_state_store_find_channel(state, visual_selected_channel_id
            ? visual_selected_channel_id : navigation.selected_channel_id);
    if (scene.view == UiShellView::friends && !visual_selected_channel_id) {
        title_text = utf8_literal("Friends");
    } else if (state.restore_channel_id && !visual_selected_channel_id) {
        title_text = utf8_literal("Loading…");
    } else if (selected_channel) {
        const Utf8View name = channel_is_private(selected_channel->type)
                                  ? private_channel_title(state, users, *selected_channel,
                                                          {private_title, sizeof(private_title)})
                                  : chat_state_store_channel_name(state, *selected_channel);
        if (name.bytes != 0) {
            title_text = name;
        }
    }
    const UiNode* title_text_owner = title_label ? title_label : title;
    UiRect title_text_rectangle = title_text_owner->rectangle;
    // Header padding owns horizontal insets only. Center both labels against
    // the complete 48-DIP bar so asymmetric top/bottom padding cannot shift
    // their baselines in opposite directions.
    title_text_rectangle.top = title->rectangle.top;
    title_text_rectangle.bottom = title->rectangle.bottom;
    if (title_label && selected_channel && channel_is_private(selected_channel->type) &&
        scene.view != UiShellView::friends)
        title_text_rectangle.left += kUiPrivateHeaderAvatar + 8.0f;
    UiPluginSlotBadge header_slot{};
    if (title_label && ui_plugin_slot_line_badge(kUiPluginSlotHeaderToolbar, 0, 0, 0, title_label->widget,
                                                 title_text_rectangle, nullptr, &header_slot))
        title_text_rectangle.right = header_slot.badge.rectangle.left - 8;
    if (header_slot.badge.label.bytes && !ui_badge_text_output(header_slot.badge, scene.widgets, theme, stream)) {
        reject_atomically(stream);
        return false;
    }
    if (!append_command(scene.widgets, title_text_owner->widget, title_text, title_text_rectangle,
                        title_text_owner->rectangle, theme.text_primary, stream, UiTextAlignment::start,
                        UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
                        text_style_none,
                        selected_channel && channel_is_private(selected_channel->type))) {
        reject_atomically(stream);
        return false;
    }

    if (channel_header) {
        const GuildRecord* selected_guild =
            chat_state_store_find_guild(state, navigation.selected_guild_id);
        if (selected_guild || state.home_selected) {
            const Utf8View name = selected_guild ? chat_state_store_guild_name(state, *selected_guild)
                                                 : utf8_literal("Direct messages");
            // A guild header draws the name on its dropdown button, ahead of
            // the trailing chevron.
            const UiNode* guild_menu = selected_guild ? ui_scene_resolve_semantic(scene, UiSemantic::guild_menu) : nullptr;
            const UiNode* owner = guild_menu ? guild_menu : channel_header;
            UiRect header_text_rectangle =
                ui_scene_content_rectangle(scene, guild_menu ? UiSemantic::guild_menu : UiSemantic::channel_header);
            header_text_rectangle.top = channel_header->rectangle.top;
            header_text_rectangle.bottom = channel_header->rectangle.bottom;
            if (guild_menu)
                header_text_rectangle.right =
                    maximum(header_text_rectangle.left, header_text_rectangle.right - kUiShellGuildMenuChevron - 4.f);
            // Over a server banner the name turns white with a drop shadow.
            const bool banner = selected_guild && shell_presentation_guild_banner_visible(scene, &channel_scene);
            if (banner) {
                const UiRect shadow{header_text_rectangle.left, header_text_rectangle.top + 1,
                                    header_text_rectangle.right, header_text_rectangle.bottom + 1};
                if (!append_command(scene.widgets, owner->widget, name, shadow, owner->rectangle,
                                    theme.tokens.overlay.shadow, stream, UiTextAlignment::start,
                                    UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis)) {
                    reject_atomically(stream);
                    return false;
                }
            }
            if (!append_command(scene.widgets, owner->widget, name, header_text_rectangle,
                                owner->rectangle, banner ? theme.tokens.text.on_accent : theme.text_primary, stream,
                                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                UiTextOverflow::ellipsis)) {
                reject_atomically(stream);
                return false;
            }
        }
    }

    const auto* home = ui_scene_resolve_semantic(scene, UiSemantic::home_button);
    const auto* rail = ui_scene_resolve_semantic(scene, UiSemantic::guild_rail);
    if (home && rail && navigation.home_badge &&
        !ui_mention_badge_text_output(navigation.home_badge, scene.widgets, home->widget,
                                      ui_mention_badge_guild_rectangle(home->rectangle, navigation.home_badge),
                                      rail->rectangle, stream)) {
        reject_atomically(stream);
        return false;
    }
    const auto* guild_navigation = ui_scene_resolve_semantic(scene, UiSemantic::guild_navigation);
    if (guild_navigation && guild_scene && guild_scene->active) {
        for (u32 index = 0; index < guild_scene->count; ++index) {
            const auto& row = guild_scene->rows[index];
            const auto* guild = row.view.folder_role != GuildFolderRole::header && !row.view.has_icon
                                    ? chat_state_store_find_guild(state, row.view.id)
                                    : nullptr;
            if (guild) {
                const auto* layout = guild_rail_scene_layout_node(*guild_scene, row);
                if (!layout) {
                    reject_atomically(stream);
                    return false;
                }
                char storage[32];
                u32 characters = 0;
                const auto acronym =
                    guild_acronym(chat_state_store_guild_name(state, *guild), {storage, sizeof(storage)}, &characters);
                const auto clip = ui_rect_intersection(guild_navigation->rectangle, guild_scene->viewport);
                if (!append_command(guild_scene->widgets, row.widget, acronym,
                                    guild_rail_icon_rectangle(layout->rectangle), clip,
                                    row.view.unavailable || (row.view.muted && !row.view.selected)
                                        ? theme.text_secondary
                                        : theme.text_primary,
                                    stream,
                                    UiTextAlignment::center, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                    UiTextOverflow::clip,
                                    characters <= 2   ? text_style_heading_3
                                    : characters <= 4 ? text_style_none
                                                      : text_style_subtext,
                                    true)) {
                    reject_atomically(stream);
                    return false;
                }
            }
            if (!row.view.mention_count ||
                (row.view.folder_role == GuildFolderRole::header && row.view.folder_expanded))
                continue;
            const auto* layout = guild_rail_scene_layout_node(*guild_scene, row);
            if (!layout) {
                reject_atomically(stream);
                return false;
            }
            const auto rectangle = ui_mention_badge_guild_rectangle(guild_rail_icon_rectangle(layout->rectangle),
                                                                    row.view.mention_count);
            // Rail badges extend beyond the row; clipping to the row itself
            // would cut the two-DIP overhang shared with the paint geometry.
            const auto clip = ui_rect_intersection(guild_navigation->rectangle, guild_scene->viewport);
            if (!ui_mention_badge_text_output(row.view.mention_count, guild_scene->widgets, row.widget,
                                              rectangle, clip, stream)) {
                reject_atomically(stream);
                return false;
            }
        }
    }
    if (channel_navigation && channel_scene.active) {
        for (u32 index = 0; index < channel_scene.count; ++index) {
            const ShellNavigationRowWidget& row_widget = channel_scene.rows[index];
            const ChannelViewRow& row = row_widget.view;
            const UiLayoutNode* layout = shell_navigation_scene_layout_node(channel_scene, row_widget);
            const UiLayoutNode* label_layout =
                ui_widget_manager_layout_node(channel_scene.widgets, row_widget.label);
            if (!layout || !label_layout) {
                reject_atomically(stream);
                return false;
            }
            if (layout->rectangle.bottom > channel_navigation->rectangle.top &&
                layout->rectangle.top < channel_navigation->rectangle.bottom) {
                const ChannelRecord* channel = chat_state_store_find_channel(state, row.id);
                const Utf8View name = row.type == kNavigationEventsRowType
                                          ? Utf8View{navigation.events_label, navigation.events_label_bytes}
                                      : row.type == kNavigationRequestsRowType ? utf8_literal("Message requests")
                                      : !channel ? Utf8View{}
                                      : channel_is_private(channel->type)
                                          ? private_channel_title(state, users, *channel,
                                                                  {private_title, sizeof(private_title)})
                                          : chat_state_store_channel_name(state, *channel);
                const bool row_selected = visual_selected_channel_id
                    ? row.id == visual_selected_channel_id : row.selected;
                const Color color = ui_navigation_foreground(theme, row, row_selected);
                // Text keeps its row's own geometry and only the clip follows the
                // list viewport, so a row half under the header scrolls away beneath
                // it instead of being squeezed into the visible part.
                const UiRect visible_clip = ui_rect_intersection(
                    ui_rect_intersection(label_layout->clip_rectangle, label_layout->content_rectangle),
                    channel_navigation->rectangle);
                const auto peer =
                    channel && channel->type == 1 ? private_channel_peer(state, channel->id) : 0;
                const auto* appearance =
                    users && peer ? user_appearance_find(users->appearance, peer) : nullptr;
                const bool hovered =
                    channel_scene.widgets.hovered.slot == row_widget.widget.slot &&
                    channel_scene.widgets.hovered.generation == row_widget.widget.generation;
                // Conversation rows centre the name, or stack it over a subtitle.
                char subtitle_storage[192];
                const Utf8View subtitle =
                    channel && channel_is_private(channel->type) && navigation.private_rows
                        ? ui_private_row_subtitle(*navigation.private_rows, channel->id,
                                                  {subtitle_storage, sizeof(subtitle_storage)})
                        : Utf8View{};
                UiRect name_rectangle = label_layout->content_rectangle;
                // A plugin badge ends the name (and the DM subtitle) early.
                UiPluginSlotBadge plugin{};
                float text_right = label_layout->content_rectangle.right;
                if (shell_navigation_plugin_badge(channel_scene, index, &plugin)) {
                    text_right = plugin.badge.rectangle.left - 6.0f;
                    name_rectangle.right = minimum(name_rectangle.right, text_right);
                    if (!ui_badge_text_output(plugin.badge, channel_scene.widgets, theme, stream)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
                if (channel && channel_is_private(channel->type)) {
                    const UiRect content = label_layout->content_rectangle;
                    const float middle = (content.top + content.bottom) * 0.5f;
                    name_rectangle = subtitle.bytes ? UiRect{content.left, middle - 19.0f, text_right, middle + 1.0f}
                                                    : UiRect{content.left, middle - 10.0f, text_right, middle + 10.0f};
                    const UiRect subtitle_rectangle{content.left, middle + 1.0f, text_right, middle + 17.0f};
                    if (subtitle.bytes && ui_rect_has_area(subtitle_rectangle) &&
                        !append_command(channel_scene.widgets, row_widget.label, subtitle, subtitle_rectangle,
                                        visible_clip, ui_background_muted_text_color(theme), stream,
                                        UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis, text_style_subtext)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
                // The connected channel's elapsed timer ends the name line.
                if (const auto* timer = navigation.voice_panel ? navigation.voice_panel->elapsed : nullptr;
                    timer && timer->label_bytes && timer->channel_id == row.id) {
                    UiRect line = label_layout->content_rectangle;
                    line.right = text_right;
                    const UiRect box =
                        ui_voice_elapsed_rectangle(line, timer->label_bytes, theme.scale.space_4, &text_right);
                    name_rectangle.right = minimum(name_rectangle.right, text_right);
                    if (!append_command(channel_scene.widgets, row_widget.label, {timer->label, timer->label_bytes},
                                        box, visible_clip, theme.tokens.text.positive, stream, UiTextAlignment::end,
                                        UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::clip,
                                        text_style_subtext)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
                if (name.bytes != 0 &&
                    !append_command(
                        channel_scene.widgets, row_widget.label, name, name_rectangle, visible_clip,
                        color, stream, UiTextAlignment::start,
                        channel && channel_is_private(channel->type) ? UiTextAlignment::center
                                                                             : UiTextAlignment::start,
                        UiTextWrapping::no_wrap, UiTextOverflow::ellipsis, text_style_none,
                        channel && channel_is_private(channel->type),
                        appearance ? profile_name_face(appearance->name.font_id) : UiTextFace::normal,
                        appearance ? profile_name_effect(appearance->name, hovered,
                                                         channel_scene.widgets.hover_started_ms)
                                   : UiTextEffect{})) {
                    reject_atomically(stream);
                    return false;
                }
                // A live stage's topic sits on its own line under the name.
                if (const auto* instance = row.stage_live && navigation.stage_instances
                                               ? stage_instance_store_find(*navigation.stage_instances, row.id)
                                               : nullptr) {
                    const float left = layout->rectangle.left + 34.0f + static_cast<float>(row.depth) * 12.0f;
                    const float top = layout->rectangle.top + kShellNavigationVoiceHeadHeight - 4.0f;
                    const UiRect topic{left, top, layout->rectangle.right - 10.0f,
                                       top + kShellNavigationStageTopicHeight};
                    const UiRect clip = ui_rect_intersection(topic, channel_navigation->rectangle);
                    if (ui_rect_has_area(clip) &&
                        !append_command(channel_scene.widgets, row_widget.widget, stage_instance_topic(*instance), topic,
                                        clip, ui_background_muted_text_color(theme), stream, UiTextAlignment::start,
                                        UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
                                        text_style_subtext)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
                UiVoiceParticipantRow voice{};
                for (u32 i = 0; navigation.voice_states && users &&
                                ui_voice_participant_row(row, layout->rectangle, *navigation.voice_states, i, &voice);
                     ++i) {
                    const auto* user = user_store_find(*users, voice.state->user_id);
                    const UiRect clip = ui_rect_intersection(voice.label, channel_navigation->rectangle);
                    // Voice members are named by their server nickname first. A
                    // member absent from the caches still occupies its row while
                    // the account requests it.
                    const auto* member = member_scene && member_scene->members
                                             ? guild_member_store_find(*member_scene->members,
                                                                       voice.state->guild_id, voice.state->user_id)
                                             : nullptr;
                    const Utf8View nickname = member ? guild_member_store_nickname(*member) : Utf8View{};
                    const Utf8View voice_name = nickname.bytes ? nickname
                                                : user         ? user_store_name(*users, *user)
                                                               : utf8_literal("Unknown user");
                    if (ui_rect_has_area(clip) &&
                        !append_command(channel_scene.widgets, row_widget.widget, voice_name, voice.label, clip,
                                        ui_background_muted_text_color(theme), stream, UiTextAlignment::start,
                                        UiTextAlignment::center, UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis, text_style_subtext)) {
                        reject_atomically(stream);
                        return false;
                    }
                    const UiRect live_clip = ui_rect_intersection(voice.live, channel_navigation->rectangle);
                    if (ui_rect_has_area(live_clip) &&
                        !append_command(channel_scene.widgets, row_widget.widget, utf8_literal("LIVE"), voice.live,
                                        live_clip, theme.tokens.text.on_accent, stream, UiTextAlignment::center,
                                        UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::clip,
                                        text_style_subtext | text_style_bold)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
                if (row.mention_count) {
                    const auto* badge =
                        ui_widget_manager_layout_node(channel_scene.widgets, row_widget.mention_badge);
                    if (!badge ||
                        !ui_mention_badge_text_output(
                            row.mention_count, channel_scene.widgets, row_widget.mention_badge,
                            badge->rectangle,
                            ui_rect_intersection(badge->clip_rectangle, channel_navigation->rectangle),
                            stream)) {
                        reject_atomically(stream);
                        return false;
                    }
                }
            }
        }
    }
    if (composer_scene && composer_scene->active && composer_scene->model) {
        const UiLayoutNode* reply =
            ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->reply);
        const UiLayoutNode* edit =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::edit);
        const UiLayoutNode* send =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::send);
        const UiLayoutNode* typing =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::typing_indicator);
        if (!edit || !send || !typing) {
            reject_atomically(stream);
            return false;
        }
        const Utf8View reply_text = message_composer_scene_reply_label(*composer_scene);
        const Utf8View attachments_text = message_composer_scene_attachment_label(*composer_scene);
        if (message_composer_scene_attachment_row(*composer_scene) == message_composer_attachment_row_strip) {
            if (!ui_composer_attachment_strip_text(message_composer_scene_attachment_view(*composer_scene, true),
                                                   theme, stream)) {
                reject_atomically(stream);
                return false;
            }
        } else if (attachments_text.bytes) {
            const auto* attachments =
                message_composer_scene_layout_node(*composer_scene, MessageComposerControl::attachments);
            if (!attachments ||
                !append_command(composer_scene->widgets, composer_scene->attachments, attachments_text,
                                attachments->content_rectangle, attachments->clip_rectangle,
                                ui_background_muted_text_color(theme), stream, UiTextAlignment::start,
                                UiTextAlignment::center, UiTextWrapping::no_wrap,
                                UiTextOverflow::ellipsis)) {
                reject_atomically(stream);
                return false;
            }
        }
        if (reply_text.bytes != 0 &&
            (!reply || !append_command(composer_scene->widgets, composer_scene->reply, reply_text,
                                       message_composer_scene_reply_text_rectangle(*composer_scene), reply->clip_rectangle,
                                       theme.text_secondary, stream,
                                       UiTextAlignment::start, UiTextAlignment::center,
                                       UiTextWrapping::no_wrap, UiTextOverflow::ellipsis))) {
            reject_atomically(stream);
            return false;
        }
        const auto mention_text = message_composer_scene_reply_mention_label(*composer_scene);
        if (mention_text.bytes) {
            const auto* mention = message_composer_scene_layout_node(
                *composer_scene, MessageComposerControl::reply_mention);
            if (!mention || !append_command(composer_scene->widgets, composer_scene->reply_mention,
                mention_text, message_composer_scene_reply_mention_text(*composer_scene),
                mention->clip_rectangle,
                composer_scene->mention_reply_author ? theme.tokens.text.link : theme.tokens.text.muted,
                stream, UiTextAlignment::start, UiTextAlignment::center,
                UiTextWrapping::no_wrap, UiTextOverflow::clip, text_style_bold | kTextStyleSizeCodeSm)) {
                reject_atomically(stream);
                return false;
            }
        }
        const Utf8View draft = message_composer_scene_display_text(*composer_scene);
        Utf8View edit_text =
            composer_scene->model->bytes != 0 ? draft : message_composer_scene_placeholder(*composer_scene);
        Color edit_color =
            composer_scene->model->bytes != 0 ? theme.text_primary : ui_background_muted_text_color(theme);
        if (composer_scene->model->phase == MessageComposerPhase::failed) {
            edit_color = theme.notification;
        }
        const UiRect edit_text_rectangle =
            composer_text_rectangle && ui_rect_has_area(*composer_text_rectangle) ? *composer_text_rectangle
                                                                                  : edit->content_rectangle;
        const UiRect edit_text_clip = ui_rect_intersection(edit->clip_rectangle, edit->content_rectangle);
        // The recording bar replaces the draft and placeholder.
        if (ui_composer_voice_bar_visible(message_composer_scene_voice_mode(*composer_scene)) &&
            composer_scene->voice) {
            if (!ui_composer_voice_bar_text(*composer_scene->voice, edit->content_rectangle, edit_text_clip,
                                            {&composer_scene->widgets, composer_scene->edit}, theme, stream)) {
                reject_atomically(stream);
                return false;
            }
        } else if (!append_command(
                composer_scene->widgets, composer_scene->edit, edit_text, edit_text_rectangle,
                edit_text_clip, edit_color, stream, UiTextAlignment::start,
                composer_scene->model->bytes == 0 ? UiTextAlignment::center : UiTextAlignment::start,
                composer_scene->model->bytes == 0 ? UiTextWrapping::no_wrap : UiTextWrapping::wrap,
                composer_scene->model->bytes == 0 ? UiTextOverflow::ellipsis : UiTextOverflow::clip)) {
            reject_atomically(stream);
            return false;
        }
        const Utf8View typing_text = message_composer_scene_typing_label(*composer_scene);
        if (typing_text.bytes != 0 &&
            !append_command(composer_scene->widgets, composer_scene->typing_indicator, typing_text,
                            typing->content_rectangle, typing->clip_rectangle,
                            (composer_scene->status_is_error || composer_scene->status_tone == 4 ? theme.notification
                              : composer_scene->status_tone == 3                        ? theme.tokens.text.warning
                              : composer_scene->status_tone == 2                        ? theme.tokens.text.positive
                              : composer_scene->status_tone == 1                        ? theme.accent
                                                                                        : theme.text_primary), stream,
                            UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                            UiTextOverflow::ellipsis, text_style_subtext)) {
            reject_atomically(stream);
            return false;
        }
    }
    if (const auto* footer = ui_scene_resolve_semantic(scene, UiSemantic::channel_footer); footer && users) {
        const auto* self = user_store_find(*users, state.self_user_id);
        if (self) {
            auto r = footer->rectangle;
            r.left += 52;
            r.right -= navigation.voice_panel ? kUiVoiceAccountButtonsWidth + 4 + ui_plugin_slot_account_width() : 44;
            r.top += 6;
            r.bottom = r.top + 22;
            if (!append_command(scene.widgets, footer->widget, user_store_name(*users, *self), r,
                                footer->rectangle, theme.text_primary, stream, UiTextAlignment::start,
                                UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
                                text_style_bold))
                return false;
            r.top = r.bottom;
            r.bottom = footer->rectangle.bottom - 4;
            // A custom status replaces the username line.
            const auto* appearance = user_appearance_find(users->appearance, state.self_user_id);
            char status[kAppearanceStatusEmojiBytes + 1 + kAppearanceActivityBytes];
            u32 status_bytes = 0;
            if (appearance && (appearance->fields & appearance_activity)) {
                if (!appearance->status_emoji_id && appearance->status_emoji.bytes) {
                    copy_bytes(status, appearance->status_emoji.view().data,
                               appearance->status_emoji.bytes);
                    status_bytes = appearance->status_emoji.bytes;
                    if (appearance->activity.bytes)
                        status[status_bytes++] = ' ';
                }
                copy_bytes(status + status_bytes, appearance->activity.view().data,
                           appearance->activity.bytes);
                status_bytes += appearance->activity.bytes;
            }
            if (!append_command(scene.widgets, footer->widget,
                                status_bytes ? Utf8View{status, status_bytes}
                                             : user_store_username(*users, *self),
                                r,
                                footer->rectangle, ui_background_muted_text_color(theme), stream,
                                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                UiTextOverflow::ellipsis, text_style_subtext))
                return false;
        }
        if (const auto* voice = navigation.voice_panel; voice && voice->state != UiVoicePanelState::hidden) {
            const UiRect panel = ui_voice_panel_rectangle(footer->rectangle);
            const Utf8View status = voice->state == UiVoicePanelState::connected  ? utf8_literal("Voice connected")
                                    : voice->state == UiVoicePanelState::connecting ? utf8_literal("Connecting…")
                                                                                     : utf8_literal("Couldn't connect");
            // "Channel / Server", copied into the output.
            char place[192];
            u32 used = 0;
            const auto put = [&](Utf8View text) noexcept {
                for (u32 i = 0; i < text.bytes && used < sizeof(place); ++i)
                    place[used++] = text.data[i];
            };
            if (const auto* channel = chat_state_store_find_channel(state, voice->channel_id)) {
                char call_title[128];
                put(channel_is_private(channel->type)
                        ? private_channel_title(state, users, *channel, {call_title, sizeof(call_title)})
                        : chat_state_store_channel_name(state, *channel));
                if (const auto* guild = chat_state_store_find_guild(state, voice->guild_id)) {
                    put(utf8_literal(" / "));
                    put(chat_state_store_guild_name(state, *guild));
                }
            }
            while (used && used == sizeof(place) && (static_cast<u8>(place[used - 1]) & 0xc0) == 0x80)
                --used; // Never cut a UTF-8 sequence.
            if (used == sizeof(place))
                --used;
            const bool hovered = voice->hovered == UiVoiceControl::channel;
            if (!append_command(scene.widgets, footer->widget, status, ui_voice_panel_status_text(panel), panel,
                                ui_voice_panel_status_color(*voice, theme), stream, UiTextAlignment::start,
                                UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
                                text_style_bold) ||
                !append_command(scene.widgets, footer->widget, {place, used}, ui_voice_panel_channel_text(panel),
                                panel, hovered ? theme.text_primary : ui_background_muted_text_color(theme),
                                stream, UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                UiTextOverflow::ellipsis, text_style_subtext, true))
                return false;
        }
    }
    return !member_scene || shell_text_presentation_member_output(*member_scene, theme, stream);
}

bool shell_text_presentation_member_output(const MemberListScene& member_list, const Theme& theme,
                                           UiTextOutput* stream) noexcept {
    const MemberListScene* member_scene = &member_list;
    if (!ui_text_output_valid(stream))
        return false;
    if (!member_scene->active || !member_scene->source || !member_scene->members || !member_scene->users ||
        !member_scene->state)
        return true;
    for (u32 index = 0; index < member_scene->count; ++index) {
        const auto& row = member_scene->rows[index];
        const auto* label = member_list_scene_layout_node(*member_scene, row.label);
        if (!member_list_scene_row_resident(*member_scene, index))
            continue;
        if (!label || row.source_index >= member_scene->source_count)
            return false;
        const bool group = row.kind == MemberListRowKind::group;
        const auto name = group ? row.group_label.view()
                                : member_list_row_label(row.source_row, *member_scene->members,
                                                        *member_scene->users, *member_scene->state);
        auto color =
            group ? ui_background_muted_text_color(theme) : packed_role_color(row.role_color, theme);
        const bool offline = !group && row.source_row.status == GuildMemberStatus::offline;
        const bool hovered =
            member_list_scene_resolve(*member_scene, member_scene->widgets.hovered) == &row;
        if (offline && !hovered)
            color.alpha = .4f;
        auto r = label->content_rectangle;
        const auto* global = user_appearance_find(member_scene->users->appearance, row.user_id);
        const auto* guild = user_appearance_find(member_scene->users->appearance, row.user_id,
                                                 member_scene->source->guild_id);
        const auto* activity = guild && (guild->fields & appearance_activity) ? guild : global;
        const bool subtitle = !group && activity && activity->activity.bytes && !offline;
        if (subtitle) {
            r.top -= 4;
            r.bottom = r.top + 20;
        }
        const bool bot = global && global->bot;
        const bool tag = global && global->primary_enabled && global->primary_tag.bytes;
        const auto badge = member_list_badge(*member_scene, index);
        if (bot || tag)
            r.right = badge.rectangle.left - 5;
        const auto plugin = member_list_plugin_badge(*member_scene, index);
        if (plugin.badge.label.bytes)
            r.right = minimum(r.right, plugin.badge.rectangle.left - 5);
        const auto* name_style = guild && guild->name.font_id ? guild : global;
        // A hovered display-name effect wins; otherwise an enhanced role
        // style paints its gradient over the role colour.
        auto name_effect =
            !group && name_style && hovered
                ? profile_name_effect(name_style->name, true, member_scene->widgets.hover_started_ms)
                : UiTextEffect{};
        if (!group && !name_effect.kind)
            name_effect = member_role_effect(*member_scene, row.source_row, theme);
        if (!append_command(
                member_scene->widgets, row.label, name, r, label->clip_rectangle, color, stream,
                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                UiTextOverflow::ellipsis, group ? text_style_subtext | text_style_bold : 0, false,
                !group && name_style ? profile_name_face(name_style->name.font_id) : UiTextFace::normal,
                name_effect))
            return false;
        if (subtitle) {
            auto sub = label->content_rectangle;
            sub.top = r.bottom;
            sub.bottom += 4;
            if (activity->status_emoji_id || activity->status_emoji.bytes) {
                if (!activity->status_emoji_id) {
                    auto emoji = sub;
                    emoji.right = emoji.left + 16;
                    if (!append_command(member_scene->widgets, row.label, activity->status_emoji.view(),
                                        emoji, label->clip_rectangle, theme.text_primary, stream,
                                        UiTextAlignment::center, UiTextAlignment::center))
                        return false;
                }
                sub.left += 20;
            }
            if (!append_command(member_scene->widgets, row.label, activity->activity.view(), sub,
                                label->clip_rectangle, ui_background_muted_text_color(theme), stream,
                                UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                UiTextOverflow::ellipsis, text_style_subtext))
                return false;
        }
        if ((bot || tag) && !ui_badge_text_output(badge, member_scene->widgets, theme, stream))
            return false;
        if (plugin.badge.label.bytes &&
            !ui_badge_text_output(plugin.badge, member_scene->widgets, theme, stream))
            return false;
    }
    return true;
}

} // namespace maicord

namespace maicord {
bool shell_text_presentation_build(const UiScene& scene, const ChatStateStore& state,
                                   const NavigationView& navigation, const Theme& theme,
                                   const ShellNavigationScene& channel_scene,
                                   const MessageComposerScene* composer_scene,
                                   const MemberListScene* member_scene,
                                   const UiRect* composer_text_rectangle, UiTextCommandStream* stream,
                                   const UserStore* users,
                                   const GuildRailScene* guild_scene,
                                   u64 visual_selected_channel_id) noexcept {
    UiTextOutput output{stream, nullptr};
    return shell_text_presentation_build_output(scene, state, navigation, theme, channel_scene,
                                                composer_scene, member_scene, composer_text_rectangle,
                                                &output, users, guild_scene, visual_selected_channel_id);
}
} // namespace maicord

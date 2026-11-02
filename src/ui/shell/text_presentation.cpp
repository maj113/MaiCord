#include "maicord/ui/widgets/badge.hpp"
#include "maicord/model/voice/stage_instance_store.hpp"
#include "maicord/model/voice/channel_info_store.hpp"
#include "maicord/protocol/voice/channel_info.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/ui/theme/role_color.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/ui/shell/navigation_foreground.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/ui/voice/panel.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/guild/member_list.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/ui/chat/composer/attachment_strip.hpp"
#include "maicord/ui/chat/composer/voice_bar.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"

namespace maicord {
namespace {

constexpr UiTextEffect no_effect{};

// How a command's text sits in its box; the presets cover the combinations the
// shell uses, and call sites with a per-row face or effect copy one and set them.
struct TextLook {
    u32 style_flags = text_style_none;
    UiTextAlignment horizontal = UiTextAlignment::start;
    UiTextAlignment vertical = UiTextAlignment::center;
    TextWrapping wrapping = TextWrapping::no_wrap;
    TextOverflow overflow = TextOverflow::ellipsis;
    bool owned = false;
    UiTextFace face = UiTextFace::normal;
    const UiTextEffect* effect = &no_effect;
};

constexpr TextLook kLookLine{};
constexpr TextLook kLookLineOwned{.owned = true};
constexpr TextLook kLookClipped{.overflow = TextOverflow::clip};
constexpr TextLook kLookCentered{.horizontal = UiTextAlignment::center, .overflow = TextOverflow::clip};
constexpr TextLook kLookSub{.style_flags = text_style_subtext};
constexpr TextLook kLookSubOwned{.style_flags = text_style_subtext, .owned = true};
constexpr TextLook kLookBold{.style_flags = text_style_bold};
constexpr TextLook kLookCode{.style_flags = kTextStyleSizeCodeSm, .owned = true};
constexpr TextLook kLookTimer{.style_flags = text_style_subtext,
                              .horizontal = UiTextAlignment::end,
                              .overflow = TextOverflow::clip,
                              .owned = true};
constexpr TextLook kLookBadge{.style_flags = text_style_subtext | text_style_bold,
                              .horizontal = UiTextAlignment::center,
                              .overflow = TextOverflow::clip};
constexpr TextLook kLookMention{.style_flags = text_style_bold | kTextStyleSizeCodeSm,
                                .overflow = TextOverflow::clip};
constexpr TextLook kLookDraft{.vertical = UiTextAlignment::start,
                              .wrapping = TextWrapping::wrap,
                              .overflow = TextOverflow::clip};
constexpr TextLook kLookTop{.vertical = UiTextAlignment::start};
constexpr TextLook kLookGroup{.style_flags = text_style_subtext | text_style_bold};

bool append_command(const UiWidgetManager& manager, UiWidgetHandle owner, const Utf8View& text,
                    const UiRect& rectangle, const UiRect& clip, const Color& color, UiTextOutput* stream,
                    const TextLook& look) noexcept {
    if (text.bytes == 0) {
        return true;
    }
    const UiTextCommand command{text,
                                UiWidgetReference{&manager, owner},
                                rectangle,
                                clip,
                                color,
                                look.style_flags,
                                look.horizontal,
                                look.vertical,
                                look.wrapping,
                                look.overflow,
                                look.face,
                                *look.effect};
    return look.owned ? ui_text_output_push_owned(stream, command) : ui_text_output_push(stream, command);
}

// A command clipped to `rectangle` limited to `region`; nothing when that is empty.
bool append_visible(const UiWidgetManager& manager, UiWidgetHandle owner, const Utf8View& text,
                    const UiRect& rectangle, const UiRect& region, const Color& color, UiTextOutput* stream,
                    const TextLook& look) noexcept {
    const UiRect clip = ui_rect_intersection(rectangle, region);
    return !ui_rect_has_area(clip) || append_command(manager, owner, text, rectangle, clip, color, stream, look);
}

// A rejected output drops every command it holds; callers `return reject(...)`.
bool reject(UiTextOutput* stream) noexcept {
    ui_text_output_reject(stream);
    return false;
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

// The server acronym: the first character of each word ("MaiCord Lab"
// -> "ML"). The length selects a smaller face for longer acronyms.
Utf8View ui_guild_acronym(const Utf8View& name, const Slice<char>& storage, u32* characters) noexcept {
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

// The channel or DM name in the header with its plugin badge and a DM's activity line; outlined with its
// name and activity buffers so they stay off the main frame.
static __declspec(noinline) bool title_output(const UiScene& scene, const ChatStateStore& state,
                                              const NavigationView& navigation, const Theme& theme,
                                              UiTextOutput* stream, const UserStore* users,
                                              u64 visual_selected_channel_id, const UiLayoutNode* title,
                                              const UiLayoutNode* title_label) noexcept {
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
    const UiLayoutNode* title_text_owner = title_label ? title_label : title;
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
    if (title_label && ui_plugin_slot_line_badge(kUiPluginSlotHeaderToolbar, 0, 0, 0, ui_scene_widget(scene, UiSemantic::title_label),
                                                 title_text_rectangle, nullptr, &header_slot))
        title_text_rectangle.right = header_slot.badge.rectangle.left - 8;
    if (header_slot.badge.label.bytes && !ui_badge_text_output(header_slot.badge, scene.widgets, theme, stream))
        return reject(stream);
    if (!append_command(scene.widgets, ui_scene_widget(scene, title_label ? UiSemantic::title_label : UiSemantic::title_bar), title_text, title_text_rectangle,
                        title_text_owner->rectangle, theme.text_primary, stream,
                        selected_channel && channel_is_private(selected_channel->type) ? kLookLineOwned : kLookLine))
        return reject(stream);
    // A one-to-one DM's activity follows the name (its glyph: private_channel_rows).
    char activity_line[192];
    UiPrivateHeaderActivity activity{};
    if (selected_channel && navigation.private_rows && title_label &&
        ui_private_header_activity(scene, *navigation.private_rows, selected_channel->id,
                                   {activity_line, sizeof(activity_line)}, &activity) &&
        !append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::title_label), activity.line, activity.text, title_label->rectangle,
                        theme.text_secondary, stream, kLookCode))
        return reject(stream);
    return true;
}

// The account name and status line and the voice panel text; outlined so the buffers stay off the main frame.
static __declspec(noinline) bool footer_output(const UiScene& scene, const ChatStateStore& state,
                                        const NavigationView& navigation, const Theme& theme, UiTextOutput* stream,
                                        const UserStore* users) noexcept {
    if (const auto* footer = ui_scene_node(scene, UiSemantic::channel_footer); footer && users) {
        const auto* self = user_store_find(*users, state.self_user_id);
        if (self) {
            auto r = footer->rectangle;
            r.left += 52;
            r.right -= navigation.voice_panel ? kUiVoiceAccountButtonsWidth + 4 + ui_plugin_slot_account_width() : 44;
            r.top += 6;
            r.bottom = r.top + 22;
            if (!append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::channel_footer), user_store_name(*users, *self), r,
                                footer->rectangle, theme.text_primary, stream, kLookBold))
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
            if (!append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::channel_footer),
                                status_bytes ? Utf8View{status, status_bytes}
                                             : user_store_username(*users, *self),
                                r,
                                footer->rectangle, ui_background_muted_text_color(theme), stream, kLookSub))
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
            if (!append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::channel_footer), status, ui_voice_panel_status_text(panel), panel,
                                ui_voice_panel_status_color(*voice, theme), stream, kLookBold) ||
                !append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::channel_footer), {place, used}, ui_voice_panel_channel_text(panel),
                                panel, hovered ? theme.text_primary : ui_background_muted_text_color(theme),
                                stream, kLookSubOwned))
                return false;
        }
    }
    return true;
}

// The typing line. Without role colours ("Role colors everywhere") it is one command; with them each name is
// its own bold command in the member's colour and the text between keeps the label's, laid left to right
// by measured width.
static __declspec(noinline) bool typing_output(const MessageComposerScene& composer, const UiLayoutNode& typing,
                                               Utf8View text, const Color& color, UiTextMeasureCache* measure,
                                               UiTextOutput* stream) noexcept {
    if (text.bytes == 0)
        return true;
    const u32 names = composer.typing_name_count > 3 ? 0u : composer.typing_name_count;
    bool colored = false;
    for (u32 i = 0; i < names; ++i)
        colored = colored || composer.typing_names[i].color != 0;
    if (!colored || !measure)
        return append_command(composer.widgets, composer.typing_indicator, text, typing.content_rectangle,
                              typing.clip_rectangle, color, stream, kLookSub);
    UiRect rectangle = typing.content_rectangle;
    u32 at = 0;
    const auto emit = [&](u32 bytes, const Color& ink, const TextLook& look) noexcept {
        if (bytes == 0)
            return true;
        const Utf8View part{text.data + at, bytes};
        const TextStyleSpan style{0, bytes, look.style_flags};
        UiTextMeasureValue measured{};
        float width = static_cast<float>(bytes) * 7.0f;
        if (ui_text_measure_cache_measure(
                measure,
                {{0x5459'5049'4E47ull, hash64_mix(hash64_bytes(kHash64Seed, part.data, bytes), look.style_flags), 0, 0,
                  0},
                 part,
                 {&style, 1},
                 {maximum(1.f, ui_rect_width(rectangle)), ui_rect_height(typing.content_rectangle)},
                 true},
                &measured)
                .ok())
            width = measured.size.width;
        const bool pushed = append_command(composer.widgets, composer.typing_indicator, part, rectangle,
                                           typing.clip_rectangle, ink, stream, look);
        rectangle.left = minimum(rectangle.right, rectangle.left + width);
        at += bytes;
        return pushed;
    };
    for (u32 i = 0; i < names; ++i) {
        const MessageComposerTypingName& name = composer.typing_names[i];
        if (name.offset < at || name.offset > text.bytes || name.bytes > text.bytes - name.offset)
            continue;
        if (!emit(name.offset - at, color, kLookSub) ||
            !emit(name.bytes, name.color ? ui_role_color(name.color) : color, kLookGroup))
            return false;
    }
    return emit(text.bytes - at, color, kLookSub);
}

bool shell_text_presentation_build_output(const UiScene& scene, const ChatStateStore& state,
                                          const NavigationView& navigation, const Theme& theme,
                                          const ShellNavigationScene& channel_scene,
                                          const MessageComposerScene* composer_scene,
                                          const MemberListScene* member_scene,
                                          const UiRect* composer_text_rectangle, UiTextOutput* stream,
                                          const UserStore* users,
                                          const GuildRailScene* guild_scene,
                                          u64 visual_selected_channel_id, UiTextMeasureCache* measure) noexcept {
    if (!ui_text_output_valid(stream) || scene.count == 0 ||
        navigation.channel_count > navigation.channel_capacity ||
        (navigation.channel_count != 0 && !navigation.channel_rows) ||
        (guild_scene && guild_scene->active &&
         (!guild_scene->rows.data || guild_scene->count > guild_scene->rows.count ||
          !ui_rect_has_area(guild_scene->viewport))) ||
        (channel_scene.active &&
         (!ui_rect_has_area(channel_scene.viewport) || channel_scene.count > channel_scene.rows.count))) {
        return reject(stream);
    }
    ui_text_output_reset(stream);
    const UiLayoutNode* title = ui_scene_node(scene, UiSemantic::title_bar);
    if (const auto* friends = ui_scene_node(scene, UiSemantic::friends_button)) {
        if (!append_command(scene.widgets, ui_scene_widget(scene, UiSemantic::friends_button), utf8_literal("Friends"),
                            ui_scene_content_rectangle(scene, UiSemantic::friends_button),
                            friends->rectangle, theme.text_primary, stream, kLookClipped))
            return reject(stream);
    }
    const UiLayoutNode* title_label = ui_scene_node(scene, UiSemantic::title_label);
    const UiLayoutNode* channel_header = ui_scene_node(scene, UiSemantic::channel_header);
    const UiLayoutNode* channel_navigation = ui_scene_node(scene, UiSemantic::channel_navigation);
    if (!title)
        return reject(stream);

    if (!title_output(scene, state, navigation, theme, stream, users, visual_selected_channel_id, title, title_label))
        return false;
    char private_title[512];

    if (channel_header) {
        const GuildRecord* selected_guild =
            chat_state_store_find_guild(state, navigation.selected_guild_id);
        if (selected_guild || state.home_selected) {
            const Utf8View name = selected_guild ? chat_state_store_guild_name(state, *selected_guild)
                                                 : utf8_literal("Direct messages");
            // A guild header draws the name on its dropdown button, the
            // chevron right after it (shell_guild_header_geometry).
            const UiLayoutNode* guild_menu = selected_guild ? ui_scene_node(scene, UiSemantic::guild_menu) : nullptr;
            const UiLayoutNode* owner = guild_menu ? guild_menu : channel_header;
            UiRect header_text_rectangle =
                ui_scene_content_rectangle(scene, guild_menu ? UiSemantic::guild_menu : UiSemantic::channel_header);
            header_text_rectangle.top = channel_header->rectangle.top;
            header_text_rectangle.bottom = channel_header->rectangle.bottom;
            if (guild_menu) {
                const UiRect menu = guild_menu->rectangle;
                const auto geometry = shell_guild_header_geometry(
                    menu, shell_text_width(measure, selected_guild->id, kShellMeasureGuildName, name, text_style_none,
                                           shell_guild_header_name_space(menu)));
                header_text_rectangle.left = geometry.name.left;
                header_text_rectangle.right = minimum(geometry.chevron.left, geometry.name.right + 1.f);
            }
            // Over a server banner the name turns white with a drop shadow.
            const bool banner = selected_guild && shell_presentation_guild_banner_visible(scene, &channel_scene);
            if (banner) {
                const UiRect shadow{header_text_rectangle.left, header_text_rectangle.top + 1,
                                    header_text_rectangle.right, header_text_rectangle.bottom + 1};
                if (!append_command(scene.widgets, ui_scene_widget(scene, guild_menu ? UiSemantic::guild_menu : UiSemantic::channel_header), name, shadow, owner->rectangle,
                                    theme.tokens.overlay.shadow, stream, kLookLine))
                    return reject(stream);
            }
            if (!append_command(scene.widgets, ui_scene_widget(scene, guild_menu ? UiSemantic::guild_menu : UiSemantic::channel_header), name, header_text_rectangle, owner->rectangle,
                                banner ? theme.tokens.text.on_accent : theme.text_primary, stream, kLookLine))
                return reject(stream);
        }
    }

    const auto* home = ui_scene_node(scene, UiSemantic::home_button);
    const auto* rail = ui_scene_node(scene, UiSemantic::guild_rail);
    if (home && rail && navigation.home_badge &&
        !ui_mention_badge_text_output(navigation.home_badge, scene.widgets, ui_scene_widget(scene, UiSemantic::home_button),
                                      ui_mention_badge_guild_rectangle(home->rectangle, navigation.home_badge),
                                      rail->rectangle, stream))
        return reject(stream);
    const auto* guild_navigation = ui_scene_node(scene, UiSemantic::guild_navigation);
    if (guild_navigation && guild_scene && guild_scene->active) {
        for (u32 index = 0; index < guild_scene->count; ++index) {
            const auto& row = guild_scene->rows[index];
            const auto* guild = row.view.folder_role != GuildFolderRole::header && !row.view.has_icon
                                    ? chat_state_store_find_guild(state, row.view.id)
                                    : nullptr;
            if (guild) {
                const auto* layout = guild_rail_scene_layout_node(*guild_scene, row);
                if (!layout)
                    return reject(stream);
                char storage[32];
                u32 characters = 0;
                const auto acronym =
                    ui_guild_acronym(chat_state_store_guild_name(state, *guild), {storage, sizeof(storage)}, &characters);
                const auto clip = ui_rect_intersection(guild_navigation->rectangle, guild_scene->viewport);
                TextLook look = kLookCentered;
                // The official sizes by length: 18, 18, 16, 16, 14, 12 and 10
                // (MaiCord's smallest code, 12, for seven and more).
                look.style_flags = characters <= 2   ? text_style_heading_3
                                   : characters <= 4 ? text_style_none
                                   : characters == 5 ? kTextStyleSizeCodeSm
                                                     : kTextStyleSizeCodeXs;
                look.owned = true;
                // Text tiles take the accent, and its ink, while hovered or selected.
                const bool accent = !row.view.unavailable &&
                                    (row.view.selected || ui_widget_handle_equal(guild_scene->widgets.hovered, row.widget));
                const Color ink = accent ? theme.tokens.emphasis.text
                                  : row.view.unavailable || (row.view.muted && !row.view.selected) ? theme.text_secondary
                                                                                                   : theme.text_primary;
                if (!append_command(guild_scene->widgets, row.widget, acronym,
                                    guild_rail_icon_rectangle(layout->rectangle), clip, ink, stream, look))
                    return reject(stream);
            }
            if (!row.view.mention_count ||
                (row.view.folder_role == GuildFolderRole::header && row.view.folder_expanded))
                continue;
            const auto* layout = guild_rail_scene_layout_node(*guild_scene, row);
            if (!layout)
                return reject(stream);
            const auto rectangle = ui_mention_badge_guild_rectangle(guild_rail_icon_rectangle(layout->rectangle),
                                                                    row.view.mention_count);
            // The badge's text clips to the rail viewport like its painted
            // cut-out, which reaches past the tile.
            const auto clip = ui_rect_intersection(guild_navigation->rectangle, guild_scene->viewport);
            if (!ui_mention_badge_text_output(row.view.mention_count, guild_scene->widgets, row.widget,
                                              rectangle, clip, stream))
                return reject(stream);
        }
        // Plugin tiles' count badges.
        if (guild_scene->materialized_top || guild_scene->materialized_bottom)
            for (u32 end = 1; end <= 2; ++end)
                for (u32 index = 0; index < kGuildRailPluginTiles; ++index) {
                    GuildRailPluginTile tile;
                    if (!guild_rail_plugin_tile(*guild_scene, end, index, &tile) || !tile.node.badge) continue;
                    if (!ui_mention_badge_text_output(
                            tile.node.badge, guild_scene->widgets, tile.widget,
                            ui_mention_badge_guild_rectangle(tile.tile, tile.node.badge),
                            ui_rect_intersection(guild_navigation->rectangle, guild_scene->viewport), stream))
                        return reject(stream);
                }
    }
    // Count badges on plugin buttons: the account panel's and the chat bar's.
    if (ui_plugin_slot_surface_active(kUiPluginSlotAccountPanel) && navigation.voice_panel) {
        const auto* footer = ui_scene_node(scene, UiSemantic::channel_footer);
        UiPluginSlotNode nodes[kUiPluginSlotAccountButtons];
        const u32 count = footer ? ui_plugin_slot_account_buttons({nodes, kUiPluginSlotAccountButtons}) : 0;
        for (u32 i = 0; i < count; ++i)
            if (nodes[i].badge &&
                !ui_mention_badge_text_output(
                    nodes[i].badge, scene.widgets, ui_scene_widget(scene, UiSemantic::channel_footer),
                    ui_plugin_slot_count_rectangle(ui_plugin_slot_account_button(footer->rectangle, i), nodes[i].badge),
                    footer->rectangle, stream))
                return reject(stream);
    }
    if (composer_scene && ui_plugin_slot_surface_active(kUiPluginSlotComposerToolbar)) {
        UiPluginSlotNode nodes[kMessageComposerPluginSlots];
        const u32 count = minimum<u32>(composer_scene->plugin_count,
                                       ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {nodes, kMessageComposerPluginSlots}));
        for (u32 i = 0; i < count; ++i) {
            const auto* cell = ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->plugin_slots[i]);
            if (!cell || !nodes[i].badge) continue;
            const UiRect badge = ui_plugin_slot_count_rectangle(cell->rectangle, nodes[i].badge);
            if (!ui_mention_badge_text_output(nodes[i].badge, composer_scene->widgets, composer_scene->plugin_slots[i],
                                              badge, {badge.left - 1, badge.top - 1, badge.right + 1, badge.bottom + 1},
                                              stream))
                return reject(stream);
        }
    }
    if (channel_navigation && channel_scene.active) {
        for (u32 index = 0; index < channel_scene.count; ++index) {
            const ShellNavigationRowWidget& row_widget = channel_scene.rows[index];
            const ChannelViewRow& row = row_widget.view;
            const UiLayoutNode* layout = shell_navigation_scene_layout_node(channel_scene, row_widget);
            const UiLayoutNode* label_layout =
                ui_widget_manager_layout_node(channel_scene.widgets, row_widget.label);
            if (!layout || !label_layout)
                return reject(stream);
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
                const bool hovered = ui_widget_handle_equal(channel_scene.widgets.hovered, row_widget.widget);
                // Conversation rows centre the name, or stack it over a subtitle.
                char subtitle_storage[192];
                UiActivityGlyph subtitle_glyph = UiActivityGlyph::none;
                const Utf8View subtitle =
                    channel && channel_is_private(channel->type) && navigation.private_rows
                        ? ui_private_row_subtitle(*navigation.private_rows, channel->id,
                                                  {subtitle_storage, sizeof(subtitle_storage)}, &subtitle_glyph)
                        : Utf8View{};
                UiRect name_rectangle = label_layout->content_rectangle;
                // A plugin badge ends the name (and the DM subtitle) early.
                UiPluginSlotBadge plugin{};
                float text_right = label_layout->content_rectangle.right;
                if (shell_navigation_plugin_badge(channel_scene, index, &plugin)) {
                    text_right = plugin.badge.rectangle.left - 6.0f;
                    name_rectangle.right = minimum(name_rectangle.right, text_right);
                    if (!ui_badge_text_output(plugin.badge, channel_scene.widgets, theme, stream))
                        return reject(stream);
                }
                if (channel && channel_is_private(channel->type)) {
                    const UiRect content = label_layout->content_rectangle;
                    const float middle = (content.top + content.bottom) * 0.5f;
                    name_rectangle = subtitle.bytes ? UiRect{content.left, middle - 19.0f, text_right, middle + 1.0f}
                                                    : UiRect{content.left, middle - 10.0f, text_right, middle + 10.0f};
                    // The activity glyph (ui_private_rows_overlay_paint) leads the line.
                    UiRect subtitle_rectangle = ui_private_row_subtitle_rectangle(content);
                    subtitle_rectangle.right = text_right;
                    subtitle_rectangle = ui_activity_glyph_text(subtitle_rectangle, subtitle_glyph);
                    if (subtitle.bytes && ui_rect_has_area(subtitle_rectangle) &&
                        !append_command(channel_scene.widgets, row_widget.label, subtitle, subtitle_rectangle,
                                        visible_clip, ui_background_muted_text_color(theme), stream, kLookSub))
                        return reject(stream);
                }
                // A voice session's elapsed time ends the name line.
                char timer[kUiVoiceElapsedBytes];
                if (const u32 timer_bytes = ui_voice_timer_label(row, navigation.voice_extras, timer, sizeof(timer))) {
                    UiRect line = label_layout->content_rectangle;
                    line.right = text_right;
                    const UiRect box = ui_voice_elapsed_rectangle(line, timer_bytes, theme.scale.space_4, &text_right);
                    name_rectangle.right = minimum(name_rectangle.right, text_right);
                    if (!append_command(channel_scene.widgets, row_widget.label, {timer, timer_bytes}, box,
                                        visible_clip, theme.tokens.text.positive, stream, kLookTimer))
                        return reject(stream);
                }
                if (name.bytes != 0) {
                    const bool private_row = channel && channel_is_private(channel->type);
                    TextLook look = private_row ? kLookLineOwned : kLookTop;
                    UiTextEffect effect;
                    if (appearance) {
                        look.face = profile_name_face(appearance->name.font_id);
                        effect = profile_name_effect(appearance->name, hovered, channel_scene.widgets.hover_started_ms);
                        look.effect = &effect;
                    }
                    Color ink = color;
                    if (row.type == 4) {
                        // A category: 14-DIP text centred on its line, ending
                        // where the chevron starts (shell_category_geometry).
                        look = kLookLine;
                        look.style_flags = kTextStyleSizeCodeSm;
                        const auto geometry = shell_category_geometry(
                            layout->rectangle, row,
                            shell_text_width(measure, row.id, kShellMeasureCategoryName, name, kTextStyleSizeCodeSm,
                                             shell_category_name_space(layout->rectangle, row)));
                        name_rectangle = {geometry.name.left, geometry.name.top,
                                          minimum(geometry.chevron.left, geometry.name.right + 1.f),
                                          geometry.name.bottom};
                    } else if (!private_row) {
                        // Shown row controls end the name early.
                        name_rectangle.right = shell_navigation_controls_left(
                            layout->rectangle, row, shell_navigation_row_controls(row, hovered, row_selected),
                            name_rectangle.right);
                    }
                    // A hovered channel's name (and a category's) brightens.
                    if (hovered && !private_row)
                        ink = theme.text_primary;
                    if (!append_command(channel_scene.widgets, row_widget.label, name, name_rectangle, visible_clip,
                                        ink, stream, look))
                        return reject(stream);
                }
                // A live stage's topic sits on its own line under the name.
                if (const auto* instance = row.stage_live && navigation.stage_instances
                                               ? stage_instance_store_find(*navigation.stage_instances, row.id)
                                               : nullptr) {
                    const float left = layout->rectangle.left + kShellChannelLabelLeft +
                                       static_cast<float>(row.depth) * kShellChannelIndent;
                    const float top = layout->rectangle.top + kShellNavigationVoiceHeadHeight - 4.0f;
                    const UiRect topic{left, top, layout->rectangle.right - 10.0f,
                                       top + kShellNavigationStageTopicHeight};
                    if (!append_visible(channel_scene.widgets, row_widget.widget, stage_instance_topic(*instance),
                                        topic, channel_navigation->rectangle, ui_background_muted_text_color(theme),
                                        stream, kLookSub))
                        return reject(stream);
                }
                // A voice channel's status, or the connected channel's offer to set one.
                if (const auto* extras = row.voice_subtitle ? navigation.voice_extras : nullptr) {
                    const auto* info =
                        extras->info ? voice_channel_info_store_find(*extras->info, row.id) : nullptr;
                    char display[kVoiceChannelStatusMaximumBytes];
                    const u32 display_bytes =
                        info && info->status_bytes
                            ? voice_channel_status_display(voice_channel_info_status(*extras->info, *info),
                                                           {display, sizeof(display)})
                            : 0;
                    UiRect line = ui_voice_status_rectangle(row, layout->rectangle);
                    if (!display_bytes)
                        line.left = minimum(line.right, line.left + 18.0f); // After the pencil.
                    const bool hovered_line = extras->status_hovered && extras->status_channel_id == row.id;
                    if (!append_visible(channel_scene.widgets, row_widget.widget,
                                        display_bytes ? Utf8View{display, display_bytes}
                                                      : utf8_literal("Set a channel status"),
                                        line, channel_navigation->rectangle,
                                        hovered_line ? theme.text_primary : ui_background_muted_text_color(theme),
                                        stream, display_bytes ? kLookSubOwned : kLookSub))
                        return reject(stream);
                }
                UiVoiceInviteRow invite{};
                if (navigation.voice_extras && ui_voice_invite_row(row, layout->rectangle, &invite)) {
                    if (!append_visible(channel_scene.widgets, row_widget.widget, utf8_literal("Invite to Voice"),
                                        invite.label, channel_navigation->rectangle,
                                        navigation.voice_extras->invite_hovered ? theme.text_primary
                                                                                : theme.text_secondary,
                                        stream, kLookLine))
                        return reject(stream);
                }
                UiVoiceParticipantRow voice{};
                for (u32 i = 0; navigation.voice_states && users &&
                                ui_voice_participant_row(row, layout->rectangle, *navigation.voice_states, i, &voice);
                     ++i) {
                    const auto* user = user_store_find(*users, voice.state->user_id);
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
                    // "Role colors everywhere": a participant's name takes their colour role.
                    const Color voice_color = navigation.role_colors && member && member->role_color
                                                  ? packed_role_color(member->role_color, theme)
                                                  : ui_background_muted_text_color(theme);
                    if (!append_visible(channel_scene.widgets, row_widget.widget, voice_name, voice.label,
                                        channel_navigation->rectangle, voice_color, stream,
                                        kLookSub) ||
                        !append_visible(channel_scene.widgets, row_widget.widget, utf8_literal("LIVE"), voice.live,
                                        channel_navigation->rectangle, theme.tokens.text.on_accent, stream,
                                        kLookBadge))
                        return reject(stream);
                }
                if (row.mention_count) {
                    const auto* badge =
                        ui_widget_manager_layout_node(channel_scene.widgets, row_widget.mention_badge);
                    if (!badge ||
                        !ui_mention_badge_text_output(
                            row.mention_count, channel_scene.widgets, row_widget.mention_badge,
                            badge->rectangle,
                            ui_rect_intersection(badge->clip_rectangle, channel_navigation->rectangle),
                            stream))
                        return reject(stream);
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
        if (!edit || !send || !typing)
            return reject(stream);
        const Utf8View reply_text = message_composer_scene_reply_label(*composer_scene);
        const Utf8View attachments_text = message_composer_scene_attachment_label(*composer_scene);
        if (message_composer_scene_attachment_row(*composer_scene) == message_composer_attachment_row_strip) {
            if (!ui_composer_attachment_strip_text(message_composer_scene_attachment_view(*composer_scene, true),
                                                   theme, stream))
                return reject(stream);
        } else if (attachments_text.bytes) {
            const auto* attachments =
                message_composer_scene_layout_node(*composer_scene, MessageComposerControl::attachments);
            if (!attachments ||
                !append_command(composer_scene->widgets, composer_scene->attachments, attachments_text,
                                attachments->content_rectangle, attachments->clip_rectangle,
                                ui_background_muted_text_color(theme), stream, kLookLine))
                return reject(stream);
        }
        if (reply_text.bytes != 0 &&
            (!reply || !append_command(composer_scene->widgets, composer_scene->reply, reply_text,
                                       message_composer_scene_reply_text_rectangle(*composer_scene), reply->clip_rectangle,
                                       theme.text_secondary, stream, kLookLine)))
            return reject(stream);
        // The option bar: the name in the strong face, then its detail muted
        // (a failed check in the danger colour), 8 DIPs apart.
        const Utf8View bar_name = message_composer_scene_command_bar_name(*composer_scene);
        if (bar_name.bytes) {
            const auto* bar = ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->command_bar);
            const UiRect text = message_composer_scene_command_bar_text_rectangle(*composer_scene);
            const TextStyleSpan name_style{0, bar_name.bytes, text_style_bold};
            UiTextMeasureValue measured{};
            float name_width = ui_rect_width(text) * .5f;
            if (measure &&
                ui_text_measure_cache_measure(
                    measure,
                    {{0x434F4D4D414E44ull, hash64_bytes(kHash64Seed, bar_name.data, bar_name.bytes), 0, 0, 0},
                     bar_name, {&name_style, 1}, {maximum(1.f, ui_rect_width(text)), kMessageComposerCommandBarHeight},
                     true},
                    &measured)
                    .ok())
                name_width = minimum(ui_rect_width(text), measured.size.width);
            UiRect name_rectangle = text, detail_rectangle = text;
            name_rectangle.right = minimum(text.right, text.left + name_width + 1.f);
            detail_rectangle.left = minimum(text.right, name_rectangle.right + 8.f);
            const Color detail_color =
                composer_scene->command_bar_error ? theme.tokens.text.danger : ui_background_muted_text_color(theme);
            if (!bar ||
                !append_command(composer_scene->widgets, composer_scene->command_bar, bar_name, name_rectangle,
                                bar->clip_rectangle, theme.text_primary, stream, kLookBold) ||
                (ui_rect_has_area(detail_rectangle) &&
                 !append_command(composer_scene->widgets, composer_scene->command_bar,
                                 message_composer_scene_command_bar_detail(*composer_scene), detail_rectangle,
                                 bar->clip_rectangle, detail_color, stream, kLookLine)))
                return reject(stream);
        }
        const auto mention_text = message_composer_scene_reply_mention_label(*composer_scene);
        if (mention_text.bytes) {
            const auto* mention = message_composer_scene_layout_node(
                *composer_scene, MessageComposerControl::reply_mention);
            if (!mention || !append_command(composer_scene->widgets, composer_scene->reply_mention,
                mention_text, message_composer_scene_reply_mention_text(*composer_scene),
                mention->clip_rectangle,
                composer_scene->mention_reply_author ? theme.tokens.text.link : theme.tokens.text.muted,
                stream, kLookMention))
                return reject(stream);
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
                                            {&composer_scene->widgets, composer_scene->edit}, theme, stream))
                return reject(stream);
        } else if (!append_command(composer_scene->widgets, composer_scene->edit, edit_text, edit_text_rectangle,
                                   edit_text_clip, edit_color, stream,
                                   composer_scene->model->bytes == 0 ? kLookLine : kLookDraft)) {
            return reject(stream);
        }
        const Color typing_color = composer_scene->status_is_error || composer_scene->status_tone == 4
                                       ? theme.notification
                                   : composer_scene->status_tone == 3 ? theme.tokens.text.warning
                                   : composer_scene->status_tone == 2 ? theme.tokens.text.positive
                                   : composer_scene->status_tone == 1 ? theme.accent
                                                                      : theme.text_primary;
        if (!typing_output(*composer_scene, *typing, message_composer_scene_typing_label(*composer_scene),
                           typing_color, measure, stream))
            return reject(stream);
    }
    if (!footer_output(scene, state, navigation, theme, stream, users))
        return false;
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
    // A plugin's label in the strip above the list.
    {
        UiPluginSlotNode header;
        if (ui_plugin_member_list_header(&header)) {
            const UiRect strip{member_scene->viewport.left, member_scene->viewport.top - kUiPluginMemberListHeaderHeight,
                               member_scene->viewport.right, member_scene->viewport.top};
            TextLook look = kLookGroup;
            look.owned = true;
            if (!append_command(member_scene->widgets, member_scene->root, ui_plugin_slot_label(header),
                                {strip.left + 16.0f, strip.top, strip.right - 8.0f, strip.bottom}, strip,
                                ui_background_muted_text_color(theme), stream, look))
                return false;
        }
    }
    char group_storage[160];
    const auto* hovered_row = member_list_scene_resolve(*member_scene, member_scene->widgets.hovered);
    for (u32 index = 0; index < member_scene->count; ++index) {
        const auto& row = member_scene->rows[index];
        const auto* label = member_list_scene_layout_node(*member_scene, row.label);
        if (!member_list_scene_row_resident(*member_scene, index))
            continue;
        if (!label || row.source_index >= member_scene->source_count)
            return false;
        const bool group = row.kind == MemberListRowKind::group;
        const auto name = group ? member_list_group_label(*member_scene, index, {group_storage, sizeof(group_storage)})
                                : member_list_row_label(row.source_row, *member_scene->members,
                                                        *member_scene->users, *member_scene->state);
        auto color =
            group ? ui_background_muted_text_color(theme) : packed_role_color(row.source_row.role_color, theme);
        const bool offline = !group && row.source_row.status == GuildMemberStatus::offline;
        const bool hovered = hovered_row == &row;
        if (offline && !hovered)
            color.alpha = .4f;
        auto r = label->content_rectangle;
        const auto* global = user_appearance_find(member_scene->users->appearance, row.user_id);
        const auto* guild = user_appearance_find(member_scene->users->appearance, row.user_id,
                                                 member_scene->source->guild_id);
        char status_storage[kAppearanceActivityBytes];
        UiActivityStatus status{};
        const bool subtitle =
            !group && member_list_row_status(*member_scene, index, {status_storage, sizeof(status_storage)}, &status);
        if (subtitle)
            r = member_list_row_name_line(r);
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
        TextLook look = group ? kLookGroup : kLookLine;
        look.owned = group; // the heading is formatted into a local buffer
        if (!group && name_style)
            look.face = profile_name_face(name_style->name.font_id);
        look.effect = &name_effect;
        // A plugin's name prefix and suffix (a username beside the display name) join the label, which is then
        // formatted into a local buffer.
        char joined[224];
        Utf8View shown = name;
        UiPluginSlotNode affix[2];
        if (!group && ui_plugin_slots(kUiPluginSlotMemberName, 0, 0, row.user_id, {affix, 2}) == 2) {
            u32 used = 0;
            const auto put = [&](Utf8View text, bool space_before) noexcept {
                if (!text.bytes) return;
                const u32 room = sizeof(joined) - used;
                const u32 take = utf8_truncated_length(text, room > 1 ? room - 1 : 0);
                if (!take) return;
                if (space_before && used) joined[used++] = ' ';
                copy_bytes(joined + used, text.data, minimum(take, static_cast<u32>(sizeof(joined)) - used));
                used += minimum(take, static_cast<u32>(sizeof(joined)) - used);
            };
            put(affix[0].text, false);
            put(name, true);
            put(affix[1].text, true);
            shown = {joined, used};
            look.owned = true;
        }
        if (!append_command(member_scene->widgets, row.label, shown, r, label->clip_rectangle, color, stream, look))
            return false;
        if (subtitle) {
            // The activity glyph (shell_presentation_build_member_status_overlay) leads the line.
            auto sub = ui_activity_glyph_text(member_list_row_status_line(label->content_rectangle), status.glyph);
            if (status.emoji_id || status.emoji.bytes) {
                if (!status.emoji_id) {
                    auto emoji = sub;
                    emoji.right = emoji.left + 16;
                    if (!append_command(member_scene->widgets, row.label, status.emoji, emoji,
                                        label->clip_rectangle, theme.text_primary, stream, kLookCentered))
                        return false;
                }
                sub.left += 20;
            }
            if (status.text.bytes &&
                !append_command(member_scene->widgets, row.label, status.text, sub, label->clip_rectangle,
                                ui_background_muted_text_color(theme), stream, kLookSub))
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

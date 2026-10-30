#pragma once
#include "maicord/ui/badge.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/ui/role_color.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
struct MessageAuthorLayout {
    UiTextCommand name, timestamp;
    UiBadgeWidget badge;
    const UserAppearanceRecord* appearance;
    // Plugin message accessory, after the author badge.
    UiPluginSlotBadge plugin;
    // Plugin decorations (mc.msg.render): muted text around the name and up
    // to four badges after the accessory, read from the host cache.
    UiTextCommand name_prefix, name_suffix;
    UiPluginSlotBadge decorations[kUiPluginRenderBadges];
    u32 decoration_count;
};

// Plugin decoration tones are theme roles; MC_TONE_ROLE keeps the author colour.
inline Color message_author_decoration_color(const Theme& theme, u8 tone, const Color& author) noexcept {
    if (tone == kUiPluginToneRole)
        return author;
    return tone ? ui_icon_role_color(theme, ui_plugin_slot_role(tone)) : ui_background_muted_text_color(theme);
}

struct MessageProfileHit {
    u64 user_id{};
    UiRect anchor{};
};

// One header geometry for text, badge paint and its server icon.
inline bool message_author_layout(const MessageScene& scene, const MessageTextCommand& command,
                                  const ChatStateStore& state, const UserStore& users,
                                  UiTextMeasureCache* cache, const Theme& theme,
                                  MessageAuthorLayout* out,
                                  const GuildMemberStore* members = nullptr) noexcept {
    if (!out || command.measurement_kind != MessageTextMeasurementKind::none || !command.styles.count ||
        command.measurement_slot != 0 || !scene.timeline || !scene.timeline->store ||
        command.owner.manager != &scene.widgets)
        return false;
    const auto found = message_store_find(*scene.timeline->store, command.message_id);
    const auto* record = found.ok() ? message_store_resolve(*scene.timeline->store, found.handle) : nullptr;
    if (!record)
        return false;
    // Only the row's author widget carries the name header. Every other
    // unmeasured styled run (poll labels, card text) keeps its own style.
    bool author_owner = false;
    for (u32 i = 0; i < scene.count && !author_owner; ++i)
        author_owner = scene.rows[i].message_id == record->id &&
                       scene.rows[i].author.slot == command.owner.handle.slot &&
                       scene.rows[i].author.generation == command.owner.handle.generation;
    if (!author_owner)
        return false;
    const auto* global = user_appearance_find(users.appearance, record->author_id);
    const auto* channel = chat_state_store_find_channel(state, record->channel_id);
    const auto* guild =
        channel ? user_appearance_find(users.appearance, record->author_id, channel->guild_id) : nullptr;
    const auto* styled = guild && (guild->name.font_id || guild->name.effect_id) ? guild : global;
    const auto face = styled ? profile_name_face(styled->name.font_id) : UiTextFace::normal;
    const Utf8View name{command.text.data, minimum(command.text.bytes, command.styles[0].byte_count)};
    if (!name.bytes)
        return false;
    reset_value(out);
    out->appearance = global;
    UiPluginRenderDecoration decoration{};
    const bool decorated = message_scene_plugin_decoration(scene, record->id, record->revision, &decoration);
    const auto decoration_width = [&](Utf8View text, u16 salt) noexcept {
        if (!text.bytes)
            return 0.0f;
        const TextStyleSpan span{0, text.bytes, text_style_subtext};
        UiTextMeasureValue value{};
        const bool measured =
            cache && ui_text_measure_cache_measure(
                         cache,
                         {{record->author_id, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, salt, 0},
                          text,
                          {&span, 1},
                          {maximum(1.f, ui_rect_width(command.rectangle)), 64},
                          true},
                         &value)
                         .ok();
        return minimum(ui_rect_width(command.rectangle) * 0.25f, (measured ? value.size.width : text.bytes * 7.f) + 2);
    };
    const float prefix_width = decorated ? decoration_width(decoration.name_prefix, 0x4850) : 0.0f;
    const float suffix_width = decorated ? decoration_width(decoration.name_suffix, 0x4853) : 0.0f;
    const float decoration_space = (prefix_width ? prefix_width + 4 : 0) + (suffix_width ? suffix_width + 4 : 0);
    auto& badge = out->badge;
    badge.owner = command.owner.handle;
    badge.style = text_style_subtext | text_style_bold;
    if (global && global->bot) {
        ui_app_badge(&badge, global->public_flags);
    } else if (global && global->primary_enabled && global->primary_tag.bytes) {
        badge.leading = true;
        badge.label = global->primary_tag.view();
    }
    const float badge_width =
        badge.label.bytes ? ui_rect_width(ui_badge_widget_layout(cache, badge, command.rectangle).outer) : 0;
    const float name_limit =
        maximum(1.f, ui_rect_width(command.rectangle) - (badge_width ? badge_width + 6 : 0) - decoration_space);
    UiTextMeasureValue measured{};
    const TextStyleSpan bold{0, name.bytes, text_style_bold};
    const bool have_measure =
        cache && ui_text_measure_cache_measure(
                     cache,
                     {{record->author_id, hash64_bytes(kHash64Seed, name.data, name.bytes), 0, 0x4844, 0},
                      name,
                      {&bold, 1},
                      {name_limit, 64},
                      true,
                      face},
                     &measured)
                     .ok();
    auto bounds = command.rectangle;
    if (prefix_width) {
        const Color color = message_author_decoration_color(theme, decoration.name_tone, command.color);
        out->name_prefix = {decoration.name_prefix, command.owner,
                            {bounds.left, bounds.top, bounds.left + prefix_width, bounds.bottom}, command.clip, color,
                            text_style_subtext, UiTextAlignment::start, UiTextAlignment::center,
                            TextWrapping::no_wrap, TextOverflow::ellipsis};
        bounds.left += prefix_width + 4;
    }
    bounds.right =
        bounds.left + minimum(name_limit, have_measure ? measured.size.width + 2 : name.bytes * 8.f);
    const auto* hovered = message_scene_resolve(scene, scene.widgets.hovered);
    out->name = {name,
                 command.owner,
                 bounds,
                 command.clip,
                 command.color,
                 text_style_bold,
                 UiTextAlignment::start,
                 UiTextAlignment::center,
                 TextWrapping::no_wrap,
                 TextOverflow::ellipsis,
                 face,
                 styled && hovered && hovered->message_id == record->id
                     ? profile_name_effect(styled->name, true, scene.widgets.hover_started_ms)
                     : UiTextEffect{}};
    // Enhanced role styles paint the author's gradient or holographic name
    // unless a hovered display-name effect already owns it.
    if (!out->name.effect.kind && members && channel && channel->guild_id &&
        record->delivery == MessageDeliveryState::authoritative) {
        const auto* member = guild_member_store_find(*members, channel->guild_id, record->author_id);
        const auto* role = member && member->color_role_id
                               ? chat_state_store_find_role(state, channel->guild_id, member->color_role_id)
                               : nullptr;
        if (role && role->color == member->role_color)
            out->name.effect = ui_role_gradient_effect(theme, *role);
    }
    float tail_left = bounds.right + 8;
    if (suffix_width) {
        const Color color = message_author_decoration_color(theme, decoration.name_tone, command.color);
        const UiRect suffix{bounds.right + 4, bounds.top, minimum(command.rectangle.right, bounds.right + 4 + suffix_width),
                            bounds.bottom};
        out->name_suffix = {decoration.name_suffix, command.owner, suffix, command.clip, color, text_style_subtext,
                            UiTextAlignment::start, UiTextAlignment::center, TextWrapping::no_wrap,
                            TextOverflow::ellipsis};
        tail_left = suffix.right + 8;
    }
    if (badge_width) {
        auto line = command.rectangle;
        line.left = bounds.right + 6;
        line.right = minimum(command.rectangle.right, line.left + badge_width);
        badge.rectangle = ui_badge_widget_layout(cache, badge, line).outer;
        tail_left = badge.rectangle.right + 8;
    }
    auto& plugin = out->plugin;
    if (ui_plugin_slot_badge(kUiPluginSlotMessageAccessory, record->channel_id, record->id, 0, command.owner.handle,
                             &plugin)) {
        const auto& label = plugin.badge.label;
        const float width = ui_rect_width(ui_badge_layout(cache, label, command.rectangle, true, plugin.badge.style).outer);
        auto line = command.rectangle;
        line.left = tail_left - 2;
        line.right = minimum(command.rectangle.right, line.left + width);
        if (ui_rect_width(line) >= 40) {
            plugin.badge.rectangle = ui_badge_layout(cache, label, line, true, plugin.badge.style).outer;
            tail_left = plugin.badge.rectangle.right + 8;
        } else {
            reset_value(&plugin);
        }
    }
    for (u32 i = 0; decorated && i < decoration.badge_count && i < kUiPluginRenderBadges; ++i) {
        const auto& node = decoration.badges[i];
        const Utf8View label = node.text.bytes ? node.text : node.tooltip;
        if (!label.bytes)
            continue;
        auto& slot = out->decorations[out->decoration_count];
        slot.badge.owner = command.owner.handle;
        slot.badge.label = label;
        slot.badge.leading = true;
        slot.badge.style = text_style_subtext | text_style_bold;
        slot.icon = node.icon;
        slot.tone = node.tone;
        slot.owner_id = node.owner_id;
        const float width = ui_rect_width(ui_badge_layout(cache, label, command.rectangle, true, slot.badge.style).outer);
        auto line = command.rectangle;
        line.left = tail_left - 2;
        line.right = minimum(command.rectangle.right, line.left + width);
        if (ui_rect_width(line) < 40) {
            reset_value(&slot);
            break;
        }
        slot.badge.rectangle = ui_badge_layout(cache, label, line, true, slot.badge.style).outer;
        tail_left = slot.badge.rectangle.right + 8;
        ++out->decoration_count;
    }
    if (command.styles.count > 1 && tail_left < command.rectangle.right) {
        const auto& tail = command.styles[1];
        if (tail.byte_offset <= command.text.bytes &&
            tail.byte_count <= command.text.bytes - tail.byte_offset)
            out->timestamp = {
                {command.text.data + tail.byte_offset, tail.byte_count},
                command.owner,
                {tail_left, command.rectangle.top, command.rectangle.right, command.rectangle.bottom},
                command.clip,
                ui_background_muted_text_color(theme),
                tail.flags,
                UiTextAlignment::start,
                UiTextAlignment::center,
                TextWrapping::no_wrap,
                TextOverflow::ellipsis};
    }
    return true;
}

// Compact rows have no author line: the name is the bold role-colored prefix
// of the content flow's first line.
inline MessageProfileHit message_author_compact_hit(const MessageScene& scene, const MessageTextCommand& command,
                                                    UiTextMeasureCache* cache, float x, float y) noexcept {
    const auto* row = message_scene_resolve(scene, command.owner.handle);
    // A plugin name prefix is a leading non-bold span before the name.
    const u32 name_span = command.styles.count > 1 && !command.styles[0].byte_offset &&
                                  !(command.styles[0].flags & text_style_bold) &&
                                  (command.styles[1].flags & text_style_bold)
                              ? 1u
                              : 0u;
    if (!row || row->grouped || command.owner.manager != &scene.widgets ||
        (command.source_id && !ui_plugin_render_source(command.source_id)) ||
        command.owner.handle.slot != row->content.slot ||
        command.owner.handle.generation != row->content.generation || command.message_id != row->message_id ||
        command.message_revision != row->message_revision || !command.styles.count ||
        (!name_span && command.styles[0].byte_offset) ||
        !(command.styles[name_span].flags & text_style_inline_role_color) ||
        command.styles[name_span].byte_offset > command.text.bytes ||
        command.styles[name_span].byte_count > command.text.bytes - command.styles[name_span].byte_offset)
        return {};
    const auto* record = message_store_resolve(*scene.timeline->store, row->message);
    const Utf8View name{command.text.data + command.styles[name_span].byte_offset, command.styles[name_span].byte_count};
    if (!record || !record->author_id || !name.bytes)
        return {};
    const auto width = [&](Utf8View text, u32 flags, u16 salt, UiTextMeasureValue* value) noexcept {
        const TextStyleSpan span{0, text.bytes, flags};
        return cache && ui_text_measure_cache_measure(
                            cache,
                            {{record->author_id, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, salt, 0},
                             text,
                             {&span, 1},
                             {maximum(1.f, ui_rect_width(command.rectangle)), 64},
                             true},
                            value)
                            .ok();
    };
    auto bounds = command.rectangle;
    if (name_span) {
        // The prefix and its separating space shift the name right.
        UiTextMeasureValue prefix{};
        const Utf8View lead{command.text.data, command.styles[1].byte_offset};
        bounds.left = minimum(bounds.right,
                              bounds.left + (width(lead, command.styles[0].flags, 0x4851, &prefix) ? prefix.size.width
                                                                                                   : lead.bytes * 7.f));
    }
    UiTextMeasureValue measured{};
    const bool have_measure = width(name, text_style_bold, 0x4843, &measured);
    bounds.right = minimum(bounds.right, bounds.left + (have_measure ? measured.size.width + 2 : name.bytes * 8.f));
    bounds.bottom = minimum(bounds.bottom, bounds.top + (have_measure ? measured.size.height : 20.f));
    if (!ui_rect_contains(ui_rect_intersection(bounds, command.clip), x, y))
        return {};
    return {record->author_id, bounds};
}

// The row whose author line owns the command; null for grouped rows and foreign commands.
inline const MessageSceneRowWidget* message_author_command_row(const MessageScene& scene,
                                                               const MessageTextCommand& c) noexcept {
    const auto* row = message_scene_resolve(scene, c.owner.handle);
    return row && !row->grouped && c.owner.manager == &scene.widgets && c.owner.handle.slot == row->author.slot &&
                   c.owner.handle.generation == row->author.generation && c.message_id == row->message_id
               ? row
               : nullptr;
}

inline MessageProfileHit message_author_hit_test(const MessageScene& scene,
                                                 const MessageTextPresentation& text,
                                                 const ChatStateStore& state,
                                                 const UserStore& users,
                                                 UiTextMeasureCache* cache, const Theme& theme,
                                                 float x, float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store ||
        !ui_rect_contains(scene.viewport, x, y)) return {};
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        if (row.grouped) continue;
        const auto* avatar = message_scene_layout_node(scene, row.avatar);
        const auto* record = message_store_resolve(*scene.timeline->store, row.message);
        if (record && avatar && ui_rect_contains(ui_rect_intersection(avatar->rectangle,
                                                                     avatar->clip_rectangle), x, y))
            return {record->author_id, avatar->rectangle};
    }
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        const auto* row = message_author_command_row(scene, command);
        if (!row || command.message_revision != row->message_revision)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(scene, command, state, users, cache, theme, &header) ||
            !ui_rect_contains(ui_rect_intersection(header.name.rectangle, header.name.clip), x, y))
            continue;
        const auto* record = message_store_resolve(*scene.timeline->store, row->message);
        if (record) return {record->author_id, header.name.rectangle};
    }
    if (scene.display.compact)
        for (u32 i = 0; i < text.command_count; ++i)
            if (const auto hit = message_author_compact_hit(scene, text.commands[i], cache, x, y); hit.user_id)
                return hit;
    return {};
}

// The server tag chip on a message's author line: the tag's server and the
// chip (the server tag popout anchors to it). Bot "APP" badges are not tags.
struct MessageGuildTagHit {
    u64 guild_id;
    UiRect chip;
};
inline MessageGuildTagHit message_author_guild_tag_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                            const ChatStateStore& state, const UserStore& users,
                                                            UiTextMeasureCache* cache, const Theme& theme, float x,
                                                            float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !ui_rect_contains(scene.viewport, x, y))
        return {};
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (!message_author_command_row(scene, command))
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(scene, command, state, users, cache, theme, &header) || !header.badge.leading ||
            header.badge.accent ||
            !header.badge.label.bytes || !header.appearance || !header.appearance->primary_guild_id ||
            !ui_rect_contains(ui_rect_intersection(header.badge.rectangle, command.clip), x, y))
            continue;
        return {header.appearance->primary_guild_id, header.badge.rectangle};
    }
    return {};
}

// The check leading a verified app's APP tag on a message's author line: its
// tooltip reads "Verified App" (the client wraps only the check).
struct MessageAppCheckHit {
    UiWidgetReference owner;
    u64 message_id;
    u32 revision;
    UiRect check;
};
inline MessageAppCheckHit message_author_app_check_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                            const ChatStateStore& state, const UserStore& users,
                                                            UiTextMeasureCache* cache, const Theme& theme, float x,
                                                            float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !ui_rect_contains(scene.viewport, x, y))
        return {};
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        const auto* row = message_author_command_row(scene, command);
        if (!row || command.message_revision != row->message_revision)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(scene, command, state, users, cache, theme, &header))
            continue;
        const UiRect check = ui_rect_intersection(ui_app_badge_check(header.badge), command.clip);
        if (ui_rect_contains(check, x, y))
            return {command.owner, row->message_id, row->message_revision, check};
    }
    return {};
}

// A plugin accessory button on a message's author line.
struct MessagePluginSlotHit {
    u64 owner_id, action_id, channel_id, message_id;
};
inline MessagePluginSlotHit message_author_plugin_hit_test(const MessageScene& scene, const MessageTextPresentation& text,
                                                           const ChatStateStore& state, const UserStore& users,
                                                           UiTextMeasureCache* cache, const Theme& theme, float x,
                                                           float y) noexcept {
    if (!g_ui_plugin_slot_query || !scene.active || !scene.timeline || !scene.timeline->store ||
        !ui_rect_contains(scene.viewport, x, y)) return {};
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        const auto* row = message_author_command_row(scene, command);
        if (!row)
            continue;
        MessageAuthorLayout header{};
        if (!message_author_layout(scene, command, state, users, cache, theme, &header) || !header.plugin.action_id ||
            !ui_rect_contains(ui_rect_intersection(header.plugin.badge.rectangle, command.clip), x, y))
            continue;
        const auto* record = message_store_resolve(*scene.timeline->store, row->message);
        if (record) return {header.plugin.owner_id, header.plugin.action_id, record->channel_id, record->id};
    }
    return {};
}
} // namespace maicord

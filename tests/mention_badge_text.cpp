#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/shell_text_presentation.hpp"

namespace maicord {
namespace mention_text_probe {
struct Fixture {
    UiScene shell;
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[6];
    u16 guild_order[1], channel_order[6];
    u8 names[1024];
    NavigationView view;
    GuildViewRow guild_rows[5];
    ChannelViewRow channel_rows[6];
    GuildRailScene rail;
    GuildRailRowWidget rail_rows[5];
    UiWidget rail_widgets[7];
    UiLayoutNode rail_nodes[7];
    ShellNavigationScene navigation;
    ShellNavigationRowWidget navigation_rows[6];
    UiWidget navigation_widgets[26];
    UiLayoutNode navigation_nodes[26];
    UiTextCommand commands[32];
    char text[64];
    UiTextCommandStream stream;
};
Fixture fixture{};
constexpr u32 counts[]{1, 12, 999, 1000, ~u32{0}};
constexpr Utf8View labels[]{utf8_literal("1"), utf8_literal("12"), utf8_literal("999"),
    utf8_literal("1k+"), utf8_literal("9k+")};
bool initialize() noexcept {
    auto& f = fixture;
    if (!ui_scene_initialize_bootstrap(&f.shell) || !ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280, 800) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
            {f.channels, 6}, {f.channel_order, 6}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() ||
        !guild_rail_scene_initialize(&f.rail, {f.rail_rows, 5}, {f.rail_widgets, 7}, {f.rail_nodes, 7}) ||
        !shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, 6},
            {f.navigation_widgets, 26}, {f.navigation_nodes, 26}) ||
        !ui_text_command_stream_initialize(&f.stream, {f.commands, 32}, {f.text, sizeof(f.text)})) return false;
    f.view = {};
    f.view.guild_rows = f.guild_rows; f.view.guild_count = f.view.guild_capacity = 5;
    f.view.channel_rows = f.channel_rows; f.view.channel_count = f.view.channel_capacity = 6;
    f.view.selected_guild_id = 7; f.view.guild_revision = f.view.channel_revision = 1;
    for (u32 index = 0; index < 6; ++index) {
        ChannelInput input{};
        input.id = 80 + index; input.guild_id = 7; input.type = 0;
        input.name = utf8_literal("Channel label"); input.name_known = true;
        if (!chat_state_store_upsert_channel(&f.state, input).ok()) return false;
        f.channel_rows[index] = {}; f.channel_rows[index].id = input.id;
        f.channel_rows[index].position = index; f.channel_rows[index].selectable = true;
        f.channel_rows[index].unread = index == 5;
        f.channel_rows[index].mention_count = index < 5 ? counts[index] : 0;
        if (index < 5) {
            f.guild_rows[index] = {}; f.guild_rows[index].id = 7 + index;
            f.guild_rows[index].mention_count = counts[index];
        }
    }
    return true;
}
bool update() noexcept {
    auto& f = fixture;
    const auto* guild = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::guild_navigation));
    const auto* channels = ui_scene_resolve(f.shell, ui_scene_find(f.shell, UiSemantic::channel_navigation));
    return guild && channels && guild_rail_scene_update(&f.rail, &f.view, guild->rectangle, 0).ok() &&
        shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok() &&
        shell_text_presentation_build(f.shell, f.state, f.view, kDefaultTheme,
            f.navigation, nullptr, nullptr, nullptr, &f.stream, nullptr, &f.rail);
}
// Rail rows also own their server acronym; a badge is the command in the badge slot.
const UiTextCommand* owned(const UiWidgetManager& manager, UiWidgetHandle handle, const UiRect* slot = nullptr) noexcept {
    auto& f = fixture;
    for (u32 index = 0; index < f.stream.count; ++index) {
        const auto& command = f.commands[index];
        if (command.owner.manager == &manager && command.owner.handle.slot == handle.slot &&
            command.owner.handle.generation == handle.generation &&
            (!slot || ui_rect_equal(command.rectangle, *slot))) return &command;
    }
    return nullptr;
}
bool numeric_commands_absent() noexcept {
    auto& f = fixture;
    for (u32 index = 0; index < f.stream.count; ++index)
        if (f.commands[index].style_flags == (text_style_subtext | text_style_bold)) return false;
    return true;
}
bool numeric(const UiTextCommand* command, Utf8View label, UiRect rectangle) noexcept {
    auto& f = fixture;
    return command && utf8_equal(command->text, label) && ui_rect_equal(command->rectangle, rectangle) &&
        command->horizontal_alignment == UiTextAlignment::center && command->vertical_alignment == UiTextAlignment::center &&
        command->style_flags == (text_style_subtext | text_style_bold) &&
        command->color.red == 1 && command->color.green == 1 && command->color.blue == 1 &&
        command->text.data >= f.text && command->text.data + command->text.bytes <= f.text + sizeof(f.text);
}
} // namespace mention_text_probe

void test_mention_badge_text(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace mention_text_probe;
    char label[3] = {'x', 'x', 'x'};
    check(!ui_mention_badge_label(0, {label, 3}).bytes && !ui_mention_badge_label(100, {label, 2}).bytes &&
        label[0] == 'x' && label[1] == 'x' && label[2] == 'x',
        "mention badge: zero count and insufficient label backing do not publish or overwrite text");
    const u32 edges[]{9, 10, 99, 100, 999, 1000, 8999, 9000, ~u32{0}};
    constexpr Utf8View expected[]{utf8_literal("9"), utf8_literal("10"), utf8_literal("99"), utf8_literal("100"),
        utf8_literal("999"), utf8_literal("1k+"), utf8_literal("8k+"), utf8_literal("9k+"), utf8_literal("9k+")};
    for (u32 index = 0; index < 9; ++index)
        check(utf8_equal(ui_mention_badge_label(edges[index], {label, 3}), expected[index]),
            "mention badge: public decimal and bounded thousands formatter preserves all threshold behavior");
    check(ui_mention_badge_width(9) == 16 && ui_mention_badge_width(10) == 22 &&
        ui_mention_badge_width(99) == 22 && ui_mention_badge_width(100) == 30 &&
        ui_mention_badge_width(~u32{0}) == 30,
        "mention badge: public single two and three-character widths stay bounded");
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized && update(), "mention badge: production shell text binds guild and channel native scenes");
    if (initialized) {
        for (u32 index = 0; index < 5; ++index) {
            const auto& rail = f.rail.rows[index];
            const auto* rail_layout = guild_rail_scene_layout_node(f.rail, rail);
            const auto& channel = f.navigation.rows[index];
            const auto* badge = ui_widget_manager_layout_node(f.navigation.widgets, channel.mention_badge);
            const auto* name = ui_widget_manager_layout_node(f.navigation.widgets, channel.label);
            const UiRect slot = rail_layout ? ui_mention_badge_guild_rectangle(rail_layout->rectangle, counts[index]) : UiRect{};
            check(rail_layout && numeric(owned(f.rail.widgets, rail.widget, &slot), labels[index], slot),
                "mention badge: guild numeric text owns copied UTF8 and matches painted badge geometry");
            check(badge && name && numeric(owned(f.navigation.widgets, channel.mention_badge), labels[index], badge->rectangle) &&
                ui_rect_width(badge->rectangle) == ui_mention_badge_width(counts[index]) &&
                name->content_rectangle.right <= badge->rectangle.left - 8.0f,
                "mention badge: channel numeric text matches widget badge and reserves name spacing at every width");
            check(f.guild_rows[index].mention_count == counts[index] && f.channel_rows[index].mention_count == counts[index],
                "mention badge: compact numeric labels never replace complete projected model counts");
        }
        // Layering: the chrome paints no rail badge (the server icon's media
        // draws over the chrome); the post-media overlay paints each badge
        // with its rail-coloured cutout ring above the icon.
        static PaintStream chrome{}, overlay{};
        ShellPresentationRegions regions{};
        const bool painted = paint_stream_initialize(&chrome) && paint_stream_initialize(&overlay) &&
                             shell_presentation_build_chrome(f.shell, &f.view, &f.rail, &f.navigation, nullptr, nullptr,
                                                             kDefaultTheme, 1280, 800, &chrome, &regions, nullptr) &&
                             paint_stream_reset(&overlay, 1280, 800) &&
                             shell_presentation_build_guild_rail_overlay(f.shell, &f.rail, kDefaultTheme, &overlay);
        const auto badge_quads = [&](const PaintStream& paint) noexcept {
            u32 found = 0;
            for (u32 index = 0; index < 5; ++index) {
                const auto* layout = guild_rail_scene_layout_node(f.rail, f.rail.rows[index]);
                const UiRect slot = layout ? ui_mention_badge_guild_rectangle(guild_rail_icon_rectangle(layout->rectangle),
                                                                              counts[index])
                                           : UiRect{};
                for (u32 q = 0; q < paint.count; ++q) {
                    const auto& quad = paint.quads[q];
                    if (quad.fill.red == kDefaultTheme.notification.red &&
                        quad.fill.green == kDefaultTheme.notification.green &&
                        quad.fill.blue == kDefaultTheme.notification.blue && quad.border_width > 0 &&
                        quad.left <= slot.left && quad.right >= slot.right && quad.top <= slot.top &&
                        quad.bottom >= slot.bottom) {
                        ++found;
                        break;
                    }
                }
            }
            return found;
        };
        check(painted && badge_quads(chrome) == 0 && badge_quads(overlay) == 5,
            "mention badge: rail badges and their cutout ring paint after the server icons, not under them");
        paint_stream_release(&chrome);
        paint_stream_release(&overlay);
        const auto& unread = f.navigation.rows[5];
        check(!unread.mention_badge.generation && unread.view.unread,
            "mention badge: unread white-dot-only channel creates no red badge or number");
        for (auto& row : f.guild_rows) row.mention_count = 0;
        for (auto& row : f.channel_rows) row.mention_count = 0;
        ++f.view.guild_revision; ++f.view.channel_revision;
        check(update() && numeric_commands_absent(),
            "mention badge: read-state transition removes guild and channel numeric commands and their owned bytes");
        for (u32 index = 0; index < f.navigation.count; ++index)
            check(!f.navigation.rows[index].mention_badge.generation,
                "mention badge: read channel materialization releases old badge widget identities");
    }
    ui_scene_release(&f.shell);
}
} // namespace maicord

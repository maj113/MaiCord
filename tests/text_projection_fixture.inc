// Shared retained shell/friends fixture for the text emitter test and the
// text projection benchmark. Include inside the test namespace after the
// friends_scene, mention_badge and shell_text_presentation headers.
static_assert(sizeof(UiTextCommand) == 160 && sizeof(UiTextEffect) == 48);
struct Fixture {
    UiScene shell;
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[192];
    u16 guild_order[1], channel_order[192];
    u8 names[8192];
    NavigationView view;
    GuildViewRow guild_rows[192];
    ChannelViewRow channel_rows[192];
    GuildRailScene rail;
    GuildRailRowWidget rail_rows[192];
    UiWidget rail_widgets[194];
    UiLayoutNode rail_nodes[194];
    ShellNavigationScene navigation;
    ShellNavigationRowWidget navigation_rows[192];
    UiWidget navigation_widgets[770];
    UiLayoutNode navigation_nodes[770];
    UiFriendsScene friends;
    UiFriendsSource source;
    u32 resolutions;
    UiTextCommand commands[1500];
    char text[4096];
    TextStyleSpan styles[2];
    UiTextCommandStream stream;
};
Fixture fixture{};
bool resolve_friend(void* context, u32 index, UiFriendsRow* out) noexcept {
    ++static_cast<Fixture*>(context)->resolutions;
    if (index >= 192) return false;
    *out = {index + 1ull, utf8_literal("Retained friend"), utf8_literal("friend.username"), {},
        UiPresence::online, UiFriendsKind::incoming, false};
    return true;
}
bool initialize() noexcept {
    auto& f = fixture;
    if (!ui_scene_initialize_bootstrap(&f.shell) ||
        !ui_scene_layout_with_metrics(&f.shell, UiShellView::friends, kFriendsShellLayoutMetrics, 1280, 8192) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
            {f.channels, 192}, {f.channel_order, 192}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() ||
        !guild_rail_scene_initialize(&f.rail, {f.rail_rows, 192}, {f.rail_widgets, 194}, {f.rail_nodes, 194}) ||
        !shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, 192},
            {f.navigation_widgets, 770}, {f.navigation_nodes, 770}) ||
        !ui_text_command_stream_initialize(&f.stream, {f.commands, 1500}, {f.text, sizeof(f.text)})) return false;
    f.view = {};
    f.view.guild_rows = f.guild_rows; f.view.guild_count = f.view.guild_capacity = 192;
    f.view.channel_rows = f.channel_rows; f.view.channel_count = f.view.channel_capacity = 192;
    f.view.selected_guild_id = 7; f.view.guild_revision = f.view.channel_revision = 1;
    for (u32 index = 0; index < 192; ++index) {
        ChannelInput input{};
        input.id = 80 + index; input.guild_id = 7; input.type = 0;
        input.name = utf8_literal("Channel label"); input.name_known = true;
        if (!chat_state_store_upsert_channel(&f.state, input).ok()) return false;
        f.channel_rows[index] = {}; f.channel_rows[index].id = input.id;
        f.channel_rows[index].position = index; f.channel_rows[index].selectable = true;
        f.channel_rows[index].mention_count = 1000;
        f.guild_rows[index] = {}; f.guild_rows[index].id = 7 + index;
        f.guild_rows[index].mention_count = 142;
    }
    const auto* guild = ui_scene_node(f.shell, UiSemantic::guild_navigation);
    const auto* channels = ui_scene_node(f.shell, UiSemantic::channel_navigation);
    const auto* content = ui_scene_node(f.shell, UiSemantic::content);
    f.source = {&f, 192, 1, resolve_friend};
    return guild && channels && content &&
        guild_rail_scene_update(&f.rail, &f.view, guild->rectangle, 0).ok() &&
        shell_navigation_scene_update(&f.navigation, &f.view, channels->rectangle, 0).ok() &&
        ui_friends_scene_update(&f.friends, &f.source, UiFriendsTab::pending, UiFriendsPhase::ready, {}, content->rectangle);
}
bool tall_projection(UiTextOutput* output) noexcept {
    auto& f = fixture;
    return shell_text_presentation_build_output(f.shell, f.state, f.view, kDefaultTheme,
        f.navigation, nullptr, nullptr, nullptr, output, nullptr, &f.rail) &&
        ui_friends_scene_text_output(f.friends, kDefaultTheme, output, nullptr);
}

// Unread banner, guild folders, invite links and taskbar attention.
#include "maicord/app/gateway_guild_folders.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/app/unread_banner_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild_display_order.hpp"
#include "maicord/platform/taskbar_attention.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::navigation_gaps_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}

// ---- unread banner -------------------------------------------------------
constexpr u64 kChannel = 900;
u64 snowflake(u64 ms) noexcept {
    return ms << 22;
}
struct Timeline {
    MessageRecord records[16];
    u32 order[16];
    u8 text[256];
    u32 units[16];
    u64 fenwick[16];
    MessageStore store;
    UiVariableList list;
};
bool timeline(Timeline& t, u32 count, u64 first_ms) noexcept {
    if (!message_store_initialize(&t.store, {t.records, 16}, {t.order, 16}, {t.text, 256}))
        return false;
    for (u32 i = 0; i < count; ++i) {
        MessageInput input{snowflake(first_ms + i * 1000), kChannel, 7, 0, 0, utf8_literal("x"), 0};
        if (!message_store_upsert(&t.store, input).ok())
            return false;
    }
    return ui_variable_list_initialize(&t.list, {t.units, 16}, {t.fenwick, 16}, count, ui_extent_units(100));
}
UnreadBannerInput banner_input(Timeline& t, u64 last_read, u64 last_message, u64 scroll_dips) noexcept {
    return {&t.store, &t.list, kChannel, last_read, last_message, ui_extent_units(static_cast<float>(scroll_dips)),
            ui_extent_units(300), true, true, false, true};
}

void unread_banner() noexcept {
    Timeline t{};
    check(timeline(t, 10, 1'000), "banner fixture timeline");
    const u64 bottom = 1000 - 300;
    // Messages 0..9; read through message 5, four unread remain resident.
    auto in = banner_input(t, snowflake(1'000 + 5'000), snowflake(1'000 + 9'000), bottom);
    auto p = unread_banner_project(in);
    check(p.view.banner && p.view.count == 4 && !p.view.count_estimated && p.first_unread_id == snowflake(7'000) &&
              p.view.since_unix_ms == snowflake_timestamp_ms(snowflake(7'000)) && !p.view.jump,
          "banner counts resident unread messages since the first unread");
    // The cursor predates the resident window and older history is not loaded.
    in = banner_input(t, snowflake(500), snowflake(10'000), 0);
    in.older_exhausted = false;
    p = unread_banner_project(in);
    check(p.view.banner && p.view.count == 10 && p.view.count_estimated &&
              p.view.since_unix_ms == snowflake_timestamp_ms(snowflake(500) + 1),
          "a cursor older than the resident window yields an estimated count since the cursor");
    in = banner_input(t, snowflake(10'000), snowflake(10'000), 0);
    check(!unread_banner_project(in).view.jump, "two viewports from the bottom keep the jump bar hidden");
    in.viewport_units = ui_extent_units(100);
    p = unread_banner_project(in);
    check(!p.view.banner && p.view.jump && !p.view.count,
          "read channels hide the bar; three viewports from the bottom show the jump bar");
    in = banner_input(t, snowflake(1'000), snowflake(10'000), bottom);
    in.read_state_known = false;
    check(!unread_banner_project(in).view.banner, "unknown read state never shows the bar");
    in = banner_input(t, snowflake(10'000), snowflake(10'000), bottom);
    in.newer_detached = true;
    p = unread_banner_project(in);
    check(p.view.jump && !p.view.banner, "detached history always offers jump to present");
    in = banner_input(t, 0, snowflake(10'000), bottom);
    in.older_exhausted = true;
    p = unread_banner_project(in);
    check(p.view.count == 10 && !p.view.count_estimated && p.first_unread_id == snowflake(1'000),
          "a never-read channel with complete history counts every message");

    // Escape: mark read and return to the live edge only when useful.
    auto plan = unread_banner_escape_plan(banner_input(t, snowflake(5'000), snowflake(10'000), bottom));
    check(plan.mark_read && !plan.jump_to_present, "escape at the bottom only marks read");
    plan = unread_banner_escape_plan(banner_input(t, snowflake(10'000), snowflake(10'000), 100));
    check(!plan.mark_read && plan.jump_to_present, "escape while scrolled up jumps to present");
    plan = unread_banner_escape_plan(banner_input(t, snowflake(10'000), snowflake(10'000), bottom));
    check(!plan.any(), "escape with nothing to do passes through");

    char label[kUiUnreadBannerLabelBytes];
    u32 bytes = ui_unread_banner_format_label(1, false, utf8_literal("3:04 PM"), {label, sizeof(label)});
    check(utf8_equal({label, bytes}, utf8_literal("1 new message since 3:04 PM")), "singular banner label");
    bytes = ui_unread_banner_format_label(50, true, utf8_literal("3:04 PM on October 5, 2026"), {label, sizeof(label)});
    check(utf8_equal({label, bytes}, utf8_literal("50+ new messages since 3:04 PM on October 5, 2026")),
          "estimated banner label with a date");
    check(!ui_unread_banner_format_label(3, false, {}, {label, 4}), "short label output writes nothing");

    // Scene: settles immediately without motion, hit tests by region, slides out.
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    UiWidgetManager manager{};
    check(ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2}), "banner owner manager");
    UiLayoutStyle style = ui_layout_style();
    const auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
                                            {1, UiWidgetKind::container, ui_widget_enabled,
                                             {utf8_literal("Messages"), style, {}, 0, 1}});
    check(root.ok(), "banner owner widget");
    UiUnreadBannerScene scene{};
    UiUnreadBannerView view{kChannel, 1, 4, false, true, true};
    const UiRect viewport{100, 50, 900, 650};
    check(ui_unread_banner_update(&scene, view, utf8_literal("4 new messages since 3:04 PM"), viewport,
                                  {&manager, root.handle}, 1000, false) &&
              ui_unread_banner_visible(scene) && scene.banner.top == 50 && scene.pill.bottom == 650 - 12,
          "banner and jump bar settle without motion");
    check(ui_unread_banner_hit_test(scene, 200, 60) == UiUnreadBannerAction::jump_to_unread &&
              ui_unread_banner_hit_test(scene, scene.banner_action.left + 4, 60) == UiUnreadBannerAction::mark_read &&
              ui_unread_banner_hit_test(scene, 500, scene.pill.top + 5) == UiUnreadBannerAction::jump_to_present &&
              ui_unread_banner_hit_test(scene, 500, 300) == UiUnreadBannerAction::none,
          "banner regions map to jump, mark read and jump to present");
    view.banner = false;
    view.jump = false;
    check(ui_unread_banner_update(&scene, view, {}, viewport, {&manager, root.handle}, 2000, true) &&
              scene.banner_motion.active && ui_unread_banner_visible(scene) && scene.label_bytes &&
              ui_unread_banner_hit_test(scene, 200, 60) == UiUnreadBannerAction::none,
          "retiring bars animate out with their label and ignore pointer input");
    check(ui_unread_banner_next_wake(scene, 2000, true) != 0, "running transitions request a wake");
    ui_unread_banner_update(&scene, view, {}, viewport, {&manager, root.handle}, 2000 + kUiUnreadBannerMotionMs, true);
    check(!ui_unread_banner_visible(scene) && !ui_unread_banner_next_wake(scene, 2200, true),
          "bars disappear after the 150 ms transition");
    view = {kChannel + 1, 0, 2, false, true, false};
    ui_unread_banner_update(&scene, view, utf8_literal("2 new messages"), viewport, {&manager, root.handle}, 3000,
                            true);
    check(scene.banner_motion.value == 0 && scene.banner_motion.active, "a new channel slides in from hidden");
}

// ---- guild folders -------------------------------------------------------
struct Writer {
    u8 data[1024];
    u32 used;
    void byte(u8 value) noexcept { data[used++] = value; }
    void varint(u64 value) noexcept {
        do {
            byte(static_cast<u8>((value & 127) | (value > 127 ? 128 : 0)));
            value >>= 7;
        } while (value);
    }
    void tag(u32 field, u32 type) noexcept { varint(field << 3 | type); }
    void fixed64(u64 value) noexcept {
        for (u32 i = 0; i < 8; ++i)
            byte(static_cast<u8>(value >> (i * 8)));
    }
    void bytes(u32 field, const Writer& nested) noexcept {
        tag(field, 2);
        varint(nested.used);
        for (u32 i = 0; i < nested.used; ++i)
            byte(nested.data[i]);
    }
    void text(u32 field, Utf8View value) noexcept {
        tag(field, 2);
        varint(value.bytes);
        for (u32 i = 0; i < value.bytes; ++i)
            byte(static_cast<u8>(value.data[i]));
    }
    void wrapped(u32 field, u64 value) noexcept {
        Writer w{};
        w.tag(1, 0);
        w.varint(value);
        bytes(field, w);
    }
};
Writer folder_fixture() noexcept {
    Writer a{}, ids{}, name{}, b{}, c{}, cids{}, positions{}, unknown{}, group{};
    ids.fixed64(30);
    ids.fixed64(40);
    a.bytes(1, ids);
    a.wrapped(2, 7);
    name.text(1, utf8_literal("Games"));
    a.bytes(3, name);
    a.wrapped(4, 0xFF0000);
    a.tag(9, 0); // unknown folder field
    a.varint(5);
    b.tag(1, 1); // unpacked single guild id without a folder id
    b.fixed64(20);
    cids.fixed64(50);
    cids.fixed64(60);
    c.bytes(1, cids);
    c.wrapped(2, 9);
    c.wrapped(4, kGuildFolderDefaultColor);
    group.bytes(1, a);
    group.bytes(1, b);
    group.bytes(1, c);
    positions.fixed64(20);
    positions.fixed64(30);
    group.bytes(2, positions);
    unknown.text(1, utf8_literal("future"));
    group.bytes(7, unknown);
    return group;
}

struct State {
    GuildRecord guilds[8];
    u16 guild_order[8];
    ChannelRecord channels[8];
    u16 channel_order[8];
    u8 names[2048];
    ChatStateStore store;
};
bool state(State& s, ConstSlice<u64> ids) noexcept {
    if (!chat_state_store_initialize(&s.store, {s.guilds, 8}, {s.guild_order, 8}, {s.channels, 8},
                                        {s.channel_order, 8}, {s.names, 2048}))
        return false;
    const char* names[] = {"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel"};
    for (u32 i = 0; i < ids.count; ++i) {
        GuildInput input{ids[i], {names[i], c_string_length(names[i])}, GuildMutationKind::create, 1};
        if (!chat_state_store_upsert_guild(&s.store, input).ok())
            return false;
    }
    return true;
}

void guild_folders() noexcept {
    const auto group = folder_fixture();
    Writer settings{}, versions{}, status{};
    versions.tag(1, 0);
    versions.varint(3);
    settings.bytes(1, versions);
    settings.bytes(kGuildFoldersSettingsField, group);
    status.text(1, utf8_literal("online"));
    settings.bytes(11, status);
    char encoded[2048];
    const auto base64 = base64_encode({settings.data, settings.used}, {encoded, sizeof(encoded)});
    u8 extracted[1024];
    u32 bytes = 0;
    bool present = false;
    check(base64.ok() &&
              guild_folders_extract_base64({encoded, base64.bytes}, {extracted, sizeof(extracted)}, &bytes, &present) ==
                  GuildFoldersError::none &&
              present && bytes == group.used && bytes_equal(extracted, group.data, bytes),
          "field 14 is extracted verbatim, unknown nested fields included");
    // {1: 3} only; then a truncated field 14.
    check(guild_folders_extract_base64(utf8_literal("CAM="), {extracted, sizeof(extracted)}, &bytes, &present) ==
                  GuildFoldersError::none &&
              !present && !bytes,
          "absent folders report not present");
    check(guild_folders_extract_base64(utf8_literal("chAKAg=="), {extracted, sizeof(extracted)}, &bytes,
                                       &present) == GuildFoldersError::protobuf,
          "truncated settings are rejected");

    static GuildFolderSettings decoded;
    check(guild_folders_decode({group.data, group.used}, &decoded) == GuildFoldersError::none && decoded.present &&
              decoded.folder_count == 3 && decoded.guild_id_count == 5 && decoded.position_count == 2,
          "folders, guild ids and positions decode");
    const auto& a = decoded.folders[0];
    const auto& b = decoded.folders[1];
    const auto& c = decoded.folders[2];
    check(a.has_id && a.id == 7 && a.has_color && a.color == 0xFF0000 && a.guild_count == 2 &&
              utf8_equal({a.name, a.name_bytes}, utf8_literal("Games")) && decoded.guild_ids[a.first_guild] == 30,
          "folder wrappers decode id, name and colour");
    check(!b.has_id && b.guild_count == 1 && decoded.guild_ids[b.first_guild] == 20 && c.has_id && !c.has_color,
          "unpacked ids decode and the brand colour means no colour");
    // A malformed known field rejects the group without touching output.
    Writer bad{}, bad_folder{};
    bad_folder.tag(2, 0); // id must be a wrapper message
    bad_folder.varint(1);
    bad.bytes(1, bad_folder);
    const u32 before = decoded.folder_count;
    check(guild_folders_decode({bad.data, bad.used}, &decoded) == GuildFoldersError::protobuf &&
              decoded.folder_count == before && decoded.folders[0].id == 7,
          "malformed folders leave the previous layout untouched");
    Writer odd{}, odd_folder{};
    odd_folder.tag(1, 2);
    odd_folder.varint(5); // packed fixed64 length not a multiple of 8
    for (u32 i = 0; i < 5; ++i)
        odd_folder.byte(0);
    odd.bytes(1, odd_folder);
    check(guild_folders_decode({odd.data, odd.used}, &decoded) == GuildFoldersError::protobuf,
          "packed ids must hold whole fixed64 values");

    // Display order: unlisted guilds prepend in reverse, missing guilds drop.
    State s{};
    const u64 ids[] = {10, 20, 30, 40, 50, 70};
    check(state(s, {ids, 6}), "folder state fixture");
    GuildDisplayEntry entries[16]{};
    const u32 count = guild_display_order_build(&decoded, s.store, {entries, 16});
    check(count == 6 && entries[0].guild_id == 70 && entries[1].guild_id == 10 && entries[2].guild_id == 30 &&
              entries[2].folder == 0 && entries[3].guild_id == 40 && entries[4].guild_id == 20 &&
              entries[4].folder == kGuildDisplayNoFolder && entries[5].guild_id == 50 && entries[5].folder == 2,
          "display order follows folders and prepends unlisted guilds");
    static GuildFolderSettings positions_only;
    positions_only = {};
    positions_only.present = true;
    positions_only.position_count = 2;
    positions_only.positions[0] = 40;
    positions_only.positions[1] = 10;
    const u32 positioned = guild_display_order_build(&positions_only, s.store, {entries, 16});
    check(positioned == 6 && entries[0].guild_id == 70 && entries[4].guild_id == 40 && entries[5].guild_id == 10,
          "guild positions order the list when there are no folders");
    check(guild_display_order_build(nullptr, s.store, {entries, 16}) == 6 && entries[0].guild_id == 10 &&
              entries[5].guild_id == 70,
          "without settings the join order is kept");
    check(s.store.guild_order[0] == 0 && chat_state_store_find_guild(s.store, 70),
          "the state's ID-lookup order is never reordered");
    char name[96];
    u32 name_bytes = guild_folder_display_name(decoded, 2, s.store, {name, sizeof(name)});
    check(utf8_equal({name, name_bytes}, utf8_literal("Echo")), "unnamed folders fall back to member names");
    check(guild_folder_row_id(guild_folder_row_key(decoded, 0)) && !guild_folder_row_id(70),
          "folder header ids never collide with snowflakes");

    // Navigation view: headers precede members; shortcuts follow display order.
    GuildViewRow guild_rows[16]{};
    ChannelViewRow channel_rows[8]{};
    u16 sorting[8]{};
    u8 visible[8]{};
    ChannelSortKey keys[8]{};
    NavigationView view{};
    check(navigation_view_initialize(&view, {guild_rows, 16}, {channel_rows, 8}, {sorting, 8}, {visible, 8},
                                             {keys, 8}),
          "folder navigation view");
    view.guild_folders = &decoded;
    check(navigation_view_rebuild(&view, s.store).ok() && view.guild_count == 8 &&
              guild_rows[2].folder_role == GuildFolderRole::header && guild_rows[2].folder_size == 2 &&
              guild_rows[3].id == 30 && guild_rows[3].folder_role == GuildFolderRole::member &&
              !guild_rows[3].folder_expanded && guild_rows[6].folder_role == GuildFolderRole::header &&
              guild_rows[2].folder_color == 0xFF0000 && guild_rows[6].folder_color == kGuildFolderNoColor,
          "navigation rows carry folder headers and members in display order");
    view.selected_guild_id = 10;
    check(navigation_target(view, {NavigationMode::guild, false}).id == 30 &&
              navigation_target(view, {NavigationMode::guild, true}).id == 70,
          "guild shortcuts skip headers and reach guilds in collapsed folders");
    view.selected_guild_id = 40;
    check(navigation_target(view, {NavigationMode::guild, false}).id == 20, "shortcuts leave a folder in order");
    check(navigation_focus_target(view, true, 10, UiListMove::next).id == guild_rows[2].id &&
              navigation_focus_target(view, true, guild_rows[2].id, UiListMove::next).id == 20,
          "rail focus visits the folder header and skips collapsed members");

    // Rail: collapsed members take no space; expanding animates their extent.
    // The trailing "Add a server" button adds one row stride after the rows.
    GuildRailScene rail{};
    GuildRailRowWidget rail_rows[16]{};
    UiWidget rail_widgets[24]{};
    UiLayoutNode rail_nodes[24]{};
    check(guild_rail_scene_initialize(&rail, {rail_rows, 16}, {rail_widgets, 24}, {rail_nodes, 24}) &&
              guild_rail_scene_update(&rail, &view, {0, 0, 72, 900}, 0).ok() && rail.count == 5 &&
              rail.content_height == 12 + 6 * 58.f && rail.add_button.generation &&
              guild_rail_scene_add_rectangle(rail).top == 12 + 5 * 58.f,
          "collapsed folders show one tile");
    const auto header = guild_rail_scene_hit_test(rail, 36, 12 + 2 * 58 + 20);
    check(header.folder && !header.selectable && header.id == guild_rows[2].id, "folder tiles hit as folders");
    const u64 expanded[] = {guild_rows[2].id};
    view.expanded_folders = expanded;
    view.expanded_folder_count = 1;
    ++view.folder_revision;
    check(guild_rail_scene_folder_toggled(&rail, guild_rows[2].id, true, 1000, true) &&
              navigation_view_rebuild(&view, s.store).ok() && guild_rows[3].folder_expanded &&
              guild_rail_scene_update(&rail, &view, rail.viewport, 0).ok() && rail.count == 5,
          "an expanding folder starts with zero-extent members");
    check(guild_rail_scene_tick(&rail, 1075, true) && guild_rail_scene_update(&rail, &view, rail.viewport, 0).ok() &&
              rail.count == 7 && rail.content_height > 12 + 6 * 58.f && rail.content_height < 12 + 8 * 58.f &&
              guild_rail_scene_next_wake(rail, 1075),
          "members grow during the 150 ms transition");
    check(guild_rail_scene_tick(&rail, 1200, true) && guild_rail_scene_update(&rail, &view, rail.viewport, 0).ok() &&
              rail.content_height == 12 + 8 * 58.f && !guild_rail_scene_next_wake(rail, 1200) &&
              guild_rail_scene_hit_test(rail, 36, 12 + 7 * 58.f + 20).add_server,
          "expanded folders settle at full height");
    view.selected_guild_id = 40;
    check(guild_rail_scene_reveal_guild(&rail, view, 40) && rail.focused_guild_id == 40,
          "expanded members are revealed directly");
}

// ---- invites -------------------------------------------------------------
bool invite(Utf8View url, Utf8View code) noexcept {
    InviteLink link{};
    return message_markdown_invite_link(url, &link) && utf8_equal({link.code, link.code_bytes}, code);
}
void invites() noexcept {
    check(invite(utf8_literal("https://discord.gg/abc-DEF9"), utf8_literal("abc-DEF9")) &&
              invite(utf8_literal("https://discord.com/invite/xyz"), utf8_literal("xyz")) &&
              invite(utf8_literal("http://discordapp.com/invite/xyz/"), utf8_literal("xyz")) &&
              invite(utf8_literal("https://www.discord.gg/q1?event=5"), utf8_literal("q1")) &&
              invite(utf8_literal("https://ptb.discord.com/invite/aa#x"), utf8_literal("aa")) &&
              invite(utf8_literal("HTTPS://Discord.GG/Case"), utf8_literal("Case")),
          "invite hosts, paths, query and case variants are recognised");
    InviteLink link{};
    check(!message_markdown_invite_link(utf8_literal("https://discord.com/channels/1/2/3"), &link) &&
              !message_markdown_invite_link(utf8_literal("https://discord.gg/"), &link) &&
              !message_markdown_invite_link(utf8_literal("https://discord.gg/a/b"), &link) &&
              !message_markdown_invite_link(utf8_literal("https://evil.com/invite/abc"), &link) &&
              !message_markdown_invite_link(utf8_literal("https://discord.gg.evil.com/abc"), &link) &&
              !message_markdown_invite_link(utf8_literal("discord.gg/abc"), &link) &&
              !message_markdown_invite_link(utf8_literal("https://discord.gg/a_b"), &link),
          "non-invite links and look-alike hosts are rejected");
    InviteLink found[kInviteLinksPerMessage + 2]{};
    const u32 count = message_markdown_find_invites(
        utf8_literal("join https://discord.gg/one, or (https://discord.com/invite/two) and https://discord.gg/one "
                     "but not https://discord.gg/bad\\x or xhttps://discord.gg/no"),
        {found, kInviteLinksPerMessage + 2});
    check(count == 2 && utf8_equal({found[0].code, found[0].code_bytes}, utf8_literal("one")) &&
              utf8_equal({found[1].code, found[1].code_bytes}, utf8_literal("two")) && found[0].source_offset == 5,
          "content scanning dedupes codes and skips backslashes and glued prefixes");
    char many[1024];
    u32 used = 0;
    for (u32 i = 0; i < 12; ++i) {
        const char text[] = "https://discord.gg/c";
        copy_bytes(many + used, text, sizeof(text) - 1);
        used += sizeof(text) - 1;
        many[used++] = static_cast<char>('a' + i);
        many[used++] = ' ';
    }
    check(message_markdown_find_invites({many, used}, {found, kInviteLinksPerMessage + 2}) ==
              kInviteLinksPerMessage,
          "at most ten invite codes are taken per message");

    // Capture the submissions the module would send.
    GuildInviteRequest request{};
    copy_bytes(request.code, "abc-1", 5);
    request.code_bytes = 5;
    u8 target[kGuildInviteMaximumTargetBytes], body[kGuildInviteMaximumBodyBytes];
    auto written = guild_invite_write_request(request, {target, sizeof(target)}, {body, sizeof(body)});
    check(written.ok() && written.method == RestMethod::get && !written.body_bytes &&
              utf8_equal({reinterpret_cast<char*>(target), written.target_bytes},
                         utf8_literal("/api/v10/invites/abc-1?with_counts=true&with_expiration=true")),
          "resolve requests counts and expiration");
    request.operation = GuildInviteOperation::accept;
    written = guild_invite_write_request(request, {target, sizeof(target)}, {body, sizeof(body)});
    check(written.ok() && written.method == RestMethod::post && written.body_bytes == 2 &&
              body[0] == '{' && body[1] == '}' && written.route_id == kGuildInviteAcceptRouteId &&
              utf8_equal({reinterpret_cast<char*>(target), written.target_bytes}, utf8_literal("/api/v10/invites/abc-1")),
          "accept posts an empty object to the invite route");
    request.code[2] = '/';
    check(!guild_invite_request_valid(request) &&
              !guild_invite_write_request(request, {target, sizeof(target)}, {body, sizeof(body)}).ok(),
          "path characters in codes are rejected before submission");

    static u32 scratch[4096];
    GuildInvite decoded{};
    const auto ok = guild_invite_decode_response(
        200,
        utf8_literal(R"({"type":0,"code":"abc","guild":{"id":"55","name":"Café club","icon":"a_1f"},)"
                     R"("channel":{"id":"66","name":"general","type":0},"approximate_member_count":1234,)"
                     R"("approximate_presence_count":56,"expires_at":null})"),
        {scratch, 4096}, &decoded);
    check(ok == GuildInviteOutcome::resolved && decoded.guild_id == 55 && decoded.channel_id == 66 &&
              decoded.member_count == 1234 && decoded.presence_count == 56 && decoded.counts_known &&
              utf8_equal({decoded.name, decoded.name_bytes}, utf8_literal("Caf\xC3\xA9 club")) &&
              utf8_equal({decoded.icon, decoded.icon_bytes}, utf8_literal("a_1f")),
          "resolved invites decode guild, channel and counts");
    check(guild_invite_decode_response(404, utf8_literal(R"({"message":"Unknown Invite","code":10006})"),
                                       {scratch, 4096}, &decoded) == GuildInviteOutcome::unknown &&
              guild_invite_decode_response(400, utf8_literal(R"({"captcha_key":["captcha-required"],"captcha_sitekey":"s"})"),
                                           {scratch, 4096}, &decoded) == GuildInviteOutcome::captcha &&
              guild_invite_decode_response(403, utf8_literal(R"({"message":"banned","code":40007})"), {scratch, 4096},
                                           &decoded) == GuildInviteOutcome::banned &&
              guild_invite_decode_response(429, {}, {scratch, 4096}, &decoded) == GuildInviteOutcome::rate_limited &&
              guild_invite_decode_response(200, utf8_literal(R"({"code":"x"})"), {scratch, 4096}, &decoded) ==
                  GuildInviteOutcome::malformed &&
              decoded.guild_id == 55,
          "invite failures classify without replacing the last decoded invite");
}

// ---- READY folder publication ---------------------------------------------
struct Published {
    GatewayClientEvent event;
    u8 payload[1024];
    u32 bytes, count;
};
bool capture(void* context, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& p = *static_cast<Published*>(context);
    p.event = event;
    p.bytes = payload.count;
    copy_bytes(p.payload, payload.data, payload.count);
    ++p.count;
    return true;
}
void folder_publication() noexcept {
    const auto group = folder_fixture();
    Writer settings{};
    settings.bytes(kGuildFoldersSettingsField, group);
    char json[2048];
    json[0] = '"';
    const auto encoded = base64_encode({settings.data, settings.used}, {json + 1, sizeof(json) - 2});
    json[encoded.bytes + 1] = '"';
    JsonValue value{};
    value.kind = JsonValueKind::string;
    value.source = {json, encoded.bytes + 2};
    static char storage[kGatewayClientDispatchPayloadCapacity];
    static Published published;
    published = {};
    gateway_client_publish_guild_folders(value, false, {storage, sizeof(storage)}, 9, true, 5, capture, &published);
    check(published.count == 1 && published.event.dispatch_kind == GatewayClientDispatchKind::guild_folders &&
              published.bytes == group.used && bytes_equal(published.payload, group.data, group.used),
          "READY publishes the verbatim folder group");
    Writer empty{};
    empty.tag(1, 0);
    empty.varint(1);
    const auto none = base64_encode({empty.data, empty.used}, {json + 1, sizeof(json) - 2});
    json[none.bytes + 1] = '"';
    value.source = {json, none.bytes + 2};
    published = {};
    gateway_client_publish_guild_folders(value, true, {storage, sizeof(storage)}, 10, true, 5, capture, &published);
    check(!published.count, "partial updates without folders publish nothing");
    gateway_client_publish_guild_folders(value, false, {storage, sizeof(storage)}, 11, true, 5, capture, &published);
    check(published.count == 1 && !published.bytes, "full updates without folders clear the layout");
}

// ---- taskbar attention ---------------------------------------------------
void taskbar() noexcept {
    check(taskbar_attention_badge(0, false, true) == kTaskbarBadgeNone &&
              taskbar_attention_badge(0, true, true) == kTaskbarBadgeDot &&
              taskbar_attention_badge(3, true, true) == 3 &&
              taskbar_attention_badge(12, false, true) == kTaskbarBadgeOverflow &&
              taskbar_attention_badge(5, true, false) == kTaskbarBadgeNone,
          "badge codes cover counts, overflow, dot and the disabled setting");
    TaskbarAttentionState state{};
    TaskbarAttentionInput input{2, 0, true, true, true, false};
    auto plan = taskbar_attention_plan(&state, input);
    check(plan.set_badge && plan.badge == 2 && !plan.start_flash, "first plan publishes the badge without flashing");
    plan = taskbar_attention_plan(&state, input);
    check(!plan.set_badge && !plan.start_flash && !plan.stop_flash, "unchanged input plans nothing");
    input.notify_serial = 1;
    plan = taskbar_attention_plan(&state, input);
    check(plan.start_flash && state.flashing, "a notification while unfocused flashes once");
    plan = taskbar_attention_plan(&state, input);
    check(!plan.start_flash, "the same notification never flashes twice");
    input.focused = true;
    input.mentions = 0;
    input.unread = false;
    plan = taskbar_attention_plan(&state, input);
    check(plan.stop_flash && !state.flashing && plan.set_badge && plan.badge == kTaskbarBadgeNone,
          "focus stops flashing and clears the badge once read");
    input.notify_serial = 2;
    plan = taskbar_attention_plan(&state, input);
    input.focused = false;
    plan = taskbar_attention_plan(&state, input);
    check(!plan.start_flash, "notifications acknowledged while focused do not flash later");
    input.notify_serial = 3;
    input.flash_enabled = false;
    plan = taskbar_attention_plan(&state, input);
    check(!plan.start_flash, "disabled desktop notifications never flash");

    static u32 pixels[32 * 32];
    check(taskbar_attention_render_badge(7, 16, {pixels, 16 * 16}) && (pixels[0] >> 24) == 0 &&
              (pixels[8 * 16 + 1] & 0xFFFFFF) == 0xF23F43 && pixels[8 * 16 + 8] != 0,
          "count badges draw a red disc with transparent corners");
    bool white = false;
    for (u32 i = 0; i < 16 * 16; ++i)
        white = white || pixels[i] == 0xFFFFFFFFu;
    check(white, "count badges draw white digits");
    check(taskbar_attention_render_badge(kTaskbarBadgeOverflow, 32, {pixels, 32 * 32}) &&
              taskbar_attention_render_badge(kTaskbarBadgeDot, 32, {pixels, 32 * 32}) && (pixels[16 * 32 + 2] >> 24) == 0 &&
              (pixels[16 * 32 + 16] >> 24) == 255 && !taskbar_attention_render_badge(kTaskbarBadgeNone, 16, {pixels, 256}) &&
              !taskbar_attention_render_badge(3, 16, {pixels, 100}),
          "overflow and dot badges render; invalid requests are rejected");
}

void run() noexcept {
    unread_banner();
    guild_folders();
    invites();
    folder_publication();
    taskbar();
}
} // namespace maicord::navigation_gaps_test

extern "C" __declspec(noreturn) void WINAPI maicord_navigation_gaps_entry() noexcept {
    maicord::navigation_gaps_test::run();
    ExitProcess(maicord::navigation_gaps_test::failures ? 1 : 0);
}

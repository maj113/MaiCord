// Server lifecycle requests, guild folder edits, channel reordering, channel
// list order, drag state machines, text-in-voice capability and the server
// menu policy.
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/channel_reorder.hpp"
#include "maicord/model/guild_folder_edit.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/protocol/user_settings_proto.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/navigation_drag.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "test_output.hpp"

namespace maicord::navigation_wave_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool text_is(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}

// ---- fixtures ------------------------------------------------------------
struct State {
    GuildRecord guilds[8];
    u16 guild_order[8];
    ChannelRecord channels[16];
    u16 channel_order[16];
    u8 names[4096];
    ChatStateStore store;
};
bool add_guild(State& s, u64 id, const char* name, u64 owner = 1) noexcept {
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    return chat_state_store_upsert_guild(&s.store, {id, {name, bytes}, GuildMutationKind::create, owner}).ok();
}
bool add_channel(State& s, u64 id, u64 guild, u64 parent, u8 type, u32 position, const char* name) noexcept {
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    ChannelInput input{id, guild, parent, {name, bytes}, position, 0, type};
    input.name_known = true;
    return chat_state_store_upsert_channel(&s.store, input).ok();
}
bool initialize(State& s) noexcept {
    return chat_state_store_initialize(&s.store, {s.guilds, 8}, {s.guild_order, 8}, {s.channels, 16},
                                       {s.channel_order, 16}, {s.names, sizeof(s.names)});
}
// Grants resolved permissions to a stored channel (fixtures own the arrays).
void grant(State& s, u64 channel_id, PermissionSet permissions) noexcept {
    auto* channel = const_cast<ChannelRecord*>(chat_state_store_find_channel(s.store, channel_id));
    if (!channel)
        return;
    channel->permissions_resolved = true;
    channel->effective_permissions = permissions;
    channel->actions = channel_actions(channel->type, permissions, true);
}

// ---- server lifecycle requests --------------------------------------------
void lifecycle_requests() noexcept {
    GuildAdminRequest leave{};
    leave.operation = GuildAdminOperation::guild_leave;
    leave.guild_id = 42;
    u8 target[256], body[256];
    const auto t = guild_admin_write_target(leave, {target, sizeof(target)});
    const auto b = guild_admin_write_body(leave, {body, sizeof(body)});
    check(guild_admin_request_valid(leave) && t.ok() && b.ok() &&
              text_is(target, t.bytes, utf8_literal("/api/v10/users/@me/guilds/42")) &&
              text_is(body, b.bytes, utf8_literal(R"({"lurking":false})")) &&
              guild_admin_method(leave.operation) == RestMethod::delete_method &&
              guild_admin_has_body(leave.operation),
          "leave: DELETE /users/@me/guilds/{id} with the official lurking body");
    auto stray = leave;
    stray.target_id = 7;
    auto unscoped = leave;
    unscoped.guild_id = 0;
    check(!guild_admin_request_valid(stray) && !guild_admin_request_valid(unscoped),
          "leave: needs exactly a guild");

    // Every invite option the official client offers is a valid InviteCreate.
    constexpr u32 expiry[]{1'800, 3'600, 21'600, 43'200, 86'400, 604'800, 0};
    constexpr u32 uses[]{0, 1, 5, 10, 25, 50, 100};
    bool options = true;
    for (u32 e : expiry)
        for (u32 u : uses) {
            const InviteCreate invite{e, u, false, false};
            GuildAdminRequest request{};
            request.operation = GuildAdminOperation::invite_create;
            request.channel_id = 9;
            request.invite = &invite;
            options = options && guild_admin_request_valid(request);
        }
    check(options, "invite: every expiry and use limit in the modal is accepted by the route");
    GuildAdminInvite decoded{};
    u32 scratch[512];
    check(guild_admin_decode_invite(
              utf8_literal(R"({"type":0,"code":"abc123","guild":{"id":"1"},"channel":{"id":"9","name":"general"},)"
                           R"("inviter":{"id":"3"},"uses":0,"max_uses":5,"max_age":3600,"temporary":false,)"
                           R"("created_at":"2026-10-07T10:00:00+00:00","expires_at":"2026-10-07T11:00:00+00:00"})"),
              {scratch, 512}, &decoded)
                  .ok() &&
              utf8_equal({decoded.code, decoded.code_bytes}, utf8_literal("abc123")) && decoded.channel_id == 9 &&
              decoded.max_uses == 5 && decoded.max_age_seconds == 3600 && decoded.expires_at_ms,
          "invite: the create response decodes code, limits and expiry");
    check(!guild_admin_decode_invite(utf8_literal(R"({"code":"bad/code"})"), {scratch, 512}, &decoded).ok() &&
              !guild_admin_decode_invite(utf8_literal(R"({"uses":1})"), {scratch, 512}, &decoded).ok(),
          "invite: a response without a valid code is refused");

    // Create a server: guild_assets prepares the icon like a server icon.
    GuildAssetRequest create{};
    create.operation = GuildAssetOperation::guild_create;
    create.name = utf8_literal("Lab");
    u8 asset_target[128];
    const auto at = guild_asset_write_target(create, {asset_target, sizeof(asset_target)});
    const u8 png[]{1, 2, 3};
    char json[512];
    const u32 plain = guild_asset_body_bytes(create, {}, {});
    const auto written = guild_asset_write_body(create, {}, {}, {reinterpret_cast<u8*>(json), sizeof(json)});
    check(guild_asset_request_valid(create) && at.ok() && text_is(asset_target, at.bytes, utf8_literal("/api/v10/guilds")) &&
              written.ok() && written.bytes == plain &&
              utf8_equal({json, written.bytes},
                         utf8_literal(R"({"name":"Lab","icon":null,"channels":[],"system_channel_id":null,"guild_template_code":"2TffvPucqHkN"})")) &&
              guild_asset_profile(create) == GuildImageProfile::icon && guild_asset_has_image(create.operation) &&
              guild_asset_route(create).major == RestMajorParameter::none,
          "create: POST /guilds with the blank template and a null icon");
    const auto with_icon = guild_asset_write_body(create, {{png, 3}, GuildImageKind::png}, {},
                                                  {reinterpret_cast<u8*>(json), sizeof(json)});
    check(with_icon.ok() &&
              utf8_equal({json, with_icon.bytes},
                         utf8_literal(R"({"name":"Lab","icon":"data:image/png;base64,AQID","channels":[],"system_channel_id":null,"guild_template_code":"2TffvPucqHkN"})")),
          "create: a prepared icon is sent as a data URI");
    auto named_guild = create;
    named_guild.guild_id = 5;
    auto short_name = create;
    short_name.name = utf8_literal("L");
    check(!guild_asset_request_valid(named_guild) && !guild_asset_request_valid(short_name),
          "create: no guild ID and a name of at least two characters");
}

// ---- guild folders ----------------------------------------------------------
GuildFolderSettings g_settings, g_decoded, g_canonical;
u32 entries_of(const GuildFolderSettings& s, u64* output, u32 capacity) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < s.guild_id_count && count < capacity; ++i)
        output[count++] = s.guild_ids[i];
    return count;
}
bool folder_is(const GuildFolderSettings& s, u32 index, u64 id, ConstSlice<u64> guilds) noexcept {
    if (index >= s.folder_count)
        return false;
    const auto& f = s.folders[index];
    if (f.has_id != (id != 0) || (id && f.id != id) || f.guild_count != guilds.count)
        return false;
    for (u32 g = 0; g < guilds.count; ++g)
        if (s.guild_ids[f.first_guild + g] != guilds[g])
            return false;
    return true;
}
void folders() noexcept {
    State s{};
    check(initialize(s) && add_guild(s, 10, "Alpha") && add_guild(s, 20, "Bravo") && add_guild(s, 30, "Charlie") &&
              add_guild(s, 40, "Delta") && add_guild(s, 50, "Echo") && add_guild(s, 60, "Foxtrot"),
          "folders: state fixture");
    // Stored layout: Games{30,40}, 20, folder 9{50,60}; 10 is unlisted.
    auto& in = g_settings;
    in = {};
    in.present = true;
    const u64 ids[]{30, 40, 20, 50, 60};
    copy_bytes(in.guild_ids, ids, sizeof(ids));
    in.guild_id_count = 5;
    in.folders[0] = {7, 0xFF0000, 0, 2, 5, true, true, {'G', 'a', 'm', 'e', 's'}};
    in.folders[1] = {0, 0, 2, 1, 0, false, false, {}};
    in.folders[2] = {9, 0, 3, 2, 0, true, false, {}};
    in.folder_count = 3;
    in.positions[0] = 20;
    in.position_count = 1;
    const u64 alpha[]{10};
    check(guild_folders_canonical(&in, s.store, &g_canonical) == GuildFolderEditError::none &&
              g_canonical.folder_count == 4 && folder_is(g_canonical, 0, 0, {alpha, 1}) &&
              folder_is(g_canonical, 1, 7, {ids, 2}) &&
              folder_is(g_canonical, 2, 0, {ids + 2, 1}) && folder_is(g_canonical, 3, 9, {ids + 3, 2}) &&
              g_canonical.position_count == 1,
          "folders: the canonical layout lists every shown guild, unlisted ones first");

    auto& work = g_settings;
    copy_bytes(&work, &g_canonical, sizeof(work));
    // Move Bravo (20) above Alpha (10).
    const u64 a1[]{20, 10, 30, 40, 50, 60};
    u64 got[8];
    check(guild_folders_move(&work, {20, false}, {10, false}, GuildFolderDrop::before, 0) == GuildFolderEditError::none &&
              entries_of(work, got, 8) == 6 && bytes_equal(got, a1, sizeof(a1)) && work.folder_count == 4,
          "folders: a guild moves above another top-level guild");
    check(guild_folders_move(&work, {20, false}, {10, false}, GuildFolderDrop::before, 0) ==
              GuildFolderEditError::unchanged,
          "folders: dropping in place changes nothing");
    // Charlie (30, in Games) next to Echo (50, in folder 9): joins folder 9.
    const u64 nested[]{50, 30};
    check(guild_folders_move(&work, {30, false}, {50, false}, GuildFolderDrop::after, 0) == GuildFolderEditError::none &&
              folder_is(work, 2, 7, {ids + 1, 1}) && work.folders[3].guild_count == 3 &&
              work.guild_ids[work.folders[3].first_guild] == 50 && work.guild_ids[work.folders[3].first_guild + 1] == 30,
          "folders: a guild dropped beside a folder member joins that folder");
    (void)nested;
    // Delta (40), the last of Games, moves top level: Games disappears.
    check(guild_folders_move(&work, {40, false}, {20, false}, GuildFolderDrop::before, 0) == GuildFolderEditError::none &&
              guild_folders_find_folder(work, 7) == ~0u && work.guild_ids[0] == 40,
          "folders: an emptied folder vanishes");
    // Alpha (10) onto Bravo (20): a new folder 1234 holding {20, 10}.
    const u64 created[]{20, 10};
    u32 bravo = ~0u;
    for (u32 f = 0; f < work.folder_count && bravo == ~0u; ++f)
        if (work.folders[f].guild_count && work.guild_ids[work.folders[f].first_guild] == 20) bravo = f;
    check(guild_folders_move(&work, {10, false}, {20, false}, GuildFolderDrop::combine, 1234) ==
                  GuildFolderEditError::none &&
              guild_folders_find_folder(work, 1234) == bravo && folder_is(work, bravo, 1234, {created, 2}),
          "folders: dropping a guild onto a guild makes a folder there");
    // Combining onto a folder member, folders into folders and folder combines are refused.
    check(guild_folders_move(&work, {40, false}, {30, false}, GuildFolderDrop::combine, 99) ==
                  GuildFolderEditError::input &&
              guild_folders_move(&work, {1234, true}, {9, true}, GuildFolderDrop::combine, 0) ==
                  GuildFolderEditError::input &&
              guild_folders_move(&work, {1234, true}, {50, false}, GuildFolderDrop::before, 0) ==
                  GuildFolderEditError::input &&
              guild_folders_move(&work, {10, false}, {20, false}, GuildFolderDrop::combine, 9) ==
                  GuildFolderEditError::input,
          "folders: moves the official client refuses are refused");
    // A guild onto a folder joins it at the end; a folder moves top level.
    check(guild_folders_move(&work, {40, false}, {9, true}, GuildFolderDrop::combine, 0) == GuildFolderEditError::none &&
              work.guild_ids[work.folders[guild_folders_find_folder(work, 9)].first_guild +
                             work.folders[guild_folders_find_folder(work, 9)].guild_count - 1] == 40 &&
              guild_folders_move(&work, {9, true}, {1234, true}, GuildFolderDrop::before, 0) ==
                  GuildFolderEditError::none &&
              guild_folders_find_folder(work, 9) == 0 && guild_folders_find_folder(work, 1234) == 1,
          "folders: guilds join folders at the end and folders reorder");
    check(guild_folders_edit(&work, 9, utf8_literal("Friends"), 0x1ABC9C, true) == GuildFolderEditError::none &&
              utf8_equal({work.folders[0].name, work.folders[0].name_bytes}, utf8_literal("Friends")) &&
              work.folders[0].has_color && work.folders[0].color == 0x1ABC9C &&
              guild_folders_edit(&work, 9, utf8_literal("Friends"), kGuildFolderDefaultColor, true) ==
                  GuildFolderEditError::none &&
              !work.folders[0].has_color &&
              guild_folders_edit(&work, 9, utf8_literal("Friends"), 0, false) == GuildFolderEditError::unchanged &&
              guild_folders_edit(&work, 77, {}, 0, false) == GuildFolderEditError::input,
          "folders: names and colours edit; the brand colour means none");
    const u64 fresh = guild_folders_new_id(work, 12345);
    check(fresh && fresh <= 0x7FFFFFFF && guild_folders_find_folder(work, fresh) == ~0u &&
              guild_folders_new_id(work, 12345) == fresh,
          "folders: new folder ids are unused, 31-bit and deterministic per seed");

    // Write-back: the encoded group decodes to the same layout and is a valid
    // folders-only settings PATCH.
    static u8 group[kGuildFoldersMaximumGroupBytes], patch[kGuildFoldersMaximumGroupBytes + 8];
    const u32 group_bytes = guild_folders_encode(work, {group, sizeof(group)});
    const u32 patch_bytes = guild_folders_encode_settings_patch(work, {patch, sizeof(patch)});
    bool same = group_bytes && guild_folders_decode({group, group_bytes}, &g_decoded) == GuildFoldersError::none &&
                g_decoded.folder_count == work.folder_count && g_decoded.guild_id_count == work.guild_id_count &&
                g_decoded.position_count == work.position_count;
    for (u32 f = 0; same && f < work.folder_count; ++f) {
        const auto &x = work.folders[f], &y = g_decoded.folders[f];
        same = x.has_id == y.has_id && x.id == y.id && x.guild_count == y.guild_count &&
               x.has_color == y.has_color && x.color == y.color && x.name_bytes == y.name_bytes &&
               bytes_equal(x.name, y.name, x.name_bytes);
    }
    same = same && bytes_equal(work.guild_ids, g_decoded.guild_ids, work.guild_id_count * 8);
    check(same, "folders: encoding round-trips ids, names, colours and positions");
    check(patch_bytes == group_bytes + 1 + (group_bytes > 127 ? 2u : 1u) && patch[0] == 0x72 &&
              bytes_equal(patch + patch_bytes - group_bytes, group, group_bytes) &&
              user_settings_proto_patch_valid({patch, patch_bytes}),
          "folders: the settings PATCH carries field 14 alone");
    u8 mixed[64];
    const u8 appearance[]{0x6A, 0x02, 0x08, 0x01};
    copy_bytes(mixed, appearance, 4);
    mixed[4] = 0x72;
    mixed[5] = 0x00;
    check(!user_settings_proto_patch_valid({mixed, 6}) && user_settings_proto_patch_valid({mixed + 4, 2}),
          "folders: folders never share a PATCH with other groups");
    // A small layout encodes exactly as the official client's protobuf writer does.
    static GuildFolderSettings tiny;
    tiny = {};
    tiny.present = true;
    tiny.guild_ids[0] = 1;
    tiny.guild_ids[1] = 2;
    tiny.guild_id_count = 2;
    tiny.folders[0] = {5, 0, 0, 1, 1, true, false, {'A'}};
    tiny.folders[1] = {0, 0, 1, 1, 0, false, false, {}};
    tiny.folder_count = 2;
    u8 small[64];
    const u8 expected[]{0x0A, 0x13, 0x0A, 0x08, 1, 0, 0, 0, 0, 0, 0, 0, 0x12, 0x02, 0x08, 0x05, 0x1A, 0x03, 0x0A, 0x01,
                        'A',  0x0A, 0x0A, 0x0A, 0x08, 2, 0, 0, 0, 0, 0, 0, 0};
    const u32 small_bytes = guild_folders_encode(tiny, {small, sizeof(small)});
    check(small_bytes == sizeof(expected) && bytes_equal(small, expected, sizeof(expected)) &&
              !guild_folders_encode(tiny, {small, 10}),
          "folders: the wire bytes match the reference GuildFolders encoding; short output fails");
}

// ---- channel reordering ------------------------------------------------------
bool position_is(const GuildAdminPosition& p, u64 id, u32 position, bool parent = false, u64 parent_id = 0) noexcept {
    return p.id == id && p.position == position &&
           ((p.flags & guild_admin_position_parent) != 0) == parent && (!parent || p.parent_id == parent_id);
}
void reorder() noexcept {
    State s{};
    // Categories C1 (100, position 0) and C2 (200, position 1); text a (11,
    // uncategorized), b/c (12/13 in C1), d (14 in C2) at guild-wide positions
    // 0-3; voice v1/v2 (15/16 in C1).
    check(initialize(s) && add_guild(s, 1, "Lab") && add_channel(s, 100, 1, 0, 4, 0, "one") &&
              add_channel(s, 200, 1, 0, 4, 1, "two") && add_channel(s, 11, 1, 0, 0, 0, "a") &&
              add_channel(s, 12, 1, 100, 0, 1, "b") && add_channel(s, 13, 1, 100, 0, 2, "c") &&
              add_channel(s, 14, 1, 200, 0, 3, "d") && add_channel(s, 15, 1, 100, 2, 0, "v1") &&
              add_channel(s, 16, 1, 100, 2, 1, "v2"),
          "reorder: channel fixture");
    GuildAdminPosition out[16]{};
    // c above b: the text group renumbers a,c,b,d; only changed entries go.
    u32 n = channel_reorder_plan(s.store, 13, 12, ChannelReorderDrop::before, {out, 16});
    check(n == 2 && position_is(out[0], 13, 1) && position_is(out[1], 12, 2),
          "reorder: moving within a category sends the official client's position deltas");
    // d onto C1: first in C1 with its parent.
    n = channel_reorder_plan(s.store, 14, 100, ChannelReorderDrop::into, {out, 16});
    check(n == 3 && position_is(out[0], 14, 1, true, 100) && position_is(out[1], 12, 2) &&
              position_is(out[2], 13, 3),
          "reorder: dropping onto a category moves the channel to its top");
    // b below a: leaves C1 for no category (parent null).
    n = channel_reorder_plan(s.store, 12, 11, ChannelReorderDrop::after, {out, 16});
    check(n == 1 && position_is(out[0], 12, 1, true, 0),
          "reorder: a channel dropped among uncategorized ones loses its category");
    // v2 above v1 in the voice group.
    n = channel_reorder_plan(s.store, 16, 15, ChannelReorderDrop::before, {out, 16});
    check(n == 2 && position_is(out[0], 16, 0) && position_is(out[1], 15, 1),
          "reorder: voice channels order among voice channels");
    // Categories: C2 above C1.
    n = channel_reorder_plan(s.store, 200, 100, ChannelReorderDrop::before, {out, 16});
    check(n == 2 && position_is(out[0], 200, 0) && position_is(out[1], 100, 1),
          "reorder: categories reorder among categories");
    check(channel_reorder_plan(s.store, 15, 12, ChannelReorderDrop::before, {out, 16}) == ~0u &&
              channel_reorder_plan(s.store, 100, 200, ChannelReorderDrop::into, {out, 16}) == ~0u &&
              channel_reorder_plan(s.store, 12, 100, ChannelReorderDrop::before, {out, 16}) == ~0u &&
              channel_reorder_plan(s.store, 12, 12, ChannelReorderDrop::after, {out, 16}) == ~0u &&
              channel_reorder_plan(s.store, 13, 12, ChannelReorderDrop::before, {out, 1}) == ~0u &&
              !channel_reorder_valid(s.store, 15, 12, ChannelReorderDrop::after) &&
              channel_reorder_valid(s.store, 15, 100, ChannelReorderDrop::into),
          "reorder: groups never mix, categories never nest and short output fails");
    check(channel_reorder_plan(s.store, 12, 13, ChannelReorderDrop::before, {out, 16}) == 0,
          "reorder: a drop in place sends nothing");
}

// ---- drag state machines -------------------------------------------------------
void drag() noexcept {
    NavigationDrag d{};
    navigation_drag_press(&d, NavigationDragList::guilds, 0, NavigationDragRowKind::guild, {10, 10});
    check(d.phase == NavigationDragPhase::idle, "drag: a press without a source stays idle");
    navigation_drag_press(&d, NavigationDragList::channels, 5, NavigationDragRowKind::folder, {10, 10});
    check(d.phase == NavigationDragPhase::idle, "drag: folders are not channel-list sources");
    navigation_drag_press(&d, NavigationDragList::guilds, 20, NavigationDragRowKind::guild, {30, 100});
    check(d.phase == NavigationDragPhase::armed && !navigation_drag_move(&d, {31, 102}) &&
              d.phase == NavigationDragPhase::armed,
          "drag: small movement keeps the press armed");
    NavigationDragTarget dropped{};
    check(!navigation_drag_release(&d, &dropped) && d.phase == NavigationDragPhase::idle,
          "drag: releasing an armed press is a click, not a drop");
    navigation_drag_press(&d, NavigationDragList::guilds, 20, NavigationDragRowKind::guild, {30, 100});
    check(navigation_drag_move(&d, {30, 110}) && navigation_drag_dragging(d) && !navigation_drag_move(&d, {30, 120}),
          "drag: crossing the threshold starts one drag");
    // Rail rows: 40 DIP icons with 18 DIP gaps.
    const NavigationDragRow rail[]{{10, {16, 12, 56, 52}, NavigationDragRowKind::guild},
                                   {20, {16, 70, 56, 110}, NavigationDragRowKind::guild},
                                   {7, {16, 128, 56, 168}, NavigationDragRowKind::folder},
                                   {30, {16, 186, 56, 226}, NavigationDragRowKind::nested_guild}};
    auto t = navigation_drag_guild_target(d, {rail, 4}, 20, kGuildRailRowGapDips);
    check(t.id == 10 && t.drop == NavigationDropKind::before && t.indicator.top < 12 && t.indicator.bottom <= 12,
          "drag: the top quarter of a guild drops above it, the line in the gap");
    t = navigation_drag_guild_target(d, {rail, 4}, 32, kGuildRailRowGapDips);
    check(t.id == 10 && t.drop == NavigationDropKind::combine && t.indicator.top == 12,
          "drag: the middle of a guild combines into a folder with a ring");
    t = navigation_drag_guild_target(d, {rail, 4}, 148, kGuildRailRowGapDips);
    check(t.id == 7 && t.drop == NavigationDropKind::combine, "drag: a guild over a folder's middle joins it");
    t = navigation_drag_guild_target(d, {rail, 4}, 206, kGuildRailRowGapDips);
    check(t.id == 30 && t.drop == NavigationDropKind::after, "drag: folder members only take neighbours");
    t = navigation_drag_guild_target(d, {rail, 4}, 90, kGuildRailRowGapDips);
    check(!t.id && t.drop == NavigationDropKind::none, "drag: a guild over itself has no target");
    t = navigation_drag_guild_target(d, {rail, 4}, 58, kGuildRailRowGapDips);
    check(t.id == 10 && t.drop == NavigationDropKind::after, "drag: the gap below a row drops after it");
    auto visual = navigation_drag_update_target(&d, navigation_drag_guild_target(d, {rail, 4}, 32, kGuildRailRowGapDips));
    check(visual.active && visual.ring && visual.source_id == 20 && navigation_drag_release(&d, &dropped) &&
              dropped.id == 10 && dropped.drop == NavigationDropKind::combine && d.phase == NavigationDragPhase::idle,
          "drag: release commits the last target");
    navigation_drag_press(&d, NavigationDragList::guilds, 7, NavigationDragRowKind::folder, {30, 148});
    navigation_drag_move(&d, {30, 20});
    t = navigation_drag_guild_target(d, {rail, 4}, 25, kGuildRailRowGapDips);
    check(t.id == 10 && t.drop == NavigationDropKind::before && !navigation_drag_guild_target(d, {rail, 4}, 206, 18).id,
          "drag: folders never combine or sit among folder members");
    navigation_drag_cancel(&d);
    check(d.phase == NavigationDragPhase::idle && !navigation_drag_release(&d, &dropped), "drag: Esc cancels");

    const NavigationDragRow list[]{{100, {0, 0, 200, 24}, NavigationDragRowKind::category},
                                   {12, {0, 24, 200, 56}, NavigationDragRowKind::text},
                                   {15, {0, 56, 200, 88}, NavigationDragRowKind::voice},
                                   {500, {0, 88, 200, 120}, NavigationDragRowKind::none}};
    navigation_drag_press(&d, NavigationDragList::channels, 13, NavigationDragRowKind::text, {10, 200});
    navigation_drag_move(&d, {10, 30});
    check(navigation_drag_channel_target(d, {list, 4}, 10).drop == NavigationDropKind::into &&
              navigation_drag_channel_target(d, {list, 4}, 30).drop == NavigationDropKind::before &&
              navigation_drag_channel_target(d, {list, 4}, 50).drop == NavigationDropKind::after &&
              !navigation_drag_channel_target(d, {list, 4}, 70).id &&
              !navigation_drag_channel_target(d, {list, 4}, 100).id,
          "drag: channels drop into categories and beside their own group only");
    navigation_drag_press(&d, NavigationDragList::channels, 200, NavigationDragRowKind::category, {10, 200});
    navigation_drag_move(&d, {10, 5});
    check(navigation_drag_channel_target(d, {list, 4}, 5).drop == NavigationDropKind::before &&
              !navigation_drag_channel_target(d, {list, 4}, 30).id,
          "drag: categories drop beside categories only");
    navigation_drag_cancel(&d);

    const UiRect viewport{0, 100, 72, 700};
    check(navigation_drag_autoscroll(viewport, 105, 16) < 0 && navigation_drag_autoscroll(viewport, 695, 16) > 0 &&
              navigation_drag_autoscroll(viewport, 400, 16) == 0 &&
              navigation_drag_autoscroll(viewport, 100, 16) < navigation_drag_autoscroll(viewport, 130, 16) &&
              navigation_drag_autoscroll(viewport, 105, 0) == 0,
          "drag: lists auto-scroll faster closer to their edges");
}

// ---- text in voice ---------------------------------------------------------------
void text_in_voice() noexcept {
    check(channel_is_message_capable(2) && channel_is_message_capable(13) && channel_is_voice_chat(2) &&
              channel_is_voice_chat(13) && !channel_is_voice_chat(0) && !channel_is_message_capable(4) &&
              !channel_is_message_capable(15),
          "voice chat: voice and stage channels carry messages");
    State s{};
    check(initialize(s) && add_guild(s, 1, "Lab") && add_channel(s, 20, 1, 0, 2, 0, "Lounge") &&
              add_channel(s, 21, 1, 0, 0, 5, "general"),
          "voice chat: fixture");
    const auto selected = chat_state_store_select_guild(&s.store, 1);
    check(selected.ok() && s.store.selected_channel_id == 21,
          "voice chat: a guild opens on its text channel, never a voice chat");
    check(chat_state_store_select_channel(&s.store, 20).ok() && s.store.selected_channel_id == 20,
          "voice chat: a voice channel's chat can be opened explicitly");
    GuildViewRow guild_rows[16]{};
    ChannelViewRow channel_rows[16]{};
    u16 sorting[16]{};
    u8 visible[16]{};
    ChannelSortKey keys[16]{};
    NavigationView view{};
    check(navigation_view_initialize(&view, {guild_rows, 16}, {channel_rows, 16}, {sorting, 16}, {visible, 16},
                                     {keys, 16}) &&
              navigation_view_rebuild(&view, s.store).ok(),
          "voice chat: navigation fixture");
    const auto* voice_row = navigation_view_find_channel(view, 20);
    const auto* text_row = navigation_view_find_channel(view, 21);
    check(voice_row && text_row && !voice_row->selectable && text_row->selectable,
          "voice chat: voice rows still join the call on click");
    // Reading and sending follow the channel's own permissions.
    grant(s, 20, kPermissionViewChannel | kPermissionConnect);
    const auto* voice = chat_state_store_find_channel(s.store, 20);
    check(voice && !(voice->actions & channel_action_read_history) && !(voice->actions & channel_action_send_messages),
          "voice chat: without Read Message History the chat stays closed");
    grant(s, 20, kPermissionViewChannel | kPermissionReadMessageHistory | kPermissionSendMessages);
    check((voice->actions & channel_action_read_history) && (voice->actions & channel_action_send_messages),
          "voice chat: View, Read History and Send open and write the chat");
}

// ---- server menu ----------------------------------------------------------------------
void server_menu() noexcept {
    State s{};
    check(initialize(s) && add_guild(s, 1, "Lab", 99) && add_channel(s, 10, 1, 0, 0, 0, "general") &&
              add_channel(s, 11, 1, 0, 0, 1, "rules") && add_channel(s, 12, 1, 0, 2, 0, "Lounge"),
          "menu: fixture");
    s.store.self_user_id = 5;
    PopupItem item{};
    check(!guild_context_invite_channel(s.store, 1, 0) && !guild_context_invite_item(s.store, 1, 0, 900, {&item, 1}),
          "menu: no invite row without Create Invite anywhere");
    grant(s, 11, kPermissionViewChannel | kPermissionCreateInvite);
    grant(s, 12, kPermissionViewChannel | kPermissionCreateInvite);
    check(guild_context_invite_channel(s.store, 1, 0) == 11 && guild_context_invite_channel(s.store, 1, 12) == 12 &&
              guild_context_invite_channel(s.store, 1, 10) == 11,
          "menu: invites go to the selected channel when allowed, else the first text channel");
    check(guild_context_invite_item(s.store, 1, 0, 900, {&item, 1}) == 1 && item.action == 900 &&
              (item.flags & popup_item_separator_before) && utf8_equal(item.label, utf8_literal("Invite people")),
          "menu: Invite people is offered with Create Invite");
    check(guild_context_leave_item(s.store, 1, 901, {&item, 1}) == 1 && (item.flags & popup_item_danger) &&
              utf8_equal(item.label, utf8_literal("Leave server")) && !guild_context_owner(s.store, 1),
          "menu: Leave server is a danger row");
    s.store.self_user_id = 99;
    check(guild_context_owner(s.store, 1) && guild_context_leave_item(s.store, 1, 901, {&item, 1}) == 0,
          "menu: owners get no Leave server row, as in the official client");
    check(guild_context_copy_id_item(s.store, 1, 902, {&item, 1}) == 1 && !guild_context_copy_id_item(s.store, 2, 902, {&item, 1}) &&
              !guild_context_settings_item(s.store, 1, 903, {&item, 1}),
          "menu: developer ID for live servers; settings only with a settings permission");
}

// ---- hide muted channels ---------------------------------------------------------------
void hide_muted() noexcept {
    State s{};
    check(initialize(s) && add_guild(s, 1, "Lab") && add_channel(s, 10, 1, 0, 0, 0, "general") &&
              add_channel(s, 11, 1, 0, 0, 1, "noisy") && add_channel(s, 12, 1, 0, 4, 2, "Muted") &&
              add_channel(s, 13, 1, 12, 0, 0, "inside"),
          "hide muted: fixture");
    constexpr u64 ids[]{10, 11, 12, 13};
    for (const u64 id : ids)
        grant(s, id, kPermissionViewChannel | kPermissionReadMessageHistory);
    GuildViewRow guild_rows[4]{};
    ChannelViewRow channel_rows[16]{};
    u16 sorting[16]{};
    u8 visible[16]{};
    ChannelSortKey keys[16]{};
    NavigationView view{};
    const auto shown = [&](u64 id) noexcept {
        return navigation_view_rebuild(&view, s.store).ok() && navigation_view_find_channel(view, id) != nullptr;
    };
    // A server-wide mute marks rows muted but not dimmed; it never hides them.
    check(navigation_view_initialize(&view, {guild_rows, 4}, {channel_rows, 16}, {sorting, 16}, {visible, 16},
                                     {keys, 16}) &&
              chat_state_store_select_channel(&s.store, 10).ok() &&
              chat_state_store_set_channel_mute(&s.store, 10, true, false) &&
              chat_state_store_set_channel_mute(&s.store, 11, true, true) &&
              chat_state_store_set_channel_mute(&s.store, 12, true, true) &&
              chat_state_store_set_channel_mute(&s.store, 13, true, true) && shown(11) && shown(12) && shown(13),
          "hide muted: muted rows stay listed until the server hides them");
    check(chat_state_store_set_guild_hide_muted(&s.store, 1, true) &&
              !chat_state_store_set_guild_hide_muted(&s.store, 1, true) && shown(10) && !shown(11) && !shown(12) &&
              !shown(13),
          "hide muted: own and category mutes leave the list; a server-wide mute does not");
    check(chat_state_store_select_channel(&s.store, 13).ok() && shown(13) && shown(12) && !shown(11),
          "hide muted: the open channel and its category stay");
    check(chat_state_store_set_guild_hide_muted(&s.store, 1, false) && shown(11) && shown(12),
          "hide muted: turning it off restores the rows");
}

// ---- channel opt-in ("Show all channels" off) ---------------------------------------
void channel_opt_in() noexcept {
    State s{};
    check(initialize(s) && add_guild(s, 1, "Lab") && add_channel(s, 10, 1, 0, 0, 0, "general") &&
              add_channel(s, 11, 1, 0, 0, 1, "other") && add_channel(s, 12, 1, 0, 4, 2, "Picked") &&
              add_channel(s, 13, 1, 12, 0, 0, "inside") && add_channel(s, 14, 1, 0, 4, 3, "Empty"),
          "channel opt-in: fixture");
    constexpr u64 ids[]{10, 11, 12, 13, 14};
    for (const u64 id : ids)
        grant(s, id, kPermissionViewChannel | kPermissionReadMessageHistory);
    GuildViewRow guild_rows[4]{};
    ChannelViewRow channel_rows[16]{};
    u16 sorting[16]{};
    u8 visible[16]{};
    ChannelSortKey keys[16]{};
    NavigationView view{};
    const auto shown = [&](u64 id) noexcept {
        return navigation_view_rebuild(&view, s.store).ok() && navigation_view_find_channel(view, id) != nullptr;
    };
    check(navigation_view_initialize(&view, {guild_rows, 4}, {channel_rows, 16}, {sorting, 16}, {visible, 16},
                                     {keys, 16}) &&
              chat_state_store_select_channel(&s.store, 10).ok() &&
              chat_state_store_set_channel_opted_in(&s.store, 13, true) && shown(11) && shown(14),
          "channel opt-in: every channel is listed while opt-in is off");
    check(chat_state_store_set_guild_channel_opt_in(&s.store, 1, true) &&
              !chat_state_store_set_guild_channel_opt_in(&s.store, 1, true) && shown(10) && !shown(11) &&
              shown(13) && shown(12) && !shown(14),
          "channel opt-in: only opted-in channels, their categories and the open channel stay");
    check(chat_state_store_select_channel(&s.store, 13).ok() && !shown(10) &&
              chat_state_store_set_guild_channel_opt_in(&s.store, 1, false) && shown(10) && shown(11) && shown(14),
          "channel opt-in: Show all channels restores the full list");
}

// ---- channel list order ------------------------------------------------------------
struct List {
    State s;
    GuildViewRow guild_rows[4];
    ChannelViewRow channel_rows[16];
    u16 sorting[16];
    u8 visible[16];
    ChannelSortKey keys[16];
    NavigationView view;
};
bool list_ready(List& l, u64 guild = 1) noexcept {
    return navigation_view_initialize(&l.view, {l.guild_rows, 4}, {l.channel_rows, 16}, {l.sorting, 16},
                                      {l.visible, 16}, {l.keys, 16}) &&
           chat_state_store_select_guild(&l.s.store, guild).ok();
}
bool rows_are(const NavigationView& view, const u64* ids, u32 count) noexcept {
    if (view.channel_count != count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (view.channel_rows[i].id != ids[i])
            return false;
    return true;
}
template <u32 N>
bool list_is(List& l, const u64 (&ids)[N]) noexcept {
    return navigation_view_rebuild(&l.view, l.s.store).ok() && rows_are(l.view, ids, N);
}
// The picker comparator and the rail agree on every pair of listed channels.
bool comparator_agrees(const List& l) noexcept {
    for (u32 i = 0; i < l.view.channel_count; ++i)
        for (u32 j = i + 1; j < l.view.channel_count; ++j) {
            const auto* a = chat_state_store_find_channel(l.s.store, l.view.channel_rows[i].id);
            const auto* b = chat_state_store_find_channel(l.s.store, l.view.channel_rows[j].id);
            if (!a || !b || !channel_list_before(l.s.store, *a, *b) || channel_list_before(l.s.store, *b, *a))
                return false;
        }
    return true;
}

void list_order_kinds() noexcept {
    List l{};
    // Text-like first, voice and stage after; equal positions fall back to the ID.
    check(initialize(l.s) && add_guild(l.s, 1, "Lab") && add_channel(l.s, 100, 1, 0, 4, 1, "Cat") &&
              add_channel(l.s, 12, 1, 100, 0, 0, "b") && add_channel(l.s, 11, 1, 100, 2, 0, "v1") &&
              add_channel(l.s, 10, 1, 100, 0, 0, "a") && add_channel(l.s, 13, 1, 100, 2, 0, "v2") &&
              add_channel(l.s, 14, 1, 100, 13, 0, "stage") && add_channel(l.s, 16, 1, 100, 0, 5, "c") &&
              add_channel(l.s, 17, 1, 100, 2, 1, "v3") && list_ready(l),
          "list order: kinds fixture");
    constexpr u64 expected[]{100, 10, 12, 16, 11, 13, 14, 17};
    check(list_is(l, expected) && comparator_agrees(l),
          "list order: equal positions list text by id, then voice and stage by id; later voice stays below text");
    check(channel_list_position(2, 5) == 10'005 && channel_list_position(13, 0) == 10'000 &&
              channel_list_position(0, 5) == 5 && channel_list_position(15, 10'000) == 10'000 &&
              channel_list_position(2, ~0u) == ~0u,
          "list order: voice and stage positions sit 10000 later, saturating");

    // Announcement, forum and media rows sort by position with text; voice-like rows follow.
    List m{};
    check(initialize(m.s) && add_guild(m.s, 1, "Lab") && add_channel(m.s, 200, 1, 0, 4, 0, "Cat") &&
              add_channel(m.s, 20, 1, 200, 0, 3, "text") && add_channel(m.s, 21, 1, 200, 5, 1, "news") &&
              add_channel(m.s, 22, 1, 200, 15, 2, "forum") && add_channel(m.s, 23, 1, 200, 16, 0, "media") &&
              add_channel(m.s, 24, 1, 200, 2, 0, "voice") && add_channel(m.s, 25, 1, 200, 13, 0, "stage") &&
              list_ready(m),
          "list order: text-like types fixture");
    constexpr u64 mixed[]{200, 23, 21, 22, 20, 24, 25};
    check(list_is(m, mixed) && comparator_agrees(m),
          "list order: announcement, forum and media interleave with text by position, ahead of voice");
}

void list_order_voice_and_uncategorized() noexcept {
    // A stage among voice channels, all uncategorized: by position, ties by ID.
    List v{};
    check(initialize(v.s) && add_guild(v.s, 1, "Lab") && add_channel(v.s, 30, 1, 0, 2, 2, "v") &&
              add_channel(v.s, 31, 1, 0, 13, 0, "s0") && add_channel(v.s, 32, 1, 0, 2, 1, "v1") &&
              add_channel(v.s, 33, 1, 0, 13, 1, "s1") && add_channel(v.s, 34, 1, 0, 0, 9, "text") &&
              list_ready(v),
          "list order: stage fixture");
    constexpr u64 stage[]{34, 31, 32, 33, 30};
    check(list_is(v, stage) && comparator_agrees(v),
          "list order: stage channels order among voice channels by position, then ID");

    // Uncategorized channels (including one whose category is unknown) lead every category;
    // categories order by position, then ID.
    List u{};
    check(initialize(u.s) && add_guild(u.s, 1, "Lab") && add_channel(u.s, 52, 1, 0, 4, 0, "A") &&
              add_channel(u.s, 70, 1, 0, 4, 0, "B") && add_channel(u.s, 50, 1, 0, 4, 1, "C") &&
              add_channel(u.s, 53, 1, 52, 0, 0, "a") && add_channel(u.s, 71, 1, 70, 0, 0, "b") &&
              add_channel(u.s, 51, 1, 50, 0, 0, "c") && add_channel(u.s, 54, 1, 50, 2, 0, "cv") &&
              add_channel(u.s, 60, 1, 0, 0, 5, "u1") && add_channel(u.s, 61, 1, 0, 2, 0, "u2") &&
              add_channel(u.s, 62, 1, 9999, 0, 0, "orphan") && list_ready(u),
          "list order: uncategorized fixture");
    constexpr u64 top[]{62, 60, 61, 52, 53, 70, 71, 50, 51, 54};
    check(list_is(u, top) && comparator_agrees(u) && u.view.channel_rows[0].depth == 0 &&
              u.view.channel_rows[3].depth == 0 && u.view.channel_rows[4].depth == 1,
          "list order: uncategorized channels lead; categories follow by position then ID");
    // A category move re-sorts the rail: C onto the lead position, then A to the end.
    check(add_channel(u.s, 50, 1, 0, 4, 0, "C"), "list order: category position update");
    constexpr u64 first[]{62, 60, 61, 50, 51, 54, 52, 53, 70, 71};
    check(list_is(u, first) && comparator_agrees(u), "list order: a category moved to the lead position goes first");
    check(add_channel(u.s, 52, 1, 0, 4, 7, "A"), "list order: second category update");
    constexpr u64 last[]{62, 60, 61, 50, 51, 54, 70, 71, 52, 53};
    check(list_is(u, last) && comparator_agrees(u), "list order: a category moved later goes last");
}

void list_order_threads() noexcept {
    List t{};
    check(initialize(t.s) && add_guild(t.s, 1, "Lab") && add_channel(t.s, 80, 1, 0, 0, 1, "text") &&
              add_channel(t.s, 91, 1, 80, 11, 0, "t2") && add_channel(t.s, 90, 1, 80, 11, 0, "t1") &&
              add_channel(t.s, 81, 1, 0, 0, 2, "later") && add_channel(t.s, 82, 1, 0, 2, 0, "voice") &&
              add_channel(t.s, 100, 1, 0, 4, 0, "Cat") && add_channel(t.s, 101, 1, 100, 0, 0, "in") &&
              add_channel(t.s, 102, 1, 101, 11, 0, "thread") && add_channel(t.s, 103, 1, 100, 2, 0, "cv") &&
              list_ready(t),
          "list order: thread fixture");
    constexpr u64 expected[]{80, 90, 91, 81, 82, 100, 101, 102, 103};
    check(list_is(t, expected) && t.view.channel_rows[1].depth == 1 && t.view.channel_rows[7].depth == 2,
          "list order: threads follow their parent, before the next channel, in both placements");
}

// Gateway-driven re-sorting: GUILD_CREATE replaces a server's channels, CHANNEL_UPDATE moves one.
struct Wire {
    ChatStateStore store;
    GuildRecord guild[1];
    ChannelRecord channels[16];
    RoleRecord roles[4];
    PermissionOverwriteRecord overwrites[8];
    u16 guild_order[1], channel_order[16];
    u8 names[2048];
    u32 structurals[2048];
    char payload[8192];
    GuildViewRow guild_rows[1];
    ChannelViewRow channel_rows[16];
    u16 sorting[16];
    u8 visible[16];
    ChannelSortKey keys[16];
    NavigationView view;
    bool apply(Utf8View wire) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> output{};
        bool handled = false;
        event.kind = GatewayClientEventKind::dispatch;
        if (!gateway_decode_envelope(wire, {structurals, 2048}, 32, &envelope).ok() ||
            !gateway_client_route_state_dispatch(envelope, {structurals, 2048}, 32, {payload, sizeof(payload)}, 1,
                                                 &event, &output, &handled)
                 .ok() ||
            !handled)
            return false;
        event.dispatch_payload_bytes = output.count;
        return chat_state_apply_gateway_event(&store, event, output, {}).ok();
    }
    template <u32 N>
    bool listed(const u64 (&ids)[N]) noexcept {
        return navigation_view_rebuild(&view, store).ok() && rows_are(view, ids, N);
    }
};
Wire g_wire{};

void list_order_gateway() noexcept {
    auto& w = g_wire;
    check(chat_state_store_initialize(&w.store, {w.guild, 1}, {w.guild_order, 1}, {w.channels, 16},
                                      {w.channel_order, 16}, {w.names, sizeof(w.names)}) &&
              chat_state_store_bind_permissions(&w.store, {w.roles, 4}, {w.overwrites, 8}) &&
              navigation_view_initialize(&w.view, {w.guild_rows, 1}, {w.channel_rows, 16}, {w.sorting, 16},
                                         {w.visible, 16}, {w.keys, 16}),
          "list order gateway: fixture");
    w.store.self_user_id = 1;
    // Payload order is unrelated to the list order; channel 30 is denied View Channel.
    check(w.apply(utf8_literal(
              R"({"op":0,"t":"GUILD_CREATE","d":{"id":"7","name":"Guild","owner_id":"2","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],)"
              R"("members":[{"user":{"id":"1"},"roles":[]}],"channels":[)"
              R"({"id":"20","type":2,"name":"voice","position":0,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"21","type":0,"name":"text","position":3,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"22","type":0,"name":"late","position":4,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"10","type":4,"name":"Cat","position":1,"permission_overwrites":[]},)"
              R"({"id":"11","type":4,"name":"Other","position":0,"permission_overwrites":[]},)"
              R"({"id":"30","type":0,"name":"secret","position":0,"parent_id":"11","permission_overwrites":[{"id":"7","type":0,"allow":"0","deny":"1024"}]},)"
              R"({"id":"31","type":15,"name":"forum","position":1,"parent_id":"11","permission_overwrites":[]}]}})")) &&
              chat_state_store_select_guild(&w.store, 7).ok(),
          "list order gateway: GUILD_CREATE");
    constexpr u64 created[]{11, 31, 10, 21, 22, 20};
    check(w.listed(created), "list order gateway: GUILD_CREATE lists by position, hides a denied channel");
    check(w.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"10","guild_id":"7","type":4,"name":"Cat","position":0,"parent_id":null}})")),
          "list order gateway: category CHANNEL_UPDATE");
    constexpr u64 tied[]{10, 21, 22, 20, 11, 31};
    check(w.listed(tied), "list order gateway: a category moved onto a tie lists by ID");
    check(w.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"22","guild_id":"7","type":0,"name":"late","position":1,"parent_id":"10"}})")),
          "list order gateway: channel CHANNEL_UPDATE");
    constexpr u64 reordered[]{10, 22, 21, 20, 11, 31};
    check(w.listed(reordered), "list order gateway: a channel moved up passes its siblings");
    check(w.apply(utf8_literal(R"({"op":0,"t":"CHANNEL_UPDATE","d":{"id":"10","guild_id":"7","type":4,"name":"Cat","position":5,"parent_id":null}})")),
          "list order gateway: category moved later");
    constexpr u64 later[]{11, 31, 10, 22, 21, 20};
    check(w.listed(later), "list order gateway: a category moved later follows the others");
    check(w.apply(utf8_literal(
              R"({"op":0,"t":"GUILD_CREATE","d":{"id":"7","name":"Guild","owner_id":"2","roles":[{"id":"7","name":"Everyone","permissions":"1024"}],)"
              R"("members":[{"user":{"id":"1"},"roles":[]}],"channels":[)"
              R"({"id":"31","type":15,"name":"forum","position":0,"parent_id":"11","permission_overwrites":[]},)"
              R"({"id":"11","type":4,"name":"Other","position":3,"permission_overwrites":[]},)"
              R"({"id":"21","type":0,"name":"text","position":2,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"22","type":0,"name":"late","position":1,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"20","type":2,"name":"voice","position":0,"parent_id":"10","permission_overwrites":[]},)"
              R"({"id":"10","type":4,"name":"Cat","position":0,"permission_overwrites":[]}]}})")),
          "list order gateway: GUILD_CREATE again");
    constexpr u64 resynced[]{10, 22, 21, 20, 11, 31};
    check(w.listed(resynced) && !chat_state_store_find_channel(w.store, 30),
          "list order gateway: a repeated GUILD_CREATE replaces and re-sorts the channels");
}

void run() noexcept {
    list_order_kinds();
    list_order_voice_and_uncategorized();
    list_order_threads();
    list_order_gateway();
    hide_muted();
    channel_opt_in();
    lifecycle_requests();
    folders();
    reorder();
    drag();
    text_in_voice();
    server_menu();
}
} // namespace maicord::navigation_wave_test

extern "C" __declspec(noreturn) void WINAPI maicord_navigation_wave_entry() noexcept {
    maicord::navigation_wave_test::run();
    ExitProcess(maicord::navigation_wave_test::failures ? 1 : 0);
}

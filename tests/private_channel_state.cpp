#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/private_channel_title.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/app/user_store_dispatch.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

void test_private_channel_state(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore store{};
    GuildRecord guilds[4]{};
    ChannelRecord channels[8]{};
    RoleRecord roles[4]{};
    PermissionOverwriteRecord overwrites[8]{};
    PrivateChannelStore private_store{};
    PrivateChannelRecord private_records[8]{};
    u64 private_recipients[32]{};
    UserStore users{};
    UserRecord user_records[8]{};
    u32 user_order[8]{};
    u8 user_names[2048]{};
    u16 guild_order[4]{}, channel_order[8]{};
    u8 names[2048]{};
    check(chat_state_store_initialize(&store, {guilds, 4}, {guild_order, 4}, {channels, 8},
              {channel_order, 8}, {names, 2048}) &&
          chat_state_store_bind_permissions(&store, {roles, 4}, {overwrites, 8}), "private state initialize");
    store.self_user_id = 1;
    check(private_channel_store_initialize(&private_store, {private_records, 8}, {private_recipients, 32}) &&
          user_store_initialize(&users, {user_records, 8}, {user_order, 8}, {user_names, 2048}),
          "private recipient and user storage initialize");
    store.private_channels = &private_store;
    u32 scratch[1024]{};
    char packet[4096]{};
    u64 ids[16]{};
    const auto ready_decode = [&](Utf8View json, GatewayReady* ready) noexcept {
        GatewayEnvelope envelope{};
        return gateway_decode_envelope(json, {scratch, 1024}, 32, &envelope).ok() &&
            gateway_decode_ready(envelope.data, {scratch, 1024}, 32, ready).ok();
    };
    GatewayReady ready{};
    check(ready_decode(utf8_literal(R"({"op":0,"s":1,"t":"READY","d":{"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","private_channels":[{"id":"10","type":1,"recipient_ids":["2"]}],"users":[{"id":"2","username":"peer"}]}})"), &ready) &&
          ready.private_channels.kind == JsonValueKind::array && ready.users.kind == JsonValueKind::array &&
          ready.private_channels.source.bytes != 0 && ready.users.source.bytes != 0,
          "READY retains compressed private channels and shared user objects as JSON views");
    check(!ready_decode(utf8_literal(R"({"op":0,"s":1,"t":"READY","d":{"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","private_channels":{}}})"), &ready) &&
          !ready_decode(utf8_literal(R"({"op":0,"s":1,"t":"READY","d":{"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","users":[],"users":[]}})"), &ready),
          "READY rejects malformed private arrays and duplicate shared users");
    const auto apply = [&](Utf8View json) noexcept {
        GatewayEnvelope envelope{};
        GatewayClientEvent event{};
        ConstSlice<u8> payload{};
        return gateway_decode_envelope(json, {scratch, 1024}, 32, &envelope).ok() &&
            gateway_client_route_dispatch(envelope, {scratch, 1024}, 32, {packet, 4096}, {ids, 16},
                store.self_user_id, &event, &payload).ok() &&
            (event.dispatch_kind == GatewayClientDispatchKind::user_snapshot ?
                user_store_apply_gateway_snapshots(&users, event, payload) :
                chat_state_apply_gateway_event(&store, event, payload, {ids, 4}, &users).ok());
    };
    check(apply(utf8_literal(R"({"op":0,"s":1,"t":"USER_UPDATE","d":{"id":"7","username":"quillfern","global_name":"𝕼𝖚𝖎𝖑𝖑𝕱𝖊𝖗𝖓"}})")) &&
        user_store_find(users, 7) &&
        utf8_equal(user_store_username(users, *user_store_find(users, 7)), utf8_literal("quillfern")) &&
        utf8_equal(user_store_name(users, *user_store_find(users, 7)), utf8_literal("𝕼𝖚𝖎𝖑𝖑𝕱𝖊𝖗𝖓")),
        "gateway_snapshot_retains_distinct_username_and_styled_display_name");
    check(apply(utf8_literal(R"({"op":0,"s":1,"t":"CHANNEL_CREATE","d":{"id":"10","type":1,"last_message_id":"100","recipients":[{"id":"2","username":"peer"}]}})")),
          "guildless DM channel-create routes through state packet pipeline");
    GatewayEnvelope metadata_envelope{};metadata_envelope.opcode=0;metadata_envelope.has_event_name=true;
    metadata_envelope.event_name={utf8_literal("CHANNEL_UPDATE"),0,false};
    metadata_envelope.data={utf8_literal(R"({"id":"10","type":1,"recipient_ids":["2"]})"),0,JsonValueKind::object};
    GatewayClientEvent metadata_event{},ordinary_event{};ConstSlice<u8> metadata_payload{},ordinary_payload{};
    const bool metadata_routed=gateway_client_route_dispatch(metadata_envelope,{scratch,1024},32,{packet,4096},{ids,16},1,&metadata_event,&metadata_payload).ok()&&
        metadata_event.notification_payload_bytes==sizeof(GatewayClientNotificationSidecar)&&
        gateway_client_notification_trim(metadata_event,metadata_payload,&ordinary_event,&ordinary_payload);
    check(metadata_routed&&ordinary_payload.count==sizeof(GatewayClientUserMutation)&&
        chat_state_apply_gateway_event(&store,metadata_event,metadata_payload,{ids,4},&users).ok(),
        "direct private state consumer validates metadata and applies exact recipient body prefix");
    if(metadata_routed&&ordinary_payload.count==sizeof(GatewayClientUserMutation)) {
        u8 original_body[sizeof(GatewayClientUserMutation)]{};copy_bytes(original_body,ordinary_payload.data,sizeof(original_body));
        GatewayClientNotificationSidecar metadata{};ConstSlice<u8> metadata_rows{};
        const bool copied=gateway_client_notification_sidecar({metadata_payload.data+ordinary_payload.count,metadata_event.notification_payload_bytes},&metadata,&metadata_rows);
        const u32 channel_mutations=store.channel_mutations,users_revision=users.revision,recipient_revision=private_store.revision;
        metadata.flags|=1u<<31;copy_bytes(packet+ordinary_payload.count,&metadata,sizeof(metadata));
        check(copied&&!chat_state_apply_gateway_event(&store,metadata_event,metadata_payload,{ids,4},&users).ok()&&
            store.channel_mutations==channel_mutations&&users.revision==users_revision&&private_store.revision==recipient_revision&&
            bytes_equal(original_body,ordinary_payload.data,sizeof(original_body)),
            "malformed policy tail rejects direct private state admission before identity or body mutation");
        metadata.flags&=~(1u<<31);copy_bytes(packet+ordinary_payload.count,&metadata,sizeof(metadata));
        check(gateway_client_route_dispatch(metadata_envelope,{scratch,1024},32,{packet,sizeof(original_body)},{ids,16},1,&metadata_event,&metadata_payload).ok()&&
            metadata_event.notification_payload_bytes==kGatewayNotificationMetadataIncomplete&&metadata_payload.count==sizeof(original_body)&&
            chat_state_apply_gateway_event(&store,metadata_event,metadata_payload,{ids,4},&users).ok()&&
            private_channel_peer(store,10)==2&&bytes_equal(original_body,metadata_payload.data,sizeof(original_body)),
            "no-header policy marker preserves ordinary private recipient admission without extra scratch space");
    }
    check(apply(utf8_literal(R"({"op":0,"s":2,"t":"CHANNEL_CREATE","d":{"id":"11","type":3,"name":"Study group","owner_id":"2","last_message_id":"101","recipients":[{"id":"2","username":"peer"},{"id":"3","username":"friend"}]}})")),
          "group DM channel-create routes without guild overwrites");
    const auto* group_identity = private_channel_store_find(private_store, 11);
    MemberListRow dm_rows[8]{};
    MemberListView dm_members{};
    dm_members.rows = {dm_rows, 8};
    GuildMemberStore no_guild_members{};
    RelationshipRecord relationship_records[4]{};
    RelationshipStore relationships{};
    check(relationship_store_initialize(&relationships, {relationship_records, 4}), "relationship storage initialize");
    const MemberListPrivatePresence no_presence{nullptr, GuildMemberStatus::unknown};
    const auto dm_projected = member_list_view_private(&dm_members, users, store, 11, no_presence);
    check(dm_projected.ok() && dm_members.member_count == 3 && dm_members.count == 4 &&
        dm_rows[1].user_id == 2 && dm_rows[2].user_id == 3 && dm_rows[3].user_id == 1 &&
        utf8_equal(member_list_row_label(dm_rows[1], no_guild_members, users, store), utf8_literal("peer")) &&
        !member_list_view_private(&dm_members, users, store, 11, no_presence).changed,
        "group DM member projection contains recipients and self without guild records and retains unchanged view");
    check(dm_rows[1].status == GuildMemberStatus::offline && dm_rows[2].status == GuildMemberStatus::offline &&
              dm_rows[3].status == GuildMemberStatus::unknown,
          "group DM members without any presence read offline; self waits for its session status");
    // Presence sources advance no shared revision: each call re-resolves the
    // rows, so the projection changes without a user or channel revision.
    RelationshipRecord peer_relation{};
    peer_relation.user_id = 2;
    peer_relation.type = RelationshipType::friend_;
    peer_relation.presence = RelationshipPresence::dnd;
    check(relationship_store_upsert(&relationships, peer_relation).ok() &&
              user_store_set_client_status(&users, 3,
                                           client_status_with(client_status_with(0, ClientPlatform::desktop,
                                                                                 ClientPlatformStatus::idle),
                                                              ClientPlatform::mobile, ClientPlatformStatus::online)),
          "group DM presence fixtures apply");
    const u32 presence_users_revision = users.revision;
    const MemberListPrivatePresence presence{&relationships, GuildMemberStatus::idle};
    check(member_list_view_private(&dm_members, users, store, 11, presence).changed &&
              users.revision == presence_users_revision && dm_rows[1].status == GuildMemberStatus::dnd &&
              dm_rows[2].status == GuildMemberStatus::online && dm_rows[3].status == GuildMemberStatus::idle &&
              !member_list_view_private(&dm_members, users, store, 11, presence).changed,
          "group DM members show a relationship's presence, any online platform and the caller's own status");
    check(relationship_store_presence(&relationships, 2, RelationshipPresence::unknown).changed &&
              user_store_set_client_status(&users, 2, client_status_with(0, ClientPlatform::web,
                                                                         ClientPlatformStatus::idle)) &&
              user_store_set_client_status(&users, 3, 0) &&
              member_list_view_private(&dm_members, users, store, 11, presence).changed &&
              dm_rows[1].status == GuildMemberStatus::idle && dm_rows[2].status == GuildMemberStatus::offline,
          "group DM members fall back to their latest platforms when a relationship has no presence");
    check(member_list_view_private(&dm_members, users, store, 999, no_presence).ok() && dm_members.count == 0 &&
        member_list_view_private(&dm_members, users, store, 11, no_presence).ok() && dm_members.member_count == 3,
        "group DM projection clears removed channels and rebuilds when changing conversations");
    // The owner's crown follows the name, and stays inside the line for a long name.
    const UiRect name_line{100, 10, 260, 30};
    const auto short_crown = ui_private_member_crown_rectangle(name_line, 40);
    const auto long_crown = ui_private_member_crown_rectangle(name_line, 400);
    check(short_crown.left == 100 + 40 + kUiPrivateMemberCrownGap &&
              ui_rect_width(short_crown) == kUiPrivateMemberCrownExtent && short_crown.top == 20 - 7 &&
              long_crown.right == name_line.right,
          "group DM member list: the owner's crown sits after the name, clamped to the name line");
    MemberListScene dm_scene{};
    MemberListRowWidget dm_scene_rows[8]{};
    UiWidget dm_widgets[41]{}; UiLayoutNode dm_layout[41]{};
    check(member_list_scene_initialize(&dm_scene, {dm_scene_rows, 8}, {dm_widgets, 41}, {dm_layout, 41}) &&
        member_list_scene_update(&dm_scene, &dm_members, &no_guild_members, &users, &store, {0, 0, 240, 400}, 0).ok() &&
        dm_scene.count == 4 && member_list_scene_focus_user(&dm_scene, 3),
        "group DM member widgets materialize and accept focus without a guild member store");
    check(group_identity && group_identity->owner_id == 2 && group_identity->recipient_count == 2 &&
          private_channel_peer(store, 10) == 2 && user_store_find(users, 2) && user_store_find(users, 3),
          "private recipients owner and user snapshots survive JSON packet reuse");
    check(apply(utf8_literal(R"({"op":0,"s":2,"t":"READY_USER","d":{"id":"2","username":"peer","global_name":"Zo\u00eb","avatar":"abcdefabcdefabcdefabcdefabcdefab"}})")) &&
          user_store_find(users, 2)->avatar_hash_bytes == 32,
          "READY shared-user snapshot hydrates global display name and avatar");
    check(apply(utf8_literal(R"({"op":0,"s":2,"t":"CHANNEL_UPDATE","d":{"id":"10","type":1,"recipient_ids":["2"]}})")),
          "compressed private recipient IDs reuse shared user identity");
    char title_buffer[128]{};
    const auto dm_title = private_channel_title(store, &users, *chat_state_store_find_channel(store, 10), {title_buffer, 128});
    check(dm_title.bytes == 4 && bytes_equal(dm_title.data, "Zo\xc3\xab", 4), "DM label uses hydrated Unicode global name");
    const u32 private_revision = private_store.revision;
    check(!apply(utf8_literal(R"({"op":0,"s":2,"t":"CHANNEL_UPDATE","d":{"id":"10","type":1,"recipient_ids":["2","2"]}})")) &&
          private_store.revision == private_revision &&
          !apply(utf8_literal(R"({"op":0,"s":2,"t":"READY_USER","d":{"id":"2","username":"peer","avatar":"bad"}})")) &&
          !apply(utf8_literal(R"({"op":0,"s":2,"t":"READY_USER","d":{"id":"2","username":"a","username":"b"}})")),
          "duplicate recipients and malformed user fields cannot change private identity");
    const auto* dm = chat_state_store_find_channel(store, 10);
    const auto* group = chat_state_store_find_channel(store, 11);
    constexpr u32 forbidden = channel_action_manage_messages | channel_action_manage_channel |
        channel_action_manage_threads | channel_action_mention_everyone;
    check(dm && group && dm->guild_id == 0 && group->guild_id == 0 &&
          chat_state_store_channel_visible(store, *dm) && dm->permissions_resolved &&
          (dm->actions & channel_action_send_messages) && !(dm->actions & forbidden) &&
          !(group->actions & forbidden) && store.guild_count == 0 && store.role_count == 0 && store.overwrite_count == 0,
          "private channels expose chat actions without guild administrative permissions");
    check(chat_state_store_select_channel(&store, 11).ok() && store.selected_guild_id == 0 &&
          store.selected_channel_id == 11, "private channel selection needs no synthetic guild");
    check(chat_state_store_note_message(&store, 10, 110, 2, false, false) &&
          chat_state_store_channel_unread(store, *dm) && dm->last_message_id == 110 &&
          chat_state_store_note_message(&store, 10, 111, 2, true, false) && dm->mention_count == 1 &&
          read_state_mark_local(&store, 10, dm->last_message_id, dm->read_last_viewed, dm->read_flags) &&
          !chat_state_store_channel_unread(store, *dm) &&
          dm->mention_count == 0 && dm->last_read_message_id == 111,
          "private activity and read state do not require guild aggregates");
    check(chat_state_store_note_message(&store, 10, 112, 1, false, false) &&
          !chat_state_store_channel_unread(store, *dm), "own DM messages stay read");
    check(!chat_state_store_can_replace_channel_overwrites(store, 10, 1) &&
          !chat_state_store_append_channel_overwrite(&store, 10, {2, ~0ull, 0, 1}),
          "private channel cannot acquire guild permission overwrites");
    check(!apply(utf8_literal(R"({"op":0,"s":3,"t":"CHANNEL_CREATE","d":{"id":"12","type":1,"guild_id":"99"}})")) &&
          !apply(utf8_literal(R"({"op":0,"s":3,"t":"CHANNEL_CREATE","d":{"id":"12","type":3,"parent_id":"10"}})")) &&
          !apply(utf8_literal(R"({"op":0,"s":3,"t":"CHANNEL_CREATE","d":{"id":"12","type":3,"permission_overwrites":[]}})")) &&
          !apply(utf8_literal(R"({"op":0,"s":3,"t":"CHANNEL_CREATE","d":{"id":"12","type":0}})")) &&
          store.channel_count == 2, "malformed private and guild channel scopes are rejected atomically");
    NavigationView view{};
    GuildViewRow guild_rows[4]{};
    ChannelViewRow channel_rows[8]{};
    u16 sorting[8]{};
    u8 visible[8]{};
    ChannelSortKey keys[8]{};
    check(navigation_view_initialize(&view, {guild_rows, 4}, {channel_rows, 8}, {sorting, 8},
              {visible, 8}, {keys, 8}) && navigation_view_rebuild(&view, store).ok() &&
          view.channel_count == 2 && view.channel_rows[0].selectable && view.channel_rows[1].selectable &&
          view.selected_guild_id == 0 && view.selected_channel_id == 11,
          "private channels project into native navigation after selection");
    check(apply(utf8_literal(R"({"op":0,"s":4,"t":"CHANNEL_UPDATE","d":{"id":"11","type":3,"name":"Renamed group"}})")) &&
          chat_state_store_channel_name(store, *group).bytes == 13 &&
          bytes_equal(chat_state_store_channel_name(store, *group).data, "Renamed group", 13),
          "group DM name updates through normal state route");
    check(apply(utf8_literal(R"({"op":0,"s":4,"t":"CHANNEL_UPDATE","d":{"id":"11","type":3,"name":null}})")) &&
          chat_state_store_channel_name(store, *group).bytes == 0,
          "explicit null group name clears old title");
    const auto group_title = private_channel_title(store, &users, *group, {title_buffer, 128});
    check(group_title.bytes == 12 && bytes_equal(group_title.data, "Zo\xc3\xab, friend", 12),
          "cleared group name falls back to retained recipient display names");
    check(apply(utf8_literal(R"({"op":0,"s":4,"t":"CHANNEL_UPDATE","d":{"id":"11","type":3,"recipient_ids":["1","3"],"icon":null}})")) &&
          private_channel_peer(store, 11) == 3 && private_channel_store_find(private_store, 11)->owner_id == 2,
          "recipient replacement preserves omitted owner and excludes self from peer display");
    check(apply(utf8_literal(R"({"op":0,"s":4,"t":"READY_USER","d":{"id":"2","username":"peer-new","global_name":null,"avatar":null}})")) &&
          user_store_find(users, 2)->avatar_hash_bytes == 0,
          "shared-user snapshot clears avatar and falls back to username");
    const auto renamed_dm = private_channel_title(store, &users, *dm, {title_buffer, 128});
    check(renamed_dm.bytes == 8 && bytes_equal(renamed_dm.data, "peer-new", 8),
          "private label follows current user metadata without renaming the channel");
    check(!apply(utf8_literal(R"({"op":0,"s":5,"t":"CHANNEL_DELETE","d":{"id":"11","type":1}})")) &&
          store.channel_count == 2, "mismatched private channel delete cannot remove resident group");
    check(apply(utf8_literal(R"({"op":0,"s":6,"t":"CHANNEL_DELETE","d":{"id":"11","type":3}})")) &&
          !chat_state_store_find_channel(store, 11) && store.selected_channel_id == 10,
          "group DM removal repairs selection to remaining private channel");

    // Selection behavior is independent of the separately tested guild role resolver.
    private_channel_store_clear(&private_store);
    check(chat_state_store_initialize(&store, {guilds, 4}, {guild_order, 4}, {channels, 8},
              {channel_order, 8}, {names, 2048}),
          "private state reinitializes without permission banks");
    store.private_channels = &private_store;
    check(chat_state_store_select_home(&store).ok() && store.home_selected &&
          store.selected_guild_id == 0 && store.selected_channel_id == 0,
          "Home can be selected without private channels or guilds");
    check(chat_state_store_upsert_guild(&store, {99, utf8_literal("Server"), GuildMutationKind::create, 1}).ok() &&
          chat_state_store_upsert_channel(&store, {100, 99, 0, utf8_literal("general"), 0, 0, 0}).ok() &&
          store.home_selected && store.selected_channel_id == 0,
          "guild hydration cannot steal empty Home selection");
    check(chat_state_store_upsert_channel(&store, {10, 0, 0, {}, 0, 0, 1}).ok() &&
          store.home_selected && store.selected_channel_id == 10 &&
          chat_state_store_select_guild(&store, 99).ok() && !store.home_selected &&
          store.selected_channel_id == 100 && chat_state_store_select_home(&store).ok() &&
          store.home_selected && store.selected_channel_id == 10,
          "Home and guild selection share normal channel state transitions");
    check(chat_state_store_remove_channel(&store, 10) && store.home_selected &&
          store.selected_channel_id == 0 && !chat_state_store_select_default(&store),
          "closing the final DM stays in Home instead of selecting a guild channel");

    UiScene shell{};
    check(ui_scene_initialize_bootstrap(&shell) && ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280, 800), "Home shell initializes");
    const auto* home = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::home_button));
    const auto* rail = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::guild_navigation));
    check(home && rail && home->rectangle.bottom <= rail->rectangle.top &&
          ui_widget_manager_input_enabled(shell.widgets, home->widget) &&
          ui_widget_manager_focus(&shell.widgets, home->widget),
          "Home is a shared focusable widget outside the virtual guild viewport");
    UiTextCommand text_commands[2]{};
    UiTextCommandStream text_stream{};
    char owned_text[16]{};
    char temporary_text[] = "Peer";
    check(ui_text_command_stream_initialize(&text_stream, {text_commands, 2}, {owned_text, 16}),
          "owned text stream initializes caller storage");
    UiTextCommand text_command{};
    text_command.owner = {&shell.widgets, home->widget};
    text_command.text = {temporary_text, 4};
    text_command.rectangle = text_command.clip = home->rectangle;
    check(ui_text_command_stream_push_owned(&text_stream, text_command), "generated label copies into stream lifetime");
    temporary_text[0] = 'X';
    check(text_commands[0].text.data[0] == 'P' && text_stream.owned_bytes == 4, "retained label never borrows transient formatter buffer");
    ui_text_command_stream_reset(&text_stream);
    check(text_stream.owned_bytes == 0 && ui_text_command_stream_push_owned(&text_stream, text_command),
          "text stream reset reuses its owned byte bank");
    Arena generated_text{};
    check(arena_initialize(&generated_text, 8192, 0, 4096) &&
          ui_text_command_stream_initialize(&text_stream, {text_commands, 2}, {}, &generated_text) &&
          ui_text_command_stream_push_owned(&text_stream, text_command) && generated_text.offset == 4,
          "generated labels can use caller-owned demand-committed arena storage");
    UiTextCommand invalid_text = text_command;
    invalid_text.owner.handle = ui_widget_invalid_handle();
    check(!ui_text_command_stream_push_owned(&text_stream, invalid_text) && generated_text.offset == 4 && text_stream.count == 1,
          "rejected owned text rolls its allocation back");
    ui_text_command_stream_reset(&text_stream);
    check(generated_text.offset == 0 && text_stream.owned_bytes == 0,
          "arena-backed text reset restores the stream entry mark");
    arena_release(&generated_text);
    UiLayoutDiagnostic diagnostics[32]{};
    check(ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(), "Home shell layout validates");
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = ui_shell_layout_navigation;
    metrics.member_sidebar_width = 0;
    check(ui_scene_layout_with_metrics(&shell, UiShellView::private_channels, metrics, 960, 600) &&
          !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_toggle)) &&
          !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::composer)) &&
          ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(),
          "empty Home has no guild member controls or composer and validates after scope switch");
    metrics.revision += 1;
    metrics.flags = 0;
    metrics.navigation_header_height = 0;
    check(ui_scene_layout_with_metrics(&shell, UiShellView::private_channels, metrics, 960, 600) &&
          ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 32}).clean(),
          "Home control does not depend on an optional navigation sidebar header");
    ui_scene_release(&shell);

    PrivateChannelStore owned{};
    Win32SystemInfo system_info{};
    GetSystemInfo(system_info.native());
    const u64 recipients_a[] = {20, 21}, recipients_b[] = {30, 31, 32};
    check(private_channel_store_initialize_owned(&owned, 4096, 65'536) && owned.record_backing.committed == 0 &&
          owned.recipient_backing.committed == 0, "private backing reserves without startup commitment");
    check(private_channel_store_replace(&owned, {20, 2, {recipients_a, 2}, {}, true, true, false}) &&
          private_channel_store_replace(&owned, {10, 3, {recipients_b, 3}, {}, true, true, false}),
          "private metadata grows on demand while keeping ordered lookup");
    const auto alias = private_channel_store_recipients(owned, *private_channel_store_find(owned, 10));
    check(private_channel_store_replace(&owned, {20, 0, alias, {}, true, false, false}) &&
          private_channel_store_remove(&owned, 10) && owned.recipient_count == 3 &&
          private_channel_store_recipients(owned, *private_channel_store_find(owned, 20))[1] == 31 &&
          owned.record_backing.committed == system_info.dwPageSize &&
          owned.recipient_backing.committed == system_info.dwPageSize,
          "recipient pool compaction preserves aliased input and commits only touched pages");
    const u32 unchanged_revision = owned.revision;
    check(private_channel_store_replace(&owned, {20, 0, {recipients_b, 3}, {}, true, false, false}) &&
          owned.revision == unchanged_revision, "unchanged recipient lists preserve metadata revision");
    private_channel_store_release(&owned);
    check(!owned.records.data && !owned.record_backing.base && !owned.recipient_backing.base,
          "private backing releases both reservations");

    UserStore compact_users{};
    UserRecord compact_records[3]{};
    u32 compact_order[3]{};
    u8 compact_names[24]{};
    check(user_store_initialize(&compact_users, {compact_records, 3}, {compact_order, 3}, {compact_names, 24}) &&
          user_store_upsert(&compact_users, 1, utf8_literal("一二")).ok() &&
          user_store_upsert(&compact_users, 2, utf8_literal("三四")).ok() &&
          user_store_upsert(&compact_users, 3, utf8_literal("五六")).ok() &&
          compact_users.count == 2 && compact_users.evictions == 1 && !user_store_find(compact_users, 1) &&
          user_store_upsert(&compact_users, 2, utf8_literal("三四七")).ok() &&
          compact_users.count == 1 && user_store_find(compact_users, 2),
          "Unicode name-byte pressure evicts older users without evicting the update target");
    check(user_store_initialize(&compact_users, {compact_records, 3}, {compact_order, 3}, {compact_names, 24}) &&
        user_store_upsert(&compact_users, 1, utf8_literal("A"), utf8_literal("alice")).ok() &&
        user_store_upsert(&compact_users, 2, utf8_literal("B"), utf8_literal("bob")).ok() &&
        user_store_upsert(&compact_users, 1, utf8_literal("AA")).ok() &&
        user_store_upsert(&compact_users, 1, utf8_literal("AAA")).ok() && compact_users.compactions > 0 &&
        user_store_find(compact_users, 1) && user_store_find(compact_users, 2) &&
        utf8_equal(user_store_username(compact_users, *user_store_find(compact_users, 1)), utf8_literal("alice")) &&
        utf8_equal(user_store_username(compact_users, *user_store_find(compact_users, 2)), utf8_literal("bob")) &&
        user_store_upsert(&compact_users, 3, utf8_literal("C"), utf8_literal("cat")).ok() &&
        !user_store_find(compact_users, 2) && user_store_find(compact_users, 1),
        "paired_display_and_username_storage_survives_compaction_and_bounded_LRU_pressure");
    GatewayEnvelope self_update{};self_update.opcode=0;self_update.has_event_name=true;
    self_update.event_name={utf8_literal("USER_UPDATE"),0,false};
    self_update.data={utf8_literal(R"({"id":"1","username":"Self","restricted_schedule":{"rules":[]}})"),0,JsonValueKind::object};
    GatewayClientEvent user_event{},ordinary_user{};ConstSlice<u8> user_payload{},user_body{};
    const bool self_routed=gateway_client_route_dispatch(self_update,{scratch,1024},32,{packet,4096},{ids,16},1,&user_event,&user_payload).ok()&&
        user_event.notification_payload_bytes==sizeof(GatewayClientNotificationSidecar)&&
        gateway_client_notification_trim(user_event,user_payload,&ordinary_user,&user_body);
    check(self_routed&&user_store_apply_gateway_snapshots(&users,user_event,user_payload)&&
        user_store_find(users,1)&&utf8_equal(user_store_name(users,*user_store_find(users,1)),utf8_literal("Self")),
        "event-aware user admission preserves self identity beside global restriction metadata");
    if(self_routed&&user_body.count>sizeof(GatewayClientUserMutation)) {
        const u32 user_revision=users.revision;
        GatewayClientNotificationSidecar metadata{};ConstSlice<u8> rows{};
        const bool copied=gateway_client_notification_sidecar({user_payload.data+user_body.count,user_event.notification_payload_bytes},&metadata,&rows);
        packet[sizeof(GatewayClientUserMutation)]='X';metadata.flags|=1u<<31;
        copy_bytes(packet+user_body.count,&metadata,sizeof(metadata));
        check(copied&&!user_store_apply_gateway_snapshots(&users,user_event,user_payload)&&users.revision==user_revision&&
            user_store_find(users,1)&&utf8_equal(user_store_name(users,*user_store_find(users,1)),utf8_literal("Self")),
            "invalid policy metadata prevents a valid changed ordinary user name from partially publishing");
    }
}

} // namespace maicord

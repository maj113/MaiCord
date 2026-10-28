// Compile the shipping controller once in this isolated executable. Private
// ownership is visible here without adding a shipping diagnostic/test hook.
#include "../src/app/application_search.cpp"
#include "../src/app/application_content_input.hpp"
#include "application_search_measurement.hpp"
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord::search_runtime_test {
using namespace app_detail;

struct AccountFixture {
    MessageSearchQuery query;
    u64 request_id;
    u64 cancelled_id;
    u32 submissions;
    u32 cancellations;
    u32 acknowledgements;
    RestClientSubmitError admission;
} account;
u32 failures;
u32 repaints;
u32 forced_account_repaints;
u32 overlay_repaints;
u32 navigations;
u32 search_saves;
u32 content_confirmations;
MessageMediaPolicyResult media_policy{MessageExplicitFilter::show, true};
u32 content_retirements;
u32 navigation_releases;
ApplicationSearchSourceTicket saved_search_ticket;
u64 saved_search_attachment;
bool saved_search_snapshot;
u32 hit_position;
u32 viewer_opens;
u32 viewer_index;
UiRect viewer_origin;
u64 navigated_channel;
u64 navigated_message;
bool settings;
bool downloads;
bool composing;
u32 member_queries;
u32 member_query_withdrawals;
bool member_query_active;
bool composer_query_owned;
u64 member_query_guild;
char member_query[32];
u32 member_query_bytes;
RelationshipRecord choice_relationship_records[1];
RelationshipStore choice_relationships;
GuildMemberRecord choice_member_records[4];
u32 choice_member_order[4];
PrivateChannelStore choice_private_store;
PrivateChannelRecord choice_private_records[1];
u64 choice_private_recipients[2];
BYTE saved_keyboard[256];
bool keyboard_saved;
MessageRecord records[2];
u32 message_order[2];
u8 message_text[128];
u32 row_heights[2];
u64 row_prefixes[2];
RoleRecord permission_roles[2];
PermissionOverwriteRecord permission_overwrites[4];
UiTextMeasureRecord poll_measurements[128];
u32 poll_projection_calls;
u64 projected_poll_id;
u32 projected_poll_publication;

bool measure_poll(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* out) noexcept {
    const auto text = utf8_validate(request.text);
    if (!out || !text.valid || request.constraint.width <= 0)
        return false;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 6));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (text.codepoints + columns - 1) / columns);
    *out = {{minimum(request.constraint.width, text.codepoints * 6.f), lines * 16.f}, 12, lines};
    return true;
}

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

usize committed(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    return pointer && VirtualQuery(pointer, &information, sizeof(information)) &&
                   information.State == MEM_COMMIT
               ? information.RegionSize
               : 0;
}

RestClientSubmitResult WINAPI submit(void*, AccountMessageSearchOperation operation,
                                     const MessageSearchQuery* query,
                                     u64 request_id) noexcept {
    if (operation == AccountMessageSearchOperation::cancel) {
        ++account.cancellations;
        account.cancelled_id = request_id;
        return {query ? RestClientSubmitError::invalid_input : RestClientSubmitError::none};
    }
    if (operation != AccountMessageSearchOperation::submit || !query || !request_id) {
        return {RestClientSubmitError::invalid_input};
    }
    ++account.submissions;
    account.query = *query;
    account.request_id = request_id;
    return {account.admission};
}

RestClientSubmitResult WINAPI acknowledge(void*, u64, u64, ConstSlice<u8>, u64) noexcept {
    ++account.acknowledgements;
    return {};
}

bool modifiers(bool control = false, bool shift = false, bool alt = false) noexcept {
    BYTE keyboard[256]{};
    keyboard[VK_CONTROL] = keyboard[VK_LCONTROL] = control ? 0x80 : 0;
    keyboard[VK_SHIFT] = keyboard[VK_LSHIFT] = shift ? 0x80 : 0;
    keyboard[VK_MENU] = keyboard[VK_LMENU] = alt ? 0x80 : 0;
    return SetKeyboardState(keyboard) != FALSE;
}

bool open(bool shift = false) noexcept {
    if (!modifiers(true, shift)) {
        return false;
    }
    const bool handled = application_search_input(UiInputKind::key_down, 'F', false);
    const bool tail = application_search_input(UiInputKind::key_up, 'F', false);
    return modifiers() && handled && tail && application_search_edit_focused();
}

bool key(u32 value) noexcept {
    const bool down = application_search_input(UiInputKind::key_down, value, false);
    const bool up = application_search_input(UiInputKind::key_up, value, false);
    return down && up;
}

bool type(Utf8View text) noexcept {
    for (u32 index = 0; index < text.bytes; ++index) {
        if (!application_search_input(UiInputKind::character, static_cast<u8>(text.data[index]), false)) {
            return false;
        }
    }
    return true;
}

bool query(Utf8View text) noexcept {
    return open(true) && type(text) && key(VK_RETURN);
}

LPARAM point(UiRect bounds) noexcept {
    return MAKELPARAM(static_cast<short>((bounds.left + bounds.right) * .5f),
                      static_cast<short>((bounds.top + bounds.bottom) * .5f));
}

UiRect action_bounds(UiSearchAction action) noexcept {
    const auto* scene = g_app.renderer.search_scene;
    if (!scene) {
        return {};
    }
    for (u32 slot = 0; slot < scene->widgets.count; ++slot) {
        if (slot >= scene->storage.widgets.count || scene->targets[slot].action != action) {
            continue;
        }
        const auto* node = ui_widget_manager_layout_node(
            scene->widgets, {static_cast<u16>(slot), scene->widgets.generation});
        if (node) {
            return node->rectangle;
        }
    }
    return {};
}

// Thread browser fixture behind the header field's thread scope.
bool browser_open, browser_forum;
u64 browser_session;
u32 browser_searches, browser_closes;
char browser_query[128];
u32 browser_query_bytes;

bool click(UiSearchAction action, UINT press = WM_LBUTTONDOWN) noexcept {
    const auto bounds = action_bounds(action);
    if (bounds.left >= bounds.right || bounds.top >= bounds.bottom) {
        return false;
    }
    return application_search_window_message(press, 0, point(bounds)) &&
           application_search_window_message(WM_LBUTTONUP, 0, point(bounds));
}

bool response(u64 request_id, u16 status, Utf8View body = {}, u64 retry_at = 0,
              u64 retry_after_ms = 0) noexcept {
    RestClientResponse value{};
    value.request_id = request_id;
    value.status_code = status;
    value.retry_at_ms = retry_at;
    value.rate.retry_after_ms = retry_after_ms;
    return application_search_response(value, body);
}

constexpr auto result_body = utf8_literal(
    R"({"total_results":41,"messages":[[{"id":"300","channel_id":"70","author":{"id":"3","username":"Original fixture author"},"content":"First original fixture hit","timestamp":"2026-01-01T00:00:00Z","hit":true},{"id":"299","channel_id":"70","author":{"id":"4","username":"Context fixture author"},"content":"Original fixture context","timestamp":"2026-01-01T00:00:00Z","hit":false}],[{"id":"280","channel_id":"70","author":{"id":"3","username":"Original fixture author"},"content":"Second original fixture hit","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");

bool initialize() noexcept {
    keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord search runtime fixture", WS_POPUP, -16000,
                                   -16000, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!keyboard_saved || !g_app.window || !g_app.account_ui || !modifiers()) {
        return false;
    }
    auto& ui = *g_app.account_ui;
    if (!message_poll_store_initialize_owned(&ui.message_poll_store, 16, 16384) ||
        !ui_text_measure_cache_initialize(&ui.text_measure_cache, {poll_measurements, 128}, measure_poll,
                                          nullptr) ||
        !chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                        {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                        {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096}) ||
        !chat_state_store_bind_permissions(&ui.chat_state, {permission_roles, 2},
                                              {permission_overwrites, 4}) ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
                                       {ui.user_order_storage, 4}, {ui.user_name_storage, 4096}) ||
        !chat_state_store_upsert_guild(&ui.chat_state, {7, utf8_literal("Original fixture guild"),
                                                              GuildMutationKind::create, 5})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {70, 7, 0, utf8_literal("fixture-channel"), 0, 0, 0})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {80, 0, 0, utf8_literal("fixture-dm"), 0, 0, 1})
             .ok() ||
        !chat_state_store_upsert_guild(&ui.chat_state, {8, utf8_literal("Successor fixture guild"),
                                                              GuildMutationKind::create, 5})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {90, 8, 0, utf8_literal("foreign-channel"), 0, 0, 0})
             .ok() ||
        !ui_scene_initialize_bootstrap(&g_app.shell_scene) ||
        !ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                      kGuildChannelShellLayoutMetrics, 1280, 720)) {
        return false;
    }
    ui.chat_state.self_user_id = 5;
    constexpr u64 readable = kPermissionViewChannel | kPermissionReadMessageHistory;
    if (!chat_state_store_upsert_role(&ui.chat_state, {7, 7, readable, utf8_literal("everyone")}) ||
        !chat_state_store_upsert_role(&ui.chat_state, {8, 8, readable, utf8_literal("everyone")}) ||
        !chat_state_store_set_self_roles(&ui.chat_state, 7, 5, 5, {}) ||
        !chat_state_store_set_self_roles(&ui.chat_state, 8, 5, 5, {}))
        return false;
    chat_state_store_recompute_guild_permissions(&ui.chat_state, 7);
    chat_state_store_recompute_guild_permissions(&ui.chat_state, 8);
    if (!chat_state_store_select_channel(&ui.chat_state, 70).ok())
        return false;
    ui.active_channel_id = 70;
    ui.user_record_storage[0].id = 5;
    ui.user_record_storage[0].occupied = true;
    ui.user_order_storage[0] = 0;
    ui.user_store.count = 1;
    auto& view = g_app.message_view;
    if (!message_store_initialize(&view.store, {records, 2}, {message_order, 2}, {message_text, 128}) ||
        !message_store_upsert(&view.store, {900, 70, 5, 0, 0, utf8_literal("Existing conversation"), 0})
             .ok() ||
        !ui_variable_list_initialize(&view.list, {row_heights, 2}, {row_prefixes, 2}, 1,
                                     ui_extent_units(57)) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, ui_extent_units(57),
                                     ui_extent_units(480), false)) {
        return false;
    }
    g_app.renderer_ready = true;
    g_app.renderer.width = 1280;
    g_app.renderer.height = 720;
    g_app.renderer.dpi = 96;
    g_app.account.instance = &account;
    g_app.account.status.profile_id = 1;
    g_app.plugin_account_scope_serial = 1;
    g_app.account.api.message_search = submit;
    g_app.account.api.message_ack = acknowledge;
    SetFocus(g_app.window);
    return application_search_synchronize();
}

void release() noexcept {
    application_search_release();
    ui_scene_release(&g_app.shell_scene);
    if (GetCapture() == g_app.window) {
        ReleaseCapture();
    }
    if (g_app.window) {
        DestroyWindow(g_app.window);
    }
    if (g_app.account_ui) {
        message_poll_store_release(&g_app.account_ui->message_poll_store);
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    }
    g_app.window = nullptr;
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    g_app.context_focus = nullptr;
    if (keyboard_saved) {
        SetKeyboardState(saved_keyboard);
    }
}
usize bank_committed(const app_detail::SearchBank* b) noexcept {
    return b ? committed(b) + b->content_storage.arena.committed + b->rich_storage.arena.committed +
                   b->user_records.arena.committed + b->user_order.arena.committed +
                   b->rich_text.committed[0] + b->rich_text.committed[1] + b->user_names.committed[0] +
                   b->user_names.committed[1]
             : 0;
}
void rich_bank_capacity() noexcept {
    auto* b = static_cast<SearchBank*>(
        VirtualAlloc(nullptr, sizeof(SearchBank), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* scratch = static_cast<SearchDecodeScratch*>(
        VirtualAlloc(nullptr, sizeof(SearchDecodeScratch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(b && scratch && bank_initialize(*b), "search rich bank: lazy owning storage initializes");
    if (!b || !scratch || !b->initialized) {
        if (scratch)
            VirtualFree(scratch, 0, MEM_RELEASE);
        bank_release(b);
        return;
    }
    for (u32 body = 0; body < 2; ++body) {
        for (u32 j = 0; j < kGatewayMessageAttachmentMaximum; ++j)
            scratch->attachments[body][j] = {
                100 + j,
                17,
                utf8_literal("original.bin"),
                {},
                utf8_literal("Original attachment"),
                utf8_literal("application/octet-stream"),
                utf8_literal("https://cdn.discordapp.com/attachments/70/100/original.bin"),
                320,
                200,
                0,
                true,
                true,
                false};
        for (u32 j = 0; j < kGatewayMessageEmbedMaximum; ++j) {
            auto& e = scratch->embeds[body][j];
            e.title = utf8_literal("Original title");
            e.description = utf8_literal("Original description");
            e.type = GatewayMessageEmbedType::rich;
            e.image.url = utf8_literal("https://example.invalid/original.png");
            e.image.has_width = e.image.has_height = true;
            e.image.width = 320;
            e.image.height = 200;
            e.field_begin = static_cast<u16>(j * 6);
            e.field_count = static_cast<u16>(j == 9 ? 10 : 6);
        }
        for (u32 j = 0; j < kGatewayMessageEmbedFieldMaximum; ++j)
            scratch->fields[body][j] = {utf8_literal("Original field"), utf8_literal("Original value"),
                                        static_cast<u16>(j < 54 ? j / 6 : 9), false};
        for (u32 j = 0; j < kGatewayMessageStickerMaximum; ++j)
            scratch->stickers[body][j] = {500 + j, utf8_literal("Original sticker"), 1};
        for (u32 j = 0; j < kGatewayMessageComponentMaximum; ++j)
            scratch->components[body][j] = {utf8_literal("Original label"),
                                            {},
                                            {},
                                            {},
                                            {},
                                            0,
                                            j,
                                            0,
                                            0xffff,
                                            static_cast<u16>(j),
                                            2,
                                            1,
                                            true,
                                            false};
    }
    for (u32 j = 0; j < kGatewayMessageReactionMaximum; ++j)
        scratch->reactions[j] = {700 + j, utf8_literal("original"), 2, 2, 0, false, false, false};
    for (u32 j = 0; j < kGatewayMessageMentionUserMaximum; ++j) {
        scratch->mention_users[j] = {2000 + j, utf8_literal("Original mention")};
        scratch->snapshot_users[j] = {3000 + j, utf8_literal("Original snapshot mention")};
    }
    MessageSearchMessage item{};
    auto& m = item.message;
    item.guild_known = true;
    item.guild_id = 7;
    item.hit = true;
    m.channel_id = 70;
    m.author_id = 3;
    m.author_name = utf8_literal("Original bank author");
    m.content = utf8_literal("Original full body");
    m.fields = gateway_message_field_content | gateway_message_field_reactions |
               gateway_message_field_attachments | gateway_message_field_embeds |
               gateway_message_field_sticker_items | gateway_message_field_components |
               gateway_message_field_message_reference | gateway_message_field_message_snapshots;
    const auto all = decode_scratch(*scratch);
    m.attachments = {all.rich.attachments.data, all.rich.attachments.count};
    m.embeds = {all.rich.embeds.data, all.rich.embeds.count};
    m.embed_fields = {all.rich.embed_fields.data, all.rich.embed_fields.count};
    m.stickers = {all.rich.stickers.data, all.rich.stickers.count};
    m.components = {all.rich.components.data, all.rich.components.count};
    m.reactions = {all.rich.reactions.data, all.rich.reactions.count};
    m.mention_users = {all.mention_users.data, all.mention_users.count};
    m.reference.message_id = 90;
    m.reference.channel_id = 70;
    m.reference.type = 1;
    m.snapshot = {utf8_literal("Original forwarded body"),
                  10,
                  0,
                  {all.rich.snapshot.attachments.data, all.rich.snapshot.attachments.count},
                  {all.rich.snapshot.embeds.data, all.rich.snapshot.embeds.count},
                  {all.rich.snapshot.embed_fields.data, all.rich.snapshot.embed_fields.count},
                  {all.rich.snapshot.stickers.data, all.rich.snapshot.stickers.count},
                  {all.rich.snapshot.components.data, all.rich.snapshot.components.count},
                  {all.rich.snapshot.mention_users.data, all.rich.snapshot.mention_users.count},
                  0,
                  gateway_message_field_content | gateway_message_field_attachments |
                      gateway_message_field_embeds | gateway_message_field_sticker_items |
                      gateway_message_field_components,
                  0,
                  0,
                  true};
    m.transient_bytes = 16 * 1024;
    bool accepted = true;
    for (u32 i = 0; i < kMessageSearchMaximumMessages && accepted; ++i) {
        m.id = 4000 + i;
        accepted = bank_accept(b, i, item);
    }
    const auto* outer = message_rich_store_find(b->rich, 4000);
    const auto* child = message_rich_store_snapshot(b->rich, 4000);
    check(
        accepted && b->store.count == 125 && b->rich.count == 125 && b->rich.snapshot_count == 125 &&
            outer && child && outer->reaction_count == 20 && outer->attachment_count == 10 &&
            outer->embed_count == 10 && outer->embed_field_count == 64 && outer->sticker_count == 3 &&
            outer->component_count == 40 && child->attachment_count == 10 &&
            child->embed_field_count == 64 && b->users.count == 201,
        "search rich bank: all125 outer and snapshot cards retain full independent descriptor capacities");
    m.id = 4000;
    check(bank_accept(b, 124, item), "search rich bank: repeated full-capacity outer and snapshot content "
                                     "shares a coherent canonical identity");
    m.content = utf8_literal("Conflicting body");
    check(!bank_accept(b, 124, item),
          "search rich bank: repeated identity with different body is rejected before overwrite");
    m.content = utf8_literal("Original full body");
    scratch->attachments[1][0].url = utf8_literal("https://example.invalid/conflicting.png");
    check(!bank_accept(b, 124, item),
          "search rich bank: repeated identity with different snapshot media is rejected before overwrite");
    clear_bytes(scratch, sizeof(*scratch));
    check(outer && child &&
              utf8_equal(message_rich_store_string(b->rich, outer->attachments[0].filename),
                         utf8_literal("original.bin")) &&
              utf8_equal(message_rich_store_string(b->rich, child->snapshot.content),
                         utf8_literal("Original forwarded body")) &&
              user_store_find(b->users, 3000),
          "search rich bank: scratch reuse cannot mutate retained text, media or named mentions");
    VirtualFree(scratch, 0, MEM_RELEASE);
    bank_release(b);
}
void private_privacy() noexcept {
    auto& state = g_app.account_ui->chat_state;
    check(
        chat_state_store_upsert_guild(
            &state, {7, utf8_literal("Fixture guild"), GuildMutationKind::create, 6})
                .ok() &&
            chat_state_store_upsert_channel(&state,
                                               {71, 7, 70, utf8_literal("Private thread"), 0, 0, 12})
                .ok() &&
            chat_state_store_set_thread_self_membership(&state, 71, 5, true),
        "search privacy: real private thread has positive current-user authority without moderator bypass");
    chat_state_store_recompute_guild_permissions(&state, 7);
    constexpr auto mixed = utf8_literal(
        R"search({"total_results":2,"messages":[[{"id":"620","channel_id":"71","guild_id":"7","author":{"id":"3","username":"Private author"},"content":"Private outer secret","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_reference":{"type":1,"message_id":"619","channel_id":"70","guild_id":"7"},"message_snapshots":[{"message":{"type":0,"content":"Private child secret","attachments":[{"id":"110","filename":"private.png","size":17,"width":320,"height":32,"url":"https://cdn.discordapp.com/attachments/70/110/private.png"}]}}]}],[{"id":"610","channel_id":"70","guild_id":"7","author":{"id":"4","username":"Public author"},"content":"Authorized public result","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_reference":{"type":1,"message_id":"609","channel_id":"71","guild_id":"7"},"message_snapshots":[{"message":{"type":0,"content":"Authorized forwarded result","attachments":[{"id":"111","filename":"public.png","size":17,"width":320,"height":32,"url":"https://cdn.discordapp.com/attachments/71/111/public.png"}]}}]}]]})search");
    check(query(utf8_literal("privacy mixed")) && application_search_submit(960, 1) &&
              response(960, 200, mixed),
          "search privacy: real mixed rich decoder and owning sink publish two independent channels");
    ApplicationSearchSourceTicket private_ticket{}, public_ticket{};
    ApplicationSearchSourceView view{};
    ApplicationSearchNavigationSource navigation_source{};
    check(application_search_source_ticket(0, &private_ticket) &&
              application_search_source_ticket(1, &public_ticket) &&
              application_search_source_resolve(private_ticket, true, &view),
          "search privacy: positive private membership permits outer-owned forwarded media");
    const auto* bank = g_app.search->bank;
    const u32 publication = bank->publication, revision = g_app.search->source.revision;
    auto* before_scene = const_cast<MessageScene*>(ui_search_scene_message_scene(g_app.search->scene));
    const auto* before_outer = before_scene ? message_scene_find_row(*before_scene, 0) : nullptr;
    const auto* before_child =
        before_outer ? message_scene_snapshot_row(*before_scene, *before_outer) : nullptr;
    const auto before_widget =
        before_child
            ? message_scene_rich_item_widget(*before_child, MessageSceneRichItemKind::attachment, 0)
            : ui_widget_invalid_handle();
    if (before_scene && before_widget.generation) {
        ui_widget_manager_capture(&before_scene->widgets, before_widget);
        ui_widget_manager_focus(&before_scene->widgets, before_widget);
        g_app.context_focus = &before_scene->widgets;
    }
    check(before_widget.generation != 0,
          "search privacy: real forwarded attachment owns the pre-revocation native press");
    UiSearchTarget old_jump{UiSearchAction::result, 71, 620};
    old_jump.source_revision = revision;
    old_jump.source_index = 0;
    GatewayEnvelope revoke_envelope{};
    revoke_envelope.event_name = {utf8_literal("THREAD_MEMBERS_UPDATE"), 0, false};
    revoke_envelope.has_event_name = true;
    revoke_envelope.data = {utf8_literal(R"({"guild_id":"7","id":"71","removed_member_ids":["5"]})"), 0,
                            JsonValueKind::object};
    u32 revoke_structurals[128]{};
    char revoke_scratch[2048]{};
    GatewayClientEvent revoke_event{};
    ConstSlice<u8> revoke_payload{};
    check(gateway_client_route_dispatch(revoke_envelope, {revoke_structurals, 128}, 32,
                                        {revoke_scratch, sizeof(revoke_scratch)}, {}, 5, &revoke_event,
                                        &revoke_payload)
                  .ok() &&
              chat_state_apply_thread_authority(revoke_event, revoke_payload, &state).ok() &&
              application_search_synchronize(),
          "search privacy: actual Gateway self-removal decode and typed authority revoke retained content "
          "before reuse");
    const auto* scene = ui_search_scene_message_scene(g_app.search->scene);
    const auto* private_row = scene ? message_scene_find_row(*scene, 0) : nullptr;
    const auto* public_row = scene ? message_scene_find_row(*scene, 1) : nullptr;
    UiSearchRow unavailable{};
    bool leaked_text = false, public_text = false;
    const auto* presentation = application_search_message_text_presentation();
    for (u32 i = 0; presentation && i < presentation->command_count; ++i) {
        leaked_text |= presentation->commands[i].message_id == 620;
        public_text |= presentation->commands[i].message_id == 610;
    }
    check(g_app.search->bank == bank && bank->publication == publication &&
              g_app.search->source.count == 2 && g_app.search->source.revision != revision &&
              resolve(g_app.search, 0, &unavailable) && unavailable.unavailable &&
              !unavailable.access_check && !unavailable.author.bytes && !unavailable.avatar_hash.bytes &&
              private_row && private_row->redacted && public_row && !public_row->redacted && scene &&
              !scene->widgets.captured.generation && !scene->widgets.focused.generation &&
              g_app.context_focus == &g_app.search->scene.widgets && !leaked_text && public_text,
          "search privacy: revoked outer and child text redact while mixed authorized "
          "publication/query/page survive");
    check(!application_search_source_resolve(private_ticket, false, &view) &&
              !application_search_source_resolve(private_ticket, true, &view) &&
              !application_search_navigation_source(private_ticket, &navigation_source) &&
              !ui_search_scene_target_current(g_app.search->scene, old_jump) &&
              application_search_source_resolve(public_ticket, true, &view),
          "search privacy: known denial revokes source and old action; authorized forward ignores original "
          "private channel gate");
    PaintStream paint{};
    MessageMediaCommand commands[32]{};
    MessageMediaCommandStream media{};
    const bool painted =
        paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 720) &&
        message_media_command_stream_initialize(&media, {commands, 32}) &&
        ui_search_scene_paint(g_app.search->scene, application_theme(), &paint, nullptr, 96, &media);
    bool private_media = false, public_media = false;
    for (u32 i = 0; i < media.count; ++i) {
        private_media |= commands[i].id == 110;
        public_media |= commands[i].id == 111;
    }
    check(painted && !private_media && public_media,
          "search privacy: actual native paint/media stream omits revoked snapshot but keeps authorized "
          "mixed snapshot");
    paint_stream_release(&paint);
    check(chat_state_store_set_thread_self_membership(&state, 71, 5, false, false) &&
              application_search_synchronize() && resolve(g_app.search, 0, &unavailable) &&
              unavailable.unavailable && unavailable.access_check &&
              !application_search_source_resolve(private_ticket, true, &view),
          "search privacy: unknown membership offers metadata Check access without lending text or media "
          "authority");
    check(chat_state_store_set_thread_self_membership(&state, 71, 5, true) &&
              application_search_synchronize() &&
              application_search_source_resolve(private_ticket, true, &view),
          "search privacy: renewed positive exact membership restores immutable same-publication content");

    constexpr auto unknown = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"630","channel_id":"99","guild_id":"7","author":{"id":"3","username":"Unknown channel author"},"content":"Unknown channel content","timestamp":"2026-01-01T00:00:00Z","hit":true,"attachments":[{"id":"112","filename":"unknown.png","size":17,"width":320,"height":32,"url":"https://cdn.discordapp.com/attachments/99/112/unknown.png"}]}]]})");
    check(query(utf8_literal("unknown privacy")) && application_search_submit(961, 1) &&
              response(961, 200, unknown) && application_search_source_ticket(0, &private_ticket) &&
              resolve(g_app.search, 0, &unavailable) && unavailable.unavailable &&
              unavailable.access_check && !unavailable.author.bytes &&
              !application_search_source_resolve(private_ticket, false, &view) &&
              application_search_navigation_source(private_ticket, &navigation_source),
          "search privacy: unknown guild channel lends metadata hydration authority but no text or media "
          "source");
    scene = ui_search_scene_message_scene(g_app.search->scene);
    private_row = scene ? message_scene_find_row(*scene, 0) : nullptr;
    check(private_row && private_row->redacted,
          "search privacy: initially unknown rich row is redacted before native projection");
    paint = {};
    media = {};
    const bool unknown_painted =
        paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 720) &&
        message_media_command_stream_initialize(&media, {commands, 32}) &&
        ui_search_scene_paint(g_app.search->scene, application_theme(), &paint, nullptr, 96, &media);
    check(unknown_painted && !media.count,
          "search privacy: initial unknown-channel attachment creates no native media command");
    paint_stream_release(&paint);
}
void rich_publication() noexcept {
    g_app.account_ui->chat_state.self_user_id = 5;
    chat_state_store_set_self_roles(&g_app.account_ui->chat_state, 7, 5, 5, {});
    chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state, 7);
    check(chat_state_store_select_channel(&g_app.account_ui->chat_state, 70).ok(),
          "search rich publication: native channel selection restores guild scope after permission "
          "recomputation");
    g_app.account_ui->active_channel_id = 70;
    const auto* readable_channel = chat_state_store_find_channel(g_app.account_ui->chat_state, 70);
    check(readable_channel && readable_channel->permissions_resolved &&
              (readable_channel->actions &
               (channel_action_view | channel_action_read_history)) ==
                  (channel_action_view | channel_action_read_history),
          "search rich publication: real role and overwrite model establishes source read permission "
          "before actions");
    constexpr auto body = utf8_literal(
        R"search({"total_results":2,"messages":[[{"id":"600","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Rich original"},"content":"Outer **bold** [original](https://example.invalid/source)","timestamp":"2026-01-01T00:00:00Z","hit":true,"attachments":[{"id":"100","filename":"original.bin","flags":16,"size":17,"url":"https://cdn.discordapp.com/attachments/70/100/original.bin"}],"message_reference":{"type":1,"message_id":"500","channel_id":"70","guild_id":"7"},"message_snapshots":[{"message":{"type":0,"content":"Forwarded original","attachments":[{"id":"101","filename":"original.png","size":17,"content_type":"image/png","width":320,"height":200,"url":"https://cdn.discordapp.com/attachments/70/101/original.png"}]}}]}],[{"id":"600","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Rich original"},"content":"Outer **bold** [original](https://example.invalid/source)","timestamp":"2026-01-01T00:00:00Z","hit":true,"attachments":[{"id":"100","filename":"original.bin","flags":16,"size":17,"url":"https://cdn.discordapp.com/attachments/70/100/original.bin"}],"message_reference":{"type":1,"message_id":"500","channel_id":"70","guild_id":"7"},"message_snapshots":[{"message":{"type":0,"content":"Forwarded original","attachments":[{"id":"101","filename":"original.png","size":17,"content_type":"image/png","width":320,"height":200,"url":"https://cdn.discordapp.com/attachments/70/101/original.png"}]}}]}]]})search");
    const bool queued = query(utf8_literal("rich original"));
    const bool submitted = queued && application_search_submit(950, 1);
    const bool received = submitted && response(950, 200, body);
    check(queued && submitted && received,
          "search rich publication: actual rich decoder response publishes repeated coherent context");
    ApplicationSearchSourceTicket ticket{};
    ApplicationSearchSourceView outer{}, child{};
    check(application_search_source_ticket(0, &ticket) &&
              application_search_source_resolve(ticket, false, &outer) &&
              application_search_source_resolve(ticket, true, &child) &&
              outer.message->revision == ticket.publication_revision && outer.rich &&
              outer.rich->message_revision == ticket.publication_revision &&
              child.rich->message_revision == ticket.publication_revision &&
              child.source_channel_id == 70 && outer.source_channel_id == 0 &&
              g_app.search->bank->store.count == 1 && g_app.search->bank->list.count == 2 &&
              g_app.search->source.rich_order.count == 2 &&
              utf8_equal(message_rich_store_string(*child.rich_store, child.rich->snapshot.content),
                         utf8_literal("Forwarded original")),
          "search rich publication: ticket owns base/outer/child revisions and repeated display order "
          "without timeline mutation");
    auto* text_owner = application_search_message_text_presentation();
    const u32 builds = text_owner ? text_owner->builds : 0;
    const auto bank_bytes = bank_committed(g_app.search->bank);
    bool unchanged = text_owner != nullptr;
    for (u32 i = 0; i < 64 && unchanged; ++i)
        unchanged = application_search_synchronize() &&
                    application_search_message_text_presentation() == text_owner &&
                    text_owner->builds == builds && bank_committed(g_app.search->bank) == bank_bytes;
    check(unchanged, "search rich publication:64 unchanged passes reuse owning text projection and "
                     "committed bank backing without reparsing");
    auto* native = ui_search_scene_message_scene(&g_app.search->scene);
    const auto* row = native ? message_scene_find_row(*native, 0) : nullptr;
    const auto file = row ? message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, 0)
                          : ui_widget_invalid_handle();
    focus(*g_app.search, false);
    const auto saves = search_saves;
    bool saved = native && file.generation && ui_widget_manager_focus(&native->widgets, file);
    if (saved) {
        g_app.context_focus = &native->widgets;
        saved = modifiers(true) && key('S') && modifiers();
    }
    check(saved && search_saves == saves + 1 && saved_search_ticket.message_id == ticket.message_id &&
              saved_search_ticket.publication_revision == ticket.publication_revision &&
              saved_search_attachment == 100 && !saved_search_snapshot,
          "search rich publication: native focused file CtrlS submits the exact current bank ticket and "
          "outer attachment");
    auto guarded = native ? ui_search_scene_rich_widget_target(g_app.search->scene, file) : UiSearchTarget{};
    guarded.save = true;
    const auto saves_before_policy = search_saves;
    const auto reveals_before_policy = native ? native->media_reveals.count : 0;
    media_policy = {MessageExplicitFilter::block, true};
    const bool blocked = native && activate(*g_app.search, guarded) && activate(*g_app.search, guarded);
    check(blocked && search_saves == saves_before_policy && native->media_reveals.count == reveals_before_policy,
          "search media policy: blocked explicit file cannot Save or reveal through controller activation");
    media_policy = {MessageExplicitFilter::blur, true};
    const auto reveal_key = message_media_reveal_key(ticket.message_id, MessageMediaSource::attachment, 0);
    const bool revealed = native && activate(*g_app.search, guarded);
    check(revealed && search_saves == saves_before_policy &&
              message_media_revealed(native->media_reveals, reveal_key) &&
              activate(*g_app.search, guarded) && search_saves == saves_before_policy + 1,
          "search media policy: first Save gesture only reveals and second Save uses the exact attachment");
    media_policy = {MessageExplicitFilter::blur, false};
    const auto confirms_before_policy = content_confirmations;
    const auto gated_reveals = native ? native->media_reveals.revision : 0;
    check(native && activate(*g_app.search, guarded) && content_confirmations == confirms_before_policy + 1 &&
              search_saves == saves_before_policy + 1 && native->media_reveals.revision == gated_reveals,
          "search media policy: eligibility confirmation prevents Save without changing prior reveal state");
    media_policy = {MessageExplicitFilter::show, true};
    if (native) {
        UiWidgetHandle last{};
        for (u32 i = 0; i < native->widgets.count; ++i) {
            const auto h = UiWidgetHandle{static_cast<u16>(i), native->widgets.generation};
            if (ui_search_scene_rich_widget_target(g_app.search->scene, h).action ==
                UiSearchAction::rich_item)
                last = h;
        }
        const bool focused = last.generation && ui_widget_manager_focus(&native->widgets, last);
        if (focused)
            g_app.context_focus = &native->widgets;
        check(focused && key(VK_TAB) && g_app.context_focus == &g_app.search->scene.widgets &&
                  g_app.search->scene.widgets.focused.generation &&
                  g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                      UiSearchAction::result,
              "search rich publication: Tab past native rich children reveals and focuses the explicit "
              "Jump chrome");
    }
    downloads = true;
    check(application_search_synchronize() && application_search_source_resolve(ticket, true, &child) &&
              !application_search_edit_focused(),
          "search rich publication: download overlay retains accepted ticket while retiring Search input "
          "focus");
    downloads = false;
    const auto* saved_instance = g_app.account.instance;
    g_app.account.instance = reinterpret_cast<void*>(1);
    check(!application_search_source_resolve(ticket, false, &outer),
          "search rich publication: account instance identity prevents actor ABA");
    g_app.account.instance = const_cast<void*>(saved_instance);
    auto* channel = const_cast<ChannelRecord*>(
        chat_state_store_find_channel(g_app.account_ui->chat_state, 70));
    const auto actions = channel->actions;
    channel->actions &= ~channel_action_read_history;
    check(
        !application_search_source_resolve(ticket, false, &outer) &&
            application_search_source_notice(ticket, utf8_literal("Permission denied.")),
        "search rich publication: current read permission fences media while scoped errors remain visible");
    channel->actions = actions;
    const auto generation = channel->generation;
    ++channel->generation;
    check(!application_search_source_resolve(ticket, false, &outer),
          "search rich publication: recreated channel generation revokes content action");
    channel->generation = generation;
    const auto old_publication = ticket.publication_revision;
    check(application_search_close_panel() && !application_search_source_resolve(ticket, false, &outer) &&
              open(true) && !application_search_source_ticket(0, &ticket),
          "search rich publication: explicit close and reopen cannot reuse old action authority");
    check(query(utf8_literal("rich original")) && application_search_submit(951, 1) &&
              response(951, 200, body) && application_search_source_ticket(0, &ticket) &&
              ticket.publication_revision > old_publication,
          "search rich publication: reopening publishes a fresh globally monotonic source identity");
    auto* accepted = g_app.search->bank;
    constexpr auto conflict = utf8_literal(
        R"({"total_results":2,"messages":[[{"id":"601","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Original"},"content":"Original","hit":true}],[{"id":"601","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Original"},"content":"Conflicting","hit":true}]]})");
    check(query(utf8_literal("conflict")) && !application_search_source_resolve(ticket, false, &outer) &&
              application_search_submit(952, 1) && response(952, 200, conflict) &&
              g_app.search->bank == accepted &&
              g_app.search->controller.failure == MessageSearchFailure::decode,
          "search rich publication: new query retires prior actions and conflicting repeated content "
          "discards candidate only");
}

void poll_publication() noexcept {
    auto& ui = *g_app.account_ui;
    auto& store = ui.message_poll_store;
    constexpr auto body = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"700","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Poll fixture author"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"poll":{"question":{"text":"Original poll question"},"answers":[{"answer_id":1,"poll_media":{"text":"First original answer"}},{"answer_id":5,"poll_media":{"text":"Second original answer","emoji":{"id":"777","name":"original","animated":true}}}],"expiry":"2099-01-01T00:00:00Z","allow_multiselect":true,"layout_type":1,"results":{"is_finalized":false,"answer_counts":[{"id":1,"count":1,"me_voted":false},{"id":5,"count":3,"me_voted":false}]}}}]]})");
    char borrowed[body.bytes];
    copy_bytes(borrowed, body.data, body.bytes);
    check(query(utf8_literal("poll publication")) && application_search_submit(970, 1) &&
              response(970, 200, {borrowed, sizeof(borrowed)}) &&
              g_app.search->controller.state == MessageSearchState::ready,
          "search polls: shipping submit/response accepts a full received poll page");
    auto* bank = g_app.search->bank;
    const auto* poll = message_poll_store_find(store, 700);
    check(bank && poll && bank->poll_fields[0] == 2 && store.count == 1 && !bank->staged_polls.owned &&
              poll->answer_count == 2 && poll->allow_multiselect && poll->answers[0].count == 1 &&
              poll->answers[1].id == 5 && poll->answers[1].emoji_id == 777 &&
              poll->answers[1].emoji_animated,
          "search polls: candidate presence survives cleared decoder summaries and shared publication owns "
          "complete answers/results");
    if (!bank || !poll)
        return;
    for (auto& byte : borrowed)
        byte = '!';
    const auto found = message_store_find(bank->store, 700);
    const auto* message = found.ok() ? message_store_resolve(bank->store, found.handle) : nullptr;
    check(message && (message->presentation_flags & message_presentation_has_poll) &&
              utf8_equal(message_poll_store_string(store, poll->question),
                         utf8_literal("Original poll question")) &&
              utf8_equal(message_poll_store_string(store, poll->answers[0].text),
                         utf8_literal("First original answer")) &&
              utf8_equal(message_poll_store_string(store, poll->answers[1].emoji_name),
                         utf8_literal("original")),
          "search polls: presence and copied strings survive input overwrite plus decoder/staging release");
    auto* scene = ui_search_scene_message_scene(&g_app.search->scene);
    const auto* row = scene ? message_scene_find_row(*scene, 0) : nullptr;
    UiPollAnswer projected[kUiPollMaximumAnswers]{};
    UiPollProjection projection{};
    check(scene && row && row->poll.generation && row->poll_height > 0 && scene->polls == &bank->polls &&
              scene->polls->store == &store && scene->polls->measure == &ui.text_measure_cache &&
              message_scene_poll_projection(*scene, *row, application_theme(),
                                            {projected, kUiPollMaximumAnswers}, &projection) &&
              poll_projection_calls && projected_poll_id == 700 &&
              projected_poll_publication == bank->publication,
          "search polls: actual card geometry uses lifetime-owned shared binding and the exact Search "
          "publication callback");

    const u32 accepted_revision = store.revision, accepted_content = store.content_revision;
    const auto* accepted_records = store.records;
    constexpr auto late_invalid = utf8_literal(
        R"({"total_results":2,"messages":[[{"id":"701","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Candidate"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"poll":{"question":{"text":"Provisional question"},"answers":[{"answer_id":1,"poll_media":{"text":"Candidate answer"}}],"expiry":null,"allow_multiselect":false,"layout_type":1}}],[{"id":"702","channel_id":"90","guild_id":"8","author":{"id":"3","username":"Foreign"},"content":"Late invalid scope","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(query(utf8_literal("poll invalid tail")) && application_search_submit(971, 1) &&
              response(971, 200, late_invalid) &&
              g_app.search->controller.failure == MessageSearchFailure::decode &&
              g_app.search->bank == bank && store.records == accepted_records &&
              store.revision == accepted_revision && store.content_revision == accepted_content &&
              store.count == 1 && !message_poll_store_find(store, 701) && g_app.search->candidate &&
              g_app.search->candidate->store.count == 1 && g_app.search->candidate->poll_fields[0] == 2 &&
              g_app.search->candidate->messages[0].message.id == 701,
          "search polls: late invalid message rejects the whole staged page without publishing its earlier "
          "valid poll");

    check(query(utf8_literal("poll vote race")) && application_search_submit(972, 1) &&
              g_app.search->submitted_poll_count == 1 && !g_app.search->submitted_poll_gateway_serial,
          "search polls: HTTP admission captures existing poll content and the submission-time Gateway "
          "serial");
    const auto live_vote =
        message_poll_store_vote(&store, {70, 700, 5, 7, 1, true, true}, 5, ++ui.poll_gateway_serial);
    check(live_vote.ok() && live_vote.changed && response(972, 200, body) &&
              g_app.search->controller.state == MessageSearchState::ready,
          "search polls: newer authoritative own vote arrives before an older HTTP results receipt");
    poll = message_poll_store_find(store, 700);
    check(poll && poll->answers[0].count == 2 && poll->answers[0].me_voted &&
              poll->last_gateway_serial == 1 && !g_app.search->submitted_poll_tickets,
          "search polls: stale HTTP results preserve newer Gateway counts/own vote and release captured "
          "tickets");

    check(query(utf8_literal("poll content race")) && application_search_submit(973, 1),
          "search polls: content race has a real submitted Search ticket");
    GatewayMessagePoll fresh_question{};
    fresh_question.present = true;
    fresh_question.fields = gateway_message_poll_field_question;
    fresh_question.question = utf8_literal("Newer Gateway question");
    const auto updated =
        message_poll_store_apply(&store, 700, 70, fresh_question, ++ui.poll_gateway_serial);
    const u32 fresh_revision = updated.record ? updated.record->content_revision : 0;
    check(updated.ok() && updated.changed && response(973, 200, body) &&
              g_app.search->controller.state == MessageSearchState::ready,
          "search polls: authoritative content changes between submission and HTTP completion");
    poll = message_poll_store_find(store, 700);
    check(poll && poll->content_revision == fresh_revision &&
              utf8_equal(message_poll_store_string(store, poll->question), fresh_question.question) &&
              poll->answers[0].count == 2,
          "search polls: submission-time content ticket prevents old question/answers/results overwriting "
          "a newer poll");

    constexpr auto null_body = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"700","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Poll fixture author"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"poll":null}]]})");
    check(query(utf8_literal("poll null race")) && application_search_submit(974, 1),
          "search polls: explicit-null race captures resident content before the live vote");
    const auto next_vote =
        message_poll_store_vote(&store, {70, 700, 6, 7, 5, true, true}, 5, ++ui.poll_gateway_serial);
    check(next_vote.ok() && next_vote.changed && response(974, 200, null_body) &&
              g_app.search->controller.state == MessageSearchState::ready,
          "search polls: stale explicit-null receipt is still a decoded and accepted Search page");
    poll = message_poll_store_find(store, 700);
    check(poll && poll->answers[1].count == 4 && poll->content_revision == fresh_revision &&
              g_app.search->bank && g_app.search->bank->poll_fields[0] == 1,
          "search polls: explicit-null staging survives summary clearing but cannot delete a newer Gateway "
          "poll");

    constexpr auto omitted_body = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"700","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Poll fixture author"},"content":"Plain Search summary","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    const u32 before_omission = store.revision;
    check(query(utf8_literal("poll omitted")) && application_search_submit(975, 1) &&
              response(975, 200, omitted_body) &&
              g_app.search->controller.state == MessageSearchState::ready &&
              message_poll_store_find(store, 700) && store.revision == before_omission &&
              g_app.search->bank && !g_app.search->bank->poll_fields[0],
          "search polls: omitted field publishes no poll mutation and preserves the shared authoritative "
          "record");
    check(
        query(utf8_literal("poll current null")) && application_search_submit(976, 1) &&
            response(976, 200, null_body) && g_app.search->controller.state == MessageSearchState::ready &&
            !message_poll_store_find(store, 700) && !store.count && g_app.search->bank &&
            g_app.search->bank->poll_fields[0] == 1,
        "search polls: current explicit-null with matching content/serial removes exactly the shared poll");
    constexpr auto adjacent = utf8_literal(
        R"({"total_results":2,"messages":[[{"id":"710","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Poll author"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"poll":{"question":{"text":"First independent question"},"answers":[{"answer_id":1,"poll_media":{"text":"First independent answer"}}],"expiry":null,"allow_multiselect":false,"layout_type":1}}],[{"id":"711","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Poll author"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"poll":{"question":{"text":"Second independent question"},"answers":[{"answer_id":5,"poll_media":{"text":"Second independent answer"}}],"expiry":null,"allow_multiselect":true,"layout_type":1}}]]})");
    check(
        query(utf8_literal("adjacent polls")) && application_search_submit(977, 1) &&
            response(977, 200, adjacent) && g_app.search->controller.state == MessageSearchState::ready,
        "search polls: one accepted page decodes two polls through the same reusable answer/count scratch");
    const auto* first = message_poll_store_find(store, 710);
    const auto* second = message_poll_store_find(store, 711);
    check(first && second && store.count == 2 &&
              utf8_equal(message_poll_store_string(store, first->question),
                         utf8_literal("First independent question")) &&
              utf8_equal(message_poll_store_string(store, first->answers[0].text),
                         utf8_literal("First independent answer")) &&
              utf8_equal(message_poll_store_string(store, second->question),
                         utf8_literal("Second independent question")) &&
              utf8_equal(message_poll_store_string(store, second->answers[0].text),
                         utf8_literal("Second independent answer")) &&
              first->answers[0].id == 1 && second->answers[0].id == 5 && !first->allow_multiselect &&
              second->allow_multiselect,
          "search polls: scratch reuse retains both independent questions/answers after decoder and "
          "candidate owner teardown");
    check(first && second && !first->results_known && !second->results_known &&
              !first->answers[0].count_known && !first->answers[0].me_known &&
              !second->answers[0].count_known && !second->answers[0].me_known,
          "search polls: shipping Search publication keeps omitted results and own membership unknown");
}

bool choice_draft(Utf8View text, u32 caret = ~u32{0}) noexcept {
    if (!open(true))
        return false;
    const u32 position = caret == ~u32{0} ? text.bytes : caret;
    return message_composer_restore(&g_app.search->edit, 0, text, position, position) &&
           application_search_edit_changed(true);
}
void filter_choices_runtime() noexcept {
    auto& ui = *g_app.account_ui;
    const auto members_before = ui.guild_member_store;
    check(!g_app.search->choices &&
              guild_member_store_initialize(&ui.guild_member_store, {choice_member_records, 4},
                                                    {choice_member_order, 4}) &&
              user_store_upsert(&ui.user_store, 5, utf8_literal("Fixture self"),
                                        utf8_literal("self"))
                  .ok() &&
              user_store_upsert(&ui.user_store, 3, utf8_literal("ŽIVA"), utf8_literal("ziva"))
                  .ok() &&
              user_store_upsert(&ui.user_store, 4, utf8_literal("Unrelated cached user"),
                                        utf8_literal("unrelated"))
                  .ok() &&
              guild_member_store_upsert(&ui.guild_member_store, ui.chat_state, 7, 3,
                                                utf8_literal("ŽIVA member"), {}, false,
                                                GuildMemberStatus::offline)
                  .ok(),
          "search choices: dormant owner has no chooser and real bounded identity stores seed cached "
          "membership");
    const auto submissions_before = account.submissions;
    check(choice_draft(utf8_literal("hello world"), 5) && click(UiSearchAction::filter_from) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hello from: world")) &&
              g_app.search->edit.caret == 11 && g_app.search->content.filter_choices && modifiers(true) &&
              key('Z') && modifiers() &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hello world")) &&
              g_app.search->edit.anchor == 5 && g_app.search->edit.caret == 5,
          "search choices: native prefix click inserts at the caret and one undo restores text and "
          "selection");
    check(choice_draft(utf8_literal("hello world"), 6) && click(UiSearchAction::filter_from) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hello from: world")) &&
              g_app.search->edit.caret == 11 && choice_draft(utf8_literal("hello"), 3) &&
              click(UiSearchAction::filter_from) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hel from: lo")) &&
              g_app.search->edit.caret == 9,
          "search choices: prefix insertion delimits a right-hand word without merging or moving past the "
          "colon");
    check(choice_draft(utf8_literal("hello brave world"), 6) &&
              message_composer_set_selection(&g_app.search->edit, 6, 11) ==
                  MessageComposerResult::changed &&
              application_search_edit_changed(false) && click(UiSearchAction::filter_in) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hello in: world")) &&
              modifiers(true) && key('Z') && modifiers() &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("hello brave world")) &&
              g_app.search->edit.anchor == 6 && g_app.search->edit.caret == 11,
          "search choices: prefix replaces the real selection once and undo restores its direction and "
          "bounds");
    check(
        choice_draft(utf8_literal("from:")) && g_app.search->choices &&
            g_app.search->content.filter_choices && g_app.search->choices->source.rows.count == 2 &&
            g_app.search->choices->rows[0].id == 5 && g_app.search->choices->rows[1].id == 3,
        "search choices: current self precedes known guild member and unrelated cached users are omitted");
    // A typed name asks the gateway member search the composer uses, matches
    // the member by profile name as well as nickname, and carries avatars.
    const bool avatar_set =
        user_store_set_avatar(&ui.user_store, 3, utf8_literal("0123456789abcdef0123456789abcdef"));
    const u32 queries_before = member_queries;
    check(avatar_set && choice_draft(utf8_literal("from:@zi")) && g_app.search->choices &&
              member_queries > queries_before && member_query_guild == 7 &&
              utf8_equal({member_query, member_query_bytes}, utf8_literal("zi")) &&
              g_app.search->choices->source.rows.count == 1 && g_app.search->choices->rows[0].id == 3 &&
              utf8_equal(g_app.search->choices->rows[0].avatar_hash,
                         utf8_literal("0123456789abcdef0123456789abcdef")),
          "search choices: people suggestions query gateway members and carry the member's avatar");
    const u32 queries_after = member_queries;
    check(choice_draft(utf8_literal("from:")) && g_app.search->choices && member_queries == queries_after &&
              g_app.search->choices->source.rows.count == 2,
          "search choices: an empty people prefix lists cached people without a gateway query");
    const u32 withdrawals_before = member_query_withdrawals;
    check(choice_draft(utf8_literal("from:zi")) && member_query_active &&
              choice_draft(utf8_literal("has:image")) && !member_query_active &&
              member_query_withdrawals == withdrawals_before + 1,
          "search choices: replacing a people query with a fixed filter withdraws only Search interest");
    const u32 fixed_queries = member_queries, fixed_withdrawals = member_query_withdrawals;
    bool fixed_stable = true;
    for (u32 i = 0; i < 64; ++i)
        fixed_stable &= application_search_synchronize();
    check(fixed_stable && member_queries == fixed_queries && member_query_withdrawals == fixed_withdrawals,
          "search choices: unchanged non-people suggestions never cancel or reschedule member work");
    check(choice_draft(utf8_literal("from:zi")) && member_query_active,
          "search choices: composition cancellation starts from an owned people query");
    composing = true;
    check(application_search_synchronize() && !member_query_active &&
              member_query_withdrawals == fixed_withdrawals + 1,
          "search choices: beginning composition withdraws the retained member request once");
    const u32 composing_queries = member_queries, composing_withdrawals = member_query_withdrawals;
    bool composing_stable = true;
    for (u32 i = 0; i < 64; ++i)
        composing_stable &= application_search_synchronize();
    composing = false;
    check(composing_stable && member_queries == composing_queries &&
              member_query_withdrawals == composing_withdrawals && application_search_synchronize() &&
              member_query_active && member_queries == composing_queries + 1,
          "search choices: composition is quiet while held and returning to the same query republishes "
          "interest");
    check(key(VK_ESCAPE) && !member_query_active && member_query_withdrawals == composing_withdrawals + 1,
          "search choices: Escape withdraws its current query with the dismissed suggestions");
    check(choice_draft(utf8_literal("from:zi")) && member_query_active,
          "search choices: delayed withdrawal starts from a real Search query");
    composer_query_owned = true;
    const u32 delayed_withdrawals = member_query_withdrawals;
    check(application_search_close_panel() && member_query_active &&
              member_query_withdrawals == delayed_withdrawals + 1 && !g_app.search->member_query_active,
          "search choices: delayed Search close cannot cancel a newer composer-owned query");
    composer_query_owned = member_query_active = false;
    const u32 closed_withdrawals = member_query_withdrawals;
    check(application_search_close_panel() && member_query_withdrawals == closed_withdrawals &&
              choice_draft(utf8_literal("from:")) && member_query_withdrawals == closed_withdrawals,
          "search choices: repeated close and cached-only reopen have no member cancellation side effects");
    auto* bank = g_app.search->choices;
    if (!bank)
        return;
    const auto scans = bank->scans, copies = bank->copies, rebuilds = bank->rebuilds;
    const auto revision = bank->source.revision, scene_revision = g_app.search->scene.revision;
    const auto layout_revision = g_app.search->scene.widgets.layout.revision;
    bool stable = true;
    for (u32 i = 0; i < 64; ++i)
        stable &= application_search_synchronize();
    const bool warm_unchanged = stable && bank == g_app.search->choices && bank->scans == scans &&
                                bank->copies == copies && bank->rebuilds == rebuilds &&
                                bank->source.revision == revision &&
                                g_app.search->scene.revision == scene_revision &&
                                g_app.search->scene.widgets.layout.revision == layout_revision &&
                                account.submissions == submissions_before && committed(bank) <= 4096;
    check(warm_unchanged,
          "search choices: 64 retained synchronizations perform no inventory scans, copies, rebuilds, "
          "layouts, allocation or requests");
    const auto caret = g_app.search->edit.caret;
    check(key(VK_DOWN) && bank->source.selected == 1 && g_app.search->edit.caret == caret &&
              g_app.search->focused && bank->scans == scans && bank->copies == copies,
          "search choices: Down highlights without moving native editor selection or rebuilding the bank");
    check(modifiers(false, true) && key(VK_DOWN) && bank->source.selected == 1 && modifiers(true) &&
              key(VK_UP) && bank->source.selected == 1 && modifiers(false, false, true) && key(VK_DOWN) &&
              bank->source.selected == 1 && modifiers(),
          "search choices: Shift, Control and Alt arrow chords preserve ordinary editor routing");
    check(
        user_store_upsert(&ui.user_store, 3, utf8_literal("ŽIVA renamed"), utf8_literal("ziva"))
                .ok() &&
            application_search_synchronize() && bank->source.revision != revision &&
            bank->source.selected < bank->source.rows.count && bank->rows[bank->source.selected].id == 3 &&
            g_app.search->edit.caret == caret,
        "search choices: roster publication preserves a still-present highlighted identity and edit "
        "selection");
    check(key(VK_RETURN) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:3 ")) &&
              !g_app.search->suggestions && g_app.search->focused &&
              account.submissions == submissions_before && !application_search_pending(1),
          "search choices: Enter inserts one canonical ID plus separator without submitting or leaving TSF "
          "focus");
    check(modifiers(true) && key('Z') && modifiers() &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:")) &&
              modifiers(true) && key('Y') && modifiers() &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:3 ")),
          "search choices: completion is one native undo and redo transaction");
    check(choice_draft(utf8_literal("from:\"ži\" in:"), utf8_literal("from:\"ži\"").bytes) &&
              g_app.search->choices->source.rows.count == 1 && key(VK_TAB) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:3 in:")),
          "search choices: Unicode matching replaces the whole quoted value and preserves an unfinished "
          "unrelated filter");
    check(choice_draft(utf8_literal("from:\"ži\"")),
          "search choices: same-token selection reuse captures a real quoted matching source");
    bank = g_app.search->choices;
    const auto reused_scans = bank->scans, reused_copies = bank->copies, reused_rebuilds = bank->rebuilds;
    UiSearchTarget selection_ticket{UiSearchAction::filter_value, 0, 0};
    selection_ticket.source_revision = bank->source.revision;
    selection_ticket.source_index = 0;
    const auto quoted_end = g_app.search->edit.caret;
    check(message_composer_set_selection(&g_app.search->edit, 6, quoted_end) ==
                  MessageComposerResult::changed &&
              !choices_current(g_app.search) && !activate(*g_app.search, selection_ticket) &&
              application_search_edit_changed(false) && bank == g_app.search->choices &&
              bank->scans == reused_scans && bank->copies == reused_copies &&
              bank->rebuilds == reused_rebuilds &&
              bank->source.revision != selection_ticket.source_revision && bank->source.rows.count == 1 &&
              bank->rows[0].id == 3 && !activate(*g_app.search, selection_ticket),
          "search choices: anchor-only movement republishes exact authority without scanning or copying "
          "matching rows");
    selection_ticket.source_revision = bank->source.revision;
    check(message_composer_set_selection(&g_app.search->edit, 6, quoted_end - 1) ==
                  MessageComposerResult::changed &&
              !activate(*g_app.search, selection_ticket) && application_search_edit_changed(false) &&
              bank->scans == reused_scans && bank->copies == reused_copies &&
              bank->rebuilds == reused_rebuilds &&
              bank->source.revision != selection_ticket.source_revision && bank->rows[0].id == 3 &&
              !activate(*g_app.search, selection_ticket),
          "search choices: crossing a closing quote with identical decoded prefix retains matching data "
          "and rejects old targets");
    check(choice_draft(utf8_literal("has:for")) && key(VK_RETURN) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("has:snapshot ")) &&
              choice_draft(utf8_literal("pinned:f")) && key(VK_TAB) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("pinned:false ")),
          "search choices: friendly Forward and pinned choices emit canonical fixed values");
    check(choice_draft(utf8_literal("from:1 from:2 from:3 from:4 from:")) &&
              g_app.search->choices->source.rows.count == 1 && g_app.search->choices->rows[0].id == 3 &&
              key(VK_RETURN) &&
              utf8_equal(message_composer_text(g_app.search->edit),
                         utf8_literal("from:1 from:2 from:3 from:4 from:3 ")),
          "search choices: four distinct IDs reject a fifth while still allowing duplicate identity "
          "completion");
    check(choice_draft(utf8_literal("mentions: from:")) && key(VK_RETURN) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("mentions: from:@me ")),
          "search choices: another unfinished filter does not block current completion");
    check(choice_draft(utf8_literal("from:987654321")) && g_app.search->choices->source.rows.count == 0 &&
              g_app.search->choices->source.selected == kUiSearchFilterNoSelection && key(VK_RETURN) &&
              application_search_pending(1) && g_app.search->controller.query.author_count == 1 &&
              g_app.search->controller.query.author_ids[0] == 987654321 &&
              account.submissions == submissions_before && application_search_close_panel(),
          "search choices: manual Enter submits a valid uncached author ID through the full query parser");
    check(choice_draft(utf8_literal("mentions:987654322")) &&
              g_app.search->choices->source.rows.count == 0 && key(VK_RETURN) &&
              application_search_pending(1) && g_app.search->controller.query.mention_count == 1 &&
              g_app.search->controller.query.mention_ids[0] == 987654322 &&
              account.submissions == submissions_before && application_search_close_panel(),
          "search choices: manual Enter submits a valid uncached mentioned-user ID without a cached row");
    check(choice_draft(utf8_literal("from:unresolvedfixtureperson")) &&
              g_app.search->choices->source.rows.count == 0 && key(VK_RETURN) &&
              !application_search_pending(1) && g_app.search->validation.bytes &&
              account.submissions == submissions_before && application_search_close_panel(),
          "search choices: empty-match Enter retains full parser validation for unresolved author labels");
    check(choice_draft(utf8_literal("from:1 from:2 from:3 from:4 from:987654321")) &&
              g_app.search->choices->source.rows.count == 0 && key(VK_RETURN) &&
              !application_search_pending(1) && g_app.search->validation.bytes &&
              account.submissions == submissions_before && application_search_close_panel(),
          "search choices: manual fifth distinct ID is rejected before any search request");
    check(choice_draft(utf8_literal("mentions:987654322")) &&
              g_app.search->choices->source.rows.count == 0 && key(VK_TAB) && !g_app.search->suggestions &&
              !g_app.search->focused &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("mentions:987654322")) &&
              !application_search_pending(1) && account.submissions == submissions_before &&
              application_search_close_panel(),
          "search choices: empty-match Tab dismisses suggestions and follows normal focus without editing "
          "or submitting");
    u64 resolved = 0;
    check(resolve_user(nullptr, utf8_literal("@me"), &resolved) && resolved == 5,
          "search choices: leading at-sign normalization preserves the reserved current user identity");
    check(
        relationship_store_initialize(&choice_relationships, {choice_relationship_records, 1}) &&
            relationship_store_upsert(&choice_relationships, {3, RelationshipType::blocked})
                .ok() &&
            choice_draft(utf8_literal("from:")) && g_app.search->choices->source.rows.count == 1 &&
            g_app.search->choices->rows[0].id == 5,
        "search choices: cached blocked relationships withdraw suggested identities");
    check(relationship_store_remove(&choice_relationships, 3).ok() &&
              application_search_synchronize() && g_app.search->choices->source.rows.count == 2,
          "search choices: relationship publication restores eligible cached membership without HTTP");
    check(choice_draft(utf8_literal("in:")) && g_app.search->choices->source.rows.count == 1 &&
              g_app.search->choices->rows[0].id == 70,
          "search choices: current readable channel leads and foreign guild/private channels are omitted");
    UiSearchTarget old{UiSearchAction::filter_value, 0, 0};
    old.source_revision = g_app.search->choices->source.revision;
    old.source_index = 0;
    message_composer_set_selection(&g_app.search->edit, 0, 0);
    check(
        !choices_current(g_app.search) && !activate(*g_app.search, old) &&
            application_search_edit_changed(false) &&
            utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("in:")),
        "search choices: exact anchor/caret fence rejects a retained value action before fresh projection");
    check(choice_draft(utf8_literal("in:")) && !activate(*g_app.search, old),
          "search choices: new document publication cannot retarget a retained row to the same channel ID");
    composing = true;
    check(application_search_synchronize() && !g_app.search->content.filter_choices &&
              !g_app.search->content.suggestions && key(VK_RETURN) && key(VK_TAB) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("in:")) &&
              !application_search_pending(1),
          "search choices: injected active-composition boundary withdraws choices and consumes "
          "submit/accept keys");
    composing = false;
    check(application_search_synchronize() && g_app.search->content.filter_choices,
          "search choices: ending composition restores current suggestions without changing the query");
    UiRect bounds{}, clip{};
    const bool geometry = ui_search_scene_filter_choice_geometry(g_app.search->scene, 0, &bounds, &clip);
    const auto before_pointer = g_app.search->edit.caret;
    check(geometry && ui_rect_has_area(ui_rect_intersection(bounds, clip)) &&
              application_search_window_message(WM_LBUTTONDOWN, 0, point(bounds)) &&
              g_app.search->focused && g_app.search->edit.caret == before_pointer &&
              application_search_window_message(WM_LBUTTONUP, 0, point(bounds)) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("in:70 ")),
          "search choices: actual hidden-HWND press/release accepts the exact row while retaining editor "
          "focus");
    check(choice_draft(utf8_literal("in:")) &&
              ui_search_scene_filter_choice_geometry(g_app.search->scene, 0, &bounds, &clip) &&
              application_search_window_message(WM_LBUTTONDOWN, 0, point(bounds)),
          "search choices: stale-release case captures a real current row");
    message_composer_set_selection(&g_app.search->edit, 0, 0);
    check(
        application_search_edit_changed(false) &&
            application_search_window_message(WM_LBUTTONUP, 0, point(bounds)) &&
            utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("in:")),
        "search choices: selection movement retires pointer capture and cannot activate a replacement row");
    check(choice_draft(utf8_literal("from:")) && key(VK_ESCAPE) && !g_app.search->suggestions &&
              g_app.search->panel && application_search_synchronize() &&
              !g_app.search->content.filter_choices,
          "search choices: Escape dismisses once and warm synchronization does not revive the chooser");
    check(choice_draft(utf8_literal("from:")) && modifiers(false, true) && key(VK_TAB) && modifiers() &&
              !g_app.search->focused,
          "search choices: Shift Tab retains ordinary reverse focus instead of accepting a value");
    check(choice_draft(utf8_literal("from:zi")) && member_query_active,
          "search choices: scope retirement starts with Search-owned server member interest");
    const u32 scope_withdrawals = member_query_withdrawals;
    check(chat_state_store_select_channel(&ui.chat_state, 80).ok(),
          "search choices: real Core selection admits private scope");
    ui.active_channel_id = 80;
    check(
        application_search_synchronize() && !g_app.search->choices && !member_query_active &&
            member_query_withdrawals == scope_withdrawals + 1 && choice_draft(utf8_literal("in:")) &&
            g_app.search->choices->source.rows.count == 1,
        "search choices: scope replacement releases the prior bank and projects readable private channel");
    auto* private_before = ui.chat_state.private_channels;
    const u64 recipient = 4;
    check(private_channel_store_initialize(&choice_private_store, {choice_private_records, 1},
                                           {choice_private_recipients, 2}) &&
              private_channel_store_replace(&choice_private_store,
                                            {80, 0, {&recipient, 1}, {}, true, false, false}),
          "search choices: typed private-channel inventory retains the actual current recipient");
    ui.chat_state.private_channels = &choice_private_store;
    check(choice_draft(utf8_literal("mentions:")) && g_app.search->choices->source.rows.count == 2 &&
              g_app.search->choices->rows[0].id == 5 && g_app.search->choices->rows[1].id == 4,
          "search choices: current private recipient is eligible independently of guild membership");
    auto* dm = const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, 80));
    const auto actions = dm ? dm->actions : 0;
    if (dm) {
        dm->actions &= ~channel_action_read_history;
        ++ui.chat_state.channel_mutations;
    }
    check(application_search_synchronize() && !g_app.search->content.filter_choices &&
              !choices_current(g_app.search),
          "search choices: withdrawn private read authority disables cached suggestions immediately");
    if (dm) {
        dm->actions = actions;
        ++ui.chat_state.channel_mutations;
    }
    check(chat_state_store_select_channel(&ui.chat_state, 70).ok(),
          "search choices: fixture returns through actual Core guild selection");
    ui.active_channel_id = 70;
    ui.chat_state.private_channels = private_before;
    ui.guild_member_store = members_before;
    application_search_synchronize();
    check(choice_draft(utf8_literal("from:")),
          "search choices: reopen fence captures current owner source");
    old.source_revision = g_app.search->choices->source.revision;
    check(application_search_close_panel() && !g_app.search->choices &&
              choice_draft(utf8_literal("from:")) && !activate(*g_app.search, old) &&
              application_search_close_panel() && !g_app.search->choices &&
              account.submissions == submissions_before && !application_search_pending(1),
          "search choices: close/reopen publications reject old actions, free the lazy bank and never "
          "submit");
}
bool click_target(UiSearchAction action, u32 index) noexcept {
    const auto* scene = g_app.renderer.search_scene;
    for (u32 slot = 1; scene && slot < scene->widgets.count; ++slot) {
        if (scene->targets[slot].action != action || scene->targets[slot].source_index != index)
            continue;
        const auto* node = ui_widget_manager_layout_node(
            scene->widgets, {static_cast<u16>(slot), scene->widgets.generation});
        return node && application_search_window_message(WM_LBUTTONDOWN, 0, point(node->rectangle)) &&
               application_search_window_message(WM_LBUTTONUP, 0, point(node->rectangle));
    }
    return false;
}
constexpr auto tabs_body = utf8_literal(
    R"({"tabs":{"messages":{"messages":[[{"id":"300","channel_id":"70","author":{"id":"3","username":"Tab author"},"content":"Tabbed hit","timestamp":"2026-01-01T00:00:00Z","hit":true}]],"total_results":41},)"
    R"("media":{"messages":[],"total_results":6},"files":{"messages":[],"total_results":0},)"
    R"("links":{"messages":[],"total_results":2},"pins":{"messages":[],"total_results":1}},"documents_indexed":3})");
constexpr auto media_body = utf8_literal(
    R"({"total_results":6,"messages":[[{"id":"310","channel_id":"70","author":{"id":"3","username":"Media author"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,)"
    R"("attachments":[{"id":"9001","filename":"cat.png","size":64,"content_type":"image/png","url":"https://cdn.discordapp.com/attachments/70/9001/cat.png","width":64,"height":32}]}]]})");
void tabs_and_recent() noexcept {
    auto& s = *g_app.search;
    check(query(utf8_literal("envelope")) && application_search_submit(398, 1) &&
              account.query.endpoint == MessageSearchEndpoint::tabs &&
              response(398, 200, utf8_literal(R"({"unexpected":true})")) && s.tabs_unavailable &&
              application_search_submit(399, 1) &&
              account.query.endpoint == MessageSearchEndpoint::legacy &&
              response(399, 200, result_body) && s.bank,
          "search runtime: an unreadable tabs envelope retries the same query through GET");
    s.tabs_unavailable = false;
    check(query(utf8_literal("tabs")) && application_search_submit(401, 1) &&
              account.query.endpoint == MessageSearchEndpoint::tabs && account.query.tab_counts &&
              account.query.tab == MessageSearchTab::messages && s.content.tabs,
          "search runtime: a new query uses the tabs endpoint and asks every tab for its total");
    check(response(401, 200, tabs_body) && s.bank && s.tab_known == 0x1f && s.content.tab_totals[1] == 6 &&
              s.content.tab_totals[3] == 2 && s.content.total == 41 &&
              g_app.renderer.search_scene->tab_bytes[1] == 1 &&
              g_app.renderer.search_scene->layout == UiSearchLayout::list,
          "search runtime: the tab strip shows every known tab total from one response");
    const auto* bank = s.bank;
    check(click_target(UiSearchAction::tab, 1) && s.tab == MessageSearchTab::media &&
              application_search_submit(402, 1) && account.query.tab == MessageSearchTab::media &&
              !account.query.tab_counts && !account.query.offset && s.bank == bank &&
              s.content.result_tab == 0 && s.tab_known == 0x1f &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::tab,
          "search runtime: a tab switch keeps counts and published results while the tab page loads");
    check(response(402, 404) && s.tabs_unavailable && application_search_pending(1) &&
              application_search_submit(403, 1) &&
              account.query.endpoint == MessageSearchEndpoint::legacy &&
              account.query.tab == MessageSearchTab::media,
          "search runtime: an unavailable tabs route falls back to the legacy GET for the same tab");
    check(response(403, 200, media_body) && s.result_tab == MessageSearchTab::media &&
              g_app.renderer.search_scene->layout == UiSearchLayout::grid &&
              ui_search_scene_visible_tile_count(*g_app.renderer.search_scene) == 1 &&
              s.content.tab_totals[1] == 6,
          "search runtime: media results publish as a tile grid");
    UiSearchTarget tile{};
    UiRect cover{}, clip{};
    const bool resolved =
        ui_search_scene_visible_tile(*g_app.renderer.search_scene, 0, &tile) &&
        ui_search_scene_rich_preview_geometry(*g_app.renderer.search_scene, tile, &cover, &clip);
    const u32 opens = viewer_opens;
    check(resolved && application_search_window_message(WM_LBUTTONDOWN, 0, point(clip)) &&
              application_search_window_message(WM_LBUTTONUP, 0, point(clip)) &&
              viewer_opens == opens + 1 && viewer_index == 0 && ui_rect_equal(viewer_origin, cover),
          "search runtime: a media tile opens the viewer at that item from its tile");
    check(query(utf8_literal("legacy now")) && application_search_submit(404, 1) &&
              account.query.endpoint == MessageSearchEndpoint::legacy &&
              account.query.tab == MessageSearchTab::media && response(404, 200, media_body),
          "search runtime: the session remembers an unavailable tabs route for later queries");
    check(s.recent && s.recent->count >= 2 &&
              utf8_equal(search_recent_entry(*s.recent, 0), utf8_literal("legacy now")) &&
              utf8_equal(search_recent_entry(*s.recent, 1), utf8_literal("tabs")),
          "search runtime: committed queries become recent searches, newest first");
    check(open(true) && s.content.recent_count && s.content.recent_count == s.recent->count &&
              utf8_equal(s.content.recent[0], utf8_literal("legacy now")) &&
              click_target(UiSearchAction::recent, 1) &&
              utf8_equal(message_composer_text(s.edit), utf8_literal("tabs")) &&
              application_search_pending(1) && application_search_submit(405, 1) &&
              utf8_equal({account.query.content, account.query.content_bytes}, utf8_literal("tabs")) &&
              utf8_equal(search_recent_entry(*s.recent, 0), utf8_literal("tabs")) &&
              response(405, 200, media_body),
          "search runtime: an empty focused field offers recent searches and choosing one runs it");
    check(open(true) && type(utf8_literal("x")) && !click_target(UiSearchAction::recent, 0),
          "search runtime: typing hides recent searches");
    check(open(true) && click(UiSearchAction::clear_recent) && !s.recent->count &&
              !s.content.recent_count && application_search_edit_focused(),
          "search runtime: Clear empties recent searches and keeps the field focused");
    s.tab = s.result_tab = MessageSearchTab::messages;
    s.tabs_unavailable = false;
    check(click(UiSearchAction::close), "search runtime: tab fixture closes");
}
// Over the open thread browser the header field is its single search entry
// point: scoped to its threads by default, widened by the chip, Backspace or
// Shift with the shortcut, re-scoped by the shortcut, and a widened search
// leaves the browser for the ordinary results pane.
void thread_scope_runtime() noexcept {
    auto& s = *g_app.search;
    message_composer_clear(&s.edit);
    char server_heading[160]{};
    const u32 server_bytes = application_search_synchronize() ? minimum<u32>(s.content.placeholder.bytes, 160) : 0;
    copy_bytes(server_heading, s.content.placeholder.data, server_bytes);
    const Utf8View server{server_heading, server_bytes};
    browser_open = true;
    ++browser_session;
    browser_searches = browser_closes = 0;
    const u32 submissions = account.submissions;
    check(application_search_synchronize() &&
              utf8_equal(s.content.placeholder, utf8_literal("Search threads in fixture-channel")) &&
              utf8_equal(s.content.scope_chip, utf8_literal("Threads")) && !s.content.panel &&
              g_app.renderer.search_scene && g_app.renderer.search_scene->scope.generation,
          "search runtime: over the thread browser the header field stays and is scoped to its threads");
    const auto chip = action_bounds(UiSearchAction::scope);
    const auto& scene = s.scene;
    check(chip.left >= scene.field.left && chip.right <= scene.field.right && chip.top >= scene.field.top &&
              chip.bottom <= scene.field.bottom && chip.right > chip.left && scene.text.left >= chip.right &&
              scene.text.right - scene.text.left > 80,
          "search runtime: the scope chip leads the field and the query text starts after it");
    check(open() && !s.panel && !message_composer_text(s.edit).bytes && !s.content.suggestions,
          "search runtime: the shortcut over the browser focuses the scoped field without an in: prefill or "
          "results pane");
    check(type(utf8_literal("design")) && !s.content.suggestions && !s.content.recent_count &&
              key(VK_RETURN) && browser_searches == 1 &&
              utf8_equal({browser_query, browser_query_bytes}, utf8_literal("design")) &&
              account.submissions == submissions && !application_search_pending(1) && !s.panel &&
              application_search_edit_focused() && browser_open,
          "search runtime: Enter sends thread names to the browser's list, not a message search");
    check(open() && s.edit.anchor == 0 && s.edit.caret == 6,
          "search runtime: the shortcut again selects the last thread query so typing replaces it");
    check(click(UiSearchAction::scope) && !s.content.scope_chip.bytes &&
              server_bytes && utf8_equal(s.content.placeholder, server) &&
              !g_app.renderer.search_scene->scope.generation && application_search_edit_focused() && browser_open,
          "search runtime: removing the chip widens the field to the whole server without leaving the browser");
    check(key(VK_RETURN) && browser_closes == 1 && !browser_open && s.panel && application_search_pending(1) &&
              browser_searches == 1,
          "search runtime: a widened search leaves the thread browser for the results pane");
    application_search_close_panel();
    browser_open = true;
    ++browser_session;
    check(open() && utf8_equal(s.content.scope_chip, utf8_literal("Threads")) && key(VK_BACK) &&
              utf8_equal(s.content.scope_chip, utf8_literal("Threads")) && !s.edit.bytes && key(VK_BACK) &&
              !s.content.scope_chip.bytes && application_search_edit_focused(),
          "search runtime: a new browsing session starts scoped; Backspace clears the query, then the chip");
    check(open() && utf8_equal(s.content.scope_chip, utf8_literal("Threads")) &&
              utf8_equal(s.content.placeholder, utf8_literal("Search threads in fixture-channel")),
          "search runtime: the shortcut re-scopes the field to the focused thread browser");
    check(open(true) && !s.content.scope_chip.bytes && browser_open && !s.panel && s.content.suggestions,
          "search runtime: Shift with the shortcut searches the whole server from the browser");
    browser_forum = true;
    ++browser_session;
    check(application_search_synchronize() &&
              utf8_equal(s.content.placeholder, utf8_literal("Search posts in fixture-channel")) &&
              utf8_equal(s.content.scope_chip, utf8_literal("Posts")),
          "search runtime: a forum browser scopes the field to its posts");
    browser_forum = browser_open = false;
    application_search_close_panel();
    message_composer_clear(&s.edit);
    check(application_search_synchronize() && !s.content.scope_chip.bytes && utf8_equal(s.content.placeholder, server),
          "search runtime: closing the browser restores the server scope");
}

void indexing_retry_runtime() noexcept {
    u32 scratch[256];
    struct RetryCase {
        Utf8View body;
        u64 milliseconds;
    };
    constexpr RetryCase valid[] = {
        {utf8_literal(R"({"code":110000,"retry_after":30})"), 30000},
        {utf8_literal(R"({"code":110000,"retry_after":30.0001,"documents_indexed":0})"), 30001},
        {utf8_literal(R"({"code":110000,"retry_after":0.0001})"), 1},
        {utf8_literal(R"({"code":110000,"retry_after":0,"global":true})"), 0},
        {utf8_literal(R"({"code":110000,"retry_after":18446744073709551.615})"), ~u64{0}},
        {utf8_literal(R"({"code":110000,"message":"Index not yet available"})"), 0},
        {utf8_literal(R"({"retry_after":1})"), 1000},
    };
    bool parsed = true;
    for (const auto& item : valid) {
        const auto decoded = decode_search_indexing_body(item.body, {scratch, 256}, 64);
        parsed &= decoded.ok() && decoded.retry_after_ms == item.milliseconds && !decoded.global;
    }
    check(parsed && decode_rate_limit_body(valid[0].body, {scratch, 256}, 64).error ==
                        RateLimitBodyError::missing_required_field,
          "search indexing: retry seconds ceil exactly and never relax the separate 429 scope decoder");
    constexpr Utf8View invalid[] = {
        {}, utf8_literal("{}"), utf8_literal(R"({"code":110000,"retry_after":-1})"),
        utf8_literal(R"({"code":110000,"retry_after":-0})"), utf8_literal(R"({"code":110000,"retry_after":"30"})"),
        utf8_literal(R"({"code":110000,"retry_after":true})"), utf8_literal(R"({"code":110000,"retry_after":NaN})"),
        utf8_literal(R"({"code":110000,"retry_after":Infinity})"), utf8_literal(R"({"code":110000,"retry_after":1e999})"),
        utf8_literal(R"({"code":110000,"retry_after":18446744073709552})"),
        utf8_literal(R"({"code":110000,"retry_after":18446744073709551.6151})"),
        utf8_literal(R"({"code":110000,"retry_after":30,"retry_after":1})"),
        utf8_literal(R"({"code":110000,"retry_after":30,"retry_\u0061fter":1})"),
        utf8_literal(R"({"nested":{"retry_after":30}})"),
        utf8_literal(R"({"code":110000,"retry_after":30)"), utf8_literal(R"({"code":110000,"retry_after":30} trailing)"),
        utf8_literal(R"({"code":110001,"retry_after":30})"), utf8_literal(R"({"code":1})"),
        utf8_literal(R"({"code":110000,"code":110000})"), utf8_literal(R"({"code":"110000"})"),
    };
    bool rejected = true;
    for (const auto body : invalid)
        rejected &= !decode_search_indexing_body(body, {scratch, 256}, 64).ok();
    check(rejected,
          "search indexing: malformed duplicate negative nonfinite and overflowing retry bodies fail closed");

    check(open(true), "search indexing: native scope opens for retry integration");
    u64 request = 8000;
    const auto* bank = g_app.search->bank;
    struct RuntimeCase {
        Utf8View body;
        u64 header;
        u64 floor;
    };
    constexpr RuntimeCase cases[] = {
        {valid[1].body, 0, 30001}, {valid[0].body, 1250, 30000},
        {valid[0].body, 60000, 60000}, {valid[3].body, 0, 5000},
        {invalid[11], 7000, 7000}, {valid[5].body, 0, 5000}, {valid[6].body, 0, 1000},
    };
    bool floors = true;
    for (const auto& item : cases) {
        floors &= query(utf8_literal("index retry floor")) && application_search_submit(++request, 1);
        const u64 before = clock_monotonic_milliseconds();
        floors &= response(request, 202, item.body, 0, item.header);
        const u64 deadline = application_search_next_wake();
        floors &= g_app.search->controller.state == MessageSearchState::indexing &&
                  g_app.search->bank == bank && deadline >= before + item.floor &&
                  !application_search_pending(deadline - 1) && application_search_pending(deadline);
    }
    check(floors,
          "search indexing: body-only and header conflicts preserve the longest floor including valid zero and malformed body");
    bool no_guess = true;
    for (const auto body : invalid) {
        no_guess &= query(utf8_literal("invalid index retry")) && application_search_submit(++request, 1) &&
                    response(request, 202, body) &&
                    g_app.search->controller.failure == MessageSearchFailure::deadline &&
                    !application_search_next_wake() && !application_search_pending(~u64{0});
    }
    check(no_guess, "search indexing: absent or invalid retry sources cannot create automatic polling");
    const u64 header_deadline = clock_monotonic_milliseconds() + 60000;
    check(query(utf8_literal("index absolute header")) && application_search_submit(++request, 1) &&
              response(request, 202, valid[0].body, header_deadline) &&
              application_search_next_wake() == header_deadline,
          "search indexing: absolute authoritative deadline survives a shorter body floor");
    check(query(utf8_literal("index stale receipt")) && application_search_submit(++request, 1) &&
              query(utf8_literal("index replacement")) && response(request, 202, valid[0].body) &&
              g_app.search->controller.state == MessageSearchState::ready_to_submit &&
              !g_app.search->controller.retry_at_ms && application_search_pending(1),
          "search indexing: cancelled prior-query receipt cannot delay its replacement");
    check(application_search_submit(++request, 1) && response(request, 202, valid[0].body) &&
              application_search_next_wake() && click(UiSearchAction::close) &&
              !application_search_next_wake() && !application_search_pending(~u64{0}) &&
              !response(request, 202, valid[0].body),
          "search indexing: closing the pane retires its index floor and rejects a duplicate receipt");
    check(open(true) && query(utf8_literal("index profile receipt")) &&
              application_search_submit(++request, 1),
          "search indexing: profile receipt fixture submits one exact ticket");
    ++g_app.account.status.profile_id;
    check(application_search_synchronize() && response(request, 202, valid[0].body) &&
              !g_app.search->controller.retry_at_ms && !application_search_next_wake() &&
              !application_search_active(),
          "search indexing: old-profile receipt cannot retain or publish an index floor");
}

void rich_scroll_runtime() noexcept {
    constexpr auto body = utf8_literal(R"json({"total_results":4,"messages":[[{"id":"410","channel_id":"70","author":{"id":"3","username":"Fixture"},"content":"Short result","timestamp":"2026-01-01T00:00:00Z","hit":true}],[{"id":"409","channel_id":"70","author":{"id":"3","username":"Fixture"},"content":"Video result","timestamp":"2026-01-01T00:00:00Z","hit":true,"embeds":[{"type":"video","video":{"url":"https://www.pornhub.com/embed/683dbe0849d99","width":1280,"height":720},"thumbnail":{"url":"https://example.com/poster.jpg","width":1280,"height":720}}]}],[{"id":"408","channel_id":"70","author":{"id":"3","username":"Fixture"},"content":"Second video","timestamp":"2026-01-01T00:00:00Z","hit":true,"embeds":[{"type":"video","video":{"url":"https://www.youtube.com/embed/aB_01234-xy","width":1280,"height":720},"thumbnail":{"url":"https://example.com/second.jpg","width":1280,"height":720}}]}],[{"id":"407","channel_id":"70","author":{"id":"3","username":"Fixture"},"content":"Last result","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})json");
    check(query(utf8_literal("scroll fixture")) && application_search_submit(980, 1) && response(980, 200, body),
          "search scrolling: four rich results publish through the real controller");
    auto& s = *g_app.search;
    check(ui_search_scene_scroll_maximum(s.scene) > 0 &&
          ui_scrollbar_geometry(s.scene.body, s.scene.scroll_dips, ui_extent_dips(s.bank->list.total_units)).visible,
          "search scrolling: rich media contributes to the scrollbar extent");
    const float maximum = ui_search_scene_scroll_maximum(s.scene);
    check(ui_search_scene_scroll(&s.scene, maximum) && s.scene.scroll_dips > 0 &&
          application_search_synchronize() && s.scene.scroll_dips > 0,
          "search scrolling: the last rich result remains reachable after controller synchronization");
    const auto saved_clock = g_app.clock;
    const auto saved_start = g_app.process_start;
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.scroll_host.immediate = false;
    POINT wheel{static_cast<LONG>(s.scene.body.left + 20), static_cast<LONG>(s.scene.body.top + 20)};
    bool scrolled = ui_search_scene_scroll(&s.scene, 0) && application_search_synchronize();
    const auto video_text_top = [&]() noexcept {
        const auto* text = application_search_message_text_presentation();
        for (u32 i = 0; text && i < text->command_count; ++i)
            if (text->commands[i].message_id == 409 &&
                text->commands[i].measurement_kind == MessageTextMeasurementKind::content)
                return text->commands[i].rectangle.top;
        return -10000.f;
    };
    const float before_top = video_text_top(), before_scroll = s.scene.scroll_dips;
    scrolled = scrolled && ClientToScreen(g_app.window, &wheel) &&
        application_search_window_message(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)),
                                          MAKELPARAM(wheel.x, wheel.y));
    bool moved = false;
    scrolled = scrolled && ui_scroll_host_tick(&g_app.scroll_host, application_now_us() + 500000, &moved);
    const float after_top = video_text_top();
    const float expected_top = before_top - (s.scene.scroll_dips - before_scroll);
    check(before_top > 0 && after_top > 0 && after_top >= expected_top - .1f && after_top <= expected_top + .1f,
          "search scrolling: one wheel notch publishes matching rich text before media or renderer work");
    check(scrolled && moved && s.scene.scroll_dips > 0,
          "search scrolling: native wheel and animation tick share the process-relative clock");
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.clock = saved_clock;
    g_app.process_start = saved_start;
}
void run() noexcept {
    check(initialize(), "search runtime: real borrowed model, shell and hidden native host initialize");
    if (!g_app.search || !g_app.renderer.search_scene) {
        release();
        return;
    }
    test_search_measurement();
    const auto timeline_before = g_app.message_view.timeline;
    const auto store_before = g_app.message_view.store;
    const auto record_before = records[0];
    check(!g_app.search->bank && !g_app.search->candidate && !application_search_pending(1) &&
              !application_search_read_blocked() && !application_search_next_wake(),
          "search runtime: dormant title field has no results bank, request or idle deadline");
    filter_choices_runtime();
    check(open() && type(utf8_literal("alpha")) && account.submissions == 0 &&
              !application_search_pending(1) && application_search_read_blocked() &&
              message_composer_text(g_app.search->edit).bytes > 5,
          "search runtime: actual CtrlF prefill and typing never submit per keystroke");
    check(key(VK_RETURN) && application_search_pending(1) && account.submissions == 0 &&
              application_search_edit_focused(),
          "search runtime: successful Enter keeps native query ownership before request submission");
    const auto committed_query = g_app.search->controller.query;
    check(type(utf8_literal(" refinement")) && account.submissions == 0 &&
              bytes_equal(&committed_query, &g_app.search->controller.query, sizeof(committed_query)) &&
              application_search_edit_focused() && application_search_pending(1),
          "search runtime: post-Enter refinement edits only query text without replacing the committed "
          "request");
    check(
        application_search_submit(101, 1) && account.submissions == 1 &&
            account.query.scope == MessageSearchScope::guild && account.query.scope_id == 7 &&
            account.query.channel_count == 1 && account.query.channel_ids[0] == 70 &&
            utf8_equal({account.query.content, account.query.content_bytes}, utf8_literal("alpha")) &&
            !application_search_next_wake(),
        "search runtime: original Enter query submits once despite later refinement and no fetching timer");
    check(type(utf8_literal(" retained")) && account.submissions == 1 && account.cancellations == 0 &&
              g_app.search->submitted.request_id == 101 &&
              bytes_equal(&committed_query, &g_app.search->controller.query, sizeof(committed_query)),
          "search runtime: typing during fetch preserves the exact committed ticket and cancellation "
          "ownership");
    const u64 before_indexing = clock_monotonic_milliseconds();
    check(response(101, 202, {}, 0, 1250) && !g_app.search->bank &&
              g_app.search->controller.state == MessageSearchState::indexing,
          "search runtime:202 exposes indexing without publishing an empty result page");
    const u64 indexing_deadline = application_search_next_wake();
    check(indexing_deadline >= before_indexing + 1250 &&
              indexing_deadline <= clock_monotonic_milliseconds() + 1250 &&
              !application_search_pending(indexing_deadline - 1) &&
              application_search_pending(indexing_deadline) &&
              application_search_submit(102, indexing_deadline),
          "search runtime: indexing RetryAfter header gates its explicit native deadline");
    const u64 rate_deadline = clock_monotonic_milliseconds() + 7000;
    check(response(102, 429, {}, rate_deadline) &&
              g_app.search->controller.state == MessageSearchState::rate_limited &&
              application_search_next_wake() == rate_deadline &&
              !application_search_pending(rate_deadline - 1) &&
              application_search_submit(103, rate_deadline),
          "search runtime:429 keeps an authoritative rate deadline distinct from indexing");
    check(response(103, 200, result_body) && g_app.search->bank &&
              g_app.search->controller.state == MessageSearchState::ready &&
              g_app.renderer.search_scene->source->count == 3 &&
              g_app.renderer.search_scene->visible[0].hit && !g_app.renderer.search_scene->visible[1].hit &&
              g_app.search->content.stale && g_app.search->content.total == 41 &&
              utf8_equal(g_app.search->content.status,
                         utf8_literal("Press Enter to update results.")) &&
              !application_search_next_wake() && navigations == 0,
          "search runtime: owned hit/context response labels refined draft as previous search with its "
          "actual total");
    const auto* ready_bank = g_app.search->bank;
    const u32 submissions_before_refinement = account.submissions;
    check(
        type(utf8_literal(" ready")) && g_app.search->bank == ready_bank &&
            account.submissions == submissions_before_refinement && account.cancellations == 0 &&
            bytes_equal(&committed_query, &g_app.search->controller.query, sizeof(committed_query)) &&
            g_app.search->content.stale && g_app.search->content.total == 41 &&
            utf8_equal(g_app.search->content.status,
                       utf8_literal("Press Enter to update results.")),
        "search runtime: ready-result draft typing keeps the owned bank and committed query without REST");
    check(g_app.search->suggestions && key(VK_ESCAPE) && !g_app.search->suggestions &&
              g_app.search->panel && g_app.search->bank == ready_bank &&
              account.submissions == submissions_before_refinement,
          "search runtime: Escape dismisses query suggestions without closing retained results or "
          "submitting");
    const auto result_bounds = action_bounds(UiSearchAction::result);
    check(application_search_window_message(WM_MOUSEMOVE, 0, point(result_bounds)) &&
              g_app.search->scene.widgets.hovered.generation != 0 &&
              !application_search_window_message(WM_MOUSELEAVE, 0, 0) &&
              !g_app.search->scene.widgets.hovered.generation,
          "search runtime: real pointer hover and leave update retained targets without a timer");
    check(
        click(UiSearchAction::next) && application_search_submit(104, rate_deadline + 1) &&
            g_app.search->content.stale && g_app.search->content.total == 41 &&
            utf8_equal(g_app.search->content.status, utf8_literal("Updating results…")) &&
            account.query.offset == 25 && !response(103, 200, result_body) &&
            response(104, 200, result_body),
        "search runtime: page activation uses server offset and ignores prior-page duplicate replies");
    check(bytes_equal(&timeline_before, &g_app.message_view.timeline, sizeof(timeline_before)) &&
              bytes_equal(&store_before, &g_app.message_view.store, sizeof(store_before)) &&
              bytes_equal(&record_before, records, sizeof(record_before)) &&
              account.acknowledgements == 0 && navigations == 0 && application_search_read_blocked(),
          "search runtime: fetch, page and render leave Main conversation and read ACK state untouched");
    check(click(UiSearchAction::result) && navigations == 1 && navigated_channel == 70 &&
              navigated_message == 300 && account.acknowledgements == 0,
          "search runtime: explicit result activation alone calls existing typed navigation");
    tabs_and_recent();

    check(query(utf8_literal("busy")) &&
              g_app.search->committed_edit_revision == g_app.search->edit.text_revision &&
              utf8_equal(g_app.search->content.status, utf8_literal("Updating results…")),
          "search runtime: new explicit Enter commits the current draft and labels previous results while "
          "updating");
    account.admission = RestClientSubmitError::busy;
    check(!application_search_submit(105, 1) && !application_search_next_wake() &&
              !g_app.search->submitted.request_id && g_app.search->bank,
          "search runtime: busy shared owner preserves stale results without an idle retry timer");
    account.admission = RestClientSubmitError::none;
    check(
        application_search_submit(106, 1) && query(utf8_literal("replacement")) &&
            account.cancelled_id == 106 && !application_search_pending(1) &&
            response(106, 200, result_body) && application_search_pending(1) &&
            application_search_submit(107, 1),
        "search runtime: query replacement cancels and drains only its exact old ticket before submitting");
    const u32 cancellations_before_close = account.cancellations;
    check(click(UiSearchAction::close) && account.cancellations == cancellations_before_close + 1 &&
              account.cancelled_id == 107 && !application_search_active() &&
              response(107, 200, result_body) && !application_search_read_blocked() &&
              !application_search_next_wake(),
          "search runtime: closed pane drains cancelled completion without publishing or retaining a wake");

    const u32 submissions_before_button = account.submissions;
    check(open(true) && type(utf8_literal("button")) && click(UiSearchAction::submit) &&
              !application_search_edit_focused() && g_app.context_focus == &g_app.search->scene.widgets &&
              g_app.search->scene.widgets.focused.generation &&
              g_app.search->scene.widgets.focused.slot < 48 &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::submit &&
              application_search_input(UiInputKind::character, 'z', false) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("button")) &&
              account.submissions == submissions_before_button && application_search_read_blocked(),
          "search runtime: magnifier commit owns Search focus and later character input cannot leak into "
          "chat");

    const u32 before_empty = account.submissions;
    check(query({}) && !application_search_pending(1) && !application_search_submit(210, 1) &&
              account.submissions == before_empty && g_app.search->validation.bytes,
          "search runtime: empty explicit query fails visibly without a broad REST submission");
    check(query(utf8_literal("in:90 local")) && !application_search_pending(1) &&
              !application_search_submit(211, 1) && account.submissions == before_empty &&
              g_app.search->validation.bytes,
          "search runtime: numeric channel filter from another guild fails visibly before submission");
    check(
        query(utf8_literal("local")) && application_search_submit(212, 1) &&
            response(212, 200, result_body) && g_app.search->bank,
        "search runtime: results without guild metadata use authoritative cached local channel membership");
    const auto* scoped_bank = g_app.search->bank;
    constexpr auto foreign_body = utf8_literal(
        R"({"total_results":8410672,"messages":[[{"id":"301","channel_id":"90","author":{"id":"3","username":"Foreign"},"content":"foreign fixture","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(query(utf8_literal("mixed")) && application_search_submit(213, 1) &&
              response(213, 200, foreign_body) && g_app.search->bank == scoped_bank &&
              g_app.search->controller.failure == MessageSearchFailure::decode &&
              g_app.search->content.total == 41 && g_app.search->validation.bytes,
          "search runtime: omitted guild metadata cannot admit a foreign cached channel or its enormous "
          "total");
    constexpr auto conflicting_metadata = utf8_literal(
        R"({"total_results":8410672,"messages":[[{"id":"301","channel_id":"90","guild_id":"7","author":{"id":"3","username":"Foreign"},"content":"foreign fixture","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(query(utf8_literal("conflicting")) && application_search_submit(218, 1) &&
              response(218, 200, conflicting_metadata) && g_app.search->bank == scoped_bank &&
              g_app.search->controller.failure == MessageSearchFailure::decode,
          "search runtime: explicit guild metadata cannot contradict cached channel membership");
    constexpr auto unproven_channel = utf8_literal(
        R"({"total_results":8410672,"messages":[[{"id":"301","channel_id":"99","author":{"id":"3","username":"Unproven"},"content":"unproven fixture","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(query(utf8_literal("unproven")) && application_search_submit(219, 1) &&
              response(219, 200, unproven_channel) && g_app.search->bank == scoped_bank &&
              g_app.search->controller.failure == MessageSearchFailure::decode,
          "search runtime: unknown channel without response guild authority fails closed without "
          "truncating cloud results");
    check(query(utf8_literal("scope fence")) && application_search_submit(214, 1),
          "search runtime: scope-fence request has an exact pending ticket");
    g_app.account_ui->chat_state.selected_guild_id = 8;
    check(application_search_synchronize() && !g_app.search->bank && g_app.search->scope_id == 0 &&
              g_app.search->source.current && !g_app.search->source.current(g_app.search) &&
              !g_app.search->scene.source && !g_app.search->scene.count &&
              ui_search_scene_current(g_app.search->scene) && g_app.search->validation.bytes &&
              !application_search_pending(1) && g_app.search->submitted.request_id == 214 &&
              account.cancelled_id == 214 && response(214, 200, result_body) && !g_app.search->bank,
          "search runtime: changed server header with old active channel quarantines scope and drains old "
          "reply without publication");
    g_app.account_ui->active_channel_id = 90;
    g_app.account_ui->chat_state.selected_channel_id = 90;
    check(application_search_synchronize() && context_current(*g_app.search) &&
              g_app.search->scope == MessageSearchScope::guild && g_app.search->scope_id == 8,
          "search runtime: resolved successor model selection and active view establish exact guild scope");
    check(query(utf8_literal("successor")) && application_search_pending(1),
          "search runtime: successor explicit query queues within resolved guild scope");
    check(application_search_submit(215, 1) && account.query.scope_id == 8 &&
              account.query.scope == MessageSearchScope::guild,
          "search runtime: successor request uses its own exact guild endpoint");
    check(response(215, 200, foreign_body) && g_app.search->bank &&
              g_app.search->controller.state == MessageSearchState::ready,
          "search runtime: successor reply publishes only after current-guild channel validation");
    check(utf8_equal(g_app.search->content.heading, utf8_literal("Search Successor fixture guild")),
          "search runtime: successor heading derives exact selected guild name independently of reply");
    check(query(utf8_literal("epoch fence")) && application_search_submit(216, 1),
          "search runtime: same-user epoch fixture submits through the real host hook");
    ++g_app.plugin_account_scope_serial;
    check(application_search_synchronize() && !g_app.search->bank && !application_search_active() &&
              g_app.search->submitted.request_id == 216 && response(216, 200, foreign_body) &&
              !g_app.search->bank && !application_search_next_wake(),
          "search runtime: same user and guild under a new account epoch cannot publish an older receipt "
          "or bank");
    check(query(utf8_literal("profile fence")) && application_search_submit(217, 1),
          "search runtime: same-user profile fixture has an exact ticket");
    ++g_app.account.status.profile_id;
    check(application_search_synchronize() && response(217, 200, foreign_body) && !g_app.search->bank &&
              !application_search_active(),
          "search runtime: same user under another profile drains the old request without adopting its "
          "results");
    g_app.account_ui->chat_state.selected_guild_id = 7;
    g_app.account_ui->active_channel_id = 70;
    g_app.account_ui->chat_state.selected_channel_id = 70;
    check(application_search_synchronize(),
          "search runtime: original scoped channel is restored for account fence");
    check(query(utf8_literal("account fence")) && application_search_submit(108, 1),
          "search runtime: account-fence request begins through production hook");
    g_app.account_ui->chat_state.self_user_id = 6;
    check(response(108, 200, result_body) && application_search_synchronize() &&
              g_app.search->account_id == 6 && !g_app.search->bank && !application_search_active(),
          "search runtime: account change cannot publish the previous account's response or results");
    g_app.account_ui->active_channel_id = 80;
    g_app.account_ui->chat_state.selected_guild_id = 0;
    g_app.account_ui->chat_state.selected_channel_id = 80;
    check(query(utf8_literal("dm")) && application_search_submit(109, 1) &&
              account.query.scope == MessageSearchScope::channel && account.query.scope_id == 80,
          "search runtime: DM selection uses the actual channel search endpoint contract");
    response(109, 200, utf8_literal(R"({"total_results":0,"messages":[]})"));
    check(open(true), "search runtime: actual keyboard ownership restores editor");
    const auto edit_bounds = application_search_edit_bounds();
    hit_position = 0;
    check(application_search_window_message(WM_LBUTTONDOWN, 0, point(edit_bounds)) &&
              GetCapture() == g_app.window && g_app.search->dragging,
          "search runtime: edit press owns actual native selection capture");
    settings = true;
    check(
        application_search_synchronize() && !application_search_input(UiInputKind::character, 'x', false) &&
            !application_search_edit_focused() && !g_app.search->focused && !g_app.search->dragging &&
            !g_app.search->captured && GetCapture() != g_app.window,
        "search runtime: modal front retires native capture and keyboard ownership before blocking input");
    application_search_window_message(WM_CAPTURECHANGED, 0, 0);
    ReleaseCapture();
    settings = false;
    application_search_window_message(WM_KILLFOCUS, 0, 0);
    check(!g_app.search->dragging && !application_search_edit_focused() &&
              !application_search_edit_next_wake(),
          "search runtime: capture and native focus loss retire editor drag and caret deadlines");
    check(open(true) && key(VK_TAB) && !g_app.search->focused &&
              g_app.context_focus == &g_app.search->scene.widgets &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::filter_from &&
              key(VK_RETURN) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:")) &&
              g_app.search->suggestions && !application_search_pending(1),
          "search runtime: Tab and Enter activate a suggested prefix without submitting the query");
    check(open(true) && type(utf8_literal("bounded")) && key(VK_ESCAPE) && click(UiSearchAction::sort) &&
              !g_app.search->focused && g_app.search->sort_menu && key(VK_TAB) &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::oldest &&
              key(VK_RETURN) && !g_app.search->sort_menu &&
              g_app.search->controller.query.sort == MessageSearchSort::oldest &&
              application_search_pending(1),
          "search runtime: checked Sort owns keyboard choices and Enter commits the selected order");
    check(click(UiSearchAction::sort) && g_app.search->sort_menu && !g_app.search->focused,
          "search runtime: pointer Sort opens independently of query editor focus");
    check(click(UiSearchAction::sort, WM_LBUTTONDBLCLK) && !g_app.search->sort_menu &&
              !g_app.search->captured && click(UiSearchAction::sort) && g_app.search->sort_menu,
          "search runtime: rapid Sort second press closes the menu once and releases capture");
    settings = true;
    check(application_search_synchronize() && !g_app.search->sort_menu && !g_app.search->suggestions &&
              !g_app.search->filters,
          "search runtime: an opaque settings owner retires an unfocused search submenu");
    settings = false;
    check(application_search_synchronize() && !g_app.search->sort_menu && open(true),
          "search runtime: returning to search cannot revive a covered submenu");
    const auto dismiss_bounds =
        UiRect{g_app.search->scene.body.left + 4, g_app.search->scene.body.bottom - 24,
               g_app.search->scene.body.left + 12, g_app.search->scene.body.bottom - 16};
    const auto* dismissed_bank = g_app.search->bank;
    const u32 navigations_before_dismiss = navigations;
    check(
        application_search_window_message(WM_LBUTTONDOWN, 0, point(dismiss_bounds)) &&
            !g_app.search->suggestions && !g_app.search->focused &&
            application_search_window_message(WM_LBUTTONUP, 0, point(dismiss_bounds)) &&
            g_app.search->bank == dismissed_bank && navigations == navigations_before_dismiss,
        "search runtime: a covered result-pane click dismisses suggestions without background activation");
    check(open(true) && key(VK_ESCAPE) && !g_app.search->suggestions && key(VK_ESCAPE) &&
              !application_search_input(UiInputKind::character, 'x', false) &&
              !application_search_active() && repaints != 0,
          "search runtime: Escape key tail is consumed and closed search cannot capture later typing");
    indexing_retry_runtime();
    thread_scope_runtime();
    rich_bank_capacity();
    rich_publication();
    rich_scroll_runtime();
    poll_publication();
    private_privacy();
    check(content_retirements != 0 && !navigation_releases,
          "search runtime: source and scope retirement cancel content while preserving independently "
          "admitted navigation");
    const u32 content_before_release = content_retirements;
    release();
    check(content_retirements == content_before_release + 1 && navigation_releases == 1,
          "search runtime: full Search release retires content and navigation owners once");
    application_search_release();
    check(navigation_releases == 2 && content_retirements == content_before_release + 2,
          "search runtime: full release also retires independent pending owners after Search allocation is "
          "absent");
    check(!g_app.search && !g_app.renderer.search_scene && !application_search_next_wake(),
          "search runtime: release removes borrowed renderer projection and every search deadline");
    check(!forced_account_repaints && overlay_repaints,
          "search runtime: editing, pointer actions, keyboard choices and caret refresh never force the "
          "unrelated account text owner");
}
} // namespace maicord::search_runtime_test

namespace maicord {
bool renderer_bind_search_message_text_presentation(D3D11Renderer* renderer,
                                                    const MessageTextPresentation* p,
                                                    const UserStore* users,
                                                    UiTextMeasureCache* cache) noexcept {
    if (!renderer)
        return false;
    renderer->search_message_text = p;
    renderer->search_message_users = users;
    renderer->search_message_measure_cache = cache;
    return true;
}
void renderer_unbind_search_message_text_presentation(D3D11Renderer* renderer) noexcept {
    if (renderer) {
        renderer->search_message_text = nullptr;
        renderer->search_message_users = nullptr;
        renderer->search_message_measure_cache = nullptr;
    }
}
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++search_runtime_test::overlay_repaints;
    return true;
}
TextSystem renderer_text_system(D3D11Renderer*, UiTextFace) noexcept {
    return search_runtime_test::search_measurement_text_system();
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position) {
        return false;
    }
    *position = search_runtime_test::hit_position;
    return true;
}
namespace app_detail {
MessageMediaPolicy application_message_media_policy() noexcept {
    return {nullptr, [](const void*, const MessageMediaOwner&) noexcept {
        return search_runtime_test::media_policy;
    }, 1};
}
void application_user_settings_request_content_confirmation() noexcept {
    ++search_runtime_test::content_confirmations;
}
bool refresh_account_frame(bool force) noexcept { return refresh_account_text(force); }
bool application_poll_search_state(u64 id, u32 publication, Slice<UiPollAnswer>, UiPollState*,
                                   ApplicationPollSource*) noexcept {
    ++search_runtime_test::poll_projection_calls;
    search_runtime_test::projected_poll_id = id;
    search_runtime_test::projected_poll_publication = publication;
    return false;
}
bool application_downloads_active() noexcept {
    return search_runtime_test::downloads;
}
bool application_call_header_search(float, UiRect* field) noexcept {
    if (field)
        *field = {};
    return false; // No call view covers the title bar in this runtime.
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& ticket, u64 attachment,
                                        bool snapshot) noexcept {
    ++search_runtime_test::search_saves;
    search_runtime_test::saved_search_ticket = ticket;
    search_runtime_test::saved_search_attachment = attachment;
    search_runtime_test::saved_search_snapshot = snapshot;
    return true;
}
bool application_image_viewer_open_search(const ApplicationSearchSourceTicket&, MessageSceneRichItemKind,
                                          u32 index, bool, const UiRect& origin, MediaEmbedResource) noexcept {
    ++search_runtime_test::viewer_opens;
    search_runtime_test::viewer_index = index;
    search_runtime_test::viewer_origin = origin;
    return false;
}
bool application_video_open_search(const UiSearchTarget&) noexcept {
    return false;
}
bool application_search_content_activate(const ApplicationSearchSourceTicket&, UiSearchTarget) noexcept {
    return true;
}
void application_search_content_release() noexcept {
    ++search_runtime_test::content_retirements;
}
void application_search_navigation_synchronize() noexcept {
}
void application_search_navigation_release() noexcept {
    ++search_runtime_test::navigation_releases;
}
bool application_search_navigation_begin(const ApplicationSearchSourceTicket& ticket) noexcept {
    ApplicationSearchNavigationSource source{};
    return application_search_navigation_source(ticket, &source) &&
           application_navigate_known_message(source.channel_id, ticket.message_id);
}
void application_pinned_messages_close() noexcept {
}
bool application_thread_browser_active() noexcept {
    return search_runtime_test::browser_open;
}
bool application_thread_browser_search_scope(ApplicationThreadBrowserSearchScope* out) noexcept {
    if (!out || !search_runtime_test::browser_open)
        return false;
    *out = {70, search_runtime_test::browser_session, utf8_literal("fixture-channel"),
            search_runtime_test::browser_forum};
    return true;
}
bool application_thread_browser_search(Utf8View name) noexcept {
    ++search_runtime_test::browser_searches;
    search_runtime_test::browser_query_bytes = minimum<u32>(name.bytes, sizeof(search_runtime_test::browser_query));
    copy_bytes(search_runtime_test::browser_query, name.data, search_runtime_test::browser_query_bytes);
    return true;
}
void application_thread_browser_close() noexcept {
    ++search_runtime_test::browser_closes;
    search_runtime_test::browser_open = false;
}
bool refresh_account_text(bool force) noexcept {
    ++search_runtime_test::repaints;
    search_runtime_test::forced_account_repaints += force ? 1u : 0u;
    return true;
}
bool application_settings_active() noexcept {
    return search_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
const RelationshipStore* application_friends_relationships() noexcept {
    return &search_runtime_test::choice_relationships;
}
bool application_composer_text_input_composing() noexcept {
    return search_runtime_test::composing;
}
void application_member_query_note(u64 guild_id, u64, Utf8View query) noexcept {
    ++search_runtime_test::member_queries;
    search_runtime_test::member_query_active = true;
    search_runtime_test::composer_query_owned = false;
    search_runtime_test::member_query_guild = guild_id;
    search_runtime_test::member_query_bytes =
        minimum<u32>(query.bytes, sizeof(search_runtime_test::member_query));
    copy_bytes(search_runtime_test::member_query, query.data, search_runtime_test::member_query_bytes);
}
void application_member_query_cancel() noexcept {
    ++search_runtime_test::member_query_withdrawals;
    if (!search_runtime_test::composer_query_owned)
        search_runtime_test::member_query_active = false;
}
void application_composer_text_input_suspend() noexcept {
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_synchronize() noexcept {
    return true;
}
bool application_navigate_known_message(u64 channel, u64 message) noexcept {
    ++search_runtime_test::navigations;
    search_runtime_test::navigated_channel = channel;
    search_runtime_test::navigated_message = message;
    return true;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_search_runtime_entry() noexcept {
    maicord::search_runtime_test::run();
    maicord::test_exit(maicord::search_runtime_test::failures, "application search runtime");
}

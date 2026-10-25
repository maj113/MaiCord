#include "application_downloads_inactive.hpp"
#include "application_state.hpp"
#include "application_read_state.hpp"
#include "application_message_mutation_notice.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/protocol/message_ack.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "test_output.hpp"
#include "maicord/ui/forum_post_scene.hpp"

namespace maicord::read_state_runtime_test {
using namespace app_detail;
bool browser_blocks_read;
struct Fixture {
    GuildRecord guild;
    ChannelRecord channels[4];
    u16 guild_order[1], channel_order[4];
    u8 names[1024];
    ReadStateEntry pending[4];
    u8 account_sentinel;
    u8 response[kMessageAckMaximumBodyBytes + 128];
    MessageRecord records[8];
    u32 order[8], heights[8];
    u64 fenwick[8], thread_members[2][4];
    u8 text[1024];
    char draft[128];
} fixture{};
struct Observations {
    u32 failures, submissions, accepted, synchronized;
    u64 channel_id, message_id, request_id, next_request;
    u32 body_bytes, target_bytes;
    RestClientSubmitError admission;
    u8 body[kMessageAckMaximumBodyBytes];
    u8 target[kMessageAckMaximumTargetBytes];
} seen{};
struct Body {
    char token[512];
    u32 token_bytes, last_viewed, flags;
    bool token_seen, token_null, viewed_seen, flags_seen, manual_seen, manual, mention_seen;
    u32 mention_count;
};
void check(bool condition, const char* label) noexcept {
    test_check(&seen.failures, condition, label);
}
bool equals(const char* bytes, u32 count, Utf8View expected) noexcept {
    return count == expected.bytes && bytes_equal(bytes, expected.data, count);
}
bool visit_body(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& body = *static_cast<Body*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("token"))) {
        if (body.token_seen)
            return false;
        body.token_seen = true;
        body.token_null = value.kind == JsonValueKind::null_value;
        if (body.token_null)
            return true;
        JsonString token{};
        if (!json_value_as_string(value, &token))
            return false;
        const auto decoded = json_string_decode_utf8(token, {body.token, sizeof(body.token)});
        if (!decoded.ok())
            return false;
        body.token_bytes = decoded.bytes;
    } else if (json_string_equals_ascii(name, utf8_literal("manual"))) {
        if (body.manual_seen || !json_value_to_bool(value, &body.manual))
            return false;
        body.manual_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("mention_count"))) {
        u64 value_count = 0;
        if (body.mention_seen || !json_value_to_u64(value, &value_count) || value_count > ~u32{0})
            return false;
        body.mention_seen = true;
        body.mention_count = static_cast<u32>(value_count);
    } else if (json_string_equals_ascii(name, utf8_literal("last_viewed")) ||
               json_string_equals_ascii(name, utf8_literal("flags"))) {
        u64 number = 0;
        if (!json_value_to_u64(value, &number) || number > ~u32{0})
            return false;
        const bool viewed = json_string_equals_ascii(name, utf8_literal("last_viewed"));
        bool& present = viewed ? body.viewed_seen : body.flags_seen;
        if (present)
            return false;
        present = true;
        if (viewed)
            body.last_viewed = static_cast<u32>(number);
        else
            body.flags = static_cast<u32>(number);
    }
    return true;
}
bool submitted_body(Body* body) noexcept {
    if (!body)
        return false;
    *body = {};
    u32 structurals[512];
    const Utf8View json{reinterpret_cast<const char*>(seen.body), seen.body_bytes};
    const auto scan = json_scan_structurals_avx1(json, {structurals, 512});
    return scan.ok() && json_visit_root_object(json, {structurals, scan.count}, 32, visit_body, body).ok();
}
RestClientSubmitResult WINAPI submit(void* instance, u64 channel_id, u64 message_id,
                                     ConstSlice<u8> body, u64 request_id) noexcept {
    ++seen.submissions;
    if (instance != &fixture.account_sentinel || !channel_id || !request_id || !body.data || !body.count ||
        body.count > sizeof(seen.body))
        return {RestClientSubmitError::invalid_input};
    seen.channel_id = channel_id;
    seen.message_id = message_id;
    seen.request_id = request_id;
    copy_bytes(seen.body, body.data, body.count);
    seen.body_bytes = body.count;
    const auto target =
        write_message_ack_target(channel_id, message_id, {seen.target, sizeof(seen.target)});
    if (!target.ok())
        return {RestClientSubmitError::target_capacity};
    seen.target_bytes = target.bytes;
    if (seen.admission == RestClientSubmitError::none)
        ++seen.accepted;
    return {seen.admission};
}
ChannelRecord& channel(u32 index = 0) noexcept {
    return fixture.channels[index];
}
void fresh_owner() noexcept {
    application_read_state_release(g_app);
    read_state_clear(&g_app.account_ui->read_state_store);
    g_app.account_ui->chat_state.self_user_id = 1;
    g_app.account_ui->chat_state.selected_channel_id = 70;
    g_app.account_ui->active_channel_id = 70;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.message_mutation_notice = {};
    g_app.plugin_account_scope_serial = 10;
    g_app.account.status.profile_id = 20;
    g_app.account.stop_failed = g_app.login.visible = false;
    for (auto& record : fixture.channels) {
        record.last_read_message_id = record.last_message_id;
        record.mention_count = 0;
        record.read_state_known = true;
        record.read_auto_ack_suppressed = false;
        record.read_mentions_incomplete = false;
    }
    message_store_initialize(&g_app.message_view.store, {fixture.records, 8}, {fixture.order, 8},
                             {fixture.text, sizeof(fixture.text)});
    ui_variable_list_initialize(&g_app.message_view.list, {fixture.heights, 8}, {fixture.fenwick, 8}, 0,
                                100);
    message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store,
                                &g_app.message_view.list, 100, 100, true);
    seen.admission = RestClientSubmitError::none;
}
bool mark(u32 index = 0, bool immediate = true) noexcept {
    ++channel(index).last_message_id;
    return application_read_state_mark(channel(index).id, immediate);
}
bool admit() noexcept {
    const u64 due = application_read_state_next_wake();
    if (!due)
        return false;
    return application_read_state_submit(++seen.next_request, maximum(clock_monotonic_milliseconds(), due));
}
bool response(u64 request, u16 status, Utf8View body = {}, u64 retry_at = 0,
              RestClientError error = RestClientError::none) noexcept {
    RestClientResponse completed{};
    completed.request_id = request;
    completed.status_code = status;
    completed.error = error;
    completed.body_bytes = body.bytes;
    completed.retry_at_ms = retry_at;
    return application_read_state_response(completed, reinterpret_cast<const u8*>(body.data), body.bytes,
                                           clock_monotonic_milliseconds());
}
bool reply(u16 status, Utf8View body = {}, u64 retry_at = 0,
           RestClientError error = RestClientError::none) noexcept {
    return response(seen.request_id, status, body, retry_at, error);
}
bool initialize() noexcept {
    seen.next_request = 100;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {&fixture.guild, 1}, {fixture.guild_order, 1},
                                        {fixture.channels, 4}, {fixture.channel_order, 4},
                                        {fixture.names, sizeof(fixture.names)}) ||
        !read_state_initialize(&ui.read_state_store, {fixture.pending, 4}) ||
        !read_state_bind(&ui.chat_state, &ui.read_state_store))
        return false;
    fixture.guild.id = 7;
    fixture.guild.owner_id = 1;
    fixture.guild.occupied = true;
    fixture.guild.permissions_resolved = true;
    fixture.guild.member_roles_known = true;
    fixture.guild.base_permissions = ~0ull;
    fixture.guild_order[0] = 0;
    ui.chat_state.guild_count = 1;
    for (u32 index = 0; index < 4; ++index) {
        auto& record = fixture.channels[index];
        record.id = 70 + index;
        record.guild_id = index == 2 ? 0 : 7;
        record.type = index == 1 ? 11 : index == 2 ? 1 : 0;
        record.parent_id = index == 1 ? 70 : 0;
        record.last_message_id = 700 + index * 100;
        record.last_read_message_id = record.last_message_id;
        record.occupied = true;
        record.permissions_resolved = true;
        record.effective_permissions = ~0ull;
        record.actions = channel_action_view | channel_action_read_history;
        record.thread_archived_known = index == 1;
        fixture.channel_order[index] = static_cast<u16>(index);
    }
    ui.chat_state.channel_count = 4;
    ui.chat_state.selected_guild_id = 7;
    g_app.account.instance = &fixture.account_sentinel;
    g_app.account.api.message_ack = submit;
    if (!message_composer_initialize(&g_app.message_composer, {fixture.draft, sizeof(fixture.draft)}) ||
        !message_composer_scene_initialize_owned(&g_app.message_composer_scene) ||
        !thread_member_store_initialize(&ui.thread_member_store, {fixture.thread_members[0], 4},
                                                {fixture.thread_members[1], 4}))
        return false;
    message_composer_reset(&g_app.message_composer, 70, true);
    g_app.message_pump.timeline = &g_app.message_view.timeline;
    g_app.message_pump.state = &ui.chat_state;
    message_event_pump_set_read_ack_cancel(&g_app.message_pump, application_read_state_cancel, nullptr);
    message_event_pump_set_read_ack_observer(&g_app.message_pump, application_read_state_ack_observed,
                                             nullptr);
    fresh_owner();
    return true;
}
void release() noexcept {
    application_read_state_release(g_app);
    message_composer_scene_release(&g_app.message_composer_scene);
    if (g_app.account_ui)
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
}
bool seed(u64 id, u64 channel_id = 70) noexcept {
    return message_store_upsert(&g_app.message_view.store,
                                {id, channel_id, 2, 1000, 0, utf8_literal("Retained authoritative source")})
        .ok();
}
bool ack_event(u64 cursor, bool manual, u64 channel_id = 70) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_ack;
    event.read_state.entry.channel_id = channel_id;
    event.read_state.entry.message_id = cursor;
    event.read_state.entry.fields = static_cast<u8>(
        gateway_read_state_cursor | gateway_read_state_mentions | (manual ? gateway_read_state_manual : 0));
    return message_event_pump_consume(&g_app.message_pump, event, {}).ok();
}
bool manual_body(Body& body) noexcept {
    return submitted_body(&body) && body.manual_seen && body.manual && !body.token_seen &&
           !body.viewed_seen && !body.flags_seen && !body.mention_seen;
}
void manual_tests() noexcept {
    Body body{};
    auto& state = g_app.account_ui->chat_state;
    fresh_owner();
    channel().last_message_id = 720;
    channel().last_read_message_id = 720;
    check(
        seed(700) && seed(710) && seed(720) &&
            message_composer_insert_utf8(&g_app.message_composer, utf8_literal("Retained unsent draft")) ==
                MessageComposerResult::changed,
        "manual read: authoritative fixture and unsent composer draft");
    channel().mention_count = 3;
    check(application_read_state_message_unread_available(720) &&
              application_read_state_mark_message_unread(720) && channel().last_read_message_id == 710 &&
              channel().read_auto_ack_suppressed && channel().read_mentions_incomplete &&
              channel().mention_count == 3 && !application_read_state_channel_unread_available(70) &&
              admit() && seen.message_id == 710 && manual_body(body),
          "manual read: cached predecessor preserves selected suppression and sends only manual with "
          "unknown count omitted");
    check(reply(200, utf8_literal(R"({"token":"manual-token-must-be-ignored"})")) &&
              channel().last_read_message_id == 710 && channel().read_auto_ack_suppressed &&
              equals(message_composer_text(g_app.message_composer).data,
                     message_composer_text(g_app.message_composer).bytes,
                     utf8_literal("Retained unsent draft")) &&
              application_read_state_observe(clock_monotonic_milliseconds()) &&
              !application_read_state_next_wake() && application_read_state_mark(70, true) && admit() &&
              submitted_body(&body) && body.token_null && !body.manual_seen && reply(204),
          "manual read: successful manual body never rotates token mutates draft or triggers selected "
          "automatic acknowledgement");

    fresh_owner();
    check(seed(1) && application_read_state_mark_message_unread(1) && channel().last_read_message_id == 0 &&
              admit() && seen.message_id == 0 && manual_body(body) &&
              equals(reinterpret_cast<const char*>(seen.target), seen.target_bytes,
                     utf8_literal("/api/v10/channels/70/messages/0/ack")) &&
              reply(204),
          "manual read: previous-millisecond clamp permits the exact zero boundary route");
    fresh_owner();
    constexpr u64 sparse = (500ull << 22) | 99;
    check(
        seed(sparse) && application_read_state_mark_message_unread(sparse) &&
            channel().last_read_message_id == (499ull << 22) && admit() &&
            seen.message_id == (499ull << 22) && reply(204),
        "manual read: absent cached predecessor uses previous millisecond rather than snowflake minus one");

    fresh_owner();
    channel().last_message_id = channel().last_read_message_id = 720;
    check(seed(700) && seed(710) && seed(720) && application_read_state_mark(70, true) && admit(),
          "manual read: old automatic flight fixture");
    const u64 automatic_request = seen.request_id;
    check(application_read_state_mark_message_unread(720) && ack_event(720, false) &&
              channel().last_read_message_id == 710 && channel().read_auto_ack_suppressed &&
              !application_read_state_next_wake() &&
              response(automatic_request, 200, utf8_literal(R"({"token":"automatic-owned"})")) && admit() &&
              seen.message_id == 710 && manual_body(body) && reply(204) &&
              application_read_state_mark(70, true) && admit() && submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("automatic-owned")) && reply(204),
          "manual read: accepted older ordinary Gateway echo cannot erase new local boundary and valid "
          "ordinary token rotation survives");

    for (u32 same = 0; same != 2; ++same) {
        fresh_owner();
        channel().last_message_id = channel().last_read_message_id = 720;
        check(seed(700) && seed(710) && seed(720) && application_read_state_mark_message_unread(710) &&
                  admit(),
              "manual read: owned manual echo fixture admits first action");
        const u64 first = seen.request_id;
        const u64 newer_message = same ? 710 : 720, desired = same ? 700 : 710;
        check(application_read_state_mark_message_unread(newer_message) && ack_event(700, true) &&
                  channel().last_read_message_id == desired && channel().read_auto_ack_suppressed &&
                  response(first, 204) && application_read_state_next_wake() && admit() &&
                  seen.message_id == desired && manual_body(body) && reply(204),
              "manual read: older owned manual echo preserves newer action including identical cursor "
              "repeats");
    }
    fresh_owner();
    check(seed(700) && seed(710) && application_read_state_mark_message_unread(710) &&
              ack_event(699, true) && channel().last_read_message_id == 699 &&
              !application_read_state_next_wake(),
          "manual read: unrelated remote manual authority cancels unsent local action");

    for (u32 fence = 0; fence != 4; ++fence) {
        fresh_owner();
        check(seed(700) && seed(710) && application_read_state_mark_message_unread(710) && admit(),
              "manual read: admitted account retirement fixture");
        const u64 retired = seen.request_id;
        switch (fence) {
        case 0:
            ++g_app.plugin_account_scope_serial;
            break;
        case 1:
            ++g_app.account.status.profile_id;
            break;
        case 2:
            ++state.self_user_id;
            break;
        case 3:
            read_state_clear(&g_app.account_ui->read_state_store);
            break;
        }
        channel().last_read_message_id = channel().last_message_id;
        check(response(retired, 200, utf8_literal(R"({"token":"retired-manual"})")) &&
                  channel().last_read_message_id == channel().last_message_id &&
                  !application_read_state_next_wake() && !application_read_state_channel_unread_retry_available(70),
              "manual read: old admitted scope or READY response cannot project cursor rotate token or "
              "expose retry");
    }

    fresh_owner();
    check(seed(700) && seed(710) && application_read_state_mark_message_unread(710),
          "manual read: navigation fixture captures authoritative boundary");
    state.selected_channel_id = g_app.account_ui->active_channel_id = 72;
    read_state_leave_channel(&state, 70);
    message_store_initialize(&g_app.message_view.store, {fixture.records, 8}, {fixture.order, 8},
                             {fixture.text, sizeof(fixture.text)});
    check(admit() && seen.channel_id == 70 && seen.message_id == 700 && reply(204) &&
              !channel().read_auto_ack_suppressed && channel().last_read_message_id == 700,
          "manual read: queued boundary survives expected source timeline retirement during navigation");

    for (u32 fence = 0; fence != 6; ++fence) {
        fresh_owner();
        check(seed(700) && seed(710) && application_read_state_mark_message_unread(710),
              "manual read: admission fence fixture");
        auto found = message_store_find(g_app.message_view.store, 710);
        const auto actions = channel().actions;
        const auto generation = channel().generation;
        switch (fence) {
        case 0:
            ++fixture.records[found.handle.slot].revision;
            break;
        case 1:
            message_store_delete(&g_app.message_view.store, 710);
            seed(710);
            break;
        case 2:
            message_store_delete(&g_app.message_view.store, 710);
            break;
        case 3:
            ++channel().generation;
            break;
        case 4:
            channel().actions &= ~channel_action_read_history;
            break;
        case 5:
            ++g_app.plugin_account_scope_serial;
            break;
        }
        const u32 calls = seen.accepted;
        check(!admit() && seen.accepted == calls && !application_read_state_next_wake(),
              "manual read: edited recycled deleted generation permission or account source cannot enter "
              "transport");
        channel().actions = actions;
        channel().generation = generation;
    }

    fresh_owner();
    channel().last_message_id = channel().last_read_message_id = 720;
    check(
        seed(700) && seed(710) && seed(720) && application_read_state_channel_unread_available(70) &&
            application_read_state_mark_channel_unread(70) && channel().last_read_message_id == 710 &&
            admit() && reply(403) && !application_read_state_channel_unread_available(70) &&
            application_read_state_channel_unread_retry_available(70) &&
            g_app.message_composer_scene.status_is_error &&
            g_app.message_composer_scene.typing_label_bytes &&
            equals(g_app.message_composer_scene.typing_label,
                   g_app.message_composer_scene.typing_label_bytes,
                   utf8_literal("Couldn't mark as unread. Try again.")),
        "manual read: terminal channel failure publishes actual composer error and exposes scoped retry "
        "despite optimistic unread");
    channel().last_message_id = 730;
    check(application_read_state_retry_channel_unread(70) &&
              !g_app.message_composer_scene.status_is_error &&
              !g_app.message_composer_scene.typing_label_bytes && admit() && seen.message_id == 710 &&
              reply(204),
          "manual read: explicit retry retains captured predecessor instead of marking newly arrived "
          "message");
    check(application_read_state_mark_message_unread(720) && admit() && reply(403) &&
              application_read_state_channel_unread_retry_available(70) && application_read_state_mark(72, true) &&
              !application_read_state_channel_unread_retry_available(70) &&
              !g_app.message_composer_scene.status_is_error,
          "manual read: later explicit read action expires terminal retry and clears composer feedback");

    fresh_owner();
    check(seed(700) && seed(710) && seed(720) && application_read_state_mark_message_unread(710) && admit(),
          "manual read: bounded retry fixture");
    u64 retry_at = clock_monotonic_milliseconds() + 5000;
    check(reply(429, {}, retry_at) && application_read_state_next_wake() == retry_at && admit(),
          "manual read: first rate limit retains deadline");
    retry_at += 5000;
    check(reply(429, {}, retry_at) && application_read_state_next_wake() == retry_at && admit(),
          "manual read: second rate limit retains deadline");
    retry_at += 5000;
    check(
        reply(429, {}, retry_at) && !application_read_state_next_wake() &&
            application_read_state_channel_unread_retry_available(70) && application_read_state_retry_channel_unread(70) &&
            application_read_state_next_wake() == retry_at &&
            !application_read_state_submit(++seen.next_request, retry_at - 1) && admit() &&
            seen.message_id == 700 && reply(204),
        "manual read: exhausted third attempt still fences fresh explicit retry with server bucket floor");
    fresh_owner();
    check(seed(700) && seed(710) && seed(720) && application_read_state_mark_message_unread(710) && admit(),
          "manual read: newer action behind terminal rate limit fixture");
    retry_at = clock_monotonic_milliseconds() + 5000;
    check(reply(429, {}, retry_at) && admit() && reply(429, {}, retry_at + 5000) && admit() &&
              application_read_state_mark_message_unread(720) && reply(429, {}, retry_at + 10000) &&
              application_read_state_next_wake() == retry_at + 10000 &&
              !application_read_state_submit(++seen.next_request, retry_at + 9999) && admit() &&
              seen.message_id == 710 && reply(204),
          "manual read: third superseded attempt cannot bypass bucket floor for newer boundary");

    fresh_owner();
    channel().flags |= 128;
    check(!application_read_state_channel_unread_available(70),
          "manual read: resource channel is ineligible");
    channel().flags &= ~128u;
    auto& thread = channel(1);
    thread.last_read_message_id = thread.last_message_id;
    thread.read_state_known = true;
    check(!application_read_state_channel_unread_available(thread.id),
          "manual read: unknown thread membership is ineligible");
    check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, thread.id, 1, false) &&
              chat_state_store_set_thread_self_membership(&g_app.account_ui->chat_state, thread.id, 1,
                                                             true) &&
              application_read_state_channel_unread_available(thread.id),
          "manual read: known joined active thread is eligible");
    check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, thread.id, 1, true),
          "manual read: authoritative archive transition is admitted");
    check(!application_read_state_channel_unread_available(thread.id),
          "manual read: archived thread awaits native unarchive prerequisite");
    fresh_owner();
    const u32 initial_bytes = application_read_state_owner_bytes();
    check(initial_bytes == 0 && mark() && application_read_state_owner_bytes() < 100000,
          "manual read: ordinary ACK demand backing excludes manual source and echo banks");
}
void run() noexcept {
    if (!initialize()) {
        check(false, "read runtime: fixture allocation and model binding");
        release();
        return;
    }
    Body body{};
    auto& state = g_app.account_ui->chat_state;
    const u64 old_cursor = channel().last_read_message_id;
    const u32 old_sync = seen.synchronized;
    check(!application_read_state_mark(0, true) && !application_read_state_mark(999, true) &&
              channel().last_read_message_id == old_cursor && !application_read_state_next_wake(),
          "read runtime: absent channel IDs cannot queue or mutate local read state");
    const u64 date_before = clock_unix_milliseconds();
    check(mark() && channel().last_read_message_id == channel().last_message_id &&
              channel().read_state_known && channel().read_flags_known && channel().read_flags == 1 &&
              channel().read_last_viewed_known && seen.synchronized == old_sync + 1 &&
              application_read_state_next_wake() && !seen.accepted,
          "read runtime: explicit mark stages an ACK before optimistic model projection and transport");
    browser_blocks_read = true;
    const u32 submissions_before_browser = seen.submissions;
    check(!application_read_state_next_wake() && !admit() && seen.submissions == submissions_before_browser,
          "read runtime: open thread browser suspends queued ACK admission and retry wake");
    browser_blocks_read = false;
    check(admit() && submitted_body(&body) && body.token_seen && body.token_null && body.viewed_seen &&
              body.flags_seen && body.flags == 1 && body.last_viewed == channel().read_last_viewed &&
              body.last_viewed >= (date_before - 1'420'070'400'000ull + 86'399'999ull) / 86'400'000ull &&
              equals(reinterpret_cast<const char*>(seen.target), seen.target_bytes,
                     utf8_literal("/api/v10/channels/70/messages/701/ack")) &&
              !application_read_state_next_wake(),
          "read runtime: first admitted POST has null token guild flags epoch-day date and exact cursor "
          "route");
    const u64 first_request = seen.request_id;
    const u64 first_message = seen.message_id;
    check(mark() && channel().last_message_id > first_message && !application_read_state_next_wake() &&
              !application_read_state_submit(++seen.next_request, clock_monotonic_milliseconds()),
          "read runtime: latest desired update cannot create a second inflight request");
    constexpr auto escaped = utf8_literal(R"({"token":"opaque\u0022\\\n\u00e9"})");
    application_read_state_ready(); // Unchanged generation, as after RESUMED.
    copy_bytes(fixture.response, escaped.data, escaped.bytes);
    check(response(first_request, 200, {reinterpret_cast<const char*>(fixture.response), escaped.bytes}) &&
              application_read_state_next_wake(),
          "read runtime: old completion retains newer queued desired cursor");
    clear_bytes(fixture.response, escaped.bytes);
    check(admit() && seen.message_id == channel().last_message_id && submitted_body(&body) &&
              !body.token_null &&
              equals(body.token, body.token_bytes, utf8_literal("opaque\"\\\n\xc3\xa9")),
          "read runtime: escaped opaque ACK token is decoded copied and safely encoded into the next body");
    check(response(first_request, 200, utf8_literal(R"({"token":"stale-duplicate"})")) &&
              reply(200, utf8_literal(R"({"token":""})")) && !application_read_state_next_wake() &&
              mark(1) && admit() && submitted_body(&body) && !body.token_null && !body.token_bytes &&
              body.flags == 2,
          "read runtime: stale duplicate cannot replace token and known empty token differs from null for "
          "thread ACK");
    check(reply(200, utf8_literal(R"({"token":null})")) && mark(2) && admit() && submitted_body(&body) &&
              body.token_null && body.flags_seen && body.flags == 0,
          "read runtime: explicit null token clears rotation and private-channel flags remain zero");
    check(reply(200, utf8_literal(R"({"token":"retained"})")) && mark() && admit() &&
              reply(200, utf8_literal("{}")) && mark() && admit() && submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("retained")),
          "read runtime: missing response token preserves the current opaque token");
    check(reply(200, utf8_literal(R"({"token":"bad","token":"duplicate"})")) && mark() && admit() &&
              submitted_body(&body) && equals(body.token, body.token_bytes, utf8_literal("retained")) &&
              reply(200, utf8_literal(R"({"token":12})")) && mark() && admit() && submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("retained")),
          "read runtime: malformed duplicate and nonstring tokens never replace retained token");
    check(reply(204) && mark() && admit() && submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("retained")) && reply(204),
          "read runtime: successful bodyless ACK leaves token unchanged");

    fresh_owner();
    check(mark() && admit(), "read runtime: manual cancellation fixture admits a request");
    GatewayClientEvent manual{};
    manual.kind = GatewayClientEventKind::dispatch;
    manual.dispatch_kind = GatewayClientDispatchKind::message_ack;
    manual.read_state.entry = {channel().id,
                               channel().last_message_id - 1,
                               1,
                               0,
                               0,
                               gateway_read_state_cursor | gateway_read_state_mentions |
                                   gateway_read_state_manual,
                               {}};
    const u64 cancelled_request = seen.request_id;
    check(message_event_pump_consume(&g_app.message_pump, manual, {}).ok() &&
              channel().read_auto_ack_suppressed &&
              channel().last_read_message_id == manual.read_state.entry.message_id &&
              application_read_state_mark(channel().id, true) &&
              response(cancelled_request, 200, utf8_literal(R"({"token":"cancel-rotation"})")) && admit() &&
              submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("cancel-rotation")) &&
              seen.message_id == channel().last_message_id && reply(204),
          "read runtime: manual pump cancellation protects fresh identical target while valid session "
          "token rotates");
    check(mark() && application_read_state_next_wake(),
          "read runtime: own-message cancellation starts with pending unsent work");
    state.selected_channel_id = 72;
    GatewayClientEvent own{};
    own.kind = GatewayClientEventKind::dispatch;
    own.dispatch_kind = GatewayClientDispatchKind::message_create;
    own.message.id = channel().last_message_id + 1;
    own.message.channel_id = channel().id;
    own.message.author_id = state.self_user_id;
    own.message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                         gateway_message_field_author_id | gateway_message_field_content;
    check(message_event_pump_consume(&g_app.message_pump, own, {}).ok() &&
              channel().last_message_id == own.message.id &&
              channel().last_read_message_id == own.message.id && !application_read_state_next_wake(),
          "read runtime: own-author pump callback cancels pending ACK even away from selected pane");
    state.selected_channel_id = 70;

    fresh_owner();
    check(mark() && admit(), "read runtime: READY reset fixture has an admitted old request");
    const u64 pre_ready_request = seen.request_id;
    read_state_clear(&g_app.account_ui->read_state_store);
    check(response(pre_ready_request, 200, utf8_literal(R"({"token":"old-ready-token"})")) &&
              !application_read_state_next_wake() && mark() && admit() && submitted_body(&body) &&
              body.token_null && reply(204),
          "read runtime: READY generation change before response automatically rejects old session token "
          "and flight");
    check(mark() && admit(), "read runtime: user identity fixture has an admitted old-user request");
    const u64 old_user_request = seen.request_id;
    state.self_user_id = 9;
    check(
        response(old_user_request, 200, utf8_literal(R"({"token":"other-user-token"})")) && mark() &&
            admit() && submitted_body(&body) && body.token_null && reply(204),
        "read runtime: actual current user identity rejects old captured user token before observer runs");
    state.self_user_id = 1;

    const struct {
        u16 status;
        u64 header_delay, expected_delay;
        RestClientError error;
    } retry_cases[]{
        {503, 5000, 5000, RestClientError::none},
        {503, 0, 1000, RestClientError::none},
        {503, 250, 1000, RestClientError::none},
        {503, 5000, 1000, RestClientError::receive},
        {429, 250, 250, RestClientError::none},
        {204, 5000, 0, RestClientError::none},
        {403, 5000, 0, RestClientError::none},
    };
    bool retry_metadata = true;
    for (const auto& item : retry_cases) {
        fresh_owner();
        retry_metadata = mark() && admit() && retry_metadata;
        const u64 now = clock_monotonic_milliseconds();
        RestClientResponse completed{};
        completed.request_id = seen.request_id;
        completed.status_code = item.status;
        completed.error = item.error;
        completed.retry_at_ms = now + item.header_delay;
        retry_metadata = application_read_state_response(completed, nullptr, 0, now) && retry_metadata;
        const u64 due = item.expected_delay ? now + item.expected_delay : 0;
        retry_metadata = application_read_state_next_wake() == due && retry_metadata;
        const u32 calls = seen.submissions;
        if (due)
            retry_metadata = !application_read_state_submit(++seen.next_request, due - 1) &&
                             seen.submissions == calls &&
                             application_read_state_submit(++seen.next_request, due) && reply(204) &&
                             retry_metadata;
        else
            retry_metadata = !application_read_state_submit(++seen.next_request, now + 10000) &&
                             seen.submissions == calls && retry_metadata;
    }
    check(retry_metadata,
          "read runtime: trusted non429 retry metadata extends eligible ACK backoff without shortening it, "
          "early submission, transport-error hints or new success/terminal retries; 429 stays exact");

    fresh_owner();
    check(mark() && admit(), "read runtime: long 429 fixture admits work");
    const u64 long_retry = clock_monotonic_milliseconds() + 6ull * 60 * 60 * 1000;
    check(reply(429, {}, long_retry) && application_read_state_next_wake() == long_retry,
          "read runtime: long numeric Retry-After reaches application wake without shortening");
    const u32 calls_before_wait = seen.submissions;
    check(!application_read_state_submit(++seen.next_request, long_retry - 1) &&
              seen.submissions == calls_before_wait && mark() &&
              application_read_state_next_wake() == long_retry &&
              application_read_state_submit(++seen.next_request, long_retry) && reply(204),
          "read runtime: retry blocks early transport and newer immediate targets until authoritative "
          "deadline");
    fresh_owner();
    check(mark(), "read runtime: busy transport fixture queues desired target");
    seen.admission = RestClientSubmitError::busy;
    const u64 attempt_at = maximum(clock_monotonic_milliseconds(), application_read_state_next_wake());
    check(!application_read_state_submit(++seen.next_request, attempt_at) &&
              application_read_state_next_wake() == attempt_at + 100,
          "read runtime: busy admission retains intent with a 100ms nonspinning wake");
    const u32 busy_calls = seen.submissions;
    seen.admission = RestClientSubmitError::none;
    check(!application_read_state_submit(++seen.next_request, attempt_at + 99) &&
              seen.submissions == busy_calls &&
              application_read_state_submit(++seen.next_request, attempt_at + 100) && reply(403) &&
              !application_read_state_next_wake(),
          "read runtime: busy backoff admits once due and terminal 4xx removes exact desired target");
    check(mark() && admit() && mark() && reply(404) && application_read_state_next_wake() && admit() &&
              reply(204),
          "read runtime: old terminal response cannot drop newer queued cursor");
    check(mark(), "read runtime: nonbusy admission failure fixture queues work");
    seen.admission = RestClientSubmitError::body_capacity;
    check(!admit() && !application_read_state_next_wake(),
          "read runtime: terminal admission error cancels intent instead of a tight retry loop");
    seen.admission = RestClientSubmitError::none;

    fresh_owner();
    check(mark() && admit() && reply(200, utf8_literal(R"({"token":"safe"})")) && mark() && admit(),
          "read runtime: oversized response fixture begins with a retained known token");
    ProtocolWriter writer{{fixture.response, sizeof(fixture.response)}, 0};
    bool built = protocol_writer_append_literal(&writer, "{\"token\":\"");
    for (u32 i = 0; i < kMessageAckMaximumBodyBytes + 1; ++i)
        built = protocol_writer_append_byte(&writer, 'x') && built;
    built = protocol_writer_append_literal(&writer, "\"}") && built;
    check(built && reply(200, {reinterpret_cast<const char*>(fixture.response), writer.used}) && mark() &&
              admit() && submitted_body(&body) &&
              equals(body.token, body.token_bytes, utf8_literal("safe")) && reply(204),
          "read runtime: oversized opaque token remains diagnostic and preserves prior token");
    check(mark() && admit(), "read runtime: serialized body capacity fixture admits work");
    writer = {{fixture.response, sizeof(fixture.response)}, 0};
    built = protocol_writer_append_literal(&writer, "{\"token\":\"");
    for (u32 i = 0; i < kMessageAckMaximumBodyBytes - 26; ++i)
        built = protocol_writer_append_byte(&writer, 'x') && built;
    built = protocol_writer_append_literal(&writer, "\"}") && built;
    const u32 before_large_body = seen.submissions;
    check(built && reply(200, {reinterpret_cast<const char*>(fixture.response), writer.used}) && mark() &&
              !admit() && seen.submissions == before_large_body && !application_read_state_next_wake(),
          "read runtime: token at raw capacity that overflows serialized body is diagnosed without "
          "transport or spin");

    fresh_owner();
    const u64 history_cursor = channel().last_message_id + 2;
    check(chat_state_store_note_history_latest(&state, channel().id, history_cursor) &&
              channel().last_read_message_id < history_cursor &&
              application_read_state_mark(channel().id, true) && admit() &&
              seen.message_id == history_cursor && channel().last_read_message_id == history_cursor &&
              reply(204),
          "read runtime: reconciled authoritative history supplies the latest ACK target");
    fresh_owner();
    check(mark() && admit(), "read runtime: token reset admits prior session work");
    application_read_state_reset_token(nullptr);
    check(reply(200, utf8_literal(R"({"token":"before-user-update"})")) && mark() && admit() &&
              submitted_body(&body) && body.token_null && reply(204),
          "read runtime: current-user token reset ignores the captured old-token response without losing "
          "work");
    fresh_owner();
    check(mark() && application_read_state_next_wake(),
          "read runtime: covered forum fixture retains a queued read intent");
    ForumPostScene cover{};
    ForumPostSurface surface{};
    cover.active = true;
    surface.scene = &cover;
    auto* previous_forum = g_app.renderer.forum_form;
    g_app.renderer.forum_form = &surface;
    const u32 before_covered_submissions = seen.submissions;
    check(!application_read_state_eligible() && !application_read_state_next_wake() && !admit() &&
              seen.submissions == before_covered_submissions,
          "read runtime: bound forum lifetime covers automatic and queued reads without host freshness");
    cover.active = false;
    check(application_read_state_next_wake() && admit() && reply(204),
          "read runtime: closing forum restores the retained intent without a polling wake");
    g_app.renderer.forum_form = previous_forum;
    fresh_owner();
    const u64 before_observe = channel().last_read_message_id;
    ++channel().last_message_id;
    g_app.message_view.timeline.follow_bottom = true;
    g_app.message_catchup.state = MessageHistoryCatchupState::complete;
    application_read_state_activity();
    check(!application_read_state_eligible() &&
              application_read_state_observe(clock_monotonic_milliseconds()) &&
              channel().last_read_message_id == before_observe && !application_read_state_next_wake(),
          "read runtime: no owned foreground window cannot fabricate an automatic read");
    g_app.message_catchup.state = MessageHistoryCatchupState::idle;
    check(!application_read_state_eligible(),
          "read runtime: noncurrent history remains ineligible for automatic read");
    fresh_owner();
    ReadScopeTarget invalid_plan[2]{};
    invalid_plan[0].channel_id = invalid_plan[1].channel_id = channel().id;
    invalid_plan[0].generation = invalid_plan[1].generation = channel().generation;
    invalid_plan[0].message_id = invalid_plan[1].message_id = channel().last_message_id;
    invalid_plan[0].outbound = invalid_plan[1].outbound = true;
    const u64 before_plan_cursor = channel().last_read_message_id;
    u64 first_serial = 999;
    check(!application_read_state_scoped_prepare({invalid_plan, 2}, true, &first_serial) &&
              first_serial == 999 && channel().last_read_message_id == before_plan_cursor &&
              !application_read_state_next_wake() && !application_read_state_metrics().action_serial,
          "read runtime: duplicate whole scoped plan fails before cursor queue or authority mutation");
    manual_tests();
    release();
}
} // namespace maicord::read_state_runtime_test

namespace maicord::app_detail {
bool application_thread_browser_read_blocked() noexcept {
    return read_state_runtime_test::browser_blocks_read;
}
bool application_search_read_blocked() noexcept {
    return false;
}
bool application_pinned_messages_read_blocked() noexcept {
    return false;
}
bool synchronize_navigation_view() noexcept {
    ++read_state_runtime_test::seen.synchronized;
    return true;
}
bool refresh_message_composer_ui() noexcept {
    const auto notice = application_message_mutation_notice_text();
    return message_composer_scene_update(&g_app.message_composer_scene, &g_app.message_composer,
                                         {0, 0, 600, 120}, true, 0, {}, nullptr, notice, {}, false, true,
                                         true, application_message_mutation_notice_is_error())
        .ok();
}
} // namespace maicord::app_detail

extern "C" __declspec(noreturn) void WINAPI maicord_read_state_runtime_entry() noexcept {
    maicord::read_state_runtime_test::run();
    const bool failed = maicord::read_state_runtime_test::seen.failures != 0;
    constexpr char pass[] = "PASS read state runtime\n", fail[] = "FAIL read state runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}

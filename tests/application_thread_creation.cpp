// The shipping controller, actual resident message store and Core permissions,
// with an offline typed exact-ID transport and a native-form boundary stub.
#include "../src/app/application_thread_creation.hpp"
#include "../src/app/application_state.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"
namespace maicord::creation_test { u64 now = 1000; }
namespace maicord { static u64 creation_fixture_now() noexcept { return creation_test::now; } }
#define clock_monotonic_milliseconds creation_fixture_now
#include "../src/app/application_thread_creation.cpp"
#undef clock_monotonic_milliseconds

namespace maicord::creation_test {
using namespace app_detail;
u32 failures{}, submissions{}, navigations{};
u64 accepted{}, cancelled{}, navigated{};
u8 actor{}, replacement_actor{};
RestClientSubmitError admission{};
ThreadCreationRequest submitted{};
char submitted_name[400]{};
char body[2048]{};
u32 body_bytes{};
ApplicationTextFormRequest form{};
bool form_open_ok = true;
GuildRecord guilds[2]{};
u16 guild_order[2]{};
ChannelRecord channels[64]{};
u16 channel_order[64]{};
u8 names[16384]{};
RoleRecord roles[4]{};
PermissionOverwriteRecord overwrites[4]{};
MessageRecord messages[32]{};
u32 message_order[32]{};
u8 message_text[16384]{};
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
ChannelRecord* row(u64 id) noexcept {
    return const_cast<ChannelRecord*>(chat_state_store_find_channel(g_app.account_ui->chat_state, id));
}
MessageInput source(u64 id, u32 flags = 0, Utf8View content = utf8_literal("Source message")) noexcept {
    return {id, 70, 3, 100, 0, content, flags};
}
bool put(u64 id, u32 flags = 0, Utf8View content = utf8_literal("Source message")) noexcept {
    return message_store_upsert(&g_app.message_view.store, source(id, flags, content)).ok();
}
void select_parent() noexcept {
    g_app.account_ui->active_channel_id = 70;
    g_app.account_ui->chat_state.selected_channel_id = 70;
    g_app.account_ui->chat_state.selected_guild_id = 7;
}
void permit(u64 bits = kPermissionViewChannel | kPermissionReadMessageHistory |
                           kPermissionCreatePublicThreads) noexcept {
    auto& state = g_app.account_ui->chat_state;
    chat_state_store_upsert_role(&state, {7, 7, bits, utf8_literal("Everyone")});
    chat_state_store_set_self_roles(&state, 7, 99, 5, {});
    chat_state_store_recompute_guild_permissions(&state, 7);
}
RestClientSubmitResult WINAPI submit(void* instance, const ThreadCreationRequest* request, u64 id) noexcept {
    if (instance != &actor || !id) return {RestClientSubmitError::invalid_input};
    if (!request) { cancelled = id; return {}; }
    ++submissions;
    if (admission != RestClientSubmitError::none) return {admission};
    if (accepted) return {RestClientSubmitError::busy};
    if (!thread_creation_request_valid(*request)) return {RestClientSubmitError::invalid_input};
    accepted = id; submitted = *request;
    CopyMemory(submitted_name, request->name.data, request->name.bytes);
    submitted.name = {submitted_name, request->name.bytes};
    return {};
}
void append(const char* value) noexcept { for (u32 i = 0; value[i]; ++i) body[body_bytes++] = value[i]; }
void number(u64 value) noexcept {
    char digits[20]; u32 count{};
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) body[body_bytes++] = digits[--count];
}
Utf8View payload(u64 id, bool member = false, u8 type = 11) noexcept {
    body_bytes = 0; append("{\"id\":\""); number(id);
    append("\",\"parent_id\":\"70\",\"guild_id\":\"7\",\"type\":"); number(type);
    append(",\"name\":\"Server name\",\"thread_metadata\":{\"archived\":false,\"locked\":false,\"auto_archive_duration\":4320,\"archive_timestamp\":\"2026-10-05T12:00:00Z\"}");
    if (member) {
        append(",\"member\":{\"id\":\""); number(id);
        append("\",\"user_id\":\"5\",\"join_timestamp\":\"2026-10-05T12:00:00Z\",\"flags\":0}");
    }
    append("}"); return {body, body_bytes};
}
bool receipt(u64 id, u16 status, Utf8View value = {}, u64 retry = 0,
             RestClientError error = {}, u32 code = 0) noexcept {
    RestClientResponse response{};
    response.request_id = id; response.status_code = status; response.body_bytes = value.bytes;
    response.error = error; response.rate_limited = status == 429; response.retry_at_ms = retry;
    response.rejection.has_code = code != 0; response.rejection.code = code;
    if (id == accepted) accepted = 0;
    return application_thread_creation_response(response, value);
}
ApplicationThreadCreationTicket begin(u64 id) noexcept {
    select_parent(); put(id);
    ApplicationThreadCreationTicket ticket{};
    check(application_thread_creation_begin(id, &ticket), "creation: fresh resident intent begins");
    return ticket;
}
bool start(const ApplicationThreadCreationTicket& ticket, u64 flight) noexcept {
    return application_thread_creation_name_submit(ticket, utf8_literal("Captured name")) &&
        application_thread_creation_submit(flight, now);
}
void reset_owner() noexcept {
    if (accepted) receipt(accepted, 500);
    application_thread_creation_release();
    g_app.account.instance = &actor; g_app.account.status.profile_id = 2;
    g_app.account.status.running = true; g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.api.thread_creation = submit; admission = {}; form_open_ok = true;
    select_parent(); permit();
}
bool gateway_thread(u64 id, Utf8View name = utf8_literal("Gateway name"), bool complete = true) noexcept {
    ChannelInput input{id, 7, 70, name, 0, 0, 11};
    input.thread_archived_known = input.thread_locked_known = complete;
    input.thread_active_known = true; input.thread_listed_active = true;
    return chat_state_store_upsert_channel(&g_app.account_ui->chat_state, input).ok();
}
bool initialize() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui) return false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 64}, {channel_order, 64}, {names, sizeof(names)}) ||
        !chat_state_store_bind_permissions(&state, {roles, 4}, {overwrites, 4}) ||
        !chat_state_store_upsert_guild(&state, {7, utf8_literal("Guild"), GuildMutationKind::create, 99}).ok() ||
        !chat_state_store_upsert_channel(&state, {70, 7, 0, utf8_literal("Parent"), 0, 0, 0}).ok() ||
        !message_store_initialize(&g_app.message_view.store, {messages, 32}, {message_order, 32}, {message_text, sizeof(message_text)})) return false;
    state.self_user_id = 5;
    g_app.plugin_account_scope_serial = 3;
    g_app.account.instance = &actor; g_app.account.status.profile_id = 2; g_app.account.status.running = true;
    g_app.account.api.thread_creation = submit; select_parent(); permit();
    return put(900);
}
void run() noexcept {
    check(initialize(), "creation: actual Core/store initialize captured profile2");
    if (!g_app.account_ui) return;
    check(!creation_owner && application_thread_creation_available(900) &&
        (row(70)->actions & channel_action_create_public_threads) &&
        !(row(70)->actions & channel_action_send_messages), "creation: public create does not require SEND or allocate owner");
    permit(kPermissionViewChannel | kPermissionCreatePublicThreads);
    check(!application_thread_creation_available(900) && !creation_owner, "creation: CREATE_PUBLIC without READ_HISTORY is denied");
    permit(); put(900, has_thread);
    check(!application_thread_creation_available(900), "creation: HAS_THREAD cannot duplicate source");
    auto input = source(900); input.delivery = MessageDeliveryState::pending;
    message_store_upsert(&g_app.message_view.store, input);
    check(!application_thread_creation_available(900), "creation: optimistic source is ineligible");
    input.delivery = MessageDeliveryState::authoritative; input.type = 6;
    message_store_upsert(&g_app.message_view.store, input);
    check(!application_thread_creation_available(900), "creation: system source is ineligible");
    auto ticket = begin(900);
    check(!application_thread_creation_name_submit(ticket, utf8_literal(" \t")) && !submissions &&
        application_thread_creation_view().can_submit, "creation: invalid blank name is local and editable");
    char unicode[404];
    for (u32 i = 0; i < 101; ++i) { unicode[i * 4] = '\xf0'; unicode[i * 4 + 1] = '\x9f'; unicode[i * 4 + 2] = '\x98'; unicode[i * 4 + 3] = '\x80'; }
    check(!application_thread_creation_name_submit(ticket, {unicode, 404}) && !submissions,
        "creation: 101 supplementary scalars exceed both native bounds");
    check(application_thread_creation_name_submit(ticket, {unicode, 400}) && application_thread_creation_next_wake() == 1 &&
        application_thread_creation_submit(10, now) && submitted.name.bytes == 400 && submitted.auto_archive_duration == 4320 &&
        submitted.operation == Operation::from_message && submitted.thread_type == 11 && submitted.source_message_id == 900,
        "creation: 100 supplementary scalars copied into immutable exact public flight; initial wake nonzero");
    unicode[0] = 'x';
    check(submitted.name.data[0] == '\xf0' && !receipt(999, 200, payload(900)) && creation_owner->flight.id == 10,
        "creation: borrowed caller edits and unrelated receipt cannot change/drain flight");
    const auto nav_before = navigations;
    check(receipt(10, 200, payload(900)) && row(900) && row(900)->thread_active_known && row(900)->thread_listed_active &&
        !row(900)->thread_self_joined_known && navigations == nav_before + 1 && navigated == 900,
        "creation: strict accepted receipt publishes active proof, never invents self membership, normal navigation boundary");
    reset_owner();
    ticket = begin(901);
    check(start(ticket, 11) && receipt(11, 200, payload(901, true)) && row(901)->thread_self_joined_known && row(901)->thread_self_joined,
        "creation: only genuine embedded current-user membership publishes joined proof");
    reset_owner(); ticket = begin(902); check(start(ticket, 20), "creation: ambiguous POST starts");
    check(receipt(20, 500) && application_thread_creation_view().uncertain && application_thread_creation_view().can_retry &&
        !application_thread_creation_pending(now + 100000), "creation: ambiguous POST requires explicit Check again");
    check(application_thread_creation_retry(ticket) && application_thread_creation_next_wake() == 1 &&
        application_thread_creation_submit(21, now) && submitted.operation == Operation::reconcile && submitted.source_message_id == 902 &&
        utf8_equal(submitted.name, utf8_literal("Captured name")) && receipt(21, 200, payload(902)) && row(902),
        "creation: explicit retry checks exact expected thread with unchanged intent before any repeat POST");
    reset_owner(); ticket = begin(903); check(start(ticket, 30) && receipt(30, 500) &&
        application_thread_creation_retry(ticket) && application_thread_creation_submit(31, now), "creation: missing reconcile fixture");
    check(receipt(31, 404) && application_thread_creation_next_wake() == 1 && application_thread_creation_submit(32, now) &&
        submitted.operation == Operation::from_message && receipt(32, 429, {}, now + 500),
        "creation: exact unchanged GET404 allows one same-intent POST; rate limit then stops automatic work");
    check(!application_thread_creation_pending(now + 10000) && application_thread_creation_retry(ticket) &&
        !application_thread_creation_pending(now + 499) && application_thread_creation_pending(now + 500),
        "creation: retry is explicit and respects actor floor");
    now += 500; reset_owner(); ticket = begin(904); check(start(ticket, 40), "creation: cancel floor fixture starts");
    const auto cancel_nav = navigations;
    check(application_thread_creation_cancel(ticket) && cancelled == 40 && creation_owner->flight.retired &&
        !application_thread_creation_pending(now), "creation: cancellation retires exact accepted flight without lane reuse");
    auto replacement = begin(905);
    check(application_thread_creation_name_submit(replacement, utf8_literal("Latest choice")) && !application_thread_creation_pending(now) &&
        !application_thread_creation_submit(41, now) && creation_owner->flight.id == 40 &&
        creation_owner->flight.source.message == 904 &&
        utf8_equal(creation_owner->flight.request.name, utf8_literal("Captured name")) &&
        utf8_equal(application_thread_creation_view().name, utf8_literal("Latest choice")),
        "creation: latest intent waits for older immutable receipt drainage");
    check(receipt(40, 429, {}, now + 700) && !row(904) && navigations == cancel_nav &&
        application_thread_creation_next_wake() == now + 700 && !application_thread_creation_pending(now + 699),
        "creation: late cancelled429 survives supersession and publishes no thread/navigation");
    now += 700; check(application_thread_creation_submit(41, now) && submitted.source_message_id == 905 &&
        receipt(41, 500), "creation: drained latest intent gets exact new flight");
    reset_owner(); ticket = begin(906); check(start(ticket, 50), "creation: cancelled uncertain fixture starts");
    application_thread_creation_cancel(ticket); check(receipt(50, 200, payload(906)) && !row(906),
        "creation: late cancelled success cannot hydrate or navigate");
    ticket = begin(906);
    check(application_thread_creation_name_submit(ticket, utf8_literal("Changed local choice")) &&
        application_thread_creation_submit(51, now) && submitted.operation == Operation::reconcile &&
        receipt(51, 200, payload(906)), "creation: transmitted cancellation retains per-source uncertainty across reopen");
    reset_owner(); ticket = begin(907); check(start(ticket, 60), "creation: Gateway progress fixture starts");
    check(put(907, has_thread) && gateway_thread(907), "creation: actual canonical Gateway model publication");
    application_thread_creation_gateway(7, 70, 907);
    const auto gateway_nav = navigations;
    check(creation_owner->phase == Phase::settled && creation_owner->flight.active && creation_owner->flight.retired &&
        receipt(60, 500) && navigations == gateway_nav &&
        utf8_equal(chat_state_store_channel_name(g_app.account_ui->chat_state, *row(907)), utf8_literal("Gateway name")),
        "creation: Gateway+HAS_THREAD settles before HTTP; failed stale receipt never rolls it back");
    reset_owner(); ticket = begin(908); check(start(ticket, 61) && gateway_thread(908, utf8_literal("Incomplete"), false),
        "creation: incomplete Gateway fixture starts");
    application_thread_creation_gateway(7, 70, 908);
    check(creation_owner->phase == Phase::creating && receipt(61, 200, payload(908)) &&
        application_thread_creation_view().uncertain && !row(908)->thread_archived_known,
        "creation: incomplete newer Gateway metadata forces read reconciliation and wins stale HTTP");
    reset_owner(); ticket = begin(909); check(start(ticket, 62), "creation: deleted Gateway fixture starts");
    gateway_thread(909); chat_state_store_remove_channel(&g_app.account_ui->chat_state, 909);
    application_thread_creation_gateway(7, 70, 909);
    check(receipt(62, 200, payload(909)) && !row(909) && application_thread_creation_view().uncertain,
        "creation: Gateway create-delete batch cannot be resurrected by HTTP");
    reset_owner(); ticket = begin(910); check(start(ticket, 70), "creation: permission withdrawal starts");
    permit(kPermissionViewChannel | kPermissionReadMessageHistory);
    application_thread_creation_synchronize();
    check(creation_owner->retired && cancelled == 70 && receipt(70, 200, payload(910)) && !row(910),
        "creation: fresh CREATE_PUBLIC withdrawal retires publication before receipt");
    reset_owner(); ticket = begin(911); check(start(ticket, 71), "creation: profile replacement starts");
    g_app.account.status.profile_id = 9; application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(71, 200, payload(911)) && !row(911), "creation: captured profile2 cannot publish under profile9");
    reset_owner(); ticket = begin(912); check(start(ticket, 72), "creation: actor replacement starts");
    g_app.account.instance = &replacement_actor; application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(72, 200, payload(912)) && !row(912), "creation: changed account instance retires accepted receipt");
    reset_owner(); ticket = begin(913); check(start(ticket, 73), "creation: timestamp association fixture starts");
    input = source(913, has_thread); input.timestamp_ms = 101; message_store_upsert(&g_app.message_view.store, input);
    application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(73, 200, payload(913)) && !row(913), "creation: HAS_THREAD plus changed canonical timestamp is not association-only");
    reset_owner(); ticket = begin(914); check(start(ticket, 74), "creation: content availability fixture starts");
    input = source(914, has_thread); input.content_availability = MessageContentAvailability::restricted;
    message_store_upsert(&g_app.message_view.store, input); application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(74, 200, payload(914)) && !row(914), "creation: changed source availability retires receipt");
    reset_owner(); ticket = begin(915); check(start(ticket, 75), "creation: source deletion fixture starts");
    message_store_delete(&g_app.message_view.store, 915); put(915); application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(75, 200, payload(915)) && !row(915), "creation: reused source ID cannot replace captured MessageHandle generation");
    reset_owner(); ticket = begin(916); check(start(ticket, 76), "creation: parent generation fixture starts");
    ++row(70)->generation; application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(76, 200, payload(916)) && !row(916), "creation: changed parent generation retires captured authority");
    reset_owner(); ticket = begin(917); check(application_thread_creation_name_submit(ticket, utf8_literal("Busy name")), "creation: busy intent queued");
    admission = RestClientSubmitError::busy;
    for (u32 i = 0; i < 21; ++i) { application_thread_creation_submit(100 + i, now); now += 50; }
    check(creation_owner->phase == Phase::failed && !creation_owner->flight.active && !application_thread_creation_next_wake(),
        "creation: busy deferral is bounded and requires explicit retry");
    admission = {}; check(application_thread_creation_retry(ticket) && application_thread_creation_submit(80, now) &&
        receipt(80, 401) && !application_thread_creation_view().can_retry && !application_thread_creation_pending(now + 100000),
        "creation: expired session is terminal and does not automatically loop");
    reset_owner(); ticket = begin(918); check(start(ticket, 81) && receipt(81, 400, {}, 0, {}, 50035) &&
        application_thread_creation_view().can_submit && !application_thread_creation_view().uncertain,
        "creation: definitive invalid-form rejection allows corrected name without uncertainty");
    reset_owner(); ticket = begin(920); check(start(ticket, 83) && receipt(83, 400, {}, 0, {}, 160004) &&
        application_thread_creation_view().uncertain && !application_thread_creation_next_wake() &&
        application_thread_creation_retry(ticket) && application_thread_creation_submit(84, now) &&
        submitted.operation == Operation::reconcile && receipt(84, 200, payload(920)),
        "creation: official already-created rejection offers explicit expected-ID read, never blind repeat POST");
    reset_owner(); row(70)->type = 5; permit(); ticket = begin(921);
    check(start(ticket, 85) && submitted.thread_type == 10 && receipt(85, 200, payload(921, false, 10)) && row(921)->type == 10,
        "creation: announcement parent requests and publishes exact announcement-thread type10");
    row(70)->type = 0;
    reset_owner(); ticket = begin(918); check(start(ticket, 86), "creation: unrelated source edit fixture starts");
    put(918, has_thread, utf8_literal("Changed content")); application_thread_creation_synchronize();
    check(creation_owner->retired && receipt(86, 200, payload(918)) && !row(918),
        "creation: HAS_THREAD plus edited content cannot masquerade as canonical association");
    reset_owner(); ticket = begin(918);
    check(application_thread_creation_name_submit(ticket, utf8_literal("Account action")), "creation: action-required intent queued");
    g_app.account.status.required_action = AccountRequirement::captcha; application_thread_creation_synchronize();
    check(application_thread_creation_view().phase == Phase::failed && !application_thread_creation_view().can_retry &&
        !application_thread_creation_next_wake() && !application_thread_creation_pending(now + 100000),
        "creation: new captcha/account action blocks queued mutation without automatic loop");
    reset_owner(); ticket = begin(918); g_app.account.status.running = false;
    check(!application_thread_creation_name_submit(ticket, utf8_literal("Reconnect name")) &&
        application_thread_creation_view().error && application_thread_creation_view().can_retry && !application_thread_creation_next_wake(),
        "creation: offline submit gives explicit reconnect feedback without fake loading");
    g_app.account.status.running = true;
    check(application_thread_creation_retry(ticket) && application_thread_creation_submit(87, now) && receipt(87, 403) &&
        !application_thread_creation_view().can_retry, "creation: reconnected explicit retry retains name and terminal permission failure");
    reset_owner(); select_parent(); put(919); form_open_ok = false;
    check(!application_thread_creation_open(919) && creation_owner->retired && !g_app.text_form,
        "creation: native form allocation failure retires prepared source without callback");
    form_open_ok = true;
    check(application_thread_creation_open(919) && form.single_line && form.maximum_bytes == 400 && form.maximum_codepoints == 100 &&
        form.progress && form.closed && form.valid(form.owner), "creation: production bridge opens bounded async native name form");
    chat_state_store_upsert_channel(&g_app.account_ui->chat_state, {70, 7, 0, utf8_literal("Renamed parent"), 0, 0, 0}); permit();
    check(utf8_equal(form.context, utf8_literal("Parent")) && form.context.data == creation_owner->parent_name && form.valid(form.owner),
        "creation: native form keeps owned captured parent label through mutable Core rename");
    check(!form.save(form.owner, utf8_literal("Form name")) && !form.progress(form.owner).editable &&
        application_thread_creation_submit(82, now), "creation: asynchronous form save freezes name while exact flight runs");
    auto close = form.closed; auto key = form.owner; g_app.text_form = nullptr; close(key);
    check(creation_owner->retired && cancelled == 82 && receipt(82, 200, payload(919)) && !row(919),
        "creation: native form closure cancels but drains accepted work and never late-navigates");
    reset_owner();
    for (u64 id = 10000; id < 10009; ++id) {
        ticket = begin(id); check(start(ticket, id) && application_thread_creation_cancel(ticket) && receipt(id, 500),
            "creation: uncertainty ledger bounded receipt fixture");
    }
    ticket = begin(10009);
    check(creation_owner->reconcile_all && application_thread_creation_name_submit(ticket, utf8_literal("Overflow conservative")) &&
        application_thread_creation_submit(90, now) && submitted.operation == Operation::reconcile,
        "creation: eight-entry uncertainty overflow conservatively requires GET for later sources");
    receipt(90, 404); application_thread_creation_release();
    check(!creation_owner && application_thread_creation_metrics().owner_bytes == 0 && !g_app.text_form,
        "creation: idle teardown frees lazy bounded owner without native-form lifetime leak");
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr;
}
}
namespace maicord::app_detail {
Application g_app{};
bool application_request_frame() noexcept { return true; }
bool application_settings_active() noexcept { return false; }
bool application_friends_active() noexcept { return false; }
bool application_thread_create_cooldown_active(u64) noexcept { return false; }
void application_thread_create_note_created(u64) noexcept {}
bool application_thread_browser_active() noexcept { return false; }
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    if (!creation_test::form_open_ok || g_app.text_form || !request.valid(request.owner)) return false;
    creation_test::form = request;
    g_app.text_form = reinterpret_cast<ApplicationTextForm*>(&creation_test::form);
    return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid valid) noexcept {
    if (!g_app.text_form || creation_test::form.valid != valid) return;
    const auto form = creation_test::form; g_app.text_form = nullptr;
    if (form.closed) form.closed(form.owner);
}
bool application_navigate_known_message(u64 id, u64 message) noexcept {
    const auto* thread = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
    if (message || !thread || !chat_state_store_thread_accessible(g_app.account_ui->chat_state, *thread)) return false;
    ++creation_test::navigations; creation_test::navigated = id;
    g_app.account_ui->active_channel_id = id;
    g_app.account_ui->chat_state.selected_channel_id = id;
    return true;
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_application_thread_creation_entry() noexcept {
    maicord::creation_test::run();
    ExitProcess(maicord::creation_test::failures ? 1 : 0);
}

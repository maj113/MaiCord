// Shipping host exercised with a typed, exact-ID DETAILS transport. No socket,
// worker or live account is created. This fixture is source-only until built.
#include "../src/app/application_thread_browser.hpp"
#include "../src/app/application_state.hpp"
#include "test_output.hpp"
namespace maicord::thread_browser_test { u64 now = 1000; }
namespace maicord { static u64 browser_fixture_now() noexcept { return thread_browser_test::now; } }
#define clock_monotonic_milliseconds browser_fixture_now
#include "../src/app/application_thread_browser.cpp"
#undef clock_monotonic_milliseconds

namespace maicord::thread_browser_test {
using namespace app_detail;
u32 failures{}, submissions{}, cancellations{}, repaints{}, navigations{}, text_rebuilds{};
u64 accepted{}, cancelled{}, navigated{};
u8 actor{};
RestClientSubmitError admission{};
ThreadBrowserRequest submitted{};
char submitted_name[400]{};
char body[32768]{};
u32 body_bytes{};
GuildRecord guilds[2]{};
u16 guild_order[2]{};
ChannelRecord channels[350]{};
u16 channel_order[350]{};
u8 names[65536]{};
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
RestClientSubmitResult WINAPI submit(void* instance, const ThreadBrowserRequest* request, u64 id) noexcept {
    if (instance != &actor || !id) return {RestClientSubmitError::invalid_input};
    if (!request) { ++cancellations; cancelled = id; return {}; }
    ++submissions;
    if (admission != RestClientSubmitError::none) return {admission};
    if (accepted) return {RestClientSubmitError::busy};
    if (!thread_browser_request_valid(*request)) return {RestClientSubmitError::invalid_input};
    accepted = id;
    submitted = *request;
    if (request->name.bytes) CopyMemory(submitted_name, request->name.data, request->name.bytes);
    submitted.name = {submitted_name, request->name.bytes};
    return {};
}
bool receipt(u64 id, u16 status, Utf8View payload = {}, u64 retry = 0, RestClientError error = {}) noexcept {
    RestClientResponse response{};
    response.request_id = id; response.status_code = status; response.body_bytes = payload.bytes;
    response.error = error; response.rate_limited = status == 429; response.retry_at_ms = retry;
    if (id == accepted) accepted = 0;
    return application_thread_browser_response(response, payload);
}
ChannelRecord* row(u64 id) noexcept {
    return const_cast<ChannelRecord*>(chat_state_store_find_channel(g_app.account_ui->chat_state, id));
}
void permit(bool allowed = true, bool manage = false) noexcept {
    auto* parent = row(70);
    parent->permissions_resolved = true;
    parent->effective_permissions = allowed ? kPermissionViewChannel | kPermissionReadMessageHistory |
        (manage ? kPermissionManageThreads : 0) : 0;
    parent->actions = allowed ? channel_action_view | channel_action_read_history |
        (manage ? channel_action_manage_threads : 0) : 0;
    ++parent->revision;
    ++g_app.account_ui->chat_state.permission_mutations;
}
void append(const char* text) noexcept {
    for (u32 i = 0; text[i] && body_bytes < sizeof(body); ++i) body[body_bytes++] = text[i];
}
void number(u64 value) noexcept {
    char digits[20]; u32 count{};
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) body[body_bytes++] = digits[--count];
}
Utf8View page(u64 first, u32 count, bool archived, bool more = false, bool private_first = false) noexcept {
    body_bytes = 0;
    append("{\"threads\":[");
    for (u32 i = 0; i < count; ++i) {
        if (i) append(",");
        append("{\"id\":\""); number(first + i);
        append("\",\"guild_id\":\"7\",\"parent_id\":\"70\",\"type\":");
        append(private_first && !i ? "12" : "11");
        append(",\"name\":\"Fixture thread\",\"thread_metadata\":{\"archived\":");
        append(archived ? "true" : "false");
        append(",\"locked\":false}}");
    }
    append("]");
    if (archived) append(more ? ",\"has_more\":true" : ",\"has_more\":false");
    append("}");
    return {body, body_bytes};
}
// Unarchived listing for the Active fallback (has_more only when there is more).
Utf8View active_page(u64 parent, u64 first, u32 count, bool more) noexcept {
    body_bytes = 0;
    append("{\"threads\":[");
    for (u32 i = 0; i < count; ++i) {
        if (i) append(",");
        append("{\"id\":\""); number(first + i);
        append("\",\"guild_id\":\"7\",\"parent_id\":\""); number(parent);
        append("\",\"type\":11,\"name\":\"Live thread\",\"thread_metadata\":{\"archived\":false,\"locked\":false}}");
    }
    append(more ? "],\"has_more\":true}" : "]}");
    return {body, body_bytes};
}
void permit_parent(u64 id) noexcept {
    auto* parent = row(id);
    parent->permissions_resolved = true;
    parent->effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    parent->actions = channel_action_view | channel_action_read_history;
    ++parent->revision;
    ++g_app.account_ui->chat_state.permission_mutations;
}
// Forum fixtures: parent metadata requests and post pages with tags/starters.
u32 forum_submissions{};
ForumPostRequest forum_request{};
RestClientSubmitResult WINAPI forum_submit(void* instance, const ForumPostRequest* request, u64 id) noexcept {
    if (instance != &actor || !id) return {RestClientSubmitError::invalid_input};
    if (!request) { ++cancellations; cancelled = id; return {}; }
    ++forum_submissions;
    forum_request = *request;
    return {};
}
Utf8View forum_page(u64 first, u32 count, bool archived, bool more) noexcept {
    body_bytes = 0;
    append("{\"threads\":[");
    for (u32 i = 0; i < count; ++i) {
        if (i) append(",");
        append("{\"id\":\""); number(first + i);
        append("\",\"guild_id\":\"7\",\"parent_id\":\"72\",\"type\":11,\"name\":\"Post\",\"owner_id\":\"5\",\"message_count\":");
        number(i);
        append(i % 2 ? ",\"applied_tags\":[\"801\",\"802\"]" : ",\"applied_tags\":[\"801\"]");
        append(i == 1 ? ",\"flags\":2" : ",\"flags\":0");
        if (i % 3 == 0) { append(",\"last_message_id\":\""); number(first + 100 + i); append("\""); }
        append(",\"thread_metadata\":{\"archived\":");
        append(archived ? "true" : "false");
        append(",\"locked\":false,\"create_timestamp\":\"2026-10-05T12:00:00Z\"}}");
    }
    append("],\"first_messages\":[{\"id\":\""); number(first);
    append("\",\"channel_id\":\""); number(first);
    append("\",\"content\":\"Hello\\n there\",\"author\":{\"id\":\"5\",\"username\":\"rob\",\"global_name\":null,\"avatar\":null}}]");
    if (archived || more) append(more ? ",\"has_more\":true" : ",\"has_more\":false");
    append("}");
    return {body, body_bytes};
}
bool tag_action(u64 id) noexcept {
    auto* scene = application_thread_browser_scene();
    if (!scene) return false;
    for (u32 i = 0; i < scene->widgets.count; ++i) {
        const auto target = scene->backing->targets[i];
        if (target.action == UiThreadBrowserAction::forum_tag && target.thread_id == id &&
            ui_thread_browser_scene_target_current(*scene, target))
            return application_thread_browser_activate(target);
    }
    return false;
}
u32 forum_row(u64 id) noexcept {
    for (u32 i = 0; i < browser_owner->row_count; ++i) if (browser_owner->rows[i].thread_id == id) return i;
    return ~0u;
}
bool action(UiThreadBrowserAction action) noexcept {
    auto* scene = application_thread_browser_scene();
    if (!scene) return false;
    for (u32 i = 0; i < scene->widgets.count; ++i) {
        const auto target = scene->backing->targets[i];
        if (target.action == action && ui_thread_browser_scene_target_current(*scene, target))
            return application_thread_browser_activate(target);
    }
    return false;
}
bool initialize() noexcept {
    g_app.window = CreateWindowExW(0, L"STATIC", L"Thread browser fixture", WS_POPUP, -16000, -16000,
        1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.window || !g_app.account_ui) return false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 350},
        {channel_order, 350}, {names, sizeof(names)}) ||
        !chat_state_store_upsert_guild(&state, {7, utf8_literal("Fixture guild"), GuildMutationKind::create, 99}).ok() ||
        !chat_state_store_upsert_channel(&state, {70, 7, 0, utf8_literal("Parent"), 0, 0, 0}).ok() ||
        !ui_scene_initialize_bootstrap(&g_app.shell_scene)) return false;
    auto metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags |= ui_shell_layout_threads;
    if (!ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, metrics, 1280, 720)) return false;
    state.self_user_id = 5; state.selected_guild_id = 7; state.selected_channel_id = 70;
    g_app.account_ui->active_channel_id = 70;
    g_app.account.instance = &actor; g_app.account.status.profile_id = 2;
    g_app.account.status.running = true; g_app.plugin_account_scope_serial = 3;
    g_app.account.api.thread_browser = submit;
    g_app.renderer.width = 1280; g_app.renderer.height = 720; g_app.renderer.dpi = 96;
    permit();
    for (u32 i = 0; i < 120; ++i) {
        ChannelInput input{100 + i, 7, 70, utf8_literal("Active"), 0, 0, 11};
        input.thread_active_known = true; input.thread_listed_active = true;
        if (!chat_state_store_upsert_channel(&state, input).ok()) return false;
    }
    // Metadata alone never creates an active-inventory observation.
    return chat_state_store_upsert_channel(&state, {500, 7, 70, utf8_literal("Metadata only"), 0, 0, 11}).ok() &&
        chat_state_store_upsert_channel(&state, {501, 7, 70, utf8_literal("Unjoined private"), 0, 0, 12}).ok();
}
void run() noexcept {
    check(initialize(), "browser: native host initializes with captured profile 2");
    if (!g_app.account_ui || !g_app.window) return;
    permit(false);
    check(!application_thread_browser_open() && !browser_owner && !submissions,
        "browser: missing fresh read authority cannot allocate or submit");
    permit();
    g_app.account.status.running = false;
    check(application_thread_browser_open() && browser_owner->total == 120 && browser_owner->row_count == 25 &&
        !browser_owner->view.inventory_synced && !application_thread_browser_pending(now) && !submissions,
        "browser: offline Active projects cached inventory directly without metadata promotion or truncation");
    const auto stable = browser_owner->revision;
    check(application_thread_browser_synchronize() && application_thread_browser_synchronize() &&
        browser_owner->revision == stable, "browser: unchanged frames do not recount Core inventory");
    for (u32 i = 0; i < 4; ++i) check(action(UiThreadBrowserAction::next), "browser: active next page");
    check(browser_owner->row_count == 20 && browser_owner->rows[19].thread_id == 219 && !browser_owner->has_more,
        "browser: direct Active paging reaches eligible records past 100");
    check(action(UiThreadBrowserAction::archived) && browser_owner->error && !browser_owner->queued &&
        !browser_owner->view.loading && !application_thread_browser_pending(now) && !submissions,
        "browser: offline Archived choice fails explicitly without an infinite loading state");
    g_app.account.status.running = true;
    g_app.account.api.thread_browser = nullptr;
    check(action(UiThreadBrowserAction::retry) && browser_owner->error && !browser_owner->queued &&
        !browser_owner->view.loading && !submissions,
        "browser: unavailable typed account API gives explicit reconnect feedback");
    g_app.account.api.thread_browser = submit;
    check(action(UiThreadBrowserAction::retry) && application_thread_browser_next_wake() == 1 &&
        application_thread_browser_pending(now),
        "browser: initially queued Archived publishes an immediate nonzero scheduler deadline");
    check(application_thread_browser_submit(10, now) && !application_thread_browser_next_wake() &&
        submitted.mode == ThreadBrowserMode::archived && submitted.offset == 0 && !submitted.name.bytes,
        "browser: archived request has exact parent and offset zero");
    check(!receipt(999, 200, page(1000, 25, true, true)) && browser_owner->flight.id == 10,
        "browser: unrelated DETAILS receipt cannot drain flight");
    check(receipt(10, 200, page(1000, 25, true, true)) && browser_owner->row_count == 25 &&
        browser_owner->has_more && browser_owner->banks[browser_owner->bank].committed == 12288,
        "browser: archived25 receipt publishes after batch preflight with bounded bank commitment");
    check(action(UiThreadBrowserAction::next) && application_thread_browser_submit(11, now) && submitted.offset == 25 &&
        receipt(11, 200, page(2000, 1, true)) && browser_owner->rows[0].thread_id == 2000,
        "browser: archived next refetches offset25 and retains exact page identity");
    check(action(UiThreadBrowserAction::previous) && application_thread_browser_submit(12, now) && submitted.offset == 0 &&
        receipt(12, 200, page(1000, 25, true, true)), "browser: archived previous refetches offset zero");
    ApplicationThreadBrowserSearchScope scope{};
    check(application_thread_browser_search_scope(&scope) && scope.parent_id == 70 &&
        scope.session == browser_owner->session && scope.session != 0 &&
        utf8_equal(scope.parent_name, utf8_literal("Parent")) && !scope.forum,
        "browser: the header search bar's thread scope names the open parent and its browsing session");
    check(application_thread_browser_search(utf8_literal("  Find thread\t")) &&
        browser_owner->tab == UiThreadBrowserTab::search && application_thread_browser_next_wake() == 1 &&
        application_thread_browser_pending(now),
        "browser: a header search query queues Search with an immediate nonzero scheduler deadline");
    check(application_thread_browser_submit(13, now) &&
        submitted.mode == ThreadBrowserMode::search && !submitted.offset && utf8_equal(submitted.name, utf8_literal("Find thread")) &&
        receipt(13, 200, page(3000, 100, false)), "browser: Search request owns name-only query and accepts100 without truncation");
    const u32 before_search_pages = submissions;
    for (u32 i = 0; i < 3; ++i) check(action(UiThreadBrowserAction::next), "browser: Search local next");
    check(browser_owner->page_index == 3 && browser_owner->row_count == 25 && browser_owner->rows[24].thread_id == 3099 &&
        submissions == before_search_pages && !browser_owner->has_more, "browser: Search is at most four local pages with zero pagination REST");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(20, now), "browser: cancellation fixture flight");
    check(application_thread_browser_search(utf8_literal("Find thread")) && cancelled == 20 && browser_owner->flight.retired &&
        !application_thread_browser_pending(now) && !application_thread_browser_submit(21, now),
        "browser: latest choice cancels exact immutable flight and awaits drainage before lane reuse");
    check(receipt(20, 429, {}, 5000) && application_thread_browser_next_wake() == 5000 &&
        !application_thread_browser_pending(4999) && application_thread_browser_pending(5000),
        "browser: stale canceled rate limit establishes surviving actor floor");
    now = 5000;
    check(application_thread_browser_submit(21, now) && submitted.mode == ThreadBrowserMode::search &&
        receipt(21, 200, page(3000, 100, false)), "browser: latest Search publishes only after exact older drainage");
    // Header search results land in the panel's list; a result opens its thread.
    const u64 searched_session = browser_owner->session;
    const auto* results = application_thread_browser_scene();
    const auto first_result = results ? ui_thread_browser_scene_target(*results, results->backing->rows[0])
                                      : UiThreadBrowserTarget{};
    check(first_result.action == UiThreadBrowserAction::open_thread && first_result.thread_id == 3000 &&
        application_thread_browser_activate(first_result) && navigated == 3000 && !application_thread_browser_active() &&
        !application_thread_browser_search_scope(&scope),
        "browser: a header search result jumps into its thread and releases the search scope");
    check(application_thread_browser_open() && browser_owner->tab == UiThreadBrowserTab::active &&
        application_thread_browser_search_scope(&scope) && scope.session == browser_owner->session &&
        scope.session != searched_session, "browser: reopening starts a new header search session on the Active list");
    check(application_thread_browser_search(utf8_literal("Find thread")) && browser_owner->tab == UiThreadBrowserTab::search &&
        application_thread_browser_search(utf8_literal(" \t ")) && browser_owner->tab == UiThreadBrowserTab::active &&
        !browser_owner->error, "browser: a cleared header query leaves the results for the Active list");
    char long_name[101];
    for (auto& byte : long_name) byte = 'x';
    check(!application_thread_browser_search({long_name, sizeof(long_name)}) && browser_owner->error &&
        utf8_equal(browser_owner->view.status, utf8_literal("Thread names can be up to 100 characters.")),
        "browser: an over-long header query explains the limit in the panel");
    BYTE keyboard[256]{}, saved_keyboard[256]{};
    const bool keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    keyboard[VK_CONTROL] = keyboard[VK_LCONTROL] = 0x80;
    g_app.context_focus = &browser_owner->scene.widgets;
    const bool search_shortcut_passes = SetKeyboardState(keyboard) &&
        !application_thread_browser_input(UiInputKind::key_down, 'F', false);
    keyboard[VK_CONTROL] = keyboard[VK_LCONTROL] = 0;
    SetKeyboardState(keyboard);
    check(keyboard_saved && search_shortcut_passes && application_thread_browser_input(UiInputKind::key_down, VK_TAB, false) &&
        application_thread_browser_input(UiInputKind::character, 'f', false) && application_thread_browser_active(),
        "browser: the search shortcut passes to the header search bar while panel keys stay in the panel");
    if (keyboard_saved) SetKeyboardState(saved_keyboard);
    g_app.context_focus = nullptr;
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(30, now), "browser: Gateway precedence fixture");
    ChannelInput update{1000, 7, 70, utf8_literal("New Gateway name"), 0, 0, 11};
    update.thread_archived_known = true; update.thread_locked_known = true;
    chat_state_store_upsert_channel(&g_app.account_ui->chat_state, update);
    application_thread_browser_gateway(7, 70);
    check(receipt(30, 200, page(1000, 1, true)) && utf8_equal(browser_owner->rows[0].name, utf8_literal("New Gateway name")) &&
        !row(1000)->thread_archived, "browser: matching later Gateway name/archive publication wins stale HTTP receipt");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(31, now), "browser: removed thread receipt fixture");
    chat_state_store_remove_channel(&g_app.account_ui->chat_state, 1000);
    application_thread_browser_gateway(7, 70);
    check(receipt(31, 200, page(1000, 1, true)) && !row(1000) && !browser_owner->row_count,
        "browser: Gateway deletion prevents HTTP resurrection");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(40, now) &&
        receipt(40, 200, page(4000, 1, true, false, true)) && !browser_owner->row_count,
        "browser: private names remain unpublished without joined or ManageThreads proof");
    chat_state_store_set_thread_self_membership(&g_app.account_ui->chat_state, 4000, 5, true);
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(41, now) &&
        receipt(41, 200, page(4000, 1, true, false, true)) && row(4000)->thread_self_joined && browser_owner->row_count == 1,
        "browser: omitted REST membership preserves positive current-user proof");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(42, now) &&
        receipt(42, 200, page(100, 1, true)) && row(100)->thread_active_known && !row(100)->thread_listed_active,
        "browser: positive archive receipt removes obsolete positive active observation");
    constexpr auto joined = utf8_literal(R"({"threads":[{"id":"4500","guild_id":"7","parent_id":"70","type":12,"name":"Joined private","thread_metadata":{"archived":true,"locked":false},"member":{"id":"4500","user_id":"5","join_timestamp":"2026-10-05T12:00:00Z","flags":0}}],"has_more":false})");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(43, now) && receipt(43, 200, joined) &&
        row(4500)->thread_self_joined_known && row(4500)->thread_self_joined && browser_owner->row_count == 1,
        "browser: validated current-user member alone establishes private read proof");
    check(action(UiThreadBrowserAction::archived) && application_thread_browser_submit(50, now), "browser: withdrawal fixture");
    permit(false);
    check(application_thread_browser_synchronize() && !application_thread_browser_active() && cancelled == 50 &&
        !g_app.renderer.thread_browser_scene && receipt(50, 200, page(4100, 1, true)) && !row(4100),
        "browser: fresh permission withdrawal cancels and retires scene before stale receipt can hydrate");
    permit();
    check(application_thread_browser_open() && action(UiThreadBrowserAction::archived) && application_thread_browser_submit(60, now),
        "browser: account replacement fixture");
    g_app.account.status.profile_id = 9;
    check(application_thread_browser_synchronize() && !application_thread_browser_active() &&
        receipt(60, 200, page(4200, 1, true)) && !row(4200), "browser: captured profile2 receipt cannot publish under profile9");
    g_app.account.status.profile_id = 2;
    check(application_thread_browser_open() && action(UiThreadBrowserAction::archived) &&
        application_thread_browser_submit(61, now), "browser: closed flight wake fixture");
    application_thread_browser_close();
    check(!application_thread_browser_active() && browser_owner->flight.id == 61 && cancelled == 61 &&
        !application_thread_browser_next_wake() && !application_thread_browser_pending(now) &&
        receipt(61, 200, page(4300, 1, true)) && !row(4300),
        "browser: closed admitted flight waits for exact drain without a scheduler wake or publication");
    check(application_thread_browser_open() && action(UiThreadBrowserAction::archived), "browser: bounded busy fixture");
    admission = RestClientSubmitError::busy;
    for (u32 i = 0; i < 21; ++i) { application_thread_browser_submit(100 + i, now); now += 50; }
    check(browser_owner->error && !browser_owner->queued && !application_thread_browser_next_wake(),
        "browser: repeated transport busy is bounded and requires explicit retry");
    admission = {};
    check(action(UiThreadBrowserAction::retry) && application_thread_browser_submit(80, now) && receipt(80, 401) &&
        browser_owner->error && !application_thread_browser_pending(now + 100000), "browser: expired session does not automatically retry");
    check(action(UiThreadBrowserAction::retry) && application_thread_browser_submit(81, now), "browser: parent generation fixture");
    ++row(70)->generation;
    check(application_thread_browser_synchronize() && !application_thread_browser_active() &&
        receipt(81, 200, page(6000, 1, true)) && !row(6000),
        "browser: reused parent ID cannot authorize a prior-generation receipt");

    // Active tab with no cached threads and no synced Gateway inventory: it
    // asks REST instead of showing "Loading threads" forever.
    auto& state = g_app.account_ui->chat_state;
    check(chat_state_store_upsert_channel(&state, {71, 7, 0, utf8_literal("Quiet parent"), 1, 0, 0}).ok(),
        "browser: unsynced empty parent fixture");
    permit_parent(71);
    check(application_thread_browser_open(71) && browser_owner->tab == UiThreadBrowserTab::active &&
        browser_owner->view.loading && !browser_owner->view.inventory_synced && !browser_owner->row_count &&
        utf8_equal(browser_owner->view.status, utf8_literal("Loading threads…")) &&
        application_thread_browser_next_wake() != 0 && application_thread_browser_pending(now),
        "browser: unsynced Active inventory queues a REST listing due now");
    check(application_thread_browser_submit(200, now) && submitted.mode == ThreadBrowserMode::active &&
        submitted.parent_id == 71 && submitted.offset == 0 && browser_owner->view.loading,
        "browser: Active fallback requests the parent's unarchived listing");
    check(receipt(200, 200, utf8_literal(R"({"threads":[]})")) && !browser_owner->view.loading &&
        !browser_owner->error && browser_owner->view.inventory_synced && !browser_owner->row_count &&
        !browser_owner->view.status.bytes && !application_thread_browser_pending(now + 100000) &&
        !application_thread_browser_next_wake(),
        "browser: empty Active listing settles to the empty state without waiting for the Gateway");
    check(action(UiThreadBrowserAction::active) && application_thread_browser_submit(201, now) &&
        receipt(201, 500) && browser_owner->error && !browser_owner->view.loading &&
        !application_thread_browser_pending(now + 100000),
        "browser: failed Active listing with nothing saved shows an error instead of loading");
    check(action(UiThreadBrowserAction::retry) && application_thread_browser_submit(202, now) &&
        submitted.mode == ThreadBrowserMode::active && receipt(202, 403) && browser_owner->error &&
        utf8_equal(browser_owner->view.status, utf8_literal("You no longer have permission to browse these threads.")),
        "browser: Active retry reaches the server and a permission denial is explicit");
    check(action(UiThreadBrowserAction::retry) && application_thread_browser_submit(203, now) &&
        receipt(203, 200, active_page(71, 7000, 25, true)) && browser_owner->total == 25 &&
        row(7000)->thread_active_known && row(7000)->thread_listed_active && !browser_owner->view.loading &&
        !browser_owner->view.inventory_synced && application_thread_browser_pending(now) &&
        utf8_equal(browser_owner->view.status, utf8_literal("Showing saved threads. Updating…")),
        "browser: Active rows publish immediately and stay usable while the next page loads");
    check(application_thread_browser_submit(204, now) && submitted.offset == 25 &&
        receipt(204, 200, active_page(71, 7100, 1, false)) && browser_owner->total == 26 &&
        browser_owner->view.inventory_synced && !browser_owner->view.status.bytes &&
        !application_thread_browser_pending(now + 100000),
        "browser: Active listing pages in order and settles when the server has no more");
    check(chat_state_store_set_thread_listing_synced(&state, 71, 5) && action(UiThreadBrowserAction::active) &&
        !browser_owner->queued && !application_thread_browser_pending(now) && browser_owner->view.inventory_synced &&
        browser_owner->total == 26, "browser: a synced Gateway inventory needs no REST listing");
    check(chat_state_store_set_thread_listing_synced(&state, 71, 5, false) && action(UiThreadBrowserAction::active) &&
        application_thread_browser_submit(205, now), "browser: raced Active page fixture");
    application_thread_browser_gateway(7, 71);
    check(receipt(205, 200, active_page(71, 7200, 1, false)) && !row(7200) && !browser_owner->view.inventory_synced &&
        application_thread_browser_pending(now) && application_thread_browser_submit(206, now) && submitted.offset == 0 &&
        receipt(206, 200, active_page(71, 7200, 1, false)) && row(7200) && browser_owner->view.inventory_synced,
        "browser: an Active page raced by a Gateway thread mutation is refetched, never merged stale");
    const u32 before_offline = submissions;
    g_app.account.status.running = false;
    check(chat_state_store_set_thread_listing_synced(&state, 71, 5, false) && action(UiThreadBrowserAction::active) &&
        !browser_owner->error && !browser_owner->queued && !browser_owner->view.loading &&
        browser_owner->row_count == 25 && browser_owner->view.status.bytes && submissions == before_offline,
        "browser: offline Active keeps saved rows usable with an explicit notice");
    g_app.account.status.running = true;
    application_thread_browser_close();

    // Forum parent: tag metadata, post metadata, official ordering, tag filter,
    // layout and pagination through the existing active/archived endpoints.
    g_app.account.api.forum_post = forum_submit;
    check(chat_state_store_upsert_channel(&state, {72, 7, 0, utf8_literal("Help forum"), 2, 0, 15}).ok(),
        "forum: parent fixture");
    permit_parent(72);
    check(application_thread_browser_open(72) && browser_owner->forum_mode && browser_owner->forum &&
        browser_owner->view.forum && browser_owner->view.forum->layout == UiForumLayout::list &&
        application_thread_browser_pending(now) && application_thread_browser_submit(300, now) && forum_submissions == 1 &&
        forum_request.operation == ForumPostOperation::parent_metadata && forum_request.parent_id == 72 &&
        forum_request.guild_id == 7 && !application_thread_browser_pending(now),
        "forum: opening a forum requests its tags first through the forum metadata route");
    check(receipt(300, 200, utf8_literal(R"({"id":"72","guild_id":"7","type":15,"name":"Help forum","flags":0,"available_tags":[{"id":"801","name":"Bug","moderated":false,"emoji_id":null,"emoji_name":null},{"id":"802","name":"Feature","moderated":false}],"default_sort_order":1,"default_forum_layout":2,"default_tag_setting":"match_all"})")) &&
        browser_owner->forum->meta_loaded && browser_owner->view.forum->tags.count == 2 &&
        browser_owner->view.forum->sort == UiForumSort::creation_date &&
        browser_owner->view.forum->layout == UiForumLayout::gallery &&
        browser_owner->view.forum->match == UiForumTagMatch::all && browser_owner->scene.forum &&
        browser_owner->scene.backing->chips[1].generation,
        "forum: available tags and the channel's sort, layout and tag-match defaults apply");
    check(application_thread_browser_pending(now) && application_thread_browser_submit(301, now) &&
        submitted.mode == ThreadBrowserMode::active && submitted.parent_id == 72 && submitted.offset == 0 &&
        receipt(301, 200, forum_page(9100, 25, false, true)) && application_thread_browser_pending(now) &&
        application_thread_browser_submit(302, now) && submitted.offset == 25 &&
        receipt(302, 200, forum_page(9200, 5, false, false)) && browser_owner->total == 30 &&
        browser_owner->view.inventory_synced && !application_thread_browser_pending(now + 100000),
        "forum: active posts always load over REST in order until the server has no more");
    check(browser_owner->rows[0].thread_id == 9201 && browser_owner->rows[1].thread_id == 9101 &&
        browser_owner->rows[2].thread_id == 9204 && browser_owner->forum->posts[0].pinned &&
        browser_owner->forum->posts[0].thread_id == 9201 && browser_owner->has_more,
        "forum: creation date order puts pinned posts first, then newest post IDs");
    const u32 starter = forum_row(9200);
    check(starter < kUiThreadBrowserPageCapacity && utf8_equal(browser_owner->forum->posts[starter].preview, utf8_literal("Hello there")) &&
        utf8_equal(browser_owner->forum->posts[starter].author_name, utf8_literal("rob")) &&
        browser_owner->forum->posts[starter].author_id == 5 && browser_owner->forum->posts[starter].metadata_known &&
        browser_owner->forum->posts[starter].reply_count == 0 && browser_owner->forum->posts[starter].tag_count == 1 &&
        browser_owner->forum->posts[forum_row(9204)].reply_count == 4 &&
        browser_owner->forum->posts[forum_row(9204)].activity.bytes != 0,
        "forum: cards carry the starter preview, author, tags, message count and relative activity");
    check(action(UiThreadBrowserAction::forum_sort) && browser_owner->view.forum->sort == UiForumSort::latest_activity &&
        browser_owner->rows[0].thread_id == 9201 && browser_owner->rows[1].thread_id == 9101 &&
        browser_owner->rows[2].thread_id == 9203 && browser_owner->rows[3].thread_id == 9200 &&
        browser_owner->rows[4].thread_id == 9124,
        "forum: latest activity orders by last message, else post ID, after pinned posts");
    check(action(UiThreadBrowserAction::next) && browser_owner->page_index == 1 && browser_owner->row_count == 5 &&
        !browser_owner->has_more,
        "forum: the ordered active set pages locally in pages of 25");
    check(tag_action(802) && browser_owner->page_index == 0 && browser_owner->total == 14 &&
        browser_owner->row_count == 14 && browser_owner->view.forum->tags[1].selected &&
        tag_action(801) && browser_owner->total == 14 && tag_action(802) && browser_owner->total == 30 &&
        tag_action(801) && browser_owner->total == 30 && !browser_owner->forum->selected_count,
        "forum: tag chips filter to posts carrying every selected tag and reset to the first page");
    check(action(UiThreadBrowserAction::forum_list) && browser_owner->view.forum->layout == UiForumLayout::list &&
        action(UiThreadBrowserAction::forum_gallery) && browser_owner->view.forum->layout == UiForumLayout::gallery &&
        browser_owner->scene.backing->grid.columns >= 1,
        "forum: List and Gallery switch the card layout");
    check(tag_action(802) && action(UiThreadBrowserAction::archived) && application_thread_browser_submit(303, now) &&
        submitted.mode == ThreadBrowserMode::archived && submitted.offset == 0 &&
        receipt(303, 200, forum_page(9300, 25, true, true)) && browser_owner->has_more &&
        browser_owner->row_count == 12 && browser_owner->total == 12 &&
        action(UiThreadBrowserAction::next) && browser_owner->page_index == 1 &&
        application_thread_browser_submit(304, now) && submitted.offset == 25 &&
        receipt(304, 200, forum_page(9400, 2, true, false)) && !browser_owner->has_more &&
        browser_owner->row_count == 1 && browser_owner->rows[0].thread_id == 9401 &&
        action(UiThreadBrowserAction::previous) && application_thread_browser_submit(305, now) && submitted.offset == 0,
        "forum: archived posts page through the server listing and filter each page by tag");
    check(receipt(305, 200, forum_page(9300, 25, true, true)) && browser_owner->forum->cache_count == 57,
        "forum: post metadata from every accepted page is retained in the bounded cache");
    application_thread_browser_close();
    check(application_thread_browser_open(72) && browser_owner->forum->meta_loaded && forum_submissions == 1 &&
        browser_owner->forum->selected_count == 1,
        "forum: reopening the same forum keeps its tags, cache and filter without refetching metadata");
    application_thread_browser_close();
    for (u32 kind = 0; kind < 4; ++kind) {
        application_thread_browser_release();
        const u32 before = forum_submissions;
        const u64 id = 1000 + kind;
        check(application_thread_browser_open(72) && application_thread_browser_submit(id, now) &&
                  forum_submissions == before + 1,
              "forum: terminal metadata rate fixture admits exactly one real metadata request");
        RestClientResponse terminal{};
        terminal.request_id = id;
        terminal.status_code = 429;
        terminal.rate_limited = true;
        terminal.retry_at_ms = now + 1000;
        terminal.rate.retry_after_ms = 2000;
        switch (kind) {
        case 0:
            terminal.retry_at_ms = 0;
            break;
        case 1:
            terminal.rate_limited = false;
            break;
        case 2:
            terminal.rejection.captcha = true;
            break;
        case 3:
            terminal.rejection = {40002, true, false};
            break;
        }
        check(application_thread_browser_response(terminal, {}) && !browser_owner->forum->meta_queued &&
                  !browser_owner->forum->meta_loaded && !browser_owner->forum->meta_flight.active &&
                  !browser_owner->actor_floor,
              "forum: unknown deadlines and terminal verification receipts do not invent a metadata retry");
        bool settled = true;
        for (u32 pass = 0; pass < 64; ++pass) {
            application_thread_browser_synchronize();
            settled = settled && !forum_meta_due(*browser_owner, ~u64{0}) &&
                      !browser_owner->forum->meta_queued && forum_submissions == before + 1;
        }
        check(settled, "forum: unchanged synchronization never recreates a terminal metadata flight");
    }
    application_thread_browser_release();
    const u32 before_rate = forum_submissions;
    check(application_thread_browser_open(72) && application_thread_browser_submit(1010, now),
          "forum: authoritative metadata rate fixture admits");
    bool bounded_rate = true;
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        RestClientResponse limited{};
        limited.request_id = 1010 + attempt;
        limited.status_code = 429;
        limited.rate_limited = true;
        limited.retry_at_ms = now + 1;
        limited.rate.retry_after_ms = 2000;
        bounded_rate = bounded_rate && application_thread_browser_response(limited, {}) &&
                       browser_owner->actor_floor == limited.retry_at_ms;
        if (attempt < 2) {
            bounded_rate = bounded_rate && browser_owner->forum->meta_queued &&
                           !forum_meta_due(*browser_owner, now) &&
                           forum_meta_due(*browser_owner, limited.retry_at_ms);
            now = limited.retry_at_ms;
            bounded_rate = bounded_rate && application_thread_browser_submit(1011 + attempt, now);
        } else
            bounded_rate = bounded_rate && !browser_owner->forum->meta_queued;
    }
    check(bounded_rate && forum_submissions == before_rate + 3,
          "forum: genuine metadata rate limits preserve the exact server floor and two-retry budget");
    application_thread_browser_release();
    g_app.account.api.forum_post = nullptr;

    // Scrolling the channel list beside the open browser rebuilds the retained
    // shell text, not just a frame over stale glyphs.
    check(application_thread_browser_open(71), "browser: scroll refresh fixture");
    g_app.renderer_ready = true;
    const u32 rebuilds_before = text_rebuilds;
    check(application_thread_browser_refresh_after_scroll() && text_rebuilds == rebuilds_before + 1,
        "browser: scroll beside the open browser rebuilds the channel list text");
    g_app.renderer_ready = false;
    application_thread_browser_close();
    application_thread_browser_release();
    check(!browser_owner && !g_app.renderer.thread_browser_scene, "browser: idle release frees owner and both bounded banks");
    ui_scene_release(&g_app.shell_scene);
    DestroyWindow(g_app.window); g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr;
}
}
namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++thread_browser_test::text_rebuilds;
    return true;
}
bool renderer_bind_thread_browser_scene(D3D11Renderer* renderer, UiThreadBrowserScene* scene) noexcept {
    renderer->thread_browser_scene = scene; return true;
}
namespace app_detail {
bool application_forum_form_active() noexcept { return false; }
bool application_forum_post_available(u64) noexcept { return false; }
bool application_forum_post_open(u64) noexcept { return false; }
Application g_app{};
const Theme& application_theme() noexcept { static Theme theme{}; return theme; }
bool application_request_frame() noexcept { ++thread_browser_test::repaints; return true; }
bool refresh_account_text(bool) noexcept { return true; }
bool application_settings_active() noexcept { return false; }
bool application_friends_active() noexcept { return false; }
bool application_search_close_panel() noexcept { return true; }
void application_pinned_messages_close() noexcept {}
void application_composer_text_input_suspend() noexcept {}
void end_message_composer_pointer_selection() noexcept {}
void end_message_text_selection() noexcept {}
bool application_navigate_known_message(u64 id, u64) noexcept {
    ++thread_browser_test::navigations; thread_browser_test::navigated = id; return true;
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_application_thread_browser_entry() noexcept {
    maicord::thread_browser_test::run();
    ExitProcess(maicord::thread_browser_test::failures ? 1 : 0);
}

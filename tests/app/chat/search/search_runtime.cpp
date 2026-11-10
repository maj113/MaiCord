// Compile the shipping controller once in this isolated executable. Private
// ownership is visible here without adding a shipping diagnostic/test hook.
#include "../../../../src/app/chat/search/search.cpp"
#include "../../../support/account_module_fake.hpp"
#include "../../../../src/app/chat/search/search_results.cpp"
#include "../../../../src/app/chat/search/search_filters.cpp"
#include "../../../../src/app/chat/search/search_actions.cpp"
#include "../../../../src/app/chat/search/search_input.cpp"
#include "search_runtime_internal.hpp"
#include "../../../support/model_store_fixtures.hpp"
#include "application_search_measurement.hpp"
#include "../../../support/test_output.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/platform/window/key.hpp"
#include "../../../support/window_fixture.hpp"

namespace maicord::search_runtime_test {

AccountFixture account;

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
    *out = {{minimum(request.constraint.width, text.codepoints * 6.f), lines * 16.f}, lines};
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

bool modifiers(bool control, bool shift, bool alt) noexcept {
    null_window_release_keys();
    if (control) window_test::hold(key::ctrl);
    if (shift) window_test::hold(key::shift);
    if (alt) window_test::hold(key::alt);
    return true;
}

bool open(bool shift) noexcept {
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
    return open(true) && type(text) && key(key::enter);
}

i64 point(UiRect bounds) noexcept {
    return window_test::pack(static_cast<short>((bounds.left + bounds.right) * .5f),
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

bool click(UiSearchAction action, bool second_press) noexcept {
    const auto bounds = action_bounds(action);
    if (bounds.left >= bounds.right || bounds.top >= bounds.bottom) {
        return false;
    }
    return application_search_window_event(second_press ? window_test::pointer_double(window_test::x_of(point(bounds)), window_test::y_of(point(bounds))) : window_test::pointer_down(window_test::x_of(point(bounds)), window_test::y_of(point(bounds)))) &&
           application_search_window_event(window_test::pointer_up(window_test::x_of(point(bounds)), window_test::y_of(point(bounds)), PointerButton::left));
}

bool response(u64 request_id, u16 status, Utf8View body, u64 retry_at,
              u64 retry_after_ms) noexcept {
    RestClientResponse value{};
    value.request_id = request_id;
    value.status_code = status;
    value.retry_at_ms = retry_at;
    value.rate.retry_after_ms = retry_after_ms;
    return application_search_response(value, body);
}

bool initialize() noexcept {
    g_app.window = window_test::open();
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.window || !g_app.account_ui || !modifiers()) {
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
    g_account_fake.message_search = submit;
    g_account_fake.message_ack = acknowledge;
    window_focus(g_app.window);
    g_window_state.keyboard_focus = true;
    return application_search_synchronize();
}

void release() noexcept {
    application_search_release();
    ui_scene_release(&g_app.shell_scene);
    window_release_capture(g_app.window);
    if (g_app.window) {
        window_test::close();
        g_window_state.keyboard_focus = false;
    }
    if (g_app.account_ui) {
        message_poll_store_release(&g_app.account_ui->message_poll_store);
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    }
    g_app.window = nullptr;
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    g_app.context_focus = nullptr;
}
usize bank_committed(const app_detail::SearchBank* b) noexcept {
    return b ? committed(b) + b->content_storage.arena.committed + b->rich_storage.arena.committed +
                   b->user_records.arena.committed + b->user_order.arena.committed +
                   b->rich_text.committed[0] + b->rich_text.committed[1] + b->user_names.committed[0] +
                   b->user_names.committed[1]
             : 0;
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
    check(key(key::enter) && application_search_pending(1) && account.submissions == 0 &&
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
    check(g_app.search->suggestions && key(key::escape) && !g_app.search->suggestions &&
              g_app.search->panel && g_app.search->bank == ready_bank &&
              account.submissions == submissions_before_refinement,
          "search runtime: Escape dismisses query suggestions without closing retained results or "
          "submitting");
    const auto result_bounds = action_bounds(UiSearchAction::result);
    check(application_search_window_event(window_test::pointer_move(window_test::x_of(point(result_bounds)), window_test::y_of(point(result_bounds)))) &&
              g_app.search->scene.widgets.hovered.generation != 0 &&
              !application_search_window_event(window_test::pointer_leave()) &&
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
    check(application_search_window_event(window_test::pointer_down(window_test::x_of(point(edit_bounds)), window_test::y_of(point(edit_bounds)))) &&
              window_pointer_captured(g_app.window) && g_app.search->dragging,
          "search runtime: edit press owns actual native selection capture");
    settings = true;
    check(
        application_search_synchronize() && !application_search_input(UiInputKind::character, 'x', false) &&
            !application_search_edit_focused() && !g_app.search->focused && !g_app.search->dragging &&
            !g_app.search->captured && !window_pointer_captured(g_app.window),
        "search runtime: modal front retires native capture and keyboard ownership before blocking input");
    application_search_window_event(window_test::capture_lost());
    window_release_capture(g_app.window);
    settings = false;
    application_search_window_event(window_test::focus_changed(false));
    check(!g_app.search->dragging && !application_search_edit_focused() &&
              !application_search_edit_next_wake(),
          "search runtime: capture and native focus loss retire editor drag and caret deadlines");
    check(open(true) && key(key::tab) && !g_app.search->focused &&
              g_app.context_focus == &g_app.search->scene.widgets &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::filter_from &&
              key(key::enter) &&
              utf8_equal(message_composer_text(g_app.search->edit), utf8_literal("from:")) &&
              g_app.search->suggestions && !application_search_pending(1),
          "search runtime: Tab and Enter activate a suggested prefix without submitting the query");
    check(open(true) && type(utf8_literal("bounded")) && key(key::escape) && click(UiSearchAction::sort) &&
              !g_app.search->focused && g_app.search->sort_menu && key(key::tab) &&
              g_app.search->scene.targets[g_app.search->scene.widgets.focused.slot].action ==
                  UiSearchAction::oldest &&
              key(key::enter) && !g_app.search->sort_menu &&
              g_app.search->controller.query.sort == MessageSearchSort::oldest &&
              application_search_pending(1),
          "search runtime: checked Sort owns keyboard choices and Enter commits the selected order");
    check(click(UiSearchAction::sort) && g_app.search->sort_menu && !g_app.search->focused,
          "search runtime: pointer Sort opens independently of query editor focus");
    check(click(UiSearchAction::sort, true) && !g_app.search->sort_menu &&
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
        application_search_window_event(window_test::pointer_down(window_test::x_of(point(dismiss_bounds)), window_test::y_of(point(dismiss_bounds)))) &&
            !g_app.search->suggestions && !g_app.search->focused &&
            application_search_window_event(window_test::pointer_up(window_test::x_of(point(dismiss_bounds)), window_test::y_of(point(dismiss_bounds)), PointerButton::left)) &&
            g_app.search->bank == dismissed_bank && navigations == navigations_before_dismiss,
        "search runtime: a covered result-pane click dismisses suggestions without background activation");
    check(open(true) && key(key::escape) && !g_app.search->suggestions && key(key::escape) &&
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

extern "C" __declspec(noreturn) void WINAPI maicord_application_search_runtime_entry() noexcept {
    maicord::search_runtime_test::run();
    maicord::test_exit(maicord::search_runtime_test::failures, "application search runtime");
}


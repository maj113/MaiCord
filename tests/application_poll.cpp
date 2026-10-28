// Actual application owner, pure controller, sparse poll store, Core authority
// and strict protocol decoders. Transport, clock, renderer binding and profile
// opening are explicit offline fixture boundaries; no account/device is used.
#include "../src/app/application_poll.hpp"
#include "../src/app/application_state.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"
namespace maicord::poll_app_test {
u64 now = 1000, unix_now = 1000;
}
namespace maicord {
static u64 poll_fixture_now() noexcept {
    return poll_app_test::now;
}
static u64 poll_fixture_unix() noexcept {
    return poll_app_test::unix_now;
}
} // namespace maicord
#define clock_monotonic_milliseconds poll_fixture_now
#define clock_unix_milliseconds poll_fixture_unix
#include "../src/app/application_poll.cpp"
#undef clock_monotonic_milliseconds
#undef clock_unix_milliseconds

namespace maicord::poll_app_test {
using namespace app_detail;
u32 failures{}, submissions{}, cancellations{}, frames{}, measures{};
u16 measured_backend{};
Theme fixture_theme = kDefaultTheme;
u64 accepted{}, cancelled{};
u8 actor_token{}, other_actor{};
bool blocked{}, host_owned{};
bool search_enabled{};
u32 search_publication = 17;
RestClientSubmitError admission{};
PollRequest submitted{};
GuildRecord guilds[2]{};
u16 guild_order[2]{};
ChannelRecord channels[8]{};
u16 channel_order[8]{};
u8 names[4096]{};
RoleRecord roles[4]{};
PermissionOverwriteRecord overwrites[4]{};
MessageRecord messages[16]{};
u32 message_order[16]{};
u8 message_text[8192]{};
UiTextMeasureRecord measure_rows[128]{};
char body[8192]{};
u32 body_bytes{};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool measure(void*, const UiTextMeasureRequest& r, UiTextMeasureValue* out) noexcept {
    ++measures;
    measured_backend = r.key.backend_revision;
    if (!out)
        return false;
    const float width = minimum(maximum(1.f, r.constraint.width), r.text.bytes * 7.f);
    *out = {{width, 18.f}, 14.f, 1};
    return true;
}
RestClientSubmitResult WINAPI submit(void* instance, const PollRequest* request,
                                     u64 id) noexcept {
    if (instance != &actor_token || !id)
        return {RestClientSubmitError::invalid_input};
    if (!request) {
        ++cancellations;
        cancelled = id;
        return {};
    }
    ++submissions;
    if (admission != RestClientSubmitError::none)
        return {admission};
    if (accepted)
        return {RestClientSubmitError::busy};
    submitted = *request;
    accepted = id;
    return {};
}
void permit(u64 bits = kPermissionViewChannel | kPermissionReadMessageHistory) noexcept {
    auto& state = g_app.account_ui->chat_state;
    chat_state_store_upsert_role(&state, {7, 7, bits, utf8_literal("Everyone")});
    chat_state_store_set_self_roles(&state, 7, 99, 5, {});
    chat_state_store_recompute_guild_permissions(&state, 7);
}
bool seed(u64 id = 900, bool multiple = false, bool known = true, u16 mine = 0, u16 layout = 1,
          u32 count = 0) noexcept {
    if (!message_store_upsert(&g_app.message_view.store,
                              {id, 70, 3, 100, 0, utf8_literal("A received poll")})
             .ok())
        return false;
    GatewayMessagePollAnswer answers[]{{7, utf8_literal("Alpha"), 0, {}, false},
                                       {42, utf8_literal("Beta"), 0, {}, false}};
    GatewayMessagePollAnswerCount counts[]{{7, count, (mine & 1) != 0}, {42, 0, (mine & 2) != 0}};
    GatewayMessagePoll p{utf8_literal("Choice"),
                         {answers, 2},
                         known ? ConstSlice<GatewayMessagePollAnswerCount>{counts, 2}
                               : ConstSlice<GatewayMessagePollAnswerCount>{},
                         10000,
                         gateway_message_poll_field_question | gateway_message_poll_field_answers |
                             gateway_message_poll_field_expiry | gateway_message_poll_field_multiselect |
                             gateway_message_poll_field_layout |
                             (known ? gateway_message_poll_field_results : 0u),
                         layout,
                         multiple,
                         false,
                         known,
                         false,
                         true};
    return message_poll_store_apply(&g_app.account_ui->message_poll_store, id, 70, p,
                                    ++g_app.account_ui->poll_gateway_serial)
        .ok();
}
bool state(UiPollState* out, ApplicationPollSource* source = nullptr) noexcept {
    static UiPollAnswer answers[10]{};
    return application_poll_state(900, {answers, 10}, out, source);
}
bool action(UiPollActionKind kind, u32 answer = 0) noexcept {
    UiPollState value{};
    ApplicationPollSource source{};
    return state(&value, &source) &&
           application_poll_action(source, {value.owner, value.generation, value.revision, kind, answer});
}
bool receipt(u64 id, u16 status, Utf8View text = {}, RestClientError error = {}) noexcept {
    RestClientResponse r{};
    r.request_id = id;
    r.status_code = status;
    r.error = error;
    r.body_bytes = text.bytes;
    if (id == accepted)
        accepted = 0;
    return application_poll_response(r, text);
}
void append(const char* value) noexcept {
    for (u32 i = 0; value[i]; ++i)
        body[body_bytes++] = value[i];
}
void number(u64 value) noexcept {
    char digits[20]{};
    u32 count{};
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        body[body_bytes++] = digits[--count];
}
Utf8View snapshot_body(u16 mine = 0, u32 count = 0, bool multiple = false,
                       bool known = true) noexcept {
    body_bytes = 0;
    // GET /channels/70/messages?limit=1&around=900 answers a one-message page.
    append("[{\"id\":\"900\",\"channel_id\":\"70\",\"poll\":{\"question\":{\"text\":\"Choice\"},\"answers\":"
           "[{\"answer_id\":7,\"poll_media\":{\"text\":\"Alpha\"}},{\"answer_id\":42,\"poll_media\":{"
           "\"text\":\"Beta\"}}],\"expiry\":\"1970-01-01T00:00:10.000Z\",\"allow_multiselect\":");
    append(multiple ? "true" : "false");
    if (!known) {
        append(",\"layout_type\":1}}]");
        return {body, body_bytes};
    }
    append(
        ",\"layout_type\":1,\"results\":{\"is_finalized\":false,\"answer_counts\":[{\"id\":7,\"count\":");
    number(count);
    append(",\"me_voted\":");
    append(mine & 1 ? "true" : "false");
    append("},{\"id\":42,\"count\":0,\"me_voted\":");
    append(mine & 2 ? "true" : "false");
    append("}]}}}]");
    return {body, body_bytes};
}
Utf8View voters_body(u64 first, u32 count) noexcept {
    body_bytes = 0;
    append("{\"users\":[");
    for (u32 i = 0; i < count; ++i) {
        if (i)
            append(",");
        append("{\"id\":\"");
        number(first + i);
        append("\",\"username\":\"voter\",\"global_name\":\"Voter\",\"avatar\":null,\"bot\":false}");
    }
    append("]}");
    return {body, body_bytes};
}
// Account change: a fresh store, with revisions moving on so bound strings rebind.
void clear_polls() noexcept {
    auto& polls = g_app.account_ui->message_poll_store;
    const u32 revision = polls.revision, content_revision = polls.content_revision;
    message_poll_store_release(&polls);
    message_poll_store_initialize_owned(&polls, 16, 8192);
    polls.revision = revision + 1;
    polls.content_revision = content_revision + 1;
}
void reset() noexcept {
    if (accepted)
        receipt(accepted, 500);
    application_poll_release();
    g_app.account.instance = &actor_token;
    g_app.account.status.running = true;
    g_app.account.status.gateway_ready = 1;
    g_app.account.status.profile_id = 2;
    g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.api.poll = submit;
    g_app.plugin_account_scope_serial = 3;
    g_app.login.visible = false;
    g_app.account_ui->active_channel_id = 70;
    g_app.account_ui->chat_state.selected_channel_id = 70;
    g_app.account_ui->chat_state.selected_guild_id = 7;
    g_app.load_trace.channel_epoch = 1;
    blocked = host_owned = search_enabled = false;
    search_publication = 17;
    admission = {};
    now = unix_now = 1000;
    clear_polls();
    permit();
    seed();
}
bool initialize() noexcept {
    g_app.renderer.text_measure_revision = 37;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    return chat_state_store_initialize(&ui.chat_state, {guilds, 2}, {guild_order, 2}, {channels, 8},
                                          {channel_order, 8}, {names, sizeof(names)}) &&
           chat_state_store_bind_permissions(&ui.chat_state, {roles, 4}, {overwrites, 4}) &&
           chat_state_store_upsert_guild(
               &ui.chat_state, {7, utf8_literal("Guild"), GuildMutationKind::create, 99})
               .ok() &&
           chat_state_store_upsert_channel(&ui.chat_state, {70, 7, 0, utf8_literal("Room"), 0, 0, 0})
               .ok() &&
           chat_state_store_upsert_channel(&ui.chat_state, {72, 7, 0, utf8_literal("Forum"), 1, 0, 15})
               .ok() &&
           chat_state_store_upsert_channel(&ui.chat_state, {73, 7, 72, utf8_literal("Post"), 2, 0, 11})
               .ok() &&
           message_store_initialize(&g_app.message_view.store, {messages, 16}, {message_order, 16},
                                    {message_text, sizeof(message_text)}) &&
           message_poll_store_initialize_owned(&ui.message_poll_store, 16, 8192) &&
           ui_text_measure_cache_initialize(&ui.text_measure_cache, {measure_rows, 128}, measure, nullptr);
}
// One real message scene row over the resident store, as the client lays it out.
struct SceneFixture {
    u32 heights[16];
    u64 fenwick[16];
    UiVariableList list;
    MessageTimeline timeline;
    UiWidget widgets[kMessageSceneWidgetsPerRow * 2 + 1];
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow * 2 + 1];
};
SceneFixture* scene_fixture{};
constexpr UiRect scene_viewport{0, 0, 600, 1400};
bool open_scene() noexcept {
    message_scene_release(&g_app.message_scene);
    if (!scene_fixture)
        scene_fixture = static_cast<SceneFixture*>(
            VirtualAlloc(nullptr, sizeof(SceneFixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!scene_fixture)
        return false;
    auto& f = *scene_fixture;
    f = {};
    auto& binding = g_app.account_ui->message_poll_binding;
    binding = {&g_app.account_ui->message_poll_store,
               &g_app.account_ui->text_measure_cache,
               unix_now,
               37,
               true,
               true,
               +[](void*, u64 id, Slice<UiPollAnswer> answers, UiPollState* out) noexcept {
                   return application_poll_state(id, answers, out);
               },
               nullptr};
    g_app.renderer.dpi = 96;
    return ui_variable_list_initialize(&f.list, {f.heights, 16}, {f.fenwick, 16},
                                       g_app.message_view.store.count, ui_extent_units(96)) &&
           message_timeline_initialize(&f.timeline, &g_app.message_view.store, &f.list, ui_extent_units(96),
                                       ui_extent_units(1400), false) &&
           message_scene_initialize_owned(&g_app.message_scene, &f.timeline, 2, 1,
                                          {f.widgets, kMessageSceneWidgetsPerRow * 2 + 1},
                                          {f.nodes, kMessageSceneWidgetsPerRow * 2 + 1}) &&
           message_scene_bind_poll_source(&g_app.message_scene, &binding) &&
           message_scene_update(&g_app.message_scene, scene_viewport).ok() && g_app.message_scene.count == 1;
}
// The text convergence that follows a model change.
bool converge() noexcept {
    g_app.account_ui->message_poll_binding.now_ms = unix_now;
    return message_scene_update(&g_app.message_scene, scene_viewport).ok() && application_poll_synchronize();
}
// A received poll in each shape the client must keep interactive.
struct PollShape {
    u64 channel = 70;
    u32 answers = 2;
    bool long_text = false, multiple = false, layout = true, results = true;
    u8 expiry = 1; // 0 omitted, 1 at 10 s, 2 null
};
bool seed_shape(const PollShape& shape, bool history = false) noexcept {
    clear_polls();
    message_store_delete(&g_app.message_view.store, 900);
    if (!message_store_upsert(&g_app.message_view.store, {900, shape.channel, 3, 100, 0, utf8_literal("")}).ok())
        return false;
    static char text[10][128];
    GatewayMessagePollAnswer answers[10]{};
    GatewayMessagePollAnswerCount counts[10]{};
    for (u32 i = 0; i < shape.answers; ++i) {
        u32 bytes = 0;
        // 55 two-byte scalars: the longest answer Discord accepts.
        for (u32 c = 0; c < (shape.long_text ? 55u : 3u); ++c) {
            if (shape.long_text) {
                text[i][bytes++] = static_cast<char>(0xC3);
                text[i][bytes++] = static_cast<char>(0xA0 + i);
            } else
                text[i][bytes++] = static_cast<char>('a' + i);
        }
        answers[i] = {100 + i * 3, {text[i], bytes}, 0, {}, false};
        counts[i] = {100 + i * 3, 0, false};
    }
    GatewayMessagePoll p{};
    p.question = utf8_literal("Choice");
    p.answers = {answers, shape.answers};
    p.answer_counts = shape.results ? ConstSlice<GatewayMessagePollAnswerCount>{counts, shape.answers}
                                    : ConstSlice<GatewayMessagePollAnswerCount>{};
    p.expiry_ms = shape.expiry == 1 ? 10000 : 0;
    p.expiry_null = shape.expiry == 2;
    p.fields = gateway_message_poll_field_question | gateway_message_poll_field_answers |
               gateway_message_poll_field_multiselect | (shape.expiry ? gateway_message_poll_field_expiry : 0u) |
               (shape.layout ? gateway_message_poll_field_layout : 0u) |
               (shape.results ? gateway_message_poll_field_results : 0u);
    p.layout_type = 1;
    p.allow_multiselect = shape.multiple;
    p.results_known = shape.results;
    p.present = true;
    if (history) {
        const MessagePollPageInput input{900, shape.channel, &p, 0, nullptr};
        if (!message_poll_store_apply_page(&g_app.account_ui->message_poll_store, {&input, 1}).ok())
            return false;
    } else if (!message_poll_store_apply(&g_app.account_ui->message_poll_store, 900, shape.channel, p,
                                         ++g_app.account_ui->poll_gateway_serial)
                    .ok())
        return false;
    g_app.account_ui->active_channel_id = shape.channel;
    g_app.account_ui->chat_state.selected_channel_id = shape.channel;
    return open_scene();
}
UiRect target(UiPollTarget t) noexcept {
    if (owner && owner->card.active)
        return ui_poll_target_rectangle(owner->card.projection, t);
    static UiPollAnswer answers[10]{};
    UiPollProjection projection{};
    return g_app.message_scene.count &&
                   message_scene_poll_projection(g_app.message_scene, g_app.message_scene.rows[0],
                                                 application_theme(), {answers, 10}, &projection)
               ? ui_poll_target_rectangle(projection, t)
               : UiRect{};
}
LPARAM point_of(UiRect r) noexcept {
    const auto x = static_cast<u32>((r.left + r.right) * .5f), y = static_cast<u32>((r.top + r.bottom) * .5f);
    return static_cast<LPARAM>((y << 16) | (x & 0xffffu));
}
// A press and release through the window procedure, as the user clicks.
bool click(UiPollTarget t) noexcept {
    const auto r = target(t);
    if (!ui_rect_has_area(r))
        return false;
    const LPARAM at = point_of(r);
    const bool down = application_poll_window_message(WM_LBUTTONDOWN, 0, at);
    return application_poll_window_message(WM_LBUTTONUP, 0, at) && down;
}
UiPollTarget answer(u32 index) noexcept {
    return {UiPollControl::answer, 100 + index * 3};
}
// Opens the card by clicking, selects one answer, clicks Vote and checks the PUT.
bool votes(const PollShape& shape, u32 index, bool history = false) noexcept {
    reset();
    return seed_shape(shape, history) && click(answer(index)) && owner && owner->selection == (1u << index) &&
           click({UiPollControl::vote}) && application_poll_submit(400 + index, now) &&
           submitted.operation == PollOperation::vote && submitted.answer_count == 1 &&
           submitted.answer_ids[0] == 100 + index * 3 && submitted.channel_id == shape.channel;
}
bool own_vote(const GatewayMessagePollVoteEvent& event) noexcept {
    const auto applied = message_poll_store_vote(&g_app.account_ui->message_poll_store, event, 5,
                                                 ++g_app.account_ui->poll_gateway_serial);
    application_poll_observe(event, applied.ok());
    return applied.ok();
}
void scene_cases() noexcept {
    reset();
    message_store_upsert(&g_app.message_view.store, {900, 70, 3, 100, 0, utf8_literal("")});
    check(open_scene() && click({UiPollControl::answer, 7}) && owner && owner->card.active &&
              owner->selection == 1,
          "poll scene: a click on a received card opens it and selects the answer");
    check(owner->card.active && owner->card.projection.state.answers[0].selected &&
              !owner->card.projection.state.answers[1].selected && click({UiPollControl::answer, 42}) &&
              owner->selection == 2 && !owner->card.projection.state.answers[0].selected &&
              owner->card.projection.state.answers[1].selected && click({UiPollControl::answer, 7}) &&
              owner->selection == 1 && owner->card.projection.state.answers[0].selected,
          "poll scene: the card shows each selected answer right after the click without a pump");
    // Lockup: the pump stopped the account whenever synchronizing returned false.
    const GatewayMessagePollVoteEvent other{70, 900, 77, 7, 42, true, true};
    own_vote(other);
    const auto* p = message_poll_store_find(g_app.account_ui->message_poll_store, 900);
    check(g_app.message_scene.rows[0].poll_result_revision != p->result_revision &&
              application_poll_synchronize() && owner->card.active &&
              owner->card.projection.state.answers[1].count == 1,
          "poll lockup: a vote event before the row converges keeps synchronizing and reprojects the card");
    // Busy poll: a vote event between press and release keeps the press.
    const LPARAM vote_at = point_of(target({UiPollControl::vote}));
    application_poll_window_message(WM_LBUTTONDOWN, 0, vote_at);
    own_vote({70, 900, 78, 7, 42, true, true});
    application_poll_synchronize();
    application_poll_window_message(WM_LBUTTONUP, 0, vote_at);
    check(owner->controller.desired_active && application_poll_submit(300, now) &&
              submitted.operation == PollOperation::vote && submitted.answer_ids[0] == 7,
          "poll busy: a release after another user's vote still sends the vote");
    UiPollState value{};
    check(state(&value) && value.loading && value.results && value.answers[0].me_voted &&
              value.answers[0].count == 1 && converge() && owner->card.active,
          "poll optimistic: the pending vote shows its count and own mark at once");
    own_vote({70, 900, 5, 7, 7, true, true});
    check(application_poll_synchronize() && receipt(300, 204) && converge() && state(&value) &&
              value.answers[0].count == 1 && value.answers[1].count == 2 && value.can_remove_vote,
          "poll counts: the own echo before the 204 neither stops the session nor counts twice");
    check(click({UiPollControl::remove_vote}) && application_poll_submit(301, now) &&
              submitted.operation == PollOperation::remove_vote && receipt(301, 204) && converge() &&
              state(&value) && !value.answers[0].me_voted && !value.answers[0].count && value.can_vote,
          "poll remove: Remove Vote sends the empty PUT and clears the own vote");
    check(click({UiPollControl::answer, 7}) && click({UiPollControl::vote}) && application_poll_submit(302, now) &&
              receipt(302, 403) && converge() && state(&value) && !value.answers[0].me_voted &&
              !value.answers[0].count && value.status.bytes,
          "poll rollback: a rejected vote restores the counts and shows a notice");

    // A vote outliving its card (channel switch) is neither cancelled nor lost.
    reset();
    message_store_upsert(&g_app.message_view.store, {900, 70, 3, 100, 0, utf8_literal("")});
    const u32 cancels = cancellations;
    check(open_scene() && click({UiPollControl::answer, 42}) && click({UiPollControl::vote}) &&
              application_poll_submit(310, now),
          "poll switch: vote admitted");
    g_app.load_trace.channel_epoch = 2;
    application_poll_synchronize();
    p = message_poll_store_find(g_app.account_ui->message_poll_store, 900);
    check(cancellations == cancels && receipt(310, 204) && p->answers[1].me_voted && p->answers[1].count == 1,
          "poll switch: leaving the channel keeps the PUT and its 204 applies the own vote");

    PollShape shape{};
    shape.answers = 10;
    shape.long_text = true;
    check(votes(shape, 9), "poll shapes: ten 55-scalar answers keep every row clickable");
    shape = {};
    shape.multiple = true;
    reset();
    check(seed_shape(shape) && click(answer(0)) && click(answer(1)) && owner->selection == 3 &&
              click({UiPollControl::vote}) && application_poll_submit(320, now) && submitted.answer_count == 2,
          "poll shapes: multiselect sends every selected answer");
    shape = {};
    shape.layout = false;
    shape.expiry = 2;
    check(votes(shape, 0), "poll shapes: omitted layout_type and null expiry stay votable");
    shape = {};
    shape.expiry = 0;
    check(votes(shape, 1), "poll shapes: omitted expiry stays votable");
    shape = {};
    shape.channel = 73;
    check(votes(shape, 1), "poll shapes: a poll in a forum post thread is votable");
    shape = {};
    check(votes(shape, 0, true), "poll shapes: a poll applied from a history page is votable");
    reset();
    search_enabled = true;
    check(seed_shape({}) && click(answer(1)) && owner && owner->selection == 2,
          "poll shapes: timeline polls stay clickable while search results are open");
    search_enabled = false;
    // History without results: Retry reads authority before voting.
    reset();
    shape = {};
    shape.results = false;
    check(seed_shape(shape, true) && ui_rect_has_area(target({UiPollControl::retry})) &&
              (click({UiPollControl::retry}), owner && owner->card.active) && application_poll_submit(330, now) &&
              submitted.operation == PollOperation::refresh,
          "poll unknown results: Retry opens the card and reads the message before voting");
    // Ended: the first click reads the final tally, then answers open the voters.
    reset();
    message_store_upsert(&g_app.message_view.store, {900, 70, 3, 100, 0, utf8_literal("")});
    unix_now = 20000;
    check(open_scene() && converge() && ui_rect_has_area(target({UiPollControl::answer, 42})) &&
              !ui_rect_has_area(target({UiPollControl::vote})) &&
              (click({UiPollControl::answer, 42}), application_poll_submit(340, now)) &&
              submitted.operation == PollOperation::refresh && receipt(340, 200, snapshot_body()) && converge() &&
              click({UiPollControl::answer, 42}) && application_poll_submit(341, now) &&
              submitted.operation == PollOperation::voters && submitted.answer_id == 42,
          "poll ended: a closed poll offers no vote and its answers open the voters");
    unix_now = 1000;
    application_poll_release();
    message_scene_release(&g_app.message_scene);
    if (scene_fixture)
        VirtualFree(scene_fixture, 0, MEM_RELEASE);
    scene_fixture = nullptr;
}
void run() noexcept {
    check(initialize(), "poll app: real Core, resident store and lazy poll store initialize");
    if (!g_app.account_ui)
        return;
    g_app.account_ui->chat_state.self_user_id = 5;
    reset();
    UiPollState value{};
    ApplicationPollSource source{};
    check(state(&value, &source) && value.can_vote && !owner && !application_poll_metrics().owner_bytes,
          "poll app: readable received cards stay owner-free and do not require creation/SEND permission");
    application_poll_open(source);
    state(&value, &source);
    const UiPollAction retired_action{value.owner, value.generation, value.revision,
                                      UiPollActionKind::toggle_answer, 7};
    const u64 retired_generation = value.generation;
    application_poll_release();
    application_poll_open(source);
    state(&value);
    check((retired_generation & 1) && (value.generation & 1) && retired_generation != value.generation &&
              !application_poll_action(source, retired_action),
          "poll app: freed owner action cannot activate reopened same message and structural poll");
    check(application_poll_action(source, {value.owner, value.generation, value.revision,
                                           UiPollActionKind::toggle_answer, 7}) &&
              owner->selection == 1,
          "poll app: reopened owner accepts its own current semantic generation");
    application_poll_release();
    check(action(UiPollActionKind::toggle_answer, 42) && owner && owner->selection == 2,
          "poll app: nonsequential answer ID captures one lazy selected owner");
    check(action(UiPollActionKind::toggle_answer, 7) && owner->selection == 1,
          "poll app: single selection replaces rather than accumulates answers");
    check(action(UiPollActionKind::vote) && application_poll_submit(100, now) &&
              submitted.operation == PollOperation::vote && submitted.answer_count == 1 &&
              submitted.answer_ids[0] == 7,
          "poll app: typed immutable accepted flight uses actual selected answer IDs");
    const auto original_count =
        message_poll_store_find(g_app.account_ui->message_poll_store, 900)->answers[0].count;
    check(!action(UiPollActionKind::vote) && original_count == 0,
          "poll app: duplicate vote input is disabled and no authoritative count is fabricated");
    check(receipt(100, 204) && !application_poll_pending(now) && state(&value) && !value.can_vote &&
              value.can_remove_vote && value.answers[0].count == 1 && value.answers[0].me_voted &&
              value.results && !owner->controller.desired_active && !owner->controller.refresh_required,
          "poll app: an accepted vote (204) applies like official vote reactions and refetches nothing");
    GatewayMessagePollVoteEvent echo{70, 900, 5, 7, 7, true, true};
    const auto echoed = message_poll_store_vote(&g_app.account_ui->message_poll_store, echo, 5,
                                                ++g_app.account_ui->poll_gateway_serial);
    application_poll_observe(echo, echoed.ok());
    check(echoed.ok() && !echoed.changed && state(&value) && value.answers[0].count == 1,
          "poll app: the gateway echo of the accepted vote never counts twice");
    check(action(UiPollActionKind::remove_vote) && application_poll_submit(102, now) &&
              submitted.operation == PollOperation::remove_vote && !submitted.answer_count,
          "poll app: removing own vote maps to typed PUT-empty selection");
    GatewayMessagePollVoteEvent removed{70, 900, 5, 7, 7, false, true};
    const auto applied = message_poll_store_vote(&g_app.account_ui->message_poll_store, removed, 5,
                                                 ++g_app.account_ui->poll_gateway_serial);
    application_poll_observe(removed, applied.ok());
    check(receipt(102, 0, {}, RestClientError::receive) && !owner->controller.pending_ready &&
              !owner->controller.desired_active,
          "poll app: matching authoritative Gateway removal beats ambiguous late HTTP and does not replay");

    reset();
    seed(900, true);
    check(action(UiPollActionKind::toggle_answer, 7) && action(UiPollActionKind::toggle_answer, 42) &&
              action(UiPollActionKind::vote) && application_poll_submit(110, now) &&
              submitted.answer_count == 2 && submitted.answer_ids[0] == 7 && submitted.answer_ids[1] == 42,
          "poll app: multi-select copies both answer IDs without message/poll borrowed pointers");
    check(receipt(110, 500) && application_poll_submit(111, now) &&
              submitted.operation == PollOperation::refresh,
          "poll app: ambiguous mutation performs recovery GET rather than automatic vote repeat");
    check(receipt(111, 200, snapshot_body(0, 0, true)) && state(&value) && value.can_vote &&
              !owner->controller.desired_active,
          "poll app: known empty fresh authority resolves uncertainty for explicit user resubmission");

    // Regression: prior successful GET must not waive the new uncertainty fence.
    check(action(UiPollActionKind::toggle_answer, 7) && action(UiPollActionKind::vote) &&
              application_poll_submit(112, now),
          "poll app: after recovery a new exact vote is admitted");
    const u32 cancels_before_disconnect = cancellations;
    g_app.account.status.gateway_ready = 0;
    application_poll_synchronize();
    check(owner->controller.flight.retired && cancellations == cancels_before_disconnect &&
              !owner->source_checked && !owner->reconcile_requested,
          "poll app: disconnect retires the vote flight without cancelling a PUT that may have landed, "
          "and invalidates prior checked authority");
    receipt(112, 500);
    g_app.account.status.gateway_ready = 1;
    application_poll_synchronize();
    check(state(&value) && !value.can_vote && application_poll_submit(113, now) &&
              submitted.operation == PollOperation::refresh,
          "poll app: reconnect cannot vote on cached empty state before a fresh GET");
    receipt(113, 200, snapshot_body(0, 0, true));
    check(state(&value) && value.can_vote,
          "poll app: only fresh known GET clears reconnect ambiguity barrier");

    reset();
    clear_polls();
    check(seed(900, false, false) && state(&value, &source) && !value.results_known &&
              !value.answers[0].count_known && !value.answers[0].me_known &&
              !value.answers[1].me_known && !value.can_vote && !value.can_remove_vote && value.can_retry &&
              !action(UiPollActionKind::toggle_answer, 7) && !action(UiPollActionKind::vote),
          "poll app: received omitted results expose unknown state and block selection and vote");
    check(application_poll_open(source) && application_poll_pending(now) &&
              application_poll_submit(114, now) &&
              submitted.operation == PollOperation::refresh && submitted.channel_id == 70 &&
              submitted.message_id == 900,
          "poll app: unknown membership reconciles through the exact scoped read before voting");
    const u32 before_unknown_receipt = submissions;
    check(receipt(114, 200, snapshot_body(0, 0, false, false)) && state(&value) &&
              !value.results_known && !value.answers[0].count_known && !value.answers[0].me_known &&
              !value.can_vote && value.can_retry && !owner->source_checked &&
              !application_poll_pending(now) && !application_poll_pending(now + 60000) &&
              submissions == before_unknown_receipt && !action(UiPollActionKind::vote),
          "poll app: successful read with omitted results remains unknown without an automatic repeat");
    check(action(UiPollActionKind::retry) && application_poll_submit(115, now) &&
              submitted.operation == PollOperation::refresh &&
              receipt(115, 200, snapshot_body(2, 9)) && state(&value) && value.results_known &&
              value.answers[1].me_known && value.answers[1].me_voted && !value.can_vote &&
              value.can_remove_vote && !action(UiPollActionKind::toggle_answer, 7) &&
              !action(UiPollActionKind::vote),
          "poll app: explicit recovery reveals an existing own vote and prevents replacement selection");
    check(seed(900, false, false) && state(&value) && value.results_known &&
              value.answers[0].count_known && value.answers[0].count == 9 &&
              value.answers[1].me_voted && value.can_remove_vote,
          "poll app: later metadata with omitted results retains known counts and own selection");

    reset();
    clear_polls();
    seed(900, false, false);
    state(&value, &source);
    application_poll_open(source);
    check(application_poll_submit(116, now) && submitted.operation == PollOperation::refresh,
          "poll app: unknown source admits a copied refresh before actor replacement");
    g_app.account.instance = &other_actor;
    check(receipt(116, 200, snapshot_body(2, 9)) &&
              !message_poll_store_find(g_app.account_ui->message_poll_store, 900)->results_known &&
              !message_poll_store_find(g_app.account_ui->message_poll_store, 900)->answers[1].me_known,
          "poll app: held results from an old account instance cannot authorize unknown membership");

    reset();
    action(UiPollActionKind::toggle_answer, 7);
    action(UiPollActionKind::vote);
    application_poll_submit(120, now);
    const auto delete_source = owner->source;
    message_store_delete(&g_app.message_view.store, 900);
    application_poll_message_deleted(70, 900);
    check(owner->controller.flight.active && owner->controller.flight.retired &&
              !application_poll_source_current(delete_source),
          "poll app: deleted source retains copied exact drain while captured handle is retired");
    check(receipt(120, 200) && !application_poll_active(),
          "poll app: held success cannot revive a deleted source");
    reset();
    state(&value, &source);
    g_app.load_trace.channel_epoch = 2;
    check(!application_poll_action(
              source, {value.owner, value.generation, value.revision, UiPollActionKind::toggle_answer, 7}),
          "poll app: channel epoch retires captured card action even when IDs are unchanged");
    reset();
    state(&value, &source);
    permit(kPermissionViewChannel);
    check(!application_poll_source_current(source) && !action(UiPollActionKind::vote),
          "poll app: losing read history rejects source and mutation");
    reset();
    state(&value, &source);
    g_app.account.instance = &other_actor;
    check(!application_poll_source_current(source),
          "poll app: account instance replacement fences identical message IDs");
    reset();
    seed(900, false, true, 0, 2);
    state(&value);
    check(!value.supported && !value.can_vote && !action(UiPollActionKind::toggle_answer, 7),
          "poll app: future layout remains readable but voting is unsupported");
    reset();
    action(UiPollActionKind::toggle_answer, 7);
    action(UiPollActionKind::vote);
    unix_now = 10000;
    application_poll_synchronize();
    check(!owner->controller.pending_ready ||
              owner->controller.pending.kind != PollControllerWorkKind::vote,
          "poll app: exact expiry cancels queued vote before transport admission");
    check(application_poll_submit(130, now) && submitted.operation == PollOperation::refresh,
          "poll app: closed poll may read final authority without sending the expired mutation");
    reset();
    state(&value, &source);
    blocked = true;
    check(!application_poll_source_current(source) && !action(UiPollActionKind::vote),
          "poll app: durable logout blocks poll authority");
    blocked = false;
    host_owned = true;
    check(!application_poll_source_current(source),
          "poll app: transferred host cannot be accessed through retained poll source");

    reset();
    search_enabled = true;
    check(!application_poll_search_state(900, search_publication, {}, &value, &source),
          "poll app: Search projection rejects missing caller answer storage atomically");
    UiPollAnswer search_answers[10]{};
    check(application_poll_search_state(900, search_publication, {search_answers, 10}, &value, &source) &&
              source.source.channel_id == 70 && source.surface == UiPollSourceSurface::search &&
              application_poll_open(source),
          "poll app: Search outer source_channel_id zero resolves the actual message channel with accepted "
          "publication");
    ++search_publication;
    check(!application_poll_source_current(source),
          "poll app: replaced Search publication retires identical message and handle authority");

    reset();
    seed(900, false, true, 0, 1, 27);
    state(&value, &source);
    application_poll_open(source);
    check(application_poll_voters(7) && application_poll_submit(140, now) && submitted.limit == 25 &&
              !submitted.after_user_id,
          "poll app: dedicated native inspector sends bounded first voter page");
    {
        UiResolvedThemeStyles styles{};
        styles.ready = true;
        styles.revision = styles.geometry_revision = 1;
        styles.metrics.poll_padding = 20.f;
        styles.metrics.poll_gap = 12.f;
        styles.metrics.poll_target = 40.f;
        styles.metrics.control_height = 40.f;
        styles.metrics.control_radius = 12.f;
        fixture_theme.styles = &styles;
        fixture_theme.style_revision = 1;
        check(project_voters() && owner && owner->scene.active && owner->scene.style.padding == 24.f &&
                  owner->scene.style.gap == 12.f && owner->scene.style.target == 40.f &&
                  owner->scene.style.row_height == 52.f && owner->scene.style.radius == 12.f &&
                  measured_backend == 37,
              "poll app: actual voter modal consumes resolved metrics and renderer measurement backend");
        fixture_theme = kDefaultTheme;
        check(project_voters() && owner && owner->scene.style.padding == 20.f &&
                  owner->scene.style.target == 38.f && measured_backend == 37,
              "poll app: voter modal restores defaults without borrowing an expired style bank");
    }
    check(receipt(140, 200, voters_body(100, 25)) && application_poll_view().voters.count == 25 &&
              application_poll_view().voters_more,
          "poll app: strict voters object owns labels and proves another page from known count");
    check(application_poll_voters(7, true) && application_poll_submit(141, now) &&
              submitted.after_user_id == 124,
          "poll app: next uses actual last returned voter cursor");
    check(receipt(141, 200, voters_body(125, 2)) && !application_poll_view().voters_more &&
              application_poll_view().voter_page == 1,
          "poll app: known final page shows real rows with no invented total or truncation");
    check(application_poll_voters_previous() && application_poll_submit(142, now) &&
              !submitted.after_user_id && receipt(142, 200, voters_body(100, 25)),
          "poll app: previous replays its captured page cursor through actual protocol");
    const auto warm_frames = frames, warm_measures = measures, warm_layout = owner->widgets.layout.revision;
    const auto warm_revision = owner->revision;
    const auto warm_submissions = submissions;
    for (u32 i = 0; i < 8; ++i)
        application_poll_synchronize();
    check(frames == warm_frames && measures == warm_measures &&
              owner->widgets.layout.revision == warm_layout && owner->revision == warm_revision &&
              submissions == warm_submissions,
          "poll app: unchanged inspector synchronizations add no layout/measurement/frame/request work");
    check(application_poll_input(UiInputKind::key_down, VK_ESCAPE, false) &&
              !application_poll_voters_active() &&
              application_poll_consume_key_tail(UiInputKind::character, VK_ESCAPE, false) &&
              application_poll_consume_key_tail(UiInputKind::key_up, VK_ESCAPE, false),
          "poll app: closing inspector consumes exact Escape tail before underlying content");
    const u32 cached_submissions = submissions;
    check(application_poll_voters(7) && !application_poll_pending(now) && submissions == cached_submissions &&
              application_poll_view().voters.count == 25 && application_poll_view().voters_more &&
              application_poll_view().voters[24].id == 124,
          "poll voters cache: reopening an answer with unchanged results shows its first page without a request");
    application_poll_voters_close();
    const GatewayMessagePollVoteEvent counted{70, 900, 99, 7, 42, true, true};
    application_poll_observe(counted, message_poll_store_vote(&g_app.account_ui->message_poll_store, counted, 5,
                                                              ++g_app.account_ui->poll_gateway_serial)
                                          .ok());
    application_poll_voters(7);
    check(application_poll_submit(143, now) && submitted.answer_id == 7,
          "poll voters cache: a vote count change refetches the first page");
    application_poll_voters(42);
    check(owner->controller.flight.active && owner->controller.flight.retired && submitted.answer_id == 7 &&
              receipt(143, 200, voters_body(100, 25)) && !application_poll_view().voters.count &&
              application_poll_submit(144, now) && submitted.answer_id == 42,
          "poll app: switching answer cancels old page and never retargets its held receipt");
    receipt(144, 200, voters_body(200, 1));
    application_poll_voters(7);
    application_poll_submit(145, now);
    g_app.account.status.running = false;
    application_poll_release();
    check(owner && owner->release_pending && owner->controller.flight.active,
          "poll app: stopped status alone cannot free source ownership before joined stop/receipt");
    receipt(145, 0, {}, RestClientError::cancelled);
    check(!owner && !g_app.renderer.poll_scene && !g_app.renderer.poll_voters_scene,
          "poll app: exact cancelled drain frees lazy owner and unbinds both native scenes");
    scene_cases();
    message_poll_store_release(&g_app.account_ui->message_poll_store);
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::poll_app_test
namespace maicord {
bool renderer_bind_poll_scene(D3D11Renderer* renderer, const UiPollScene* scene) noexcept {
    renderer->poll_scene = scene;
    return true;
}
bool renderer_bind_poll_voters_scene(D3D11Renderer* renderer, const UiPollVotersScene* scene) noexcept {
    renderer->poll_voters_scene = scene;
    return true;
}
namespace app_detail {
Application g_app{};
const Theme& application_theme() noexcept {
    return poll_app_test::fixture_theme;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, 800, 700};
}
bool application_request_frame() noexcept {
    ++poll_app_test::frames;
    return true;
}
bool application_account_logout_blocked() noexcept {
    return poll_app_test::blocked;
}
bool application_account_logout_host_owned() noexcept {
    return poll_app_test::host_owned;
}
bool application_profile_open(u64, const UiRect&, ProfileAnchor) noexcept {
    return true;
}
bool application_search_active() noexcept {
    return poll_app_test::search_enabled;
}
UiSearchScene* application_search_scene() noexcept {
    return nullptr;
}
bool application_search_source_ticket(u32, ApplicationSearchSourceTicket*) noexcept {
    return false;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* out) noexcept {
    if (!poll_app_test::search_enabled || snapshot || !out || ticket.message_id != 900 ||
        ticket.publication_revision != poll_app_test::search_publication || ticket.index)
        return false;
    const auto found = message_store_find(g_app.message_view.store, ticket.message_id);
    if (!found.ok())
        return false;
    *out = {ticket,
            &g_app.message_view.store,
            message_store_resolve(g_app.message_view.store, found.handle),
            found.handle,
            nullptr,
            nullptr,
            {},
            {},
            0,
            7,
            false};
    return true;
}
bool application_search_source_resolve(u64 id, u32 source_revision, bool snapshot,
                                       ApplicationSearchSourceView* out) noexcept {
    return application_search_source_resolve({id, source_revision, 0}, snapshot, out);
}
} // namespace app_detail
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_application_poll_entry() noexcept {
    maicord::poll_app_test::run();
    ExitProcess(maicord::poll_app_test::failures ? 1 : 0);
}

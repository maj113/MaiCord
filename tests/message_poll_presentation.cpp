#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
namespace {
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* out) noexcept {
    const auto text = utf8_validate(request.text);
    if (!out || !text.valid || request.constraint.width <= 0)
        return false;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 6));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (text.codepoints + columns - 1u) / columns);
    *out = {{minimum(request.constraint.width, static_cast<float>(text.codepoints) * 6.f),
             static_cast<float>(lines) * 16.f},
            12,
            lines};
    return true;
}
struct Fixture {
    MessageRecord records[1]{};
    u32 order[1]{};
    u8 bytes[256]{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
    MessageStore store{};
    MessagePollStore polls{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    UiTextMeasureRecord cache_records[128]{};
    UiTextMeasureCache cache{};
    MessageScenePollBinding binding{};
    MessageTextCommand commands[96]{};
    char display[4096]{};
    TextStyleSpan styles[96]{};
    MessageMarkdownSemantic semantics[8]{};
    MessageTextMeasurement measurements[4]{};
    MessageTextSpoilerReveal reveals[2]{};
    u32 scratch[512]{};
    MessageTextPresentation text{};
    bool initialize(void (*check)(bool, const char*) noexcept) noexcept {
        GatewayMessagePollAnswer answers[]{
            {1, utf8_literal("First answer with enough original text to wrap"), 0, {}, false},
            {2, utf8_literal("Second original answer with Unicode \342\230\203"), 0, {}, false}};
        GatewayMessagePollAnswerCount counts[]{{1, 2, false}, {2, 3, false}};
        GatewayMessagePoll poll{};
        poll.present = poll.results_known = true;
        poll.question = utf8_literal("An original question that wraps in a narrow message body");
        poll.answers = {answers, 2};
        poll.answer_counts = {counts, 2};
        poll.fields = gateway_message_poll_field_question | gateway_message_poll_field_answers |
                      gateway_message_poll_field_layout | gateway_message_poll_field_results;
        poll.layout_type = 1;
        const auto require = [check](bool value, const char* label) noexcept {
            check(value, label);
            return value;
        };
        if (!require(message_poll_store_initialize_owned(&polls, 2, 4096),
                     "poll message setup: lazy poll storage initializes") ||
            !require(message_poll_store_apply(&polls, 4400, 55, poll, 1).ok(),
                     "poll message setup: original Unicode metadata applies") ||
            !require(message_store_initialize(&store, {records, 1}, {order, 1}, {bytes, sizeof(bytes)}) &&
                         message_store_upsert(&store, {4400, 55, 91, 0, 0, utf8_literal(""), 0}).ok(),
                     "poll message setup: authoritative message is resident") ||
            !require(
                ui_variable_list_initialize(&list, {heights, 1}, {fenwick, 1}, 1, ui_extent_units(96)) &&
                    message_timeline_initialize(&timeline, &store, &list, ui_extent_units(96),
                                                ui_extent_units(1200), false),
                "poll message setup: timeline has one resident item") ||
            !require(ui_text_measure_cache_initialize(&cache, {cache_records, 128}, measure, nullptr),
                     "poll message setup: wrapping cache initializes") ||
            !require(message_scene_initialize_owned(&scene, &timeline, 1, 1,
                                                    {widgets, kMessageSceneWidgetsPerRow + 1},
                                                    {nodes, kMessageSceneWidgetsPerRow + 1}),
                     "poll message setup: native lazy scene initializes"))
            return false;
        binding.store = &polls;
        binding.measure = &cache;
        binding.backend_revision = 1;
        binding.can_vote = binding.can_view_voters = true;
        return require(message_scene_bind_poll_source(&scene, &binding),
                       "poll message setup: native poll binding admits measured source") &&
               require(message_scene_update(&scene, {0, 0, 250, 1200}).ok(),
                       "poll message setup: narrow measured row publishes") &&
               require(message_text_presentation_initialize(
                           &text, {commands, 96}, {display, sizeof(display)}, {styles, 96}, {semantics, 8},
                           {measurements, 4}, {reveals, 2}, {scratch, 512}),
                       "poll message setup: owning text bank initializes");
    }
    void release() noexcept {
        message_scene_release(&scene);
        message_poll_store_release(&polls);
    }
};
bool selected(void* source, u64 id, Slice<UiPollAnswer> answers, UiPollState* out) noexcept {
    const auto& fixture = *static_cast<const Fixture*>(source);
    const auto* poll = message_poll_store_find(fixture.polls, id);
    if (!poll || answers.count < poll->answer_count)
        return false;
    UiPollState state{};
    state.owner = id;
    state.generation = 1;
    state.revision = 20;
    state.content_revision = poll->content_revision;
    state.result_revision = poll->result_revision;
    state.question = message_poll_store_string(fixture.polls, poll->question);
    state.answers = {answers.data, poll->answer_count};
    state.supported = true;
    state.loading = true;
    state.status = utf8_literal("Sending vote");
    *out = state;
    return true;
}
} // namespace

void test_message_poll_presentation(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture fixture{};
    bool ready = fixture.initialize(check);
    check(ready, "poll message: lazy model binds one optional native row root");
    if (!ready) {
        fixture.release();
        return;
    }
    auto& row = fixture.scene.rows[0];
    const auto live = row.poll;
    const u32 widget_count = fixture.scene.widgets.count;
    check(row.poll_content_revision && row.poll_height > 160 &&
              widget_count == kMessageSceneBaseWidgetsPerRow + 2 &&
              message_scene_resolve(fixture.scene, live) == &row,
          "poll message: wrapped two-answer poll uses one root without answer child widgets");
    UiPollAnswer answers[kUiPollMaximumAnswers]{};
    UiPollProjection projection{};
    ready = message_scene_poll_projection(fixture.scene, row, kDefaultTheme,
                                          {answers, kUiPollMaximumAnswers}, &projection);
    check(ready && ui_rect_width(projection.geometry.bounds) < 160 &&
              ui_rect_height(projection.geometry.question) > 24,
          "poll message: 250DIP viewport admits a full wrapped narrow question with real measure-cache "
          "geometry");
    ++fixture.records[0].revision;
    ++fixture.store.updated;
    ready = ready && message_scene_update(&fixture.scene, fixture.scene.viewport).ok() &&
            message_scene_poll_projection(fixture.scene, row, kDefaultTheme,
                                          {answers, kUiPollMaximumAnswers}, &projection);
    const auto* shared_poll = message_poll_store_find(fixture.polls, 4400);
    check(ready && row.poll.slot == live.slot && row.poll.generation == live.generation && shared_poll &&
              projection.state.content_revision == shared_poll->content_revision,
          "poll message: independent local message revision preserves shared poll authority and root");
    const float height = row.poll_height;
    const u32 calls = fixture.cache.backend_calls;
    const auto vote = GatewayMessagePollVoteEvent{55, 4400, 90, 0, 1, true, false};
    ready = message_poll_store_vote(&fixture.polls, vote, 91, 2).ok() &&
            message_scene_update(&fixture.scene, fixture.scene.viewport).ok();
    check(ready && row.poll.slot == live.slot && row.poll.generation == live.generation &&
              row.poll_height == height && fixture.cache.backend_calls == calls &&
              fixture.scene.widgets.count == widget_count,
          "poll message: authoritative count-only update preserves root, wrapped height and measure "
          "backend work");
    ChatStateStore state{};
    NavigationView navigation{};
    ready = ready && message_text_presentation_build(&fixture.text, fixture.scene, state, nullptr,
                                                     navigation, kDefaultTheme, 0)
                         .ok();
    Utf8View retained{};
    bool plain_answer{}, styled_question{};
    for (u32 i = 0; i < fixture.text.command_count; ++i) {
        if (fixture.commands[i].owner.handle.slot == live.slot &&
            utf8_equal(fixture.commands[i].text,
                       utf8_literal("An original question that wraps in a narrow message body"))) {
            retained = fixture.commands[i].text;
            styled_question = fixture.commands[i].styles.count == 1 &&
                              fixture.commands[i].styles[0].flags == text_style_bold;
        }
        if (fixture.commands[i].owner.handle.slot == live.slot &&
            utf8_equal(fixture.commands[i].text,
                       utf8_literal("First answer with enough original text to wrap")))
            plain_answer = fixture.commands[i].styles.count == 1 &&
                           fixture.commands[i].styles[0].flags == kDefaultUiPollStyle.answer_style;
    }
    check(ready && plain_answer && styled_question,
          "poll message: answers keep the official text-sm/semibold style and the question stays bold");
    fixture.binding.project_state = selected;
    fixture.binding.context = &fixture;
    ready = ready && message_scene_poll_projection(fixture.scene, row, kDefaultTheme,
                                                   {answers, kUiPollMaximumAnswers}, &projection);
    check(ready && projection.state.loading &&
              utf8_equal(projection.state.status, utf8_literal("Sending vote")),
          "poll message: host selected-state callback projects pending status with caller-owned answers");
    fixture.binding.project_state = nullptr;
    message_poll_store_delete(&fixture.polls, 4400);
    check(ready &&
              utf8_equal(retained,
                         utf8_literal("An original question that wraps in a narrow message body")) &&
              !message_scene_poll_projection(fixture.scene, row, kDefaultTheme,
                                             {answers, kUiPollMaximumAnswers}, &projection),
          "poll message: retained question owns bytes after model retirement and stale source projection "
          "is rejected");
    ready = message_scene_update(&fixture.scene, fixture.scene.viewport).ok();
    check(
        ready && !fixture.scene.rows[0].poll_content_revision &&
            !ui_widget_manager_resolve(fixture.scene.widgets, live),
        "poll message: removed poll retires its widget generation without converting it to another source");
    fixture.records[0].presentation_flags |= message_presentation_has_poll;
    ++fixture.records[0].revision;
    ++fixture.store.updated;
    ready = message_scene_update(&fixture.scene, fixture.scene.viewport).ok() &&
            message_text_presentation_build(&fixture.text, fixture.scene, state, nullptr, navigation,
                                            kLightTheme, 0)
                .ok();
    bool unavailable{};
    for (u32 i = 0; i < fixture.text.command_count; ++i)
        unavailable = unavailable || utf8_equal(fixture.commands[i].text, kDefaultUiPollStyle.unavailable);
    check(ready && unavailable && fixture.scene.rows[0].poll_unavailable &&
              fixture.scene.rows[0].poll_height == 64 &&
              !ui_widget_manager_input_enabled(fixture.scene.widgets, fixture.scene.rows[0].poll),
          "poll message: admitted poll flag without retained metadata displays bounded unavailable shell "
          "with disabled mutation");
    fixture.release();
}
} // namespace maicord

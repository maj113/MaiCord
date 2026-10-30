#include "maicord/ui/poll_presentation.hpp"
#include "maicord/ui/poll_voters_avatar_media.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool contained(UiRect outer, UiRect inner) noexcept {
    return ui_rect_has_area(inner) && inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}
UiPoint center(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}
bool same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
// Deterministic wrapping oracle for component tests. Native tests use DirectWrite.
bool measure(void* context, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    if (!value || request.constraint.width <= 0 || !utf8_validate(request.text).valid)
        return false;
    // Normal labels must select the ordinary native measurement frontend.
    if (request.styles.count && request.styles[0].flags == text_style_none)
        return false;
    if (!request.styles.count)
        ++*static_cast<u32*>(context);
    float x{}, widest{};
    u32 lines = 1, offset{};
    while (offset < request.text.bytes) {
        const auto decoded = utf8_decode_one(request.text.data + offset, request.text.bytes - offset);
        offset += decoded.width;
        if (decoded.codepoint == '\n') {
            widest = maximum(widest, x);
            x = 0;
            ++lines;
            continue;
        }
        if (!request.no_wrap && x && x + 7 > request.constraint.width) {
            widest = maximum(widest, x);
            x = 0;
            ++lines;
        }
        x += 7;
    }
    *value = {{maximum(widest, x), static_cast<float>(lines) * 16}, 12, lines};
    return true;
}
struct Fixture {
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    UiWidgetManager manager{};
    UiWidgetHandle root{};
    UiTextMeasureRecord records[256]{};
    UiTextMeasureCache cache{};
    u32 unstyled_measures{};
    UiPollAnswer answers[3]{};
    UiPollState state{};
    bool initialize() noexcept {
        if (!ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2}) ||
            !ui_text_measure_cache_initialize(&cache, {records, 256}, measure, &unstyled_measures))
            return false;
        const auto added = ui_widget_manager_add(
            &manager, ui_widget_invalid_handle(),
            {1,
             UiWidgetKind::button,
             static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
             {utf8_literal("Poll owner"), ui_layout_style(), {}, 0, 1}});
        root = added.handle;
        answers[0].id = 7;
        answers[0].text = utf8_literal("A quiet walk beside the river");
        answers[1].id = 42;
        answers[1].text = utf8_literal("An original Unicode \342\230\203 idea");
        answers[1].emoji_name = utf8_literal("\342\230\203");
        answers[2].id = 900;
        answers[2].emoji_name = utf8_literal("\342\230\205");
        state.owner = 11;
        state.generation = 12;
        state.revision = state.content_revision = 1;
        state.question = utf8_literal("Which original activity should we choose together?");
        state.answers = {answers, 3};
        state.supported = state.can_vote = state.can_view_results = state.can_view_voters = true;
        return added.ok();
    }
    UiWidgetReference owner() noexcept {
        return {&manager, root};
    }
};
bool layout_clean(const UiPollProjection& p) noexcept {
    const auto& g = p.geometry;
    if (!contained(g.bounds, g.question) || !contained(g.bounds, g.instruction) ||
        !contained(g.bounds, g.summary) || g.question.bottom > g.instruction.top ||
        !ui_rect_equal(g.instruction, g.status))
        return false;
    // Every footer action lies in the footer band below the details text.
    const UiRect actions[]{g.vote, g.remove_vote, g.results, g.retry};
    for (const auto& action : actions)
        if (ui_rect_has_area(action) && (!contained(g.bounds, action) || action.top < g.summary.top - 16))
            return false;
    float bottom = g.instruction.bottom, height = 0;
    for (u32 i = 0; i < p.state.answers.count; ++i) {
        const auto& row = g.answers[i];
        // Label left, vote data, then the selection icon on the right.
        const bool percent = ui_rect_has_area(row.percent);
        if (!contained(g.bounds, row.bounds) || !contained(row.bounds, row.marker) ||
            !contained(row.bounds, row.text) || (percent && !contained(row.bounds, row.percent)) ||
            (percent && (row.text.right > row.percent.left || row.percent.right > row.marker.left)) ||
            (!percent && ui_rect_has_area(row.result)) || row.text.right > row.marker.left ||
            (ui_rect_has_area(row.result) && (!contained(row.bounds, row.result) || row.result.right > row.percent.left ||
                                              row.text.right > row.result.left)) ||
            row.bounds.top < bottom || (height && ui_rect_height(row.bounds) != height))
            return false;
        if (ui_rect_has_area(row.emoji) && (!contained(row.bounds, row.emoji) || row.emoji.right > row.text.left))
            return false;
        height = ui_rect_height(row.bounds);
        bottom = row.bounds.bottom;
    }
    return bottom <= g.summary.top;
}
void card_tests(Fixture& f, void (*check)(bool, const char*) noexcept) noexcept {
    // The official card is at least 302 wide; narrower hosts still lay out.
    const float widths[]{224, 302, 320, 400, 480};
    const Theme* themes[]{&kDefaultTheme, &kLightTheme};
    bool geometry = true;
    const float narrow_widths[]{128.f, 152.f};
    for (float narrow : narrow_widths) {
        UiPollProjection p{};
        geometry = geometry && ui_poll_controls_project(f.state, {0, 0, narrow, 1}, kDefaultUiPollStyle,
                                                        kDefaultTheme, &f.cache, 1, &p) &&
                   contained(p.geometry.bounds, p.geometry.answers[2].bounds);
    }
    for (auto width : widths)
        for (auto* theme : themes) {
            UiPollProjection p{};
            geometry = geometry &&
                       ui_poll_controls_project(f.state, {16, 16, 16 + width, 17}, kDefaultUiPollStyle,
                                                *theme, &f.cache, 1, &p) &&
                       layout_clean(p) && ui_rect_width(p.geometry.bounds) == minimum(width, 472.f) &&
                       same(p.foreground, ui_foreground_color(*theme, theme->text_primary));
        }
    check(geometry, "poll layout: both themes wrap exact Unicode at 224..480 DIP (128/152 still project) with equal-height answer rows, "
                    "the selection icon right of the label and a footer below");
    // The official card's container, answer rows and footer bar at full width.
    UiPollProjection reference{};
    const bool measured = ui_poll_controls_project(f.state, {0, 0, 800, 1}, kDefaultUiPollStyle, kDefaultTheme,
                                                   &f.cache, 1, &reference);
    const auto& g = reference.geometry;
    check(measured && ui_rect_width(g.bounds) == 472 && g.question.left == 16 && g.question.top == 16 &&
              ui_rect_height(g.answers[0].bounds) == 50 && g.answers[0].bounds.left == 16 &&
              g.answers[0].bounds.right == 456 && g.answers[1].bounds.top == g.answers[0].bounds.bottom + 8 &&
              ui_rect_width(g.answers[0].marker) == 24 && g.answers[0].marker.right == 456 - 16 &&
              g.answers[0].text.left == 32 && ui_rect_width(g.answers[1].emoji) == 24 &&
              g.answers[1].text.left == g.answers[1].emoji.right + 2 + 8 && ui_rect_height(g.vote) == 38 &&
              g.vote.right == 456 && g.bounds.bottom == g.vote.bottom + 16,
          "poll geometry: 472-DIP card, 16 padding, 50-DIP answers 8 apart, 24-DIP right-hand selection icon, "
          "24-DIP emoji with 2+8 spacing and a 38-DIP Vote button in the footer");
    // 270 DIPs of content: the 52-character failure status would wrap, and
    // "Poll closed" fits beside "Show results" + "Vote".
    UiPollProjection narrow{};
    check(ui_poll_controls_project(f.state, {0, 0, 302, 1}, kDefaultUiPollStyle, kDefaultTheme, &f.cache, 1,
                                   &narrow) &&
              ui_rect_height(narrow.geometry.instruction) == 16 && narrow.geometry.footer_rows == 1,
          "poll narrow card: a long status reserves one ellipsized line and the footer stays one row");
    check(f.unstyled_measures > 0 || f.cache.backend_calls > 0, "poll measurement: labels measured natively");
    ui_text_measure_cache_invalidate(&f.cache);
    UiPollLayout layout{};
    bool ready = ui_poll_measure_layout(f.state, 152, kDefaultUiPollStyle, &f.cache, 1, &layout);
    const auto backend = f.cache.backend_calls;
    bool stable = ready;
    for (u32 i = 0; i < 64; ++i) {
        UiPollLayout repeated{};
        stable = stable &&
                 ui_poll_measure_layout(f.state, 152, kDefaultUiPollStyle, &f.cache, 1, &repeated) &&
                 repeated.height == layout.height && repeated.content_signature == layout.content_signature;
    }
    check(stable && f.cache.backend_calls == backend, "poll retention: 64 unchanged wrapped projections "
                                                      "reuse native-measure cache without backend calls");
    UiPollProjection ballot{}, results{};
    ready = ready && ui_poll_controls_project_layout(f.state, {0, 0}, layout, kDefaultUiPollStyle,
                                                     kDefaultTheme, &ballot);
    check(ready && ui_poll_target_visible(ballot, {UiPollControl::vote}) &&
              !ui_poll_target_enabled(ballot, {UiPollControl::vote}) &&
              ui_poll_target_visible(ballot, {UiPollControl::results}) && !ballot.results_primary &&
              ballot.geometry.results.right <= ballot.geometry.vote.left,
          "poll ballot: Vote waits for a selection; the \"Show results\" text action sits left of it");
    Theme gradient = kDefaultTheme;
    gradient.background = background_theme_defaults();
    UiPollProjection themed{};
    check(ui_theme_resolve_emphasis(&gradient) &&
              ui_poll_controls_project_layout(f.state, {0, 0}, layout, kDefaultUiPollStyle, gradient, &themed) &&
              same(themed.accent, gradient.tokens.emphasis.fill) &&
              same(themed.outline, gradient.tokens.emphasis.outline) &&
              same(themed.answer_fill, gradient.tokens.fill.subtle) &&
              same(themed.selected, gradient.tokens.fill.muted) &&
              !same(themed.accent, kDefaultTheme.tokens.accent.brand) &&
              !same(themed.answer_fill, kDefaultTheme.tokens.fill.subtle) &&
              same(ballot.accent, kDefaultTheme.tokens.accent.brand) &&
              same(ballot.outline, kDefaultTheme.tokens.accent.brand),
          "poll theme: a gradient theme's emphasis and tinted fills style the vote button, selection and "
          "answer rows; flat themes keep the accent");
    f.answers[0].count = 3;
    f.answers[0].count_known = true;
    f.answers[1].count = 1;
    f.answers[1].count_known = true;
    f.answers[2].count_known = true;
    f.state.results = f.state.results_known = f.state.total_votes_known = true;
    f.state.total_votes = 4;
    f.state.revision++;
    f.state.result_revision++;
    ready = ready && ui_poll_controls_project_layout(f.state, {0, 0}, layout, kDefaultUiPollStyle,
                                                     kLightTheme, &results);
    check(ready && layout_clean(results) && results.percentages[0] == 75 && results.percentages[1] == 25 &&
              utf8_equal(ui_poll_count_label(results, 0), utf8_literal("3 votes")) &&
              utf8_equal(ui_poll_count_label(results, 1), utf8_literal("1 vote")) &&
              utf8_equal(Utf8View{results.percent_text[2], results.percent_bytes[2]}, utf8_literal("0%")) &&
              ui_rect_equal(ballot.geometry.answers[0].bounds, results.geometry.answers[0].bounds) &&
              results.results_primary && ui_poll_target_visible(results, {UiPollControl::results}) &&
              !ui_poll_target_visible(results, {UiPollControl::vote}),
          "poll results: \"N votes\" and rounded percentages without reflow; \"Go back to vote\" takes the "
          "primary slot");
    f.state.expiry_known = true;
    f.state.now_ms = 100;
    const u64 expiries[]{100 + 3 * 86'400'000ull + 5, 100 + 5 * 3'600'000ull + 5, 100 + 7 * 60'000ull,
                         100 + 30'000};
    const char* labels[]{"4 votes \xE2\x88\x99 3d left", "4 votes \xE2\x88\x99 5h left", "4 votes \xE2\x88\x99 7m left",
                         "4 votes \xE2\x88\x99 < 1m left"};
    bool details = true;
    for (u32 i = 0; i < 4; ++i) {
        f.state.expiry_ms = expiries[i];
        UiPollProjection timed{};
        details = details &&
                  ui_poll_controls_project_layout(f.state, {0, 0}, layout, kDefaultUiPollStyle, kLightTheme, &timed) &&
                  utf8_equal(ui_poll_summary_label(timed), Utf8View{labels[i], c_string_length(labels[i])});
    }
    check(details, "poll footer: \"{n} votes \xE2\x88\x99 {d}d/{h}h/{m}m/< 1m left\" like the official client");
    f.state.expiry_ms = 5000;
    f.state.loading = true;
    f.state.error = true;
    f.state.can_retry = true;
    f.state.status = utf8_literal("A bounded retry status");
    UiPollProjection pending{};
    check(ui_poll_controls_project_layout(f.state, {0, 0}, layout, kDefaultUiPollStyle, kLightTheme,
                                          &pending) &&
              ui_rect_equal(results.geometry.bounds, pending.geometry.bounds) &&
              !ui_poll_target_enabled(pending, {UiPollControl::retry}) &&
              ui_poll_target_visible(pending, {UiPollControl::retry}),
          "poll phases: loading/error/retry/expiry keep content-only layout while disabling mutation");
    auto invalid = f.state;
    const auto saved = results.geometry.bounds;
    invalid.answers = {f.answers, 11};
    bool atomic =
        !ui_poll_controls_project_layout(invalid, {}, layout, kDefaultUiPollStyle, kLightTheme, &results) &&
        ui_rect_equal(results.geometry.bounds, saved);
    invalid = f.state;
    const auto id = f.answers[1].id;
    f.answers[1].id = f.answers[0].id;
    atomic = atomic && !ui_poll_measure_layout(invalid, 152, kDefaultUiPollStyle, &f.cache, 1, &layout);
    f.answers[1].id = id;
    invalid.question = {"\xff", 1};
    atomic = atomic && !ui_poll_measure_layout(invalid, 152, kDefaultUiPollStyle, &f.cache, 1, &layout);
    auto malformed = layout;
    malformed.geometry.bounds.right++;
    atomic = atomic && !ui_poll_controls_project_layout(f.state, {}, malformed, kDefaultUiPollStyle,
                                                        kLightTheme, &results);
    check(atomic, "poll validation: oversized/duplicate-ID/invalid UTF8 and malformed retained geometry "
                  "fail atomically");
    f.state.loading = f.state.error = f.state.can_retry = f.state.results = false;
    f.state.status = {};
    f.answers[0].selected = true;
    UiPollScene scene{};
    ready = ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                                  UiPollSourceSurface::timeline, kDefaultUiPollStyle, kDefaultTheme,
                                  &f.cache, 1);
    auto point = center(scene.projection.geometry.answers[1].bounds);
    const auto down = ui_poll_scene_pointer(&scene, UiPollPointerEvent::down, point);
    const auto up = ui_poll_scene_pointer(&scene, UiPollPointerEvent::up, point);
    check(ready && down.request_capture && !down.action_requested && up.release_capture &&
              up.action_requested && up.action.answer_id == 42 &&
              up.action.kind == UiPollActionKind::toggle_answer &&
              ui_poll_action_current(f.state, up.action) &&
              ui_poll_target_enabled(scene.projection, {UiPollControl::vote}),
          "poll pointer: matching release publishes exact arbitrary answer ID; a selection enables Vote");
    auto stale = up.action;
    stale.generation++;
    ui_poll_scene_pointer(&scene, UiPollPointerEvent::down, point);
    f.state.revision++;
    ready = ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                                  UiPollSourceSurface::timeline, kDefaultUiPollStyle, kDefaultTheme,
                                  &f.cache, 1);
    const auto busy = ui_poll_scene_pointer(&scene, UiPollPointerEvent::up, point);
    check(ready && busy.action_requested && busy.action.answer_id == 42 &&
              ui_poll_action_current(f.state, busy.action),
          "poll busy input: a count-only revision between press and release keeps the press and acts on "
          "the current revision");
    ui_poll_scene_pointer(&scene, UiPollPointerEvent::down, point);
    f.state.generation++;
    ready = ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                                  UiPollSourceSurface::timeline, kDefaultUiPollStyle, kDefaultTheme,
                                  &f.cache, 1);
    stale.generation = f.state.generation - 1;
    check(ready && scene.capture.control == UiPollControl::none &&
              !ui_poll_action_current(f.state, stale) &&
              !ui_poll_scene_pointer(&scene, UiPollPointerEvent::up, point).action_requested &&
              ui_poll_scene_hit(scene, {-1, -1}).control == UiPollControl::none,
          "poll stale input: publication change retires pointer capture and rejects old receipts or "
          "clipped points");
    check(ui_poll_scene_key(&scene, UiPollKey::first).handled && scene.focus.answer_id == 7 &&
              ui_poll_scene_key(&scene, UiPollKey::previous).handled &&
              scene.focus.control == UiPollControl::vote &&
              ui_poll_scene_key(&scene, UiPollKey::next).handled && scene.focus.answer_id == 7 &&
              ui_poll_scene_key(&scene, UiPollKey::activate).action_requested,
          "poll keyboard: forward/reverse cyclic focus visits answers, Show results and Vote and activates the "
          "exact answer");
    f.answers[0].me_known = f.answers[0].me_voted = true;
    f.state.can_remove_vote = true;
    f.state.results = true;
    ready =
        ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                              UiPollSourceSurface::timeline, kDefaultUiPollStyle, kLightTheme, &f.cache, 1);
    check(ready && ui_poll_target_visible(scene.projection, {UiPollControl::remove_vote}) &&
              ui_poll_target_enabled(scene.projection, {UiPollControl::remove_vote}) &&
              !ui_poll_target_visible(scene.projection, {UiPollControl::vote}) &&
              !ui_poll_target_visible(scene.projection, {UiPollControl::results}),
          "poll own vote: a recorded vote shows Remove Vote in the primary slot and no Show results");
    f.state.now_ms = 5000;
    ready =
        ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                              UiPollSourceSurface::timeline, kDefaultUiPollStyle, kLightTheme, &f.cache, 1);
    check(ready && !ui_poll_target_visible(scene.projection, {UiPollControl::vote}) &&
              !ui_poll_target_visible(scene.projection, {UiPollControl::remove_vote}) &&
              scene.projection.victor_answers[0] && !scene.projection.victor_answers[1] &&
              utf8_equal(Utf8View{scene.projection.expiry_text, scene.projection.expiry_bytes},
                         utf8_literal("Poll closed")),
          "poll expiry: a closed poll has no action, marks the winner and reads \"Poll closed\"");
    f.state.now_ms = 100;
    ready =
        ui_poll_scene_project(&scene, f.state, {0, 0, 320, 1}, {0, 0, 320, 2000}, f.owner(),
                              UiPollSourceSurface::timeline, kDefaultUiPollStyle, kLightTheme, &f.cache, 1);
    UiPollAction voters{};
    check(ready && ui_poll_controls_action(scene.projection, {UiPollControl::answer, 42}, &voters) &&
              voters.kind == UiPollActionKind::voters,
          "poll results: answer target requests voter inspection using exact answer ID");
    PaintStream paint{};
    UiTextCommand commands[64]{};
    char bytes[8192]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text, nullptr};
    ready = ready && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 640, 2000) &&
            ui_poll_scene_paint(scene, kLightTheme, &paint) &&
            ui_text_command_stream_initialize(&text, {commands, 64}, {bytes, sizeof(bytes)}) &&
            ui_poll_scene_text_output(scene, &output);
    bool wrap{}, count{}, percent{};
    for (u32 i = 0; i < text.count; ++i) {
        wrap = wrap || (utf8_equal(commands[i].text, f.state.question) &&
                        commands[i].wrapping == TextWrapping::wrap);
        count = count || utf8_equal(commands[i].text, ui_poll_count_label(scene.projection, 0));
        percent = percent || utf8_equal(commands[i].text, utf8_literal("75%"));
    }
    const auto signature = ui_poll_scene_text_revision(scene);
    scene.projection.count_text[0][0] = '9';
    bool copied{};
    for (u32 i = 0; i < text.count; ++i)
        copied = copied || utf8_equal(commands[i].text, utf8_literal("3 votes"));
    check(ready && wrap && count && percent && copied && signature && paint.count > 10 && paint.count < 128 &&
              paint.clip_depth == 1 && f.manager.count == 1,
          "poll presentation: selection marks, bars and own-vote checks are bounded; labels, counts and "
          "percentages are copied under one root");
    paint_stream_release(&paint);
}
void voter_tests(Fixture& f, void (*check)(bool, const char*) noexcept) noexcept {
    UiPollVoter users[100]{};
    for (u32 i = 0; i < 100; ++i) {
        users[i].id = 1000 + i;
        users[i].name = utf8_literal("Original Unicode \342\230\203 participant");
        if (i % 2 == 0) {
            users[i].avatar_hash = utf8_literal("0123456789abcdef0123456789abcdef");
            users[i].avatar_variant = user_avatar_variant(users[i].avatar_hash);
        }
    }
    UiPollVotersState state{};
    state.owner = 31;
    state.generation = 32;
    state.revision = state.content_revision = 1;
    state.question = f.state.question;
    state.answer_id = 42;
    state.answers = {f.answers, 3};
    state.voters = {users, 100};
    state.has_next = true;
    UiPollVotersScene scene{};
    bool ready = ui_poll_voters_scene_project(&scene, state, {0, 0, 320, 640}, f.owner(),
                                              kDefaultUiPollVotersStyle, kLightTheme, &f.cache, 1);
    check(ready && contained(scene.geometry.viewport, scene.geometry.panel) &&
              contained(scene.geometry.panel, scene.geometry.body) && scene.visible > 0 &&
              scene.visible <= 25 &&
              ui_poll_voters_scene_hit(scene, center(scene.geometry.answers[0])).id == 7 &&
              !ui_poll_voters_target_enabled(scene, {UiPollVotersControl::previous}) &&
              ui_poll_voters_target_enabled(scene, {UiPollVotersControl::next}),
          "poll voters: narrow Light panel keeps bounded visible page and arbitrary answer tabs above "
          "independent pagination");
    PollVotersAvatarMedia voter_avatar{}, blank_avatar{};
    check(poll_voters_avatar_media_resolve(scene, 0, &voter_avatar) &&
              voter_avatar.key.kind == MediaAssetKind::user_avatar && voter_avatar.key.id == 1000 &&
              voter_avatar.key.variant == users[0].avatar_variant &&
              contained(ui_poll_voters_row_rectangle(scene, 0), voter_avatar.rectangle) &&
              poll_voters_avatar_media_current(scene, voter_avatar.key) &&
              !poll_voters_avatar_media_resolve(scene, 1, &blank_avatar) &&
              !poll_voters_avatar_media_resolve(scene, 99, &blank_avatar),
          "poll voters: visible rows resolve voter avatars from page hashes; culled rows admit none");
    const auto point = center(ui_poll_voters_row_rectangle(scene, 0));
    const auto down = ui_poll_voters_scene_pointer(&scene, UiPollPointerEvent::down, point);
    const auto up = ui_poll_voters_scene_pointer(&scene, UiPollPointerEvent::up, point);
    check(down.request_capture && up.action_requested && up.action.id == 1000,
          "poll voters pointer: exact visible user action is release-bound and publication fenced");
    check(ui_poll_voters_scene_focus(&scene, {UiPollVotersControl::user, 1099}, true) && scene.scroll > 0 &&
              contained(scene.geometry.body, ui_poll_voters_row_rectangle(scene, 99)) &&
              ui_poll_voters_scene_key(&scene, UiPollKey::activate).action_requested,
          "poll voters keyboard: last page user scrolls fully into the clipped native body before "
          "activation");
    ready = ui_poll_voters_scene_scroll(&scene, -100000);
    check(ready && scene.scroll == 0 && scene.first == 0 && scene.visible <= 25 &&
              ui_poll_voters_scene_hit(scene, {scene.geometry.body.left, scene.geometry.body.bottom + 1})
                      .control != UiPollVotersControl::user,
          "poll voters scroll: exact clamping preserves body ownership and excludes footer from user hit "
          "tests");
    state.loading = true;
    state.has_previous = true;
    ready = ui_poll_voters_scene_project(&scene, state, {0, 0, 640, 800}, f.owner(),
                                         kDefaultUiPollVotersStyle, kDefaultTheme, &f.cache, 1);
    check(ready && ui_poll_voters_target_enabled(scene, {UiPollVotersControl::close}) &&
              !ui_poll_voters_target_enabled(scene, {UiPollVotersControl::next}) &&
              !ui_poll_voters_target_enabled(scene, {UiPollVotersControl::user, 1000}),
          "poll voters loading: Close remains accessible while page/user actions are locked");
    check(ready && scene.geometry.columns && scene.geometry.answers[1].top > scene.geometry.answers[0].bottom &&
              scene.geometry.answers[0].left == scene.geometry.panel.left + scene.style.padding &&
              scene.geometry.body.left > scene.geometry.answers[0].right &&
              contained(scene.geometry.panel, scene.geometry.answers[2]),
          "poll voters wide: answers list in a left column beside the selected answer's voters");
    {
        UiTextCommand listed[64]{};
        char listed_bytes[16384]{};
        UiTextCommandStream list_text{};
        UiTextOutput list_output{&list_text, nullptr};
        bool labelled = ui_text_command_stream_initialize(&list_text, {listed, 64}, {listed_bytes, sizeof(listed_bytes)}) &&
                        ui_poll_voters_scene_text_output(scene, &list_output);
        bool answer_label{};
        for (u32 i = 0; i < list_text.count; ++i)
            answer_label = answer_label || (utf8_equal(listed[i].text, f.answers[0].text) &&
                                            contained(scene.geometry.answers[0], listed[i].rectangle));
        check(labelled && answer_label, "poll voters wide: each answer row shows its text, not an index");
    }
    state.loading = false;
    state.error = state.can_retry = true;
    state.revision++;
    ready = ui_poll_voters_scene_project(&scene, state, {0, 0, 320, 640}, f.owner(),
                                         kDefaultUiPollVotersStyle, kLightTheme, &f.cache, 1);
    check(ready && ui_poll_voters_target_enabled(scene, {UiPollVotersControl::retry}) &&
              same(scene.error, ui_foreground_color(kLightTheme, kLightTheme.notification)),
          "poll voters error: explicit Retry and semantic Light error ink retain exact stale-response "
          "fences");
    UiTextCommand commands[64]{};
    char bytes[16384]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text, nullptr};
    ready = ui_text_command_stream_initialize(&text, {commands, 64}, {bytes, sizeof(bytes)}) &&
            ui_poll_voters_scene_text_output(scene, &output);
    u32 user_labels{};
    bool copied_page{};
    for (u32 i = 0; i < text.count; ++i) {
        if (utf8_equal(commands[i].text, users[0].name)) {
            user_labels++;
            ready = ready && ui_rect_equal(commands[i].clip, scene.geometry.body);
        }
        copied_page = copied_page || utf8_equal(commands[i].text, utf8_literal("Page 1"));
    }
    scene.page_text[0] = 'X';
    check(ready && user_labels <= 25 && user_labels > 0 && copied_page && f.manager.count == 1,
          "poll voters text: visible names use exact body clips and generated page label is owned by "
          "output storage");
    const auto revision = ui_poll_voters_scene_text_revision(scene);
    ui_widget_manager_reset(&f.manager);
    check(revision && !ui_poll_voters_scene_key(&scene, UiPollKey::activate).action_requested &&
              !ui_poll_voters_scene_paint_revision(scene) && !ui_poll_voters_scene_text_revision(scene),
          "poll voters stale root: retired native widget generation prevents input and retained paint/text "
          "admission");
}
} // namespace
void test_poll_controls(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture fixture{};
    const bool ready = fixture.initialize();
    check(ready, "poll components: bounded one-root fixture and wrapping backend initialize");
    if (!ready)
        return;
    card_tests(fixture, check);
    voter_tests(fixture, check);
}
} // namespace maicord

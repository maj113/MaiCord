#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool current(const UiPollVotersScene& scene) noexcept {
    const auto* widget =
        scene.owner.manager ? ui_widget_manager_resolve(*scene.owner.manager, scene.owner.handle) : nullptr;
    return scene.active && widget && (widget->flags & ui_widget_enabled) &&
           !(widget->flags & ui_widget_block_input);
}
bool valid_text(Utf8View text, u32 maximum) noexcept {
    return text.bytes <= maximum && utf8_validate(text).valid;
}
bool valid(const UiPollVotersState& state) noexcept {
    if (!state.owner || !state.generation || !state.revision || !state.content_revision ||
        !state.answer_id || !valid_text(state.question, kUiPollQuestionMaximumBytes) ||
        !valid_text(state.status, 256) || !state.answers.data || !state.answers.count ||
        state.answers.count > kUiPollMaximumAnswers || (!state.voters.data && state.voters.count) ||
        state.voters.count > kUiPollVoterPageMaximum)
        return false;
    bool selected{};
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        if (!answer.id || !valid_text(answer.text, kUiPollAnswerMaximumBytes) ||
            !valid_text(answer.emoji_name, kUiPollEmojiMaximumBytes))
            return false;
        selected = selected || answer.id == state.answer_id;
        for (u32 j = 0; j < i; ++j)
            if (state.answers[j].id == answer.id)
                return false;
    }
    for (u32 i = 0; i < state.voters.count; ++i) {
        if (!state.voters[i].id || !valid_text(state.voters[i].name, 512))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (state.voters[j].id == state.voters[i].id)
                return false;
    }
    return selected;
}
bool style_valid(const UiPollVotersStyle& style) noexcept {
    const float lengths[]{
        style.preferred_width, style.preferred_height, style.margin,     style.padding,       style.gap,
        style.header_height,   style.target,           style.row_height, style.avatar_extent, style.radius};
    for (float value : lengths)
        if (!ui_coordinate_valid(value) || value < 0)
            return false;
    if (style.preferred_width < 160 || style.preferred_height < 200 || style.header_height < 40 ||
        style.target < 32 || style.row_height < style.target || style.avatar_extent < 16 ||
        style.avatar_extent > style.row_height || !ui_coordinate_valid(style.backdrop_opacity) ||
        style.backdrop_opacity < 0 || style.backdrop_opacity > 1)
        return false;
    const Utf8View labels[]{style.title, style.previous, style.next,   style.retry,
                            style.close, style.loading,  style.failed, style.empty};
    for (auto text : labels)
        if (!text.bytes || !valid_text(text, 256))
            return false;
    return true;
}
bool contains(UiRect bounds, UiPoint point) noexcept {
    return ui_rect_has_area(bounds) && point.x >= bounds.left && point.x < bounds.right &&
           point.y >= bounds.top && point.y < bounds.bottom;
}
bool measured(const UiPollVotersState& state, Utf8View text, u16 item, u32 flags, float width,
              UiTextMeasureCache* cache, u16 backend, UiTextMeasureValue* value,
              bool no_wrap = false) noexcept {
    const TextStyleSpan style{0, text.bytes, flags};
    const u64 signature = hash64_mix(hash64_counted_bytes(kHash64Seed, text.data, text.bytes), flags);
    return width > 0 &&
           ui_text_measure_cache_measure(cache,
                                         {{state.owner, signature, state.content_revision, item, backend},
                                          text,
                                          flags == text_style_none ? ConstSlice<TextStyleSpan>{}
                                                                   : ConstSlice<TextStyleSpan>{&style, 1},
                                          {width, kMaximumUiCoordinate},
                                          no_wrap},
                                         value)
               .ok() &&
           ui_coordinate_valid(value->size.width) && ui_coordinate_valid(value->size.height) &&
           value->size.height > 0;
}
void publish_scroll(UiPollVotersScene& scene, float requested) noexcept {
    const float total = static_cast<float>(scene.state.voters.count) * scene.style.row_height;
    const float maximum_scroll = maximum(0.f, total - ui_rect_height(scene.geometry.body));
    scene.scroll = minimum(maximum(requested, 0.f), maximum_scroll);
    scene.first =
        minimum(scene.state.voters.count, static_cast<u32>(scene.scroll / scene.style.row_height));
    const float remainder = scene.scroll - static_cast<float>(scene.first) * scene.style.row_height;
    const auto rows =
        static_cast<u32>((ui_rect_height(scene.geometry.body) + remainder) / scene.style.row_height) + 1;
    scene.visible =
        minimum(kUiPollVisibleVotersMaximum, minimum(rows, scene.state.voters.count - scene.first));
}
UiPollVotersAction action(const UiPollVotersScene& scene, UiPollVotersTarget target) noexcept {
    return {scene.state.owner, scene.state.generation, scene.state.revision, target.control, target.id};
}
u32 targets(const UiPollVotersScene& scene, UiPollVotersTarget* output) noexcept {
    u32 count{};
    output[count++] = {UiPollVotersControl::close};
    for (u32 i = 0; i < scene.state.answers.count; ++i)
        output[count++] = {UiPollVotersControl::answer, scene.state.answers[i].id};
    for (u32 i = 0; i < scene.state.voters.count; ++i)
        output[count++] = {UiPollVotersControl::user, scene.state.voters[i].id};
    output[count++] = {UiPollVotersControl::previous};
    output[count++] = {UiPollVotersControl::next};
    output[count++] = {UiPollVotersControl::retry};
    return count;
}
} // namespace

bool ui_poll_voters_scene_project(UiPollVotersScene* scene, const UiPollVotersState& state, UiRect viewport,
                                  UiWidgetReference owner, const UiPollVotersStyle& style,
                                  const Theme& theme, UiTextMeasureCache* cache, u16 backend,
                                  float requested_scroll) noexcept {
    if (!scene || !valid(state) || !style_valid(style) || !cache || !ui_rect_has_area(viewport) ||
        !ui_coordinate_valid(requested_scroll))
        return false;
    const auto* widget = owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    if (!widget || !(widget->flags & ui_widget_enabled) || (widget->flags & ui_widget_block_input))
        return false;
    UiPollVotersScene projected{};
    projected.state = state;
    projected.style = style;
    projected.owner = owner;
    projected.active = true;
    projected.surface = theme.surface;
    projected.foreground = ui_foreground_color(theme, theme.text_primary);
    projected.secondary = ui_foreground_color(theme, theme.text_secondary);
    projected.accent = theme.tokens.emphasis.fill;
    projected.accent_foreground = ui_surface_foreground(theme, theme.tokens.emphasis.fill);
    projected.error = ui_foreground_color(theme, theme.notification);
    auto& geometry = projected.geometry;
    geometry.viewport = viewport;
    const float width = minimum(style.preferred_width, ui_rect_width(viewport) - 2 * style.margin);
    const float height = minimum(style.preferred_height, ui_rect_height(viewport) - 2 * style.margin);
    if (width < 160 || height < 200)
        return false;
    const float left = (viewport.left + viewport.right - width) * .5f;
    const float top = (viewport.top + viewport.bottom - height) * .5f;
    geometry.panel = {left, top, left + width, top + height};
    geometry.close = {geometry.panel.right - style.padding - style.target,
                      top + (style.header_height - style.target) * .5f,
                      geometry.panel.right - style.padding,
                      top + (style.header_height + style.target) * .5f};
    geometry.title = {left + style.padding, top, geometry.close.left - style.gap,
                      top + style.header_height};
    float y = geometry.title.bottom + style.gap;
    const float inner_left = left + style.padding, inner_right = geometry.panel.right - style.padding;
    UiTextMeasureValue value{};
    if (state.question.bytes) {
        if (!measured(state, state.question, 100, text_style_subtext, inner_right - inner_left, cache,
                      backend, &value))
            return false;
        geometry.question = {inner_left, y, inner_right, y + minimum(32.f, value.size.height)};
        y = geometry.question.bottom + style.gap;
    }
    for (u32 i = 0; i < state.answers.count; ++i)
        if (state.answers[i].id == state.answer_id) {
            const auto& answer = state.answers[i];
            const Utf8View label = answer.text.bytes         ? answer.text
                                   : answer.emoji_name.bytes ? answer.emoji_name
                                                             : utf8_literal("Answer");
            if (!measured(state, label, 101, text_style_bold, inner_right - inner_left, cache, backend,
                          &value))
                return false;
            geometry.answer = {inner_left, y, inner_right, y + maximum(20.f, value.size.height)};
            y = geometry.answer.bottom + style.gap;
            break;
        }
    float x = inner_left;
    for (u32 i = 0; i < state.answers.count; ++i) {
        if (x > inner_left && x + style.target > inner_right) {
            x = inner_left;
            y += style.target + style.gap;
        }
        geometry.answers[i] = {x, y, x + style.target, y + style.target};
        x += style.target + style.gap;
    }
    y += style.target + style.gap;
    const auto status = state.status.bytes    ? state.status
                        : state.loading       ? style.loading
                        : state.error         ? style.failed
                        : !state.voters.count ? style.empty
                                              : Utf8View{};
    if (status.bytes) {
        if (!measured(state, status, 102, text_style_subtext, inner_right - inner_left, cache, backend,
                      &value))
            return false;
        geometry.status = {inner_left, y, inner_right, y + maximum(16.f, value.size.height)};
        y = geometry.status.bottom + style.gap;
    }
    float footer_y = geometry.panel.bottom - style.padding - style.target;
    const UiPollVotersControl controls[]{UiPollVotersControl::previous, UiPollVotersControl::next,
                                         UiPollVotersControl::retry};
    const Utf8View button_labels[]{style.previous, style.next, style.retry};
    const bool visible[]{state.has_previous, state.has_next, state.can_retry};
    float widths[3]{};
    float total{};
    for (u32 i = 0; i < 3; ++i)
        if (visible[i]) {
            if (!measured(state, button_labels[i], static_cast<u16>(110 + static_cast<u8>(controls[i])),
                          text_style_none, kMaximumUiCoordinate, cache, backend, &value, true))
                return false;
            widths[i] = minimum(inner_right - inner_left, maximum(style.target, value.size.width + 16));
            total += widths[i] + style.gap;
        }
    const bool second_row = total > inner_right - inner_left + style.gap;
    if (second_row)
        footer_y -= style.target + style.gap;
    x = inner_left;
    float button_y = footer_y;
    UiRect* buttons[]{&geometry.previous, &geometry.next, &geometry.retry};
    for (u32 i = 0; i < 3; ++i)
        if (visible[i]) {
            if (x > inner_left && x + widths[i] > inner_right) {
                x = inner_left;
                button_y += style.target + style.gap;
            }
            *buttons[i] = {x, button_y, x + widths[i], button_y + style.target};
            x += widths[i] + style.gap;
        }
    geometry.page = {inner_left, footer_y - 20 - style.gap, inner_right, footer_y - style.gap};
    geometry.body = {inner_left, y, inner_right, geometry.page.top - style.gap};
    if (ui_rect_height(geometry.body) < style.row_height)
        return false;
    char digits[20];
    const u32 digit_count = decimal_write(digits, static_cast<u64>(state.page_index) + 1);
    constexpr char prefix[] = "Page ";
    for (u32 i = 0; i < sizeof(prefix) - 1; ++i)
        projected.page_text[projected.page_bytes++] = prefix[i];
    for (u32 i = 0; i < digit_count; ++i)
        projected.page_text[projected.page_bytes++] = digits[i];
    publish_scroll(projected, requested_scroll);
    const bool same_owner = current(*scene) && scene->owner.manager == owner.manager &&
                            scene->owner.handle.slot == owner.handle.slot &&
                            scene->owner.handle.generation == owner.handle.generation &&
                            scene->state.owner == state.owner &&
                            scene->state.generation == state.generation;
    if (same_owner && ui_poll_voters_target_enabled(projected, scene->focus)) {
        projected.focus = scene->focus;
        projected.keyboard_focus = scene->keyboard_focus;
    }
    if (same_owner && scene->state.revision == state.revision && scene->scroll == projected.scroll &&
        ui_rect_equal(ui_poll_voters_target_rectangle(*scene, scene->capture),
                      ui_poll_voters_target_rectangle(projected, scene->capture)) &&
        ui_poll_voters_target_enabled(projected, scene->capture))
        projected.capture = scene->capture;
    *scene = projected;
    return true;
}
bool ui_poll_voters_scene_scroll(UiPollVotersScene* scene, float delta) noexcept {
    if (!scene || !current(*scene) || !ui_coordinate_valid(delta))
        return false;
    const float old = scene->scroll;
    publish_scroll(*scene, old + delta);
    if (old == scene->scroll)
        return false;
    scene->hover = {};
    scene->capture = {};
    return true;
}
UiRect ui_poll_voters_row_rectangle(const UiPollVotersScene& scene, u32 index) noexcept {
    if (index >= scene.state.voters.count)
        return {};
    const float top =
        scene.geometry.body.top + static_cast<float>(index) * scene.style.row_height - scene.scroll;
    return {scene.geometry.body.left, top, scene.geometry.body.right, top + scene.style.row_height};
}
bool ui_poll_voters_avatar(const UiPollVotersScene& scene, u32 index, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !scene.active || index < scene.first || index >= scene.first + scene.visible ||
        index >= scene.state.voters.count)
        return false;
    const auto row = ui_poll_voters_row_rectangle(scene, index);
    const float top = row.top + (scene.style.row_height - scene.style.avatar_extent) * .5f;
    *rectangle = {row.left, top, row.left + scene.style.avatar_extent, top + scene.style.avatar_extent};
    *clip = scene.geometry.body;
    return ui_rect_has_area(ui_rect_intersection(*rectangle, *clip));
}
UiRect ui_poll_voters_target_rectangle(const UiPollVotersScene& scene, UiPollVotersTarget target) noexcept {
    switch (target.control) {
    case UiPollVotersControl::close:
        return scene.geometry.close;
    case UiPollVotersControl::previous:
        return scene.geometry.previous;
    case UiPollVotersControl::next:
        return scene.geometry.next;
    case UiPollVotersControl::retry:
        return scene.geometry.retry;
    case UiPollVotersControl::answer:
        for (u32 i = 0; i < scene.state.answers.count; ++i)
            if (scene.state.answers[i].id == target.id)
                return scene.geometry.answers[i];
        break;
    case UiPollVotersControl::user:
        for (u32 i = 0; i < scene.state.voters.count; ++i)
            if (scene.state.voters[i].id == target.id)
                return ui_poll_voters_row_rectangle(scene, i);
        break;
    default:
        break;
    }
    return {};
}
bool ui_poll_voters_target_enabled(const UiPollVotersScene& scene, UiPollVotersTarget target) noexcept {
    if (target.control != UiPollVotersControl::answer && target.control != UiPollVotersControl::user &&
        target.id)
        return false;
    if (!current(scene) || !ui_rect_has_area(ui_poll_voters_target_rectangle(scene, target)))
        return false;
    switch (target.control) {
    case UiPollVotersControl::close:
        return true;
    case UiPollVotersControl::answer:
        return !scene.state.loading && target.id != scene.state.answer_id;
    case UiPollVotersControl::user:
        return !scene.state.loading && !scene.state.error;
    case UiPollVotersControl::previous:
        return !scene.state.loading && scene.state.has_previous;
    case UiPollVotersControl::next:
        return !scene.state.loading && scene.state.has_next;
    case UiPollVotersControl::retry:
        return !scene.state.loading && scene.state.can_retry;
    default:
        return false;
    }
}
UiPollVotersTarget ui_poll_voters_scene_hit(const UiPollVotersScene& scene, UiPoint point) noexcept {
    if (!current(scene) || !contains(scene.geometry.panel, point))
        return {};
    UiPollVotersTarget all[kUiPollMaximumAnswers + kUiPollVoterPageMaximum + 4]{};
    const auto count = targets(scene, all);
    for (u32 i = 0; i < count; ++i) {
        const auto target = all[i];
        auto bounds = ui_poll_voters_target_rectangle(scene, target);
        if (target.control == UiPollVotersControl::user)
            bounds = ui_rect_intersection(bounds, scene.geometry.body);
        if (ui_poll_voters_target_enabled(scene, target) && contains(bounds, point))
            return target;
    }
    return {};
}
bool ui_poll_voters_scene_focus(UiPollVotersScene* scene, UiPollVotersTarget target,
                                bool keyboard) noexcept {
    if (!scene || !ui_poll_voters_target_enabled(*scene, target))
        return false;
    if (target.control == UiPollVotersControl::user) {
        const auto row = ui_poll_voters_target_rectangle(*scene, target);
        if (row.top < scene->geometry.body.top)
            publish_scroll(*scene, scene->scroll + row.top - scene->geometry.body.top);
        else if (row.bottom > scene->geometry.body.bottom)
            publish_scroll(*scene, scene->scroll + row.bottom - scene->geometry.body.bottom);
    }
    scene->focus = target;
    scene->keyboard_focus = keyboard;
    return true;
}
UiPollVotersSceneResult ui_poll_voters_scene_pointer(UiPollVotersScene* scene, UiPollPointerEvent event,
                                                     UiPoint point) noexcept {
    UiPollVotersSceneResult result{};
    if (!scene)
        return result;
    if (!current(*scene) || event == UiPollPointerEvent::cancel || event == UiPollPointerEvent::leave) {
        result.release_capture = scene->capture.control != UiPollVotersControl::none;
        scene->capture = {};
        scene->hover = {};
        return result;
    }
    const auto hit = ui_poll_voters_scene_hit(*scene, point);
    scene->hover = hit;
    if (event == UiPollPointerEvent::down && hit.control != UiPollVotersControl::none) {
        scene->focus = scene->capture = hit;
        scene->keyboard_focus = false;
        result.handled = result.request_capture = true;
    } else if (event == UiPollPointerEvent::up && scene->capture.control != UiPollVotersControl::none) {
        result.handled = result.release_capture = true;
        result.action_requested = ui_poll_voters_target_equal(hit, scene->capture);
        if (result.action_requested)
            result.action = action(*scene, hit);
        scene->capture = {};
    } else
        result.handled =
            hit.control != UiPollVotersControl::none || scene->capture.control != UiPollVotersControl::none;
    return result;
}
UiPollVotersSceneResult ui_poll_voters_scene_key(UiPollVotersScene* scene, UiPollKey key) noexcept {
    UiPollVotersSceneResult result{};
    if (!scene || !current(*scene))
        return result;
    if (key == UiPollKey::activate) {
        result.handled = result.action_requested = ui_poll_voters_target_enabled(*scene, scene->focus);
        if (result.action_requested)
            result.action = action(*scene, scene->focus);
        return result;
    }
    UiPollVotersTarget all[kUiPollMaximumAnswers + kUiPollVoterPageMaximum + 4]{},
        enabled[kUiPollMaximumAnswers + kUiPollVoterPageMaximum + 4]{};
    const auto count = targets(*scene, all);
    u32 available{}, focused = ~0u;
    for (u32 i = 0; i < count; ++i)
        if (ui_poll_voters_target_enabled(*scene, all[i])) {
            if (ui_poll_voters_target_equal(all[i], scene->focus))
                focused = available;
            enabled[available++] = all[i];
        }
    if (!available)
        return result;
    u32 next = key == UiPollKey::last || key == UiPollKey::previous ? available - 1 : 0;
    if (focused < available && (key == UiPollKey::next || key == UiPollKey::previous))
        next = key == UiPollKey::next ? (focused + 1) % available : (focused + available - 1) % available;
    result.handled = ui_poll_voters_scene_focus(scene, enabled[next], true);
    return result;
}
} // namespace maicord

#include "maicord/app/input_layers.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {

struct RoutingFixture {
    MessageComposer composer;
    u32 calls[5];
    UiInputDisposition responses[5];
    UiInputLayer* close_layer;
    bool repeated;
};

UiInputDisposition login(void* opaque, const UiInputEvent&) noexcept {
    auto& state = *static_cast<RoutingFixture*>(opaque);
    ++state.calls[0];
    return state.responses[0];
}
UiInputDisposition viewer(void* opaque, const UiInputEvent&) noexcept {
    auto& state = *static_cast<RoutingFixture*>(opaque);
    ++state.calls[1];
    return state.responses[1];
}
UiInputDisposition popup(void* opaque, const UiInputEvent&) noexcept {
    auto& state = *static_cast<RoutingFixture*>(opaque);
    ++state.calls[2];
    if (state.close_layer) state.close_layer->barrier = 0;
    return state.responses[2];
}
UiInputDisposition content(void* opaque, const UiInputEvent& event) noexcept {
    auto& state = *static_cast<RoutingFixture*>(opaque);
    ++state.calls[3];
    state.repeated = event.repeated;
    if (event.kind == UiInputKind::character)
        message_composer_insert_utf16(&state.composer, static_cast<u16>(event.code));
    return state.responses[3];
}
UiInputDisposition profile(void* opaque, const UiInputEvent&) noexcept {
    auto& state = *static_cast<RoutingFixture*>(opaque);
    ++state.calls[4];
    return state.responses[4];
}

} // namespace

void test_input_router(void (*check)(bool, const char*) noexcept) noexcept {
    u32 held = VK_RETURN;
    check(ui_input_consume_key_tail(&held, {UiInputKind::character, '\r'}) &&
        ui_input_consume_key_tail(&held, {UiInputKind::character, '\n'}) &&
        ui_input_consume_key_tail(&held, {UiInputKind::key_down, VK_RETURN, true}) &&
        !ui_input_consume_key_tail(&held, {UiInputKind::character, 'a'}) &&
        ui_input_consume_key_tail(&held, {UiInputKind::key_up, VK_RETURN}) && !held,
        "overlay activation tail cannot type or submit into newly exposed content");
    held = VK_SPACE;
    check(!ui_input_consume_key_tail(&held, {UiInputKind::key_down, VK_SPACE, false}) && !held,
        "fresh key press recovers an overlay guard after a lost release");
    RoutingFixture fixture{};
    char text[128]{};
    check(message_composer_initialize(&fixture.composer, {text, 128}), "input routing composer fixture");
    const AppInputHandlers handlers{&fixture, login, viewer, popup, content, profile};
    auto frame = app_input_layers({AppPopupInput::modal, false, false}, handlers);
    auto route = [&](UiInputKind kind, u32 code = 0) noexcept {
        return ui_input_route({frame.layers, frame.count}, {kind, code});
    };
    const auto blocked_text = route(UiInputKind::character, 'x');
    check(blocked_text.disposition == UiInputDisposition::blocked &&
          blocked_text.layer_id == static_cast<u32>(AppInputLayerId::popup) &&
          fixture.composer.bytes == 0 && fixture.calls[3] == 0,
          "context popup blocks character event from composer");
    check(route(UiInputKind::key_down, 65).consumed() && route(UiInputKind::primary_double_click).consumed() &&
          route(UiInputKind::key_up, 32).consumed() &&
          route(UiInputKind::system_character, 65).consumed() && fixture.calls[3] == 0,
          "popup barriers cover keys system text and double click");
    frame = app_input_layers({AppPopupInput::autocomplete, false, false}, handlers);
    check(route(UiInputKind::character, 'a').disposition == UiInputDisposition::ignored &&
          fixture.composer.bytes == 1 && fixture.composer.text[0] == 'a',
          "autocomplete passes unhandled typing to composer");
    check(route(UiInputKind::primary_double_click).disposition == UiInputDisposition::ignored,
          "autocomplete permits ordinary editor double click");
    check(ui_input_route({frame.layers, frame.count}, {UiInputKind::key_down, 32, true}).disposition == UiInputDisposition::ignored &&
          fixture.repeated, "layer routing preserves key-repeat metadata");
    const u32 content_calls = fixture.calls[3];
    fixture.responses[2] = UiInputDisposition::handled;
    check(route(UiInputKind::key_down, 40).disposition == UiInputDisposition::handled &&
          fixture.calls[3] == content_calls, "handled autocomplete navigation stops at popup");
    fixture.responses[2] = UiInputDisposition::failed;
    check(route(UiInputKind::character, 'b').disposition == UiInputDisposition::failed &&
          fixture.composer.bytes == 1 && fixture.calls[3] == content_calls,
          "failed handler never leaks event to lower layers");
    fixture.responses[2] = UiInputDisposition::ignored;
    frame = app_input_layers({AppPopupInput::autocomplete, false, false, true}, handlers);
    check(route(UiInputKind::character, 'p').layer_id == static_cast<u32>(AppInputLayerId::profile) &&
          route(UiInputKind::key_down, VK_DELETE).consumed() &&
          route(UiInputKind::primary_double_click).consumed() && fixture.calls[3] == content_calls &&
          fixture.calls[4] == 3 && fixture.composer.bytes == 1,
          "profile owns unhandled autocomplete input instead of underlying composer");
    frame = app_input_layers({AppPopupInput::modal, false, false, true}, handlers);
    check(route(UiInputKind::character, 'p').layer_id == static_cast<u32>(AppInputLayerId::popup) &&
          fixture.calls[4] == 3 && fixture.calls[3] == content_calls,
          "nested profile menu owns input before profile and messages");
    frame = app_input_layers({AppPopupInput::modal, true, false}, handlers);
    const u32 popup_calls = fixture.calls[2];
    check(route(UiInputKind::character, 'c').layer_id == static_cast<u32>(AppInputLayerId::reaction_viewer) &&
          fixture.calls[2] == popup_calls && fixture.calls[3] == content_calls,
          "top modal blocks lower popup and composer");
    frame = app_input_layers({AppPopupInput::modal, true, true}, handlers);
    const u32 viewer_calls = fixture.calls[1];
    check(route(UiInputKind::character, 'd').layer_id == static_cast<u32>(AppInputLayerId::login) &&
          fixture.calls[1] == viewer_calls && fixture.calls[2] == popup_calls &&
          fixture.calls[3] == content_calls, "login owns text even if old account overlays remain active");
    check(route(UiInputKind::key_up, 32).layer_id == static_cast<u32>(AppInputLayerId::login) &&
          fixture.calls[3] == content_calls, "login blocks late release from underlying account controls");
    frame = app_input_layers({AppPopupInput::modal, false, false}, handlers);
    fixture.close_layer = &frame.layers[0];
    check(route(UiInputKind::character, 'e').disposition == UiInputDisposition::blocked &&
          fixture.calls[3] == content_calls, "closing layer cannot remove current event barrier");
    fixture.close_layer = nullptr;
    frame = app_input_layers({AppPopupInput::none, false, false}, handlers);
    check(route(UiInputKind::character, 'f').disposition == UiInputDisposition::ignored &&
          fixture.composer.bytes == 2 && fixture.composer.text[1] == 'f',
          "later event reaches content after overlay closes");
    const u32 before_invalid = fixture.calls[3];
    frame.layers[0].barrier = 0x80000000u;
    check(route(UiInputKind::character, 'g').disposition == UiInputDisposition::failed &&
          fixture.calls[3] == before_invalid, "invalid barrier rejected before callbacks");
    frame.layers[0].barrier = 0;
    check(route(UiInputKind::count).disposition == UiInputDisposition::failed &&
          fixture.calls[3] == before_invalid, "invalid event rejected before callbacks");
    check(ui_input_route({nullptr, 1}, {UiInputKind::character, 0}).disposition == UiInputDisposition::failed &&
          ui_input_route({}, {UiInputKind::character, 0}).disposition == UiInputDisposition::ignored &&
          ui_input_route({frame.layers, kUiInputLayerLimit + 1}, {UiInputKind::character, 0}).disposition ==
              UiInputDisposition::failed, "router validates storage and bounded layer count");
    message_composer_release(&fixture.composer);
}

} // namespace maicord

#include "maicord/app/call_video_presenter.hpp"
#include "maicord/app/call_view_lifecycle.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool close_to(float a, float b, float tolerance = .05f) noexcept {
    return a - b <= tolerance && b - a <= tolerance;
}
bool aspect(UiRect r) noexcept {
    return close_to(ui_rect_width(r) / ui_rect_height(r), kUiCallTileAspect, .01f);
}
bool overlaps(UiRect a, UiRect b) noexcept {
    return a.left < b.right - .01f && b.left < a.right - .01f && a.top < b.bottom - .01f && b.top < a.bottom - .01f;
}

void grid_cases(Check check) noexcept {
    const auto one = ui_call_grid_measure(1600, 900, 1, 8);
    check(one.columns == 1 && one.rows == 1 && close_to(one.tile_width, 1600) && close_to(one.tile_height, 900),
          "call grid: one participant fills the 16:9 stage");
    const auto two = ui_call_grid_measure(1600, 900, 2, 8);
    check(two.columns == 2 && two.rows == 1 && close_to(two.tile_width, 796), "call grid: two tiles sit side by side");
    const auto tall = ui_call_grid_measure(500, 1000, 2, 8);
    check(tall.columns == 1 && tall.rows == 2, "call grid: a tall stage stacks tiles");
    UiRect tiles[40]{};
    UiCallGridLayout layout{};
    const UiRect area{100, 50, 1100, 550};
    check(ui_call_grid_place(area, 5, 8, 0, {tiles, 5}, &layout) && layout.columns == 3 && layout.rows == 2,
          "call grid: five tiles use three columns");
    bool valid = true;
    for (u32 i = 0; i < 5; ++i) {
        valid = valid && aspect(tiles[i]) && tiles[i].left >= area.left - .01f && tiles[i].right <= area.right + .01f &&
                tiles[i].top >= area.top - .01f && tiles[i].bottom <= area.bottom + .01f;
        for (u32 j = 0; j < i; ++j)
            valid = valid && !overlaps(tiles[i], tiles[j]);
    }
    check(valid, "call grid: tiles are 16:9, inside the stage and never overlap");
    const float middle = (area.left + area.right) * .5f;
    check(close_to((tiles[3].left + tiles[4].right) * .5f, middle) && close_to((tiles[0].left + tiles[2].right) * .5f, middle) &&
              close_to(tiles[1].left - tiles[0].right, 8) && close_to(tiles[3].top - tiles[0].bottom, 8),
          "call grid: rows and the partial last row are centered with token gaps");
    check(close_to((tiles[0].top + tiles[4].bottom) * .5f, (area.top + area.bottom) * .5f),
          "call grid: a block that fits is centered vertically");
    // Too many for the minimum tile: fixed width, vertical scroll.
    const UiRect small{0, 0, 800, 400};
    check(ui_call_grid_place(small, 40, 8, 0, {tiles, 40}, &layout) && layout.columns == 4 &&
              close_to(layout.tile_width, (800 - 24) / 4.f) && layout.content_height > 400 && close_to(tiles[0].top, 0),
          "call grid: a crowded stage keeps the minimum tile and scrolls");
    UiRect scrolled[40]{};
    check(ui_call_grid_place(small, 40, 8, 120, {scrolled, 40}, &layout) && close_to(scrolled[0].top, -120) &&
              ui_call_grid_place(small, 40, 8, 1e6f, {scrolled, 40}, &layout) &&
              close_to(scrolled[39].bottom, 400),
          "call grid: scrolling offsets rows and clamps at the end");
    check(!ui_call_grid_place(small, 0, 8, 0, {tiles, 40}) && !ui_call_grid_place(small, 3, 8, 0, {tiles, 2}),
          "call grid: rejects empty and undersized output");
}

void fit_cases(Check check) noexcept {
    const UiRect tile{0, 0, 400, 400};
    const auto contain = ui_call_video_fit(tile, 1920, 1080, false);
    const auto cover = ui_call_video_fit(tile, 1920, 1080, true);
    check(close_to(ui_rect_width(contain), 400) && close_to(ui_rect_height(contain), 225) && close_to(contain.top, 87.5f),
          "call video: contain letterboxes inside the tile");
    check(close_to(ui_rect_height(cover), 400) && close_to(ui_rect_width(cover), 711.11f) && close_to(cover.left, -155.55f, .1f),
          "call video: cover fills the tile and is clipped by the host");
    check(!ui_rect_has_area(ui_call_video_fit(tile, 0, 1080, true)), "call video: an empty picture has no rectangle");
}

UiCallParticipant person(u64 id, const char* name, u32 ssrc = 0) noexcept {
    UiCallParticipant p{};
    p.user_id = id;
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    p.name = {name, bytes};
    p.video_ssrc = ssrc;
    return p;
}

void scene_cases(Check check) noexcept {
    static UiCallScene scene;
    UiCallParticipant people[12]{person(1, "Ana", 101), person(2, "Bo"), person(3, "Cy", 103), person(4, "Di")};
    people[0].video_width = 1280;
    people[0].video_height = 720;
    people[1].muted = people[1].speaking = true;
    people[3].live = true;
    UiCallContent content{};
    content.title = utf8_literal("General");
    content.participants = {people, 4};
    content.connected = true;
    const UiRect viewport{300, 0, 1500, 900};
    check(ui_call_scene_update(&scene, content, viewport, 1000, true) && scene.active && scene.tile_count == 4 &&
              !scene.focus_user,
          "call scene: a projection lays out every participant");
    check(close_to(scene.header.bottom - scene.header.top, kUiCallHeaderHeight) &&
              close_to(scene.bar.bottom, viewport.bottom) && scene.stage.top >= scene.header.bottom &&
              scene.stage.bottom <= scene.bar.top,
          "call scene: header, stage and control bar partition the view");
    UiCallTileMedia media{};
    check(ui_call_scene_tile_media(scene, 0, &media) && media.video_ssrc == 101 &&
              close_to(ui_rect_width(media.video) / ui_rect_height(media.video), 16.f / 9.f, .01f) &&
              ui_call_scene_tile_media(scene, 1, &media) && !media.video_ssrc && ui_rect_has_area(media.avatar),
          "call scene: resident video gets a picture rectangle; others an avatar");
    people[2].video_width = 0;
    check(ui_call_scene_tile_media(scene, 2, &media) && !media.video_ssrc,
          "call scene: an announced camera without a picture keeps the avatar");
    // Focus: stage tile plus filmstrip.
    content.focus_user = 3;
    check(ui_call_scene_update(&scene, content, viewport, 1100, true) && scene.focus_user == 3 &&
              scene.tiles[0].user_id == 3 && scene.tiles[0].focused && scene.tile_count == 4 &&
              scene.tiles[1].strip && ui_rect_has_area(scene.strip) &&
              scene.tiles[0].target.bottom <= scene.strip.top,
          "call scene: focus shows one stage tile above the filmstrip");
    const auto& strip_tile = scene.tiles[2];
    const auto hit = ui_call_scene_hit_test(scene, (strip_tile.display.left + strip_tile.display.right) * .5f,
                                            (strip_tile.display.top + strip_tile.display.bottom) * .5f);
    check(hit.user_id == strip_tile.user_id && hit.control == UiCallControl::none,
          "call scene: a filmstrip tile is hit by its user");
    content.focus_user = 99;
    check(ui_call_scene_update(&scene, content, viewport, 1200, true) && !scene.focus_user &&
              !scene.tiles[0].focused,
          "call scene: focus on someone who left falls back to the grid");
    // A crowded filmstrip: tiles past the strip are not visible (and unwanted).
    UiCallParticipant crowd[12]{};
    char names[12][4]{};
    for (u32 i = 0; i < 12; ++i) {
        names[i][0] = static_cast<char>('A' + i);
        crowd[i] = person(10 + i, names[i], 200 + i);
    }
    content.participants = {crowd, 12};
    content.focus_user = 10;
    check(ui_call_scene_update(&scene, content, viewport, 1300, true), "call scene: crowded focus lays out");
    u32 visible = 0;
    for (u32 i = 0; i < scene.tile_count; ++i)
        visible += ui_call_scene_tile_visible(scene, i);
    check(visible > 1 && visible < 12 && ui_call_scene_tile_visible(scene, 0),
          "call scene: filmstrip overflow hides the tiles past its edge");
    const float before = scene.tiles[1].target.left;
    check(ui_call_scene_scroll(&scene, (scene.strip.left + scene.strip.right) * .5f, scene.strip.top + 4, 300) &&
              close_to(scene.tiles[1].target.left, before - 300),
          "call scene: the filmstrip scrolls under the pointer");
    // Spring morph: a focus change starts from the drawn rectangles.
    content.participants = {people, 4};
    content.focus_user = 0;
    check(ui_call_scene_update(&scene, content, viewport, 2000, true), "call scene: back to the grid");
    const UiRect grid_rect = scene.tiles[1].display;
    content.focus_user = 2;
    check(ui_call_scene_update(&scene, content, viewport, 3000, false) && scene.morph.active &&
              ui_rect_equal(scene.tiles[0].display, scene.tiles[0].from) &&
              !ui_rect_equal(scene.tiles[0].display, scene.tiles[0].target) &&
              ui_rect_equal(scene.tiles[0].from, grid_rect) && ui_call_scene_next_wake(scene, 3000, false),
          "call scene: a focus change morphs from the grid rectangle");
    check(ui_call_scene_update(&scene, content, viewport, 3000 + kUiCallMorphMs + 50, false) &&
              ui_rect_equal(scene.tiles[0].display, scene.tiles[0].target) && !scene.morph.active &&
              !ui_call_scene_next_wake(scene, 3500, false),
          "call scene: the morph settles on the target layout");
    content.focus_user = 0;
    check(ui_call_scene_update(&scene, content, viewport, 4000, true) &&
              ui_rect_equal(scene.tiles[0].display, scene.tiles[0].target),
          "call scene: reduced motion settles immediately");
    ui_call_scene_release(&scene);
    check(!scene.active && !ui_call_scene_contains(scene, 600, 400), "call scene: release deactivates");
}

void control_cases(Check check) noexcept {
    static UiCallScene scene;
    UiCallParticipant people[1]{person(7, "Me")};
    UiCallContent content{};
    content.title = utf8_literal("Call");
    content.participants = {people, 1};
    const UiRect viewport{0, 0, 1000, 700};
    check(ui_call_scene_update(&scene, content, viewport, 0, true), "call controls: lay out");
    const auto center = [&](UiCallControl control) noexcept {
        const auto r = scene.controls[static_cast<u32>(control)];
        return UiPoint{(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
    };
    bool placed = true;
    constexpr UiCallControl kControls[]{UiCallControl::mute,         UiCallControl::deafen,
                                        UiCallControl::camera,       UiCallControl::screen_share,
                                        UiCallControl::disconnect,   UiCallControl::fullscreen,
                                        UiCallControl::close};
    for (const auto control : kControls) {
        const auto p = center(control);
        placed = placed && ui_rect_has_area(scene.controls[static_cast<u32>(control)]) &&
                 ui_call_scene_hit_test(scene, p.x, p.y).control == control;
    }
    check(placed, "call controls: every control is placed and hit");
    // The official bar: 44-DIP rounded squares, audio and video groups 4 DIP
    // apart inside and 12 between, then the wider red disconnect.
    const auto box = [&](UiCallControl control) noexcept { return scene.controls[static_cast<u32>(control)]; };
    check(close_to(ui_rect_width(box(UiCallControl::mute)), kUiCallButton) &&
              close_to(ui_rect_height(box(UiCallControl::mute)), kUiCallButton) &&
              close_to(box(UiCallControl::deafen).left - box(UiCallControl::mute).right, kUiCallGroupGap) &&
              close_to(box(UiCallControl::camera).left - box(UiCallControl::deafen).right, kUiCallControlGap) &&
              close_to(box(UiCallControl::screen_share).left - box(UiCallControl::camera).right, kUiCallGroupGap) &&
              close_to(box(UiCallControl::disconnect).left - box(UiCallControl::screen_share).right,
                       kUiCallControlGap) &&
              close_to(ui_rect_width(box(UiCallControl::disconnect)), kUiCallDisconnectWidth) &&
              close_to((box(UiCallControl::mute).left + box(UiCallControl::disconnect).right) * .5f,
                       (viewport.left + viewport.right) * .5f) &&
              ui_call_control_group(UiCallControl::mute) == ui_call_control_group(UiCallControl::deafen) &&
              ui_call_control_group(UiCallControl::camera) != ui_call_control_group(UiCallControl::mute),
          "call controls: grouped 44-DIP buttons centred in the bar with a wider disconnect");
    // Avatar-less participants: initials on their identity colour; the tile
    // plate carries a translucent tint of the same hue.
    const Color hue = ui_call_identity_color(kDefaultTheme, 7), tint = ui_call_tile_tint(kDefaultTheme, 7);
    check(hue.alpha == 1 && tint.alpha > 0 && tint.alpha < .5f && tint.red == hue.red &&
              (hue.red != ui_call_identity_color(kDefaultTheme, 8).red ||
               hue.blue != ui_call_identity_color(kDefaultTheme, 8).blue),
          "call tiles: a stable identity colour per user tints the avatar tile");
    {
        UiTextCommand commands[32]{};
        char owned[256]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream};
        bool initials = false;
        if (ui_text_command_stream_initialize(&stream, {commands, 32}, {owned, sizeof(owned)}) &&
            ui_call_scene_text_output(scene, kDefaultTheme, &output))
            for (u32 i = 0; i < stream.count; ++i)
                initials = initials || (stream.storage[i].text.bytes == 1 && stream.storage[i].text.data[0] == 'M');
        check(initials, "call tiles: an avatar-less participant shows initials");
    }
    check(ui_call_control_enabled(scene, UiCallControl::mute) && ui_call_control_enabled(scene, UiCallControl::disconnect) &&
              !ui_call_control_enabled(scene, UiCallControl::camera) &&
              !ui_call_control_enabled(scene, UiCallControl::screen_share),
          "call controls: camera and screen share stay disabled until sending exists");
    const auto label = [&](UiCallControl control, Utf8View expected) noexcept {
        const Utf8View value = ui_call_control_label(scene, control);
        return value.bytes == expected.bytes && bytes_equal(value.data, expected.data, value.bytes);
    };
    check(label(UiCallControl::mute, utf8_literal("Mute")) && label(UiCallControl::deafen, utf8_literal("Deafen")) &&
              label(UiCallControl::fullscreen, utf8_literal("Full screen")) &&
              !ui_call_control_active(scene, UiCallControl::mute),
          "call controls: idle labels");
    content.self_mute = true;
    content.self_deaf = true;
    content.fullscreen = true;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) && label(UiCallControl::mute, utf8_literal("Unmute")) &&
              label(UiCallControl::deafen, utf8_literal("Undeafen")) &&
              label(UiCallControl::fullscreen, utf8_literal("Exit full screen")) &&
              ui_call_control_active(scene, UiCallControl::mute) &&
              ui_call_control_active(scene, UiCallControl::deafen) &&
              !ui_rect_has_area(scene.controls[static_cast<u32>(UiCallControl::close)]),
          "call controls: toggled state and fullscreen hide the close button");
    content.server_mute = true;
    check(ui_call_scene_update(&scene, content, viewport, 0, true) &&
              label(UiCallControl::mute, utf8_literal("Server muted")),
          "call controls: server mute wins the label");
    UiRect anchor{};
    u32 identity = 0;
    const auto p = center(UiCallControl::camera);
    const Utf8View tip = ui_call_scene_tooltip(scene, p.x, p.y, &anchor, &identity);
    check(tip.bytes && ui_rect_equal(anchor, scene.controls[static_cast<u32>(UiCallControl::camera)]) && identity,
          "call controls: the disabled camera still explains itself");
    const auto d = center(UiCallControl::disconnect);
    check(ui_call_scene_set_hover(&scene, d.x, d.y) && scene.hovered == UiCallControl::disconnect &&
              !ui_call_scene_set_hover(&scene, d.x, d.y) && ui_call_scene_set_hover(&scene, -10, -10) &&
              scene.hovered == UiCallControl::none,
          "call controls: hover changes only when the target changes");
    // Hovering a large tile reveals its fullscreen button.
    const auto& tile = scene.tiles[0];
    const float bx = tile.display.right - 22, by = tile.display.top + 22;
    check(ui_call_scene_set_hover(&scene, bx, by) && scene.hovered == UiCallControl::tile_fullscreen &&
              scene.hovered_user == 7 && ui_call_scene_hit_test(scene, bx, by).control == UiCallControl::tile_fullscreen,
          "call controls: a hovered tile offers fullscreen");
    ui_call_scene_release(&scene);
}

// Fake frame source: counts outstanding texture references.
struct FakeHost {
    u32 sequence[4]{};
    i32 outstanding;
    u32 presents, frames, last_extent;
    u64 wanted_user[16];
    bool wanted_value[16];
    u32 wanted_count;
    bool refuse_wants, evicted;
};
int texture_token;
CallVideoHost fake_host(FakeHost* fake) noexcept {
    CallVideoHost host{};
    host.context = fake;
    host.frame = +[](void* context, u32 ssrc, VideoStreamFrame* output) noexcept {
        auto& f = *static_cast<FakeHost*>(context);
        if (ssrc < 100 || ssrc >= 104)
            return false;
        *output = {};
        output->gpu.texture = reinterpret_cast<ID3D11Texture2D*>(&texture_token);
        output->gpu.width = 1920;
        output->gpu.height = 1080;
        output->display_width = 1920;
        output->display_height = 1080;
        output->ssrc = ssrc;
        output->sequence = f.sequence[ssrc - 100];
        ++f.outstanding;
        ++f.frames;
        return true;
    };
    host.present = +[](void* context, u64, u32, const VideoStreamFrame&, u32 extent) noexcept {
        auto& f = *static_cast<FakeHost*>(context);
        ++f.presents;
        f.last_extent = extent;
        return true;
    };
    host.release = +[](void* context, VideoStreamFrame* frame) noexcept {
        --static_cast<FakeHost*>(context)->outstanding;
        frame->gpu.texture = nullptr;
    };
    host.wanted = +[](void* context, u64 user, bool wanted) noexcept {
        auto& f = *static_cast<FakeHost*>(context);
        if (f.refuse_wants || f.wanted_count == 16)
            return false;
        f.wanted_user[f.wanted_count] = user;
        f.wanted_value[f.wanted_count++] = wanted;
        return true;
    };
    host.resident = +[](void* context, u64, u32) noexcept { return !static_cast<FakeHost*>(context)->evicted; };
    return host;
}

void presenter_cases(Check check) noexcept {
    static CallVideoPresenter presenter;
    call_video_presenter_reset(&presenter);
    FakeHost fake{};
    const auto host = fake_host(&fake);
    CallVideoTarget targets[2]{{1, 100, 640, true}, {2, 101, 640, false}};
    auto result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(result.presented && result.changed && result.visible && fake.presents == 1 && fake.frames == 1 &&
              fake.wanted_count == 2 && fake.wanted_user[0] == 1 && fake.wanted_value[0] &&
              fake.wanted_user[1] == 2 && !fake.wanted_value[1],
          "call video: visible camera wanted and presented; hidden camera unwanted, never polled");
    check(fake.outstanding == 0 && presenter.acquired == presenter.released && presenter.acquired == 1,
          "call video: the frame reference is released after presenting");
    check(fake.last_extent == 640, "call video: a 1080p picture in a 640 px tile is reduced three times");
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(!result.presented && fake.presents == 1 && fake.frames == 2 && fake.wanted_count == 2 &&
              fake.outstanding == 0,
          "call video: an unchanged picture is not presented again and wants are not resent");
    fake.sequence[0] = 5;
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(result.presented && fake.presents == 2 && !result.changed, "call video: a new sequence is presented");
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, false);
    check(fake.frames == 3 && result.visible, "call video: without poll only wants are reconciled");
    fake.evicted = true;
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(result.presented && fake.presents == 3, "call video: an evicted picture is presented again");
    fake.evicted = false;
    targets[1].visible = true;
    targets[0].tile_pixels = targets[1].tile_pixels = 2000;
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(fake.wanted_count == 3 && fake.wanted_user[2] == 2 && fake.wanted_value[2] && fake.presents == 5 &&
              fake.last_extent == 0,
          "call video: scrolling a tile into view wants it; a larger tile re-presents at native size");
    targets[0].visible = targets[1].visible = false;
    result = call_video_presenter_pump(&presenter, host, {targets, 2}, true);
    check(!result.visible && fake.wanted_count == 5 && !fake.wanted_value[3] && !fake.wanted_value[4] &&
              fake.frames == 6,
          "call video: a hidden call view unwants every camera and stops polling");
    // A camera that ends is forgotten without a want; a refused want retries.
    result = call_video_presenter_pump(&presenter, host, {targets, 1}, true);
    check(presenter.count == 1 && fake.wanted_count == 5 && result.changed,
          "call video: an ended camera is forgotten");
    fake.refuse_wants = true;
    targets[0].visible = true;
    result = call_video_presenter_pump(&presenter, host, {targets, 1}, true);
    check(result.want_pending && call_video_presenter_find(presenter, 1)->want == CallVideoWant::unwanted,
          "call video: a refused want stays pending");
    fake.refuse_wants = false;
    result = call_video_presenter_pump(&presenter, host, {targets, 1}, true);
    check(!result.want_pending && call_video_presenter_find(presenter, 1)->want == CallVideoWant::wanted,
          "call video: the pending want is retried");
    check(fake.outstanding == 0 && presenter.acquired == presenter.released,
          "call video: every acquired texture reference was released");
    check(call_video_reduction_extent(1920, 1080, 640) == 640 && call_video_reduction_extent(1920, 1080, 2000) == 0 &&
              call_video_reduction_extent(3840, 2160, 4000) == 1920 && call_video_reduction_extent(1280, 720, 0) == 0 &&
              call_video_reduction_extent(0, 0, 100) == 0,
          "call video: reduction extents cover the tile and cap 4K");
}
} // namespace

// Departures: the voice states decide the tiles once known, so a camera the
// voice server still announces (op 13 late or lost) does not keep a user
// who left or moved; a camera roster change repaints the tiles.
void roster_cases(Check check) noexcept {
    static VoiceState records[8];
    VoiceStateStore states{};
    const CallVideoTarget cameras[3]{{2, 201, 0, false}, {3, 301, 0, false}, {2, 202, 0, false}};
    u64 users[4]{};
    bool valid = voice_state_store_initialize_sparse(&states, {records, 8});
    check(valid && call_video_unlisted_users(states, 10, {cameras, 3}, {users, 4}) == 2 && users[0] == 2 &&
              users[1] == 3,
          "call roster: cameras announced before any voice state of the channel still get tiles");
    valid = voice_state_store_apply(&states, 1, 10, 1, 0) == VoiceStateResult::changed &&
            voice_state_store_apply(&states, 1, 10, 2, voice_self_video) == VoiceStateResult::changed;
    check(valid && !call_video_unlisted_users(states, 10, {cameras, 3}, {users, 4}),
          "call roster: once the channel's voice states are known, an announced camera adds no tile");
    valid = voice_state_store_apply(&states, 1, 0, 2, 0) == VoiceStateResult::changed &&
            voice_state_store_channel_count(states, 10) == 1 &&
            voice_state_store_apply(&states, 1, 11, 1, 0) == VoiceStateResult::changed;
    check(valid && !voice_state_store_channel_count(states, 10) &&
              !call_video_unlisted_users(states, 11, {cameras, 3}, {users, 4}),
          "call roster: a user who disconnects or moves leaves no tile behind for a still-announced camera");
    const CallVideoTarget fewer[1]{{3, 301, 0, false}}, moved[2]{{2, 299, 0, false}, {3, 301, 0, false}};
    check(call_video_roster_changed({cameras, 2}, {fewer, 1}) && call_video_roster_changed({cameras, 2}, {moved, 2}) &&
              !call_video_roster_changed({cameras, 2}, {cameras, 2}),
          "call roster: an added, removed or re-announced camera reprojects the tiles");
}


namespace {
void lifecycle_cases(Check check) noexcept {
    CallViewLifecycle life{};
    check(call_view_lifecycle_step(&life, 20, 0, false, 100) == CallViewStep::keep &&
              call_view_lifecycle_deadline(life, 1'000) == 1'000 + kCallViewJoinGraceMs,
          "call view: a join in flight keeps the view and wakes when its grace runs out");
    check(call_view_lifecycle_step(&life, 20, 10, false, 100) == CallViewStep::keep && !life.joined,
          "call view: the old call lasting while a new join is in flight keeps the view");
    check(call_view_lifecycle_step(&life, 20, 20, false, 200) == CallViewStep::keep && life.joined &&
              !call_view_lifecycle_deadline(life, 1'000),
          "call view: the joined call keeps the view with no grace wake");
    check(call_view_lifecycle_step(&life, 20, 0, false, 300) == CallViewStep::close,
          "call view: leaving or a server disconnect after joining closes the view at once");
    CallViewLifecycle moved{};
    call_view_lifecycle_step(&moved, 20, 20, false, 0);
    check(call_view_lifecycle_step(&moved, 20, 30, false, 50) == CallViewStep::follow,
          "call view: a server move of the joined call follows it at once");
    CallViewLifecycle failed{};
    call_view_lifecycle_step(&failed, 20, 20, false, 0);
    check(call_view_lifecycle_step(&failed, 20, 20, true, 50) == CallViewStep::close,
          "call view: a connection that failed for good closes the view");
    CallViewLifecycle stale{};
    check(call_view_lifecycle_step(&stale, 20, 0, false, kCallViewJoinGraceMs) == CallViewStep::close &&
              call_view_lifecycle_step(&stale, 20, 30, false, kCallViewJoinGraceMs) == CallViewStep::follow,
          "call view: a join that never shows up closes after the grace, a moved one follows");
}
} // namespace

void test_call_view(Check check) noexcept {
    grid_cases(check);
    fit_cases(check);
    scene_cases(check);
    control_cases(check);
    presenter_cases(check);
    roster_cases(check);
    lifecycle_cases(check);
}
} // namespace maicord

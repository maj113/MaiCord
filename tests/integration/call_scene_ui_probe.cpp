#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/call_scene.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {
// Synthetic camera pictures: a horizontal luma ramp over one chroma tint, in
// the NV12 layout the H.264 stream decoders hand to the renderer.
ID3D11Texture2D* synthetic_frame(D3D11Renderer& r, u32 width, u32 height, u8 cb, u8 cr) noexcept {
    static u8 pixels[1280 * 720 * 3 / 2];
    if (width > 1280 || height > 720 || (width | height) & 1)
        return nullptr;
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            pixels[y * width + x] = static_cast<u8>(60 + x * 120 / width);
    u8* chroma = pixels + width * height;
    for (u32 i = 0; i < width * height / 2; i += 2) {
        chroma[i] = cb;
        chroma[i + 1] = cr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_NV12;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    const D3D11_SUBRESOURCE_DATA data{pixels, width, 0};
    ID3D11Texture2D* texture = nullptr;
    return SUCCEEDED(r.graphics.device->CreateTexture2D(&desc, &data, &texture)) ? texture : nullptr;
}
bool present_frame(D3D11Renderer& r, ID3D11Texture2D* texture, u64 user, u32 ssrc, u32 extent) noexcept {
    GpuVideoFrame frame{};
    frame.texture = texture;
    frame.width = 1280;
    frame.height = 720;
    if (!texture)
        return false;
    const auto result = renderer_present_video_frame(&r, media_asset_call_video(user, ssrc), frame, extent);
    if (!result.ok()) {
        log_u64(LogLevel::error, "probe", "call_video_present_error", static_cast<u64>(result.error));
        log_u64(LogLevel::error, "probe", "call_video_present_atlas", static_cast<u64>(result.allocation.error));
    }
    return result.ok();
}
// Reads one back-buffer pixel of a freshly drawn frame (BGRA or RGBA).
bool sample(D3D11Renderer& r, const Theme& theme, float x_dip, float y_dip, u8 rgb[3]) noexcept {
    if (!d3d11_renderer_draw_stream(&r, theme, true, true, nullptr, 10000))
        return false;
    ID3D11Resource* resource = nullptr;
    r.graphics.target->GetResource(&resource);
    ID3D11Texture2D* source = nullptr;
    const HRESULT queried = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (FAILED(queried))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(r.graphics.device->CreateTexture2D(&desc, nullptr, &staging));
    if (ok)
        r.graphics.context->CopyResource(staging, source);
    source->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ok = ok && SUCCEEDED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    const float scale = static_cast<float>(r.dpi) / 96.f;
    const u32 x = static_cast<u32>(x_dip * scale), y = static_cast<u32>(y_dip * scale);
    if (ok && x < desc.Width && y < desc.Height) {
        const u8* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rgb[0] = bgra ? p[2] : p[0];
        rgb[1] = p[1];
        rgb[2] = bgra ? p[0] : p[2];
    } else
        ok = false;
    if (staging) {
        r.graphics.context->Unmap(staging, 0);
        staging->Release();
    }
    return ok;
}
UiPoint center(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}
UiCallParticipant person(u64 id, Utf8View name) noexcept {
    UiCallParticipant p{};
    p.user_id = id;
    p.name = name;
    return p;
}
} // namespace

bool call_scene_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.content_text || !renderer.dpi || !renderer.graphics.device || !renderer.preview_media_atlas)
        return false;
    static UiCallScene scene;
    struct Restore {
        D3D11Renderer& renderer;
        const UiCallScene* previous;
        ID3D11Texture2D* textures[2];
        ~Restore() noexcept {
            renderer.call_scene = previous;
            renderer.retained_presentation.valid = false;
            ui_call_scene_release(&scene);
            for (auto* texture : textures)
                if (texture)
                    texture->Release();
        }
    } restore{renderer, renderer.call_scene, {}};
    // Red and blue tints (BT.601 limited range chroma). The probe atlas is
    // 256 DIP square, so pictures are reduced like a small tile.
    restore.textures[0] = synthetic_frame(renderer, 1280, 720, 90, 240);
    restore.textures[1] = synthetic_frame(renderer, 1280, 720, 240, 110);
    Theme theme = kDefaultTheme;
    const UiRect window{0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi};
    const UiRect docked{minimum(312.f, window.right * .3f), 0, window.right, window.bottom};
    UiCallParticipant people[12]{person(11, utf8_literal("Ana")), person(12, utf8_literal("Bo Lindqvist")),
                                 person(13, utf8_literal("Cy")), person(14, utf8_literal("Dee"))};
    people[0].video_ssrc = 501;
    people[0].video_width = 1280;
    people[0].video_height = 720;
    people[1].speaking = people[1].muted = true;
    people[2].video_ssrc = 503;
    people[2].video_width = 1280;
    people[2].video_height = 720;
    people[3].live = people[3].deafened = people[3].muted = true;
    people[3].self = true;
    UiCallContent content{};
    content.title = utf8_literal("Lounge");
    content.participants = {people, 4};
    content.connected = true;
    content.gap = theme.scale.space_4;
    const u32 frames_before = renderer.media_gpu.video.frames;
    u32 stage = __LINE__;
    bool valid = restore.textures[0] && restore.textures[1];
    stage = valid ? __LINE__ : stage;
    valid = valid && present_frame(renderer, restore.textures[0], 11, 501, 200);
    stage = valid ? __LINE__ : stage;
    valid = valid && present_frame(renderer, restore.textures[1], 13, 503, 200);
    stage = valid ? __LINE__ : stage;
    valid = valid && renderer.media_gpu.video.frames == frames_before + 2;
    // A tile resize changes the reduced picture extent under the same key:
    // the next frame must replace the resident picture, not fail as a size
    // mismatch until LRU eviction (per-frame call_video_present_failed=3).
    stage = valid ? __LINE__ : stage;
    valid = valid && present_frame(renderer, restore.textures[0], 11, 501, 100) &&
            media_atlas_peek(renderer.preview_media_atlas, media_asset_call_video(11, 501)).width <= 100 &&
            present_frame(renderer, restore.textures[0], 11, 501, 200) &&
            media_atlas_peek(renderer.preview_media_atlas, media_asset_call_video(11, 501)).width > 100;
    const auto show = [&](const wchar_t* name) noexcept {
        renderer.call_scene = &scene;
        renderer.retained_presentation.valid = false;
        return renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
               (!capture || capture(name, theme));
    };
    const auto tinted = [&](UiPoint p, bool red) noexcept {
        u8 rgb[3]{};
        if (!sample(renderer, theme, p.x, p.y, rgb))
            return false;
        return red ? rgb[0] > rgb[1] + 40 && rgb[0] > rgb[2] + 40 : rgb[2] > rgb[0] + 40 && rgb[2] > rgb[1] + 20;
    };
    // Grid: two cameras, two avatars, a speaking ring, badges; hover a tile.
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && scene.tile_count == 4;
    if (valid) {
        const auto& tile = scene.tiles[0];
        ui_call_scene_set_hover(&scene, tile.display.right - 22, tile.display.top + 22);
    }
    stage = valid ? __LINE__ : stage;
    valid = valid && show(L"call-grid.bmp") && renderer.media_quad_count >= 2;
    stage = valid ? __LINE__ : stage;
    valid = valid && tinted(center(scene.tiles[0].display), true) && tinted(center(scene.tiles[2].display), false);
    // Avatar-less tiles: the identity disc (initials over it) centred on a
    // plate tinted with the same hue, and 44-DIP rounded-square bar buttons.
    const auto matches_color = [&](UiPoint p, Color expected, int tolerance) noexcept {
        u8 rgb[3]{};
        if (!sample(renderer, theme, p.x, p.y, rgb))
            return false;
        const int channels[3]{static_cast<int>(expected.red * 255 + .5f), static_cast<int>(expected.green * 255 + .5f),
                              static_cast<int>(expected.blue * 255 + .5f)};
        for (u32 i = 0; i < 3; ++i)
            if (rgb[i] - channels[i] > tolerance || channels[i] - rgb[i] > tolerance)
                return false;
        return true;
    };
    stage = valid ? __LINE__ : stage;
    if (valid) {
        UiCallTileMedia media{};
        valid = ui_call_scene_tile_media(scene, 1, &media) && ui_rect_has_area(media.avatar);
        const auto a = media.avatar;
        const auto tile = scene.tiles[1].display;
        const UiPoint disc{a.left + ui_rect_width(a) * .2f, (a.top + a.bottom) * .5f};
        const UiPoint plate{tile.left + 24, tile.bottom - 6};
        const Color identity = ui_call_identity_color(theme, scene.tiles[1].user_id);
        u8 plate_rgb[3]{};
        valid = valid && matches_color(disc, identity, 6) && sample(renderer, theme, plate.x, plate.y, plate_rgb) &&
                !matches_color(plate, theme.surface, 3) &&
                (ui_call_identity_color(theme, 11).green != ui_call_identity_color(theme, 12).green ||
                 ui_call_identity_color(theme, 11).blue != ui_call_identity_color(theme, 12).blue);
        const auto mute = scene.controls[static_cast<u32>(UiCallControl::mute)];
        const auto deafen = scene.controls[static_cast<u32>(UiCallControl::deafen)];
        const auto camera = scene.controls[static_cast<u32>(UiCallControl::camera)];
        const auto hangup = scene.controls[static_cast<u32>(UiCallControl::disconnect)];
        valid = valid && ui_rect_width(mute) == kUiCallButton && ui_rect_height(mute) == kUiCallButton &&
                deafen.left - mute.right == kUiCallGroupGap && camera.left - deafen.right == kUiCallControlGap &&
                ui_rect_width(hangup) == kUiCallDisconnectWidth;
    }
    // Soundboard: the bar button joins camera and screen share (open: brand
    // tint) and a played sound's label sits on the sender's tile.
    content.soundboard_available = content.soundboard_open = true;
    people[1].effect = utf8_literal("\xF0\x9F\xA6\x86 quack");
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && show(L"call-soundboard.bmp");
    if (valid) {
        const auto board = scene.controls[static_cast<u32>(UiCallControl::soundboard)];
        const auto share = scene.controls[static_cast<u32>(UiCallControl::screen_share)];
        const auto hangup = scene.controls[static_cast<u32>(UiCallControl::disconnect)];
        const auto tile = scene.tiles[1].display;
        u8 pill[3]{}, plate[3]{};
        // The label's scrim darkens the top centre of the tile below it.
        valid = ui_rect_width(board) == kUiCallButton && board.left - share.right == kUiCallGroupGap &&
                hangup.left - board.right == kUiCallControlGap && ui_call_control_active(scene, UiCallControl::soundboard) &&
                ui_call_control_label(scene, UiCallControl::soundboard).bytes;
        stage = valid ? __LINE__ : stage;
        valid = valid && sample(renderer, theme, (tile.left + tile.right) * .5f - 30, tile.top + 10, pill) &&
                sample(renderer, theme, tile.left + 24, tile.bottom - 6, plate);
        stage = valid ? __LINE__ : stage;
        valid = valid && pill[0] + pill[1] + pill[2] < plate[0] + plate[1] + plate[2];
    }
    content.soundboard_available = content.soundboard_open = false;
    people[1].effect = {};
    // Header toolbar (text in voice): close, the channel search field and the
    // chat toggle each get their own slot, 8 DIP apart, never overlapping the
    // title, at wide and narrow widths; the search slot is not call input.
    content.chat_available = true;
    content.search_width = 244;
    const float header_widths[]{1400, 900, 640, 480, 380};
    for (u32 w = 0; valid && w < sizeof(header_widths) / sizeof(header_widths[0]); ++w) {
        const float width = minimum(header_widths[w], window.right);
        const UiRect viewport{window.right - width, 0, window.right, window.bottom};
        stage = __LINE__;
        valid = ui_call_scene_update(&scene, content, viewport, 0, true);
        const auto close = scene.controls[static_cast<u32>(UiCallControl::close)];
        const auto chat = scene.controls[static_cast<u32>(UiCallControl::chat)];
        const auto search = scene.search_field;
        const auto title_floor = scene.header.left + 44 + kUiCallHeaderTitleMinimum;
        const auto inside = [&](UiRect r) noexcept {
            return r.left >= scene.header.left && r.right <= scene.header.right && r.top >= scene.header.top &&
                   r.bottom <= scene.header.bottom;
        };
        const auto apart = [](UiRect left, UiRect right) noexcept {
            return !ui_rect_has_area(left) || !ui_rect_has_area(right) || right.left - left.right >= kUiCallHeaderGap;
        };
        valid = valid && ui_rect_has_area(close) && inside(close) && (!ui_rect_has_area(chat) || inside(chat)) &&
                (!ui_rect_has_area(search) || (inside(search) && ui_rect_width(search) >= kUiCallHeaderSearchMinimum &&
                                               search.left >= title_floor)) &&
                apart(search, close) && apart(chat, search) && apart(chat, close) &&
                !ui_rect_has_area(ui_rect_intersection(chat, search)) &&
                !ui_rect_has_area(ui_rect_intersection(search, close));
        stage = valid ? __LINE__ : stage;
        // Wide headers keep every slot; the narrowest drops only the search.
        valid = valid && (width < 480 || (ui_rect_has_area(search) && ui_rect_has_area(chat))) &&
                ui_rect_has_area(chat);
        stage = valid ? __LINE__ : stage;
        if (valid && ui_rect_has_area(search)) {
            const auto c = center(search);
            valid = !ui_call_scene_contains(scene, c.x, c.y) &&
                    ui_call_scene_hit_test(scene, c.x, c.y).control == UiCallControl::none &&
                    ui_call_scene_contains(scene, c.x, scene.header.bottom + 4);
        }
        stage = valid ? __LINE__ : stage;
        if (valid) {
            const auto c = center(chat);
            valid = ui_call_scene_hit_test(scene, c.x, c.y).control == UiCallControl::chat &&
                    ui_call_control_enabled(scene, UiCallControl::chat) &&
                    !ui_call_control_active(scene, UiCallControl::chat);
        }
    }
    // Below the call view's own minimum width the slots still hold: the
    // search is dropped first, then the chat toggle; close always stays.
    if (valid) {
        const auto narrow = ui_call_header_layout({0, 0, 300, kUiCallHeaderHeight}, true, 244);
        const auto tiny = ui_call_header_layout({0, 0, 110, kUiCallHeaderHeight}, true, 244);
        stage = __LINE__;
        valid = !ui_rect_has_area(narrow.search) && ui_rect_has_area(narrow.chat) &&
                narrow.close.left - narrow.chat.right >= kUiCallHeaderGap && ui_rect_has_area(tiny.close) &&
                !ui_rect_has_area(tiny.chat) && !ui_rect_has_area(tiny.search);
    }
    content.chat_open = true;
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) &&
            ui_call_control_active(scene, UiCallControl::chat) &&
            ui_call_control_label(scene, UiCallControl::chat).bytes == 9; // "Hide Chat"
    if (valid) {
        const auto chat = scene.controls[static_cast<u32>(UiCallControl::chat)];
        ui_call_scene_set_hover(&scene, center(chat).x, center(chat).y);
    }
    stage = valid ? __LINE__ : stage;
    valid = valid && show(L"call-header-toolbar.bmp");
    ui_call_scene_set_hover(&scene, -1, -1);
    content.chat_available = content.chat_open = false;
    content.search_width = 0;
    // Focus: the blue camera on the stage above the filmstrip.
    content.focus_user = 13;
    stage = valid ? __LINE__ : stage;
    ui_call_scene_set_hover(&scene, -1, -1);
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && scene.tiles[0].user_id == 13 &&
            show(L"call-focus.bmp") &&
            tinted(center(scene.tiles[0].display), false);
    // Borderless fullscreen covers the whole window.
    content.fullscreen = true;
    content.self_mute = true;
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, window, 0, true) &&
            ui_rect_equal(scene.viewport, window) && show(L"call-fullscreen.bmp");
    // Light theme, a crowded scrolling grid, connecting state.
    theme = kLightTheme;
    static char names[12][8];
    for (u32 i = 0; i < 12; ++i) {
        names[i][0] = 'U';
        names[i][1] = 's';
        names[i][2] = 'e';
        names[i][3] = 'r';
        names[i][4] = ' ';
        names[i][5] = static_cast<char>('A' + i);
        people[i] = person(20 + i, {names[i], 6});
    }
    people[1].video_ssrc = 501; // Announced, picture not resident: avatar.
    content = {};
    content.title = utf8_literal("Town hall");
    content.participants = {people, 12};
    content.gap = theme.scale.space_4;
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, {docked.left, 0, docked.right, minimum(window.bottom, 560.f)}, 0,
                                          true) &&
            show(L"call-crowded-light.bmp");
    // Go Live: a watched stream (synthetic picture under its tagged tile and
    // SSRC), an unwatched one over a synthetic preview and two cameras.
    theme = kDefaultTheme;
    constexpr u64 kStreamTag = 1ull << 63;
    constexpr u32 kStreamSsrc = 0x8000'0105u;
    const MediaAssetKey preview = media_asset_embed(32, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::jpeg);
    static u8 preview_rgba[160 * 90 * 4];
    for (u32 i = 0; i < 160 * 90; ++i) {
        preview_rgba[i * 4] = 40;
        preview_rgba[i * 4 + 1] = static_cast<u8>(140 + (i % 160) / 4);
        preview_rgba[i * 4 + 2] = 60;
        preview_rgba[i * 4 + 3] = 255;
    }
    stage = valid ? __LINE__ : stage;
    valid = valid && renderer_upload_media_straight_rgba8(&renderer, preview, 160, 90, 160 * 4,
                                                          {preview_rgba, sizeof(preview_rgba)})
                         .ok() &&
            present_frame(renderer, restore.textures[0], 31 | kStreamTag, kStreamSsrc, 200);
    people[0] = person(31 | kStreamTag, utf8_literal("Ana"));
    people[0].stream = UiCallStream::live;
    people[0].video_ssrc = kStreamSsrc;
    people[0].video_width = 1280;
    people[0].video_height = 720;
    people[0].viewers = 4;
    people[1] = person(32 | kStreamTag, utf8_literal("Bo Lindqvist"));
    people[1].stream = UiCallStream::available;
    people[1].preview = preview;
    people[2] = person(31, utf8_literal("Ana"));
    people[2].live = true;
    people[3] = person(33, utf8_literal("Cy"));
    people[3].video_ssrc = 503;
    people[3].video_width = 1280;
    people[3].video_height = 720;
    content = {};
    content.title = utf8_literal("Lounge");
    content.participants = {people, 4};
    content.connected = true;
    content.gap = theme.scale.space_4;
    stage = valid ? __LINE__ : stage;
    valid = valid && present_frame(renderer, restore.textures[1], 33, 503, 200) &&
            ui_call_scene_update(&scene, content, docked, 0, true) && scene.tile_count == 4;
    if (valid) {
        const auto& tile = scene.tiles[0];
        ui_call_scene_set_hover(&scene, center(tile.display).x, center(tile.display).y);
    }
    stage = valid ? __LINE__ : stage;
    valid = valid && show(L"call-stream-grid.bmp") && tinted(center(scene.tiles[0].display), true);
    if (valid) {
        // The preview shows beside the Watch stream button (green synthetic image).
        const auto& r = scene.tiles[1].display;
        u8 rgb[3]{};
        valid = sample(renderer, theme, r.left + ui_rect_width(r) * .2f, r.top + ui_rect_height(r) * .3f, rgb) &&
                rgb[1] > rgb[0] + 40 && rgb[1] > rgb[2] + 30;
    }
    // Focused stream in fullscreen.
    content.focus_user = 31 | kStreamTag;
    content.fullscreen = true;
    stage = valid ? __LINE__ : stage;
    ui_call_scene_set_hover(&scene, -1, -1);
    valid = valid && ui_call_scene_update(&scene, content, window, 0, true) && scene.tiles[0].focused &&
            show(L"call-stream-focus.bmp") && tinted(center(scene.tiles[0].display), true);
    // States: connecting over the preview, reconnecting, paused and ended.
    people[0].stream = UiCallStream::connecting;
    people[0].preview = preview;
    people[0].video_width = people[0].video_height = 0;
    people[1] = person(34 | kStreamTag, utf8_literal("Dee"));
    people[1].stream = UiCallStream::reconnecting;
    people[2] = person(35 | kStreamTag, utf8_literal("Eli"));
    people[2].stream = UiCallStream::live;
    people[2].paused = true;
    people[3] = person(36 | kStreamTag, utf8_literal("Flo"));
    people[3].stream = UiCallStream::ended;
    people[3].notice = UiCallStreamNotice::ended;
    content.focus_user = 0;
    content.fullscreen = false;
    scene.spinner_phase = 9;
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && ui_call_scene_spinner_visible(scene) &&
            show(L"call-stream-states.bmp");
    // Stage: a live topic, two speakers, the audience band and a moderator's
    // hand raises; then the invite banner in the light theme.
    static UiCallStagePerson audience[8]{{41, utf8_literal("Ivy")}, {42, utf8_literal("Jon")}, {43, utf8_literal("Kai")},
                                         {44, utf8_literal("Lea")}, {45, utf8_literal("Max")}, {46, utf8_literal("Nia")},
                                         {47, utf8_literal("Oli")}, {48, utf8_literal("Pia")}};
    people[0] = person(11, utf8_literal("Ana"));
    people[0].speaking = true;
    people[1] = person(12, utf8_literal("Bo Lindqvist"));
    people[1].muted = true;
    content = {};
    content.title = utf8_literal("stage-hall");
    content.participants = {people, 2};
    content.connected = true;
    content.gap = theme.scale.space_4;
    content.stage.active = content.stage.live = true;
    content.stage.topic = utf8_literal("Weekly design review");
    content.stage.actions = stage_self_actions(StageRole::speaker, kStageModeratorPermissions, true);
    content.stage.audience_count = 23;
    content.stage.audience = {audience, 8};
    content.stage.request_count = 2;
    content.stage.requests = {audience + 2, 2};
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && scene.tile_count == 2 &&
            ui_rect_has_area(scene.stage_audience) && ui_rect_has_area(scene.stage_requests);
    if (valid)
        ui_call_scene_set_hover(&scene, scene.tiles[1].display.right - 22, scene.tiles[1].display.top + 22);
    stage = valid ? __LINE__ : stage;
    valid = valid && show(L"call-stage.bmp");
    theme = kLightTheme;
    content.gap = theme.scale.space_4;
    content.stage.actions = stage_action_accept_invite | stage_action_decline_invite;
    content.stage.request_count = 0;
    content.stage.requests = {};
    stage = valid ? __LINE__ : stage;
    ui_call_scene_set_hover(&scene, -1, -1);
    valid = valid && ui_call_scene_update(&scene, content, docked, 0, true) && ui_rect_has_area(scene.stage_banner) &&
            show(L"call-stage-invite-light.bmp");
    theme = kDefaultTheme;
    // The preview card beside a voice channel row, light theme.
    static UiStreamCardScene card;
    struct CardRestore {
        D3D11Renderer& renderer;
        const UiStreamCardScene* previous;
        ~CardRestore() noexcept {
            renderer.stream_card_scene = previous;
            ui_stream_card_close(&card);
        }
    } card_restore{renderer, renderer.stream_card_scene};
    renderer.call_scene = nullptr;
    UiStreamCardContent card_content{};
    card_content.name = utf8_literal("Bo Lindqvist");
    card_content.channel = utf8_literal("Lounge");
    card_content.preview = preview;
    card_content.preview_state = UiStreamCardPreview::ready;
    stage = valid ? __LINE__ : stage;
    valid = valid && ui_stream_card_show(&card, 32, card_content, {72, 160, 300, 184}, window, 0, true);
    UiStreamCardMedia card_media{};
    for (u32 pass = 0; valid && pass < 2; ++pass) {
        theme = pass ? kLightTheme : kDefaultTheme;
        renderer.stream_card_scene = &card;
        renderer.retained_presentation.valid = false;
        valid = renderer_rebuild_content_text(&renderer, theme) && probe_present(&renderer, theme) &&
                (!capture || capture(pass ? L"stream-card-light.bmp" : L"stream-card.bmp", theme)) &&
                ui_stream_card_media(card, theme, &card_media);
        u8 rgb[3]{};
        valid = valid && sample(renderer, theme, center(card_media.rectangle).x, center(card_media.rectangle).y, rgb) &&
                rgb[1] > rgb[0] + 40;
    }
    if (!valid)
        log_u64(LogLevel::error, "probe", "call_scene_probe_failure_line", stage);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "call_scene_grid_focus_fullscreen_passed" : "call_scene_probe_failed");
    return valid;
}
} // namespace maicord

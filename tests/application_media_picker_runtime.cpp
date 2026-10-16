// Manual extension: MAICORD_STICKER_CATALOG_FIXTURE=<json file> replays a saved catalog response.
// Compile the shipping controller once in this isolated executable. Private
// ownership remains test-visible without adding a shipping diagnostic hook.
#include "../src/app/application_media_picker.cpp"
#include "../src/app/application_button_input.cpp"
// The pure surface motion controller only (its host stays stubbed below), to
// count gallery open animations the way application_surface_motion observes them.
#define MAICORD_SURFACE_MOTION_CONTROLLER_TEST
#include "../src/app/application_surface_motion.cpp"
#undef MAICORD_SURFACE_MOTION_CONTROLLER_TEST
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord::app_detail {
// No voice message recording in this runtime.
void application_voice_message_pointer(UiButtonPointerEvent, bool, bool) noexcept {}
} // namespace maicord::app_detail

namespace maicord::media_picker_runtime_test {
using namespace app_detail;
struct AccountFixture {
    MediaCatalogQuery query;
    char query_text[kMediaCatalogMaximumQueryBytes];
    u8 target[kMediaCatalogMaximumTargetBytes];
    u32 target_bytes, submissions, cancellations;
    u64 request_id, cancelled_id;
    RestClientSubmitError admission;
} account;
u32 failures, repaints, composer_refreshes, history_pumps, text_builds;
u32 member_activations, launcher_activations;
bool settings;
bool favorites_enabled, favorites_visible;
u32 favorite_toggles;
Utf8View toggled_url, toggled_src;
GifFavoriteFormat toggled_format;
u32 reaction_calls;
u64 reaction_messages[4], reaction_emoji[4];
Utf8View reaction_names[4];
app_detail::ApplicationMessageReactionEnqueueResult reaction_next;
Utf8View reaction_status;
ApplicationSurfaceMotionOwner motion;
u32 animation_starts;
// application_surface_motion's gallery identity: the visible scene and its
// account scope. Every key change restarts the open animation.
void observe_motion(u64 now) noexcept {
    const auto* scene = g_app.renderer.media_picker_scene;
    const bool visible = scene && ui_media_picker_scene_current(*scene);
    const u64 before = motion.scope_serial;
    application_surface_motion_update(&motion, RendererLayerSurface::gallery,
                                      visible ? ApplicationSurfaceMotionKey{reinterpret_cast<u64>(scene),
                                                                            scene->account_scope, 0, 0}
                                              : ApplicationSurfaceMotionKey{},
                                      now, true);
    animation_starts += motion.scope_serial != before;
}
struct PickerCategories {
    UiMediaPickerSectionVisual visuals[16];
    Utf8View labels[16];
    u32 count;
    bool glyphs;
};
// The category list a scene shows, and whether every rail entry draws its
// emoji glyph as text with no icon quad in the rail.
PickerCategories categories(const UiMediaPickerScene& scene) noexcept {
    PickerCategories result{};
    if (!scene.source || scene.source->sections.count > 16)
        return result;
    result.count = scene.source->sections.count;
    for (u32 i = 0; i < result.count; ++i) {
        result.visuals[i] = scene.source->sections[i].visual;
        result.labels[i] = scene.source->sections[i].label;
    }
    static UiTextCommand commands[512];
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    PaintStream paint{};
    result.glyphs = scene.rail_count && ui_text_command_stream_initialize(&stream, {commands, 512}) &&
                    ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr) &&
                    paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 720) &&
                    ui_media_picker_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96);
    for (u32 i = 0; result.glyphs && i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto glyph = ui_media_picker_section_glyph(scene.source->sections[slot.section_index].visual);
        bool drawn = false;
        for (u32 c = 0; glyph.bytes && c < stream.count; ++c)
            drawn |= commands[c].owner.handle.slot == slot.widget.slot && utf8_equal(commands[c].text, glyph);
        result.glyphs = drawn;
    }
    for (u32 i = 0; result.glyphs && i < paint.count; ++i)
        result.glyphs = !ui_icon_quad(paint.quads[i]) ||
                        !ui_rect_has_area(ui_rect_intersection(
                            {paint.quads[i].left, paint.quads[i].top, paint.quads[i].right, paint.quads[i].bottom},
                            scene.rail));
    paint_stream_release(&paint);
    return result;
}
bool same_categories(const PickerCategories& a, const PickerCategories& b) noexcept {
    bool equal = a.count && a.count == b.count && a.glyphs && b.glyphs;
    for (u32 i = 0; equal && i < a.count; ++i)
        equal = a.visuals[i] == b.visuals[i] && utf8_equal(a.labels[i], b.labels[i]);
    return equal;
}
ApplicationGifFavoritesView favorites;
ApplicationGifFavoriteState favorite_state;
GifFavoriteRecord saved_gifs[2];
BYTE saved_keyboard[256];
bool keyboard_saved;
char draft[256];
MessageRecord records[16];
u32 order[16], heights[16];
u64 prefixes[16];
u8 message_text[4096], draft_text[1024], request_text[kMessageComposerMaximumBytes];
MessageDraftRecord draft_records[4];
MessageDraftStore drafts;
u8 generated[65536];

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
void measurement(const char* label, usize value, const char* stage = nullptr) noexcept {
    char output[192], digits[20];
    u32 used = 0, count = 0;
    if (stage) {
        while (*stage)
            output[used++] = *stage++;
        output[used++] = '_';
    }
    while (*label)
        output[used++] = *label++;
    output[used++] = '=';
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        output[used++] = digits[--count];
    output[used++] = '\n';
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, used, &written, nullptr);
}
usize committed(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    return pointer && VirtualQuery(pointer, &information, sizeof(information)) &&
                   information.State == MEM_COMMIT
               ? information.RegionSize
               : 0;
}
void owner_measurements(const char* stage) noexcept {
    const auto* p = g_app.media_picker;
    usize banks = 0, logical_banks = 0;
    if (p) {
        const CatalogBank* owned[]{p->gif, p->guild, p->packs};
        for (const auto* bank : owned)
            if (bank) {
                banks += committed(bank);
                logical_banks += bank->allocation_bytes;
            }
    }
    const auto owner = committed(p);
    const auto filtered = p ? committed(p->filtered) : 0;
    const auto scene = p ? p->scene.storage.arena.committed : 0;
    const auto emoji = p ? p->emoji.storage.committed : 0;
    const auto saved_mask = p ? committed(p->saved_preview_mask) : 0;
    measurement("owner_logical_bytes", application_media_picker_owner_bytes(), stage);
    measurement("owner_vm_committed_bytes", owner, stage);
    measurement("banks_logical_bytes", logical_banks, stage);
    measurement("banks_vm_committed_bytes", banks, stage);
    measurement("filtered_vm_committed_bytes", filtered, stage);
    measurement("scene_arena_used_bytes", p ? p->scene.storage.arena.offset : 0, stage);
    measurement("scene_arena_committed_bytes", scene, stage);
    measurement("emoji_arena_used_bytes", p ? p->emoji.storage.offset : 0, stage);
    measurement("emoji_arena_committed_bytes", emoji, stage);
    measurement("favorites_mask_vm_committed_bytes", saved_mask, stage);
    // The scene/model records are inside the owner. Count their sidecar arenas
    // and each separate bank/filter/mask once. Shared edit history is separate.
    measurement("retained_vm_committed_bytes", owner + banks + filtered + scene + emoji + saved_mask,
                stage);
}
void record_measurements() noexcept {
    measurement("picker_owner_record_bytes", sizeof(ApplicationMediaPicker));
    measurement("picker_bank_header_record_bytes", sizeof(CatalogBank));
    measurement("catalog_item_record_bytes", sizeof(MediaCatalogItem));
    measurement("picker_scene_record_bytes", sizeof(UiMediaPickerScene));
    measurement("picker_projection_record_bytes", sizeof(UiMediaPickerProjection));
    measurement("picker_media_slot_record_bytes", sizeof(UiMediaPickerMediaSlot));
    measurement("catalog_preview_request_record_bytes", sizeof(AccountMediaCatalogPreviewRequest));
    measurement("account_media_frame_record_bytes", sizeof(AccountMediaFrame));
    measurement("application_record_bytes", sizeof(g_app));
    measurement("account_api_record_bytes", sizeof(g_app.account.api));
    measurement("account_host_record_bytes", sizeof(g_app.account));
    measurement("renderer_record_bytes", sizeof(g_app.renderer));
    measurement("account_ui_record_bytes", sizeof(AccountUiRuntime));
    const usize scratch_bytes = kMediaCatalogStructuralCapacity * sizeof(u32);
    // Production releases this scratch before returning a response. Measure
    // its identical allocation separately, never include it in retained banks.
    void* scratch = VirtualAlloc(nullptr, scratch_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(scratch != nullptr, "picker runtime: temporary parser scratch measurement allocates");
    measurement("picker_response_scratch_logical_bytes", scratch_bytes);
    measurement("picker_response_scratch_vm_committed_bytes", committed(scratch));
    if (scratch)
        VirtualFree(scratch, 0, MEM_RELEASE);
}
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
RestClientSubmitResult WINAPI submit(void*, AccountMediaCatalogOperation operation,
                                     const MediaCatalogQuery* query, u64 id) noexcept {
    if (operation == AccountMediaCatalogOperation::cancel) {
        ++account.cancellations;
        account.cancelled_id = id;
        return {query || !id ? RestClientSubmitError::invalid_input
                             : RestClientSubmitError::none};
    }
    if (!query || !id || query->query.bytes > sizeof(account.query_text))
        return {RestClientSubmitError::invalid_input};
    ++account.submissions;
    account.request_id = id;
    account.query = *query;
    CopyMemory(account.query_text, query->query.data, query->query.bytes);
    account.query.query = {account.query_text, query->query.bytes};
    const auto target = media_catalog_write_target(*query, {account.target, sizeof(account.target)});
    account.target_bytes = target.bytes;
    return {target.ok() ? account.admission : RestClientSubmitError::invalid_input};
}
bool response(u64 id, Utf8View body, u16 status = 200, u64 retry_at = 0, bool rate_limited = false) noexcept {
    RestClientResponse value{};
    value.request_id = id;
    value.status_code = status;
    value.body_bytes = body.bytes;
    value.retry_at_ms = retry_at;
    value.rate_limited = rate_limited;
    return application_media_picker_response(value, body);
}
bool query(Utf8View text) noexcept {
    auto* model = application_media_picker_edit_model();
    return model && message_composer_restore(model, 0, text, text.bytes, text.bytes) &&
           application_media_picker_edit_changed(true);
}
bool send_pending(u64 id) noexcept {
    return application_media_picker_submit(id, g_app.media_picker->due_at);
}
constexpr auto gif_body = utf8_literal(
    R"([{"id":"fixture-provider","title":"Fixture GIF","width":640,"height":128,"src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/preview.webp","gif_src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/animated.webp","url":"https://klipy.com/gifs/fixture-send"}])");
constexpr auto guild_body = utf8_literal(
    R"([{"id":"71","guild_id":"7","name":"Server wave","format_type":2,"tags":"wave","description":"Server fixture","available":true}])");
Utf8View full_packs() noexcept {
    ProtocolWriter writer{{generated, sizeof(generated)}, 0};
    protocol_writer_append_literal(
        &writer, "{\"sticker_packs\":[{\"id\":\"42\",\"name\":\"Fixture pack\",\"stickers\":[");
    for (u32 i = 0; i < 60; ++i) {
        if (i)
            protocol_writer_append_literal(&writer, ",");
        if (i == 59)
            protocol_writer_append_literal(
                &writer, "{\"id\":\"99\",\"name\":\"Final "
                         "special\",\"format_type\":4,\"tags\":\"needle\",\"available\":true}");
        else if (i == 1)
            protocol_writer_append_literal(
                &writer, "{\"id\":\"73\",\"name\":\"Unsupported "
                         "vector\",\"format_type\":3,\"tags\":\"vector\",\"available\":true}");
        else if (i == 0)
            protocol_writer_append_literal(
                &writer, "{\"id\":\"72\",\"name\":\"Pack "
                         "wave\",\"format_type\":2,\"tags\":\"curious, ?\\thuh, wave\",\"available\":true}");
        else
            protocol_writer_append_literal(
                &writer, "{\"id\":\"72\",\"name\":\"Pack "
                         "wave\",\"format_type\":2,\"tags\":\"wave\",\"available\":true}");
    }
    protocol_writer_append_literal(&writer, "]}]}");
    return {reinterpret_cast<const char*>(generated), writer.used};
}
UiMediaPickerTarget target(u32 index) noexcept {
    const auto& p = *g_app.media_picker;
    UiMediaPickerItem item{};
    if (!resolve(g_app.media_picker, index, &item))
        return {};
    return {p.scope,
            p.generation,
            item.id,
            p.source.revision,
            index,
            p.scene.generation,
            UiMediaPickerAction::item,
            p.kind};
}
bool capture_edit() noexcept {
    const auto& field = g_app.media_picker->scene.field;
    const auto point = MAKELPARAM(static_cast<short>((field.left + field.right) * .5f),
                                  static_cast<short>((field.top + field.bottom) * .5f));
    return application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point) &&
           g_app.media_picker->captured && GetCapture() == g_app.window;
}
bool capture_body_thumb() noexcept {
    const auto scrollbar = ui_media_picker_scene_scrollbar(g_app.media_picker->scene);
    if (!scrollbar.visible)
        return false;
    const auto point = MAKELPARAM(static_cast<short>((scrollbar.thumb.left + scrollbar.thumb.right) * .5f),
                                  static_cast<short>((scrollbar.thumb.top + scrollbar.thumb.bottom) * .5f));
    return application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point) &&
           g_app.media_picker->scrollbar_dragging && GetCapture() == g_app.window;
}
bool initialize() noexcept {
    keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    BYTE keyboard[256]{};
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord picker runtime fixture", WS_POPUP, -16000,
                                   -16000, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!keyboard_saved || !SetKeyboardState(keyboard) || !g_app.window || !g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                        {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                        {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096}) ||
        !chat_state_store_upsert_guild(
             &ui.chat_state, {7, utf8_literal("Fixture guild"), GuildMutationKind::create, 5})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state, {70, 7, 0, utf8_literal("fixture"), 0, 0, 0})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {80, 0, 0, utf8_literal("fixture-dm"), 0, 0, 1})
             .ok() ||
        !ui_scene_initialize_bootstrap(&g_app.shell_scene) ||
        !ui_scene_layout(&g_app.shell_scene, 1280, 720) ||
        !message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}) ||
        !reaction_preferences_initialize(&ui.reaction_preferences) ||
        !message_composer_scene_initialize_owned(&g_app.message_composer_scene))
        return false;
    ui.chat_state.self_user_id = 5;
    ui.chat_state.selected_guild_id = 7;
    ui.active_channel_id = 70;
    message_composer_reset(&g_app.message_composer, 70, false);
    message_composer_insert_utf8(&g_app.message_composer, utf8_literal("Existing draft"));
    if (!message_composer_scene_update(&g_app.message_composer_scene, &g_app.message_composer,
                                       {320, 620, 1280, 720}, true)
             .ok())
        return false;
    auto& view = g_app.message_view;
    if (!message_store_initialize(&view.store, {records, 16}, {order, 16},
                                  {message_text, sizeof(message_text)}) ||
        !ui_variable_list_initialize(&view.list, {heights, 16}, {prefixes, 16}, 0, ui_extent_units(57)) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, ui_extent_units(57),
                                     ui_extent_units(480), true) ||
        !message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) ||
        !message_composer_controller_initialize(&ui.message_composer_controller, &g_app.message_composer,
                                                &drafts, &view.timeline,
                                                {request_text, sizeof(request_text)}))
        return false;
    g_app.renderer_ready = true;
    g_app.renderer.width = 1280;
    g_app.renderer.height = 720;
    g_app.renderer.dpi = 96;
    g_app.account.instance = &account;
    g_app.account.api.media_catalog = submit;
    return application_media_picker_synchronize();
}
void release() noexcept {
    application_media_picker_release();
    if (g_app.account_ui)
        reaction_preferences_release(&g_app.account_ui->reaction_preferences);
    message_composer_scene_release(&g_app.message_composer_scene);
    message_composer_release(&g_app.message_composer);
    ui_scene_release(&g_app.shell_scene);
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    if (g_app.window)
        DestroyWindow(g_app.window);
    if (g_app.account_ui)
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.window = nullptr;
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    g_app.context_focus = nullptr;
    if (keyboard_saved)
        SetKeyboardState(saved_keyboard);
}
void run() noexcept {
    if (!initialize()) {
        check(false, "picker runtime: hidden host, native scene and durable queue initialize");
        release();
        return;
    }
    record_measurements();
    g_app.renderer.dpi = 144;
    check(client_dip_x(MAKELPARAM(96, -48)) == 64.f && client_dip_y(MAKELPARAM(96, -48)) == -32.f,
          "button host: coordinate bridge preserves signed pixels and real DPI scaling");
    g_app.renderer.dpi = 96;
    owner_measurements("picker_dormant");
    check(!g_app.media_picker && !g_app.renderer.media_picker_scene &&
              !application_media_picker_next_wake(),
          "picker runtime: dormant picker allocates no owner or deadline");
    if (!application_media_picker_open(UiMediaPickerKind::gif) || !g_app.media_picker) {
        check(false, "picker runtime: ordinary GIF control opens native scene");
        release();
        return;
    }
    auto& p = *g_app.media_picker;
    const auto pointer = [](float x, float y) noexcept {
        return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    };
    const float initial_width = ui_rect_width(p.scene.panel), fixed_edge = p.scene.panel.right;
    const float resize_y = (p.scene.panel.top + p.scene.panel.bottom) * .5f;
    const auto resize_generation = p.generation;
    check(application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                   pointer(p.scene.panel.left, resize_y)) &&
              p.resizing && GetCapture() == g_app.window &&
              application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                       pointer(fixed_edge - initial_width - 64, resize_y)) &&
              ui_rect_width(p.scene.panel) == initial_width + 64 && p.scene.panel.right == fixed_edge &&
              p.generation == resize_generation && !p.edit.bytes &&
              application_media_picker_window_message(WM_LBUTTONUP, 0,
                                                       pointer(fixed_edge - initial_width - 64, resize_y)) &&
              !p.resizing && GetCapture() != g_app.window,
          "picker runtime: left-edge captured resize keeps right anchor and query identity");
    const float saved_width = p.preferred_width;
    application_media_picker_close();
    check(application_media_picker_open(UiMediaPickerKind::gif) && p.preferred_width == saved_width &&
              ui_rect_width(p.scene.panel) == saved_width,
          "picker runtime: expression drawer width survives close and reopen");
    check(application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                   pointer(p.scene.panel.left, resize_y)) &&
              application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                       pointer(fixed_edge + 200, resize_y)) &&
              ui_rect_width(p.scene.panel) == initial_width &&
              !application_media_picker_window_message(WM_CANCELMODE, 0, 0) &&
              !p.resizing && GetCapture() != g_app.window,
          "picker runtime: resize clamps to the minimum and native cancel releases capture");
    check(application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON,
                                                   pointer(p.scene.panel.left, resize_y)) &&
              application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON, pointer(-1000, resize_y)) &&
              p.scene.panel.left == p.scene.viewport.left + p.scene.style.metrics.viewport_inset &&
              !application_media_picker_window_message(WM_CAPTURECHANGED, 0, 0) && !p.resizing,
          "picker runtime: outward resize clamps to viewport and lost capture ends the drag");
    p.preferred_width = 0;
    check(application_media_picker_synchronize(), "picker runtime: default width restores after resize");
    owner_measurements("picker_open");
    check(send_pending(101) && account.query.operation == MediaCatalogOperation::gif_categories &&
              account.query.limit == kMediaCatalogMaximumPageItems && !application_media_picker_next_wake(),
          "picker runtime: category admission uses one canonical bounded request");
    check(query(utf8_literal("hello &+")) && account.cancelled_id == 101 && p.submitted_id == 101 &&
              !application_media_picker_pending(p.due_at) && response(101, gif_body) && !p.gif,
          "picker runtime: query replacement cancels and drains old receipt without publishing");
    const auto due = p.due_at;
    check(
        !application_media_picker_pending(due - 1) && application_media_picker_pending(due) &&
            application_media_picker_next_wake() == due && application_media_picker_submit(102, due) &&
            account.query.limit == 50 && kMediaCatalogMinimumGifPageItems == 20 &&
            same({reinterpret_cast<const char*>(account.target), account.target_bytes},
                 utf8_literal(
                     "/api/v10/gifs/search?locale=en-US&q=hello%20%26%2B&media_format=tinywebp&limit=50")),
        "picker runtime: content debounce gates exact encoded native search target and canonical fifty "
        "items");
    const auto admitted_generation = p.generation;
    const auto admitted_submissions = account.submissions;
    const auto admitted_cancellations = account.cancellations;
    check(application_media_picker_edit_changed(false) && p.generation == admitted_generation &&
              account.submissions == admitted_submissions &&
              account.cancellations == admitted_cancellations,
          "picker runtime: caret-only layout keeps query generation and transport admission");
    CopyMemory(generated, gif_body.data, gif_body.bytes);
    check(response(102, {reinterpret_cast<const char*>(generated), gif_body.bytes}) && p.gif &&
              p.gif->count == 1 && p.phase == UiMediaPickerPhase::ready && p.scene.cell_count == 1,
          "picker runtime: accepted exact receipt owns a native visible result bank");
    if (!p.gif) {
        release();
        return;
    }
    owner_measurements("picker_gif_ready");
    for (u32 i = 0; i < gif_body.bytes; ++i)
        generated[i] = '?';
    check(same(p.gif->items[0].send_url, utf8_literal("https://klipy.com/gifs/fixture-send")) &&
              !byte_ranges_overlap({reinterpret_cast<const u8*>(p.gif->items[0].send_url.data),
                                    p.gif->items[0].send_url.bytes},
                                   {generated, gif_body.bytes}),
          "picker runtime: selected canonical URL survives response storage reuse");
    AccountMediaCatalogPreviewRequest preview{};
    check(application_media_picker_preview_request(0, &preview) && preview.catalog_scope == p.scope &&
              preview.catalog_generation == p.gif->generation && preview.width == 512 &&
              preview.height == 102 && preview.format == MediaAttachmentFormat::webp &&
              preview.key.kind == MediaAssetKind::catalog_preview &&
              application_media_picker_preview_current(preview.key, preview.catalog_scope,
                                                       preview.catalog_generation) &&
              !application_media_picker_preview_request(p.scene.cell_count, &preview),
          "picker runtime: visible preview has opaque typed bank ownership and aspect-preserving bounded "
          "dimensions");
    auto wrong = preview.key;
    wrong.flags ^= 1;
    check(!application_media_picker_preview_owner_current(wrong, preview.catalog_scope,
                                                          preview.catalog_generation) &&
              !application_media_picker_preview_current(preview.key, preview.catalog_scope + 1,
                                                        preview.catalog_generation),
          "picker runtime: format flags and account scope cannot alias accepted preview identity");
    const auto cell_count = p.scene.cell_count;
    p.scene.cell_count = 0;
    check(application_media_picker_preview_owner_current(preview.key, preview.catalog_scope,
                                                         preview.catalog_generation) &&
              !application_media_picker_preview_visible(preview.key),
          "picker runtime: offscreen accepted owner remains authoritative without claiming visibility");
    p.scene.cell_count = cell_count;
    focus(p, true);
    application_media_picker_tick(p.blink_at);
    check(account.submissions == admitted_submissions && p.generation == admitted_generation &&
              !application_media_picker_next_wake() && !application_media_picker_pending(~u64{0}),
          "picker runtime: caret timer and idle pumps never duplicate ready catalog requests");
    const auto old_key = preview.key;
    check(capture_edit() && query(utf8_literal("replacement")) && !p.captured &&
              GetCapture() != g_app.window &&
              application_media_picker_preview_owner_current(old_key, p.scope, old_key.variant) &&
              send_pending(103) && capture_edit() && response(103, gif_body) && !p.captured &&
              !p.dragging && GetCapture() != g_app.window &&
              !application_media_picker_preview_owner_current(old_key, p.scope, old_key.variant) &&
              p.gif->first_id > old_key.id,
          "picker runtime: query and accepted bank replacements immediately release real editor capture "
          "while replacing exact monotonic bank authority");
    const auto replacement_key = media_asset_catalog_preview(p.gif->first_id, p.gif->generation);
    check(query(utf8_literal("invalid")) && send_pending(104) && response(104, utf8_literal("{broken")) &&
              p.phase == UiMediaPickerPhase::failed &&
              application_media_picker_preview_owner_current(replacement_key, p.scope,
                                                             replacement_key.variant),
          "picker runtime: malformed replacement preserves last accepted bank without selectable stale "
          "cells");
    check(query(utf8_literal("closed")) && send_pending(105),
          "picker runtime: close fixture admits owned receipt");
    application_media_picker_close();
    check(!application_media_picker_active() && !application_media_picker_next_wake() &&
              !application_media_picker_preview_owner_current(replacement_key, p.scope,
                                                              replacement_key.variant) &&
              application_media_picker_open(UiMediaPickerKind::gif) && p.submitted_id == 105 &&
              response(105, gif_body) && application_media_picker_pending(p.due_at),
          "picker runtime: close and reopen drain old receipt before fresh generation can submit");
    check(send_pending(106), "picker runtime: channel stale fixture submits");
    const auto bank_before_channel = p.gif;
    g_app.account_ui->active_channel_id = 80;
    check(!application_media_picker_active() && !application_media_picker_pending(~u64{0}) &&
              response(106, gif_body) && p.gif == bank_before_channel,
          "picker runtime: channel change immediately fences late response without synchronization");
    g_app.message_composer.channel_id = 80;
    check(application_media_picker_open(UiMediaPickerKind::gif) && p.channel_id == 80 && !p.guild_id &&
              send_pending(107),
          "picker runtime: reopen binds exact current DM ownership");
    const auto old_scope = p.scope;
    g_app.account_ui->chat_state.self_user_id = 6;
    check(
        !application_media_picker_active() && response(107, gif_body) && p.gif == bank_before_channel &&
            application_media_picker_open(UiMediaPickerKind::gif) && p.account_id == 6 &&
            p.scope != old_scope,
        "picker runtime: account replacement cannot accept previous response and allocates a fresh scope");
    check(send_pending(108) && response(108, gif_body),
          "picker runtime: new account current receipt publishes");
    const auto send_target = target(0);
    const auto requests_before = g_app.account_ui->message_composer_controller.request_count;
    auto stale_target = send_target;
    ++stale_target.scene_generation;
    check(activate(p, stale_target) &&
              g_app.account_ui->message_composer_controller.request_count == requests_before,
          "picker runtime: stale scene target cannot queue a neighboring or replaced item");
    check(activate(p, send_target) && !application_media_picker_active() &&
              g_app.account_ui->message_composer_controller.request_count == requests_before + 1 &&
              same(message_composer_text(g_app.message_composer), utf8_literal("Existing draft")),
          "picker runtime: ordinary GIF selection queues durable canonical URL while preserving existing "
          "draft");
    MessageComposerSubmit queued{};
    check(message_composer_controller_prepare_submit(&g_app.account_ui->message_composer_controller,
                                                     clock_monotonic_milliseconds(), &queued)
                  .ok() &&
              queued.channel_id == 80 && !queued.sticker_count &&
              same(queued.content, utf8_literal("https://klipy.com/gifs/fixture-send")),
          "picker runtime: primary-lane payload uses canonical send URL rather than thumbnail URL");
    // Guild and pack fixtures remain entirely local; selection queues but never submits.
    g_app.account_ui->active_channel_id = 70;
    g_app.message_composer.channel_id = 70;
    check(application_media_picker_open(UiMediaPickerKind::sticker) && send_pending(109) &&
              account.query.operation == MediaCatalogOperation::guild_stickers &&
              account.query.guild_id == 7 && response(109, guild_body) && p.guild && p.guild->count == 1 &&
              send_pending(110) && account.query.operation == MediaCatalogOperation::sticker_packs &&
              response(110, full_packs()) && p.packs && p.packs->count == 60 && p.source.count == 61 &&
              p.section_count == 2 && p.sections[0].count == 1 && p.sections[1].count == 60,
          "picker runtime: complete pack bank exceeds visible page and retains server and pack sections");
    if (!p.packs || !p.guild) {
        release();
        return;
    }
    owner_measurements("picker_full_packs_ready");
    check(query(utf8_literal("huh")) && p.phase == UiMediaPickerPhase::ready && p.source.count == 1 &&
              item_at(p, 0)->sticker_id == 72 &&
              same(item_at(p, 0)->tags, utf8_literal("curious, ?\thuh, wave")) && query({}),
          "picker runtime: official tab-separated sticker metadata loads and remains searchable locally");
    const auto sticker_submissions = account.submissions;
    check(capture_body_thumb() && query(utf8_literal("needle")) && !p.captured && !p.scrollbar_dragging &&
              GetCapture() != g_app.window && p.source.count == 1 && p.section_count == 1 &&
              p.sections[0].id == 42 && item_at(p, 0)->sticker_id == 99 &&
              account.submissions == sticker_submissions,
          "picker runtime: local catalog filtering immediately retires real thumb capture and reaches "
          "final full-pack item without another request");
    check(application_media_picker_preview_request(0, &preview) &&
              preview.format == MediaAttachmentFormat::gif &&
              same(preview.url, utf8_literal("https://media.discordapp.net/stickers/99.gif")) &&
              application_media_picker_preview_owner_current(preview.key, preview.catalog_scope,
                                                             preview.catalog_generation),
          "picker runtime: GIF sticker preview resolves exact official path and typed current bank");
    const auto sticker_target = target(0);
    const auto stickers_before = g_app.account_ui->message_composer_controller.request_count;
    check(activate(p, sticker_target) && !application_media_picker_active() &&
              g_app.account_ui->message_composer_controller.request_count == stickers_before + 1 &&
              same(message_composer_text(g_app.message_composer), utf8_literal("Existing draft")),
          "picker runtime: sticker selection queues copied ID without clearing conversation draft");
    const auto& controller = g_app.account_ui->message_composer_controller;
    bool found = false;
    for (const auto& request : controller.requests)
        found |= request.occupied && request.channel_id == 70 && request.sticker_count == 1 &&
                 request.sticker_ids[0] == 99 && !request.text_bytes;
    check(found, "picker runtime: durable sticker payload owns actual sticker ID with empty wire content");
    check(application_media_picker_open(UiMediaPickerKind::sticker) && query(utf8_literal("vector")) &&
              p.source.count == 1,
          "picker runtime: vector sticker remains searchable");
    UiMediaPickerItem unsupported{};
    const auto requests_before_vector = controller.request_count;
    check(application_media_picker_preview_request(0,&preview) &&
              !application_media_picker_preview_rejected(preview.key,preview.catalog_scope+1,
                                                        preview.catalog_generation),
          "picker runtime: stale preview failures cannot revoke current artwork");
    const auto account_before_failure=g_app.account.instance;
    check(application_media_picker_preview_rejected(preview.key,preview.catalog_scope,
                                                    preview.catalog_generation) &&
              application_media_picker_synchronize() && application_media_picker_active() &&
              p.scene.active && g_app.account.instance==account_before_failure &&
              resolve(&p,0,&unsupported) && unsupported.enabled && !unsupported.asset_id &&
              !application_media_picker_preview_request(0,&preview),
          "picker runtime: rejected preview preserves account and pane, stops retries, and retains sticker selection");
    check(resolve(&p, 0, &unsupported) && unsupported.enabled &&
              preview.format == MediaAttachmentFormat::lottie && activate(p, target(0)) &&
              controller.request_count == requests_before_vector + 1,
          "picker runtime: Lottie uses typed JSON artwork and queues its actual sticker ID");
    check(!item_at(p, p.source.count) && !resolve(&p, p.source.count, &unsupported),
          "picker runtime: filtered projection refuses indices outside authoritative source range");
    check(
        application_media_picker_open(UiMediaPickerKind::gif) && query({}) && send_pending(111) &&
            response(
                111,
                utf8_literal(
                    R"({"gifs":[{"id":"fixture-trending","title":"Provider preview","width":200,"height":100,"src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/trending.webp","url":"https://klipy.com/gifs/fixture"}],"categories":[{"name":"Fixture category","src":"https://images-ext-1.discordapp.net/external/fixture/https/static.klipy.com/ii/00000000000000000000000000000000/00/00/category.gif?format=webp&animated=true&width=220&height=220"}]})")) &&
            p.source.count == 2 && application_media_picker_preview_request(0, &preview) &&
            preview.format == MediaAttachmentFormat::webp,
        "picker runtime: provider Trending preview is an owned native WebP landing tile");
    UiMediaPickerItem trending{};
    check(resolve(&p, 0, &trending) && trending.kind == UiMediaPickerItemKind::category &&
              same(trending.label, utf8_literal("Trending GIFs")) &&
              application_media_picker_preview_request(1, &preview) &&
              preview.format == MediaAttachmentFormat::webp,
          "picker runtime: proxied category GIF filename uses explicit WebP output admission");
    auto trending_target = target(0);
    trending_target.action = UiMediaPickerAction::category;
    const auto before_navigation = controller.request_count;
    check(activate(p, trending_target) && p.operation == MediaCatalogOperation::gif_trending &&
              !message_composer_text(p.edit).bytes && controller.request_count == before_navigation &&
              send_pending(112) && account.query.operation == MediaCatalogOperation::gif_trending &&
              response(112, utf8_literal("[]")),
          "picker runtime: Trending tile navigates its real results endpoint without sending its preview");
    check(
        query({}) && send_pending(113) &&
            response(
                113,
                utf8_literal(
                    R"({"categories":[{"name":"Fixture category","src":"https://images-ext-1.discordapp.net/external/fixture/https/static.klipy.com/ii/00000000000000000000000000000000/00/00/category.gif?format=webp&animated=true&width=220&height=220"},{"name":"Unsupported preview","src":"https://example.invalid/category.jpg"}],"gifs":[]})")),
        "picker runtime: returning to landing refreshes the authoritative category bank");
    UiMediaPickerItem unsupported_preview{};
    check(p.gif && p.gif->preview_mask[0] == 1 && resolve(&p, 1, &unsupported_preview) &&
              unsupported_preview.enabled && !unsupported_preview.asset_id &&
              !application_media_picker_preview_request(1, &preview),
          "picker runtime: bank caches preview admission without disabling category navigation");
    auto category_target = target(0);
    category_target.action = UiMediaPickerAction::category;
    check(activate(p, category_target) && p.operation == MediaCatalogOperation::gif_search &&
              same(message_composer_text(p.edit), utf8_literal("Fixture category")) && send_pending(114),
          "picker runtime: explicit category activation requests its query while preserving outbox");
    const auto retry_at = clock_monotonic_milliseconds() + 5000;
    check(response(114, {}, 429, retry_at, true) && p.due_at == retry_at &&
              !application_media_picker_pending(retry_at - 1) &&
              application_media_picker_pending(retry_at) &&
              application_media_picker_next_wake() == retry_at,
          "picker runtime: authoritative rate limit gates retries through the existing primary lane");
    check(send_pending(1900) && response(1900, {}, 429, retry_at, false) && !p.wanted &&
              !application_media_picker_pending(~u64{0}) && !application_media_picker_next_wake(),
          "picker runtime: terminal verification 429 ignores a stray deadline and does not retry");
    check(query(utf8_literal("no retry deadline")) && send_pending(1901) &&
              response(1901, {}, 429, 0, true) && !p.wanted &&
              !application_media_picker_pending(~u64{0}) && !application_media_picker_next_wake(),
          "picker runtime: a rate flag without a server deadline does not invent a timer");
    focus(p, true);
    SetCapture(g_app.window);
    p.captured = p.dragging = true;
    settings = true;
    check(application_media_picker_synchronize() && !application_media_picker_active() && !p.captured &&
              !p.dragging && GetCapture() != g_app.window && !application_media_picker_caret_wake() &&
              !g_app.renderer.media_picker_scene,
          "picker runtime: modal front retires native capture, projection and caret ownership");
    settings = false;
    favorites_enabled = true;
    g_app.account.api.gif_favorites = [](void*, AccountGifFavoritesOperation, ConstSlice<u8>,
                                         u64) noexcept -> RestClientSubmitResult { return {}; };
    saved_gifs[0] = {};
    saved_gifs[0].url = utf8_literal("https://klipy.com/gifs/HELLO-world_animated");
    saved_gifs[0].src =
        utf8_literal("https://static.klipy.com/ii/00000000000000000000000000000000/00/00/saved.webp");
    saved_gifs[0].width = 160;
    saved_gifs[0].height = 90;
    saved_gifs[0].format = GifFavoriteFormat::image;
    saved_gifs[1] = {};
    saved_gifs[1].url = utf8_literal("https://klipy.com/gifs/legacy-video");
    // A Klipy VIDEO favourite as the official client stores one without a
    // gif_src (the reported https://klipy.com/gifs/cat-drool-1).
    saved_gifs[1].src = utf8_literal(
        "https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4");
    saved_gifs[1].width = 498;
    saved_gifs[1].height = 280;
    saved_gifs[1].format = GifFavoriteFormat::video;
    favorites = {{saved_gifs, 2}, 1, 1, true, false, false, {}};
    favorite_state = {true, false, false, true};
    check(
        application_media_picker_open(UiMediaPickerKind::gif) && query({}) && send_pending(115) &&
            response(
                115,
                utf8_literal(
                    R"({"categories":[{"name":"Fixture category","src":"https://static.klipy.com/ii/00000000000000000000000000000000/00/00/category.webp"}],"gifs":[]})")) &&
            p.source.count == 2 && favorites_visible,
        "picker Favorites: landing adds one lazy Favorites tile before remote categories");
    auto favorite_category = target(0);
    favorite_category.action = UiMediaPickerAction::category;
    const auto requests_before_favorites = account.submissions;
    const auto queued_before_favorites = g_app.account_ui->message_composer_controller.queued;
    check(activate(p, favorite_category) && p.favorites_view && p.source.count == 2 &&
              !application_media_picker_pending(~u64{0}) && query(utf8_literal("hello WORLD animated")) &&
              p.source.count == 1 && account.submissions == requests_before_favorites,
          "picker Favorites: normalized local filter ignores spaces, hyphens, underscores and ASCII case");
    UiMediaPickerItem saved_item{};
    check(resolve(&p, 0, &saved_item) && saved_item.favorite && saved_item.asset_id &&
              application_media_picker_preview_request(0, &preview),
          "picker Favorites: confirmed image borrows exact owned source and produces an admitted preview");
    const auto saved_preview = preview;
    auto star = target(0);
    star.action = UiMediaPickerAction::favorite;
    favorite_state.pending = true;
    ++favorites.revision;
    application_media_picker_favorites_changed();
    check(resolve(&p, 0, &saved_item) && saved_item.favorite_pending &&
              application_media_picker_preview_owner_current(saved_preview.key, saved_preview.catalog_scope,
                                                             saved_preview.catalog_generation) &&
              activate(p, star) && !favorite_toggles,
          "picker Favorites: pending state preserves preview identity and rejects stale or repeated star "
          "action");
    favorite_state.pending = false;
    ++favorites.revision;
    application_media_picker_favorites_changed();
    star = target(0);
    star.action = UiMediaPickerAction::favorite;
    check(activate(p, star) && favorite_toggles == 1 && same(toggled_url, saved_gifs[0].url) &&
              same(toggled_src, saved_gifs[0].src) &&
              g_app.account_ui->message_composer_controller.queued == queued_before_favorites &&
              same(message_composer_text(g_app.message_composer), utf8_literal("Existing draft")),
          "picker Favorites: star routes exact mutation source without sending or altering draft");
    favorites.failed = true;
    favorites.status =
        utf8_literal("Favorites changed on another device. Try again.");
    favorite_state.failed = true;
    ++favorites.revision;
    application_media_picker_favorites_changed();
    check(p.content.phase == UiMediaPickerPhase::ready && p.source.count == 1 && p.scene.cell_count == 1 &&
              same(p.content.status, favorites.status) && resolve(&p, 0, &saved_item) &&
              saved_item.favorite && saved_item.favorite_failed,
          "picker Favorites: conflict retains authoritative visible records and failure status instead of "
          "a blank retry trap");
    favorites.failed = false;
    favorite_state.failed = false;
    ++favorites.revision;
    application_media_picker_favorites_changed();
    check(query({}) && resolve(&p, 1, &saved_item) && saved_item.enabled && saved_item.asset_id &&
              application_media_picker_preview_request(1, &preview) &&
              preview.format == MediaAttachmentFormat::mp4 && same(preview.url, saved_gifs[1].src) &&
              media_asset_catalog_format(preview.key) == MediaAttachmentFormat::mp4 &&
              (preview.key.flags & media_asset_flag_animated) != 0 && media_asset_key_valid(preview.key) &&
              saved_item.asset_id == preview.key.id && saved_item.asset_flags == preview.key.flags &&
              preview.width == 498 && preview.height == 280 &&
              application_media_picker_preview_owner_current(preview.key, preview.catalog_scope,
                                                             preview.catalog_generation),
          "picker Favorites: a Klipy VIDEO favourite's MP4 is a muted looping video tile, not a guessed "
          "WebP sibling");
    star = target(1);
    star.action = UiMediaPickerAction::favorite;
    check(activate(p, star) && favorite_toggles == 2 && same(toggled_src, saved_gifs[1].src) &&
              toggled_format == GifFavoriteFormat::video,
          "picker Favorites: an MP4 favourite round-trips as FavoriteGIF.format VIDEO");
    {
        // The other stored shapes at the picker boundary.
        GifFavoriteRecord shapes[5]{};
        shapes[0].url = utf8_literal("https://klipy.com/gifs/legacy-webm");
        shapes[0].src =
            utf8_literal("https://static.klipy.com/ii/00000000000000000000000000000000/00/00/legacy.webm");
        shapes[0].format = GifFavoriteFormat::video;
        // An embed favourite: the official client stores the proxied media.
        shapes[1].url = utf8_literal("https://gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp");
        shapes[1].src = utf8_literal("https://images-ext-1.discordapp.net/external/Zm9vYmFyU2lnbmF0dXJl_-1/https/"
                                     "gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp?animated=true");
        shapes[1].format = GifFavoriteFormat::image;
        // Unsigned: only the media proxy may reach an arbitrary host.
        shapes[2].url = utf8_literal("https://gif.fxtwitter.com/raw");
        shapes[2].src = utf8_literal("https://gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp");
        shapes[2].format = GifFavoriteFormat::image;
        shapes[3].url = utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mp4");
        shapes[3].src = utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mp4?ex=1&is=2&hm=ab");
        shapes[3].format = GifFavoriteFormat::video;
        shapes[4].url = utf8_literal("https://imgur.com/gifv");
        shapes[4].src = utf8_literal("https://images-ext-2.discordapp.net/external/c2ln/https/i.imgur.com/a.mp4");
        shapes[4].format = GifFavoriteFormat::video;
        for (auto& record : shapes) {
            record.width = 498;
            record.height = 280;
        }
        const auto restore = favorites;
        favorites = {{shapes, 5}, restore.revision + 1, restore.data_revision + 1, true, false, false, {}};
        application_media_picker_favorites_changed();
        UiMediaPickerItem shape{};
        AccountMediaCatalogPreviewRequest shaped[5]{};
        bool admitted[5]{};
        for (u32 cell = 0; cell < p.scene.cell_count; ++cell) {
            UiMediaPickerMediaSlot slot{};
            AccountMediaCatalogPreviewRequest request{};
            if (ui_media_picker_scene_media(p.scene, cell, &slot) && slot.target.source_index < 5 &&
                application_media_picker_preview_request(cell, &request)) {
                admitted[slot.target.source_index] = true;
                shaped[slot.target.source_index] = request;
            }
        }
        check(p.source.count == 5 && resolve(&p, 0, &shape) && shape.enabled && !shape.asset_id && !admitted[0] &&
                  resolve(&p, 2, &shape) && shape.enabled && !shape.asset_id && !admitted[2],
              "picker Favorites: undecodable WebM and unsigned external sources keep the sendable fallback tile");
        check(resolve(&p, 1, &shape) && shape.asset_id && admitted[1] &&
                  shaped[1].format == MediaAttachmentFormat::webp && same(shaped[1].url, shapes[1].src),
              "picker Favorites: an external animated WebP behind the signed proxy is an animated tile");
        check(admitted[3] && shaped[3].format == MediaAttachmentFormat::mp4 && admitted[4] &&
                  shaped[4].format == MediaAttachmentFormat::mp4 &&
                  media_asset_catalog_format(shaped[4].key) == MediaAttachmentFormat::mp4,
              "picker Favorites: attachment and proxied MP4 favourites are video tiles");
        favorites = {restore.records, restore.revision + 2, restore.data_revision + 2, true, false, false, {}};
        application_media_picker_favorites_changed();
        check(p.source.count == 2 && resolve(&p, 1, &shape) && shape.asset_id,
              "picker Favorites: restoring the authoritative records restores their tiles");
    }
    p.guild_id = 7;
    p.sticker_guild_done = false;
    check(query(utf8_literal("legacy")) && switch_tab(p, UiMediaPickerKind::sticker) &&
              !favorites_visible && !p.favorites_view && p.gif_favorites_view && send_pending(116) &&
              switch_tab(p, UiMediaPickerKind::gif) && response(116, guild_body) && p.favorites_view &&
              p.gif_favorites_view && !p.wanted && p.source.count == 1 &&
              same(message_composer_text(p.edit), utf8_literal("legacy")),
          "picker Favorites: tab restoration fences cancelled sticker receipt and preserves local view and "
          "query");
    application_media_picker_close();
    check(!favorites_visible && application_media_picker_open(UiMediaPickerKind::gif) && p.favorites_view &&
              p.source.count == 1,
          "picker Favorites: reopening preserves view without issuing remote catalog search");
    auto back = target(0);
    back.action = UiMediaPickerAction::back;
    check(activate(p, back) && !p.favorites_view && !p.edit.bytes && p.wanted &&
              p.operation == MediaCatalogOperation::gif_categories,
          "picker Favorites: back restores remote categories and clears local filter");
    const auto common_panel = p.scene.panel;
    const auto submitted_before = account.submissions;
    check(send_pending(117) && switch_tab(p, UiMediaPickerKind::emoji) &&
              p.kind == UiMediaPickerKind::emoji && p.emoji.search == &p.edit && p.emoji.search_borrowed &&
              p.scene.content->available_tabs == 7 && !g_app.popup_scene.active &&
              ui_rect_equal(p.scene.panel, common_panel) && p.source.count > 100 && !p.wanted &&
              p.submitted_id == 117 && account.cancelled_id == 117 &&
              account.submissions == submitted_before + 1,
          "common picker: native Emoji borrows the single edit/catalog source and cancels exact cloud loan "
          "without a second popup");
    const auto emoji_panel = p.scene.panel;
    owner_measurements("picker_emoji_active");
    check(switch_tab(p, UiMediaPickerKind::sticker) && !p.emoji.storage.base &&
              ui_rect_equal(p.scene.panel, emoji_panel) && switch_tab(p, UiMediaPickerKind::gif) &&
              ui_rect_equal(p.scene.panel, emoji_panel) && response(117, gif_body) && !p.submitted_id &&
              p.kind == UiMediaPickerKind::gif && p.wanted,
          "common picker: all three tab states share outer geometry and drain older cancelled receipt "
          "without overwriting successor");
    owner_measurements("picker_emoji_departed");
    check(switch_tab(p, UiMediaPickerKind::emoji) && query(utf8_literal("smile")),
          "common picker: Emoji query uses native shared edit and local result filtering");
    const auto matches = p.source.count;
    UiMediaPickerItem emoji_item{};
    check(matches && resolve(&p, 0, &emoji_item) && emoji_item.kind == UiMediaPickerItemKind::emoji &&
              emoji_item.glyph.bytes && !emoji_item.asset_id && switch_tab(p, UiMediaPickerKind::sticker) &&
              switch_tab(p, UiMediaPickerKind::emoji) &&
              same(message_composer_text(p.edit), utf8_literal("smile")) && p.source.count == matches &&
              !application_media_picker_preview_request(0, &preview),
          "common picker: per-tab query restores with real Unicode glyph and cannot enter catalog-preview "
          "domain");
    check(query({}), "common picker: clearing local search restores complete emoji sections");
    UiScrollRange wheel_range{};
    UiScrollHost wheel_host{};
    wheel_host.immediate = true;
    const UiScrollBinding wheel_binding{&p, p.scope, body_scroll_read, body_scroll_write};
    const UiPoint wheel_point{(p.scene.body.left + p.scene.body.right) * .5f,
                              (p.scene.body.top + p.scene.body.bottom) * .5f};
    check(body_scroll_read(&p, p.scope, &wheel_range) && wheel_range.line == kUiScrollLineDips &&
              ui_scroll_host_wheel(&wheel_host, wheel_binding, wheel_point, -120, 3, false, 1) ==
                  UiScrollDispatch::handled && p.scene.scroll_dips == 3 * kUiScrollLineDips &&
              ui_scroll_host_wheel(&wheel_host, wheel_binding, wheel_point, -30, 3, false, 2) ==
                  UiScrollDispatch::handled && p.scene.scroll_dips == 3.75f * kUiScrollLineDips &&
              ui_scroll_host_wheel(&wheel_host, wheel_binding, wheel_point, 120, 3, false, 3) ==
                  UiScrollDispatch::handled && p.scene.scroll_dips == .75f * kUiScrollLineDips &&
              ui_scroll_host_wheel(&wheel_host, wheel_binding, wheel_point, 30, 3, false, 4) ==
                  UiScrollDispatch::handled && p.scene.scroll_dips == 0,
          "common picker: shared wheel line gives symmetric detents and high-resolution deltas");
    focus(p, true);
    u8 keyboard[256]{}, shifted_keyboard[256]{};
    const bool keyboard_ready = GetKeyboardState(keyboard) != FALSE;
    CopyMemory(shifted_keyboard, keyboard, sizeof(keyboard));
    shifted_keyboard[VK_SHIFT] = 0x80;
    check(keyboard_ready && SetKeyboardState(shifted_keyboard) &&
              application_media_picker_input(UiInputKind::key_down, VK_TAB, false) && !p.focused &&
              focused_target(p).action == UiMediaPickerAction::tab &&
              focused_target(p).tab == UiMediaPickerKind::emoji,
          "common picker: Shift+Tab returns from the single search edit to the shared tab strip");
    SetKeyboardState(keyboard);
    application_media_picker_input(UiInputKind::key_up, VK_TAB, false);
    const auto category_bar = ui_media_picker_scene_scrollbar(p.scene, true);
    if (category_bar.visible) {
        const auto point =
            MAKELPARAM(static_cast<short>(category_bar.thumb.right - 1),
                       static_cast<short>((category_bar.thumb.top + category_bar.thumb.bottom) * .5f));
        const float prior = p.scene.rail_scroll_dips;
        check(application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point) &&
                  !p.scrollbar_dragging &&
                  application_media_picker_window_message(WM_LBUTTONUP, 0, point) &&
                  p.scene.rail_scroll_dips == prior,
              "common picker: category rail has no invisible draggable thumb over its icons");
    }
    const auto scrollbar = ui_media_picker_scene_scrollbar(p.scene);
    if (scrollbar.visible) {
        const auto point = [](float x, float y) noexcept {
            return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
        };
        const float x = (scrollbar.thumb.left + scrollbar.thumb.right) * .5f;
        const float y = (scrollbar.thumb.top + scrollbar.thumb.bottom) * .5f;
        const auto generation = p.generation;
        const u32 builds_before_drag = text_builds;
        check(application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point(x, y)) &&
                  p.scrollbar_dragging &&
                  application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON, point(x, y + 60)) &&
                  p.scene.scroll_dips > 0 && p.generation == generation &&
                  application_media_picker_window_message(WM_LBUTTONUP, 0, point(x, y + 60)) &&
                  !p.scrollbar_dragging,
              "common picker: thumb drag scrolls real native body without changing source/query or "
              "activating a cell");
        const auto burst_scrollbar = ui_media_picker_scene_scrollbar(p.scene);
        const float burst_y = (burst_scrollbar.thumb.top + burst_scrollbar.thumb.bottom) * .5f;
        bool burst = application_media_picker_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point(x, burst_y));
        for (u32 i = 0; i < 64; ++i)
            burst &= application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON,
                                                             point(x, burst_y + (i % 17) + 1));
        burst &= application_media_picker_window_message(WM_LBUTTONUP, 0, point(x, burst_y + 13));
        check(burst && g_app.frame_pending && !p.scrollbar_dragging && p.generation == generation &&
                  text_builds == builds_before_drag && ui_media_picker_scene_current(p.scene),
              "common picker: actual drag bursts publish current hit geometry without per-input glyph "
              "builds");
        const auto next = ui_media_picker_scene_scrollbar(p.scene);
        check(application_media_picker_window_message(
                  WM_LBUTTONDOWN, MK_LBUTTON, point(x, (next.thumb.top + next.thumb.bottom) * .5f)) &&
                  query(utf8_literal("smile")) && !p.scrollbar_dragging && !p.captured &&
                  GetCapture() != g_app.window &&
                  application_media_picker_window_message(WM_MOUSEMOVE, MK_LBUTTON, point(x, y)) &&
                  !p.scrollbar_dragging,
              "common picker: publication/query successor revokes scrollbar capture rather than "
              "retargeting it");
    } else
        check(false, "common picker: complete emoji catalog has genuine overflow scrollbar");
    ReactionPickerSelection inserted_emoji{};
    check(reaction_picker_result_at(p.emoji, 0, &inserted_emoji) &&
              message_composer_restore(&g_app.message_composer, 70, utf8_literal("abcdef"), 2, 5) &&
              activate(p, target(0)) && !application_media_picker_active() &&
              message_composer_text(g_app.message_composer).bytes == 3 + inserted_emoji.name.bytes &&
              bytes_equal(message_composer_text(g_app.message_composer).data, "ab", 2) &&
              bytes_equal(message_composer_text(g_app.message_composer).data + 2, inserted_emoji.name.data,
                          inserted_emoji.name.bytes) &&
              message_composer_text(g_app.message_composer).data[2 + inserted_emoji.name.bytes] == 'f',
          "common picker: Unicode selection replaces exact composer selection while preserving surrounding "
          "draft");
    MessageDraftSnapshot inserted_draft{};
    check(message_draft_store_snapshot(drafts, 70, &inserted_draft) &&
              same(inserted_draft.text, message_composer_text(g_app.message_composer)) &&
              inserted_draft.anchor == g_app.message_composer.anchor &&
              inserted_draft.caret == g_app.message_composer.caret,
          "common picker: Unicode insertion commits the channel draft before the picker closes");
    check(!p.emoji.storage.base && application_media_picker_open(UiMediaPickerKind::emoji) &&
              p.emoji.storage.base && same(message_composer_text(p.edit), utf8_literal("smile")) &&
              message_composer_enable_history(&p.edit, 2048) &&
              message_composer_insert_scalar(&p.edit, 's') == MessageComposerResult::changed &&
              application_media_picker_edit_changed(),
          "common picker: reopening recreates only the Emoji source and restores its query");
    const auto edit_identity = p.edit.document_identity;
    auto* const edit_history = p.edit.history;
    auto* const edit_text = p.edit.text;
    application_media_picker_close();
    owner_measurements("picker_emoji_closed");
    check(!p.emoji.storage.base && p.edit.document_identity == edit_identity &&
              p.edit.history == edit_history && edit_history && p.edit.text == edit_text &&
              same(message_composer_text(p.edit), utf8_literal("smiles")) &&
              message_composer_undo(&p.edit) == MessageComposerResult::changed &&
              same(message_composer_text(p.edit), utf8_literal("smile")) &&
              message_composer_redo(&p.edit) == MessageComposerResult::changed &&
              same(message_composer_text(p.edit), utf8_literal("smiles")),
          "common picker: inactive Emoji frees its arena without retiring the borrowed edit or undo owner");
    check(application_media_picker_open(UiMediaPickerKind::emoji) && query({}) &&
              ui_media_picker_scene_scroll(&p.scene, 120),
          "common picker: complete reopened source provides a genuine saved scroll position");
    const float saved_emoji_scroll = p.scene.scroll_dips;
    application_media_picker_close();
    check(!p.emoji.storage.base && application_media_picker_open(UiMediaPickerKind::emoji) &&
              p.emoji.storage.base && !p.edit.bytes && p.scene.scroll_dips == saved_emoji_scroll,
          "common picker: arena retirement preserves per-tab query and scroll across reopen");
    application_media_picker_close();
    const auto route_pointer = [](UINT message, LPARAM point) noexcept {
        return application_media_picker_window_message(message, 0, point) ||
               application_button_pointer(message, point);
    };
    const auto center = [](UiRect rectangle) noexcept {
        return MAKELPARAM(static_cast<short>((rectangle.left + rectangle.right) * .5f),
                          static_cast<short>((rectangle.top + rectangle.bottom) * .5f));
    };
    const auto* member_node =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::member_toggle));
    check(member_node != nullptr, "button host: actual shell publishes a member toggle");
    if (member_node) {
        const auto member_point = center(member_node->rectangle);
        const auto before_member = member_activations;
        const bool initial_visibility = g_app.member_pane_visible;
        check(route_pointer(WM_LBUTTONDOWN, member_point) &&
                  g_app.button_pointer_owner == &g_app.shell_scene.widgets &&
                  GetCapture() == g_app.window && route_pointer(WM_LBUTTONUP, member_point) &&
                  member_activations == before_member + 1 &&
                  g_app.member_pane_visible != initial_visibility,
              "button host: owned HWND member press/release activates exactly once");
        check(route_pointer(WM_LBUTTONDBLCLK, member_point) &&
                  g_app.button_pointer_owner == &g_app.shell_scene.widgets &&
                  route_pointer(WM_LBUTTONUP, member_point) && member_activations == before_member + 2 &&
                  g_app.member_pane_visible == initial_visibility && !g_app.button_pointer_owner &&
                  GetCapture() != g_app.window,
              "button host: native double-click second press/release toggles once without a delay");
    }
    const auto* gif_node =
        message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::gif);
    const auto* sticker_node =
        message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::sticker);
    check(gif_node && sticker_node, "button host: real composer publishes both picker launchers");
    if (gif_node && sticker_node) {
        const auto gif_point = center(gif_node->rectangle);
        const auto sticker_point = center(sticker_node->rectangle);
        const auto before_launcher = launcher_activations;
        check(route_pointer(WM_LBUTTONDOWN, gif_point) && route_pointer(WM_LBUTTONUP, gif_point) &&
                  p.active && p.kind == UiMediaPickerKind::gif &&
                  launcher_activations == before_launcher + 1,
              "button host: actual launcher press/release opens GIF once");
        check(route_pointer(WM_LBUTTONDOWN, gif_point) && p.active &&
                  route_pointer(WM_LBUTTONUP, gif_point) && !p.active &&
                  launcher_activations == before_launcher + 2,
              "button host: same launcher preserves current owner on press then closes on release");
        check(route_pointer(WM_LBUTTONDOWN, gif_point) && route_pointer(WM_LBUTTONUP, gif_point) &&
                  p.active && p.kind == UiMediaPickerKind::gif &&
                  route_pointer(WM_LBUTTONDOWN, sticker_point) && p.active &&
                  p.kind == UiMediaPickerKind::gif && route_pointer(WM_LBUTTONUP, sticker_point) &&
                  p.active && p.kind == UiMediaPickerKind::sticker &&
                  launcher_activations == before_launcher + 4,
              "button host: other launcher changes tab on one release without dismiss/reopen");
        const auto outside = MAKELPARAM(4, 4);
        check(!application_media_picker_window_message(WM_LBUTTONDOWN, 0, outside) && !p.active &&
                  launcher_activations == before_launcher + 4,
              "button host: ordinary outside press still dismisses without activating a launcher");
    }
    // The official reaction popout is the same emoji picker without the
    // GIF/Sticker/Emoji selector, beside the Add Reaction button.
    u8 react_keyboard[256]{}, shift_keyboard[256]{};
    GetKeyboardState(react_keyboard);
    CopyMemory(shift_keyboard, react_keyboard, sizeof(react_keyboard));
    shift_keyboard[VK_SHIFT] = 0x80;
    check(message_store_upsert(&g_app.message_view.store,
                               {9100, 70, 5, 2000, 0, utf8_literal("React to this"), 0})
              .ok(),
          "reaction popout: fixture message is current in the active channel");
    check(application_media_picker_open(UiMediaPickerKind::emoji) && query(utf8_literal("smile")),
          "reaction popout: composer Emoji tab keeps its own query");
    const auto composer_categories = [&]() noexcept {
        query({});
        return categories(p.scene);
    }();
    query(utf8_literal("smile"));
    application_media_picker_close();
    const UiRect add_reaction{900, 300, 928, 328};
    const u32 popup_opens = g_app.popup_scene.opens;
    const u32 starts_before = animation_starts;
    observe_motion(5000);
    check(application_media_picker_open_reaction(9100, add_reaction) && p.active && p.scene.active &&
              p.reaction_message_id == 9100 && p.kind == UiMediaPickerKind::emoji && p.focused &&
              !p.edit.bytes && p.emoji.preference_kind == ReactionPreferenceKind::reaction &&
              p.scene.variant == UiMediaPickerVariant::reaction &&
              p.content.available_tabs == ui_media_picker_tab_bit(UiMediaPickerKind::emoji) &&
              !p.scene.tab_widgets[0].generation && !p.scene.tab_widgets[1].generation &&
              !p.scene.tab_widgets[2].generation && same(p.content.placeholder, utf8_literal("Find the perfect reaction")) &&
              ui_rect_width(p.scene.panel) == kUiMediaPickerReactionWidth &&
              ui_rect_height(p.scene.panel) == kUiMediaPickerReactionHeight &&
              p.scene.panel.right == add_reaction.left - 8 && p.scene.field.top < p.scene.panel.top + 16 &&
              !g_app.popup_scene.active,
          "reaction popout: Add Reaction opens the shared Emoji component left of its button without the "
          "tab selector, focused on its own search");
    const auto reaction_categories = categories(p.scene);
    check(same_categories(composer_categories, reaction_categories),
          "reaction popout: both variants list the same categories, each drawn as an emoji glyph rather "
          "than an icon");
    observe_motion(5016);
    const u32 opened_starts = animation_starts;
    bool scrolled = true;
    for (u32 i = 0; i < 24; ++i) {
        scrolled = scrolled && ui_media_picker_scene_scroll(&p.scene, i & 1 ? -40.f : 90.f) &&
                   application_media_picker_synchronize();
        observe_motion(5032 + i * 16);
    }
    bool wheel = true;
    for (u32 i = 0; ui_media_picker_scene_scrollbar(p.scene, true).visible && i < 6; ++i) {
        bool changed = false;
        wheel = wheel && rail_scroll_write(&p, p.scope, p.scene.rail_scroll_dips + 20, &changed) &&
                application_media_picker_synchronize();
        observe_motion(5500 + i * 16);
    }
    check(scrolled && wheel && opened_starts == starts_before + 1 && animation_starts == opened_starts &&
              g_app.popup_scene.opens == popup_opens && p.scene.active,
          "reaction popout: the open animation starts once; body and rail scrolling never replay it");
    const auto search_before = p.source.count;
    check(application_media_picker_input(UiInputKind::character, 't', false) &&
              application_media_picker_input(UiInputKind::character, 'h', false) && p.source.count &&
              p.source.count < search_before && p.scene.scroll_dips == 0 && p.section_count == 1,
          "reaction popout: typing filters through the shared search field");
    query({});
    reaction_calls = 0;
    reaction_next = ApplicationMessageReactionEnqueueResult::accepted;
    reaction_status = {};
    SetKeyboardState(shift_keyboard);
    const bool kept = activate(p, target(0)) && activate(p, target(1));
    SetKeyboardState(react_keyboard);
    check(kept && p.active && reaction_calls == 2 && reaction_messages[0] == 9100 &&
              reaction_messages[1] == 9100 && !utf8_equal(reaction_names[0], reaction_names[1]) &&
              same(p.status, utf8_literal("Shift-click to add more reactions")) && !p.reaction_error,
          "reaction popout: two Shift activations add distinct reactions and keep the popout open");
    const ApplicationMessageReactionEnqueueResult refusals[]{ApplicationMessageReactionEnqueueResult::capacity,
                                                            ApplicationMessageReactionEnqueueResult::permission,
                                                            ApplicationMessageReactionEnqueueResult::storage};
    bool refused = true;
    for (const auto refusal : refusals) {
        reaction_next = refusal;
        refused = refused && activate(p, target(0)) && p.active && p.reaction_error &&
                  same(p.status, application_message_reaction_admission_text(refusal)) &&
                  application_media_picker_synchronize() && same(p.content.status, p.status);
    }
    check(refused && reaction_calls == 5,
          "reaction popout: an owner refusal keeps the popout open with its reason in the footer");
    ++g_app.plugin_account_scope_serial;
    const bool stale = !owns_current(p) && activate(p, target(0)) && reaction_calls == 5 &&
                       application_media_picker_synchronize() && !p.active;
    --g_app.plugin_account_scope_serial;
    check(stale && !p.reaction_message_id && p.kind == UiMediaPickerKind::emoji &&
              same(message_composer_text(p.edit), utf8_literal("smile")),
          "reaction popout: a retired account scope cannot react and closes back to the composer picker state");
    reaction_next = ApplicationMessageReactionEnqueueResult::accepted;
    check(application_media_picker_open_reaction(9100, add_reaction) && p.active &&
              application_media_picker_open_reaction(9100, add_reaction) && !p.active,
          "reaction popout: Add Reaction on the open popout's message toggles it shut");
    observe_motion(6000);
    const u32 closed_starts = animation_starts;
    check(application_media_picker_open_reaction(9100, add_reaction) && (observe_motion(6016), true) &&
              animation_starts == closed_starts + 1 && activate(p, target(0)) && !p.active &&
              reaction_calls == 6 && !p.reaction_message_id,
          "reaction popout: a reopened popout animates in once and an ordinary pick closes it");
    check(application_media_picker_open(UiMediaPickerKind::emoji) && p.active && !p.reaction_message_id &&
              p.scene.variant == UiMediaPickerVariant::expression &&
              same(message_composer_text(p.edit), utf8_literal("smile")) &&
              same(p.content.placeholder, utf8_literal("Find the perfect emoji")),
          "reaction popout: the composer Emoji tab returns with its own query and selector");
    application_media_picker_close();
    release();
    owner_measurements("picker_released");
    check(!g_app.media_picker && !g_app.renderer.media_picker_scene &&
              !application_media_picker_next_wake() && !application_media_picker_caret_wake(),
          "picker runtime: release removes owner, scene and all deadlines");
}
// Optional offline replay of a public catalog response, without retaining a
// network-dependent fixture in the normal suite or sending any message.
bool replay_catalog_fixture() noexcept {
    wchar_t path[4096]{};
    const DWORD path_bytes = GetEnvironmentVariableW(L"MAICORD_STICKER_CATALOG_FIXTURE", path, 4096);
    if (!path_bytes) return false;
    HANDLE file = path_bytes < 4096 ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    LARGE_INTEGER size{};
    const bool valid = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart <= kMediaCatalogMaximumStickerResponseBytes;
    auto* body = valid ? static_cast<char*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    DWORD bytes = 0;
    const bool read = body && ReadFile(file, body, static_cast<DWORD>(size.QuadPart), &bytes, nullptr) &&
        bytes == size.QuadPart;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    check(read && initialize(), "picker replay: bounded public fixture and hidden host load");
    if (read && g_app.account_ui) {
        check(application_media_picker_open(UiMediaPickerKind::sticker) && send_pending(901) &&
            response(901, utf8_literal("[]")) && send_pending(902) &&
            response(902, {body, bytes}), "picker replay: guild then public catalog responses apply");
        auto* p = g_app.media_picker;
        check(p && p->packs && p->packs->count && p->phase == UiMediaPickerPhase::ready &&
            p->source.count == p->packs->count && p->section_count,
            "picker replay: complete public bank and pack sections are ready");
        if (p && p->packs) {
            measurement("public_catalog_response_bytes", bytes);
            measurement("public_catalog_stickers", p->packs->count);
            measurement("public_catalog_sections", p->section_count);
            bool previews=true;
            for(u32 i=0;i<p->packs->count;++i){UiMediaPickerItem item{};
                previews=previews && preview_admitted(*p->packs,i) && resolve(p,i,&item) && item.enabled;}
            check(previews,"picker replay: every available public sticker has an enabled typed preview");
            for (u32 step = 0; step < 1000; ++step) {
                const auto bar = ui_media_picker_scene_scrollbar(p->scene);
                if (p->scene.scroll_dips >= bar.maximum_scroll) break;
                const bool scrolled = ui_media_picker_scene_scroll(&p->scene, 20);
                if (!scrolled || !p->scene.active) {
                    measurement("public_catalog_scroll_failed_step", step);
                    measurement("public_catalog_scroll_failed_offset", static_cast<usize>(p->scene.scroll_dips));
                    check(false, "picker replay: full catalog scroll keeps scene active");
                    break;
                }
            }
            ui_media_picker_scene_scroll(&p->scene, -p->scene.scroll_dips);
            UiScrollHost host{};
            u64 now = 1000000;
            bool wheel_ok = true;
            for (u32 burst = 0; wheel_ok && burst < 50; ++burst) {
                const UiPoint point{(p->scene.body.left + p->scene.body.right) * .5f,
                    (p->scene.body.top + p->scene.body.bottom) * .5f};
                wheel_ok = ui_scroll_host_wheel(&host, {p, p->scope, body_scroll_read, body_scroll_write},
                    point, -120, 3, false, now, .8) != UiScrollDispatch::failed;
                for (u32 frame = 0; wheel_ok && frame < 16; ++frame) {
                    now += 8333;
                    bool changed = false;
                    wheel_ok = ui_scroll_host_tick(&host, now, &changed) &&
                        p->scene.active && application_media_picker_synchronize() && p->scene.active;
                    if (!wheel_ok) {
                        measurement("public_catalog_wheel_failed_burst", burst);
                        measurement("public_catalog_wheel_failed_frame", frame);
                    }
                }
            }
            check(wheel_ok, "picker replay: smooth wheel frames keep the catalog open");
            u32 random = 0x9827491;
            for (u32 trial = 0; trial < 2000 && p->scene.active; ++trial) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                const float extent = ui_media_picker_scene_scrollbar(p->scene).maximum_scroll;
                const float offset = extent * static_cast<float>(random & 0xffff) / 65535.f;
                ui_media_picker_scene_scroll(&p->scene, offset - p->scene.scroll_dips);
                if (!p->scene.active) measurement("fractional_scroll_failure", trial);
            }
            check(p->scene.active, "picker replay: arbitrary fractional scroll offsets retain the pane");
            ui_media_picker_scene_close(&p->scene);
            check(application_media_picker_open(UiMediaPickerKind::sticker) && p->active && p->scene.active,
                "picker replay: launcher immediately recovers a retired projection on the same click");
            bool preserved = false;
            for (u32 i = 0; i < p->packs->count; ++i)
                if (p->packs->items[i].sticker_id == 749046696482439188ull)
                    for (u32 b = 0; b < p->packs->items[i].tags.bytes; ++b)
                        preserved |= p->packs->items[i].tags.data[b] == '\t';
            check(preserved && query(utf8_literal("huh")) && p->source.count,
                "picker replay: official Think tags retain tab and local search succeeds");
        }
    }
    release();
    if (body) VirtualFree(body, 0, MEM_RELEASE);
    return true;
}
} // namespace maicord::media_picker_runtime_test

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++media_picker_runtime_test::text_builds;
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position)
        return false;
    *position = 0;
    return true;
}
namespace app_detail {
void reset_message_composer_caret_timing(bool) noexcept {}
bool stop_local_typing(u64) noexcept { return true; }
bool application_thread_header_activate_target(UiThreadHeader*, const UiThreadHeaderTarget&) noexcept { return false; }
float client_dip_x(LPARAM point) noexcept {
    return static_cast<short>(LOWORD(point)) * (96.f / maximum<u32>(g_app.renderer.dpi, 1));
}
float client_dip_y(LPARAM point) noexcept {
    return static_cast<short>(HIWORD(point)) * (96.f / maximum<u32>(g_app.renderer.dpi, 1));
}
bool handle_shell_click(LPARAM point) noexcept {
    const auto handle =
        ui_widget_manager_hit_test(g_app.shell_scene.widgets, client_dip_x(point), client_dip_y(point));
    const auto* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, handle);
    if (!widget || widget->id != static_cast<u32>(UiSemantic::member_toggle) + 1u)
        return false;
    ++media_picker_runtime_test::member_activations;
    g_app.member_pane_visible = !g_app.member_pane_visible;
    return true;
}
bool handle_composer_click(LPARAM point) noexcept {
    const auto control = message_composer_scene_hit_test(g_app.message_composer_scene, client_dip_x(point),
                                                         client_dip_y(point));
    UiMediaPickerKind kind{};
    if (control == MessageComposerControl::gif)
        kind = UiMediaPickerKind::gif;
    else if (control == MessageComposerControl::sticker)
        kind = UiMediaPickerKind::sticker;
    else if (control == MessageComposerControl::emoji)
        kind = UiMediaPickerKind::emoji;
    else
        return false;
    ++media_picker_runtime_test::launcher_activations;
    return application_media_picker_open(kind);
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
ApplicationMessageReactionEnqueueResult application_queue_message_reaction_emoji(u64 message_id, u64 emoji_id,
                                                                             Utf8View name, bool) noexcept {
    using namespace media_picker_runtime_test;
    if (reaction_calls < 4) {
        reaction_messages[reaction_calls] = message_id;
        reaction_emoji[reaction_calls] = emoji_id;
        reaction_names[reaction_calls] = name;
    }
    ++reaction_calls;
    return reaction_next;
}
Utf8View application_message_reaction_status(u64, bool* error) noexcept {
    if (error)
        *error = false;
    return media_picker_runtime_test::reaction_status;
}
Utf8View application_message_reaction_admission_text(ApplicationMessageReactionEnqueueResult admission) noexcept {
    switch (admission) {
    case ApplicationMessageReactionEnqueueResult::capacity: return utf8_literal("Too many pending reactions.");
    case ApplicationMessageReactionEnqueueResult::permission: return utf8_literal("You cannot add reactions here.");
    case ApplicationMessageReactionEnqueueResult::storage: return utf8_literal("Unable to save this reaction.");
    default: return {};
    }
}
bool application_emoji_preferences_toggle_favorite(ReactionEmojiRef emoji) noexcept {
    return reaction_preferences_toggle_favorite_emoji(&g_app.account_ui->reaction_preferences, emoji).ok();
}
void application_emoji_preferences_open() noexcept {
}
bool application_gif_favorites_open() noexcept {
    media_picker_runtime_test::favorites_visible = media_picker_runtime_test::favorites_enabled;
    return media_picker_runtime_test::favorites_enabled;
}
void application_gif_favorites_set_visible(bool value) noexcept {
    media_picker_runtime_test::favorites_visible = value;
}
ApplicationGifFavoritesView application_gif_favorites_view() noexcept {
    return media_picker_runtime_test::favorites;
}
ApplicationGifFavoriteState application_gif_favorites_state(Utf8View) noexcept {
    return media_picker_runtime_test::favorite_state;
}
bool application_gif_favorites_toggle(Utf8View url, Utf8View src, u32, u32, GifFavoriteFormat format) noexcept {
    ++media_picker_runtime_test::favorite_toggles;
    media_picker_runtime_test::toggled_url = url;
    media_picker_runtime_test::toggled_src = src;
    media_picker_runtime_test::toggled_format = format;
    return true;
}
bool refresh_account_text(bool) noexcept {
    ++media_picker_runtime_test::repaints;
    return true;
}
bool refresh_message_composer_ui() noexcept {
    ++media_picker_runtime_test::composer_refreshes;
    return true;
}
bool composer_apply_result(MessageComposerResult result, bool) noexcept {
    if (result != MessageComposerResult::changed)
        return result == MessageComposerResult::unchanged || result == MessageComposerResult::capacity;
    return g_app.account_ui &&
           message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller).ok() &&
           refresh_message_composer_ui();
}
bool pump_message_history() noexcept {
    ++media_picker_runtime_test::history_pumps;
    return true;
}
bool application_settings_active() noexcept {
    return media_picker_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
bool focus_message_composer_control(MessageComposerControl) noexcept {
    return true;
}
void application_composer_text_input_suspend() noexcept {
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_synchronize() noexcept {
    return true;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_media_picker_runtime_entry() noexcept {
    if (!maicord::media_picker_runtime_test::replay_catalog_fixture())
        maicord::media_picker_runtime_test::run();
    const bool failed = maicord::media_picker_runtime_test::failures != 0;
    constexpr char pass[] = "PASS application media picker runtime\n";
    constexpr char fail[] = "FAIL application media picker runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}

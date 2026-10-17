// Shipping Save host with owned authoritative models and deterministic native
// dialog/transport boundaries. No chooser, network or filesystem write occurs.
#include "../src/app/application_downloads.cpp"
#include "test_output.hpp"

namespace maicord::downloads_runtime_test {
using namespace app_detail;
u32 failures, picker_starts, picker_cancels, starts, cancels, suspends;
bool picker_ready, picker_reap = true, transfer_reap = true, chosen_executable;
HttpsFileDownloadError start_error{};
bool retain_failed_start;
NativeFilePickerResult picker_result{};
HttpsFileDownloadResult transfer_result{};
u64 transfer_id, submitted_origin, submitted_attachment, submitted_bytes;
bool submitted_overwrite;
char submitted_url[2048];
wchar_t submitted_path[256];
constexpr wchar_t chosen_path[] = L"D:\\MaiCord-build\\download-runtime.bin";
constexpr wchar_t chosen_script[] = L"D:\\MaiCord-build\\download-runtime.cmd";
GuildRecord guilds[2];
u16 guild_order[2];
ChannelRecord channels[4];
u16 channel_order[4];
u8 names[512];
MessageRecord records[32];
u32 order[32];
u8 text[1024];
MessageRichRecord rich_records[32];
u16 rich_order[32];
u8 rich_text[16384];
MessageStore search_store;
MessageRichStore search_rich;
MessageRecord search_records[4];
u32 search_order[4];
u8 search_text[1024];
MessageRichRecord search_rich_records[8];
u16 search_rich_order[4];
u8 search_rich_text[4096];
u32 search_publication = 100, search_closes;
bool search_panel;
u64 search_channel_generation;
MessageTimeline search_timeline{};
UiVariableList search_list{};
u32 search_heights[4]{};
u64 search_fenwick[4]{};
UiSearchSource search_source{};
UiSearchContent search_content{};
UiSearchScene search_scene{};
u32 confirmations, media_refreshes;
MessageExplicitFilter media_filter = MessageExplicitFilter::show;
bool media_reveal_allowed = true;
MessageMediaOwner queried_owner{};
char draft[256];
UiRect fixture_viewport{0, 0, 1200, 900};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
MessageMediaPolicyResult query_media(const void*, const MessageMediaOwner& owner) noexcept {
    queried_owner = owner;
    return {media_filter, media_reveal_allowed};
}
bool publish_search() noexcept {
    if (!ui_variable_list_initialize(&search_list, {search_heights, 4}, {search_fenwick, 4},
                                     search_store.count, ui_extent_units(57)) ||
        !message_timeline_initialize(&search_timeline, &search_store, &search_list, ui_extent_units(57),
                                     ui_extent_units(900), false) ||
        !message_timeline_bind_rich_store(&search_timeline, &search_rich))
        return false;
    search_source.count = search_store.count;
    search_source.revision = search_publication;
    search_source.rich_timeline = &search_timeline;
    search_source.current = +[](void*) noexcept {
        return search_panel && search_source.revision == search_publication;
    };
    search_source.resolve = +[](void*, u32 index, UiSearchRow* row) noexcept {
        if (index || !row)
            return false;
        *row = {101, 800, utf8_literal("Fixture author"), utf8_literal("#fixture"), {}, {}, true, true};
        return true;
    };
    search_content.edit.active = search_content.panel = true;
    search_content.phase = UiSearchPhase::ready;
    search_content.revision = search_publication;
    ui_search_scene_set_media_policy(&search_scene, {nullptr, query_media, search_publication});
    return ui_search_scene_update(&search_scene, &search_source, &search_content, fixture_viewport,
                                  {0, 0, 1200, 48});
}
bool seed_into(MessageStore& source, MessageRichStore& rich, u64 id, u64 attachment, Utf8View filename,
               bool snapshot) noexcept {
    const auto inserted = message_store_upsert(&source, {id, 101, 3, 1, 0, {}, 0});
    const auto* record = inserted.ok() ? message_store_resolve(source, inserted.handle) : nullptr;
    if (!record)
        return false;
    GatewayMessageAttachment a{};
    a.id = attachment;
    a.size = 7;
    a.filename = filename;
    char url[256]{};
    ProtocolWriter writer{{reinterpret_cast<u8*>(url), sizeof(url)}, 0};
    protocol_writer_append_literal(&writer, "https://cdn.discordapp.com/attachments/");
    protocol_writer_append_u64(&writer, snapshot ? 777 : 101);
    protocol_writer_append_literal(&writer, "/");
    protocol_writer_append_u64(&writer, attachment);
    protocol_writer_append_literal(&writer,
                                   "/original.bin?ex=ffffffff&is=1&hm="
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    a.url = {url, writer.used};
    GatewayMessageEvent event{};
    event.id = id;
    if (snapshot) {
        event.fields = gateway_message_field_message_reference | gateway_message_field_message_snapshots;
        event.reference.type = 1;
        event.reference.channel_id = 777;
        event.reference.message_id = 888;
        event.snapshot.present = true;
        event.snapshot.fields = gateway_message_field_attachments;
        event.snapshot.attachments = {&a, 1};
    } else {
        event.fields = gateway_message_field_attachments;
        event.attachments = {&a, 1};
    }
    return message_rich_store_apply(&rich, id, record->revision, event).ok();
}
// A Tenor-style GIFV embed whose video and poster the media proxy serves
// (without proxy URLs, only the third-party host knows them).
bool seed_embed(u64 id, bool proxied) noexcept {
    const auto inserted = message_store_upsert(&g_app.message_view.store, {id, 101, 3, 1, 0, {}, 0});
    const auto* record =
        inserted.ok() ? message_store_resolve(g_app.message_view.store, inserted.handle) : nullptr;
    if (!record)
        return false;
    GatewayMessageEmbed gif{};
    gif.type = GatewayMessageEmbedType::gifv;
    gif.url = utf8_literal("https://tenor.com/view/wave");
    gif.thumbnail.url = utf8_literal("https://media.tenor.com/wave.png");
    gif.thumbnail.content_type = utf8_literal("image/png");
    gif.video.url = utf8_literal("https://media.tenor.com/wave.mp4");
    gif.video.content_type = utf8_literal("video/mp4");
    if (proxied) {
        gif.thumbnail.proxy_url =
            utf8_literal("https://images-ext-1.discordapp.net/external/AbC/https/media.tenor.com/wave.png");
        gif.video.proxy_url =
            utf8_literal("https://images-ext-1.discordapp.net/external/AbC/https/media.tenor.com/wave.mp4");
    }
    gif.thumbnail.width = gif.video.width = 498;
    gif.thumbnail.height = gif.video.height = 280;
    gif.thumbnail.has_width = gif.thumbnail.has_height = gif.video.has_width = gif.video.has_height = true;
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_embeds;
    event.embeds = {&gif, 1};
    return message_rich_store_apply(&g_app.account_ui->message_rich_store, id, record->revision, event)
        .ok();
}
bool seed(u64 id, u64 attachment, Utf8View filename, bool snapshot = false) noexcept {
    return seed_into(g_app.message_view.store, g_app.account_ui->message_rich_store, id, attachment,
                     filename, snapshot);
}

void audio_hit_tests() noexcept {
    static u32 heights[16]{};
    static u64 sums[16]{};
    static UiWidget widgets[16 * kMessageSceneWidgetsPerRow + 1]{};
    static UiLayoutNode nodes[16 * kMessageSceneWidgetsPerRow + 1]{};
    MessageTimeline timeline{};
    UiVariableList list{};
    auto& scene = g_app.message_scene;
    const bool ready = seed(503, 603, utf8_literal("voice.ogg")) &&
                       seed(504, 604, utf8_literal("forward.opus"), true) &&
                       ui_variable_list_initialize(&list, {heights, 16}, {sums, 16},
                                                   g_app.message_view.store.count, ui_extent_units(57)) &&
                       message_timeline_initialize(&timeline, &g_app.message_view.store, &list,
                                                   ui_extent_units(57), ui_extent_units(900), false) &&
                       message_timeline_bind_rich_store(&timeline, &g_app.account_ui->message_rich_store) &&
                       message_scene_initialize_owned(&scene, &timeline, 8, 8,
                                                      {widgets, 16 * kMessageSceneWidgetsPerRow + 1},
                                                      {nodes, 16 * kMessageSceneWidgetsPerRow + 1}) &&
                       message_scene_update(&scene, fixture_viewport).ok();
    check(ready,
          "Downloads audio hit fixture uses actual owning rich models and responsive message layout");
    if (!ready) {
        message_scene_release(&scene);
        return;
    }
    const auto starts_before = starts, pickers_before = picker_starts;
    for (u32 index = 0; index < 2; ++index) {
        const u64 message_id = 503 + index, attachment_id = 603 + index;
        const auto found = message_store_find(g_app.message_view.store, message_id);
        const auto* outer = found.ok() ? message_scene_find_row(scene, found.order_index) : nullptr;
        const auto* row = outer && index ? message_scene_snapshot_row(scene, *outer) : outer;
        const auto* rich =
            index ? message_rich_store_snapshot(g_app.account_ui->message_rich_store, message_id)
                  : message_rich_store_find(g_app.account_ui->message_rich_store, message_id);
        const auto widget =
            row ? message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, 0)
                : UiWidgetHandle{};
        const auto* node =
            row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::attachment, 0)
                : nullptr;
        const auto* row_node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
        UiAudioCardGeometry card{};
        const bool geometry = rich && node && row_node &&
                              message_rich_attachment_audio_supported(g_app.account_ui->message_rich_store,
                                                                      rich->attachments[0]) &&
                              ui_audio_card_geometry(node->content_rectangle, kDefaultUiAudioControlsStyle,
                                                     &card, message_rich_record_voice(*rich));
        check(geometry, "Downloads ordinary and forwarded audio cards resolve actual card geometry");
        if (!geometry)
            continue;
        const float title_x = (card.title.left + card.title.right) * .5f,
                    title_y = (card.title.top + card.title.bottom) * .5f;
        const float download_x = (card.download.left + card.download.right) * .5f,
                    download_y = (card.download.top + card.download.bottom) * .5f;
        DownloadSource source{};
        UiRect bounds{};
        UiWidgetHandle resolved{};
        check(!attachment_at(title_x, title_y, true, &source) &&
                  !attachment_at(download_x, download_y, true, &source),
              "Downloads audio title and inline footer never enter legacy image Save hit target");
        const auto expected = ui_rect_intersection(
            card.bounds, ui_rect_intersection(node->clip_rectangle,
                                              ui_rect_intersection(row_node->rectangle, scene.viewport)));
        check(attachment_at(title_x, title_y, false, &source, {}, &bounds, &resolved) &&
                  source.message == message_id && source.attachment == attachment_id &&
                  source.snapshot == (index != 0) && source.origin_channel == (index ? 777 : 101) &&
                  resolved.slot == widget.slot && resolved.generation == widget.generation &&
                  bounds.left == expected.left && bounds.top == expected.top &&
                  bounds.right == expected.right && bounds.bottom == expected.bottom &&
                  attachment_at(download_x, download_y, false, &source),
              "Downloads audio context covers full clipped card and preserves exact forwarded source "
              "identity");
        auto stale = widget;
        ++stale.generation;
        check(!attachment_at(0, 0, false, &source, stale),
              "Downloads stale audio widget cannot resolve focused context");
        check(ui_widget_manager_block_input(&scene.widgets, widget, true) &&
                  !attachment_at(title_x, title_y, false, &source) &&
                  !attachment_at(0, 0, false, &source, widget),
              "Downloads disabled audio widget rejects pointer and focused context capture");
        ui_widget_manager_block_input(&scene.widgets, widget, false);
        auto* mutable_node = const_cast<UiLayoutNode*>(node);
        const auto old_clip = mutable_node->clip_rectangle;
        mutable_node->clip_rectangle.bottom = minimum(old_clip.bottom, card.title.bottom);
        check(!attachment_at(download_x, download_y, false, &source) &&
                  attachment_at(title_x, title_y, false, &source, {}, &bounds) &&
                  bounds.bottom == mutable_node->clip_rectangle.bottom,
              "Downloads audio footer outside exact layout clip is not a context target");
        mutable_node->clip_rectangle = old_clip;
        auto* mutable_rich = const_cast<MessageRichRecord*>(rich);
        const u32 revision = mutable_rich->message_revision;
        ++mutable_rich->message_revision;
        check(!attachment_at(title_x, title_y, false, &source),
              "Downloads stale audio rich revision cannot publish context source");
        mutable_rich->message_revision = revision;
    }
    check(starts == starts_before && picker_starts == pickers_before,
          "Downloads audio hit checks do not start transfer or chooser work");
    message_scene_release(&scene);
}
// ---- Attachment context menu and cursor ownership ---------------------------
// The message menu host is a boundary here: it records the media rows the
// downloads owner merges and publishes the controller identity it validates.
PopupItem menu_rows[8];
u32 menu_row_count, menu_open_calls;
// What plugin rows would have received: attachment target, index and filename.
u32 plugin_target, plugin_index;
char plugin_text[64];
u32 plugin_text_bytes;
u64 menu_message;
bool menu_refuse;
u32 opened_links;
u64 opened_message, opened_attachment, opened_origin;
u32 rgba_reads;
MediaAssetKey resident_key{};
struct CapturedClipboard {
    u32 opens, publishes;
    UINT formats[4];
    u32 sizes[4];
    char text[512];
};
CapturedClipboard clipboard{};
bool clipboard_open(void*, HWND) noexcept {
    ++clipboard.opens;
    clipboard.publishes = 0;
    return true;
}
bool clipboard_publish(void*, UINT format, HGLOBAL data) noexcept {
    if (clipboard.publishes >= 4)
        return false;
    const u32 slot = clipboard.publishes++;
    clipboard.formats[slot] = format;
    clipboard.sizes[slot] = static_cast<u32>(GlobalSize(data));
    if (format == CF_UNICODETEXT) {
        const auto* wide = static_cast<const wchar_t*>(GlobalLock(data));
        const int bytes = wide ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, clipboard.text,
                                                     sizeof(clipboard.text), nullptr, nullptr)
                               : 0;
        if (wide)
            GlobalUnlock(data);
        if (bytes <= 0)
            clipboard.text[0] = 0;
    }
    GlobalFree(data);
    return true;
}
void clipboard_close(void*) noexcept {
}
bool seed_media(u64 id, u64 attachment, Utf8View filename, Utf8View content_type, u32 width = 0,
                u32 height = 0) noexcept {
    const auto inserted = message_store_upsert(&g_app.message_view.store, {id, 101, 3, 1, 0, {}, 0});
    const auto* record =
        inserted.ok() ? message_store_resolve(g_app.message_view.store, inserted.handle) : nullptr;
    if (!record)
        return false;
    GatewayMessageAttachment a{};
    a.id = attachment;
    a.size = 7;
    a.filename = filename;
    a.content_type = content_type;
    a.width = width;
    a.height = height;
    a.has_width = width != 0;
    a.has_height = height != 0;
    char url[256]{};
    ProtocolWriter writer{{reinterpret_cast<u8*>(url), sizeof(url)}, 0};
    protocol_writer_append_literal(&writer, "https://cdn.discordapp.com/attachments/101/");
    protocol_writer_append_u64(&writer, attachment);
    protocol_writer_append_literal(&writer, "/");
    protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(filename.data), filename.bytes);
    protocol_writer_append_literal(&writer,
                                   "?ex=ffffffff&is=1&hm="
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    a.url = {url, writer.used};
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_attachments;
    event.attachments = {&a, 1};
    return message_rich_store_apply(&g_app.account_ui->message_rich_store, id, record->revision, event)
        .ok();
}
bool labels_are(const char* const* expected, u32 count) noexcept {
    if (menu_row_count != count)
        return false;
    for (u32 index = 0; index < count; ++index) {
        u32 bytes = 0;
        while (expected[index][bytes])
            ++bytes;
        if (!utf8_equal(menu_rows[index].label, {expected[index], bytes}))
            return false;
    }
    return true;
}
u32 row_action(const char* label) noexcept {
    for (u32 index = 0; index < menu_row_count; ++index) {
        u32 bytes = 0;
        while (label[bytes])
            ++bytes;
        if (utf8_equal(menu_rows[index].label, {label, bytes}))
            return menu_rows[index].action;
    }
    return 0;
}
bool open_menu(u64 message, u64 attachment) noexcept {
    DownloadSource source{};
    menu_row_count = 0;
    return capture(message, attachment, false, &source) && open_context(source, {10, 10, 11, 11});
}

void attachment_menu_tests() noexcept {
    check(seed_media(520, 620, utf8_literal("cat.png"), utf8_literal("image/png")) &&
              seed_media(521, 621, utf8_literal("dance.gif"), utf8_literal("image/gif")) &&
              seed_media(522, 622, utf8_literal("clip.mp4"), utf8_literal("video/mp4")) &&
              seed_media(523, 623, utf8_literal("song.mp3"), utf8_literal("audio/mpeg")) &&
              seed_media(524, 624, utf8_literal("voice-message.ogg"), utf8_literal("audio/ogg")) &&
              seed_media(525, 625, utf8_literal("notes.txt"), utf8_literal("text/plain")),
          "Attachment menu fixture seeds one message per attachment kind");
    if (auto* rich = const_cast<MessageRichRecord*>(
            message_rich_store_find(g_app.account_ui->message_rich_store, 524)))
        rich->voice_message = true;
    const ClipboardSink previous =
        clipboard_sink_install({nullptr, clipboard_open, clipboard_publish, clipboard_close});

    const char* image[]{"Copy Image", "Save Image", "Copy Image Link", "Open Image Link"};
    const char* gif[]{"Save Image", "Copy Media Link", "Open Media Link"};
    const char* video[]{"Save Video", "Copy Media Link", "Open Media Link"};
    const char* file[]{"Save File", "Copy Link", "Open Link"};
    check(open_menu(520, 620) && labels_are(image, 4) && menu_message == 520 &&
              g_app.popup_owner_kind == PopupOwnerKind::message && g_app.downloads &&
              g_app.downloads->menu_valid && plugin_target == MC_MENU_ATTACHMENT && !plugin_index &&
              utf8_equal({plugin_text, plugin_text_bytes}, utf8_literal("cat.png")),
          "Attachment menu: an image merges Copy Image, Save Image and its image-link pair into the "
          "message menu");
    check(open_menu(521, 621) && labels_are(gif, 3) && open_menu(522, 622) && labels_are(video, 3) &&
              open_menu(523, 623) && labels_are(file, 3) && open_menu(525, 625) && labels_are(file, 3),
          "Attachment menu: gifs save as images with media links; videos, audio and files get Save and "
          "link rows");
    check(open_menu(524, 624) && labels_are(file, 3) &&
              g_app.downloads->menu_kind == ReceivedAttachmentKind::voice_message,
          "Attachment menu: a voice message offers Save File");

    // Copy Link and Open Link act on the exact captured attachment.
    check(open_menu(520, 620), "Attachment menu: reopen image menu for Copy Link");
    const auto& source = g_app.downloads->menu;
    const u32 opens_before = clipboard.opens;
    check(
        application_downloads_context_action(row_action("Copy Image Link")) && !g_app.popup_scene.active &&
            clipboard.opens == opens_before + 1 && clipboard.publishes == 1 &&
            clipboard.formats[0] == CF_UNICODETEXT &&
            utf8_equal({clipboard.text, static_cast<u32>(lstrlenA(clipboard.text))},
                       {source.url, source.url_bytes}),
        "Attachment menu: Copy Image Link publishes the signed CDN URL as text through the clipboard sink");
    check(!application_downloads_context_action(kReceivedAttachmentActionCopyLink) &&
              clipboard.opens == opens_before + 1,
          "Attachment menu: a consumed menu cannot act again");
    check(open_menu(522, 622) && application_downloads_context_action(row_action("Open Media Link")) &&
              opened_links == 1 && opened_message == 522 && opened_attachment == 622 && !opened_origin,
          "Attachment menu: Open Media Link routes through the refreshing external-link opener");

    // Copy Image reads the largest resident decoded tier.
    resident_key = media_asset_attachment(620, MediaAttachmentFormat::png, MediaResolutionTier::px512);
    check(open_menu(520, 620) && application_downloads_context_action(row_action("Copy Image")) &&
              rgba_reads >= 4 && clipboard.publishes == 3 && clipboard.formats[0] == CF_DIBV5 &&
              clipboard.formats[1] == RegisterClipboardFormatW(L"PNG") &&
              clipboard.formats[2] == RegisterClipboardFormatW(L"HTML Format"),
          "Attachment menu: Copy Image publishes CF_DIBV5, PNG and HTML Format from the resident tier");
    const u32 publishes = clipboard.opens;
    resident_key = {};
    check(open_menu(520, 620) && application_downloads_context_action(row_action("Copy Image")) &&
              clipboard.opens == publishes,
          "Attachment menu: Copy Image with no resident pixels leaves the clipboard alone");

    // Stale or foreign menus never act.
    check(open_menu(523, 623), "Attachment menu: reopen audio menu");
    ++g_app.message_context.opens;
    check(!application_downloads_context_action(row_action("Copy Link")),
          "Attachment menu: a later message menu invalidates the attachment rows");
    menu_refuse = true;
    check(!open_menu(525, 625) && !g_app.downloads->menu_valid,
          "Attachment menu: a refused message menu leaves no armed attachment source");
    menu_refuse = false;

    // Save keeps the existing chooser flow.
    const u32 pickers = picker_starts;
    check(open_menu(525, 625) && application_downloads_context_action(row_action("Save File")),
          "Attachment menu: Save File starts the download owner's flow");
    application_downloads_pump();
    check(picker_starts == pickers + 1, "Attachment menu: Save File reaches the native chooser");
    clipboard_sink_install(previous);
}

// Regression: the cursor once ran the Save button's press handler on every
// WM_SETCURSOR. It took SetCapture, and a capturing window gets no further
// WM_SETCURSOR, so the hand stayed until the next click.
void save_target_tests() noexcept {
    static u32 heights[32]{};
    static u64 sums[32]{};
    static UiWidget widgets[32 * kMessageSceneWidgetsPerRow + 1]{};
    static UiLayoutNode nodes[32 * kMessageSceneWidgetsPerRow + 1]{};
    MessageTimeline timeline{};
    UiVariableList list{};
    auto& scene = g_app.message_scene;
    // A tall viewport keeps every resident message on screen at once.
    const bool ready =
        seed_media(526, 626, utf8_literal("photo.jpg"), utf8_literal("image/jpeg"), 400, 300) &&
        ui_variable_list_initialize(&list, {heights, 32}, {sums, 32}, g_app.message_view.store.count,
                                    ui_extent_units(57)) &&
        message_timeline_initialize(&timeline, &g_app.message_view.store, &list, ui_extent_units(57),
                                    ui_extent_units(4000), false) &&
        message_timeline_bind_rich_store(&timeline, &g_app.account_ui->message_rich_store) &&
        message_scene_initialize_owned(&scene, &timeline, 32, 32,
                                       {widgets, 32 * kMessageSceneWidgetsPerRow + 1},
                                       {nodes, 32 * kMessageSceneWidgetsPerRow + 1}) &&
        message_scene_update(&scene, {0, 0, 1200, 4000}).ok();
    const auto found = message_store_find(g_app.message_view.store, 526);
    const auto* row = ready && found.ok() ? message_scene_find_row(scene, found.order_index) : nullptr;
    const auto* node =
        row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::attachment, 0)
            : nullptr;
    float width = 0, height = 0;
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, 526);
    const bool preview =
        node && rich &&
        message_rich_attachment_preview_extent(g_app.account_ui->message_rich_store, rich->attachments[0],
                                               ui_rect_width(node->content_rectangle), &width, &height);
    const auto save =
        preview ? ui_attachment_save_rectangle({node->rectangle.left, node->rectangle.top,
                                                node->rectangle.left + width, node->rectangle.top + height},
                                               false)
                : UiRect{};
    check(ui_rect_has_area(save) && ui_rect_contains(scene.viewport, save.left + 1, save.top + 1),
          "Downloads cursor fixture lays out an image with its inline Save button");
    if (!ui_rect_has_area(save)) {
        message_scene_release(&scene);
        return;
    }
    const float x = (save.left + save.right) * .5f, y = (save.top + save.bottom) * .5f;
    const auto focused = scene.widgets.focused, captured = scene.widgets.captured;
    const bool lazy = !g_app.downloads;
    check(application_downloads_save_target(x, y) && GetCapture() == nullptr &&
              scene.widgets.captured.slot == captured.slot &&
              scene.widgets.captured.generation == captured.generation &&
              scene.widgets.focused.slot == focused.slot &&
              scene.widgets.focused.generation == focused.generation && (!lazy || !g_app.downloads),
          "Downloads Save target query takes no capture, focus, widget capture or owner");
    check(!application_downloads_save_target(save.left - 40, y) &&
              !application_downloads_save_target(x, save.bottom + 40),
          "Downloads Save target releases the pointer once it leaves the button");
    // A scroll re-lays the content out under a still pointer.
    auto* moved = const_cast<UiLayoutNode*>(node);
    const auto laid_out = moved->rectangle;
    moved->rectangle.top += 200;
    moved->rectangle.bottom += 200;
    check(!application_downloads_save_target(x, y) && application_downloads_save_target(x, y + 200),
          "Downloads Save target follows content scrolled away under a still pointer");
    moved->rectangle = laid_out;
    check(application_downloads_save_press(x, y) && g_app.downloads && g_app.downloads->inline_capture,
          "Downloads Save press alone captures the pointer for its click");
    application_downloads_window_message(WM_LBUTTONUP, 0,
                                         MAKELPARAM(static_cast<i16>(-500), static_cast<i16>(-500)));
    check(GetCapture() == nullptr && g_app.downloads && !g_app.downloads->inline_capture,
          "Downloads Save press release outside the button returns the pointer");
    message_scene_release(&scene);
}
AccountDownloadResult WINAPI download(void*, AccountDownloadOperation op, u64 id,
                                      const AccountDownloadRequest* request) noexcept {
    if (op == AccountDownloadOperation::start) {
        ++starts;
        transfer_id = id;
        transfer_result = {};
        transfer_result.phase = HttpsFileDownloadPhase::receiving;
        if (request) {
            submitted_origin = request->source_channel_id;
            submitted_attachment = request->attachment_id;
            submitted_bytes = request->expected_bytes;
            submitted_overwrite = request->destination_exists && request->overwrite_authorized;
            copy_bytes(submitted_url, request->url.data, request->url.bytes);
            submitted_url[request->url.bytes] = 0;
            copy_bytes(submitted_path, request->path.data, request->path.count * sizeof(wchar_t));
            submitted_path[request->path.count] = 0;
        }
    } else if (op == AccountDownloadOperation::cancel)
        ++cancels;
    AccountDownloadResult result{};
    result.id = id;
    result.transfer = transfer_result;
    if (op == AccountDownloadOperation::start && start_error != HttpsFileDownloadError::none) {
        transfer_result.phase = HttpsFileDownloadPhase::complete;
        transfer_result.error = start_error;
        result.transfer = transfer_result;
        result.submit_error = start_error;
        result.released = !retain_failed_start;
        return result;
    }
    result.released = op == AccountDownloadOperation::release && transfer_reap;
    return result;
}
void finish(HttpsFileDownloadError error = HttpsFileDownloadError::none) noexcept {
    transfer_result.error = error;
    transfer_result.phase = HttpsFileDownloadPhase::complete;
    transfer_result.committed_bytes = transfer_result.file_bytes =
        error == HttpsFileDownloadError::none ? 7 : 0;
    application_downloads_pump();
}
void dismiss_all() noexcept {
    auto* q = g_app.downloads;
    if (!q)
        return;
    // Receipts are dismissed from the panel, which a Save no longer opens.
    if (!q->active)
        application_downloads_open();
    for (auto& j : q->jobs)
        if (j.occupied && terminal(j.phase))
            activate(*q, {UiDownloadAction::dismiss, j.id});
}
void media_admission_tests() noexcept {
    check(seed(500, 600, utf8_literal("original.bin")), "Downloads policy source seeds");
    auto* rich = const_cast<MessageRichRecord*>(
        message_rich_store_find(g_app.account_ui->message_rich_store, 500));
    if (!rich)
        return;
    MessageTimeline timeline{};
    timeline.store = &g_app.message_view.store;
    auto& scene = g_app.message_scene;
    scene.timeline = &timeline;
    scene.media_policy = {nullptr, query_media, 1};
    const u32 dialogs_before = picker_starts, transfers_before = starts,
              confirmations_before = confirmations, refreshes_before = media_refreshes;
    rich->attachments[0].flags |= kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::block;
    check(application_downloads_begin(500, 600) && !g_app.downloads &&
              picker_starts == dialogs_before && starts == transfers_before,
          "Downloads blocked Save is consumed before allocating a download owner or job");
    check(open_menu(500, 600) && application_downloads_context_action(row_action("Save File")) &&
              g_app.downloads && !g_app.downloads->jobs[0].occupied,
          "Downloads context Save obeys the same blocked media policy");
    auto* q = g_app.downloads;
    if (!q) {
        scene = {};
        return;
    }
    rich->attachments[0].flags = 0;
    check(application_downloads_begin(500, 600) && q->jobs[0].phase == JobPhase::queued &&
              q->jobs[0].occupied,
          "Downloads ordinary unflagged files remain saveable under explicit BLOCK");
    cancel_job(*q, 0);
    dismiss_all();
    scene.plugin_decorations = true;
    g_ui_plugin_render_query = +[](u64, u32, UiPluginRenderDecoration* output) noexcept {
        *output = {};
        output->media = ui_plugin_media_hide;
        return true;
    };
    check(application_downloads_begin(500, 600) && !q->jobs[0].occupied &&
              !scene.media_reveals.count && confirmations == confirmations_before,
          "Downloads plugin-hidden Save neither admits nor reveals nor asks for age confirmation");
    g_ui_plugin_render_query = nullptr;
    scene.plugin_decorations = false;
    rich->attachments[0].flags |= kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::show;
    picker_ready = false;
    application_downloads_begin(500, 600);
    media_filter = MessageExplicitFilter::blur;
    media_reveal_allowed = false;
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::failed && picker_starts == dialogs_before &&
              starts == transfers_before && confirmations == confirmations_before &&
              media_refreshes == refreshes_before && !scene.media_reveals.count,
          "Downloads queued policy change stops before chooser without asynchronous reveal or prompt");
    check(application_downloads_release(), "Downloads policy fixture releases its pending owner");
    rich->attachments[0].flags = 0;
    media_filter = MessageExplicitFilter::show;
    media_reveal_allowed = true;
    scene = {};
}
void run() noexcept {
    g_app.window = CreateWindowExW(0, L"STATIC", L"Save isolated fixture", WS_POPUP, 0, 0, 1200, 900,
                                   nullptr, nullptr, nullptr, nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui, "Downloads fixture creates hidden native owner");
    if (!g_app.window || !g_app.account_ui)
        return;
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.renderer.width = 1200;
    g_app.renderer.height = 900;
    g_app.account.instance = &starts;
    g_app.account.api.download = download;
    g_app.account.status.profile_id = 4;
    g_app.plugin_account_scope_serial = 7;
    auto& ui = *g_app.account_ui;
    check(
        chat_state_store_initialize(&ui.chat_state, {guilds, 2}, {guild_order, 2}, {channels, 4},
                                    {channel_order, 4}, {names, sizeof(names)}) &&
            chat_state_store_upsert_guild(&ui.chat_state,
                                          {10, utf8_literal("Fixture"), GuildMutationKind::create, 3})
                .ok() &&
            message_store_initialize(&g_app.message_view.store, {records, 32}, {order, 32},
                                     {text, sizeof(text)}) &&
            message_rich_store_initialize(&ui.message_rich_store, {rich_records, 32}, {rich_order, 32},
                                          {rich_text, sizeof(rich_text)}) &&
            message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}) &&
            message_composer_restore(&g_app.message_composer, 101, utf8_literal("Retained draft"), 14, 14),
        "Downloads fixture storage initializes");
    ChannelInput c{};
    c.id = 101;
    c.guild_id = 10;
    c.name = utf8_literal("source");
    check(chat_state_store_upsert_channel(&ui.chat_state, c).ok(), "Downloads source channel exists");
    auto* channel =
        const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, 101));
    if (!channel)
        return;
    channel->permissions_resolved = true;
    channel->actions = channel_action_view | channel_action_read_history;
    ui.chat_state.self_user_id = 3;
    ui.active_channel_id = 101;
    check(seed(500, 600, utf8_literal("original.bin")) && seed(501, 601, utf8_literal("run.cmd")) &&
              seed(502, 602, utf8_literal("forward.bin"), true),
          "Downloads retains ordinary and snapshot attachments");
    audio_hit_tests();
    check(!application_downloads_owner_bytes() && application_downloads_available(500, 600) &&
              !application_downloads_available(500, 999),
          "Downloads owner is lazy and exact attachment identity is required");
    check(application_downloads_begin(501, 601) && application_downloads_active() && suspends,
          "Downloads risky attachment opens native warning before chooser");
    auto* q = g_app.downloads;
    if (!q)
        return;
    check(q->content.mode == UiDownloadMode::confirmation &&
              q->scene.widgets.focused.slot == q->scene.cancel.slot && !picker_starts && !starts,
          "Downloads warning defaults to Cancel without URL or dialog work");
    application_downloads_input(UiInputKind::key_down, VK_RETURN, false);
    check(q->jobs[0].phase == JobPhase::cancelled && !starts && !picker_starts,
          "Downloads Enter on default warning cancels admission");
    application_downloads_input(UiInputKind::key_up, VK_RETURN, false);
    dismiss_all();
    check(application_downloads_begin(500, 600), "Downloads captures ordinary source");
    application_downloads_pump();
    check(picker_starts == 1 && !starts && q->picker_job != kNoDownload,
          "Downloads chooser precedes transfer admission");
    picker_ready = true;
    application_downloads_pump();
    check(starts == 1 && q->active_job != kNoDownload && submitted_origin == 101 &&
              submitted_attachment == 600 && submitted_bytes == 7 && submitted_overwrite,
          "Downloads transport receives original owner and exact chosen overwrite consent");
    check(bytes_equal(submitted_path, chosen_path, sizeof(chosen_path)) &&
              bytes_equal(g_app.message_composer.text, "Retained draft", 14),
          "Downloads path is owned and composer draft is retained");
    application_downloads_close();
    ui.active_channel_id = 999;
    transfer_result.committed_bytes = 3;
    application_downloads_pump();
    check(q->jobs[0].admitted && q->jobs[0].transfer.committed_bytes == 3 && !q->active,
          "Downloads admitted transfer survives hidden panel and navigation");
    finish();
    check(q->jobs[0].phase == JobPhase::saved && q->jobs[0].path && !q->jobs[0].admitted,
          "Downloads saved receipt retains path only after worker drain");
    ui.active_channel_id = 101;
    application_downloads_open();
    dismiss_all();
    check(application_downloads_begin(502, 602, true),
          "Downloads forwarded attachment begins from enclosing authoritative source");
    picker_ready = false;
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(starts == 2 && submitted_origin == 777 && submitted_attachment == 602,
          "Downloads snapshot transport preserves original attachment channel");
    transfer_reap = false;
    transfer_result.error = HttpsFileDownloadError::file_cleanup;
    transfer_result.phase = HttpsFileDownloadPhase::complete;
    transfer_result.cleanup_pending = true;
    application_downloads_pump();
    UiDownloadRow row{};
    check(resolve_row(q, 0, &row) && row.phase == UiDownloadPhase::failed && !row.retry && !row.dismiss &&
              q->jobs[0].admitted,
          "Downloads cleanup failure is visible while worker and staging ownership remain retained");
    transfer_reap = true;
    application_downloads_pump();
    dismiss_all();
    const auto previous_starts = picker_starts;
    application_downloads_begin(500, 600);
    ui.active_channel_id = 999;
    application_downloads_pump();
    check(picker_starts == previous_starts && q->jobs[0].phase == JobPhase::failed,
          "Downloads navigation retires queued source before chooser");
    ui.active_channel_id = 101;
    dismiss_all();
    chosen_executable = true;
    picker_ready = false;
    application_downloads_begin(500, 600);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::warning && q->jobs[0].path && q->jobs[0].after_choice_warning &&
              starts == 2,
          "Downloads risky chosen extension requires a second default Cancel warning before transport");
    activate(*q, {UiDownloadAction::cancel_confirmation, q->jobs[0].id});
    dismiss_all();
    chosen_executable = false;
    start_error = HttpsFileDownloadError::worker;
    retain_failed_start = false;
    picker_ready = false;
    application_downloads_begin(500, 600);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::failed && !q->jobs[0].admitted && q->active_job == kNoDownload,
          "Downloads synchronously released startup failure keeps only a retryable receipt");
    dismiss_all();
    retain_failed_start = true;
    transfer_reap = false;
    picker_ready = false;
    application_downloads_begin(500, 600);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(q->jobs[0].admitted && q->active_job == 0 && resolve_row(q, 0, &row) &&
              row.phase == UiDownloadPhase::failed && !row.retry && !row.dismiss,
          "Downloads retained failed-start worker cannot be replaced or dismissed before drain");
    transfer_reap = true;
    application_downloads_pump();
    check(!q->jobs[0].admitted && q->active_job == kNoDownload && q->jobs[0].phase == JobPhase::failed,
          "Downloads retained failed-start owner drains before releasing its slot");
    dismiss_all();
    start_error = HttpsFileDownloadError::none;
    retain_failed_start = false;
    picker_ready = false;
    application_downloads_begin(500, 600);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    transfer_result.status_code = 403;
    finish(HttpsFileDownloadError::http);
    const auto attempts_before_refresh = starts;
    activate(*q, {UiDownloadAction::retry, q->jobs[0].id});
    picker_ready = false;
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(q->jobs[0].force_refresh && q->jobs[0].resolution.pending && starts == attempts_before_refresh &&
              media_url_resolution_state(g_app.media_urls,
                                         {q->jobs[0].source.url, q->jobs[0].source.url_bytes}) ==
                  MediaUrlState::queued,
          "Downloads explicit403 retry refreshes the exact source before another transfer");
    for (auto& entry : g_app.media_urls.entries)
        if (entry.state == MediaUrlState::queued)
            entry.state = MediaUrlState::failed;
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::failed && starts == attempts_before_refresh,
          "Downloads failed explicit refresh never automatically admits a second transfer");
    dismiss_all();
    picker_ready = false;
    application_downloads_begin(502, 602, true);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    auto cancel_bounds = q->scene.primary_rectangles[0];
    const LPARAM cancel_point =
        MAKELPARAM(static_cast<i16>((cancel_bounds.left + cancel_bounds.right) * .5f),
                   static_cast<i16>((cancel_bounds.top + cancel_bounds.bottom) * .5f));
    application_downloads_window_message(WM_LBUTTONDOWN, MK_LBUTTON, cancel_point);
    const auto pressed_widget = q->scene.widgets.captured;
    transfer_result.committed_bytes = 3;
    application_downloads_pump();
    check(pressed_widget.generation && q->scene.widgets.captured.slot == pressed_widget.slot &&
              q->scene.widgets.captured.generation == pressed_widget.generation,
          "Downloads progress-only update retains actual native button capture");
    const auto prior_cancels = cancels;
    application_downloads_window_message(WM_LBUTTONUP, 0, cancel_point);
    check(cancels == prior_cancels + 1 && q->jobs[0].cancel_requested,
          "Downloads Cancel remains usable while byte progress advances");
    finish(HttpsFileDownloadError::cancelled);
    dismiss_all();
    picker_ready = false;
    application_downloads_begin(502, 602, true);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    cancel_bounds = q->scene.primary_rectangles[0];
    const LPARAM resized_point =
        MAKELPARAM(static_cast<i16>((cancel_bounds.left + cancel_bounds.right) * .5f),
                   static_cast<i16>((cancel_bounds.top + cancel_bounds.bottom) * .5f));
    application_downloads_window_message(WM_LBUTTONDOWN, MK_LBUTTON, resized_point);
    fixture_viewport.right = 1000;
    application_downloads_synchronize();
    fixture_viewport.right = 1200;
    application_downloads_synchronize();
    const auto cancels_before_resize = cancels;
    application_downloads_window_message(WM_LBUTTONUP, 0, resized_point);
    check(cancels == cancels_before_resize && !q->jobs[0].cancel_requested &&
              !q->scene.widgets.captured.generation,
          "Downloads resize retires capture even when the old button geometry returns");
    finish();
    dismiss_all();
    picker_ready = false;
    application_downloads_begin(502, 602, true);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    const LPARAM backdrop = MAKELPARAM(10, 10);
    application_downloads_window_message(WM_LBUTTONDOWN, MK_LBUTTON, backdrop);
    application_downloads_window_message(WM_LBUTTONUP, 0, backdrop);
    check(!q->active && q->jobs[0].admitted && !q->jobs[0].cancel_requested,
          "Downloads matching backdrop release hides the panel and retains admitted work");
    application_downloads_open();
    application_downloads_window_message(WM_LBUTTONDOWN, MK_LBUTTON, backdrop);
    fixture_viewport.right = 1000;
    application_downloads_synchronize();
    fixture_viewport.right = 1200;
    application_downloads_synchronize();
    application_downloads_window_message(WM_LBUTTONUP, 0, backdrop);
    check(q->active && q->jobs[0].admitted,
          "Downloads backdrop capture retires when layout moves and returns");
    TextFormScene external_form{};
    TextFormSurface external_surface{};
    external_form.active = true;
    external_surface.scene = &external_form;
    g_app.renderer.text_form = &external_surface;
    g_app.context_focus = &external_form.widgets;
    g_app.renderer.focus_owner = &external_form.widgets;
    application_downloads_synchronize();
    check(!q->active && q->jobs[0].admitted && !q->jobs[0].cancel_requested &&
              g_app.context_focus == &external_form.widgets &&
              g_app.renderer.focus_owner == &external_form.widgets,
          "Downloads yields to an actual foreground form without cancelling work or stealing its focus");
    g_app.renderer.text_form = nullptr;
    g_app.context_focus = nullptr;
    g_app.renderer.focus_owner = nullptr;
    application_downloads_open();
    finish();
    dismiss_all();
    // Saving goes straight to the save dialog, as in the official client; the
    // panel opens only for a confirmation or a failure.
    application_downloads_close();
    picker_ready = false;
    check(application_downloads_begin(500, 600) && !application_downloads_active(),
          "Downloads ordinary Save opens no panel");
    application_downloads_pump();
    check(q->picker_job != kNoDownload && !application_downloads_active(),
          "Downloads ordinary Save goes straight to the chooser");
    picker_ready = true;
    application_downloads_pump();
    const u32 first_save = q->active_job, transfers_before_second = starts;
    picker_ready = false;
    application_downloads_begin(502, 602, true);
    application_downloads_pump();
    check(first_save != kNoDownload && q->active_job == first_save && q->picker_job != kNoDownload &&
              q->picker_job != first_save && starts == transfers_before_second &&
              !application_downloads_active(),
          "Downloads a second Save asks for its location while the first transfer runs");
    finish(HttpsFileDownloadError::file_commit);
    check(q->jobs[first_save].phase == JobPhase::failed && application_downloads_active(),
          "Downloads a failed save opens the panel so its Retry is reachable");
    picker_ready = true;
    application_downloads_pump();
    check(starts == transfers_before_second + 1 && q->active_job != kNoDownload,
          "Downloads the waiting save starts its transfer once the first one drains");
    finish();
    dismiss_all();
    for (u32 i = 0; i < kDownloadJobs; ++i) {
        application_downloads_begin(501, 601);
        if (q->warning_id)
            activate(*q, {UiDownloadAction::cancel_confirmation, q->warning_id});
    }
    u32 finished = 0;
    for (const auto& j : q->jobs)
        finished += j.occupied && j.phase == JobPhase::cancelled;
    const u64 newest_before = q->jobs[kDownloadJobs - 1].id;
    check(finished == kDownloadJobs && application_downloads_begin(500, 600) &&
              q->jobs[0].source.message == 500 && q->jobs[0].phase == JobPhase::queued &&
              q->jobs[0].id > newest_before && q->jobs[1].phase == JobPhase::cancelled,
          "Downloads a Save with every slot finished reuses the oldest finished receipt");
    application_downloads_open();
    application_downloads_pump();
    application_downloads_pump();
    finish();
    check(q->jobs[0].phase == JobPhase::saved, "Downloads the reused receipt saves normally");
    dismiss_all();
    // Embed media saves through Discord's media proxy with a length the response gives.
    check(seed_embed(503, true) && seed_embed(504, false) &&
              application_downloads_embed_available(503, 0) &&
              !application_downloads_embed_available(503, 1) &&
              !application_downloads_embed_available(504, 0),
          "Downloads embed media is saveable only through Discord's media proxy");
    const u32 embed_transfers = starts;
    picker_ready = false;
    check(application_downloads_begin_embed(503, 0) && q->jobs[0].source.embed &&
              !q->jobs[0].source.attachment &&
              q->jobs[0].source.bytes == kHttpsFileDownloadSizeFromResponse &&
              utf8_equal({q->jobs[0].source.filename, q->jobs[0].source.filename_bytes},
                         utf8_literal("wave.mp4")),
          "Downloads embed Save names the file after its original media URL");
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    constexpr char proxied_video[] =
        "https://images-ext-1.discordapp.net/external/AbC/https/media.tenor.com/wave.mp4";
    check(starts == embed_transfers + 1 && !submitted_attachment && !submitted_origin &&
              submitted_bytes == kHttpsFileDownloadSizeFromResponse &&
              bytes_equal(submitted_url, proxied_video, sizeof(proxied_video)),
          "Downloads embed transfer fetches the proxied media without an attachment identity or link "
          "refresh");
    transfer_result.file_bytes = 9;
    transfer_result.committed_bytes = 4;
    application_downloads_pump();
    UiDownloadRow embed_row{};
    application_downloads_open();
    check(resolve_row(q, 0, &embed_row) && embed_row.total == 9 && embed_row.bytes == 4,
          "Downloads embed progress uses the length the response declared");
    finish();
    dismiss_all();
    DownloadSource embed_source{};
    menu_row_count = 0;
    check(capture_embed(503, 0, false, &embed_source) &&
              open_embed_context(embed_source, utf8_literal("https://media.tenor.com/wave.mp4"),
                                 {10, 10, 11, 11}) &&
              row_action("Save Image") && row_action("Copy Media Link") && !row_action("Copy Image"),
          "Downloads embed menu offers Save beside links named after the original media");
    const u32 links_before = opened_links;
    check(application_downloads_context_action(row_action("Save Image")) && q->jobs[0].occupied &&
              q->jobs[0].source.embed && opened_links == links_before,
          "Downloads embed menu Save queues the proxied media");
    cancel_job(*q, 0);
    dismiss_all();
    check(message_store_initialize(&search_store, {search_records, 4}, {search_order, 4},
                                   {search_text, sizeof(search_text)}) &&
              message_rich_store_initialize(&search_rich, {search_rich_records, 8}, {search_rich_order, 4},
                                            {search_rich_text, sizeof(search_rich_text)}) &&
              seed_into(search_store, search_rich, 800, 900, utf8_literal("search-forward.bin"), true),
          "Downloads Search fixture retains an independent accepted message and forwarded rich bank");
    search_channel_generation = channel->generation;
    search_panel = true;
    ApplicationSearchSourceTicket search_ticket{800, search_publication, 0};
    check(!message_store_find(g_app.message_view.store, 800).ok() &&
              !application_downloads_available(800, 900, true) &&
              application_downloads_search_available(search_ticket, 900, true),
          "Downloads Search attachment authority does not publish or borrow the live timeline");
    const u32 before_search_policy_dialogs = picker_starts, before_search_policy_starts = starts;
    check(application_downloads_begin_search(search_ticket, 900, true) && !q->jobs[0].occupied,
          "Downloads Search Save cannot fall back to the main scene without an accepted native scene");
    check(publish_search(), "Downloads Search policy binds the real accepted native rich scene");
    auto* native_media = ui_search_scene_message_scene(&search_scene);
    auto* policy_child = const_cast<MessageRichRecord*>(message_rich_store_snapshot(search_rich, 800));
    check(native_media && policy_child, "Downloads Search media policy fixture resolves its snapshot");
    if (!native_media || !policy_child)
        return;
    policy_child->attachments[0].flags |= kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::block;
    check(application_downloads_begin_search(search_ticket, 900, true) && !q->jobs[0].occupied &&
              !native_media->media_reveals.count && !confirmations,
          "Downloads blocked Search Save creates no job, reveal or confirmation");
    media_filter = MessageExplicitFilter::blur;
    media_reveal_allowed = false;
    check(application_downloads_begin_search(search_ticket, 900, true) && !q->jobs[0].occupied &&
              confirmations == 1 && !native_media->media_reveals.count && queried_owner.forwarded &&
              queried_owner.author_id == 0 && queried_owner.channel_id == 101 &&
              queried_owner.source_channel_id == 777 && queried_owner.source_message_id == 888,
          "Downloads age-gated Search Save uses outer forwarding ownership before native confirmation");
    media_reveal_allowed = true;
    const auto child_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0, true),
               outer_key = message_media_reveal_key(800, MessageMediaSource::attachment, 0);
    g_app.frame_pending = false;
    check(application_downloads_begin_search(search_ticket, 900, true) && !q->jobs[0].occupied &&
              message_media_revealed(native_media->media_reveals, child_key) &&
              !message_media_revealed(native_media->media_reveals, outer_key) &&
              g_app.frame_pending && !media_refreshes &&
              picker_starts == before_search_policy_dialogs && starts == before_search_policy_starts,
          "Downloads first Search Save only reveals its scoped snapshot and never opens a chooser");
    const u32 search_dialogs = picker_starts, search_transfers = starts,
              closed_before_overlay = search_closes;
    picker_ready = false;
    application_downloads_begin_search(search_ticket, 900, true);
    application_downloads_pump();
    check(search_panel && search_closes == closed_before_overlay && q->jobs[0].source.search_result &&
              q->picker_job == 0,
          "Downloads overlay retains the accepted Search source beneath the Save chooser");
    policy_child->attachments[0].flags = 0;
    media_filter = MessageExplicitFilter::show;
    const u32 before_picker_cancel = picker_cancels;
    ++search_publication;
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::failed && !q->jobs[0].admitted && starts == search_transfers &&
              picker_cancels == before_picker_cancel + 1 && q->picker_job == kNoDownload &&
              !application_downloads_search_available(search_ticket, 900, true),
          "Downloads replaced Search publication cancels and drains a showing chooser before URL or "
          "transfer admission");
    dismiss_all();
    search_ticket.publication_revision = search_publication;
    check(publish_search(), "Downloads replacement Search publication binds its native scene");
    picker_ready = false;
    application_downloads_begin_search(search_ticket, 900, true);
    application_downloads_pump();
    picker_ready = true;
    application_downloads_pump();
    check(q->jobs[0].admitted && submitted_origin == 777 && submitted_attachment == 900 &&
              submitted_bytes == 7 && starts == search_transfers + 1 && picker_starts == search_dialogs + 2,
          "Downloads Search snapshot Save preserves exact original CDN owner size and attachment identity");
    media_filter = MessageExplicitFilter::block;
    search_panel = false;
    ++search_publication;
    message_store_delete(&search_store, 800);
    transfer_result.committed_bytes = 4;
    application_downloads_pump();
    check(q->jobs[0].admitted && q->jobs[0].transfer.committed_bytes == 4,
          "Downloads admitted Search transfer survives page eviction and explicit source closure");
    finish();
    check(q->jobs[0].phase == JobPhase::saved && q->jobs[0].path && !q->jobs[0].admitted,
          "Downloads Search receipt retains only its owned path after independent transfer drain");
    dismiss_all();
    media_filter = MessageExplicitFilter::show;
    search_panel = true;
    check(seed_into(search_store, search_rich, 800, 900, utf8_literal("search-forward.bin"), true),
          "Downloads Search source republishes");
    search_ticket.publication_revision = search_publication;
    check(publish_search(), "Downloads republished Search source binds its native scene");
    const u32 before_search_close = picker_starts;
    application_downloads_begin_search(search_ticket, 900, true);
    application_search_close_panel();
    application_downloads_pump();
    check(q->jobs[0].phase == JobPhase::failed && picker_starts == before_search_close &&
              !source_current(q->jobs[0].source),
          "Downloads explicit Search close retires queued admission and terminal retry authority");
    dismiss_all();
    search_panel = true;
    ++search_publication;
    search_ticket.publication_revision = search_publication;
    check(publish_search(), "Downloads latest Search authority binds its native scene");
    const u32 old_actions = channel->actions;
    channel->actions &= ~channel_action_read_history;
    check(!application_downloads_search_available(search_ticket, 900, true),
          "Downloads Search permission loss blocks new Save authority");
    channel->actions = old_actions;
    ++g_app.account.status.profile_id;
    check(!application_downloads_search_available(search_ticket, 900, true),
          "Downloads Search profile fence rejects accepted old-account content");
    --g_app.account.status.profile_id;
    ++channel->generation;
    check(!application_downloads_search_available(search_ticket, 900, true),
          "Downloads Search channel generation fence rejects recycled source");
    --channel->generation;
    picker_ready = false;
    application_downloads_begin_search(search_ticket, 900, true);
    const auto* search_outer = message_rich_store_find(search_rich, 800);
    const auto* search_child = message_rich_store_snapshot(search_rich, 800);
    const u32 url_offset = search_child ? search_child->attachments[0].url.offset : 0;
    if (search_child)
        search_rich.text[url_offset + search_child->attachments[0].url.bytes - 1] = 'b';
    application_downloads_pump();
    check(search_outer && search_child && q->jobs[0].phase == JobPhase::failed &&
              picker_starts == before_search_close,
          "Downloads Search URL fingerprint retirement prevents queued source substitution within the same "
          "ticket");
    dismiss_all();
    search_panel = false;
    for (u32 i = 0; i < 9; ++i) {
        check(seed(510 + i, 610 + i, utf8_literal("queued.bin")), "Downloads bounded receipt source seeds");
        application_downloads_begin(510 + i, 610 + i);
    }
    u32 count = 0;
    for (const auto& j : q->jobs)
        count += j.occupied;
    check(count == 8 && !q->jobs[7].admitted, "Downloads queue is bounded to eight owned receipts");
    const auto panel = q->scene.panel;
    const LPARAM point = MAKELPARAM(static_cast<i16>(panel.left + 2), static_cast<i16>(panel.top + 2));
    application_downloads_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point);
    application_downloads_close();
    check(application_downloads_window_message(WM_LBUTTONUP, 0, point) && GetCapture() != g_app.window,
          "Downloads hiding releases capture and consumes pointer tail");
    application_downloads_prepare_account_stop();
    check(application_downloads_release() && !application_downloads_owner_bytes() &&
              !g_app.renderer.download_scene,
          "Downloads account retirement drains and releases all scoped paths");
    save_target_tests();
    attachment_menu_tests();
    check(application_downloads_release() && !application_downloads_owner_bytes(),
          "Downloads attachment menu owner releases cleanly");
    media_admission_tests();
    ui_search_scene_release(&search_scene);
    message_composer_release(&g_app.message_composer);
    DestroyWindow(g_app.window);
    g_app.window = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::downloads_runtime_test
namespace maicord {
NativeFilePickerError native_file_picker_save_start(NativeFilePicker* picker, HWND, Utf8View,
                                                    Utf8View) noexcept {
    using namespace downloads_runtime_test;
    ++picker_starts;
    picker->runtime = picker;
    picker_result = {1, 0, NativeFilePickerError::none, NativeFilePickerPhase::complete};
    return NativeFilePickerError::none;
}
NativeFilePickerResult native_file_picker_status(const NativeFilePicker* picker) noexcept {
    using namespace downloads_runtime_test;
    return picker && picker->runtime ? picker_ready
                                           ? picker_result
                                           : NativeFilePickerResult{0, 0, NativeFilePickerError::none,
                                                                    NativeFilePickerPhase::showing}
                                     : NativeFilePickerResult{};
}
bool native_file_picker_save_selection(const NativeFilePicker*, NativeFileSaveSelection* out) noexcept {
    using namespace downloads_runtime_test;
    if (!out)
        return false;
    *out = {};
    out->path =
        chosen_executable
            ? ConstSlice<wchar_t>{chosen_script,
                                  static_cast<u32>(sizeof(chosen_script) / sizeof(wchar_t) - 1)}
            : ConstSlice<wchar_t>{chosen_path, static_cast<u32>(sizeof(chosen_path) / sizeof(wchar_t) - 1)};
    out->destination = {{42, 100, 4, 0}, 12};
    out->destination_exists = out->overwrite_authorized = true;
    return true;
}
void native_file_picker_cancel(NativeFilePicker*) noexcept {
    ++downloads_runtime_test::picker_cancels;
    downloads_runtime_test::picker_result.error = NativeFilePickerError::cancelled;
    downloads_runtime_test::picker_ready = true;
}
bool native_file_picker_release(NativeFilePicker* picker, u32) noexcept {
    if (!downloads_runtime_test::picker_reap)
        return false;
    *picker = {};
    return true;
}
u32 native_file_picker_committed_bytes(const NativeFilePicker*) noexcept {
    return 0;
}
bool renderer_read_media_rgba(D3D11Renderer*, MediaAssetKey key, Slice<u8> rgba, u32* width,
                              u32* height) noexcept {
    using namespace downloads_runtime_test;
    ++rgba_reads;
    if (!media_asset_key_valid(resident_key) || !bytes_equal(&key, &resident_key, sizeof(key)) ||
        rgba.count < 16)
        return false;
    for (u32 index = 0; index < 16; ++index)
        rgba[index] = static_cast<u8>(index * 16);
    *width = *height = 2;
    return true;
}
void renderer_bind_focus_owner(D3D11Renderer* r, const UiWidgetManager* owner) noexcept {
    if (r)
        r->focus_owner = owner;
}
namespace app_detail {
UiRect application_viewport_dips() noexcept {
    return downloads_runtime_test::fixture_viewport;
}
void application_composer_text_input_suspend() noexcept {
    ++downloads_runtime_test::suspends;
}
void application_message_forward_close() noexcept {
}
bool application_message_forward_active() noexcept {
    return false;
}
void application_quick_switcher_close() noexcept {
}
bool application_quick_switcher_active() noexcept {
    return false;
}
void application_account_menu_clear() noexcept {
}
void application_image_viewer_close() noexcept {
}
void application_video_blur() noexcept {
}
bool application_search_close_panel() noexcept {
    ++downloads_runtime_test::search_closes;
    downloads_runtime_test::search_panel = false;
    return true;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* out) noexcept {
    using namespace downloads_runtime_test;
    if (!out)
        return false;
    *out = {};
    const auto* channel =
        g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, 101) : nullptr;
    if (!search_panel || ticket.publication_revision != search_publication || ticket.index != 0 ||
        ticket.message_id != 800 || !g_app.account_ui || g_app.plugin_account_scope_serial != 7 ||
        g_app.account.status.profile_id != 4 || g_app.account_ui->chat_state.self_user_id != 3 ||
        !channel || channel->generation != search_channel_generation ||
        !(channel->actions & channel_action_read_history))
        return false;
    const auto found = message_store_find(search_store, ticket.message_id);
    const auto* message = found.ok() ? message_store_resolve(search_store, found.handle) : nullptr;
    const auto* rich = snapshot ? message_rich_store_snapshot(search_rich, ticket.message_id)
                                : message_rich_store_find(search_rich, ticket.message_id);
    if (!message || !rich || rich->message_revision != message->revision)
        return false;
    out->ticket = ticket;
    out->store = &search_store;
    out->message = message;
    out->handle = found.handle;
    out->rich_store = &search_rich;
    out->rich = rich;
    out->snapshot = snapshot;
    return true;
}
UiSearchScene* application_search_scene() noexcept {
    return &downloads_runtime_test::search_scene;
}
void application_pinned_messages_close() noexcept {
}
bool application_pinned_messages_active() noexcept {
    return false;
}
void application_media_picker_close() noexcept {
}
bool application_media_picker_active() noexcept {
    return false;
}
bool application_search_active() noexcept {
    return false;
}
bool application_profile_close() noexcept {
    return true;
}
bool application_settings_active() noexcept {
    return false;
}
bool application_theme_editor_active() noexcept {
    return false;
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    message_context_controller_close(&g_app.message_context);
    return true;
}
void set_popup_owner(PopupOwnerKind kind) noexcept {
    g_app.popup_owner_kind = kind;
}
bool refresh_account_text(bool) noexcept {
    return true;
}
bool refresh_account_frame(bool) noexcept {
    ++downloads_runtime_test::media_refreshes;
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {
    ++downloads_runtime_test::confirmations;
}
bool open_message_media_context_popup(u64 message_id, UiRect, ConstSlice<PopupItem> media,
                                      const ApplicationPluginMenuExtra* plugin) noexcept {
    using namespace downloads_runtime_test;
    ++menu_open_calls;
    plugin_target = plugin ? plugin->target : 0;
    plugin_index = plugin ? plugin->attachment_index : ~0u;
    plugin_text_bytes = plugin ? minimum(plugin->text.bytes, 64u) : 0;
    copy_bytes(plugin_text, plugin ? plugin->text.data : nullptr, plugin_text_bytes);
    if (menu_refuse || media.count > 8)
        return false;
    menu_row_count = media.count;
    for (u32 index = 0; index < media.count; ++index)
        menu_rows[index] = media[index];
    menu_message = message_id;
    g_app.message_context.active = true;
    g_app.message_context.message_id = message_id;
    ++g_app.message_context.opens;
    g_app.popup_scene.active = true;
    g_app.popup_owner_kind = PopupOwnerKind::message;
    return true;
}
bool application_open_content_url(Utf8View) noexcept {
    ++downloads_runtime_test::opened_links;
    return true;
}
bool application_received_attachment_open(u64 message_id, u64 attachment_id, u64 origin) noexcept {
    using namespace downloads_runtime_test;
    ++opened_links;
    opened_message = message_id;
    opened_attachment = attachment_id;
    opened_origin = origin;
    return true;
}
} // namespace app_detail
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_application_downloads_runtime_entry() noexcept {
    maicord::downloads_runtime_test::run();
    ExitProcess(maicord::downloads_runtime_test::failures ? 1 : 0);
}

#include "maicord/app/attachment_media_probe.hpp"
#include "maicord/app/local_preview_thumbnail.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/jpeg.hpp"
#include "jpeg_progressive_fixture.hpp"
#include "media_fixture.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/ui/attachment_menu.hpp"
#include "maicord/ui/composer_attachment_strip.hpp"
#include "maicord/ui/message_composer_scene.hpp"

// Composer upload strip: the file classifier, tile geometry, hit testing,
// scrolling, scene projection and draft metadata that reaches the body.
namespace maicord {
namespace {
bool text_equal(Slice<char> storage, u32 bytes, const char* expected) noexcept {
    u32 length = 0;
    while (expected[length])
        ++length;
    return utf8_equal({storage.data, bytes}, {expected, length});
}
UiComposerAttachmentTile make_tile(u64 id, const char* name, UiComposerAttachmentPhase phase) noexcept {
    UiComposerAttachmentTile tile{};
    tile.id = id;
    u32 bytes = 0;
    while (name[bytes])
        ++bytes;
    tile.filename = {name, bytes};
    tile.bytes = 2048;
    tile.phase = phase;
    tile.kind = ui_attachment_file_kind(tile.filename);
    return tile;
}
} // namespace

void test_composer_attachment_strip(void (*check)(bool, const char*) noexcept) noexcept {
    check(ui_attachment_file_kind(utf8_literal("photo.PNG")) == UiAttachmentFileKind::image &&
              ui_attachment_file_kind(utf8_literal("a.jpeg")) == UiAttachmentFileKind::image &&
              ui_attachment_file_kind(utf8_literal("anim.gif")) == UiAttachmentFileKind::image &&
              ui_attachment_file_kind(utf8_literal("x.webp")) == UiAttachmentFileKind::image &&
              ui_attachment_file_kind(utf8_literal("logo.svg")) == UiAttachmentFileKind::webcode &&
              ui_attachment_file_kind(utf8_literal("clip.MOV")) == UiAttachmentFileKind::video &&
              ui_attachment_file_kind(utf8_literal("clip.mp4")) == UiAttachmentFileKind::video &&
              ui_attachment_file_kind(utf8_literal("song.flac")) == UiAttachmentFileKind::audio &&
              ui_attachment_file_kind(utf8_literal("pack.7z")) == UiAttachmentFileKind::archive &&
              ui_attachment_file_kind(utf8_literal("main.cpp")) == UiAttachmentFileKind::code &&
              ui_attachment_file_kind(utf8_literal("notes.md")) == UiAttachmentFileKind::document &&
              ui_attachment_file_kind(utf8_literal("sheet.csv")) == UiAttachmentFileKind::spreadsheet &&
              ui_attachment_file_kind(utf8_literal("paper.pdf")) == UiAttachmentFileKind::acrobat &&
              ui_attachment_file_kind(utf8_literal("README")) == UiAttachmentFileKind::unknown &&
              ui_attachment_file_kind(utf8_literal(".gitignore")) == UiAttachmentFileKind::unknown &&
              ui_attachment_file_kind(utf8_literal("dir.png\\file")) == UiAttachmentFileKind::unknown &&
              ui_attachment_file_kind(utf8_literal("caf\xC3\xA9.p\xC3\xA9g")) == UiAttachmentFileKind::unknown,
          "attachment classifier follows the official MIME-then-extension classes");
    check(ui_attachment_file_icon(UiAttachmentFileKind::image) == UiIcon::image &&
              ui_attachment_file_icon(UiAttachmentFileKind::video) == UiIcon::file_video &&
              ui_attachment_file_icon(UiAttachmentFileKind::archive) == UiIcon::file_archive &&
              ui_attachment_file_icon(UiAttachmentFileKind::code) == UiIcon::code &&
              ui_attachment_file_icon(UiAttachmentFileKind::acrobat) == UiIcon::file_text &&
              ui_attachment_file_icon(UiAttachmentFileKind::unknown) == UiIcon::file,
          "attachment classes map to icon-pack file cards");
    char text[24];
    check(text_equal({text, 24}, ui_attachment_format_size(12, {text, 24}), "12 B") &&
              text_equal({text, 24}, ui_attachment_format_size(1536, {text, 24}), "1.5 KB") &&
              text_equal({text, 24}, ui_attachment_format_size(10 * 1024 * 1024, {text, 24}), "10 MB") &&
              text_equal({text, 24}, ui_attachment_format_size(512ull * 1024 * 1024 + 1, {text, 24}), "512 MB") &&
              text_equal({text, 24}, ui_attachment_format_size(1288490189ull, {text, 24}), "1.2 GB") &&
              text_equal({text, 24}, ui_attachment_format_duration(7'200, {text, 24}), "0:07") &&
              text_equal({text, 24}, ui_attachment_format_duration(754'000, {text, 24}), "12:34") &&
              text_equal({text, 24}, ui_attachment_format_duration(3'723'000, {text, 24}), "1:02:03"),
          "attachment size and duration labels use the official compact units");

    // Geometry: 200 DIP tiles, 24 DIP gaps, action bar translated (25%, -25%).
    const UiRect strip{100, 50, 700, 50 + ui_composer_attachment_strip_height()};
    UiComposerAttachmentTile tiles[4]{make_tile(11, "photo.png", UiComposerAttachmentPhase::ready),
                                      make_tile(12, "notes.txt", UiComposerAttachmentPhase::uploading),
                                      make_tile(13, "broken.zip", UiComposerAttachmentPhase::failed),
                                      make_tile(14, "clip.mp4", UiComposerAttachmentPhase::ready)};
    tiles[1].uploaded_bytes = 1024;
    tiles[2].can_retry = true;
    tiles[3].duration_ms = 65'000;
    const auto first = ui_composer_attachment_tile_geometry(strip, tiles[0], 0, 0);
    const auto second = ui_composer_attachment_tile_geometry(strip, tiles[1], 1, 0);
    const auto failed = ui_composer_attachment_tile_geometry(strip, tiles[2], 2, 0);
    check(ui_composer_attachment_strip_height() == 224 && first.card.left == 110 && first.card.top == 66 &&
              ui_rect_width(first.card) == 200 && ui_rect_height(first.card) == 200 && second.card.left == 334 &&
              first.media.left == 118 && first.media.top == 74 && first.media.right == 302 &&
              first.name.bottom == 258 && first.media.bottom == first.name.top - 8,
          "upload tiles are fixed 200 DIP cards with an 8 DIP inset media well and filename row");
    check(first.button_count == 3 && first.parts[0] == UiComposerAttachmentPart::spoiler &&
              first.parts[1] == UiComposerAttachmentPart::edit && first.parts[2] == UiComposerAttachmentPart::remove &&
              failed.button_count == 4 && failed.parts[0] == UiComposerAttachmentPart::retry &&
              first.actions.right == first.card.right + ui_rect_width(first.actions) * .25f &&
              first.actions.top == first.card.top - ui_rect_height(first.actions) * .25f &&
              first.actions.top >= strip.top && first.actions.right < second.card.left,
          "the action bar overhangs the tile corner without reaching the next tile");
    check(ui_rect_has_area(second.progress) && !ui_rect_has_area(first.progress) &&
              second.progress.bottom <= second.media.bottom && ui_rect_has_area(second.size) &&
              second.name.right < second.size.left,
          "uploading tiles reserve a progress bar and a right-aligned status");
    const ConstSlice<UiComposerAttachmentTile> all{tiles, 4};
    const auto spoiler_hit = ui_composer_attachment_strip_hit(strip, all, 0, first.buttons[0].left + 2, first.buttons[0].top + 2);
    // The third tile overflows a 600 DIP strip; scroll one tile to reach its bar.
    const auto failed_scrolled = ui_composer_attachment_tile_geometry(strip, tiles[2], 2, 224);
    const auto remove_hit = ui_composer_attachment_strip_hit(strip, all, 224, failed_scrolled.buttons[3].left + 4,
                                                             failed_scrolled.buttons[3].bottom - 4);
    const auto clipped_hit = ui_composer_attachment_strip_hit(strip, all, 0, failed.buttons[3].left + 4,
                                                              failed.buttons[3].bottom - 4);
    const auto body_hit = ui_composer_attachment_strip_hit(strip, all, 0, 200, 150);
    const auto gap_hit = ui_composer_attachment_strip_hit(strip, all, 0, 322, 200);
    const auto outside_hit = ui_composer_attachment_strip_hit(strip, all, 0, 90, 150);
    check(spoiler_hit.index == 0 && spoiler_hit.part == UiComposerAttachmentPart::spoiler && remove_hit.index == 2 &&
              remove_hit.part == UiComposerAttachmentPart::remove && body_hit.index == 0 &&
              body_hit.part == UiComposerAttachmentPart::tile && gap_hit.part == UiComposerAttachmentPart::none &&
              outside_hit.index == kUiComposerAttachmentNoTile && clipped_hit.part == UiComposerAttachmentPart::none,
          "hit testing prefers action buttons, then cards, and ignores gaps and the outside");
    const float content = ui_composer_attachment_strip_content_width(4);
    check(content == 10 + 4 * 200 + 3 * 24 + 30 &&
              ui_composer_attachment_strip_clamp_scroll(strip, 4, 9999) == content - 600 &&
              ui_composer_attachment_strip_clamp_scroll(strip, 4, -5) == 0 &&
              ui_composer_attachment_strip_clamp_scroll(strip, 2, 50) == 0 &&
              ui_composer_attachment_strip_reveal(strip, 4, 3, 0) == 10 + 3 * 224 + 224 - 600 &&
              ui_composer_attachment_strip_reveal(strip, 4, 0, 200) == 0,
          "the strip scrolls horizontally, clamps and reveals keyboard selections");
    const auto scrolled = ui_composer_attachment_strip_hit(strip, all, 224, 200, 150);
    check(scrolled.index == 1, "hit testing follows the horizontal scroll offset");
    const UiRect wide = ui_composer_attachment_fit(first.media, 400, 100);
    const UiRect tiny = ui_composer_attachment_fit(first.media, 10, 10);
    check(ui_rect_width(wide) == 184 && ui_rect_height(wide) == 46 && wide.top > first.media.top &&
              ui_rect_width(tiny) == 20 && !ui_rect_has_area(ui_composer_attachment_fit(first.media, 0, 10)),
          "thumbnails contain-fit their well and are not magnified past 2x");
    UiComposerAttachmentTag tags[kUiComposerAttachmentTags];
    char duration[16];
    tiles[0].animated = tiles[0].described = tiles[0].spoiler = true;
    const u32 photo_tags = ui_composer_attachment_tile_tags(tiles[0], first.media, {tags, 4}, {duration, 16});
    const bool photo_order = photo_tags == 3 && utf8_equal(tags[0].text, utf8_literal("GIF")) &&
                             utf8_equal(tags[1].text, utf8_literal("ALT")) &&
                             utf8_equal(tags[2].text, utf8_literal("SPOILER")) &&
                             tags[0].rectangle.left == first.media.left + 6 && tags[1].rectangle.left > tags[0].rectangle.right;
    const u32 video_tags = ui_composer_attachment_tile_tags(tiles[3], first.media, {tags, 4}, {duration, 16});
    check(photo_order && video_tags == 1 && utf8_equal(tags[0].text, utf8_literal("1:05")),
          "tags show GIF or the video duration, then ALT and SPOILER");
    tiles[0].animated = tiles[0].described = tiles[0].spoiler = false;

    // Paint and text emit bounded native primitives for every visible tile.
    PaintStream paint{};
    UiComposerAttachmentStripView view{};
    view.tiles = all;
    view.strip = {0, 50, 1000, 50 + ui_composer_attachment_strip_height()};
    view.hovered = 0;
    view.hovered_part = UiComposerAttachmentPart::remove;
    view.selected = 1;
    view.focus_visible = true;
    bool painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 400);
    const u32 clip_depth = paint.clip_depth;
    painted = painted && ui_composer_attachment_strip_paint_base(view, kDefaultTheme, &paint);
    const u32 base_quads = paint.count;
    painted = painted && ui_composer_attachment_strip_paint_overlay(view, kDefaultTheme, &paint) &&
              paint.count > base_quads && paint.clip_depth == clip_depth;
    check(painted && base_quads > 8, "strip paint layers cover cards, wells, progress, tags and the action bar");
    paint_stream_release(&paint);

    // Scene projection: the strip replaces the label row and keeps send admission.
    char storage[128]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    const bool ready =
        message_composer_initialize(&model, {storage, sizeof(storage)}) && message_composer_scene_initialize_owned(&scene);
    check(ready, "composer strip scene fixture initializes");
    if (ready) {
        message_composer_reset(&model, 42, false);
        const float width = 740;
        const float height = message_composer_scene_preferred_height(model, width, false, false,
                                                                     message_composer_attachment_row_strip);
        const float label_height =
            message_composer_scene_preferred_height(model, width, false, false, message_composer_attachment_row_label);
        const UiRect viewport{0, 300, width, 300 + height};
        MessageComposerAttachmentInput input{utf8_literal("4 attachments - uploading 10%"), false, true};
        input.tiles = all;
        bool ok = message_composer_scene_update(&scene, &model, viewport, true, 0, {}, nullptr, {}, input).ok();
        const auto strip_view = message_composer_scene_attachment_view(scene, true);
        const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
        check(ok && height - label_height == ui_composer_attachment_strip_height() - 28 &&
                  message_composer_scene_attachment_row(scene) == message_composer_attachment_row_strip &&
                  strip_view.tiles.count == 4 && ui_rect_height(strip_view.strip) == ui_composer_attachment_strip_height() &&
                  edit && edit->rectangle.top >= strip_view.strip.bottom && !message_composer_scene_can_send(scene) &&
                  utf8_equal(strip_view.tiles[2].filename, utf8_literal("broken.zip")) &&
                  strip_view.tiles[0].filename.data != tiles[0].filename.data,
              "the scene copies tiles into a strip row above the editor and blocks sends until uploads finish");
        UiTextCommand commands[48]{};
        char owned[1024]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream, nullptr};
        const bool texted = ui_text_command_stream_initialize(&stream, {commands, 48}, {owned, sizeof(owned)}) &&
                            ui_composer_attachment_strip_text(strip_view, kDefaultTheme, &output);
        bool names = false, percent = false, failure = false;
        for (u32 i = 0; i < stream.count; ++i) {
            names = names || utf8_equal(stream.storage[i].text, utf8_literal("photo.png"));
            percent = percent || utf8_equal(stream.storage[i].text, utf8_literal("50%"));
            failure = failure || utf8_equal(stream.storage[i].text, utf8_literal("Failed"));
        }
        check(texted && names && percent && failure, "strip text covers filenames, upload progress and failures");
        const auto tile_hit =
            message_composer_scene_attachment_hit(scene, strip_view.strip.left + 100, strip_view.strip.top + 100);
        check(message_composer_scene_hit_test(scene, strip_view.strip.left + 100, strip_view.strip.top + 100) ==
                      MessageComposerControl::attachments &&
                  tile_hit.index == 0 && tile_hit.part == UiComposerAttachmentPart::tile &&
                  message_composer_scene_attachment_hover(&scene, tile_hit) &&
                  !message_composer_scene_attachment_hover(&scene, tile_hit) &&
                  message_composer_scene_attachment_scroll(&scene, 120) &&
                  message_composer_scene_attachment_view(scene, false).scroll == 120 &&
                  message_composer_scene_attachment_select(&scene, 0) &&
                  message_composer_scene_attachment_view(scene, false).scroll == 0 &&
                  message_composer_scene_attachment_selected(scene) == 0,
              "scene hover, wheel scroll and keyboard selection update strip presentation");
        // Removing the selected tile keeps the selection on its id, not its index.
        input.tiles = {tiles + 1, 3};
        ok = message_composer_scene_update(&scene, &model, viewport, true, 0, {}, nullptr, {}, input).ok();
        check(ok && message_composer_scene_attachment_selected(scene) == kUiComposerAttachmentNoTile &&
                  message_composer_scene_attachment_view(scene, false).hovered == 0,
              "a removed tile drops its selection");
        input = {utf8_literal("Preparing files… Click to cancel."), false, true};
        ok = message_composer_scene_update(&scene, &model, viewport, true, 0, {}, nullptr, {}, input).ok();
        check(ok && message_composer_scene_attachment_row(scene) == message_composer_attachment_row_label &&
                  !message_composer_scene_attachment_view(scene, false).tiles.count,
              "a label without tiles keeps the compact attachment row");
    }
    message_composer_scene_release(&scene);

    // Draft metadata: rename keeps the uploaded object, alt text and spoiler.
    MessageAttachmentDraftStore store{};
    if (!message_attachment_drafts_initialize(&store)) {
        check(false, "composer strip drafts initialize");
        return;
    }
    const wchar_t path[] = L"C:\\files\\cat.png";
    const MessageAttachmentFile file{{path, 16}, {{3, 4, 5, 0}, 4096}};
    u64 id = 0;
    bool staged = message_attachment_drafts_add(&store, 9, {&file, 1}, {&id, 1}) == MessageAttachmentDraftError::none &&
                  message_attachment_drafts_describe(&store, id, utf8_literal("A sleeping cat"), true) &&
                  message_attachment_drafts_uploaded(&store, id, utf8_literal("uploads/9/abc/cat.png"));
    const u32 revision = store.revision;
    staged = staged && message_attachment_drafts_rename(&store, id, utf8_literal("my cat (final).png")) &&
             store.revision == revision + 1 && message_attachment_drafts_rename(&store, id, utf8_literal("my cat (final).png")) &&
             store.revision == revision + 1;
    UploadedAttachment reference{};
    staged = staged && message_attachment_drafts_reference(&store, id, 0, &reference);
    check(staged && utf8_equal(reference.filename, utf8_literal("my cat (final).png")) &&
              utf8_equal(reference.uploaded_filename, utf8_literal("uploads/9/abc/cat.png")) &&
              utf8_equal(reference.description, utf8_literal("A sleeping cat")) && reference.spoiler,
          "renaming a draft keeps its uploaded object name, description and spoiler");
    check(message_attachment_drafts_rename(&store, id, utf8_literal("c.png")) &&
              message_attachment_drafts_reference(&store, id, 0, &reference) &&
              utf8_equal(reference.uploaded_filename, utf8_literal("uploads/9/abc/cat.png")) &&
              utf8_equal(reference.description, utf8_literal("A sleeping cat")) &&
              !message_attachment_drafts_rename(&store, id, {}) &&
              !message_attachment_drafts_rename(&store, id + (1ull << 32), utf8_literal("x.png")),
          "shorter names restage trailing strings; empty names and stale ids are rejected");
    u8 body[256]{};
    const u32 bytes = attachment_upload_write_references({&reference, 1}, {body, sizeof(body)});
    check(bytes && utf8_equal({reinterpret_cast<const char*>(body), bytes},
                              utf8_literal("[{\"id\":\"0\",\"filename\":\"c.png\",\"uploaded_filename\":"
                                           "\"uploads/9/abc/cat.png\",\"description\":\"A sleeping cat\","
                                           "\"is_spoiler\":true}]")),
          "the attachments array carries filename, alt text and is_spoiler (no SPOILER_ prefix)");
    const u64 ids[]{id};
    check(message_attachment_drafts_mark_queued(&store, {ids, 1}, 55) &&
              !message_attachment_drafts_rename(&store, id, utf8_literal("late.png")),
          "queued drafts cannot be renamed");
    // Limits: ten files per message; the eleventh is rejected atomically.
    MessageAttachmentFile batch[kAttachmentUploadMaximumCount + 1];
    for (auto& entry : batch)
        entry = file;
    u64 batch_ids[kAttachmentUploadMaximumCount + 1]{};
    check(message_attachment_drafts_add(&store, 10, {batch, kAttachmentUploadMaximumCount}, {batch_ids, 11}) ==
                  MessageAttachmentDraftError::none &&
              message_attachment_drafts_add(&store, 10, {batch, 1}, {batch_ids, 11}) ==
                  MessageAttachmentDraftError::channel_capacity &&
              message_attachment_drafts_channel(&store, 10, {}) == kAttachmentUploadMaximumCount,
          "a channel stages at most ten attachments");
    PopupItem items[kAttachmentModifyMenuItems];
    check(attachment_modify_menu(utf8_literal("c.png"), true, {items, kAttachmentModifyMenuItems}) == 3 &&
              items[1].action == popup_action_attachment_rename &&
              utf8_equal(items[2].label, utf8_literal("Edit description (alt text)")) &&
              !attachment_modify_menu({}, false, {items, kAttachmentModifyMenuItems}),
          "modify menu offers rename and alt text");
    message_attachment_drafts_release(&store);
}
// Local thumbnails: streamed box filter, PNG/GIF expansion, EXIF orientation
// and the reduced-scale JPEG decoder for camera-sized photos.
namespace {
struct BitWriter {
    u8* data;
    u32 capacity, used, accumulator, bits;
    void put(u32 value, u32 count) noexcept {
        for (u32 i = count; i-- > 0;) {
            accumulator = accumulator << 1 | (value >> i & 1u);
            if (++bits == 8) {
                if (used < capacity)
                    data[used++] = static_cast<u8>(accumulator);
                if (static_cast<u8>(accumulator) == 0xFF && used < capacity)
                    data[used++] = 0;
                accumulator = bits = 0;
            }
        }
    }
    void flush() noexcept {
        while (bits)
            put(1, 1);
    }
};
u32 put_bytes(u8* out, u32 at, const u8* bytes, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        out[at + i] = bytes[i];
    return at + count;
}
u8 block_value(u32 bx, u32 by) noexcept {
    return static_cast<u8>(20 + (bx * 3 + by * 5) % 200);
}
// Baseline grayscale JFIF whose blocks carry only DC (unit quantization).
u32 build_dc_jpeg(u8* out, u32 capacity, u16 width, u16 height, bool exif_rotate) noexcept {
    u32 at = 0;
    const u8 soi[]{0xFF, 0xD8};
    at = put_bytes(out, at, soi, 2);
    if (exif_rotate) {
        // APP1 Exif, little-endian TIFF, IFD0 with Orientation = 6.
        const u8 app1[]{0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
                        0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00};
        at = put_bytes(out, at, app1, sizeof(app1));
    }
    u8 dqt[69]{0xFF, 0xDB, 0x00, 0x43, 0x00};
    for (u32 i = 5; i < 69; ++i)
        dqt[i] = 1;
    at = put_bytes(out, at, dqt, sizeof(dqt));
    const u8 sof[]{0xFF, 0xC0, 0x00, 0x0B, 8, static_cast<u8>(height >> 8), static_cast<u8>(height),
                   static_cast<u8>(width >> 8), static_cast<u8>(width), 1, 1, 0x11, 0};
    at = put_bytes(out, at, sof, sizeof(sof));
    // DC table: twelve four-bit codes, symbol c is code c.
    u8 dht_dc[]{0xFF, 0xC4, 0x00, 0x1F, 0x00, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    at = put_bytes(out, at, dht_dc, sizeof(dht_dc));
    // AC table: EOB only, one-bit code 0.
    const u8 dht_ac[]{0xFF, 0xC4, 0x00, 0x14, 0x10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};
    at = put_bytes(out, at, dht_ac, sizeof(dht_ac));
    const u8 sos[]{0xFF, 0xDA, 0x00, 0x08, 1, 1, 0x00, 0, 63, 0};
    at = put_bytes(out, at, sos, sizeof(sos));
    BitWriter writer{out + at, capacity - at, 0, 0, 0};
    i32 previous = 0;
    const u32 columns = (width + 7u) / 8u, rows = (height + 7u) / 8u;
    for (u32 by = 0; by < rows; ++by)
        for (u32 bx = 0; bx < columns; ++bx) {
            const i32 dc = (static_cast<i32>(block_value(bx, by)) - 128) * 8;
            const i32 diff = dc - previous;
            previous = dc;
            u32 magnitude = static_cast<u32>(diff < 0 ? -diff : diff), category = 0;
            while (magnitude) {
                ++category;
                magnitude >>= 1;
            }
            writer.put(category, 4);
            if (category)
                writer.put(static_cast<u32>(diff < 0 ? diff + (1 << category) - 1 : diff), category);
            writer.put(0, 1);
        }
    writer.flush();
    at += writer.used;
    const u8 eoi[]{0xFF, 0xD9};
    return put_bytes(out, at, eoi, 2);
}
struct MemoryFile {
    const u8* data;
    u64 bytes;
    u32 reads;
};
bool read_memory(void* context, u64 offset, Slice<u8> output) noexcept {
    auto* file = static_cast<MemoryFile*>(context);
    ++file->reads;
    if (offset > file->bytes || output.count > file->bytes - offset)
        return false;
    copy_bytes(output.data, file->data + offset, output.count);
    return true;
}
void put32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}
u32 rgba_at(const u8* rgba, u32 width, u32 x, u32 y) noexcept {
    const u8* p = rgba + (y * width + x) * 4u;
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
bool close_to(u32 left, u32 right, u32 tolerance) noexcept {
    return (left > right ? left - right : right - left) <= tolerance;
}
struct JpegRowCopy {
    u8* rgba;
    u32 width;
};
bool copy_jpeg_row(void* context, u32 row_index, const u8* row) noexcept {
    const auto* copy = static_cast<JpegRowCopy*>(context);
    copy_bytes(copy->rgba + static_cast<usize>(row_index) * copy->width * 4u, row, copy->width * 4u);
    return true;
}
// Thumbnails decode row by row; the copy is the full RGBA image the checks read.
JpegDecodeResult jpeg_thumbnail_decode_image(ConstSlice<u8> encoded, const JpegImage& image,
                                             const JpegThumbnailLayout& layout, Slice<u8> scratch,
                                             u8* rgba) noexcept {
    static u8 row[kJpegThumbnailDimensionMaximum * 4u];
    JpegRowCopy copy{rgba, layout.width};
    return jpeg_thumbnail_decode_rows(encoded, image, layout, scratch, {row, sizeof(row)}, copy_jpeg_row, &copy);
}
} // namespace

void test_local_preview_thumbnail(void (*check)(bool, const char*) noexcept) noexcept {
    // Alpha-weighted box filter: transparent pixels never darken the average.
    u8 source[4 * 2 * 4]{};
    const u8 red[]{255, 0, 0, 255}, clear[]{0, 0, 0, 0}, blue[]{0, 0, 255, 255};
    for (u32 x = 0; x < 4; ++x)
        for (u32 y = 0; y < 2; ++y)
            copy_bytes(source + (y * 4 + x) * 4, x < 2 ? (y ? clear : red) : blue, 4);
    u8 output[2 * 1 * 4]{};
    u32 sums[2 * 5]{};
    LocalThumbnailSink sink{};
    bool ok = local_thumbnail_begin(&sink, 4, 2, 2, {output, sizeof(output)}, {sums, 10}) && sink.width == 2 &&
              sink.height == 1 && local_thumbnail_push_row(&sink, source) && !local_thumbnail_complete(sink) &&
              local_thumbnail_push_row(&sink, source + 16) && local_thumbnail_complete(sink) &&
              !local_thumbnail_push_row(&sink, source);
    check(ok && output[0] == 255 && output[1] == 0 && output[3] == 128 && output[4] == 0 && output[6] == 255 &&
              output[7] == 255 && local_thumbnail_scale(4032, 3024, kLocalPreviewThumbnailExtent) == 11 &&
              local_thumbnail_scale(300, 200, kLocalPreviewThumbnailExtent) == 1,
          "thumbnail box filter weights color by alpha and bounds the longest side");
    u8 oriented[]{1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 4, 0, 0, 255, 5, 0, 0, 255, 6, 0, 0, 255};
    u8 rotated_scratch[sizeof(oriented)]{};
    u32 w = 3, h = 2;
    // Stored 3x2 [1 2 3 / 4 5 6]; orientation 6 rotates clockwise to 2x3 [4 1 / 5 2 / 6 3].
    ok = local_thumbnail_orient({oriented, sizeof(oriented)}, &w, &h, 6, {rotated_scratch, sizeof(rotated_scratch)});
    check(ok && w == 2 && h == 3 && oriented[0] == 4 && oriented[4] == 1 && oriented[8] == 5 && oriented[20] == 3,
          "EXIF orientation 6 rotates camera thumbnails upright");

    // Every standard PNG color type and depth expands to straight RGBA.
    static u8 png_encoded[4096], png_zlib[4096], png_scanlines[4096];
    struct FormatCase { u8 color_type, bit_depth; };
    constexpr FormatCase formats[]{{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1},
                                   {3, 2},  {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16}};
    bool png_ok = true;
    for (const auto& format : formats) {
        PngFixtureResult fixture{};
        PngImage image{};
        DeflateWorkspace workspace{};
        u8 row[9 * 4]{};
        png_ok = png_ok &&
                 build_png_format_fixture(format.color_type, format.bit_depth, 9, 3, {png_encoded, sizeof(png_encoded)},
                                          {png_zlib, sizeof(png_zlib)}, &fixture) &&
                 png_decode_scanlines({png_encoded, fixture.encoded_bytes}, &image, &workspace,
                                      {png_scanlines, sizeof(png_scanlines)})
                     .ok();
        for (u32 y = 0; png_ok && y < 3; ++y)
            png_ok = local_thumbnail_png_row(image, png_scanlines + y * image.row_stride + 1, row);
    }
    check(png_ok, "PNG thumbnail rows expand gray, RGB, palette, alpha and 16-bit samples");

    // A 1000 px wide RGBA PNG streams into a 334 px thumbnail (box factor 3).
    static u8 large_png[96u * 1024u], large_zlib[96u * 1024u];
    static u8 large_scanlines[16u * (1000u * 4u + 1u)];
    static u8 large_output[kLocalPreviewThumbnailExtent * kLocalPreviewThumbnailExtent * 4u];
    static u32 large_sums[(kLocalPreviewThumbnailExtent + 1) * 5];
    PngFixtureResult fixture{};
    PngImage image{};
    DeflateWorkspace workspace{};
    static u8 row[1000 * 4];
    ok = build_rgba8_png_fixture(1000, 16, {large_png, sizeof(large_png)}, {large_zlib, sizeof(large_zlib)}, &fixture,
                                 [](u32 x, u32 y, u32, u32) noexcept -> u32 {
                                     return 0xFF000000u | (x < 500 ? 0xFFu : 0u) | (y < 8 ? 0xFF00u : 0u);
                                 }) &&
         png_decode_scanlines({large_png, fixture.encoded_bytes}, &image, &workspace,
                              {large_scanlines, sizeof(large_scanlines)})
             .ok() &&
         local_thumbnail_begin(&sink, image.width, image.height, kLocalPreviewThumbnailExtent,
                               {large_output, sizeof(large_output)}, {large_sums, sizeof(large_sums) / 4});
    for (u32 y = 0; ok && y < image.height; ++y)
        ok = local_thumbnail_png_row(image, large_scanlines + y * image.row_stride + 1, row) &&
             local_thumbnail_push_row(&sink, row);
    check(ok && local_thumbnail_complete(sink) && sink.width == 334 && sink.height == 6 &&
              rgba_at(large_output, sink.width, 10, 1) == 0xFF00FFFFu &&
              rgba_at(large_output, sink.width, 330, 4) == 0xFF000000u,
          "large PNGs stream through the thumbnail filter without a full-size RGBA copy");

    // GIF: the first frame composites onto a transparent canvas.
    static u8 gif_encoded[2048], gif_lzw[512], gif_indices[64];
    GifFixtureResult gif_fixture{};
    GifImage animation{};
    static GifWorkspace gif_workspace;
    ok = build_indexed_gif_fixture({gif_encoded, sizeof(gif_encoded)}, {gif_lzw, sizeof(gif_lzw)}, &gif_fixture);
    const auto first_frame = ok ? gif_decode_frame({gif_encoded, gif_fixture.encoded_bytes}, 0, &animation,
                                                   &gif_workspace, {gif_indices, sizeof(gif_indices)})
                                : GifDecodeResult{};
    u8 gif_output[16 * 4]{};
    u32 gif_sums[4 * 5]{};
    u8 gif_row[16]{};
    ok = first_frame.ok() && local_thumbnail_begin(&sink, animation.canvas_width, animation.canvas_height,
                                                   kLocalPreviewThumbnailExtent, {gif_output, sizeof(gif_output)},
                                                   {gif_sums, 20});
    for (u32 y = 0; ok && y < animation.canvas_height; ++y)
        ok = local_thumbnail_gif_row(first_frame.frame, first_frame.palette,
                                     {gif_indices, first_frame.pixels}, animation.canvas_width, y, gif_row) &&
             local_thumbnail_push_row(&sink, gif_row);
    check(ok && local_thumbnail_complete(sink) && sink.width == animation.canvas_width &&
              gif_output[3] == static_cast<u8>(first_frame.palette.rgba[gif_indices[0]] >> 24),
          "GIF thumbnails take the first frame's palette colors");

    // Camera-sized JPEG beyond the 512 px GPU path: reduced-scale decode.
    static u8 jpeg[256 * 1024];
    static JpegWorkspace jpeg_workspace;
    const u32 jpeg_bytes = build_dc_jpeg(jpeg, sizeof(jpeg), 1600, 900, false);
    JpegImage jpeg_image{};
    JpegThumbnailLayout layout{};
    const bool legacy_rejects = !jpeg_parse({jpeg, jpeg_bytes}, &jpeg_image, &jpeg_workspace).ok();
    ok = jpeg_thumbnail_parse({jpeg, jpeg_bytes}, kLocalPreviewThumbnailExtent, &jpeg_image, &jpeg_workspace, &layout)
             .ok();
    check(legacy_rejects && ok && layout.samples == 2 && layout.width == 400 && layout.height == 225 &&
              layout.coefficient_bytes == 0 && jpeg_image.orientation == JpegOrientation::top_left,
          "large JPEGs parse for thumbnails at the smallest sufficient block scale");
    static u8 jpeg_rgba[512u * 384u * 4u];
    alignas(16) static u8 aligned_planes[1u * 1024u * 1024u];
    ok = ok && layout.plane_bytes <= sizeof(aligned_planes) &&
         jpeg_thumbnail_decode_image({jpeg, jpeg_bytes}, jpeg_image, layout, {aligned_planes, sizeof(aligned_planes)},
                                     jpeg_rgba)
             .ok();
    bool jpeg_values = ok;
    for (u32 by = 0; jpeg_values && by < 28; by += 9)
        for (u32 bx = 0; jpeg_values && bx < 200; bx += 37) {
            const u32 pixel = rgba_at(jpeg_rgba, layout.width, bx * 2, by * 2);
            jpeg_values = close_to(pixel & 0xFFu, block_value(bx, by), 1) && (pixel >> 24) == 255 &&
                          (pixel & 0xFFu) == (pixel >> 8 & 0xFFu);
        }
    check(jpeg_values, "reduced-scale JPEG blocks reconstruct their exact average");
    const u32 rotated_bytes = build_dc_jpeg(jpeg, sizeof(jpeg), 1024, 768, true);
    ok = jpeg_thumbnail_parse({jpeg, rotated_bytes}, kLocalPreviewThumbnailExtent, &jpeg_image, &jpeg_workspace, &layout)
             .ok();
    check(ok && jpeg_image.orientation == JpegOrientation::right_top && layout.samples == 4 && layout.width == 512 &&
              layout.height == 384,
          "camera JPEGs report EXIF orientation for the thumbnail");
    // Progressive streams keep coefficients and match the reference within chroma upsampling.
    ok = jpeg_thumbnail_parse({reinterpret_cast<const u8*>(kJpegProgressive420Reference), kJpegProgressive420ReferenceBytes},
                              kLocalPreviewThumbnailExtent, &jpeg_image, &jpeg_workspace, &layout)
             .ok() &&
         layout.samples == 8 && layout.width == 16 && layout.coefficient_bytes &&
         jpeg_thumbnail_decode_image({reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                                      kJpegProgressive420ReferenceBytes},
                                     jpeg_image, layout, {aligned_planes, sizeof(aligned_planes)}, jpeg_rgba)
             .ok();
    u64 difference = 0;
    for (u32 i = 0; ok && i < 16 * 16; ++i)
        for (u32 c = 0; c < 3; ++c) {
            const i32 delta = static_cast<i32>(jpeg_rgba[i * 4 + c]) -
                              static_cast<i32>(static_cast<u8>(kJpegProgressive420ReferenceRgb[i * 3 + c]));
            difference += static_cast<u64>(delta < 0 ? -delta : delta);
        }
    check(ok && difference / (16 * 16 * 3) <= 12, "progressive JPEG thumbnails match the reference decode");

    // Video duration badge: the movie header is found behind a large mdat
    // (non-faststart) and in a 64-bit version-1 header, reading headers only.
    static u8 movie[4096]{};
    u32 at = 0;
    put32(movie + at, 16);
    copy_bytes(movie + at + 4, "ftypisom    ", 12);
    at += 16;
    put32(movie + at, 1);
    copy_bytes(movie + at + 4, "mdat", 4);
    put32(movie + at + 8, 0);
    put32(movie + at + 12, 3000);
    at += 3000;
    const u32 moov = at;
    put32(movie + at, 8 + 8 + 100);
    copy_bytes(movie + at + 4, "moov", 4);
    at += 8;
    put32(movie + at, 8 + 100);
    copy_bytes(movie + at + 4, "mvhd", 4);
    put32(movie + at + 8 + 12, 600);
    put32(movie + at + 8 + 16, 600u * 65u + 300u);
    at += 108;
    MemoryFile file{movie, at, 0};
    u32 duration = 0;
    const bool v0 = attachment_mp4_duration(at, read_memory, &file, &duration) && duration == 65'500 && file.reads < 10;
    movie[moov + 16] = 1; // version 1: 64-bit times
    put32(movie + moov + 16 + 20, 1000);
    put32(movie + moov + 16 + 24, 0);
    put32(movie + moov + 16 + 28, 7'250);
    const bool v1 = attachment_mp4_duration(at, read_memory, &file, &duration) && duration == 7'250;
    put32(movie + moov + 16 + 24, 0xffffffffu);
    put32(movie + moov + 16 + 28, 0xffffffffu);
    const bool unknown = !attachment_mp4_duration(at, read_memory, &file, &duration);
    file.bytes = moov + 20;
    const bool truncated = !attachment_mp4_duration(moov + 20, read_memory, &file, &duration);
    check(v0 && v1 && unknown && truncated && !attachment_mp4_duration(16, read_memory, &file, &duration),
          "MP4 duration probe reads mvhd v0/v1 behind media data and rejects unknown or truncated headers");
}

} // namespace maicord

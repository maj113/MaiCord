#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/button.hpp"

namespace maicord {
namespace {
struct DownloadFixture {
    u32 calls{}; u64 base{100}, received{65536};
    UiDownloadPhase first_phase{UiDownloadPhase::downloading};
    bool current{true}, duplicate{}, retire_on_resolve{}, cancelled_retry{};
};
bool download_current(void* context) noexcept { return static_cast<DownloadFixture*>(context)->current; }
bool download_row(void* context, u32 index, UiDownloadRow* row) noexcept {
    auto& f = *static_cast<DownloadFixture*>(context); ++f.calls;
    const UiDownloadPhase phases[]{f.first_phase,UiDownloadPhase::queued,UiDownloadPhase::preparing,
        UiDownloadPhase::committing,UiDownloadPhase::saved,UiDownloadPhase::failed,
        UiDownloadPhase::cancelled,UiDownloadPhase::downloading};
    const auto phase = phases[index];
    const bool terminal = phase == UiDownloadPhase::saved || phase == UiDownloadPhase::failed || phase == UiDownloadPhase::cancelled;
    *row = {f.duplicate && index == 1 ? f.base : f.base + index,
        utf8_literal("original-interface-study-with-a-long-filename.png"),
        utf8_literal("MaiCord Lab • release-planning • Forwarded attachment"),
        phase == UiDownloadPhase::failed ? utf8_literal("Connection lost. Try again.") : Utf8View{},
        index == 0 ? f.received : index == 3 || index == 4 ? 262144 : 0,
        index == 2 || index == 7 ? 0ull : 262144ull,phase,index == 2 || index == 7,!terminal,
        phase == UiDownloadPhase::failed || (phase == UiDownloadPhase::cancelled && f.cancelled_retry),
        phase == UiDownloadPhase::saved,terminal};
    if (f.retire_on_resolve) f.current = false;
    return true;
}
bool has(Utf8View text, const char* part) noexcept {
    const u32 bytes = c_string_length(part);
    for (u32 i = 0; i + bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i,part,bytes)) return true;
    return false;
}
}
void test_download_scene(void (*check)(bool,const char*) noexcept) noexcept {
    UiDownloadScene s{}; DownloadFixture fixture{};
    UiDownloadSource source{&fixture,8,1,download_row,download_current};
    UiDownloadContent content{}; content.revision = 1;
    const UiRect viewport{0,0,1280,800};
    check(!s.storage.arena.base && ui_download_scene_update(&s,&source,&content,viewport) &&
        fixture.calls == 8 && s.widgets.count == kUiDownloadWidgetCapacity && s.count == 3 && s.page_size == 3,
        "downloads: bounded eight-receipt bank allocates only twelve stable visible controls lazily");
    const auto root = s.root, first_button = s.primary[0];
    const auto layout = s.widgets.layout.revision, revision = s.revision, calls = fixture.calls;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i) warm &= ui_download_scene_update(&s,&source,&content,viewport);
    check(warm && s.revision == revision && s.widgets.layout.revision == layout && fixture.calls == calls,
        "downloads: sixty-four unchanged publications perform no receipt resolution or layout");
    content.status = utf8_literal("1 saving • 2 waiting"); ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,viewport) && fixture.calls == calls &&
        s.widgets.layout.revision == layout && s.root.slot == root.slot && s.primary[0].generation == first_button.generation,
        "downloads: header-only changes retain handles and accepted receipt data");
    // Previous on the first page and Next on the last fade to half opacity and
    // leave the Tab order; the enabled side keeps full-strength text.
    const auto pager_alpha = [&](const char* label) noexcept {
        UiTextCommand commands[64]{};
        UiTextCommandStream stream{};
        UiTextOutput out{&stream,nullptr};
        if (!ui_text_command_stream_initialize(&stream,{commands,64}) ||
            !ui_download_scene_text_output(s,kDefaultTheme,&out)) return -1.f;
        for (u32 i = 0; i < stream.count; ++i)
            if (has(commands[i].text,label) && commands[i].text.bytes < 9) return commands[i].color.alpha;
        return -1.f;
    };
    const auto* previous_widget = ui_widget_manager_resolve(s.widgets,s.previous);
    check(previous_widget && !(previous_widget->flags & ui_widget_focusable) &&
        pager_alpha("Previous") == kDefaultTheme.text_secondary.alpha * .5f &&
        pager_alpha("Next") == kDefaultTheme.text_primary.alpha,
        "downloads: Previous is faded and skipped by Tab on the first page");
    check(ui_download_scene_page(&s,2) && pager_alpha("Next") == kDefaultTheme.text_secondary.alpha * .5f &&
        pager_alpha("Previous") == kDefaultTheme.text_primary.alpha && ui_download_scene_page(&s,0),
        "downloads: Next is faded on the last page");
    check(ui_download_scene_page(&s,1) && s.first == 3 && s.count == 3 &&
        ui_download_scene_target(s,s.primary[0]).action == UiDownloadAction::cancel &&
        ui_download_scene_target(s,s.primary[1]).action == UiDownloadAction::reveal &&
        ui_download_scene_target(s,s.primary[2]).action == UiDownloadAction::retry &&
        ui_download_scene_target(s,s.secondary[1]).action == UiDownloadAction::dismiss &&
        ui_download_scene_target(s,s.primary[2]).id == 105 &&
        has({s.progress[0],s.progress_bytes[0]},"100%"),
        "downloads: committing remains cancellable without fake Saved while saved failed terminal controls retain exact ids");
    check(ui_download_scene_page(&s,2) && s.first == 6 && s.count == 2 &&
        ui_download_scene_target(s,s.primary[0]).action == UiDownloadAction::none &&
        ui_download_scene_target(s,s.secondary[0]).action == UiDownloadAction::dismiss &&
        !has({s.progress[1],s.progress_bytes[1]},"%") &&
        ui_download_scene_target(s,s.next).action == UiDownloadAction::none && !ui_download_scene_page(&s,3),
        "downloads: last page preserves cancelled dismissal and indeterminate bytes without fictitious percentage");
    fixture.cancelled_retry = true; ++source.revision; ++content.revision;
    UiTextCommand retry_commands[32]{}; UiTextCommandStream retry_stream{}; UiTextOutput retry_output{&retry_stream,nullptr};
    bool cancelled_retry = ui_download_scene_update(&s,&source,&content,viewport) &&
        ui_text_command_stream_initialize(&retry_stream,{retry_commands,32}) &&
        ui_download_scene_text_output(s,kDefaultTheme,&retry_output);
    const auto cancelled_target = ui_download_scene_target(s,s.primary[0]);
    bool retry_shown = false;
    for (u32 i = 0; i < retry_stream.count; ++i)
        if (retry_commands[i].owner.handle.slot == s.primary[0].slot && retry_commands[i].text.bytes == 5 &&
            has(retry_commands[i].text,"Retry")) retry_shown = true;
    check(cancelled_retry && retry_shown && cancelled_target.action == UiDownloadAction::retry &&
        cancelled_target.id == 106 && cancelled_target.index == 6 && cancelled_target.source_revision == source.revision &&
        ui_download_scene_target(s,s.secondary[0]).action == UiDownloadAction::dismiss,
        "downloads: host-approved cancelled Retry is visible and retains the exact receipt id index and source fence");
    fixture.cancelled_retry = false; ++source.revision; ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,viewport) &&
        ui_download_scene_target(s,s.primary[0]).action == UiDownloadAction::none &&
        ui_download_scene_target(s,s.secondary[0]).action == UiDownloadAction::dismiss,
        "downloads: cancelled receipt without Retry authority keeps only Dismiss");
    ui_download_scene_page(&s,0);
    auto r = s.primary_rectangles[0]; UiPoint point{(r.left + r.right) * .5f,(r.top + r.bottom) * .5f};
    const auto press = ui_button_pointer(&s.widgets,UiButtonPointerEvent::press,point);
    fixture.received += 8192; ++source.revision; ++content.revision;
    const bool progress = ui_download_scene_update(&s,&source,&content,viewport);
    const auto release = ui_button_pointer(&s.widgets,UiButtonPointerEvent::release,point);
    check(press.handled && progress && release.activate && ui_download_scene_target(s,release.target).id == 100,
        "downloads: byte progress retains a valid Cancel capture for the same receipt and action");
    const auto press2 = ui_button_pointer(&s.widgets,UiButtonPointerEvent::press,point);
    fixture.base = 200; ++source.revision; ++content.revision;
    const bool replacement = ui_download_scene_update(&s,&source,&content,viewport);
    const auto release2 = ui_button_pointer(&s.widgets,UiButtonPointerEvent::release,point);
    check(press2.handled && replacement && !release2.activate && !s.widgets.captured.generation,
        "downloads: replacing a receipt cannot inherit an earlier Cancel press");
    ui_button_pointer(&s.widgets,UiButtonPointerEvent::press,point);
    fixture.first_phase = UiDownloadPhase::saved; ++source.revision; ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,viewport) && !s.widgets.captured.generation &&
        ui_download_scene_target(s,s.primary[0]).action == UiDownloadAction::reveal,
        "downloads: final commit changes the action only after saved and cancels old Cancel capture");
    PaintStream paint{}; UiTextCommand commands[128]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream,nullptr};
    bool visual = paint_stream_initialize(&paint) && ui_text_command_stream_initialize(&stream,{commands,128});
    Theme bright = kLightTheme; bright.accent = {.95f,.85f,.4f,1};
    const Theme themes[]{kDefaultTheme,kLightTheme,bright,ui_theme_amoled()};
    const UiRect sizes[]{viewport,{0,0,320,420},{0,0,240,300},{0,0,1920,1080}};
    for (u32 variant = 0; variant < 4; ++variant) {
        const auto& theme = themes[variant]; const auto size = sizes[variant];
        visual &= ui_download_scene_update(&s,&source,&content,size);
        for (u32 page = 0; page * s.page_size < source.count; ++page) {
            visual &= ui_download_scene_page(&s,page);
            paint_stream_reset(&paint,size.right,size.bottom); ui_text_command_stream_reset(&stream);
            visual &= ui_download_scene_paint(s,theme,&paint) && ui_download_scene_text_output(s,theme,&output) &&
                !paint.rejected && !stream.rejected && paint.clip_depth == 1;
            for (u32 i = 0; i < s.count; ++i) {
                visual &= ui_rect_height(s.cards[i]) >= 136 && s.cards[i].bottom <= s.body.bottom &&
                    s.primary_rectangles[i].left >= s.cards[i].left && s.secondary_rectangles[i].right <= s.cards[i].right;
            }
            for (u32 i = 0; i < stream.count; ++i)
                visual &= commands[i].clip.left >= s.panel.left && commands[i].clip.right <= s.panel.right &&
                    commands[i].clip.top >= s.panel.top && commands[i].clip.bottom <= s.panel.bottom;
        }
    }
    check(visual,"downloads: all pages stay usable at minimum narrow wide dark light bright-accent and 144 DPI layouts");
    // Quads drawn on a card's progress track: the track itself plus any fill.
    const auto track_quads = [&](UiRect card) noexcept {
        const UiRect track{card.left + 12, card.bottom - 42, card.right - 12, card.bottom - 38};
        u32 found = 0;
        for (u32 i = 0; i < paint.count; ++i) {
            const auto& q = paint.quads[i];
            found += q.top == track.top && q.bottom == track.bottom && q.left >= track.left &&
                     q.right <= track.right;
        }
        return found;
    };
    paint_stream_reset(&paint, viewport.right, viewport.bottom);
    const bool waiting_painted = ui_download_scene_update(&s, &source, &content, viewport) &&
                                 ui_download_scene_page(&s, 0) &&
                                 ui_download_scene_paint(s, kDefaultTheme, &paint) && s.count == 3;
    check(
        waiting_painted && track_quads(s.cards[2]) == 1 && track_quads(s.cards[1]) == 1,
        "downloads: a row still preparing or queued shows a bare progress track without placeholder marks");
    fixture.first_phase = UiDownloadPhase::downloading;
    ++source.revision;
    ++content.revision;
    paint_stream_reset(&paint, viewport.right, viewport.bottom);
    check(ui_download_scene_update(&s, &source, &content, viewport) &&
              ui_download_scene_paint(s, kDefaultTheme, &paint) && track_quads(s.cards[0]) == 2,
          "downloads: a downloading row fills its track to the received fraction");
    fixture.first_phase = UiDownloadPhase::saved;
    ++source.revision;
    ++content.revision;
    check(ui_download_scene_update(&s, &source, &content, sizes[3]),
          "downloads: the saved fixture republishes");
    const UiWidgetReference attachment_owner{&s.widgets,s.close};
    const auto* attachment_node = ui_widget_manager_layout_node(s.widgets,s.close);
    const UiRect attachment = attachment_node ? attachment_node->rectangle : UiRect{};
    const UiRect narrow_action = ui_attachment_save_rectangle(attachment,false);
    paint_stream_reset(&paint,s.viewport.right,s.viewport.bottom);
    check(!ui_rect_has_area(narrow_action) &&
        ui_attachment_save_paint(&paint,attachment_owner,narrow_action,s.viewport,kDefaultTheme,false,true) &&
        !paint.count && !paint.batch_count && !paint.rejected && paint.clip_depth == 1 && paint.clip_count == 1,
        "attachment Save: too-narrow media yields an empty successful no-op without rejecting the frame");
    const UiRect gif{100,100,400,300};
    const UiRect gif_save = ui_attachment_save_rectangle(gif,false);
    const UiRect gif_attachment_star = ui_gif_favorite_rectangle(gif,true);
    const UiRect gif_embed_star = ui_gif_favorite_rectangle(gif,false);
    paint_stream_reset(&paint,s.viewport.right,s.viewport.bottom);
    check(ui_rect_has_area(gif_attachment_star) && gif_attachment_star.right + 4 == gif_save.left &&
        gif_attachment_star.top == gif_save.top && ui_rect_width(gif_attachment_star) == ui_rect_width(gif_save) &&
        gif_embed_star.left == gif_save.left && gif_embed_star.top == gif_save.top &&
        !ui_rect_has_area(ui_gif_favorite_rectangle({100,100,170,300},true)) &&
        ui_gif_favorite_paint(&paint,attachment_owner,gif_embed_star,s.viewport,kDefaultTheme,true,false) &&
        !paint.rejected && paint.clip_depth == 1 &&
        !ui_gif_favorite_paint(&paint,attachment_owner,{10,10,5,20},s.viewport,kDefaultTheme,false,false),
        "favourite star: sits left of an attachment's Save or in its place on an embed, needs room, and "
        "rejects reversed geometry");
    const UiRect file_action = ui_attachment_save_rectangle(
        {attachment.left - 2,attachment.top - 2,attachment.right + 2,attachment.bottom + 2},true);
    check(ui_rect_has_area(file_action) &&
        ui_attachment_save_paint(&paint,attachment_owner,file_action,{},kDefaultTheme,true) &&
        !paint.count && !paint.batch_count && !paint.rejected && paint.clip_depth == 1 && paint.clip_count == 1,
        "attachment Save: an empty caller clip is a successful no-op without paint or clip-stack work");
    const UiRect invalid_geometry{10,10,5,20};
    check(!ui_attachment_save_paint(&paint,attachment_owner,invalid_geometry,s.viewport,kDefaultTheme,true) &&
        !ui_attachment_save_paint(&paint,attachment_owner,file_action,invalid_geometry,kDefaultTheme,true) &&
        !paint.count && !paint.batch_count && !paint.rejected && paint.clip_depth == 1 && paint.clip_count == 1,
        "attachment Save: reversed action or clip geometry fails before emitting partial paint");
    const UiRect half_clip{file_action.left,file_action.top,
        (file_action.left + file_action.right) * .5f,file_action.bottom};
    bool clipped_save = attachment_node &&
        ui_attachment_save_paint(&paint,attachment_owner,file_action,half_clip,kLightTheme,true) &&
        paint.count && paint.batch_count && !paint.rejected && paint.clip_depth == 1;
    for (u32 i = 0; i < paint.batch_count; ++i) {
        const auto clip = paint.clips[paint.batches[i].clip_index];
        clipped_save &= ui_rect_equal({clip.left,clip.top,clip.right,clip.bottom},half_clip);
    }
    check(clipped_save,"attachment Save: file action paints without hover under the exact partial clip");
    content.mode = UiDownloadMode::confirmation; content.confirmation_id = 42;
    content.filename = utf8_literal("review-tool.exe");
    content.warning = utf8_literal("This file can run software on your computer. Save it only if you trust its source."); ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,{0,0,240,300}) && !s.count &&
        s.widgets.focused.slot == s.cancel.slot && ui_download_scene_target(s,s.cancel).action == UiDownloadAction::cancel_confirmation &&
        ui_download_scene_target(s,s.confirm).action == UiDownloadAction::confirm &&
        ui_download_scene_target(s,s.confirm).id == 42 &&
        ui_download_scene_hit_test(s,0,0).action == UiDownloadAction::close,
        "downloads: dangerous-file confirmation defaults to Cancel and exposes an exact token without admitting a receipt");
    ui_widget_manager_focus(&s.widgets,s.confirm); ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,s.viewport) && s.widgets.focused.slot == s.confirm.slot,
        "downloads: unchanged confirmation authority does not steal user keyboard focus");
    r = s.confirm_rectangle; point = {(r.left + r.right) * .5f,(r.top + r.bottom) * .5f};
    ui_button_pointer(&s.widgets,UiButtonPointerEvent::press,point);
    content.confirmation_id = 43; ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,s.viewport) && !s.widgets.captured.generation &&
        s.widgets.focused.slot == s.cancel.slot && ui_download_scene_target(s,s.confirm).id == 43,
        "downloads: replacing confirmation cancels old approval capture and restores the safe default");
    fixture.current = false; paint_stream_reset(&paint,240,300); ui_text_command_stream_reset(&stream);
    check(!ui_download_scene_current(s) && ui_download_scene_target(s,s.confirm).action == UiDownloadAction::none &&
        ui_download_scene_paint(s,kDefaultTheme,&paint) && !paint.count &&
        ui_download_scene_text_output(s,kDefaultTheme,&output) && !stream.count,
        "downloads: retired account authority removes modal paint text and every job or approval route");
    fixture.current = true; fixture.duplicate = true; ++source.revision; ++content.revision;
    check(!ui_download_scene_update(&s,&source,&content,viewport) && !ui_download_scene_current(s),
        "downloads: duplicate receipt identity rejects partial bank publication");
    fixture.duplicate = false; fixture.retire_on_resolve = true; ++source.revision;
    check(!ui_download_scene_update(&s,&source,&content,viewport) && !ui_download_scene_current(s),
        "downloads: post-resolve authority fence rejects a bank retired during projection");
    fixture.retire_on_resolve = false; fixture.current = true;
    source.count = 0; ++source.revision; content.mode = UiDownloadMode::receipts; ++content.revision;
    check(ui_download_scene_update(&s,&source,&content,viewport) && !s.count &&
        ui_download_scene_target(s,s.primary[0]).action == UiDownloadAction::none,
        "downloads: empty receipts retire job controls while retaining hide-only modal ownership");
    paint_stream_release(&paint); ui_download_scene_release(&s);
    check(!s.active && !s.storage.arena.base,"downloads: closing the scene releases its bounded lazy UI bank");
}
} // namespace maicord

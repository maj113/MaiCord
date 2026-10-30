#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text_measure_cache.hpp"

namespace maicord {
namespace {
bool forum_measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* result) noexcept {
    const auto value = utf8_validate(request.text);
    if (!value.valid || request.constraint.width <= 0)
        return false;
    const float width = value.codepoints * 8.f;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 rows = request.no_wrap ? 1u : maximum(1u, (value.codepoints + columns - 1) / columns);
    *result = {{request.no_wrap ? width : minimum(width, request.constraint.width), rows * 20.f}, 16, rows};
    return true;
}
} // namespace
void test_forum_post_scene(void (*check)(bool, const char*) noexcept) noexcept {
    UiTextMeasureRecord records[64]{};
    UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 64}, forum_measure, nullptr),
          "forum_scene_measure_init");
    ForumPostTagView tags[] = {{1, utf8_literal("Questions"), false, true},
                               {2, utf8_literal("日本語"), true, true},
                               {3, utf8_literal("Review"), false, false}};
    ForumPostContent content{};
    content.heading = utf8_literal("Create a forum post");
    content.context = utf8_literal("Community");
    content.title_label = utf8_literal("Title");
    content.body_label = utf8_literal("Message");
    content.hint = utf8_literal("Ctrl+Enter creates your post.");
    content.tags_hint = utf8_literal("Choose at least one tag (up to 5).");
    content.cancel = utf8_literal("Cancel");
    content.accept = utf8_literal("Create post");
    content.fields[0] = {utf8_literal("Release 日本語😀"), 0, 0, {}, true};
    content.fields[1] = {utf8_literal("First line\nSecond line"), 0, 0, {}, false};
    content.tags = {tags, 3};
    content.editable = content.can_accept = content.focus_visible = true;
    utf8_utf16_position_for_byte_offset(content.fields[0].text, content.fields[0].text.bytes,
                                        &content.fields[0].caret_utf16);
    utf8_utf16_position_for_byte_offset(content.fields[1].text, content.fields[1].text.bytes,
                                        &content.fields[1].caret_utf16);
    ForumPostScene scene{};
    const UiRect viewport{0, 0, 640, 600};
    check(forum_post_update(&scene, content, viewport, &cache) &&
              forum_post_focused(scene).element == ForumPostElement::title,
          "forum_scene_two_fields_initial_title_focus");
    if (!scene.active) {
        forum_post_release(&scene);
        return;
    }
    check(forum_post_move_focus(&scene, false) &&
              forum_post_focused(scene).element == ForumPostElement::body &&
              forum_post_move_focus(&scene, false) &&
              forum_post_focused(scene).element == ForumPostElement::tags &&
              forum_post_focused(scene).tag == 0 && forum_post_move_focus(&scene, false) &&
              forum_post_focused(scene).tag == 1 && forum_post_move_focus(&scene, false) &&
              forum_post_focused(scene).element == ForumPostElement::cancel &&
              forum_post_move_focus(&scene, true) &&
              forum_post_focused(scene).element == ForumPostElement::tags &&
              forum_post_focused(scene).tag == 1,
          "forum_scene_tab_visual_order_skips_disabled_tag_reverse");
    const auto generation = scene.widgets.generation;
    const auto revision = scene.revision;
    const auto layout = scene.widgets.layout.revision;
    const auto calls = cache.backend_calls;
    const auto storage = scene.storage.arena.base;
    bool stable = true;
    for (u32 i = 0; i < 64; ++i)
        stable = forum_post_update(&scene, content, viewport, &cache) && stable;
    check(stable && scene.widgets.generation == generation && scene.revision == revision &&
              scene.widgets.layout.revision == layout && cache.backend_calls == calls &&
              scene.storage.arena.base == storage,
          "forum_scene_64_unchanged_updates_reuse_widgets_layout_measurement_storage");
    const auto original_name = tags[0].name;
    const Utf8View wire_name{};
    tags[0].name = forum_post_tag_label(wire_name);
    check(!wire_name.bytes && tags[0].id == 1 && utf8_equal(tags[0].name, utf8_literal("Unnamed tag")) &&
              forum_post_update(&scene, content, viewport, &cache) &&
              forum_post_focus(&scene, {ForumPostElement::tags, 0}),
          "forum_scene_empty_wire_tag_label_projects_named_selectable_chip_without_mutating_metadata");
    tags[0].name = original_name;
    ForumPostTagView capped[6]{};
    for (u32 i = 0; i < 6; ++i)
        capped[i] = {10 + i, utf8_literal("Tag"), i < 5, true};
    content.tags = {capped, 6};
    check(forum_post_update(&scene, content, viewport, &cache) &&
              !forum_post_focus(&scene, {ForumPostElement::tags, 5}) &&
              forum_post_focus(&scene, {ForumPostElement::tags, 0}),
          "forum_scene_five_tag_cap_blocks_sixth_keeps_selected_removable");
    capped[0].selected = false;
    check(forum_post_update(&scene, content, viewport, &cache) &&
              forum_post_focus(&scene, {ForumPostElement::tags, 5}),
          "forum_scene_deselecting_tag_reenables_sixth_chip");
    content.tags = {tags, 3};
    content.editable = content.can_accept = false;
    check(forum_post_update(&scene, content, viewport, &cache) &&
              !forum_post_focus(&scene, {ForumPostElement::title, 0}) &&
              !forum_post_focus(&scene, {ForumPostElement::body, 0}) &&
              !forum_post_focus(&scene, {ForumPostElement::tags, 0}) &&
              !forum_post_focus(&scene, {ForumPostElement::accept, 0}) &&
              forum_post_focus(&scene, {ForumPostElement::cancel, 0}) &&
              utf8_equal(scene.content.fields[1].text, content.fields[1].text),
          "forum_scene_busy_preserves_both_drafts_readonly_fields_tags_primary_cancel_works");
    content.editable = content.can_accept = content.uncertain = true;
    content.accept = utf8_literal("Retry");
    content.inspect = utf8_literal("View posts");
    content.risk = utf8_literal("Post anyway");
    const auto stale = scene.elements[static_cast<u32>(ForumPostElement::accept)];
    check(forum_post_update(&scene, content, viewport, &cache) &&
              !ui_widget_manager_resolve(scene.widgets, stale) &&
              !forum_post_focus(&scene, {ForumPostElement::accept, 0}) &&
              forum_post_focus(&scene, {ForumPostElement::inspect, 0}) &&
              forum_post_focus(&scene, {ForumPostElement::risk, 0}),
          "forum_scene_ambiguous_retry_requires_acknowledgment_stale_generation_rejected");
    content.risk_acknowledged = true;
    check(forum_post_update(&scene, content, viewport, &cache) &&
              forum_post_focus(&scene, {ForumPostElement::accept, 0}),
          "forum_scene_explicit_risk_ack_enables_retry");
    const UiRect sizes[] = {{0, 0, 320, 360}, {0, 0, 640, 600}, {0, 0, 96, 64}};
    for (auto size : sizes) {
        check(forum_post_update(&scene, content, size, &cache) &&
                  forum_post_focus(&scene, {ForumPostElement::accept, 0}),
              "forum_scene_narrow_short_reveal_primary");
        UiRect anchor{};
        check(ui_widget_manager_context_anchor(scene.widgets, &anchor) &&
                  forum_post_hit_test(scene, (anchor.left + anchor.right) * .5f,
                                      (anchor.top + anchor.bottom) * .5f)
                          .element == ForumPostElement::accept,
              "forum_scene_revealed_primary_pointer_hit");
        UiLayoutDiagnostic diagnostics[64]{};
        check(ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 64}).clean(),
              "forum_scene_narrow_short_layout_clean");
        check(forum_post_scroll(&scene, -10000), "forum_scene_panel_scroll_clamped");
    }
    content.uncertain = false;
    content.risk = {};
    content.inspect = {};
    content.accept = utf8_literal("Create post");
    check(forum_post_update(&scene, content, viewport, &cache) &&
              forum_post_focus(&scene, {ForumPostElement::title, 0}),
          "forum_scene_title_reveal");
    TextFormEditGeometry geometry[2]{};
    for (u32 i = 0; i < 2; ++i)
        geometry[i].text_rectangle =
            forum_post_node(scene, i ? ForumPostElement::body : ForumPostElement::title)->content_rectangle;
    const Theme themes[] = {kDefaultTheme, kLightTheme};
    for (const auto& theme : themes) {
        UiTextCommand commands[40]{};
        UiTextCommandStream text{};
        UiTextOutput output{&text, nullptr};
        bool title_single = false, body_multi = false;
        bool ok = ui_text_command_stream_initialize(&text, {commands, 40}) &&
                  forum_post_text_output(scene, geometry, theme, &output);
        for (u32 i = 0; ok && i < text.count; ++i) {
            const auto& command = text.storage[i];
            if (command.owner.handle.slot == scene.elements[static_cast<u32>(ForumPostElement::title)].slot)
                title_single = command.wrapping == TextWrapping::no_wrap;
            if (command.owner.handle.slot == scene.elements[static_cast<u32>(ForumPostElement::body)].slot)
                body_multi = command.wrapping == TextWrapping::wrap;
        }
        check(ok && title_single && body_multi,
              "forum_scene_unicode_two_fields_dark_light_singleline_title_multiline_body");
    }
    UiAttachmentDraftView files[kUiAttachmentDraftMaximum]{};
    for (u32 i = 0; i < kUiAttachmentDraftMaximum; ++i)
        files[i] = {1001 + i,
                    utf8_literal("Review photograph 日本語.png"),
                    utf8_literal("Uploading · 25%"),
                    {},
                    400,
                    100,
                    {},
                    UiAttachmentDraftPhase::uploading,
                    false,
                    true,
                    false,
                    true,
                    false};
    files[1].phase = UiAttachmentDraftPhase::failed;
    files[1].status = utf8_literal("Upload failed. Retry this file.");
    files[1].can_retry = true;
    files[2].description = utf8_literal("Original photograph description");
    content.attachments = {files, kUiAttachmentDraftMaximum};
    content.add_files = utf8_literal("Add files");
    content.attachment_status = utf8_literal("10 / 10 files");
    content.can_add_files = false;
    check(forum_post_update(&scene, content, viewport, &cache), "forum_scene_attachment_tray_shared_tree");
    auto* first = ui_widget_manager_layout_node(scene.widgets, scene.attachment_list.parts[0][0]);
    auto* second = ui_widget_manager_layout_node(scene.widgets, scene.attachment_list.parts[1][0]);
    check(first && second && first->rectangle.top == second->rectangle.top &&
              first->rectangle.right < second->rectangle.left,
          "forum_scene_wide_attachment_cards_two_columns");
    const auto footer = forum_post_node(scene, ForumPostElement::actions)->rectangle;
    check(forum_post_focus(
              &scene, {ForumPostElement::attachments, 0, files[9].id, UiAttachmentDraftAction::remove, {}}),
          "forum_scene_attachment_keyboard_reveal_last_remove");
    UiRect attachment_anchor{};
    check(ui_widget_manager_context_anchor(scene.widgets, &attachment_anchor) &&
              forum_post_hit_test(scene, (attachment_anchor.left + attachment_anchor.right) * .5f,
                                  (attachment_anchor.top + attachment_anchor.bottom) * .5f)
                      .attachment_id == files[9].id &&
              ui_rect_equal(footer, forum_post_node(scene, ForumPostElement::actions)->rectangle),
          "forum_scene_attachment_scroll_preserves_persistent_footer_and_exact_hit_id");
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache),
          "forum_scene_narrow_attachment_update");
    first = ui_widget_manager_layout_node(scene.widgets, scene.attachment_list.parts[0][0]);
    second = ui_widget_manager_layout_node(scene.widgets, scene.attachment_list.parts[1][0]);
    check(first && second && first->rectangle.left == second->rectangle.left &&
              first->rectangle.bottom < second->rectangle.top,
          "forum_scene_narrow_attachment_cards_one_column");
    check(forum_post_scroll(&scene, -10000), "forum_scene_attachment_scrollbar_top");
    const auto top_bar = forum_post_scrollbar(scene);
    const auto* scrolling_content = forum_post_node(scene, ForumPostElement::content);
    const auto narrow_footer = forum_post_node(scene, ForumPostElement::actions)->rectangle;
    check(top_bar.visible && top_bar.maximum_scroll > 0 && scrolling_content &&
              top_bar.track.left >= scrolling_content->rectangle.right &&
              top_bar.track.bottom <= narrow_footer.top &&
              ui_rect_equal(ui_rect_intersection(top_bar.thumb, top_bar.track), top_bar.thumb),
          "forum_scene_ten_files_themed_scrollbar_in_padding_above_persistent_footer");
    check(forum_post_scroll(&scene, 10000) && forum_post_scrollbar(scene).thumb.top > top_bar.thumb.top &&
              ui_rect_equal(narrow_footer, forum_post_node(scene, ForumPostElement::actions)->rectangle),
          "forum_scene_attachment_scrollbar_tracks_clamped_content_scroll_without_moving_footer");
    PaintStream overflow_paint{};
    bool overflow_painted = paint_stream_initialize(&overflow_paint);
    const auto thumb = forum_post_scrollbar(scene).thumb;
    for (const auto& theme : themes) {
        overflow_painted = paint_stream_reset(&overflow_paint, 320, 600) && overflow_painted;
        overflow_paint.filter_scrollbars = true;
        overflow_paint.scrollbar_viewport = {};
        overflow_painted = forum_post_paint(scene, geometry, theme, &overflow_paint) &&
                           overflow_paint.filter_scrollbars && !overflow_paint.rejected && overflow_painted;
        bool thumb_found = false;
        for (u32 quad = 0; quad < overflow_paint.count; ++quad) {
            const auto& painted = overflow_paint.quads[quad];
            thumb_found = thumb_found ||
                          ui_rect_equal({painted.left, painted.top, painted.right, painted.bottom}, thumb);
        }
        overflow_painted = overflow_painted && thumb_found;
    }
    check(overflow_painted,
          "forum_scene_overflow_thumb_paints_dark_light_despite_covered_shell_filter_and_restores_it");
    paint_stream_release(&overflow_paint);
    check(
        forum_post_focus(
            &scene, {ForumPostElement::attachments, 0, files[1].id, UiAttachmentDraftAction::retry, {}}) &&
            !forum_post_focus(
                &scene,
                {ForumPostElement::attachments, 0, files[0].id, UiAttachmentDraftAction::retry, {}}),
        "forum_scene_retry_focus_only_failed_file");
    const auto stale_attachment = forum_post_focused(scene);
    const auto saved_id = files[1].id;
    files[1].id = 2002;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache) &&
              !forum_post_focus(&scene, stale_attachment),
          "forum_scene_stale_attachment_handle_generation_and_id_rejected");
    files[1].id = saved_id;
    content.editable = false;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache) &&
              !forum_post_focus(
                  &scene,
                  {ForumPostElement::attachments, 0, files[1].id, UiAttachmentDraftAction::retry, {}}) &&
              !forum_post_focus(
                  &scene,
                  {ForumPostElement::attachments, 0, files[1].id, UiAttachmentDraftAction::remove, {}}),
          "forum_scene_upload_locked_actions_disabled");
    content.editable = true;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache),
          "forum_scene_attachment_warm_baseline");
    const auto attachment_revision = scene.revision, attachment_layout = scene.widgets.layout.revision,
               attachment_measures = cache.backend_calls;
    bool attachment_stable = true;
    for (u32 i = 0; i < 64; ++i)
        attachment_stable =
            forum_post_update(&scene, content, {0, 0, 320, 600}, &cache) && attachment_stable;
    check(attachment_stable && scene.revision == attachment_revision &&
              scene.widgets.layout.revision == attachment_layout &&
              cache.backend_calls == attachment_measures,
          "forum_scene_64_attachment_updates_reuse_layout_and_measurement");
    files[0].uploaded_bytes = 200;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache) &&
              scene.revision != attachment_revision && scene.widgets.layout.revision == attachment_layout &&
              cache.backend_calls == attachment_measures,
          "forum_scene_progress_only_invalidates_paint_without_layout_or_shape");
    const Utf8View wrapped_statuses[]{
        utf8_literal("Image · 4208 bytes · Waiting to upload. Your file will stay in this draft while the "
                     "upload is prepared."),
        utf8_literal("Image · 4208 bytes · Uploading... (50%). The upload is still running and this "
                     "detailed status wraps across lines."),
        utf8_literal("Image · Source changed after selection. Remove this file and choose the current "
                     "version before creating your post.")};
    content.attachments = {files, 1};
    const UiRect wrapped_sizes[] = {{0, 0, 640, 600}, {0, 0, 320, 600}};
    for (const auto size : wrapped_sizes) {
        for (u32 state = 0; state < 3; ++state) {
            files[0].status = wrapped_statuses[state];
            files[0].phase = state == 0   ? UiAttachmentDraftPhase::waiting
                             : state == 1 ? UiAttachmentDraftPhase::uploading
                                          : UiAttachmentDraftPhase::failed;
            files[0].can_retry = state == 2;
            const bool updated = forum_post_update(&scene, content, size, &cache);
            const auto& list = scene.attachment_list;
            const auto* header = ui_widget_manager_layout_node(scene.widgets, list.parts[0][1]);
            const auto* status = ui_widget_manager_layout_node(scene.widgets, list.parts[0][5]);
            const auto* actions = ui_widget_manager_layout_node(scene.widgets, list.parts[0][6]);
            const auto* card = ui_widget_manager_layout_node(scene.widgets, list.parts[0][0]);
            const auto picture = ui_attachment_draft_list_preview(list, 0);
            const auto progress = ui_attachment_draft_list_progress(list, 0);
            const bool progressing = ui_attachment_draft_progress_visible(files[0]);
            check(updated && header && status && actions && card &&
                      ui_rect_height(status->content_rectangle) > 20 &&
                      header->rectangle.bottom >= status->rectangle.bottom &&
                      actions->rectangle.top >= status->rectangle.bottom + list.style.gap &&
                      card->content_rectangle.bottom >= actions->rectangle.bottom &&
                      (progressing ? ui_rect_has_area(progress) &&
                                         progress.top >= status->rectangle.bottom + list.style.gap &&
                                         progress.top >= picture.bottom + list.style.gap &&
                                         actions->rectangle.top >= progress.bottom + list.style.gap &&
                                         ui_rect_height(progress) == list.style.progress_height
                                   : !ui_rect_has_area(progress)),
                  "forum_scene_wrapped_waiting_uploading_source_changed_status_progress_and_controls_never_"
                  "overlap");
        }
    }
    files[0].phase = UiAttachmentDraftPhase::waiting;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache),
          "forum_scene_progress_presence_layout_baseline");
    const auto waiting_layout = scene.widgets.layout.revision;
    files[0].phase = UiAttachmentDraftPhase::ready;
    check(forum_post_update(&scene, content, {0, 0, 320, 600}, &cache) &&
              scene.widgets.layout.revision != waiting_layout &&
              !ui_rect_has_area(ui_attachment_draft_list_progress(scene.attachment_list, 0)),
          "forum_scene_phase_only_progress_removal_reflows_reserved_band");
    content.attachments = {};
    content.add_files = {};
    content.attachment_status = {};
    check(forum_post_update(&scene, content, viewport, &cache),
          "forum_scene_attachment_detach_retains_two_fields");
    const auto retained = scene.revision;
    content.fields[0].caret_utf16 -= 1;
    check(!forum_post_update(&scene, content, viewport, &cache) && scene.revision == retained &&
              scene.active,
          "forum_scene_split_surrogate_rejected_without_mutating_scene");
    forum_post_release(&scene);
    check(!scene.storage.arena.base && !scene.widgets.count, "forum_scene_release_lazy_storage");
}
} // namespace maicord

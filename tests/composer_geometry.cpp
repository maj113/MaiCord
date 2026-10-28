#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

namespace maicord {

void test_composer_geometry(void (*check)(bool, const char*) noexcept) noexcept {
    {
        UiScene shell{};
        UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
        bool responsive = ui_scene_initialize_bootstrap(&shell);
        const float widths[]{632.f, 872.f, 871.5f, 1280.f, 672.f, 872.f};
        for (const float width : widths) {
            responsive &=
                ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, width, 800.f);
            const auto* center = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::content));
            const auto* members = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar));
            const auto* composer = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::composer));
            responsive &= center && ui_rect_width(center->rectangle) >= kUiShellMinimumMessageWidthDips &&
                          (members != nullptr) == (width >= 872.f) &&
                          ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_toggle)) &&
                          composer &&
                          ui_scene_composer_width(metrics, width) ==
                              ui_rect_width(ui_scene_content_rectangle(shell, UiSemantic::composer));
            const u32 passes = shell.layout_passes, rebuilds = shell.layout_rebuilds;
            responsive &=
                ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, width, 800.f) &&
                passes == shell.layout_passes && rebuilds == shell.layout_rebuilds;
        }
        check(
            responsive && metrics.flags == kGuildChannelShellLayoutMetrics.flags &&
                metrics.member_sidebar_width == 240.f,
            "shell_narrow_member_transitions_preserve_message_minimum_restore_pane_and_retain_preferences");
        metrics.revision++;
        metrics.guild_rail_width = 80.f;
        metrics.navigation_width = 260.f;
        metrics.navigation_gutter_before = 5.f;
        metrics.content_gutter_before = 12.f;
        metrics.member_sidebar_width = 225.f;
        bool custom =
            ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 901.5f, 800.f) &&
            !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar)) &&
            ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 902.f, 800.f);
        const auto* center = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::content));
        const auto* custom_composer = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::composer));
        custom &= center && center->rectangle.left == 357.f && ui_rect_width(center->rectangle) == 320.f &&
                  ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar)) &&
                  custom_composer &&
                  ui_scene_composer_width(metrics, 902.f) ==
                      ui_rect_width(ui_scene_content_rectangle(shell, UiSemantic::composer));
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_members);
        metrics.revision++;
        check(
            custom &&
                ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 1280.f, 800.f) &&
                !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar)),
            "shell_responsive_member_boundary_honors_custom_gutters_widths_and_explicit_hidden_preference");
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_navigation);
        metrics.revision++;
        bool hidden_navigation =
            ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics, 320.f, 800.f);
        const auto* narrow_composer = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::composer));
        hidden_navigation &=
            narrow_composer && ui_scene_composer_width(metrics, 320.f) ==
                                   ui_rect_width(ui_scene_content_rectangle(shell, UiSemantic::composer));
        const float shell_inset = 320.f - ui_scene_composer_width(metrics, 320.f);
        const float inline_boundary = message_composer_scene_minimum_inline_width();
        const float toolbar_boundary_widths[]{160.f, 163.5f, 164.f, inline_boundary - .5f, inline_boundary};
        for (const float composer_width : toolbar_boundary_widths) {
            const float shell_width = shell_inset + composer_width;
            hidden_navigation &= ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, metrics,
                                                              shell_width, 800.f);
            const auto* boundary_composer =
                ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::composer));
            hidden_navigation &=
                boundary_composer && ui_scene_composer_width(metrics, shell_width) == composer_width &&
                ui_rect_width(ui_scene_content_rectangle(shell, UiSemantic::composer)) == composer_width;
        }
        metrics.flags = static_cast<u16>(metrics.flags & ~ui_shell_layout_composer);
        check(hidden_navigation && ui_scene_composer_width(metrics, 320.f) == 0.f &&
                  ui_scene_composer_width(metrics, 0.f) == 0.f,
              "shell_composer_width_matches_actual_layout_for_custom_gutters_hidden_navigation_and_hidden_"
              "composer");
        ui_scene_release(&shell);
    }
    {
        TextStyleSpan spans[256]{};
        const auto source = utf8_literal(
            "**bold *both* end** __under__ ~~strike~~ `code <#42>`\n```cpp\nint x = 1;\n```\n<@&99> <#42> "
            "<:party:111> **after**\n- *item*\nhttps://discord.com/channels/1/2/3 **linkend**");
        const u32 count = message_markdown_source_styles(source, {spans, 256});
        const auto style_at = [&](Utf8View needle) noexcept {
            for (u32 i = 0; i + needle.bytes <= source.bytes; ++i) {
                if (!utf8_equal({source.data + i, needle.bytes}, needle))
                    continue;
                for (u32 s = 0; s < count; ++s)
                    if (i >= spans[s].byte_offset && i < spans[s].byte_offset + spans[s].byte_count)
                        return spans[s].flags;
                return 0u;
            }
            return 0u;
        };
        check(count && (style_at(utf8_literal("bold")) & text_style_bold) &&
                  (style_at(utf8_literal("both")) & (text_style_bold | text_style_italic)) ==
                      (text_style_bold | text_style_italic) &&
                  (style_at(utf8_literal("under")) & text_style_underline) &&
                  (style_at(utf8_literal("strike")) & text_style_strikethrough) &&
                  (style_at(utf8_literal("code")) & text_style_monospace) &&
                  (style_at(utf8_literal("int x")) & text_style_code_block) &&
                  (style_at(utf8_literal("<@&99>")) & text_style_mention) &&
                  (style_at(utf8_literal("after")) & text_style_bold) &&
                  (style_at(utf8_literal("item")) & text_style_italic) &&
                  (style_at(utf8_literal("linkend")) & text_style_bold),
              "composer_shared_markdown_styles_preserve_source_coordinates_across_blocks_and_entities");
        check(style_at(utf8_literal("**bold")) == 0 &&
                  !(style_at(utf8_literal("<#42>`")) & text_style_mention),
              "composer_editable_delimiters_and_mentions_inside_code_remain_literal");
    }
    char storage[128]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    const bool ready = message_composer_initialize(&model, {storage, sizeof(storage)}) &&
                       message_composer_scene_initialize_owned(&scene);
    check(ready, "composer_compact_geometry_fixture");
    if (!ready) {
        message_composer_scene_release(&scene);
        return;
    }

    const Utf8View content[]{{}, utf8_literal("hello"), utf8_literal("one\ntwo\nthree")};
    bool compact = true;
    bool accessories = true;
    bool clean = true;
    bool separate_typing = true;
    bool primary_readable = true;
    const float widths[]{280.0f, 304.0f, 740.0f};
    for (const Utf8View text : content) {
        message_composer_reset(&model, 42, false);
        if (text.bytes != 0 &&
            message_composer_insert_utf8(&model, text) != MessageComposerResult::changed) {
            clean = false;
        }
        for (const float width : widths) {
            const float base =
                message_composer_preferred_height(model, message_composer_scene_text_width(width));
            const float toolbar_height = message_composer_scene_toolbar_height(width);
            compact = compact && (text.bytes > 5 || base == 48.0f);
            // Show/hide typing and reply rows without stealing space from the editor.
            for (u32 mode = 0; mode < 9; ++mode) {
                const bool reply = (mode & 1u) != 0;
                const bool typing = (mode & 2u) != 0;
                const bool attachments = (mode & 4u) != 0;
                const float height =
                    message_composer_scene_preferred_height(model, width, reply, typing, attachments);
                compact = compact && height == base + toolbar_height + (reply ? 32.0f : 0.0f) +
                                                   (typing ? 24.0f : 0.0f) + (attachments ? 32.0f : 0.0f);
                const UiRect viewport{10, 20, 10 + width, 20 + height};
                clean = clean && message_composer_scene_update(
                                     &scene, &model, viewport, true, reply ? 77 : 0,
                                     reply ? utf8_literal("Replying to someone") : Utf8View{}, nullptr,
                                     typing ? utf8_literal("Someone is typing…") : Utf8View{},
                                     {attachments ? utf8_literal("photo.png - uploading 50%") : Utf8View{},
                                      false, true})
                                     .ok();
                const UiLayoutNode* edit =
                    message_composer_scene_layout_node(scene, MessageComposerControl::edit);
                const UiLayoutNode* surface = ui_widget_manager_layout_node(scene.widgets, scene.surface);
                const UiLayoutNode* indicator =
                    message_composer_scene_layout_node(scene, MessageComposerControl::typing_indicator);
                separate_typing =
                    separate_typing && surface && indicator &&
                    ui_rect_height(surface->rectangle) ==
                        base + toolbar_height + (reply ? 32.0f : 0.0f) + (attachments ? 32.0f : 0.0f) &&
                    (!typing ||
                     (indicator->rectangle.top == viewport.top &&
                      indicator->rectangle.bottom + 4.0f == surface->rectangle.top &&
                      !ui_rect_has_area(ui_rect_intersection(indicator->rectangle, surface->rectangle))));
                compact = compact && edit && ui_rect_height(edit->rectangle) == base - 8.0f;
                // The 632-DIP window minimum leaves a 304-DIP composer: GIF and
                // sticker fold into the emoji picker so the editor keeps one row.
                if (width == 304.f && text.bytes == 0)
                    primary_readable &= edit && toolbar_height == 0.f &&
                                        ui_rect_width(edit->content_rectangle) >= 156.f &&
                                        ui_rect_height(edit->content_rectangle) >= 28.f &&
                                        !message_composer_scene_control_visible(scene, MessageComposerControl::gif) &&
                                        !message_composer_scene_control_visible(scene, MessageComposerControl::sticker) &&
                                        !message_composer_scene_focus_control(&scene, MessageComposerControl::gif);
                const MessageComposerControl buttons[]{
                    MessageComposerControl::add_attachment, MessageComposerControl::gif,
                    MessageComposerControl::sticker, MessageComposerControl::emoji,
                    MessageComposerControl::send};
                for (const auto control : buttons) {
                    const UiLayoutNode* button = message_composer_scene_layout_node(scene, control);
                    if (!message_composer_scene_control_visible(scene, control))
                        continue;
                    const bool editor_row =
                        toolbar_height == 0.f || control == MessageComposerControl::add_attachment;
                    accessories =
                        accessories && edit && button && ui_rect_height(button->rectangle) == 32.0f &&
                        ui_rect_equal(ui_rect_intersection(button->rectangle, button->clip_rectangle),
                                      button->rectangle) &&
                        (editor_row ? button->rectangle.top >= edit->rectangle.top &&
                                          button->rectangle.bottom <= edit->rectangle.bottom &&
                                          button->rectangle.top + button->rectangle.bottom ==
                                              edit->rectangle.top + edit->rectangle.bottom
                                    : button->rectangle.top >= edit->rectangle.bottom + 4.f);
                }
                accessories &= edit && ui_rect_width(edit->content_rectangle) >= 120.f;
                UiLayoutDiagnostic diagnostics[16]{};
                clean = clean && ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 16}).clean();
            }
        }
    }
    check(compact, "composer_single_line_editable_row_48dips_without_double_padding");
    check(accessories, "composer_buttons_fixed_with_readable_editor_for_text_reply_and_typing");
    check(clean, "composer_compact_multiline_and_auxiliary_layouts_validate");
    check(separate_typing, "composer_typing_above_separate_fixed_height_surface");
    check(primary_readable,
          "composer_shipping_304dip_width_keeps_one_editor_row_by_folding_gif_and_sticker");

    {
        const Utf8View draft =
            utf8_literal("Wide caf\xC3\xA9 \xF0\x9F\x92\xAC text\nSecond readable line\nThird line");
        message_composer_reset(&model, 42, false);
        bool usable =
            message_composer_insert_utf8(&model, draft) == MessageComposerResult::changed &&
            message_composer_set_selection(&model, 5, draft.bytes - 4) == MessageComposerResult::changed;
        const u32 document_revision = model.revision;
        const u64 document_identity = model.document_identity;
        const u32 anchor = model.anchor, caret = model.caret;
        const auto* widget_storage = scene.widgets.storage.data;
        const auto* node_storage = scene.widgets.layout.storage.data;
        const usize arena_used = scene.owned_storage.offset;
        bool rows = true, routing = true, preserves = true, stable = true;
        const MessageComposerControl buttons[]{MessageComposerControl::add_attachment,
                                               MessageComposerControl::gif, MessageComposerControl::sticker,
                                               MessageComposerControl::emoji, MessageComposerControl::send};
        const float inline_boundary = message_composer_scene_minimum_inline_width();
        const float compact_boundary = message_composer_scene_minimum_compact_width();
        usable &= compact_boundary < 304.f && compact_boundary > 232.f;
        const float responsive_widths[]{304.f,           279.5f, 232.f, 160.f,
                                        163.5f,          164.f,  280.f, inline_boundary - .5f,
                                        inline_boundary, 232.f,  740.f, 304.f};
        for (const float width : responsive_widths) {
            const bool wrapped = width < compact_boundary;
            const bool folded = !wrapped && width < inline_boundary;
            const float toolbar_height = width < 164.f ? 72.f : (wrapped ? 36.f : 0.f);
            for (u32 mode = 0; mode < 8; ++mode) {
                const bool reply = (mode & 1u) != 0, typing = (mode & 2u) != 0;
                const bool attachments = (mode & 4u) != 0;
                const float height =
                    message_composer_scene_preferred_height(model, width, reply, typing, attachments);
                const UiRect responsive_viewport{10.f, 20.f, 10.f + width, 20.f + height};
                const auto update = [&]() noexcept {
                    return message_composer_scene_update(
                        &scene, &model, responsive_viewport, true, reply ? 77 : 0,
                        reply ? utf8_literal("Replying to author") : Utf8View{}, nullptr,
                        typing ? utf8_literal("Someone is typing…") : Utf8View{},
                        {attachments ? utf8_literal("photo.png - ready") : Utf8View{}, true, true});
                };
                usable &= update().ok();
                const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
                const auto* surface = ui_widget_manager_layout_node(scene.widgets, scene.surface);
                const auto* gif = message_composer_scene_layout_node(scene, MessageComposerControl::emoji);
                const auto* send = message_composer_scene_layout_node(scene, MessageComposerControl::send);
                usable &= edit &&
                          ui_rect_width(edit->content_rectangle) >=
                              (width < 232.f ? 96.f : wrapped ? 168.f : folded ? 120.f : 156.f) &&
                          ui_rect_height(edit->content_rectangle) >= 60.f;
                rows &= edit && gif && send && surface &&
                        (wrapped ? gif->rectangle.top >= edit->rectangle.bottom + 4.f
                                 : gif->rectangle.bottom <= edit->rectangle.bottom) &&
                        send->rectangle.top == gif->rectangle.top + (width < 164.f ? 36.f : 0.f) &&
                        send->rectangle.bottom == gif->rectangle.bottom + (width < 164.f ? 36.f : 0.f) &&
                        send->rectangle.bottom <= surface->content_rectangle.bottom;
                for (const auto control : buttons) {
                    const auto* button = message_composer_scene_layout_node(scene, control);
                    if (folded && (control == MessageComposerControl::gif || control == MessageComposerControl::sticker)) {
                        routing &= !message_composer_scene_control_visible(scene, control) &&
                                   !message_composer_scene_focus_control(&scene, control);
                        continue;
                    }
                    routing &=
                        button && ui_rect_width(button->rectangle) == 36.f &&
                        ui_rect_height(button->rectangle) == 32.f &&
                        ui_rect_equal(button->rectangle,
                                      ui_rect_intersection(button->rectangle, button->clip_rectangle)) &&
                        !ui_rect_has_area(ui_rect_intersection(button->rectangle, edit->rectangle)) &&
                        message_composer_scene_hit_test(
                            scene, (button->rectangle.left + button->rectangle.right) * .5f,
                            (button->rectangle.top + button->rectangle.bottom) * .5f) == control &&
                        message_composer_scene_focus_control(&scene, control) &&
                        message_composer_scene_focused_control(scene) == control;
                }
                routing &= message_composer_scene_focus_edit(&scene) &&
                           message_composer_scene_focused_control(scene) == MessageComposerControl::edit &&
                           ui_widget_manager_next_focus(scene.widgets, scene.edit, false, false).slot ==
                               (folded ? scene.emoji : scene.gif).slot;
                UiLayoutDiagnostic diagnostics[32]{};
                rows &= ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean();
                const float chrome = message_composer_scene_chrome_height(reply, typing, attachments);
                const float measured = message_composer_scene_measured_height(scene, 60.f);
                rows &= measured == 80.f + toolbar_height + chrome &&
                        message_composer_scene_measured_height(scene, 1000.f) ==
                            kMessageComposerMaximumHeight + toolbar_height + chrome;
                scene.measured_row_height = 80.f;
                rows &= message_composer_scene_cached_height(scene, reply, typing, attachments, width) ==
                        measured;
                const u32 layout_passes = scene.layout_passes, projection_passes = scene.projection_passes;
                const u64 display_signature = scene.display_signature;
                for (u32 frame = 0; frame < 64; ++frame) {
                    const auto warmed = update();
                    stable &= warmed.ok() && !warmed.changed && scene.layout_passes == layout_passes &&
                              scene.projection_passes == projection_passes &&
                              scene.display_signature == display_signature &&
                              message_composer_scene_focused_control(scene) == MessageComposerControl::edit;
                }
                u32 composition_first = 0, composition_last = 0;
                TextCompositionSpan accent_span{};
                accent_span.start = 5;
                accent_span.end = 9;
                preserves &= model.revision == document_revision &&
                             model.document_identity == document_identity && model.anchor == anchor &&
                             model.caret == caret && utf8_equal(message_composer_text(model), draft) &&
                             utf8_equal(message_composer_scene_display_text(scene), draft) &&
                             scene.display_anchor == anchor && scene.display_caret == caret &&
                             message_composer_scene_composition_range(
                                 scene, accent_span, &composition_first, &composition_last) &&
                             composition_first == 5 && composition_last == 9 &&
                             scene.widgets.count == kMessageComposerWidgetCapacity &&
                             scene.widgets.storage.data == widget_storage &&
                             scene.widgets.layout.storage.data == node_storage &&
                             scene.owned_storage.offset == arena_used;
            }
        }
        check(usable, "composer_160dip_and_232dip_unicode_multiline_keep_readable_editor_through_"
                      "breakpoints_and_restoration");
        check(rows, "composer_compact_toolbar_and_auxiliary_rows_size_once_without_overlap_or_clipping");
        check(routing, "composer_visible_accessories_retain_targets_focus_and_hit_routes_folded_ones_none");
        check(preserves, "composer_width_changes_preserve_unicode_document_selection_composition_and_"
                         "fourteen_widget_storage");
        check(stable,
              "composer_compact_64_warm_updates_keep_focus_without_layout_projection_or_allocation");
    }

    message_composer_reset(&model, 42, false);
    message_composer_insert_utf8(&model, utf8_literal("draft"));
    const UiRect minimum_reply{0, 0, 304, 116};
    bool mention = message_composer_scene_update(&scene, &model, minimum_reply, true, 77,
                                                 utf8_literal("Replying to a long original author name"),
                                                 nullptr, {}, {}, false, true, true)
                       .ok();
    const auto* reply_toggle =
        message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
    const auto* reply_banner = ui_widget_manager_layout_node(scene.widgets, scene.reply);
    const auto* reply_cancel =
        message_composer_scene_layout_node(scene, MessageComposerControl::cancel_reply);
    const UiRect reply_text = message_composer_scene_reply_text_rectangle(scene);
    mention &=
        reply_toggle && reply_banner && reply_cancel &&
        reply_text.right <= reply_toggle->rectangle.left - 8.f && ui_rect_width(reply_text) > 80.f &&
        ui_rect_width(reply_cancel->rectangle) == 28.f && ui_rect_height(reply_cancel->rectangle) == 28.f &&
        !ui_rect_has_area(ui_rect_intersection(reply_toggle->rectangle, reply_cancel->rectangle)) &&
        ui_rect_equal(reply_cancel->rectangle,
                      ui_rect_intersection(reply_cancel->rectangle, reply_cancel->clip_rectangle)) &&
        ui_rect_equal(reply_toggle->rectangle,
                      ui_rect_intersection(reply_toggle->rectangle, reply_toggle->clip_rectangle)) &&
        message_composer_scene_hit_test(scene, reply_toggle->rectangle.left + 50.f,
                                        reply_toggle->rectangle.top + 11.f) ==
            MessageComposerControl::reply_mention &&
        message_composer_scene_hit_test(scene, reply_cancel->rectangle.left + 14.f,
                                        reply_cancel->rectangle.top + 14.f) ==
            MessageComposerControl::cancel_reply &&
        message_composer_scene_hit_test(scene, reply_text.left + 4.f, reply_banner->rectangle.top + 14.f) ==
            MessageComposerControl::none &&
        message_composer_scene_focus_control(&scene, MessageComposerControl::reply_mention) &&
        message_composer_scene_focused_control(scene) == MessageComposerControl::reply_mention &&
        ui_widget_manager_next_focus(scene.widgets, scene.reply_mention, false, false).slot ==
            scene.reply_cancel.slot &&
        ui_widget_manager_next_focus(scene.widgets, scene.reply_cancel, true, false).slot ==
            scene.reply_mention.slot &&
        !ui_widget_manager_focus(&scene.widgets, scene.reply) &&
        utf8_equal(message_composer_scene_reply_mention_label(scene), utf8_literal("ON"));
    const u32 mention_passes = scene.layout_passes, draft_revision = model.revision;
    const auto toggled = message_composer_scene_update(
        &scene, &model, minimum_reply, true, 77, utf8_literal("Replying to a long original author name"),
        nullptr, {}, {}, false, false, true);
    check(mention && toggled.ok() && toggled.changed && scene.layout_passes == mention_passes &&
              model.revision == draft_revision &&
              utf8_equal(message_composer_scene_reply_mention_label(scene), utf8_literal("OFF")),
          "composer_minimum_reply_mentions_have_separate_hit_focus_label_and_no_document_or_layout_work_on_"
          "toggle");
    bool stable_reply = true;
    for (u32 frame = 0; frame < 64; ++frame) {
        const auto stable = message_composer_scene_update(
            &scene, &model, minimum_reply, true, 77,
            utf8_literal("Replying to a long original author name"), nullptr, {}, {}, false, false, true);
        stable_reply &= stable.ok() && !stable.changed && scene.layout_passes == mention_passes;
    }
    check(stable_reply,
          "composer_reply_static_author_ping_cancel_64_updates_without_layout_or_document_changes");
    const bool focused_send = message_composer_scene_focus_control(&scene, MessageComposerControl::send);
    const auto restricted_reply = message_composer_scene_update(
        &scene, &model, minimum_reply, true, 77, utf8_literal("Replying to a long original author name"),
        nullptr, {}, {}, false, false, true, false, false);
    check(focused_send && restricted_reply.ok() && restricted_reply.changed &&
              !message_composer_scene_can_send(scene) &&
              !ui_widget_manager_input_enabled(scene.widgets, scene.send) &&
              message_composer_scene_focused_control(scene) == MessageComposerControl::none &&
              scene.layout_passes == mention_passes && model.revision == draft_revision &&
              utf8_equal(message_composer_text(model), utf8_literal("draft")) &&
              message_composer_scene_focus_control(&scene, MessageComposerControl::reply_mention) &&
              message_composer_scene_focus_control(&scene, MessageComposerControl::cancel_reply),
          "composer_withdrawn_reply_access_disables_send_retains_text_controls_and_reconciles_send_focus_"
          "without_relayout");
    const auto restored_access = message_composer_scene_update(
        &scene, &model, minimum_reply, true, 77, utf8_literal("Replying to a long original author name"),
        nullptr, {}, {}, false, false, true, false, true);
    check(restored_access.ok() && restored_access.changed && message_composer_scene_can_send(scene) &&
              ui_widget_manager_input_enabled(scene.widgets, scene.send) &&
              scene.layout_passes == mention_passes && model.revision == draft_revision &&
              message_composer_scene_focus_control(&scene, MessageComposerControl::reply_mention),
          "composer_reply_access_restoration_reenables_send_without_document_or_layout_changes");
    check(message_composer_scene_update(&scene, &model, minimum_reply, true, 77,
                                        utf8_literal("Editing message"), nullptr, {}, {}, false, false,
                                        false)
                  .ok() &&
              !message_composer_scene_reply_mention_label(scene).bytes &&
              !message_composer_scene_focus_control(&scene, MessageComposerControl::reply_mention) &&
              message_composer_scene_focused_control(scene) == MessageComposerControl::none &&
              message_composer_scene_focus_control(&scene, MessageComposerControl::cancel_reply),
          "composer_edit_banner_hides_reply_mention_and_reconciles_removed_toggle_focus");
    check(message_composer_scene_update(&scene, &model, {0, 0, 280, 116}, true, 77,
                                        utf8_literal("Replying to a message"), nullptr, {}, {}, false,
                                        false, true)
                  .ok() &&
              ui_rect_width(message_composer_scene_reply_text_rectangle(scene)) >= 140.f,
          "composer_narrow_reply_retains_author_space_with_ping_and_cancel_targets");
    check(message_composer_scene_focus_control(&scene, MessageComposerControl::cancel_reply) &&
              message_composer_scene_update(&scene, &model, {0, 0, 280, 84}, true).ok() &&
              !ui_widget_manager_input_enabled(scene.widgets, scene.reply_cancel) &&
              message_composer_scene_focused_control(scene) == MessageComposerControl::none,
          "composer_removed_reply_disables_cancel_and_reconciles_cancel_focus");

    GuildRecord guilds[1]{};
    u16 guild_order[1]{};
    ChannelRecord channels[2]{};
    u16 channel_order[2]{};
    u8 names[2048]{};
    ChatStateStore state{};
    const MessageComposerProjectionContext projection{nullptr, &state, 7};
    bool named =
        chat_state_store_initialize(&state, {guilds, 1}, {guild_order, 1}, {channels, 2},
                                       {channel_order, 2}, {names, sizeof(names)}) &&
        chat_state_store_upsert_channel(&state, {42, 7, 0, utf8_literal("general"), 0, 0, 0}).ok() &&
        chat_state_store_upsert_channel(&state,
                                           {43, 7, 0, utf8_literal("\xF0\x9F\x92\xAC-main"), 0, 0, 0})
            .ok();
    message_composer_reset(&model, 42, false);
    const UiRect viewport{0, 0, 740, 48};
    named = named &&
            message_composer_scene_update(&scene, &model, viewport, true, 0, {}, &projection).ok() &&
            utf8_equal(message_composer_scene_placeholder(scene), utf8_literal("Message #general"));
    message_composer_reset(&model, 43, false);
    named = named &&
            message_composer_scene_update(&scene, &model, viewport, true, 0, {}, &projection).ok() &&
            utf8_equal(message_composer_scene_placeholder(scene),
                       utf8_literal("Message #\xF0\x9F\x92\xAC-main"));
    named = named &&
            chat_state_store_upsert_channel(&state,
                                               {43, 7, 0, utf8_literal("\xF0\x9F\x92\xAC-chat"), 0, 0, 0})
                .ok() &&
            message_composer_scene_update(&scene, &model, viewport, true, 0, {}, &projection).changed &&
            utf8_equal(message_composer_scene_placeholder(scene),
                       utf8_literal("Message #\xF0\x9F\x92\xAC-chat"));
    message_composer_reset(&model, 404, false);
    named = named &&
            message_composer_scene_update(&scene, &model, viewport, true, 0, {}, &projection).ok() &&
            utf8_equal(message_composer_scene_placeholder(scene), utf8_literal("Message"));
    check(named, "composer_placeholder_channel_switch_unicode_rename_and_missing_channel");
    message_composer_reset(&model, 42, false);
    bool rich =
        message_composer_insert_utf8(&model, utf8_literal("**<#42>** `<#42>`")) ==
            MessageComposerResult::changed &&
        message_composer_scene_update(&scene, &model, viewport, true, 0, {}, &projection).ok() &&
        utf8_equal(message_composer_scene_display_text(scene), utf8_literal("**#general** `<#42>`"));
    bool mention_style = false, code_style = false;
    for (u32 s = 0; s < scene.display_style_count; ++s) {
        const auto span = scene.display_styles[s];
        mention_style |=
            span.byte_offset == 2 && span.byte_count == 8 &&
            (span.flags & (text_style_mention | text_style_bold)) == (text_style_mention | text_style_bold);
        code_style |= (span.flags & text_style_monospace) != 0 && !(span.flags & text_style_mention);
    }
    check(rich && mention_style && code_style && scene.projection_span_count == 1,
          "composer_resolved_channel_uses_shared_blue_bold_style_and_code_suppresses_projection");
    message_composer_reset(&model, 42, false);
    const UiRect attached_viewport{0, 0, 280, 116};
    bool attached =
        message_composer_scene_update(&scene, &model, attached_viewport, true, 0, {}, nullptr, {},
                                      {utf8_literal("photo.png - uploading 50%"), false, true})
            .ok() &&
        !message_composer_scene_can_send(scene) &&
        message_composer_scene_focus_control(&scene, MessageComposerControl::attachments);
    const u32 passes = scene.layout_passes;
    attached = attached &&
               message_composer_scene_update(&scene, &model, attached_viewport, true, 0, {}, nullptr, {},
                                             {utf8_literal("photo.png - ready"), true, true})
                   .changed &&
               scene.layout_passes == passes && message_composer_scene_can_send(scene);
    const auto next = ui_widget_manager_next_focus(scene.widgets, scene.attachments, false, false);
    attached = attached && next.slot == scene.add_attachment.slot;
    check(attached, "composer_attachment_only_ready_send_and_keyboard_without_progress_relayout");
    check(message_composer_scene_update(&scene, &model, attached_viewport, true, 0, {}, nullptr, {},
                                        {utf8_literal("photo.png - ready"), true, false})
                  .ok() &&
              !message_composer_scene_can_send(scene) &&
              !message_composer_scene_focus_control(&scene, MessageComposerControl::add_attachment) &&
              message_composer_scene_focus_control(&scene, MessageComposerControl::attachments),
          "composer_attachment_permission_loss_blocks_send_but_allows_removal");
    check(message_composer_scene_update(&scene, &model, viewport, true).ok() &&
              !message_composer_scene_can_send(scene) && !scene.widgets.focused.generation &&
              !message_composer_scene_attachment_label(scene).bytes,
          "composer_last_attachment_removal_clears_stale_focus_and_empty_send");
    const UiRect retained_viewport{0, 0, 304, 116};
    const u32 retained_layout = scene.layout_passes;
    bool retained = message_composer_scene_update(&scene, &model, retained_viewport, true, 77,
                                                  utf8_literal("Editing message"), nullptr, {},
                                                  {{}, false, false, true}, false, true, false)
                        .ok() &&
                    message_composer_scene_can_send(scene) &&
                    ui_widget_manager_input_enabled(scene.widgets, scene.send) &&
                    !message_composer_scene_attachment_label(scene).bytes &&
                    !ui_widget_manager_layout_node(scene.widgets, scene.attachments)->visible &&
                    message_composer_scene_focus_control(&scene, MessageComposerControl::send);
    const u32 retained_passes = scene.layout_passes;
    retained &=
        retained_passes > retained_layout &&
        message_composer_scene_update(&scene, &model, retained_viewport, true, 77,
                                      utf8_literal("Editing message"), nullptr, {}, {}, false, true, false)
            .ok() &&
        !message_composer_scene_can_send(scene) && scene.layout_passes == retained_passes &&
        !ui_widget_manager_input_enabled(scene.widgets, scene.send) && !scene.widgets.focused.generation;
    check(retained, "composer_empty_caption_retained_rich_content_enables_save_without_upload_row_and_lost_"
                    "content_disables_focus");
    check(message_composer_scene_update(
              &scene, &model, {0, 0, 304, 148}, true, 77, utf8_literal("Editing message"), nullptr, {},
              {utf8_literal("photo.png - uploading"), false, true, true}, false, true, false)
                  .ok() &&
              !message_composer_scene_can_send(scene) &&
              message_composer_scene_update(&scene, &model, retained_viewport, false, 77,
                                            utf8_literal("Editing message"), nullptr, {},
                                            {{}, false, false, true}, false, true, false)
                  .ok() &&
              !message_composer_scene_can_send(scene),
          "composer_retained_rich_content_does_not_bypass_new_upload_readiness_or_send_permission");

    UiScene status_shell{};
    NavigationView status_navigation{};
    ShellNavigationScene status_channels{};
    UiTextCommand status_commands[16]{};
    UiTextCommandStream status_stream{};
    const auto status_label = utf8_literal("Could not save caption. Try again.");
    bool status = ui_scene_initialize_bootstrap(&status_shell) &&
                  ui_scene_layout_with_metrics(&status_shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280, 800) &&
                  ui_text_command_stream_initialize(&status_stream, {status_commands, 16});
    u32 status_passes = 0;
    for (u32 severity = 0; severity < 2; ++severity) {
        const auto result =
            message_composer_scene_update(&scene, &model, {0, 0, 304, 108}, true, 0, {}, nullptr,
                                          status_label, {}, false, true, true, severity != 0);
        status &= result.ok() &&
                  (severity == 0 || (result.changed && scene.layout_passes == status_passes)) &&
                  shell_text_presentation_build(status_shell, state, status_navigation, kDefaultTheme,
                                                status_channels, &scene, nullptr, &status_stream);
        const auto expected = severity ? kDefaultTheme.notification : kDefaultTheme.text_primary;
        bool found = false;
        for (u32 i = 0; i < status_stream.count; ++i) {
            const auto& command = status_commands[i];
            if (utf8_equal(command.text, status_label)) {
                found = command.color.red == expected.red && command.color.green == expected.green &&
                        command.color.blue == expected.blue && command.color.alpha == expected.alpha;
            }
        }
        status &= found;
        status_passes = scene.layout_passes;
    }
    check(status, "composer_notice_severity_uses_theme_error_text_without_relayout");
    Theme banner_themes[]{kDefaultTheme, kLightTheme, kDefaultTheme};
    banner_themes[2].background = background_theme_defaults();
    banner_themes[2].text_secondary = {.72f, .81f, .76f, 1.f};
    const auto banner_label = utf8_literal("Editing original caption");
    bool readable_banner =
        message_composer_scene_update(&scene, &model, {0, 0, 304, 116}, true, 77, banner_label, nullptr, {},
                                      {{}, false, false, true}, false, true, false)
            .ok();
    for (const auto& theme : banner_themes) {
        readable_banner &= shell_text_presentation_build(status_shell, state, status_navigation, theme,
                                                         status_channels, &scene, nullptr, &status_stream);
        bool found = false;
        for (u32 i = 0; i < status_stream.count; ++i) {
            const auto& command = status_commands[i];
            if (utf8_equal(command.text, banner_label)) {
                const auto expected = theme.text_secondary;
                found = command.color.red == expected.red && command.color.green == expected.green &&
                        command.color.blue == expected.blue && command.color.alpha == expected.alpha;
            }
        }
        readable_banner &= found;
    }
    check(
        readable_banner,
        "composer_reply_and_edit_banner_use_readable_secondary_text_in_dark_light_and_custom_backgrounds");
    // The reply mention switch: "@ ON"/"@ OFF" with no button box; only
    // the icon and label take the link colour when pinging, muted when not.
    bool ping_rendered = true;
    for (u32 on = 0; on < 2; ++on) {
        const auto updated = message_composer_scene_update(&scene, &model, {0, 0, 304, 116}, true, 77,
                                                           utf8_literal("Replying to Fixture"), nullptr, {}, {},
                                                           false, on != 0, true);
        const auto ink = on ? kDefaultTheme.tokens.text.link : kDefaultTheme.tokens.text.muted;
        const auto same_color = [&](const Color& color) noexcept {
            return color.red == ink.red && color.green == ink.green && color.blue == ink.blue &&
                   color.alpha == ink.alpha;
        };
        const auto* toggle = message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
        const auto icon = message_composer_scene_reply_mention_icon(scene);
        PaintStream paint{};
        bool painted = updated.ok() && toggle && paint_stream_initialize(&paint) &&
                       paint_stream_reset(&paint, 400, 200) &&
                       shell_presentation_paint_reply_mention(scene, kDefaultTheme, &paint);
        u32 icons = 0, boxes = 0;
        for (u32 i = 0; painted && i < paint.count; ++i) {
            const auto& q = paint.quads[i];
            if (ui_icon_quad(q) && ui_icon_quad_data(q).icon == static_cast<u32>(UiIcon::mention) &&
                same_color(q.fill) &&
                ui_rect_equal({q.left, q.top, q.right, q.bottom}, icon))
                ++icons;
            else if (q.left <= toggle->rectangle.left && q.right >= toggle->rectangle.right)
                ++boxes;
        }
        paint_stream_release(&paint);
        painted = painted &&
                  shell_text_presentation_build(status_shell, state, status_navigation, kDefaultTheme,
                                                status_channels, &scene, nullptr, &status_stream);
        bool label = false;
        for (u32 i = 0; painted && i < status_stream.count; ++i) {
            const auto& command = status_commands[i];
            if (utf8_equal(command.text, on ? utf8_literal("ON") : utf8_literal("OFF")))
                label = same_color(command.color) && command.rectangle.left >= icon.right &&
                        (command.style_flags & text_style_bold) &&
                        command.horizontal_alignment == UiTextAlignment::start;
        }
        ping_rendered &= painted && icons == 1 && !boxes && label &&
                         ui_rect_width(toggle->rectangle) == kMessageComposerReplyMentionWidth;
    }
    check(ping_rendered, "composer_reply_mention_switch_is_a_boxless_tinted_at_icon_and_on_off_label");
    {
        UiResolvedThemeStyles styles{};
        styles.ready = true;
        styles.revision = styles.geometry_revision = styles.paint_revision = 1;
        check(message_composer_scene_minimum_inline_width(&styles) == 380.f &&
                  message_composer_scene_minimum_compact_width(&styles) == 264.f &&
                  message_composer_scene_text_width(304.f, &styles) == 160.f &&
                  message_composer_scene_toolbar_height(304.f, &styles) == 0.f &&
                  message_composer_scene_toolbar_height(160.f, &styles) == 72.f,
              "composer_resolved_default_bank_preserves_inline_and_wrapped_geometry");
        styles.metrics.composer_padding = 8.f;
        styles.metrics.control_height = 40.f;
        styles.metrics.menu_row_height = 44.f;
        styles.metrics.menu_padding = 10.f;
        styles.metrics.menu_gap = 10.f;
        message_composer_reset(&model, 42, false);
        message_composer_insert_utf8(&model, utf8_literal("Style-bound draft"));
        const u32 model_revision = model.revision;
        const auto* widget_storage = scene.widgets.storage.data;
        const usize allocated = scene.owned_storage.offset;
        scene.measured_row_height = 100.f;
        const float height =
            message_composer_scene_preferred_height(model, 304.f, true, true, true, &styles);
        bool projected = message_composer_scene_bind_theme_styles(&scene, &styles) &&
                         scene.measured_row_height == 0.f && scene.geometry_dirty &&
                         message_composer_scene_minimum_inline_width(&styles) == 496.f &&
                         message_composer_scene_minimum_compact_width(&styles) == 312.f &&
                         message_composer_scene_text_width(304.f, &styles) == 216.f &&
                         message_composer_scene_toolbar_height(160.f, &styles) == 96.f &&
                         message_composer_scene_toolbar_height(216.f, &styles) == 48.f && height == 212.f;
        const UiRect styled_viewport{0.f, 0.f, 304.f, height};
        const auto update = [&]() noexcept {
            return message_composer_scene_update(
                &scene, &model, styled_viewport, true, 77, utf8_literal("Replying to author"), nullptr,
                utf8_literal("Someone is typing"), {utf8_literal("photo.png - ready"), true, true});
        };
        projected &= update().ok() && !scene.geometry_dirty;
        const MessageComposerControl controls[]{
            MessageComposerControl::add_attachment, MessageComposerControl::gif,
            MessageComposerControl::sticker, MessageComposerControl::emoji, MessageComposerControl::send};
        for (const auto control : controls) {
            const auto* node = message_composer_scene_layout_node(scene, control);
            projected &=
                node && ui_rect_width(node->rectangle) == 44.f && ui_rect_height(node->rectangle) == 40.f &&
                ui_rect_equal(node->rectangle, ui_rect_intersection(node->rectangle, node->clip_rectangle));
        }
        const auto* edit = message_composer_scene_layout_node(scene, MessageComposerControl::edit);
        projected &= edit && ui_rect_width(edit->content_rectangle) == 216.f;
        UiLayoutDiagnostic diagnostics[16]{};
        projected &= ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 16}).clean();
        check(projected, "composer_resolved_metrics_drive_actual_controls_padding_wrap_and_auxiliary_rows");
        const u32 layouts = scene.layout_passes, projections = scene.projection_passes;
        scene.measured_row_height = 64.f;
        bool stable = !message_composer_scene_bind_theme_styles(&scene, &styles) && !update().changed &&
                      scene.measured_row_height == 64.f &&
                      message_composer_scene_cached_height(scene, true, true, true) == height &&
                      message_composer_scene_measured_height(scene, 28.f) == height;
        ++styles.revision;
        ++styles.paint_revision;
        styles.metrics.control_radius = 12.f;
        styles.metrics.edit_radius = 16.f;
        stable &= !message_composer_scene_bind_theme_styles(&scene, &styles) && !update().changed &&
                  scene.measured_row_height == 64.f && scene.layout_passes == layouts &&
                  scene.projection_passes == projections && scene.widgets.storage.data == widget_storage &&
                  scene.owned_storage.offset == allocated && model.revision == model_revision;
        check(stable, "composer_same_bank_and_paint_only_publications_preserve_measurement_layout_"
                      "projection_and_storage");
        ++styles.geometry_revision;
        styles.metrics.menu_gap = 12.f;
        bool geometry_changed = update().ok() && scene.measured_row_height == 0.f &&
                                scene.observed_geometry_revision == styles.geometry_revision &&
                                scene.layout_passes == layouts + 1 &&
                                scene.projection_passes == projections && model.revision == model_revision;
        UiResolvedThemeStyles unready{};
        geometry_changed &=
            !message_composer_scene_bind_theme_styles(&scene, &unready) && scene.theme_styles == &styles &&
            message_composer_scene_bind_theme_styles(&scene, nullptr) && scene.theme_styles == nullptr &&
            message_composer_scene_update(&scene, &model, {0, 0, 304, 84}, true).ok();
        const auto* restored = message_composer_scene_layout_node(scene, MessageComposerControl::send);
        geometry_changed &= restored && ui_rect_width(restored->rectangle) == 36.f &&
                            ui_rect_height(restored->rectangle) == 32.f &&
                            scene.widgets.storage.data == widget_storage &&
                            scene.owned_storage.offset == allocated;
        check(geometry_changed, "composer_mutated_geometry_bank_retires_measurement_without_text_"
                                "reprojection_and_can_restore_defaults");
        styles.metrics.control_height = 28.f;
        styles.metrics.menu_row_height = 32.f;
        ++styles.geometry_revision;
        message_composer_scene_bind_theme_styles(&scene, &styles);
        const float small_height =
            message_composer_scene_preferred_height(model, 304.f, false, false, false, &styles);
        bool minimum_target =
            message_composer_scene_update(&scene, &model, {0, 0, 304, small_height}, true).ok();
        const auto* small = message_composer_scene_layout_node(scene, MessageComposerControl::send);
        minimum_target &=
            small && ui_rect_width(small->rectangle) == 32.f && ui_rect_height(small->rectangle) == 32.f;
        check(minimum_target, "composer_theme_metrics_preserve_minimum_32dip_accessory_hit_targets");
        message_composer_scene_bind_theme_styles(&scene, nullptr);
    }
    ui_scene_release(&status_shell);
    message_composer_scene_release(&scene);
}

namespace {
UiPluginSlotNode composer_slot_nodes[3];
u32 composer_slot_count;
u32 composer_slots(u32 surface, u64, u64, u64, u64, Slice<UiPluginSlotNode> output) noexcept {
    u32 used = 0;
    for (; surface == kUiPluginSlotComposerToolbar && used < composer_slot_count && used < output.count; ++used)
        output[used] = composer_slot_nodes[used];
    return used;
}
UiPoint center(const UiLayoutNode* node) noexcept {
    return node ? UiPoint{(node->rectangle.left + node->rectangle.right) * .5f,
                          (node->rectangle.top + node->rectangle.bottom) * .5f}
                : UiPoint{};
}
} // namespace

void test_composer_plugin_slots(void (*check)(bool, const char*) noexcept) noexcept {
    char storage[64]{};
    MessageComposer model{};
    MessageComposerScene scene{};
    const UiRect viewport{0, 0, 640, 56};
    bool ready = message_composer_initialize(&model, {storage, sizeof(storage)}) &&
                 message_composer_scene_initialize_owned(&scene) &&
                 message_composer_scene_update(&scene, &model, viewport, true).ok() && !scene.plugin_count;
    const float plain_width = message_composer_scene_text_width(640.f);
    const auto* gif = message_composer_scene_layout_node(scene, MessageComposerControl::gif);
    const float gif_left = gif ? gif->rectangle.left : 0;
    composer_slot_nodes[0] = {41, 7, utf8_literal("Timer"), utf8_literal("Start timer"),
                              static_cast<u16>(UiIcon::timer), ui_plugin_node_button, 1};
    composer_slot_nodes[1] = {41, 0, {}, utf8_literal("Recording"), kUiPluginSlotNoIcon, ui_plugin_node_icon, 3};
    composer_slot_nodes[2] = {42, 0, utf8_literal("Label"), {}, kUiPluginSlotNoIcon, ui_plugin_node_label, 0};
    composer_slot_count = 3;
    g_ui_plugin_slot_query = composer_slots;
    ready = ready && message_composer_scene_update(&scene, &model, viewport, true).ok();
    const auto& first = *ui_widget_manager_layout_node(scene.widgets, scene.plugin_slots[0]);
    const auto* second = ui_widget_manager_layout_node(scene.widgets, scene.plugin_slots[1]);
    const auto* third = ui_widget_manager_layout_node(scene.widgets, scene.plugin_slots[2]);
    gif = message_composer_scene_layout_node(scene, MessageComposerControl::gif);
    const UiPoint button = center(&first), icon = center(second);
    check(ready && scene.plugin_count == 2 && ui_rect_has_area(first.rectangle) && second &&
              ui_rect_has_area(second->rectangle) && third && !ui_rect_has_area(third->rectangle) && gif &&
              first.rectangle.right <= second->rectangle.left && second->rectangle.right <= gif->rectangle.left &&
              gif->rectangle.left == gif_left &&
              message_composer_scene_text_width(640.f) < plain_width - 2 * ui_rect_width(first.rectangle),
          "composer plugin slots: buttons and icons take toolbar places before GIFs; labels don't");
    check(message_composer_scene_hit_test(scene, button.x, button.y) == MessageComposerControl::plugin_slot &&
              message_composer_scene_plugin_slot(scene, button.x, button.y) == 0 &&
              message_composer_scene_plugin_slot(scene, icon.x, icon.y) == 1 &&
              message_composer_scene_plugin_slot(scene, center(gif).x, center(gif).y) == kMessageComposerPluginSlots &&
              ui_plugin_slot_icon(composer_slot_nodes[0]) == UiIcon::timer &&
              ui_plugin_slot_icon(composer_slot_nodes[1]) == UiIcon::puzzle,
          "composer plugin slots: each place hit-tests to its node; a missing icon shows the plugin glyph");
    // Tab from the editor reaches both plugin buttons before the GIF button.
    const auto after_edit = ui_widget_manager_next_focus(scene.widgets, scene.edit, false, false);
    const auto after_first = ui_widget_manager_next_focus(scene.widgets, scene.plugin_slots[0], false, false);
    const bool focused = ui_widget_manager_focus(&scene.widgets, scene.plugin_slots[1]);
    check(after_edit.slot == scene.plugin_slots[0].slot && after_first.slot == scene.plugin_slots[1].slot &&
              focused && message_composer_scene_focused_control(scene) == MessageComposerControl::plugin_slot &&
              message_composer_scene_focused_plugin_slot(scene) == 1 &&
              ui_widget_manager_next_focus(scene.widgets, scene.plugin_slots[1], false, false).slot == scene.gif.slot,
          "composer plugin slots: buttons are Tab reachable and report the focused slot for Enter");
    composer_slot_count = 0;
    const bool cleared = message_composer_scene_update(&scene, &model, viewport, true).ok();
    check(cleared && !scene.plugin_count && message_composer_scene_text_width(640.f) == plain_width &&
              message_composer_scene_hit_test(scene, button.x, button.y) != MessageComposerControl::plugin_slot,
          "composer plugin slots: cleared slots give the editor its width back");
    g_ui_plugin_slot_query = nullptr;
    message_composer_scene_release(&scene);
    message_composer_release(&model);
}

} // namespace maicord

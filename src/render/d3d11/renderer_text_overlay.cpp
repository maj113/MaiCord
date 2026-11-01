#include "renderer_text_internal.hpp"
#include "maicord/ui/account/profile_appearance.hpp"
#include "maicord/ui/chat/messages/action_bar_presentation.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "maicord/ui/media/picker_presentation.hpp"
#include "maicord/ui/chat/messages/reaction_viewer_presentation.hpp"
#include "maicord/ui/shell/text_presentation.hpp"
#include "maicord/ui/chat/threads/header.hpp"
#include "renderer_edit.hpp"
#include "renderer_login_form.hpp"

namespace maicord {
namespace renderer_text_detail {

// Glyph inputs beyond the projection (appearance and thread header revisions,
// a hovered name effect, composer styles and selection) key the shell exactly.
u64 retention_shell_signature(const D3D11Renderer& renderer, u64 shell_signature) noexcept {
    const bool settings_active = renderer_settings_page_visible(renderer);
    const bool friends_active =
        !settings_active && d3d11_renderer_active(renderer.friends_scene);
    // A hovered header chip or quote restyles its message's retained text.
    if (renderer.message_scene && renderer.message_scene->header_hover_message_id)
        shell_signature = hash64_mix(hash64_mix(shell_signature, renderer.message_scene->header_hover_message_id),
                                     renderer.message_scene->header_hover_target);
    const bool browser_active = d3d11_renderer_thread_browser_visible(renderer);
    const auto* composer =
        settings_active || friends_active || browser_active ? nullptr : renderer.message_composer_scene;
    if (!settings_active && renderer.user_store)
        shell_signature = hash64_mix(shell_signature, renderer.user_store->appearance.revision);
    if (!settings_active && !friends_active && !browser_active && renderer.thread_header)
        shell_signature = hash64_mix(shell_signature, renderer.thread_header->revision);
    if (!settings_active && !friends_active && !browser_active && renderer.message_scene &&
        renderer.user_store && renderer.chat_state) {
        // Ordinary row hover changes paint and the action-bar suffix. Only a
        // visible author name effect changes the retained base glyphs; grouped
        // rows have no author header to animate.
        const auto& scene = *renderer.message_scene;
        const auto* hovered = message_scene_resolve(scene, scene.widgets.hovered);
        const auto* record = hovered && !hovered->grouped && scene.timeline && scene.timeline->store
                                 ? message_store_resolve(*scene.timeline->store, hovered->message)
                                 : nullptr;
        if (record) {
            const auto& users = *renderer.user_store;
            const auto* global = user_appearance_find(users.appearance, record->author_id);
            const auto* channel =
                chat_state_store_find_channel(*renderer.chat_state, record->channel_id);
            const auto* guild =
                channel ? user_appearance_find(users.appearance, record->author_id, channel->guild_id)
                        : nullptr;
            // Match message_author_layout's guild/global style source.
            const auto* styled = guild && (guild->name.font_id || guild->name.effect_id) ? guild : global;
            if (styled && profile_name_effect(styled->name, true).kind) {
                shell_signature = hash64_mix(shell_signature, record->id);
                shell_signature = hash64_mix(shell_signature, scene.widgets.hover_started_ms);
            }
        }
    }
    if (composer && composer->active)
        shell_signature = hash64_counted_bytes(shell_signature, composer->display_styles,
                                               composer->display_style_count * sizeof(TextStyleSpan));
    if (composer && composer->model && composer->model->focused &&
        composer->display_anchor != composer->display_caret) {
        shell_signature = hash64_mix(shell_signature, composer->display_anchor);
        shell_signature = hash64_mix(shell_signature, composer->display_caret);
    }
    return shell_signature;
}

ContentTextRetentionKey content_text_retention_key(const D3D11Renderer& renderer, const Theme& theme,
                                                   u64 shell_signature, u64 popup_signature,
                                                   bool include_messages) noexcept {
    const bool login_form = d3d11_renderer_login_form_text(renderer);
    if (!login_form)
        shell_signature = retention_shell_signature(renderer, shell_signature);
    include_messages = include_messages && !login_form;
    const MessageTextPresentation* presentation = renderer.message_text;
    return ContentTextRetentionKey{
        shell_signature,
        include_messages && presentation ? presentation->projection_signature : 0,
        popup_signature,
        hash64_mix(content_text_theme_signature(theme),
                   typography_source_identity(renderer.glyph_text.system)),
        include_messages ? renderer.message_text_selection_message_id : 0,
        include_messages ? renderer.message_text_selection_command_identity : 0,
        include_messages ? renderer.message_text_selection_revision : 0,
        include_messages ? renderer.message_text_selection_command_index : 0,
        include_messages ? renderer.message_text_selection_anchor : 0,
        include_messages ? renderer.message_text_selection_caret : 0,
        renderer.dpi,
        renderer.glyph_text.allocator.evicted_entries,
        renderer.glyph_text.allocator.full_resets,
        include_messages,
    };
}

// Friends search borrows composer_layout while the main chat content is
// covered. The thread browser has no field: the header search bar serves it.
bool measure_content_search(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->friends_search_geometry);
    const auto* scene = renderer->friends_scene;
    if (!scene || !scene->active || !scene->content || !scene->content->search.active ||
        d3d11_renderer_active(renderer->friends_requests_scene)) {
        text_edit_layout_invalidate(&renderer->composer_layout);
        return true;
    }
    auto& layout = renderer->composer_layout;
    if (renderer->edit_selection_owner != &layout) {
        text_edit_layout_invalidate(&renderer->login_edit);
        renderer->edit_selection_owner = &layout;
    }
    return d3d11_renderer_measure_search_edit(&layout, renderer->edit_selections, scene->content->search,
                                              ui_friends_scene_search_geometry(*scene).text,
                                              renderer->glyph_text.system, true, &renderer->friends_search_geometry);
}

bool measure_message_search(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->search_edit_geometry);
    if (!d3d11_renderer_search_visible(*renderer)) {
        text_edit_layout_invalidate(&renderer->search_layout);
        return true;
    }
    const auto& scene = *renderer->search_scene;
    return d3d11_renderer_measure_search_edit(&renderer->search_layout, renderer->search_edit_selections,
                                              scene.content->edit, scene.text, renderer->glyph_text.system, false,
                                              &renderer->search_edit_geometry);
}
bool measure_media_picker(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->media_picker_edit_geometry);
    auto& layout = renderer->media_picker_layout;
    const bool quick = d3d11_renderer_quick_switcher_visible(*renderer);
    const bool forward = d3d11_renderer_message_forward_visible(*renderer);
    // Inline results (the composer's /gif and /sticker rows) have no search field.
    if (!quick && !forward &&
        (!d3d11_renderer_media_picker_visible(*renderer) ||
         renderer->media_picker_scene->variant == UiMediaPickerVariant::inline_results)) {
        text_edit_layout_invalidate(&layout);
        return true;
    }
    const auto& edit = forward ? renderer->message_forward_scene->content->edit
                       : quick ? renderer->quick_switcher_scene->content->edit
                               : renderer->media_picker_scene->content->edit;
    const auto bounds = forward ? renderer->message_forward_scene->text
                        : quick ? renderer->quick_switcher_scene->text
                                : renderer->media_picker_scene->text;
    return d3d11_renderer_measure_search_edit(&layout, renderer->media_picker_edit_selections, edit, bounds,
                                              renderer->glyph_text.system, true,
                                              &renderer->media_picker_edit_geometry);
}
u32 overlay_boundary(const TextOverlayBoundaries& boundaries, const OverlayBatchBoundary& entry) noexcept {
    return *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(&boundaries) + entry.boundary);
}
u32& overlay_batch(D3D11Renderer* renderer, const OverlayBatchBoundary& entry) noexcept {
    return *reinterpret_cast<u32*>(reinterpret_cast<u8*>(renderer) + entry.batch);
}
bool project_toast_text(const D3D11Renderer* renderer, const Theme& theme, UiTextOutput* output,
                        TextOverlayBoundaries* boundaries) noexcept {
    if (!d3d11_renderer_toast_visible(*renderer))
        return true;
    boundaries->toast_start = output->emitter->count;
    return ui_toast_text_output(*renderer->toast_scene, theme, output);
}

bool search_message_command(const D3D11Renderer& renderer, u32 index, MessageTextCommand* native,
                            UiTextCommand* neutral) noexcept {
    const auto* presentation = renderer.search_message_text;
    if (!presentation || !presentation->active || !d3d11_renderer_search_visible(renderer) ||
        index >= presentation->command_count)
        return false;
    *native = presentation->commands[index];
    native->clip = ui_search_scene_content_clip(*renderer.search_scene, native->clip);
    if (!native->text.bytes || !ui_rect_has_area(native->rectangle) || !ui_rect_has_area(native->clip))
        return false;
    reset_value(neutral);
    neutral->text = native->text;
    neutral->owner = native->owner;
    neutral->rectangle = native->rectangle;
    neutral->clip = native->clip;
    neutral->color = native->color;
    neutral->styles = native->styles;
    neutral->horizontal_alignment = native->horizontal_alignment;
    neutral->vertical_alignment = native->vertical_alignment;
    neutral->wrapping = native->wrapping;
    neutral->overflow = native->overflow;
    return true;
}
bool project_search_message_text(D3D11Renderer* renderer, UiTextEmitter* emitter) noexcept {
    const auto* presentation = renderer->search_message_text;
    if (!d3d11_renderer_search_visible(*renderer) || !presentation || !presentation->active)
        return true;
    if (presentation->command_count > presentation->commands.count)
        return false;
    UiTextOutput output{nullptr, emitter};
    for (u32 i = 0; i < presentation->command_count; ++i) {
        MessageTextCommand native{};
        UiTextCommand neutral{};
        if (search_message_command(*renderer, i, &native, &neutral) &&
            !ui_text_output_push(&output, neutral))
            return false;
    }
    // Semantic/reveal changes can preserve identical displayed bytes.
    emitter->signature = hash64_mix(emitter->signature, presentation->projection_signature);
    return true;
}
void reset_motion_text_ranges(D3D11Renderer* renderer, u32 batch) noexcept {
    renderer->forum_text_end_batch = batch;
    renderer->profile_text_first_batch = renderer->profile_text_end_batch = batch;
    renderer->theme_editor_text_first_batch = renderer->theme_editor_text_end_batch = batch;
    renderer->settings_page_text_first_batch = renderer->settings_page_text_end_batch = batch;
    renderer->popup_text_first_batch = renderer->popup_text_end_batch = batch;
    renderer->settings_choice_text_first_batch = renderer->settings_choice_text_end_batch = batch;
    renderer->settings_preview_text_first_batch = renderer->settings_preview_text_end_batch = batch;
    renderer->settings_dialog_text_first_batch = renderer->settings_dialog_text_end_batch = batch;
}
bool reserve_overlay_text(void* context, const UiTextCommand& command, u32) noexcept {
    auto& reserve = *static_cast<OverlayReservation*>(context);
    const u64 requested = static_cast<u64>(command.text.bytes) + 2u;
    reserve.quads += static_cast<u32>(minimum<u64>(requested, reserve.capacity - reserve.quads));
    // Picker emoji are CPU-clipped to their cells and share a scissor within
    // one scene. Reserve their merged batch, not one batch per emoji.
    const auto* picker = (command.style_flags & text_style_picker_emoji) ? command.owner.manager : nullptr;
    if (!picker || picker != reserve.picker_owner || !ui_rect_equal(command.clip, reserve.picker_clip))
        ++reserve.batches;
    reserve.picker_owner = picker;
    reserve.picker_clip = command.clip;
    return true;
}
struct BrowserShellTextFilter {
    UiTextEmitter* target;
    UiRect panel;
};
bool emit_browser_shell_text(void* context, const UiTextCommand& command, u32) noexcept {
    const auto& filter = *static_cast<BrowserShellTextFilter*>(context);
    const auto visible = ui_rect_intersection(command.rectangle, command.clip);
    if (visible.left >= filter.panel.left && visible.top >= filter.panel.top &&
        visible.right <= filter.panel.right && visible.bottom <= filter.panel.bottom)
        return true;
    UiTextOutput output{nullptr, filter.target};
    return ui_text_output_push(&output, command);
}
// The shell before the member list rows, which end the shell's commands.
bool project_shell_static_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept {
    if (d3d11_renderer_login_form_text(*renderer)) {
        const auto& cached = *renderer->login_form_text;
        if (!cached.valid)
            return false;
        UiTextOutput output{nullptr, emitter};
        for (u32 i = 0; i < cached.projection.count; ++i)
            if (!ui_text_output_push(&output, cached.projection.storage[i]))
                return false;
        return true;
    }
    const bool settings = renderer_settings_page_visible(*renderer);
    const bool friends = !settings && d3d11_renderer_active(renderer->friends_scene);
    const bool browser = d3d11_renderer_thread_browser_visible(*renderer);
    const bool hidden = friends || browser;
    BrowserShellTextFilter filter{emitter, browser ? renderer->thread_browser_scene->panel : UiRect{}};
    UiTextEmitter filtered{};
    if (browser && !ui_text_emitter_initialize(&filtered, emit_browser_shell_text, &filter))
        return false;
    UiTextOutput output{nullptr, browser ? &filtered : emitter};
    UiRect composer_rectangle{};
    const UiRect* composer = nullptr;
    if (!settings && !hidden && renderer->composer_layout.active &&
        renderer->composer_layout.layout_height > 0) {
        const auto& layout = renderer->composer_layout;
        composer_rectangle = {layout.origin_x, layout.origin_y, layout.origin_x + layout.width,
                              layout.origin_y + layout.layout_height};
        composer = &composer_rectangle;
    }
    if (!settings &&
        !shell_text_presentation_build_output(
            *renderer->ui_scene, *renderer->chat_state, *renderer->navigation_view, theme,
            *renderer->shell_navigation_scene, hidden ? nullptr : renderer->message_composer_scene, nullptr,
            composer, &output, renderer->user_store, renderer->guild_rail_scene,
            browser ? renderer->thread_browser_scene->view->parent_id : 0, renderer->message_measure_cache))
        return false;
    output.emitter = emitter;
    const bool projected =
        (settings || hidden || !renderer->thread_header ||
         ui_thread_header_text(*renderer->thread_header, theme, &output)) &&
        (!friends || ui_friends_scene_text_output(*renderer->friends_scene, theme, &output,
                                                  &renderer->friends_search_geometry)) &&
        (!browser || ui_thread_browser_scene_text_output(*renderer->thread_browser_scene, theme, &output));
    const auto mix_owner = [emitter](const auto& owner) noexcept {
        emitter->signature = hash64_mix(emitter->signature, reinterpret_cast<usize>(owner.manager));
        emitter->signature = hash64_mix(emitter->signature, owner.handle.slot);
        emitter->signature = hash64_mix(emitter->signature, owner.handle.generation);
    };
    // Selected audio replaces this attachment's static labels. Guard that
    // omission in the retained base; elapsed time remains suffix-only.
    if (projected && d3d11_renderer_audio_scene_visible(*renderer))
        mix_owner(renderer->audio_scene->owner);
    if (projected && d3d11_renderer_poll_scene_visible(*renderer))
        mix_owner(renderer->poll_scene->owner);
    return projected;
}
// The member list rows, appended after the static shell in the same emitter.
bool project_shell_member_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept {
    if (d3d11_renderer_login_form_text(*renderer) || renderer_settings_page_visible(*renderer) ||
        d3d11_renderer_active(renderer->friends_scene) || d3d11_renderer_thread_browser_visible(*renderer))
        return true;
    UiTextOutput output{nullptr, emitter};
    // The DM profile panel's text goes with the member slot's.
    return (!renderer->member_list_scene ||
            shell_text_presentation_member_output(*renderer->member_list_scene, theme, &output)) &&
           (!d3d11_renderer_active(renderer->dm_profile_panel) ||
            profile_scene_text_output(*renderer->dm_profile_panel, theme, &output, nullptr));
}
bool project_shell_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter) noexcept {
    return project_shell_static_text(renderer, theme, emitter) &&
           project_shell_member_text(renderer, theme, emitter);
}
bool project_overlay_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter,
                          TextOverlayBoundaries* boundaries) noexcept {
    if (d3d11_renderer_login_form_text(*renderer)) {
        reset_value(boundaries);
        UiTextOutput output{nullptr, emitter};
        return !d3d11_renderer_text_form_active(*renderer) ||
               (renderer->text_form->scene->content.confirmation_only &&
                text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                                      &output));
    }
    const bool settings = renderer_settings_page_visible(*renderer);
    const bool friends = !settings && d3d11_renderer_active(renderer->friends_scene);
    const bool requests =
        !settings && d3d11_renderer_active(renderer->friends_requests_scene);
    const bool form = d3d11_renderer_text_form_active(*renderer);
    const bool forum = d3d11_renderer_forum_form_active(*renderer);
    UiTextOutput output{nullptr, emitter};
    if (settings) {
        boundaries->friends_end = boundaries->menu_start = 0;
        if (!ui_settings_scene_text_output(*renderer->settings_scene, theme, &output))
            return false;
        // The profile editor's preview card draws its text over its media.
        const auto* preview = renderer->settings_profile_preview;
        boundaries->settings_preview_start = emitter->count;
        if (preview && preview->active && !profile_scene_text_output(*preview, theme, &output, nullptr))
            return false;
        boundaries->settings_preview_end = emitter->count;
        boundaries->form_start = emitter->count;
        boundaries->popup_start = boundaries->popup_end = emitter->count;
        boundaries->settings_choice_start = emitter->count;
        const auto* popup = d3d11_renderer_settings_popup(*renderer);
        if (popup && !popup_text_presentation_build_output(*popup, theme, &output,
                                                          renderer->message_measure_cache))
            return false;
        boundaries->settings_choice_end = emitter->count;
        const auto* dialog = renderer->settings_profile_dialog;
        boundaries->settings_dialog_start = emitter->count;
        if (dialog && dialog->active && !ui_profile_dialog_text_output(*dialog, theme, &output))
            return false;
        boundaries->settings_dialog_end = emitter->count;
        boundaries->theme_editor_start = boundaries->theme_editor_end = emitter->count;
        boundaries->form_start = emitter->count;
        if (forum && !forum_post_text_output(*renderer->forum_form->scene, renderer->forum_form->geometry,
                                             theme, &output))
            return false;
        boundaries->forum_end = emitter->count;
        if (form && !text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry,
                                           theme, &output))
            return false;
        return project_toast_text(renderer, theme, &output, boundaries);
    }
    // The call view leads the side-panel text range, above the base content.
    if (d3d11_renderer_call_visible(*renderer) &&
        !ui_call_scene_text_output(*renderer->call_scene, theme, &output))
        return false;
    if (d3d11_renderer_unread_banner_visible(*renderer) &&
        !ui_unread_banner_text_output(*renderer->unread_banner_scene, theme, &output))
        return false;
    if (d3d11_renderer_video_scene_visible(*renderer)) {
        const auto* video_owner = renderer->video_scene_search_result && renderer->search_scene
                                      ? ui_search_scene_message_scene(*renderer->search_scene)
                                      : renderer->message_scene;
        if (!video_owner || !ui_video_scene_text_output(*renderer->video_scene, theme, &output,
                                                        {&video_owner->widgets, video_owner->root}))
            return false;
    }
    if (d3d11_renderer_search_visible(*renderer) &&
        !ui_search_scene_text_output(*renderer->search_scene, theme, &output,
                                     &renderer->search_edit_geometry))
        return false;
    boundaries->search_rich_start = emitter->count;
    if (!project_search_message_text(renderer, emitter))
        return false;
    boundaries->search_rich_end = emitter->count;
    if (d3d11_renderer_pinned_messages_visible(*renderer) &&
        !ui_pinned_messages_scene_text_output(*renderer->pinned_messages_scene, theme, &output))
        return false;
    if (requests &&
        !ui_friends_scene_text_output(*renderer->friends_requests_scene, theme, &output, nullptr))
        return false;
    boundaries->friends_end = emitter->count;
    boundaries->media_picker_start = emitter->count;
    if (d3d11_renderer_media_picker_visible(*renderer) &&
        !ui_media_picker_scene_text_output(*renderer->media_picker_scene, theme, &output,
                                           &renderer->media_picker_edit_geometry))
        return false;
    boundaries->media_picker_end = emitter->count;
    // The action bar's opaque panel is painted after the gallery range. Its
    // quick-reaction glyphs must belong to the following text range as well.
    // Beside a call its text chat keeps the toolbar (as painted in layout).
    if (!friends && !d3d11_renderer_thread_browser_visible(*renderer) &&
        (!d3d11_renderer_call_visible(*renderer) || renderer->call_scene->chat_open) &&
        !d3d11_renderer_active(renderer->profile_scene) &&
        !d3d11_renderer_media_picker_visible(*renderer) && renderer->message_action_bar &&
        !d3d11_renderer_message_modal_active(*renderer) && renderer->message_action_bar->active &&
        !message_action_bar_text_presentation_build_output(*renderer->message_action_bar, theme, &output))
        return false;
    boundaries->profile_start = emitter->count;
    if (!settings && renderer->profile_scene) {
        const auto profile_edit = d3d11_renderer_profile_edit_geometry(*renderer);
        if (!profile_scene_text_output(*renderer->profile_scene, theme, &output, &profile_edit))
            return false;
    }
    boundaries->profile_end = emitter->count;
    boundaries->menu_start = emitter->count;
    boundaries->popup_start = emitter->count;
    if (d3d11_renderer_download_visible(*renderer) &&
        !ui_download_scene_text_output(*renderer->download_scene, theme, &output))
        return false;
    if (d3d11_renderer_message_forward_visible(*renderer)) {
        const auto popup_edit = d3d11_renderer_popup_edit_geometry(*renderer);
        if (!ui_message_forward_scene_text_output(*renderer->message_forward_scene, theme, &output,
                                                  &renderer->media_picker_edit_geometry, &popup_edit))
            return false;
    }
    boundaries->audio_start = emitter->count;
    if (d3d11_renderer_audio_scene_visible(*renderer) &&
        !ui_audio_scene_text_output(*renderer->audio_scene, &output))
        return false;
    boundaries->audio_end = emitter->count;
    boundaries->poll_start = emitter->count;
    if (d3d11_renderer_poll_scene_visible(*renderer) &&
        !ui_poll_scene_text_output(*renderer->poll_scene, &output))
        return false;
    if (d3d11_renderer_poll_voters_visible(*renderer) &&
        !ui_poll_voters_scene_text_output(*renderer->poll_voters_scene, &output))
        return false;
    boundaries->poll_end = emitter->count;
    if (d3d11_renderer_inbox_visible(*renderer) &&
        !ui_inbox_scene_text_output(*renderer->inbox_scene, theme, &output))
        return false;
    if (d3d11_renderer_stream_card_visible(*renderer) &&
        !ui_stream_card_text_output(*renderer->stream_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_guild_card_visible(*renderer) &&
        !ui_guild_card_text_output(*renderer->guild_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_expression_card_visible(*renderer) &&
        !ui_expression_card_text_output(*renderer->expression_card_scene, theme, &output))
        return false;
    if (d3d11_renderer_quick_switcher_visible(*renderer) &&
        !ui_quick_switcher_scene_text_output(*renderer->quick_switcher_scene, theme, &output,
                                             &renderer->media_picker_edit_geometry))
        return false;
    if (d3d11_renderer_shortcuts_visible(*renderer) &&
        !ui_shortcuts_scene_text_output(*renderer->shortcuts_scene, theme, &output))
        return false;
    if (d3d11_renderer_active(renderer->popup_scene) &&
        !popup_text_presentation_build_output(*renderer->popup_scene, theme, &output,
                                               renderer->message_measure_cache))
        return false;
    boundaries->popup_end = emitter->count;
    boundaries->settings_choice_start = boundaries->settings_choice_end = emitter->count;
    if (d3d11_renderer_active(renderer->reaction_viewer) &&
        !reaction_viewer_text_presentation_build_output(*renderer->reaction_viewer, theme, &output))
        return false;
    if (!settings && renderer->image_viewer &&
        !ui_image_viewer_text_output(*renderer->image_viewer, theme, &output))
        return false;
    boundaries->theme_editor_start = emitter->count;
    // Hosted video time glyphs follow their bar in the viewer's upper layer.
    if (!settings && d3d11_renderer_viewer_video_visible(*renderer) &&
        !ui_video_scene_text_output(*renderer->video_scene, theme, &output, renderer->image_viewer->owner))
        return false;

    if (renderer_theme_editor_visible(*renderer) &&
        !ui_theme_editor_text_output(*renderer->theme_editor_scene, theme, &output))
        return false;
    boundaries->theme_editor_end = emitter->count;
    boundaries->form_start = emitter->count;
    if (forum && !forum_post_text_output(*renderer->forum_form->scene, renderer->forum_form->geometry,
                                         theme, &output))
        return false;
    boundaries->forum_end = emitter->count;
    return (!form || text_form_text_output(*renderer->text_form->scene, renderer->text_form->geometry,
                                           theme, &output)) &&
           (!renderer_settings_page_visible(*renderer) ||
            ui_settings_scene_text_output(*renderer->settings_scene, theme, &output)) &&
           (!d3d11_renderer_tooltip_visible(*renderer) ||
            ui_tooltip_text_output(*renderer->tooltip_scene, theme, &output)) &&
           project_toast_text(renderer, theme, &output, boundaries);
}
bool raster_text_command(void* context, const UiTextCommand& command, u32 ordinal) noexcept {
    auto& raster = *static_cast<TextRasterConsumer*>(context);
    auto* renderer = raster.renderer;
    bool boundary = false;
    if (raster.overlay) {
        const auto& bounds = *raster.boundaries;
        const bool settings = renderer_settings_page_visible(*renderer);
        const bool form = d3d11_renderer_form_active(*renderer) || d3d11_renderer_tooltip_visible(*renderer);
        const u32 batch = renderer->content_text->batch_count;
        boundary = ordinal == 0;
        for (const auto& entry : kOverlayBatchBoundaries) {
            if (ordinal == overlay_boundary(bounds, entry)) {
                overlay_batch(renderer, entry) = batch;
                boundary = true;
            }
        }
        if (ordinal == bounds.forum_end)
            renderer->forum_text_end_batch = batch;
        if (form || settings) {
            if (ordinal == bounds.form_start) {
                renderer->form_text_first_batch = batch;
                boundary = true;
            }
            boundary = boundary || ordinal == bounds.forum_end;
        }
    }
    const bool page = raster.overlay && renderer_settings_page_visible(*renderer) &&
                      ui_settings_scene_page_owner(*renderer->settings_scene, command.owner);
    boundary = boundary || page != raster.previous_settings_page;
    if (page && !raster.settings_page_started) {
        renderer->settings_page_text_first_batch = renderer->content_text->batch_count;
        raster.settings_page_started = true;
    }
    raster.previous_settings_page = page;
    raster.pending_boundary = raster.pending_boundary || boundary;
    const u32 previous_batches = renderer->content_text->batch_count;
    bool appended{};
    if (raster.overlay && ordinal >= raster.boundaries->search_rich_start &&
        ordinal < raster.boundaries->search_rich_end) {
        const auto* presentation = renderer->search_message_text;
        MessageTextCommand native{};
        UiTextCommand neutral{};
        bool found = false;
        while (presentation && raster.search_command_cursor < presentation->command_count) {
            const u32 index = raster.search_command_cursor++;
            if (!search_message_command(*renderer, index, &native, &neutral))
                continue;
            // The emitter validates every command but omits fully clipped labels.
            // Advance this source cursor over the same omitted commands.
            if (!ui_rect_intersects(neutral.rectangle, neutral.clip))
                continue;
            // The hashing and raster visitors walk the same immutable projection.
            if (neutral.text.data != command.text.data || neutral.text.bytes != command.text.bytes ||
                neutral.owner.manager != command.owner.manager ||
                neutral.owner.handle.slot != command.owner.handle.slot ||
                !ui_rect_equal(neutral.clip, command.clip))
                return false;
            appended = append_message_text_command(renderer, presentation, native, index, *raster.theme,
                                                   raster.atlas_invalidated, true, raster.pending_boundary);
            found = true;
            break;
        }
        if (!found)
            return false;
    } else {
        const bool selected_audio_text = raster.overlay && ordinal >= raster.boundaries->audio_start &&
                                         ordinal < raster.boundaries->audio_end;
        const bool selected_poll_text = raster.overlay && ordinal >= raster.boundaries->poll_start &&
                                        ordinal < raster.boundaries->poll_end;
        appended = ((!selected_audio_text && d3d11_renderer_audio_owner(*renderer, command.owner)) ||
                    (!selected_poll_text && d3d11_renderer_poll_owner(*renderer, command.owner)))
                       ? true
                       : append_content_text_command(renderer, command, *raster.theme,
                                                     raster.atlas_invalidated, raster.pending_boundary);
    }
    // Culled or zero-glyph commands must not consume a layer boundary: the
    // next emitted batch still belongs wholly to the new layer.
    if (renderer->content_text->batch_count != previous_batches)
        raster.pending_boundary = false;
    if (page)
        renderer->settings_page_text_end_batch = renderer->content_text->batch_count;
    return appended && !*raster.atlas_invalidated;
}
} // namespace renderer_text_detail
} // namespace maicord

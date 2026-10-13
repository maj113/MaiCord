#include "maicord/render/renderer_presentation.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/core/hash.hpp"
#include "d3d11_renderer_interaction_state.hpp"
#include "d3d11_renderer_edit.hpp"
#include "d3d11_renderer_login_form.hpp"
#include <stddef.h>

#include <Windows.h>

namespace maicord {
namespace {

bool presentation_stamp_push(RendererPresentationStamp* stamp, u64 value) noexcept {
    if (!stamp) {
        return false;
    }
    if (stamp->count == ~0u) {
        stamp->valid = false;
        return false;
    }
    const u64 ordinal = stamp->count++;
    const u64 first = value + 0x9E3779B97F4A7C15ull + ordinal;
    stamp->primary = (stamp->primary ^ first) * 0x100000001B3ull;
    const u64 rotated = (value << 29) | (value >> 35);
    const u64 second = rotated ^ (ordinal * 0xD6E8FEB86659FD93ull);
    const u64 mixed = (stamp->secondary ^ second) * 0xC2B2AE3D27D4EB4Full;
    stamp->secondary = (mixed << 23) | (mixed >> 41);
    if (!stamp->timeline) {
        const u64 shell = stamp->shell ^ (value + 0x9E3779B97F4A7C15ull * ++stamp->shell_count);
        stamp->shell = (shell ^ (shell >> 31)) * 0xC2B2AE3D27D4EB4Full;
    }
    return true;
}

// Inputs only the message timeline reads stay out of the shell hash.
struct TimelineInputs {
    RendererPresentationStamp* stamp;
    bool previous;
    explicit TimelineInputs(RendererPresentationStamp* source) noexcept
        : stamp(source), previous(source->timeline) {
        stamp->timeline = true;
    }
    ~TimelineInputs() noexcept {
        stamp->timeline = previous;
    }
};

bool presentation_stamp_bytes(RendererPresentationStamp* stamp, const void* bytes, u32 count) noexcept {
    if (!stamp) {
        return false;
    }
    if (!bytes && count != 0) {
        stamp->valid = false;
        return false;
    }
    const u8* source = static_cast<const u8*>(bytes);
    while (count != 0) {
        const u32 chunk = minimum<u32>(count, sizeof(u64));
        u64 value = 0;
        CopyMemory(&value, source, chunk);
        if (!presentation_stamp_push(stamp, value)) {
            return false;
        }
        source += chunk;
        count -= chunk;
    }
    return true;
}

u64 presentation_handle(UiWidgetHandle handle) noexcept {
    return static_cast<u64>(handle.slot) | (static_cast<u64>(handle.generation) << 16);
}

bool presentation_stamp_widgets(RendererPresentationStamp* stamp, const UiWidgetManager& widgets) noexcept {
    return presentation_stamp_push(stamp, widgets.generation) &&
           presentation_stamp_push(stamp, widgets.layout.revision) &&
           presentation_stamp_push(stamp, presentation_handle(widgets.focused)) &&
           presentation_stamp_push(stamp, stamp->structural ? 0 : presentation_handle(widgets.hovered)) &&
           presentation_stamp_push(stamp, presentation_handle(widgets.captured));
}

bool presentation_stamp_caret(RendererPresentationStamp* stamp, bool visible) noexcept {
    return presentation_stamp_push(stamp, !stamp->structural && visible);
}

bool presentation_stamp_pointer(RendererPresentationStamp* stamp, const void* pointer) noexcept {
    return presentation_stamp_push(stamp, static_cast<u64>(reinterpret_cast<ULONG_PTR>(pointer)));
}

} // namespace

RendererPresentationStamp renderer_capture_presentation_stamp(const D3D11Renderer& renderer,
                                                              const Theme& theme, bool structural) noexcept {
    RendererPresentationStamp stamp{};
    stamp.primary = 0xCBF29CE484222325ull;
    stamp.secondary = 0x84222325CBF29CE4ull;
    stamp.shell = 0x243F6A8885A308D3ull;
    stamp.valid = true;
    stamp.structural = structural;
    if (!presentation_stamp_push(&stamp, renderer.typography_revision))
        return stamp;
    if (d3d11_renderer_login_form_text(renderer)) {
        const auto& owned = *renderer.login_form_text;
        const auto* form =
            renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active
                ? renderer.text_form->scene
                : nullptr;
        if (!presentation_stamp_push(&stamp, renderer.width) ||
            !presentation_stamp_push(&stamp, renderer.height) ||
            !presentation_stamp_push(&stamp, renderer.dpi) ||
            !presentation_stamp_bytes(&stamp, &theme, sizeof(theme)) ||
            !presentation_stamp_pointer(&stamp, renderer.login_scene) ||
            (renderer.login_scene && !presentation_stamp_widgets(&stamp, renderer.login_scene->widgets)) ||
            !presentation_stamp_push(&stamp, owned.valid) ||
            !presentation_stamp_push(&stamp, owned.projection.signature) ||
            !presentation_stamp_push(&stamp, renderer.content_text->rebuilds) ||
            !presentation_stamp_push(&stamp, renderer.content_text->quad_count) ||
            !presentation_stamp_push(&stamp, renderer.content_text->batch_count) ||
            !presentation_stamp_push(&stamp, renderer.overlay_text_first_batch) ||
            !presentation_stamp_push(&stamp, renderer.form_text_first_batch) ||
            !presentation_stamp_push(&stamp, renderer.login_qr_ready) ||
            (renderer.login_qr_ready &&
             !presentation_stamp_bytes(&stamp, &renderer.login_qr_code, sizeof(renderer.login_qr_code))) ||
            !presentation_stamp_pointer(&stamp, renderer.text_form) ||
            !presentation_stamp_pointer(&stamp, form) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_form_revision))
            return stamp;
        if (form && (!presentation_stamp_push(&stamp, form->revision) ||
                     !presentation_stamp_widgets(&stamp, form->widgets) ||
                     !presentation_stamp_bytes(&stamp, &form->viewport, sizeof(form->viewport))))
            return stamp;
        if (!form && (!presentation_stamp_push(&stamp, renderer.login_caret_visible) ||
                      !presentation_stamp_bytes(&stamp, &renderer.login_edit.caret,
                                                sizeof(renderer.login_edit.caret)) ||
                      !presentation_stamp_push(&stamp, renderer.login_edit.selection_count) ||
                      !presentation_stamp_bytes(&stamp, renderer.login_edit.selections,
                                                renderer.login_edit.selection_count *
                                                    sizeof(TextEditMetric))))
            return stamp;
        return stamp;
    }
    if (renderer.forum_form && renderer.forum_form->scene) {
        const auto& form = *renderer.forum_form->scene;
        if (!presentation_stamp_pointer(&stamp, renderer.forum_form) ||
            !presentation_stamp_push(&stamp, form.content.owner_serial) ||
            !presentation_stamp_push(&stamp, form.active) ||
            !presentation_stamp_push(&stamp, form.revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_forum_form_revision) ||
            !presentation_stamp_widgets(&stamp, form.widgets))
            return stamp;
        if (form.active)
            for (u32 field = 0; field < 2; ++field) {
                const auto& geometry = renderer.forum_form->geometry[field];
                if ((!geometry.selections.data && geometry.selections.count) ||
                    geometry.selections.count > kTextEditSelectionCapacity ||
                    (!geometry.composition.data && geometry.composition.count) ||
                    geometry.composition.count > kTextCompositionSpanCapacity) {
                    stamp.valid = false;
                    return stamp;
                }
                if (!presentation_stamp_bytes(&stamp, &geometry.text_rectangle, sizeof(UiRect)) ||
                    !presentation_stamp_bytes(&stamp, &geometry.caret, sizeof(UiRect)) ||
                    !presentation_stamp_push(&stamp, geometry.selections.count) ||
                    !presentation_stamp_bytes(&stamp, geometry.selections.data,
                                              geometry.selections.count * sizeof(UiRect)) ||
                    !presentation_stamp_push(&stamp, geometry.composition.count) ||
                    !presentation_stamp_bytes(&stamp, geometry.composition.data,
                                              geometry.composition.count * sizeof(TextFormCompositionMark)))
                    return stamp;
            }
    }
    if (renderer_settings_page_visible(renderer)) {
        const auto& settings = *renderer.settings_scene;
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        if (!presentation_stamp_pointer(&stamp, renderer.settings_scene) ||
            !presentation_stamp_push(&stamp, settings.revision) ||
            !presentation_stamp_push(&stamp, settings.motion_revision) ||
            !presentation_stamp_widgets(&stamp, settings.widgets) ||
            !presentation_stamp_bytes(&stamp, &settings.viewport, sizeof(settings.viewport)) ||
            !presentation_stamp_push(&stamp, renderer.width) ||
            !presentation_stamp_push(&stamp, renderer.height) ||
            !presentation_stamp_push(&stamp, renderer.dpi) ||
            !presentation_stamp_push(&stamp, renderer.login_visible) ||
            !presentation_stamp_bytes(&stamp, &theme, sizeof(theme)) ||
            !presentation_stamp_pointer(&stamp, renderer.content_text) ||
            !presentation_stamp_push(&stamp, renderer.glyph_text.allocator.evicted_entries) ||
            !presentation_stamp_push(&stamp, renderer.glyph_text.allocator.full_resets) ||
            !presentation_stamp_push(&stamp, renderer.glyph_text.ready) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_settings_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_active_mask) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_popup_revision) ||
            !presentation_stamp_pointer(&stamp, popup) ||
            (popup && (!presentation_stamp_push(&stamp, popup->revision) ||
                       !presentation_stamp_widgets(&stamp, popup->widgets) ||
                       !presentation_stamp_bytes(&stamp, &popup->rectangle, sizeof(UiRect)))))
            return stamp;
        if (renderer.content_text &&
            (!presentation_stamp_push(&stamp, renderer.content_text->rebuilds) ||
             !presentation_stamp_push(&stamp, renderer.content_text->quad_count) ||
             !presentation_stamp_push(&stamp, renderer.content_text->batch_count) ||
             !presentation_stamp_push(&stamp, renderer.overlay_text_first_batch) ||
             !presentation_stamp_push(&stamp, renderer.form_text_first_batch)))
            return stamp;
        return stamp;
    }
    if (!presentation_stamp_pointer(&stamp, renderer.theme_editor_scene) ||
        (renderer_theme_editor_visible(renderer) &&
         (!presentation_stamp_push(&stamp, renderer.theme_editor_scene->revision) ||
          !presentation_stamp_push(&stamp, renderer.theme_editor_scene->generation) ||
          !presentation_stamp_widgets(&stamp, renderer.theme_editor_scene->widgets))))
        return stamp;
    const bool friends_active = renderer.friends_scene && renderer.friends_scene->active;
    const bool browser_active = d3d11_renderer_thread_browser_visible(renderer);
    const bool content_hidden = friends_active || browser_active;
    if (!presentation_stamp_pointer(&stamp, renderer.thread_browser_scene) ||
        !presentation_stamp_push(&stamp, browser_active))
        return stamp;
    if (browser_active) {
        const auto& browser = *renderer.thread_browser_scene;
        if (!presentation_stamp_push(&stamp, browser.revision) ||
            !presentation_stamp_push(&stamp, stamp.structural ? 0 : browser.spinner_phase) ||
            !presentation_stamp_push(&stamp, browser.view->parent_id) ||
            !presentation_stamp_push(&stamp, browser.content_revision) ||
            !presentation_stamp_push(&stamp, browser.source_publication) ||
            !presentation_stamp_push(&stamp, browser.view->revision) ||
            !presentation_stamp_push(&stamp, browser.view->source_publication) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_thread_browser_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_thread_browser_publication) ||
            !presentation_stamp_widgets(&stamp, browser.widgets) ||
            !presentation_stamp_bytes(&stamp, &browser.panel, sizeof(browser.panel)) ||
            !presentation_stamp_bytes(&stamp, &browser.style, sizeof(browser.style)) ||
            !presentation_stamp_bytes(&stamp, &browser.scroll_dips, sizeof(browser.scroll_dips)))
            return stamp;
    }
    if (!presentation_stamp_pointer(&stamp, renderer.media_picker_scene) ||
        !presentation_stamp_push(&stamp, d3d11_renderer_media_picker_visible(renderer)))
        return stamp;
    if (renderer.media_picker_scene) {
        const auto& gallery = *renderer.media_picker_scene;
        if (!presentation_stamp_push(&stamp, gallery.active) ||
            !presentation_stamp_push(&stamp, gallery.revision) ||
            !presentation_stamp_push(&stamp, gallery.generation) ||
            !presentation_stamp_push(&stamp, gallery.account_scope) ||
            !presentation_stamp_push(&stamp, gallery.query_generation) ||
            !presentation_stamp_push(&stamp, gallery.source_revision) ||
            !presentation_stamp_pointer(&stamp, gallery.source) ||
            !presentation_stamp_pointer(&stamp, gallery.content) ||
            (gallery.source && (!presentation_stamp_push(&stamp, gallery.source->revision) ||
                                !presentation_stamp_push(&stamp, gallery.source->account_scope) ||
                                !presentation_stamp_push(&stamp, gallery.source->query_generation))) ||
            (gallery.content && (!presentation_stamp_push(&stamp, gallery.content->revision) ||
                                 !presentation_stamp_push(&stamp, gallery.content->account_scope) ||
                                 !presentation_stamp_push(&stamp, gallery.content->query_generation))))
            return stamp;
        if (d3d11_renderer_media_picker_visible(renderer) &&
            (!presentation_stamp_widgets(&stamp, gallery.widgets) ||
             !presentation_stamp_bytes(&stamp, &gallery.panel, sizeof(gallery.panel)) ||
             !presentation_stamp_bytes(&stamp, &renderer.media_picker_edit_geometry,
                                       sizeof(renderer.media_picker_edit_geometry)) ||
             !presentation_stamp_push(&stamp, renderer.interaction_text_media_picker_revision)))
            return stamp;
    }
    if (d3d11_renderer_search_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.search_scene) ||
         !presentation_stamp_push(&stamp, renderer.search_scene->revision) ||
         !presentation_stamp_push(&stamp, renderer.interaction_text_search_revision) ||
         !presentation_stamp_bytes(&stamp, &renderer.search_edit_geometry,
                                   sizeof(renderer.search_edit_geometry)) ||
         !presentation_stamp_widgets(&stamp, renderer.search_scene->widgets)))
        return stamp;
    if (d3d11_renderer_search_visible(renderer)) {
        const auto* scene = ui_search_scene_message_scene(*renderer.search_scene);
        if (scene &&
            (!presentation_stamp_pointer(&stamp, scene) ||
             !presentation_stamp_widgets(&stamp, scene->widgets) ||
             !presentation_stamp_push(&stamp, scene->rebuilds) ||
             !presentation_stamp_push(&stamp, scene->layout_passes) ||
             !presentation_stamp_pointer(&stamp, renderer.search_message_text) ||
             !presentation_stamp_push(&stamp, renderer.search_message_text
                                                  ? renderer.search_message_text->projection_signature
                                                  : 0)))
            return stamp;
    }
    if (d3d11_renderer_pinned_messages_visible(renderer)) {
        const auto& pins = *renderer.pinned_messages_scene;
        if (!presentation_stamp_pointer(&stamp, renderer.pinned_messages_scene) ||
            !presentation_stamp_push(&stamp, pins.revision) ||
            !presentation_stamp_push(&stamp, stamp.structural ? 0 : pins.spinner_phase) ||
            !presentation_stamp_push(&stamp, pins.source_revision) ||
            !presentation_stamp_push(&stamp, pins.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_pinned_messages_revision) ||
            !presentation_stamp_widgets(&stamp, pins.widgets) ||
            !presentation_stamp_bytes(&stamp, &pins.panel, sizeof(pins.panel)))
            return stamp;
    }
    if (d3d11_renderer_inbox_visible(renderer)) {
        const auto& inbox = *renderer.inbox_scene;
        if (!presentation_stamp_pointer(&stamp, renderer.inbox_scene) ||
            !presentation_stamp_push(&stamp, inbox.revision) ||
            !presentation_stamp_push(&stamp, stamp.structural ? 0 : inbox.spinner_phase) ||
            !presentation_stamp_push(&stamp, inbox.source_revision) ||
            !presentation_stamp_push(&stamp, inbox.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_inbox_revision) ||
            !presentation_stamp_widgets(&stamp, inbox.widgets) ||
            !presentation_stamp_bytes(&stamp, &inbox.panel, sizeof(inbox.panel)))
            return stamp;
    }
    if (d3d11_renderer_quick_switcher_visible(renderer)) {
        const auto& quick = *renderer.quick_switcher_scene;
        if (!presentation_stamp_pointer(&stamp, renderer.quick_switcher_scene) ||
            !presentation_stamp_push(&stamp, quick.revision) ||
            !presentation_stamp_push(&stamp, quick.source_revision) ||
            !presentation_stamp_push(&stamp, quick.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_quick_switcher_revision) ||
            !presentation_stamp_widgets(&stamp, quick.widgets) ||
            !presentation_stamp_bytes(&stamp, &quick.panel, sizeof(quick.panel)) ||
            !presentation_stamp_bytes(&stamp, &renderer.media_picker_edit_geometry,
                                      sizeof(renderer.media_picker_edit_geometry)))
            return stamp;
    }
    if (d3d11_renderer_message_forward_visible(renderer)) {
        const auto& forward = *renderer.message_forward_scene;
        const auto caption = d3d11_renderer_popup_edit_geometry(renderer);
        if (!presentation_stamp_pointer(&stamp, renderer.message_forward_scene) ||
            !presentation_stamp_push(&stamp, forward.revision) ||
            !presentation_stamp_push(&stamp, forward.source_revision) ||
            !presentation_stamp_push(&stamp, forward.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_message_forward_revision) ||
            !presentation_stamp_widgets(&stamp, forward.widgets) ||
            !presentation_stamp_bytes(&stamp, &forward.panel, sizeof(forward.panel)) ||
            !presentation_stamp_bytes(&stamp, &renderer.media_picker_edit_geometry,
                                      sizeof(renderer.media_picker_edit_geometry)) ||
            !presentation_stamp_bytes(&stamp, &caption, sizeof(caption)))
            return stamp;
    }
    if (d3d11_renderer_call_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.call_scene) ||
         !presentation_stamp_push(&stamp, renderer.call_scene->revision) ||
         !presentation_stamp_push(&stamp, stamp.structural ? 0 : renderer.call_scene->spinner_phase) ||
         !presentation_stamp_push(&stamp, renderer.interaction_text_call_revision) ||
         !presentation_stamp_bytes(&stamp, &renderer.call_scene->viewport, sizeof(renderer.call_scene->viewport))))
        return stamp;
    if (d3d11_renderer_stream_card_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.stream_card_scene) ||
         !presentation_stamp_push(&stamp, renderer.stream_card_scene->revision) ||
         !presentation_stamp_push(&stamp, stamp.structural ? 0 : renderer.stream_card_scene->spinner_phase) ||
         !presentation_stamp_push(&stamp, renderer.interaction_text_stream_card_revision) ||
         !presentation_stamp_bytes(&stamp, &renderer.stream_card_scene->panel,
                                   sizeof(renderer.stream_card_scene->panel))))
        return stamp;
    if (d3d11_renderer_guild_card_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.guild_card_scene) ||
         !presentation_stamp_push(&stamp, renderer.guild_card_scene->revision) ||
         !presentation_stamp_push(&stamp, stamp.structural ? 0 : renderer.guild_card_scene->spinner_phase) ||
         !presentation_stamp_push(&stamp, renderer.interaction_text_guild_card_revision) ||
         !presentation_stamp_bytes(&stamp, &renderer.guild_card_scene->panel,
                                   sizeof(renderer.guild_card_scene->panel))))
        return stamp;
    if (d3d11_renderer_shortcuts_visible(renderer)) {
        const auto& shortcuts = *renderer.shortcuts_scene;
        if (!presentation_stamp_pointer(&stamp, renderer.shortcuts_scene) ||
            !presentation_stamp_push(&stamp, shortcuts.revision) ||
            !presentation_stamp_push(&stamp, shortcuts.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_shortcuts_revision) ||
            !presentation_stamp_widgets(&stamp, shortcuts.widgets) ||
            !presentation_stamp_bytes(&stamp, &shortcuts.panel, sizeof(shortcuts.panel)) ||
            !presentation_stamp_bytes(&stamp, &shortcuts.scroll, sizeof(shortcuts.scroll)))
            return stamp;
    }
    if (d3d11_renderer_download_visible(renderer)) {
        const auto& downloads = *renderer.download_scene;
        if (!presentation_stamp_pointer(&stamp, renderer.download_scene) ||
            !presentation_stamp_push(&stamp, downloads.revision) ||
            !presentation_stamp_push(&stamp, downloads.source_revision) ||
            !presentation_stamp_push(&stamp, downloads.content_revision) ||
            !presentation_stamp_push(&stamp, renderer.interaction_text_download_revision) ||
            !presentation_stamp_widgets(&stamp, downloads.widgets) ||
            !presentation_stamp_bytes(&stamp, &downloads.panel, sizeof(downloads.panel)) ||
            !presentation_stamp_bytes(&stamp, &downloads.viewport, sizeof(downloads.viewport)))
            return stamp;
    }
    if (d3d11_renderer_unread_banner_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.unread_banner_scene) ||
         !presentation_stamp_push(&stamp, renderer.unread_banner_scene->revision)))
        return stamp;
    // A hidden tooltip contributes nothing to the stamp.
    if (d3d11_renderer_tooltip_visible(renderer) &&
        (!presentation_stamp_pointer(&stamp, renderer.tooltip_scene) ||
         !presentation_stamp_push(&stamp, renderer.tooltip_scene->revision)))
        return stamp;
    if (!presentation_stamp_pointer(&stamp, renderer.friends_requests_scene) ||
        (renderer.friends_requests_scene &&
         (!presentation_stamp_push(&stamp, renderer.friends_requests_scene->active) ||
          !presentation_stamp_push(&stamp, renderer.friends_requests_scene->revision) ||
          !presentation_stamp_push(&stamp, renderer.friends_requests_scene->source_revision) ||
          !presentation_stamp_push(&stamp, renderer.friends_requests_scene->source_count) ||
          !presentation_stamp_push(&stamp, renderer.interaction_text_friends_requests_revision) ||
          !presentation_stamp_widgets(&stamp, renderer.friends_requests_scene->widgets) ||
          !presentation_stamp_bytes(&stamp, &renderer.friends_requests_scene->viewport,
                                    sizeof(renderer.friends_requests_scene->viewport)) ||
          (renderer.friends_requests_scene->source &&
           (!presentation_stamp_push(&stamp, renderer.friends_requests_scene->source->revision) ||
            !presentation_stamp_push(&stamp, renderer.friends_requests_scene->source->count))))))
        return stamp;
    if (friends_active &&
        (!presentation_stamp_bytes(&stamp, &renderer.friends_search_geometry,
                                   sizeof(renderer.friends_search_geometry)) ||
         (renderer.friends_scene->content &&
          (!presentation_stamp_push(&stamp, renderer.friends_scene->content->revision) ||
           !presentation_stamp_push(&stamp, renderer.friends_scene->content->search.focused) ||
           !presentation_stamp_caret(&stamp, renderer.friends_scene->content->search.caret_visible)))))
        return stamp;
    if (!presentation_stamp_pointer(&stamp, renderer.friends_scene) ||
        (renderer.friends_scene &&
         (!presentation_stamp_push(&stamp, renderer.friends_scene->active) ||
          !presentation_stamp_push(&stamp, renderer.friends_scene->revision) ||
          !presentation_stamp_push(&stamp, renderer.friends_scene->source_revision) ||
          !presentation_stamp_push(&stamp, renderer.friends_scene->source_count) ||
          !presentation_stamp_push(&stamp, renderer.interaction_text_friends_revision) ||
          !presentation_stamp_widgets(&stamp, renderer.friends_scene->widgets) ||
          !presentation_stamp_bytes(&stamp, &renderer.friends_scene->viewport,
                                    sizeof(renderer.friends_scene->viewport)) ||
          (renderer.friends_scene->source &&
           (!presentation_stamp_push(&stamp, renderer.friends_scene->source->revision) ||
            !presentation_stamp_push(&stamp, renderer.friends_scene->source->count))))))
        return stamp;
    if (!presentation_stamp_push(&stamp, renderer.reduced_motion) ||
        !presentation_stamp_pointer(&stamp, renderer.settings_scene) ||
        (renderer.settings_scene && (!presentation_stamp_push(&stamp, renderer.settings_scene->revision) ||
                                     !presentation_stamp_push(&stamp, renderer.settings_scene->active) ||
                                     !presentation_stamp_push(&stamp, renderer.settings_scene->spinner_phase))))
        return stamp;
    // Name effects are shader-time driven and repaint through bounded damage
    // (renderer_note_text_effect_damage). Profile effect layers select media
    // on the CPU each tick, so they rebuild the scene.
    const u64 now_ms = renderer.profile_scene ? GetTickCount64() : 0;
    if (profile_scene_animating(renderer.profile_scene, now_ms) &&
        !presentation_stamp_push(&stamp, now_ms / 42))
        return stamp;
    if (!presentation_stamp_pointer(&stamp, renderer.profile_scene) ||
        (renderer.profile_scene &&
         (!presentation_stamp_push(&stamp, renderer.profile_scene->revision) ||
          !presentation_stamp_caret(&stamp, renderer.profile_scene->edit.caret_visible) ||
          !presentation_stamp_push(&stamp, renderer.profile_scene->active) ||
          !presentation_stamp_widgets(&stamp, renderer.profile_scene->widgets))))
        return stamp;
    if (!presentation_stamp_bytes(&stamp, &renderer.scrollbar_viewport,
                                  sizeof(renderer.scrollbar_viewport)))
        return stamp;
    if (!presentation_stamp_pointer(&stamp, renderer.image_viewer) ||
        (renderer.image_viewer && !presentation_stamp_push(&stamp, renderer.image_viewer->revision)))
        return stamp;
    const auto* video = d3d11_renderer_video_scene_visible(renderer) || d3d11_renderer_viewer_video_visible(renderer)
                            ? renderer.video_scene
                            : nullptr;

    if (!presentation_stamp_pointer(&stamp, video) ||
        (video && !presentation_stamp_push(&stamp, ui_video_scene_paint_revision(*video))) ||
        !presentation_stamp_push(&stamp, renderer.interaction_text_video_revision))
        return stamp;
    const auto* audio = d3d11_renderer_audio_scene_visible(renderer) ? renderer.audio_scene : nullptr;
    if (!presentation_stamp_pointer(&stamp, audio) ||
        (audio && !presentation_stamp_push(&stamp, ui_audio_scene_paint_revision(*audio))) ||
        !presentation_stamp_push(&stamp, renderer.interaction_text_audio_revision))
        return stamp;
    const auto* poll = d3d11_renderer_poll_scene_visible(renderer) ? renderer.poll_scene : nullptr;
    const auto* voters =
        d3d11_renderer_poll_voters_visible(renderer) ? renderer.poll_voters_scene : nullptr;
    if (!presentation_stamp_pointer(&stamp, poll) ||
        (poll && !presentation_stamp_push(&stamp, ui_poll_scene_paint_revision(*poll))) ||
        !presentation_stamp_push(&stamp, renderer.interaction_text_poll_revision) ||
        !presentation_stamp_pointer(&stamp, voters) ||
        (voters && !presentation_stamp_push(&stamp, ui_poll_voters_scene_paint_revision(*voters))) ||
        !presentation_stamp_push(&stamp, renderer.interaction_text_poll_voters_revision))
        return stamp;
    if (renderer.message_scene && renderer.message_scene->polls && renderer.message_scene->polls->store) {
        const TimelineInputs timeline(&stamp);
        if (!presentation_stamp_push(&stamp, renderer.message_scene->polls->store->revision))
            return stamp;
    }
    const auto* poll_search = d3d11_renderer_search_visible(renderer)
                                  ? ui_search_scene_message_scene(*renderer.search_scene)
                                  : nullptr;
    if (poll_search && poll_search->polls && poll_search->polls->store &&
        !presentation_stamp_push(&stamp, poll_search->polls->store->revision))
        return stamp;
    const auto* focus_owner = d3d11_renderer_visible_focus_owner(renderer);
    if (!presentation_stamp_pointer(&stamp, focus_owner) ||
        (focus_owner && !presentation_stamp_widgets(&stamp, *focus_owner)))
        return stamp;
    if (!presentation_stamp_push(&stamp, renderer.width) ||
        !presentation_stamp_push(&stamp, renderer.height) ||
        !presentation_stamp_push(&stamp, renderer.dpi) ||
        !presentation_stamp_push(&stamp, renderer.login_visible) ||
        !presentation_stamp_bytes(&stamp, &theme, sizeof(theme)) ||
        !presentation_stamp_pointer(&stamp, renderer.content_text) ||
        !presentation_stamp_pointer(&stamp, renderer.media_atlas) ||
        !presentation_stamp_pointer(&stamp, renderer.preview_media_atlas)) {
        return stamp;
    }
    if (renderer.preview_media_atlas &&
        (!presentation_stamp_push(&stamp, renderer.preview_media_atlas->entry_count) ||
         !presentation_stamp_push(&stamp, renderer.preview_media_atlas->insertions) ||
         !presentation_stamp_push(&stamp, renderer.preview_media_atlas->stale_entries) ||
         !presentation_stamp_push(&stamp, renderer.preview_media_atlas->allocations.evicted_entries) ||
         !presentation_stamp_push(&stamp, renderer.preview_media_atlas->allocations.full_resets) ||
         !presentation_stamp_push(&stamp, renderer.preview_media_atlas->next_allocation_key))) {
        return stamp;
    }
    if (renderer.content_text) {
        const TimelineInputs timeline(&stamp);
        if (!presentation_stamp_push(&stamp, renderer.content_text->rebuilds) ||
            !presentation_stamp_push(&stamp, renderer.content_text->quad_count) ||
            !presentation_stamp_push(&stamp, renderer.content_text->batch_count) ||
            !presentation_stamp_push(&stamp, renderer.content_text->message_geometry_quads) ||
            !presentation_stamp_push(&stamp, renderer.content_text->content_semantic_count) ||
            !presentation_stamp_push(&stamp, renderer.content_text->content_hit_region_count))
            return stamp;
    }
    if (renderer.media_atlas &&
        (!presentation_stamp_push(&stamp, renderer.media_atlas->entry_count) ||
         !presentation_stamp_push(&stamp, renderer.media_atlas->insertions) ||
         !presentation_stamp_push(&stamp, renderer.media_atlas->stale_entries) ||
         !presentation_stamp_push(&stamp, renderer.media_atlas->allocations.evicted_entries) ||
         !presentation_stamp_push(&stamp, renderer.media_atlas->allocations.full_resets) ||
         !presentation_stamp_push(&stamp, renderer.media_atlas->next_allocation_key))) {
        return stamp;
    }
    if (!content_hidden &&
        (!presentation_stamp_pointer(&stamp, renderer.thread_header) ||
         (renderer.thread_header && (!presentation_stamp_push(&stamp, renderer.thread_header->revision) ||
                                     !presentation_stamp_push(&stamp, renderer.thread_header->active))))) {
        return stamp;
    }
    if (!presentation_stamp_pointer(&stamp, renderer.ui_scene)) {
        return stamp;
    }
    if (renderer.ui_scene &&
        (!presentation_stamp_push(&stamp, renderer.ui_scene->count) ||
         !presentation_stamp_push(&stamp, renderer.ui_scene->layout_rebuilds) ||
         !presentation_stamp_push(&stamp, renderer.ui_scene->layout_passes) ||
         !presentation_stamp_push(&stamp, renderer.ui_scene->layout_metrics_revision) ||
         !presentation_stamp_push(&stamp, renderer.ui_scene->generation) ||
         !presentation_stamp_push(&stamp, static_cast<u64>(renderer.ui_scene->view)) ||
         !presentation_stamp_push(&stamp, renderer.ui_scene->guild_menu_expanded) ||
         !presentation_stamp_bytes(&stamp, &renderer.ui_scene->width, sizeof(renderer.ui_scene->width)) ||
         !presentation_stamp_bytes(&stamp, &renderer.ui_scene->height, sizeof(renderer.ui_scene->height)) ||
         !presentation_stamp_widgets(&stamp, renderer.ui_scene->widgets))) {
        return stamp;
    }
    if (!presentation_stamp_pointer(&stamp, renderer.guild_rail_scene)) {
        return stamp;
    }
    if (renderer.guild_rail_scene) {
        const GuildRailScene& scene = *renderer.guild_rail_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_push(&stamp, scene.source_revision) ||
            !presentation_stamp_push(&stamp, scene.source_count) ||
            !presentation_stamp_push(&stamp, scene.first) ||
            !presentation_stamp_push(&stamp, scene.count) ||
            !presentation_stamp_push(&stamp, scene.rebuilds) ||
            !presentation_stamp_push(&stamp, scene.layout_passes) ||
            !presentation_stamp_push(&stamp, scene.data_updates) ||
            !presentation_stamp_push(&stamp, scene.focused_guild_id) ||
            !presentation_stamp_bytes(&stamp, &scene.scroll_dips, sizeof(scene.scroll_dips)) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.shell_navigation_scene)) {
        return stamp;
    }
    if (renderer.shell_navigation_scene) {
        const ShellNavigationScene& scene = *renderer.shell_navigation_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_push(&stamp, scene.source_revision) ||
            !presentation_stamp_push(&stamp, scene.source_count) ||
            !presentation_stamp_push(&stamp, scene.first) ||
            !presentation_stamp_push(&stamp, scene.count) ||
            !presentation_stamp_push(&stamp, scene.rebuilds) ||
            !presentation_stamp_push(&stamp, scene.layout_passes) ||
            !presentation_stamp_push(&stamp, scene.focused_channel_id) ||
            !presentation_stamp_bytes(&stamp, &scene.scroll_dips, sizeof(scene.scroll_dips)) ||
            !presentation_stamp_push(&stamp, scene.banner_revision) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.message_scene)) {
        return stamp;
    }
    if (!content_hidden && renderer.message_scene) {
        const TimelineInputs timeline(&stamp);
        const MessageScene& scene = *renderer.message_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_push(&stamp, scene.dirty) ||
            !presentation_stamp_push(&stamp, scene.focused_message_id) ||
            !presentation_stamp_push(&stamp, scene.observed_scroll_units) ||
            !presentation_stamp_push(&stamp, scene.first) ||
            !presentation_stamp_push(&stamp, scene.count) ||
            !presentation_stamp_push(&stamp, scene.source_count) ||
            !presentation_stamp_push(&stamp, scene.observed_created) ||
            !presentation_stamp_push(&stamp, scene.observed_updated) ||
            !presentation_stamp_push(&stamp, scene.observed_deleted) ||
            !presentation_stamp_push(&stamp, scene.observed_height_updates) ||
            !presentation_stamp_push(&stamp, scene.observed_structural_updates) ||
            !presentation_stamp_push(&stamp, scene.unread_revision) ||
            !presentation_stamp_push(&stamp, scene.unread_first_message_id) ||
            !presentation_stamp_push(&stamp, scene.highlight_revision) ||
            !presentation_stamp_push(&stamp, scene.rebuilds) ||
            !presentation_stamp_push(&stamp, scene.layout_passes) ||
            !presentation_stamp_push(&stamp, scene.data_updates) ||
            !presentation_stamp_push(&stamp, scene.measurement_updates) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.message_action_bar)) {
        return stamp;
    }
    if (!content_hidden && renderer.message_action_bar) {
        const MessageActionBar& bar = *renderer.message_action_bar;
        if (!presentation_stamp_push(&stamp, bar.active) ||
            !presentation_stamp_push(&stamp, bar.message_id) ||
            !presentation_stamp_push(&stamp, bar.revision) ||
            !presentation_stamp_push(&stamp, bar.preference_revision) ||
            !presentation_stamp_push(&stamp, bar.hovered_index) ||
            !presentation_stamp_push(&stamp, bar.item_count)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.reaction_viewer)) {
        return stamp;
    }
    if (renderer.reaction_viewer) {
        const ReactionViewer& viewer = *renderer.reaction_viewer;
        if (!presentation_stamp_push(&stamp, viewer.active) ||
            !presentation_stamp_push(&stamp, viewer.revision) ||
            !presentation_stamp_push(&stamp, viewer.message_id)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.message_composer_scene)) {
        return stamp;
    }
    if (!content_hidden && renderer.message_composer_scene) {
        const MessageComposerScene& scene = *renderer.message_composer_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_pointer(&stamp, scene.model) ||
            !presentation_stamp_push(&stamp, scene.observed_revision) ||
            !presentation_stamp_push(&stamp, scene.composition_revision) ||
            !presentation_stamp_push(&stamp, scene.reply_message_id) ||
            !presentation_stamp_push(&stamp, scene.mention_reply_author) ||
            !presentation_stamp_push(&stamp, scene.reply_mention_visible) ||
            !presentation_stamp_push(&stamp, scene.reply_sendable) ||
            !presentation_stamp_push(&stamp, scene.retained_content) ||
            !presentation_stamp_push(&stamp, scene.status_is_error) ||
            !presentation_stamp_push(&stamp, scene.status_tone) ||
            !presentation_stamp_push(&stamp, scene.reply_label_bytes) ||
            !presentation_stamp_push(&stamp, scene.typing_label_bytes) ||
            !presentation_stamp_push(&stamp, scene.layout_passes) ||
            !presentation_stamp_push(&stamp, scene.channel_allows_send) ||
            !presentation_stamp_caret(&stamp, scene.caret_visible) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.member_list_scene)) {
        return stamp;
    }
    if (!content_hidden && renderer.member_list_scene) {
        const MemberListScene& scene = *renderer.member_list_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_pointer(&stamp, scene.source) ||
            !presentation_stamp_push(&stamp, scene.source_revision) ||
            !presentation_stamp_push(&stamp, scene.source_count) ||
            !presentation_stamp_push(&stamp, scene.first) ||
            !presentation_stamp_push(&stamp, scene.count) ||
            !presentation_stamp_push(&stamp, scene.rebuilds) ||
            !presentation_stamp_push(&stamp, scene.layout_passes) ||
            !presentation_stamp_push(&stamp, scene.focused_user_id) ||
            !presentation_stamp_bytes(&stamp, &scene.scroll_dips, sizeof(scene.scroll_dips)) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.popup_scene)) {
        return stamp;
    }
    if (renderer.popup_scene) {
        const PopupScene& scene = *renderer.popup_scene;
        if (!presentation_stamp_push(&stamp, scene.active) ||
            !presentation_stamp_push(&stamp, scene.revision) ||
            !presentation_stamp_push(&stamp, scene.owner_id) ||
            !presentation_stamp_push(&stamp, scene.item_count) ||
            !presentation_stamp_push(&stamp, scene.selected_index) ||
            !presentation_stamp_push(&stamp, static_cast<u64>(scene.kind)) ||
            !presentation_stamp_widgets(&stamp, scene.widgets)) {
            return stamp;
        }
    }
    if (!presentation_stamp_pointer(&stamp, renderer.text_form))
        return stamp;
    if (renderer.text_form && renderer.text_form->scene) {
        const auto& form = *renderer.text_form->scene;
        if (!presentation_stamp_push(&stamp, form.active) ||
            !presentation_stamp_push(&stamp, form.revision) ||
            !presentation_stamp_widgets(&stamp, form.widgets))
            return stamp;
    }
    if (!presentation_stamp_pointer(&stamp, renderer.navigation_view) ||
        !presentation_stamp_pointer(&stamp, renderer.chat_state) ||
        !presentation_stamp_pointer(&stamp, renderer.user_store)) {
        return stamp;
    }
    if (renderer.navigation_view && !presentation_stamp_push(&stamp, renderer.navigation_view->revision)) {
        return stamp;
    }
    // Conversation rows paint presence dots and typing subtitles from these
    // sources; only the Home view shows them (typing elsewhere repaints nothing).
    if (const auto* rows = renderer.navigation_view && renderer.chat_state && renderer.chat_state->home_selected
                               ? renderer.navigation_view->private_rows
                               : nullptr;
        rows && ((rows->relationships && !presentation_stamp_push(&stamp, rows->relationships->revision)) ||
                 (rows->typing && !presentation_stamp_push(&stamp, rows->typing->revision)))) {
        return stamp;
    }
    if (renderer.chat_state &&
        (!presentation_stamp_push(&stamp, renderer.chat_state->guild_mutations) ||
         !presentation_stamp_push(&stamp, renderer.chat_state->channel_mutations) ||
         !presentation_stamp_push(&stamp, renderer.chat_state->permission_mutations) ||
         !presentation_stamp_push(&stamp, renderer.chat_state->activity_mutations) ||
         !presentation_stamp_push(&stamp, renderer.chat_state->emoji_mutations) ||
         !presentation_stamp_push(&stamp, renderer.chat_state->selection_revision))) {
        return stamp;
    }
    if (renderer.user_store && (!presentation_stamp_push(&stamp, renderer.user_store->revision) ||
                                   !presentation_stamp_push(&stamp, renderer.user_store->mutations))) {
        return stamp;
    }
    if (!content_hidden &&
        (!presentation_stamp_push(&stamp, renderer.composer_layout.active) ||
         !presentation_stamp_push(&stamp, renderer.composer_layout.selection_count))) {
        return stamp;
    }
    if (!content_hidden) {
        const TimelineInputs timeline(&stamp);
        if (!presentation_stamp_push(&stamp, renderer.message_text_selection_message_id) ||
            !presentation_stamp_push(&stamp, renderer.message_text_selection_command_identity) ||
            !presentation_stamp_push(&stamp, renderer.message_text_selection_revision) ||
            !presentation_stamp_push(&stamp, renderer.message_text_selection_command_index) ||
            !presentation_stamp_push(&stamp, renderer.message_text_selection_anchor) ||
            !presentation_stamp_push(&stamp, renderer.message_text_selection_caret))
            return stamp;
    }
    return stamp;
}

bool renderer_note_text_effect_damage(D3D11Renderer* renderer, u64 now_ms) noexcept {
    if (!renderer || !renderer->text_effect_animated || now_ms < renderer->text_effect_next_ms ||
        !renderer->dpi)
        return false;
    const float scale = 96.f / static_cast<float>(renderer->dpi);
    const PresentationDamageRect viewport{0, 0, static_cast<float>(renderer->width) * scale,
                                          static_cast<float>(renderer->height) * scale};
    bool noted = false;
    for (u32 i = 0; i < minimum(renderer->text_effect_count, kTextEffectCapacity); ++i) {
        const auto& material = renderer->text_effects.materials[i];
        if (material.animate == 0 || material.kind == 1)
            continue;
        const UiRect& bounds = renderer->text_effect_bounds[i];
        if (!presentation_damage_add(&renderer->presentation_damage,
                                     {bounds.left, bounds.top, bounds.right, bounds.bottom}, viewport))
            presentation_damage_invalidate_full(&renderer->presentation_damage);
        noted = true;
    }
    // A tick whose damage lies off screen draws nothing; the deadline must
    // still advance or the scheduler would treat the frame as perpetually due.
    renderer->text_effect_next_ms = now_ms + 42;
    return noted;
}

bool renderer_presentation_stamp_equal(const RendererPresentationStamp& left,
                                       const RendererPresentationStamp& right) noexcept {
    return left.valid && right.valid && left.count == right.count && left.primary == right.primary &&
           left.secondary == right.secondary;
}

namespace {

// Covers icon pixel snapping at upload and edge antialiasing.
constexpr float kPaintDiffFringe = 2.f;

void release_paint_diff_storage(RendererPaintDiff* diff) noexcept {
    stable_storage_release(&diff->paint_quads);
    stable_storage_release(&diff->media_quads);
    stable_storage_release(&diff->media_handles);
    stable_storage_release(&diff->media_domains);
    stable_storage_release(&diff->media_batches);
}

bool paint_diff_storage(RendererPaintDiff* diff) noexcept {
    if (diff->paint_quads.arena.base)
        return true;
    if (stable_storage_initialize(&diff->paint_quads, kMaxPaintQuads, 0, sizeof(PaintQuad), alignof(PaintQuad)) &&
        stable_storage_initialize(&diff->media_quads, kRendererMediaQuadCapacity, 0, sizeof(MediaQuad),
                                  alignof(MediaQuad)) &&
        stable_storage_initialize(&diff->media_handles, kRendererMediaQuadCapacity, 0, sizeof(MediaAtlasHandle),
                                  alignof(MediaAtlasHandle)) &&
        stable_storage_initialize(&diff->media_domains, kRendererMediaQuadCapacity, 0, 1, 1) &&
        stable_storage_initialize(&diff->media_batches, kRendererMediaBatchCapacity, 0, sizeof(MediaDrawBatch),
                                  alignof(MediaDrawBatch)))
        return true;
    release_paint_diff_storage(diff);
    return false;
}

bool bytes_equal(const void* left, const void* right, usize count) noexcept {
    const u8* a = static_cast<const u8*>(left);
    const u8* b = static_cast<const u8*>(right);
    for (usize index = 0; index < count; ++index)
        if (a[index] != b[index])
            return false;
    return true;
}

u64 text_signature(const D3D11Renderer& renderer) noexcept {
    if (!renderer.content_text)
        return 0;
    const RendererTextStorage& text = *renderer.content_text;
    const u64 identity = static_cast<u64>(reinterpret_cast<ULONG_PTR>(renderer.content_text));
    const u32 values[] = {text.rebuilds, text.quad_count, text.batch_count, renderer.text_effect_count};
    return hash64_bytes(hash64_bytes(kHash64Seed, &identity, sizeof(identity)), values, sizeof(values));
}

// Batch markers decide how paint, media and text ranges interleave; they are
// contiguous u32 fields from media_overlay_paint_first_batch onward. The
// settings backdrop block between them (textures, its stamp and its media
// record) is not layout: a backdrop redraw invalidates the whole frame.
u64 layout_signature(const D3D11Renderer& renderer) noexcept {
    const u8* first = reinterpret_cast<const u8*>(&renderer.media_overlay_paint_first_batch);
    const u8* backdrop = reinterpret_cast<const u8*>(&renderer.settings_backdrop);
    const u8* resume = reinterpret_cast<const u8*>(&renderer.draw_layer_scale);
    const u8* end = reinterpret_cast<const u8*>(&renderer.interaction_text_action_bar_revision);
    static_assert(offsetof(D3D11Renderer, settings_backdrop) > offsetof(D3D11Renderer, media_overlay_paint_first_batch) &&
                  offsetof(D3D11Renderer, draw_layer_scale) > offsetof(D3D11Renderer, settings_backdrop_failed) &&
                  offsetof(D3D11Renderer, interaction_text_action_bar_revision) > offsetof(D3D11Renderer, draw_layer_scale));
    u64 hash = hash64_bytes(kHash64Seed, first, static_cast<u32>(backdrop - first));
    hash = hash64_bytes(hash, resume, static_cast<u32>(end - resume));
    const u32 inline_range[] = {renderer.inline_media_first_batch, renderer.inline_media_last_batch,
                                renderer.inline_media.count};
    hash = hash64_bytes(hash, inline_range, sizeof(inline_range));
    hash = hash64_bytes(hash, renderer.inline_media_layer_first, sizeof(renderer.inline_media_layer_first));
    return hash64_bytes(hash, renderer.inline_media_layer_end, sizeof(renderer.inline_media_layer_end));
}

bool add_quad_damage(PresentationDamageTracker* tracker, float left, float top, float right, float bottom,
                     PresentationDamageRect clip) noexcept {
    return presentation_damage_add(tracker,
                                   {left - kPaintDiffFringe, top - kPaintDiffFringe, right + kPaintDiffFringe,
                                    bottom + kPaintDiffFringe},
                                   clip);
}

// Adds old/new bounds of the quads between the common prefix and suffix of
// each batch. False means the structures differ and nothing was added.
bool add_paint_damage(D3D11Renderer* renderer, const RendererPaintDiff& diff) noexcept {
    const PaintStream& paint = renderer->paint_stream;
    const auto* previous = stable_storage_data<PaintQuad>(diff.paint_quads);
    for (u32 batch = 0; batch < paint.batch_count; ++batch) {
        const PaintBatch& before = diff.batches[batch];
        const PaintBatch& after = paint.batches[batch];
        if (before.clip_index != after.clip_index || after.clip_index >= paint.clip_count ||
            before.first_quad > diff.paint_count || before.quad_count > diff.paint_count - before.first_quad ||
            after.first_quad > paint.count || after.quad_count > paint.count - after.first_quad)
            return false;
    }
    for (u32 batch = 0; batch < paint.batch_count; ++batch) {
        const PaintBatch& before = diff.batches[batch];
        const PaintBatch& after = paint.batches[batch];
        const PaintQuad* old_quads = previous + before.first_quad;
        const PaintQuad* new_quads = paint.quads + after.first_quad;
        const u32 old_count = before.quad_count, new_count = after.quad_count;
        u32 prefix = 0;
        while (prefix < old_count && prefix < new_count &&
               bytes_equal(&old_quads[prefix], &new_quads[prefix], sizeof(PaintQuad)))
            ++prefix;
        u32 suffix = 0;
        while (suffix < old_count - prefix && suffix < new_count - prefix &&
               bytes_equal(&old_quads[old_count - 1 - suffix], &new_quads[new_count - 1 - suffix], sizeof(PaintQuad)))
            ++suffix;
        const PaintClip& clip = paint.clips[after.clip_index];
        const PresentationDamageRect bounds{clip.left, clip.top, clip.right, clip.bottom};
        for (u32 index = prefix; index < old_count - suffix; ++index) {
            const PaintQuad& quad = old_quads[index];
            if (!add_quad_damage(&renderer->presentation_damage, quad.left, quad.top, quad.right, quad.bottom,
                                 bounds))
                return false;
        }
        for (u32 index = prefix; index < new_count - suffix; ++index) {
            const PaintQuad& quad = new_quads[index];
            if (!add_quad_damage(&renderer->presentation_damage, quad.left, quad.top, quad.right, quad.bottom,
                                 bounds))
                return false;
        }
    }
    return true;
}

// Media batches, handles and domains must match exactly; changed sources and
// pending atlas writes for visible handles become damage, as on a retained frame.
bool add_media_damage(D3D11Renderer* renderer, const RendererPaintDiff& diff) noexcept {
    const u32 count = renderer->media_quad_count;
    const RendererMediaDrawStorage& media = renderer->media_draw;
    if (count != diff.media_count || renderer->media_batch_count != diff.media_batch_count ||
        (renderer->media_batch_count &&
         !bytes_equal(media.batch_data, stable_storage_data<MediaDrawBatch>(diff.media_batches),
                      static_cast<usize>(renderer->media_batch_count) * sizeof(MediaDrawBatch))) ||
        (count && (!bytes_equal(media.handle_data, stable_storage_data<MediaAtlasHandle>(diff.media_handles),
                                static_cast<usize>(count) * sizeof(MediaAtlasHandle)) ||
                   !bytes_equal(media.domain_data, stable_storage_data<u8>(diff.media_domains), count))))
        return false;
    const auto* previous = stable_storage_data<MediaQuad>(diff.media_quads);
    for (u32 batch_index = 0; batch_index < renderer->media_batch_count; ++batch_index) {
        const MediaDrawBatch& batch = media.batch_data[batch_index];
        if (batch.first_quad > count || batch.quad_count > count - batch.first_quad)
            return false;
        const PresentationDamageRect clip{batch.clip.left, batch.clip.top, batch.clip.right, batch.clip.bottom};
        for (u32 index = batch.first_quad; index < batch.first_quad + batch.quad_count; ++index) {
            const MediaQuad& quad = media.quad_data[index];
            const PresentationDamageRect rectangle{quad.left, quad.top, quad.right, quad.bottom};
            if (!presentation_damage_add_media(&renderer->presentation_damage, media.handle_data[index], rectangle,
                                               clip, media.domain_data[index]))
                return false;
            if (bytes_equal(&quad, &previous[index], sizeof(MediaQuad)))
                continue;
            const MediaQuad& old = previous[index];
            if (!add_quad_damage(&renderer->presentation_damage, old.left, old.top, old.right, old.bottom, clip) ||
                !add_quad_damage(&renderer->presentation_damage, quad.left, quad.top, quad.right, quad.bottom,
                                 clip))
                return false;
        }
    }
    return true;
}

template <typename T>
bool record_array(StableStorage* storage, const T* source, u32 count) noexcept {
    if (count == 0)
        return true;
    if (!source || !stable_storage_reserve(storage, count))
        return false;
    CopyMemory(stable_storage_data<T>(*storage), source, static_cast<usize>(count) * sizeof(T));
    return true;
}

} // namespace

bool renderer_paint_diff_damage(D3D11Renderer* renderer, const Theme& theme) noexcept {
    if (!renderer)
        return false;
    RendererPaintDiff& diff = renderer->paint_diff;
    const PaintStream& paint = renderer->paint_stream;
    const RendererPresentationStamp structure = renderer_capture_presentation_stamp(*renderer, theme, true);
    const u64 text = text_signature(*renderer);
    const u64 layout = layout_signature(*renderer);
    u8 reject = !diff.valid ? 1
                : renderer->login_visible || !renderer->content_text || paint.rejected != 0 ||
                        paint.batch_count > kMaxPaintBatches || paint.clip_count > kMaxPaintClips
                    ? 2
                : !renderer_presentation_stamp_equal(structure, diff.structure) ? 3
                : text != diff.text_signature                                  ? 4
                : layout != diff.layout_signature                              ? 5
                : paint.clip_count != diff.clip_count || paint.batch_count != diff.batch_count ||
                        !bytes_equal(paint.clips, diff.clips,
                                     static_cast<usize>(paint.clip_count) * sizeof(PaintClip))
                    ? 6
                    : 0;
    // Damage is staged on a copy so a rejected comparison leaves the tracker
    // exactly as it was (the caller then invalidates the whole frame).
    const PresentationDamageTracker staged = renderer->presentation_damage;
    if (!reject && !add_paint_damage(renderer, diff))
        reject = 7;
    if (!reject && !add_media_damage(renderer, diff))
        reject = 8;
    const bool partial = reject == 0;
    diff.reject_reason = reject;
    if (!partial)
        renderer->presentation_damage = staged;
    diff.valid = false;
    diff.pending = paint_diff_storage(&diff) && paint.batch_count <= kMaxPaintBatches &&
                   paint.clip_count <= kMaxPaintClips &&
                   record_array(&diff.paint_quads, paint.quads, paint.count) &&
                   record_array(&diff.media_quads, renderer->media_draw.quad_data, renderer->media_quad_count) &&
                   record_array(&diff.media_handles, renderer->media_draw.handle_data, renderer->media_quad_count) &&
                   record_array(&diff.media_domains, renderer->media_draw.domain_data, renderer->media_quad_count) &&
                   record_array(&diff.media_batches, renderer->media_draw.batch_data, renderer->media_batch_count);
    if (diff.pending) {
        CopyMemory(diff.clips, paint.clips, static_cast<usize>(paint.clip_count) * sizeof(PaintClip));
        CopyMemory(diff.batches, paint.batches, static_cast<usize>(paint.batch_count) * sizeof(PaintBatch));
        diff.structure = structure;
        diff.text_signature = text;
        diff.layout_signature = layout;
        diff.paint_count = paint.count;
        diff.clip_count = paint.clip_count;
        diff.batch_count = paint.batch_count;
        diff.media_count = renderer->media_quad_count;
        diff.media_batch_count = renderer->media_batch_count;
    }
    if (partial)
        ++diff.partial_rebuilds;
    return partial;
}

void renderer_paint_diff_commit(D3D11Renderer* renderer) noexcept {
    if (renderer && renderer->paint_diff.pending) {
        renderer->paint_diff.valid = true;
        renderer->paint_diff.pending = false;
    }
}

void renderer_paint_diff_release(D3D11Renderer* renderer) noexcept {
    if (!renderer)
        return;
    release_paint_diff_storage(&renderer->paint_diff);
    renderer->paint_diff = RendererPaintDiff{};
}

} // namespace maicord

#include "maicord/render/renderer_presentation.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/core/hash.hpp"
#include "d3d11_renderer_interaction_state.hpp"
#include "d3d11_renderer_edit.hpp"
#include "d3d11_renderer_login_form.hpp"
#include <stddef.h>

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

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

// Folds the inputs a frame depends on into the stamp. An unusable input
// (counter overflow, a null block with a length) invalidates the stamp for good.
class StampWriter {
public:
    explicit StampWriter(RendererPresentationStamp* stamp) noexcept : stamp_(stamp) {}
    StampWriter& push(u64 value) noexcept;
    StampWriter& bytes(const void* bytes, u32 count) noexcept;
    StampWriter& widgets(const UiWidgetManager& widgets) noexcept;
    StampWriter& pointer(const void* pointer) noexcept {
        return push(static_cast<u64>(reinterpret_cast<ULONG_PTR>(pointer)));
    }
    StampWriter& caret(bool visible) noexcept {
        return push(!stamp_->structural && visible);
    }

private:
    RendererPresentationStamp* stamp_;
};

StampWriter& StampWriter::push(u64 value) noexcept {
    RendererPresentationStamp* stamp = stamp_;
    if (!stamp->valid)
        return *this;
    if (stamp->count == ~0u) {
        stamp->valid = false;
        return *this;
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
    return *this;
}

StampWriter& StampWriter::bytes(const void* bytes, u32 count) noexcept {
    if (!bytes && count != 0) {
        stamp_->valid = false;
        return *this;
    }
    const u8* source = static_cast<const u8*>(bytes);
    while (count != 0 && stamp_->valid) {
        const u32 chunk = minimum<u32>(count, sizeof(u64));
        u64 value = 0;
        CopyMemory(&value, source, chunk);
        push(value);
        source += chunk;
        count -= chunk;
    }
    return *this;
}

u64 presentation_handle(UiWidgetHandle handle) noexcept {
    return static_cast<u64>(handle.slot) | (static_cast<u64>(handle.generation) << 16);
}

StampWriter& StampWriter::widgets(const UiWidgetManager& widgets) noexcept {
    return push(widgets.generation)
        .push(widgets.layout.revision)
        .push(presentation_handle(widgets.focused))
        .push(stamp_->structural ? 0 : presentation_handle(widgets.hovered))
        .push(presentation_handle(widgets.captured));
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
    StampWriter s(&stamp);
    s.push(renderer.typography_revision);
    if (d3d11_renderer_login_form_text(renderer)) {
        const auto& owned = *renderer.login_form_text;
        const auto* form = d3d11_renderer_text_form_active(renderer) ? renderer.text_form->scene : nullptr;
        s.push(renderer.width).push(renderer.height).push(renderer.dpi).bytes(&theme, sizeof(theme));
        s.pointer(renderer.login_scene);
        if (renderer.login_scene)
            s.widgets(renderer.login_scene->widgets);
        s.push(owned.valid)
            .push(owned.projection.signature)
            .push(renderer.content_text->rebuilds)
            .push(renderer.content_text->quad_count)
            .push(renderer.content_text->batch_count)
            .push(renderer.overlay_text_first_batch)
            .push(renderer.form_text_first_batch)
            .push(renderer.login_qr_ready);
        if (renderer.login_qr_ready)
            s.bytes(&renderer.login_qr_code, sizeof(renderer.login_qr_code));
        s.pointer(renderer.text_form).pointer(form).push(renderer.interaction_text_form_revision);
        if (form)
            s.push(form->revision).widgets(form->widgets).bytes(&form->viewport, sizeof(form->viewport));
        else
            s.push(renderer.login_caret_visible)
                .bytes(&renderer.login_edit.caret, sizeof(renderer.login_edit.caret))
                .push(renderer.login_edit.selection_count)
                .bytes(renderer.login_edit.selections,
                       renderer.login_edit.selection_count * sizeof(TextEditMetric));
        return stamp;
    }
    if (renderer.forum_form && renderer.forum_form->scene) {
        const auto& form = *renderer.forum_form->scene;
        s.pointer(renderer.forum_form)
            .push(form.content.owner_serial)
            .push(form.active)
            .push(form.revision)
            .push(renderer.interaction_text_forum_form_revision)
            .widgets(form.widgets);
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
                s.bytes(&geometry.text_rectangle, sizeof(UiRect))
                    .bytes(&geometry.caret, sizeof(UiRect))
                    .push(geometry.selections.count)
                    .bytes(geometry.selections.data, geometry.selections.count * sizeof(UiRect))
                    .push(geometry.composition.count)
                    .bytes(geometry.composition.data,
                           geometry.composition.count * sizeof(TextFormCompositionMark));
            }
    }
    if (renderer_settings_page_visible(renderer)) {
        const auto& settings = *renderer.settings_scene;
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        s.pointer(renderer.settings_scene)
            .push(settings.revision)
            .push(settings.motion_revision)
            .widgets(settings.widgets)
            .bytes(&settings.viewport, sizeof(settings.viewport))
            .push(renderer.width)
            .push(renderer.height)
            .push(renderer.dpi)
            .push(renderer.login_visible)
            .bytes(&theme, sizeof(theme))
            .pointer(renderer.content_text)
            .push(renderer.glyph_text.allocator.evicted_entries)
            .push(renderer.glyph_text.allocator.full_resets)
            .push(renderer.glyph_text.ready)
            .push(renderer.interaction_text_settings_revision)
            .push(renderer.interaction_text_active_mask)
            .push(renderer.interaction_text_popup_revision)
            .pointer(popup);
        if (popup)
            s.push(popup->revision).widgets(popup->widgets).bytes(&popup->rectangle, sizeof(UiRect));
        if (renderer.content_text)
            s.push(renderer.content_text->rebuilds)
                .push(renderer.content_text->quad_count)
                .push(renderer.content_text->batch_count)
                .push(renderer.overlay_text_first_batch)
                .push(renderer.form_text_first_batch);
        return stamp;
    }
    s.pointer(renderer.theme_editor_scene);
    if (renderer_theme_editor_visible(renderer))
        s.push(renderer.theme_editor_scene->revision)
            .push(renderer.theme_editor_scene->generation)
            .widgets(renderer.theme_editor_scene->widgets);
    const bool friends_active = d3d11_renderer_active(renderer.friends_scene);
    const bool browser_active = d3d11_renderer_thread_browser_visible(renderer);
    const bool content_hidden = friends_active || browser_active;
    s.pointer(renderer.thread_browser_scene).push(browser_active);
    if (browser_active) {
        const auto& browser = *renderer.thread_browser_scene;
        s.push(browser.revision)
            .push(stamp.structural ? 0 : browser.spinner_phase)
            .push(browser.view->parent_id)
            .push(browser.content_revision)
            .push(browser.source_publication)
            .push(browser.view->revision)
            .push(browser.view->source_publication)
            .push(renderer.interaction_text_thread_browser_revision)
            .push(renderer.interaction_text_thread_browser_publication)
            .widgets(browser.widgets)
            .bytes(&browser.panel, sizeof(browser.panel))
            .bytes(&browser.style, sizeof(browser.style))
            .bytes(&browser.scroll_dips, sizeof(browser.scroll_dips));
    }
    s.pointer(renderer.media_picker_scene).push(d3d11_renderer_media_picker_visible(renderer));
    if (renderer.media_picker_scene) {
        const auto& gallery = *renderer.media_picker_scene;
        s.push(gallery.active)
            .push(gallery.revision)
            .push(gallery.generation)
            .push(gallery.account_scope)
            .push(gallery.query_generation)
            .push(gallery.source_revision)
            .pointer(gallery.source)
            .pointer(gallery.content);
        if (gallery.source)
            s.push(gallery.source->revision)
                .push(gallery.source->account_scope)
                .push(gallery.source->query_generation);
        if (gallery.content)
            s.push(gallery.content->revision)
                .push(gallery.content->account_scope)
                .push(gallery.content->query_generation);
        if (d3d11_renderer_media_picker_visible(renderer))
            s.widgets(gallery.widgets)
                .bytes(&gallery.panel, sizeof(gallery.panel))
                .bytes(&renderer.media_picker_edit_geometry, sizeof(renderer.media_picker_edit_geometry))
                .push(renderer.interaction_text_media_picker_revision);
    }
    if (d3d11_renderer_search_visible(renderer)) {
        s.pointer(renderer.search_scene)
            .push(renderer.search_scene->revision)
            .push(renderer.interaction_text_search_revision)
            .bytes(&renderer.search_edit_geometry, sizeof(renderer.search_edit_geometry))
            .widgets(renderer.search_scene->widgets);
        if (const auto* scene = ui_search_scene_message_scene(*renderer.search_scene))
            s.pointer(scene)
                .widgets(scene->widgets)
                .push(scene->rebuilds)
                .push(scene->layout_passes)
                .pointer(renderer.search_message_text)
                .push(renderer.search_message_text ? renderer.search_message_text->projection_signature : 0);
    }
    if (d3d11_renderer_pinned_messages_visible(renderer)) {
        const auto& pins = *renderer.pinned_messages_scene;
        s.pointer(renderer.pinned_messages_scene)
            .push(pins.revision)
            .push(stamp.structural ? 0 : pins.spinner_phase)
            .push(pins.source_revision)
            .push(pins.content_revision)
            .push(renderer.interaction_text_pinned_messages_revision)
            .widgets(pins.widgets)
            .bytes(&pins.panel, sizeof(pins.panel));
    }
    if (d3d11_renderer_inbox_visible(renderer)) {
        const auto& inbox = *renderer.inbox_scene;
        s.pointer(renderer.inbox_scene)
            .push(inbox.revision)
            .push(stamp.structural ? 0 : inbox.spinner_phase)
            .push(inbox.source_revision)
            .push(inbox.content_revision)
            .push(renderer.interaction_text_inbox_revision)
            .widgets(inbox.widgets)
            .bytes(&inbox.panel, sizeof(inbox.panel));
    }
    if (d3d11_renderer_quick_switcher_visible(renderer)) {
        const auto& quick = *renderer.quick_switcher_scene;
        s.pointer(renderer.quick_switcher_scene)
            .push(quick.revision)
            .push(quick.source_revision)
            .push(quick.content_revision)
            .push(renderer.interaction_text_quick_switcher_revision)
            .widgets(quick.widgets)
            .bytes(&quick.panel, sizeof(quick.panel))
            .bytes(&renderer.media_picker_edit_geometry, sizeof(renderer.media_picker_edit_geometry));
    }
    if (d3d11_renderer_message_forward_visible(renderer)) {
        const auto& forward = *renderer.message_forward_scene;
        const auto caption = d3d11_renderer_popup_edit_geometry(renderer);
        s.pointer(renderer.message_forward_scene)
            .push(forward.revision)
            .push(forward.source_revision)
            .push(forward.content_revision)
            .push(renderer.interaction_text_message_forward_revision)
            .widgets(forward.widgets)
            .bytes(&forward.panel, sizeof(forward.panel))
            .bytes(&renderer.media_picker_edit_geometry, sizeof(renderer.media_picker_edit_geometry))
            .bytes(&caption, sizeof(caption));
    }
    if (d3d11_renderer_call_visible(renderer))
        s.pointer(renderer.call_scene)
            .push(renderer.call_scene->revision)
            .push(stamp.structural ? 0 : renderer.call_scene->spinner_phase)
            .push(renderer.interaction_text_call_revision)
            .bytes(&renderer.call_scene->viewport, sizeof(renderer.call_scene->viewport));
    if (d3d11_renderer_stream_card_visible(renderer))
        s.pointer(renderer.stream_card_scene)
            .push(renderer.stream_card_scene->revision)
            .push(stamp.structural ? 0 : renderer.stream_card_scene->spinner_phase)
            .push(renderer.interaction_text_stream_card_revision)
            .bytes(&renderer.stream_card_scene->panel, sizeof(renderer.stream_card_scene->panel));
    if (d3d11_renderer_guild_card_visible(renderer))
        s.pointer(renderer.guild_card_scene)
            .push(renderer.guild_card_scene->revision)
            .push(stamp.structural ? 0 : renderer.guild_card_scene->spinner_phase)
            .push(renderer.interaction_text_guild_card_revision)
            .bytes(&renderer.guild_card_scene->panel, sizeof(renderer.guild_card_scene->panel));
    if (d3d11_renderer_shortcuts_visible(renderer)) {
        const auto& shortcuts = *renderer.shortcuts_scene;
        s.pointer(renderer.shortcuts_scene)
            .push(shortcuts.revision)
            .push(shortcuts.content_revision)
            .push(renderer.interaction_text_shortcuts_revision)
            .widgets(shortcuts.widgets)
            .bytes(&shortcuts.panel, sizeof(shortcuts.panel))
            .bytes(&shortcuts.scroll, sizeof(shortcuts.scroll));
    }
    if (d3d11_renderer_download_visible(renderer)) {
        const auto& downloads = *renderer.download_scene;
        s.pointer(renderer.download_scene)
            .push(downloads.revision)
            .push(downloads.source_revision)
            .push(downloads.content_revision)
            .push(renderer.interaction_text_download_revision)
            .widgets(downloads.widgets)
            .bytes(&downloads.panel, sizeof(downloads.panel))
            .bytes(&downloads.viewport, sizeof(downloads.viewport));
    }
    if (d3d11_renderer_unread_banner_visible(renderer))
        s.pointer(renderer.unread_banner_scene).push(renderer.unread_banner_scene->revision);
    // A hidden tooltip contributes nothing to the stamp.
    if (d3d11_renderer_tooltip_visible(renderer))
        s.pointer(renderer.tooltip_scene).push(renderer.tooltip_scene->revision);
    if (d3d11_renderer_toast_visible(renderer))
        s.pointer(renderer.toast_scene).push(renderer.toast_scene->revision);
    s.pointer(renderer.friends_requests_scene);
    if (const auto* requests = renderer.friends_requests_scene) {
        s.push(requests->active)
            .push(requests->revision)
            .push(requests->source_revision)
            .push(requests->source_count)
            .push(renderer.interaction_text_friends_requests_revision)
            .widgets(requests->widgets)
            .bytes(&requests->viewport, sizeof(requests->viewport));
        if (requests->source)
            s.push(requests->source->revision).push(requests->source->count);
    }
    if (friends_active) {
        s.bytes(&renderer.friends_search_geometry, sizeof(renderer.friends_search_geometry));
        if (const auto* content = renderer.friends_scene->content)
            s.push(content->revision).push(content->search.focused).caret(content->search.caret_visible);
    }
    s.pointer(renderer.friends_scene);
    if (const auto* friends = renderer.friends_scene) {
        s.push(friends->active)
            .push(friends->revision)
            .push(friends->source_revision)
            .push(friends->source_count)
            .push(renderer.interaction_text_friends_revision)
            .widgets(friends->widgets)
            .bytes(&friends->viewport, sizeof(friends->viewport));
        if (friends->source)
            s.push(friends->source->revision).push(friends->source->count);
    }
    s.push(renderer.reduced_motion).pointer(renderer.settings_scene);
    if (renderer.settings_scene)
        s.push(renderer.settings_scene->revision)
            .push(renderer.settings_scene->active)
            .push(renderer.settings_scene->spinner_phase);
    // Name effects are shader-time driven and repaint through bounded damage
    // (renderer_note_text_effect_damage). Profile effect layers select media
    // on the CPU each tick, so they rebuild the scene.
    const u64 now_ms = renderer.profile_scene ? GetTickCount64() : 0;
    if (profile_scene_animating(renderer.profile_scene, now_ms))
        s.push(now_ms / 42);
    s.pointer(renderer.profile_scene);
    if (renderer.profile_scene)
        s.push(renderer.profile_scene->revision)
            .caret(renderer.profile_scene->edit.caret_visible)
            .push(renderer.profile_scene->active)
            .widgets(renderer.profile_scene->widgets);
    s.bytes(&renderer.scrollbar_viewport, sizeof(renderer.scrollbar_viewport));
    // The loading spinner's phase repaints without rebuilding the viewer's text.
    s.pointer(renderer.image_viewer);
    if (renderer.image_viewer)
        s.push(renderer.image_viewer->revision).push(renderer.image_viewer->spinner_phase);
    s.push(renderer.gif_favorite_owner.slot)
        .push(renderer.gif_favorite_owner.generation)
        .push(renderer.gif_favorite)
        .push(renderer.gif_favorite_pending);
    const auto* video = d3d11_renderer_video_scene_visible(renderer) || d3d11_renderer_viewer_video_visible(renderer)
                            ? renderer.video_scene
                            : nullptr;
    s.pointer(video);
    if (video)
        s.push(ui_video_scene_paint_revision(*video));
    s.push(renderer.interaction_text_video_revision);
    const auto* audio = d3d11_renderer_audio_scene_visible(renderer) ? renderer.audio_scene : nullptr;
    s.pointer(audio);
    if (audio)
        s.push(ui_audio_scene_paint_revision(*audio));
    s.push(renderer.interaction_text_audio_revision);
    const auto* poll = d3d11_renderer_poll_scene_visible(renderer) ? renderer.poll_scene : nullptr;
    const auto* voters = d3d11_renderer_poll_voters_visible(renderer) ? renderer.poll_voters_scene : nullptr;
    s.pointer(poll);
    if (poll)
        s.push(ui_poll_scene_paint_revision(*poll));
    s.push(renderer.interaction_text_poll_revision).pointer(voters);
    if (voters)
        s.push(ui_poll_voters_scene_paint_revision(*voters));
    s.push(renderer.interaction_text_poll_voters_revision);
    if (renderer.message_scene && renderer.message_scene->polls && renderer.message_scene->polls->store) {
        const TimelineInputs timeline(&stamp);
        s.push(renderer.message_scene->polls->store->revision);
    }
    const auto* poll_search =
        d3d11_renderer_search_visible(renderer) ? ui_search_scene_message_scene(*renderer.search_scene) : nullptr;
    if (poll_search && poll_search->polls && poll_search->polls->store)
        s.push(poll_search->polls->store->revision);
    const auto* focus_owner = d3d11_renderer_visible_focus_owner(renderer);
    s.pointer(focus_owner);
    if (focus_owner)
        s.widgets(*focus_owner);
    s.push(renderer.width)
        .push(renderer.height)
        .push(renderer.dpi)
        .push(renderer.login_visible)
        .bytes(&theme, sizeof(theme))
        .pointer(renderer.content_text)
        .pointer(renderer.media_atlas)
        .pointer(renderer.preview_media_atlas);
    if (renderer.preview_media_atlas) {
        const auto& atlas = *renderer.preview_media_atlas;
        s.push(atlas.entry_count)
            .push(atlas.insertions)
            .push(atlas.stale_entries)
            .push(atlas.allocations.evicted_entries)
            .push(atlas.allocations.full_resets)
            .push(atlas.next_allocation_key);
    }
    if (renderer.content_text) {
        const TimelineInputs timeline(&stamp);
        s.push(renderer.content_text->rebuilds)
            .push(renderer.content_text->quad_count)
            .push(renderer.content_text->batch_count)
            .push(renderer.content_text->message_geometry_quads)
            .push(renderer.content_text->content_semantic_count)
            .push(renderer.content_text->content_hit_region_count);
    }
    if (renderer.media_atlas) {
        const auto& atlas = *renderer.media_atlas;
        s.push(atlas.entry_count)
            .push(atlas.insertions)
            .push(atlas.stale_entries)
            .push(atlas.allocations.evicted_entries)
            .push(atlas.allocations.full_resets)
            .push(atlas.next_allocation_key);
    }
    if (!content_hidden) {
        s.pointer(renderer.thread_header);
        if (renderer.thread_header)
            s.push(renderer.thread_header->revision).push(renderer.thread_header->active);
    }
    s.pointer(renderer.ui_scene);
    if (renderer.ui_scene) {
        const auto& scene = *renderer.ui_scene;
        s.push(scene.count)
            .push(scene.layout_rebuilds)
            .push(scene.layout_passes)
            .push(scene.layout_metrics_revision)
            .push(scene.generation)
            .push(static_cast<u64>(scene.view))
            .push(scene.guild_menu_expanded)
            .bytes(&scene.width, sizeof(scene.width))
            .bytes(&scene.height, sizeof(scene.height))
            .widgets(scene.widgets);
    }
    s.pointer(renderer.guild_rail_scene);
    if (renderer.guild_rail_scene) {
        const GuildRailScene& scene = *renderer.guild_rail_scene;
        s.push(scene.active)
            .push(scene.source_revision)
            .push(scene.source_count)
            .push(scene.first)
            .push(scene.count)
            .push(scene.rebuilds)
            .push(scene.layout_passes)
            .push(scene.data_updates)
            .push(scene.focused_guild_id)
            .bytes(&scene.scroll_dips, sizeof(scene.scroll_dips))
            .widgets(scene.widgets);
    }
    s.pointer(renderer.shell_navigation_scene);
    if (renderer.shell_navigation_scene) {
        const ShellNavigationScene& scene = *renderer.shell_navigation_scene;
        s.push(scene.active)
            .push(scene.source_revision)
            .push(scene.source_count)
            .push(scene.first)
            .push(scene.count)
            .push(scene.rebuilds)
            .push(scene.layout_passes)
            .push(scene.focused_channel_id)
            .bytes(&scene.scroll_dips, sizeof(scene.scroll_dips))
            .push(scene.banner_revision)
            .widgets(scene.widgets);
    }
    s.pointer(renderer.message_scene);
    if (!content_hidden && renderer.message_scene) {
        const TimelineInputs timeline(&stamp);
        const MessageScene& scene = *renderer.message_scene;
        s.push(scene.active)
            .push(scene.dirty)
            .push(scene.focused_message_id)
            .push(scene.observed_scroll_units)
            .push(scene.first)
            .push(scene.count)
            .push(scene.source_count)
            .push(scene.observed_created)
            .push(scene.observed_updated)
            .push(scene.observed_deleted)
            .push(scene.observed_height_updates)
            .push(scene.observed_structural_updates)
            .push(scene.unread_revision)
            .push(scene.unread_first_message_id)
            .push(scene.highlight_revision)
            .push(scene.rebuilds)
            .push(scene.layout_passes)
            .push(scene.data_updates)
            .push(scene.measurement_updates)
            .widgets(scene.widgets);
    }
    s.pointer(renderer.message_action_bar);
    if (!content_hidden && renderer.message_action_bar) {
        const MessageActionBar& bar = *renderer.message_action_bar;
        s.push(bar.active)
            .push(bar.message_id)
            .push(bar.revision)
            .push(bar.preference_revision)
            .push(bar.hovered_index)
            .push(bar.item_count);
    }
    s.pointer(renderer.reaction_viewer);
    if (renderer.reaction_viewer) {
        const ReactionViewer& viewer = *renderer.reaction_viewer;
        s.push(viewer.active).push(viewer.revision).push(viewer.message_id);
    }
    s.pointer(renderer.message_composer_scene);
    if (!content_hidden && renderer.message_composer_scene) {
        const MessageComposerScene& scene = *renderer.message_composer_scene;
        s.push(scene.active)
            .pointer(scene.model)
            .push(scene.observed_revision)
            .push(scene.command.name_bytes)
            .push(scene.command.bot_id)
            .push(scene.command.avatar_variant)
            .push(scene.command.animated)
            .push(scene.composition_revision)
            .push(scene.reply_message_id)
            .push(scene.mention_reply_author)
            .push(scene.reply_mention_visible)
            .push(scene.reply_sendable)
            .push(scene.retained_content)
            .push(scene.status_is_error)
            .push(scene.status_tone)
            .push(scene.reply_label_bytes)
            .push(scene.typing_label_bytes)
            .push(scene.layout_passes)
            .push(scene.channel_allows_send)
            .caret(scene.caret_visible)
            .widgets(scene.widgets);
    }
    s.pointer(renderer.member_list_scene);
    if (!content_hidden && renderer.member_list_scene) {
        const MemberListScene& scene = *renderer.member_list_scene;
        s.push(scene.active)
            .pointer(scene.source)
            .push(scene.source_revision)
            .push(scene.source_count)
            .push(scene.first)
            .push(scene.count)
            .push(scene.rebuilds)
            .push(scene.layout_passes)
            .push(scene.focused_user_id)
            .bytes(&scene.scroll_dips, sizeof(scene.scroll_dips))
            .widgets(scene.widgets);
    }
    s.pointer(renderer.popup_scene);
    if (renderer.popup_scene) {
        const PopupScene& scene = *renderer.popup_scene;
        s.push(scene.active)
            .push(scene.revision)
            .push(scene.owner_id)
            .push(scene.item_count)
            .push(scene.selected_index)
            .push(static_cast<u64>(scene.kind))
            .widgets(scene.widgets);
    }
    s.pointer(renderer.text_form);
    if (renderer.text_form && renderer.text_form->scene) {
        const auto& form = *renderer.text_form->scene;
        s.push(form.active).push(form.revision).widgets(form.widgets);
    }
    s.pointer(renderer.navigation_view).pointer(renderer.chat_state).pointer(renderer.user_store);
    if (renderer.navigation_view)
        s.push(renderer.navigation_view->revision);
    // Conversation rows paint presence dots and typing subtitles from these
    // sources; only the Home view shows them (typing elsewhere repaints nothing).
    if (const auto* rows = renderer.navigation_view && renderer.chat_state && renderer.chat_state->home_selected
                               ? renderer.navigation_view->private_rows
                               : nullptr) {
        if (rows->relationships)
            s.push(rows->relationships->revision);
        if (rows->typing)
            s.push(rows->typing->revision);
    }
    if (renderer.chat_state)
        s.push(renderer.chat_state->guild_mutations)
            .push(renderer.chat_state->channel_mutations)
            .push(renderer.chat_state->permission_mutations)
            .push(renderer.chat_state->activity_mutations)
            .push(renderer.chat_state->emoji_mutations)
            .push(renderer.chat_state->selection_revision);
    if (renderer.user_store)
        s.push(renderer.user_store->revision).push(renderer.user_store->mutations);
    if (!content_hidden) {
        s.push(renderer.composer_layout.active).push(renderer.composer_layout.selection_count);
        const TimelineInputs timeline(&stamp);
        s.push(renderer.message_text_selection_message_id)
            .push(renderer.message_text_selection_command_identity)
            .push(renderer.message_text_selection_revision)
            .push(renderer.message_text_selection_command_index)
            .push(renderer.message_text_selection_anchor)
            .push(renderer.message_text_selection_caret);
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

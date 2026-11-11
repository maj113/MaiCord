#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/chat/threads/header.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "renderer_interaction_state.hpp"
#include "renderer_edit.hpp"
#include "renderer_login_form.hpp"
#include "maicord/ui/core/transition.hpp"
#include <stddef.h>

#include "maicord/core/time.hpp"
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

// Folds the inputs a frame depends on into the stamp, a word per value or per
// eight bytes of a block. An unusable input (counter overflow, a null block
// with a length) invalidates the stamp for good.
class StampWriter {
public:
    explicit StampWriter(RendererPresentationStamp* stamp) noexcept : stamp_(stamp) {}
    // A pointer enters as its address, a float as its bits, any other value
    // zero-extended.
    template <typename... Values>
    StampWriter& push(Values... values) noexcept {
        const u64 words[]{word(values)...};
        return bytes(words, sizeof(words));
    }
    // Adjacent fields first through last as one block: every byte from first to
    // the end of last is an input (no padding, no field the frame does not read).
    template <typename First, typename Last>
    __forceinline StampWriter& span(const First& first, const Last& last) noexcept {
        const auto* begin = reinterpret_cast<const u8*>(&first);
        return bytes(begin, static_cast<u32>(reinterpret_cast<const u8*>(&last + 1) - begin));
    }
    StampWriter& bytes(const void* bytes, u32 count) noexcept;
    StampWriter& widgets(const UiWidgetManager& widgets) noexcept;
    StampWriter& atlas(const MediaAtlas& atlas) noexcept {
        return push(atlas.entry_count, atlas.allocations.evicted_entries, atlas.allocations.full_resets)
            .span(atlas.insertions, atlas.next_allocation_key);
    }

private:
    template <typename T>
    static u64 word(T value) noexcept {
        if constexpr (__is_pointer(T))
            return static_cast<u64>(reinterpret_cast<ULONG_PTR>(value));
        else if constexpr (__is_floating_point(T))
            return __builtin_bit_cast(u32, value);
        else
            return static_cast<u64>(value);
    }

    RendererPresentationStamp* stamp_;
};

StampWriter& StampWriter::bytes(const void* bytes, u32 count) noexcept {
    RendererPresentationStamp* stamp = stamp_;
    const u32 words = count / sizeof(u64) + (count % sizeof(u64) != 0);
    if ((!bytes && count != 0) || !stamp->valid || stamp->count >= ~0u - words) {
        stamp->valid = false;
        return *this;
    }
    // The accumulators stay in registers across the block (a theme is
    // hundreds of words).
    const u8* source = static_cast<const u8*>(bytes);
    u64 primary = stamp->primary, secondary = stamp->secondary, shell = stamp->shell;
    u64 ordinal = stamp->count, shell_count = stamp->shell_count;
    const bool shell_input = !stamp->timeline;
    for (u32 left = count; left != 0; ++ordinal) {
        u64 value = 0;
        if (left >= sizeof(u64)) {
            CopyMemory(&value, source, sizeof(value));
            source += sizeof(u64);
            left -= sizeof(u64);
        } else {
            // A block's partial last word is zero-extended.
            while (left != 0)
                value = value << 8 | source[--left];
        }
        primary = (primary ^ (value + 0x9E3779B97F4A7C15ull + ordinal)) * 0x100000001B3ull;
        const u64 mixed =
            (secondary ^ (((value << 29) | (value >> 35)) ^ (ordinal * 0xD6E8FEB86659FD93ull))) * 0xC2B2AE3D27D4EB4Full;
        secondary = (mixed << 23) | (mixed >> 41);
        if (shell_input) {
            const u64 folded = shell ^ (value + 0x9E3779B97F4A7C15ull * ++shell_count);
            shell = (folded ^ (folded >> 31)) * 0xC2B2AE3D27D4EB4Full;
        }
    }
    stamp->primary = primary;
    stamp->secondary = secondary;
    stamp->shell = shell;
    stamp->count = static_cast<u32>(ordinal);
    stamp->shell_count = static_cast<u32>(shell_count);
    return *this;
}

u64 presentation_handle(UiWidgetHandle handle) noexcept {
    return static_cast<u64>(handle.slot) | (static_cast<u64>(handle.generation) << 16);
}

// The theme is hundreds of words and changes rarely: it enters a stamp as a
// digest, folded again only when its bytes differ from the last theme seen.
u64 theme_digest(const Theme& theme) noexcept {
    alignas(Theme) static u8 last[sizeof(Theme)];
    static u64 digest;
    if (!digest || !bytes_equal(last, &theme, sizeof(theme))) {
        copy_bytes(last, &theme, sizeof(theme));
        digest = hash64_bytes(kHash64Seed, &theme, sizeof(theme)) | 1u;
    }
    return digest;
}

StampWriter& StampWriter::widgets(const UiWidgetManager& widgets) noexcept {
    return push(widgets.generation, widgets.layout.revision, presentation_handle(widgets.focused),
                stamp_->structural ? 0 : presentation_handle(widgets.hovered), presentation_handle(widgets.captured));
}

// A profile card's banner fill changes after its layout when its avatar's colour arrives.
u64 avatar_color_token(const ProfileScene* scene) noexcept {
    return scene && scene->has_avatar_color ? (u64{1} << 32) | scene->avatar_color : 0;
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
        s.push(renderer.width, renderer.height, renderer.dpi, theme_digest(theme), renderer.login_scene, owned.valid,
               owned.projection.signature, renderer.content_text->rebuilds, renderer.content_text->quad_count,
               renderer.content_text->batch_count, renderer.overlay_text_first_batch, renderer.form_text_first_batch,
               renderer.login_qr_ready, renderer.text_form, form, renderer.interaction_text_form_revision);
        if (renderer.login_scene)
            s.widgets(renderer.login_scene->widgets);
        if (renderer.login_qr_ready)
            s.bytes(&renderer.login_qr_code, sizeof(renderer.login_qr_code));
        if (form)
            s.push(form->revision).widgets(form->widgets).bytes(&form->viewport, sizeof(form->viewport));
        else
            s.push(renderer.login_caret_visible, renderer.login_edit.selection_count)
                .bytes(&renderer.login_edit.caret, sizeof(renderer.login_edit.caret))
                .bytes(renderer.login_edit.selections,
                       renderer.login_edit.selection_count * sizeof(TextEditMetric));
        return stamp;
    }
    if (renderer.forum_form && renderer.forum_form->scene) {
        const auto& form = *renderer.forum_form->scene;
        s.push(renderer.forum_form, form.content.owner_serial, form.active, form.revision,
               renderer.interaction_text_forum_form_revision)
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
                s.push(geometry.selections.count, geometry.composition.count)
                    .bytes(&geometry.text_rectangle, sizeof(UiRect))
                    .bytes(&geometry.caret, sizeof(UiRect))
                    .bytes(geometry.selections.data, geometry.selections.count * sizeof(UiRect))
                    .bytes(geometry.composition.data,
                           geometry.composition.count * sizeof(TextFormCompositionMark));
            }
    }
    const u32 shown = d3d11_renderer_frame_surfaces(renderer);
    if (shown & d3d11_surface_settings_page) {
        const auto& settings = *renderer.settings_scene;
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        s.push(renderer.settings_scene, settings.revision, settings.motion_revision,
               stamp.structural ? 0 : settings.spinner_phase,
               stamp.structural || !renderer.settings_store ? 0 : renderer.settings_store->spinner_phase,
               renderer.width, renderer.height, renderer.dpi, renderer.login_visible, theme_digest(theme),
               renderer.content_text, renderer.glyph_text.allocator.evicted_entries,
               renderer.glyph_text.allocator.full_resets, renderer.glyph_text.ready,
               renderer.interaction_text_settings_revision, renderer.interaction_text_active_mask,
               renderer.interaction_text_popup_revision, popup, avatar_color_token(renderer.settings_profile_preview))
            .widgets(settings.widgets)
            .bytes(&settings.viewport, sizeof(settings.viewport))
            .span(renderer.scrollbar_hot.track, renderer.scrollbar_hot.state);
        if (popup)
            s.push(popup->revision).widgets(popup->widgets).bytes(&popup->rectangle, sizeof(UiRect));
        if (renderer.content_text)
            s.push(renderer.content_text->rebuilds, renderer.content_text->quad_count,
                   renderer.content_text->batch_count, renderer.overlay_text_first_batch,
                   renderer.form_text_first_batch);
        return stamp;
    }
    s.push(renderer.theme_editor_scene);
    if (shown & d3d11_surface_theme_editor)
        s.push(renderer.theme_editor_scene->revision, renderer.theme_editor_scene->generation)
            .widgets(renderer.theme_editor_scene->widgets);
    const bool friends_active = d3d11_renderer_active(renderer.friends_scene);
    const bool browser_active = (shown & d3d11_surface_thread_browser) != 0;
    const bool content_hidden = friends_active || browser_active;
    s.push(renderer.thread_browser_scene, browser_active);
    if (browser_active) {
        const auto& browser = *renderer.thread_browser_scene;
        s.push(browser.revision, stamp.structural ? 0 : browser.spinner_phase, browser.view->parent_id,
               browser.content_revision, browser.source_publication, browser.view->revision,
               browser.view->source_publication, renderer.interaction_text_thread_browser_revision,
               renderer.interaction_text_thread_browser_publication, browser.scroll_dips)
            .widgets(browser.widgets)
            .bytes(&browser.panel, sizeof(browser.panel))
            .bytes(&browser.style, sizeof(browser.style));
    }
    s.push(renderer.media_picker_scene, (shown & d3d11_surface_media_picker) != 0);
    if (renderer.media_picker_scene) {
        const auto& gallery = *renderer.media_picker_scene;
        s.push(gallery.active, gallery.revision, gallery.generation, gallery.account_scope, gallery.query_generation,
               gallery.source_revision, gallery.source, gallery.content);
        if (gallery.source)
            s.push(gallery.source->revision, gallery.source->account_scope, gallery.source->query_generation);
        if (gallery.content)
            s.span(gallery.content->account_scope, gallery.content->revision);
        if (shown & d3d11_surface_media_picker)
            s.push(gallery.spinner_phase)
                .widgets(gallery.widgets)
                .bytes(&gallery.panel, sizeof(gallery.panel))
                .span(renderer.media_picker_edit_geometry, renderer.interaction_text_media_picker_revision);
    }
    if (shown & d3d11_surface_search) {
        s.push(renderer.search_scene, renderer.search_scene->revision)
            .span(renderer.search_edit_geometry, renderer.interaction_text_search_revision)
            .widgets(renderer.search_scene->widgets);
        if (const auto* scene = ui_search_scene_message_scene(*renderer.search_scene))
            s.push(scene, scene->rebuilds, scene->layout_passes, renderer.search_message_text,
                   renderer.search_message_text ? renderer.search_message_text->projection_signature : 0)
                .widgets(scene->widgets);
    }
    if (shown & d3d11_surface_pinned_messages) {
        const auto& pins = *renderer.pinned_messages_scene;
        s.push(renderer.pinned_messages_scene, pins.revision, stamp.structural ? 0 : pins.spinner_phase,
               pins.source_revision, pins.content_revision, renderer.interaction_text_pinned_messages_revision)
            .widgets(pins.widgets)
            .bytes(&pins.panel, sizeof(pins.panel));
    }
    if (shown & d3d11_surface_inbox) {
        const auto& inbox = *renderer.inbox_scene;
        s.push(renderer.inbox_scene, inbox.revision, stamp.structural ? 0 : inbox.spinner_phase,
               inbox.source_revision, inbox.content_revision, renderer.interaction_text_inbox_revision)
            .widgets(inbox.widgets)
            .bytes(&inbox.panel, sizeof(inbox.panel));
    }
    if (shown & d3d11_surface_quick_switcher) {
        const auto& quick = *renderer.quick_switcher_scene;
        s.push(renderer.quick_switcher_scene, quick.revision, quick.source_revision, quick.content_revision,
               renderer.interaction_text_quick_switcher_revision)
            .widgets(quick.widgets)
            .bytes(&quick.panel, sizeof(quick.panel))
            .bytes(&renderer.media_picker_edit_geometry, sizeof(renderer.media_picker_edit_geometry));
    }
    if (shown & d3d11_surface_message_forward) {
        const auto& forward = *renderer.message_forward_scene;
        const auto caption = d3d11_renderer_popup_edit_geometry(renderer);
        s.push(renderer.message_forward_scene, forward.revision, forward.source_revision, forward.content_revision,
               renderer.interaction_text_message_forward_revision)
            .widgets(forward.widgets)
            .bytes(&forward.panel, sizeof(forward.panel))
            .bytes(&renderer.media_picker_edit_geometry, sizeof(renderer.media_picker_edit_geometry))
            .bytes(&caption, sizeof(caption));
    }
    if (shown & d3d11_surface_call)
        s.push(renderer.call_scene, renderer.call_scene->revision,
               stamp.structural ? 0 : renderer.call_scene->spinner_phase, renderer.interaction_text_call_revision)
            .bytes(&renderer.call_scene->viewport, sizeof(renderer.call_scene->viewport));
    if (shown & d3d11_surface_media_page)
        s.push(renderer.media_page, renderer.media_page->revision,
               stamp.structural ? 0 : renderer.media_page->spinner_phase, renderer.interaction_text_media_page_revision);
    if (shown & d3d11_surface_stream_card)
        s.push(renderer.stream_card_scene, renderer.stream_card_scene->revision,
               stamp.structural ? 0 : renderer.stream_card_scene->spinner_phase,
               renderer.interaction_text_stream_card_revision)
            .bytes(&renderer.stream_card_scene->panel, sizeof(renderer.stream_card_scene->panel));
    if (shown & d3d11_surface_names_card)
        s.push(renderer.names_card_scene, renderer.names_card_scene->revision,
               renderer.interaction_text_names_card_revision)
            .bytes(&renderer.names_card_scene->panel, sizeof(renderer.names_card_scene->panel));
    if (shown & d3d11_surface_guild_card)
        s.push(renderer.guild_card_scene, renderer.guild_card_scene->revision,
               stamp.structural ? 0 : renderer.guild_card_scene->spinner_phase,
               renderer.interaction_text_guild_card_revision)
            .bytes(&renderer.guild_card_scene->panel, sizeof(renderer.guild_card_scene->panel));
    if (shown & d3d11_surface_expression_card)
        s.push(renderer.expression_card_scene, renderer.expression_card_scene->revision,
               renderer.interaction_text_expression_card_revision)
            .bytes(&renderer.expression_card_scene->panel, sizeof(renderer.expression_card_scene->panel));
    if (shown & d3d11_surface_shortcuts) {
        const auto& shortcuts = *renderer.shortcuts_scene;
        s.push(renderer.shortcuts_scene, shortcuts.revision, shortcuts.content_revision,
               renderer.interaction_text_shortcuts_revision, shortcuts.scroll)
            .widgets(shortcuts.widgets)
            .bytes(&shortcuts.panel, sizeof(shortcuts.panel));
    }
    if (shown & d3d11_surface_download) {
        const auto& downloads = *renderer.download_scene;
        s.push(renderer.download_scene, downloads.revision, downloads.source_revision, downloads.content_revision,
               renderer.interaction_text_download_revision)
            .widgets(downloads.widgets)
            .span(downloads.viewport, downloads.panel);
    }
    if (shown & d3d11_surface_go_live) {
        const auto& go_live = *renderer.go_live_scene;
        s.push(renderer.go_live_scene, go_live.revision, go_live.content_revision,
               renderer.interaction_text_go_live_revision, go_live.scroll)
            .widgets(go_live.widgets)
            .bytes(&go_live.panel, sizeof(go_live.panel));
    }
    if (shown & d3d11_surface_unread_banner)
        s.push(renderer.unread_banner_scene, renderer.unread_banner_scene->revision);
    // A hidden tooltip contributes nothing to the stamp.
    if (shown & d3d11_surface_tooltip)
        s.push(renderer.tooltip_scene, renderer.tooltip_scene->revision);
    if (shown & d3d11_surface_toast)
        s.push(renderer.toast_scene, renderer.toast_scene->revision);
    s.push(renderer.friends_requests_scene, renderer.friends_scene, renderer.reduced_motion, renderer.settings_scene);
    if (const auto* requests = renderer.friends_requests_scene) {
        s.push(requests->active, requests->revision, requests->source_revision, requests->source_count,
               renderer.interaction_text_friends_requests_revision)
            .widgets(requests->widgets)
            .bytes(&requests->viewport, sizeof(requests->viewport));
        if (requests->source)
            s.span(requests->source->count, requests->source->revision);
    }
    if (friends_active) {
        s.bytes(&renderer.friends_search_geometry, sizeof(renderer.friends_search_geometry));
        if (const auto* content = renderer.friends_scene->content)
            s.push(content->revision, content->search.focused, !stamp.structural && content->search.caret_visible);
    }
    if (const auto* friends = renderer.friends_scene) {
        s.push(friends->active, friends->revision, friends->source_revision, friends->source_count,
               renderer.interaction_text_friends_revision)
            .widgets(friends->widgets)
            .bytes(&friends->viewport, sizeof(friends->viewport));
        if (friends->source)
            s.span(friends->source->count, friends->source->revision);
        // The rows' hover fade is paint-only state, outside the scene revision.
        if (friends->row_hover)
            s.bytes(friends->row_hover, sizeof(*friends->row_hover));
    }
    if (renderer.settings_scene)
        s.push(renderer.settings_scene->revision, renderer.settings_scene->active,
               renderer.settings_scene->spinner_phase);
    // Name effects are shader-time driven and repaint through bounded damage
    // (renderer_note_text_effect_damage). Profile effect layers select media
    // on the CPU each tick, so they rebuild the scene.
    const u64 now_ms = renderer.profile_scene ? clock_monotonic_milliseconds() : 0;
    if (profile_scene_animating(renderer.profile_scene, now_ms))
        s.push(now_ms / 42);
    s.push(renderer.profile_scene);
    if (renderer.profile_scene)
        s.push(renderer.profile_scene->revision, !stamp.structural && renderer.profile_scene->edit.caret_visible,
               renderer.profile_scene->active, avatar_color_token(renderer.profile_scene))
            .widgets(renderer.profile_scene->widgets);
    s.span(renderer.scrollbar_viewport, renderer.scrollbar_hot.state);
    // The loading spinner's phase repaints without rebuilding the viewer's text.
    s.push(renderer.image_viewer);
    if (renderer.image_viewer)
        s.push(renderer.image_viewer->revision, renderer.image_viewer->spinner_phase);
    // The magnifier follows the pointer and only adds media and paint after
    // the shell: every change of the lens bumps its revision.
    {
        const TimelineInputs timeline(&stamp);
        s.push(renderer.zoom_lens);
        if (renderer.zoom_lens)
            s.push(renderer.zoom_lens->active, renderer.zoom_lens->revision);
    }
    const auto* video = (shown & (d3d11_surface_video | d3d11_surface_viewer_video | d3d11_surface_page_video))
                            ? renderer.video_scene
                            : nullptr;
    const auto* audio = (shown & d3d11_surface_audio) ? renderer.audio_scene : nullptr;
    const auto* poll = (shown & d3d11_surface_poll) ? renderer.poll_scene : nullptr;
    const auto* voters = (shown & d3d11_surface_poll_voters) ? renderer.poll_voters_scene : nullptr;
    s.span(renderer.gif_favorite_owner, renderer.gif_favorite_pending)
        .span(renderer.interaction_text_video_revision, renderer.interaction_text_poll_voters_revision)
        .push(video, video ? ui_video_scene_paint_revision(*video) : 0, audio,
              audio ? ui_audio_scene_paint_revision(*audio) : 0, poll, poll ? ui_poll_scene_paint_revision(*poll) : 0,
              voters, voters ? ui_poll_voters_scene_paint_revision(*voters) : 0);
    if (renderer.message_scene && renderer.message_scene->polls && renderer.message_scene->polls->store) {
        const TimelineInputs timeline(&stamp);
        s.push(renderer.message_scene->polls->store->revision);
    }
    const auto* poll_search =
        (shown & d3d11_surface_search) ? ui_search_scene_message_scene(*renderer.search_scene) : nullptr;
    if (poll_search && poll_search->polls && poll_search->polls->store)
        s.push(poll_search->polls->store->revision);
    const auto* focus_owner = d3d11_renderer_visible_focus_owner(renderer, shown);
    // Every block below is keyed by its scene pointer here.
    s.push(focus_owner, renderer.width, renderer.height, renderer.dpi, renderer.login_visible, theme_digest(theme),
           renderer.content_text, renderer.media_atlas, renderer.preview_media_atlas, renderer.ui_scene,
           renderer.guild_rail_scene, renderer.shell_navigation_scene, renderer.message_scene,
           renderer.message_action_bar, renderer.reaction_viewer, renderer.message_composer_scene,
           renderer.dm_profile_panel, renderer.member_list_scene, renderer.popup_scene);
    if (focus_owner)
        s.widgets(*focus_owner);
    if (renderer.preview_media_atlas)
        s.atlas(*renderer.preview_media_atlas);
    if (renderer.content_text) {
        const TimelineInputs timeline(&stamp);
        s.span(renderer.content_text->quad_count, renderer.content_text->rebuilds)
            .push(renderer.content_text->message_geometry_quads);
    }
    if (renderer.media_atlas)
        s.atlas(*renderer.media_atlas);
    if (!content_hidden) {
        s.push(renderer.thread_header);
        if (renderer.thread_header)
            s.push(renderer.thread_header->revision, renderer.thread_header->active);
    }
    if (renderer.ui_scene) {
        const auto& scene = *renderer.ui_scene;
        s.span(scene.count, scene.layout_metrics_revision)
            .span(scene.view, scene.guild_menu_expanded)
            .widgets(scene.widgets);
    }
    if (renderer.guild_rail_scene) {
        const GuildRailScene& scene = *renderer.guild_rail_scene;
        s.push(scene.active, scene.focused_guild_id).span(scene.scroll_dips, scene.data_updates).widgets(scene.widgets);
    }
    if (renderer.shell_navigation_scene) {
        const ShellNavigationScene& scene = *renderer.shell_navigation_scene;
        s.push(scene.active, scene.focused_channel_id, scene.banner_revision, scene.chevron_category,
               scene.chevron_turn)
            .span(scene.scroll_dips, scene.layout_passes)
            .widgets(scene.widgets);
    }
    if (!content_hidden && renderer.message_scene) {
        const TimelineInputs timeline(&stamp);
        const MessageScene& scene = *renderer.message_scene;
        s.push(scene.active, message_scene_pending(scene), scene.unread_revision, scene.unread_first_message_id,
               scene.highlight_revision, stamp.structural ? 0 : scene.interaction_phase)
            .span(scene.focused_message_id, scene.observed_deleted)
            .span(scene.observed_height_updates, scene.measurement_updates)
            .widgets(scene.widgets);
    }
    if (!content_hidden && renderer.message_action_bar) {
        const MessageActionBar& bar = *renderer.message_action_bar;
        s.push(bar.active, bar.message_id, bar.revision, bar.preference_revision, bar.hovered_index, bar.item_count);
    }
    if (renderer.reaction_viewer) {
        const ReactionViewer& viewer = *renderer.reaction_viewer;
        s.push(viewer.active, viewer.revision, viewer.message_id);
    }
    if (!content_hidden && renderer.message_composer_scene) {
        const MessageComposerScene& scene = *renderer.message_composer_scene;
        s.push(scene.active, scene.model, scene.observed_revision, scene.composition_revision, scene.reply_message_id,
               scene.reply_label_bytes, scene.typing_label_bytes, scene.typing_name_count, scene.typing_collapsible,
               scene.layout_passes, scene.channel_allows_send, !stamp.structural && scene.caret_visible)
            .span(scene.command.avatar, scene.command.name_bytes)
            .span(scene.mention_reply_author, scene.typing_dots)
            .widgets(scene.widgets);
        // The dots' phase is not an input: a step patches their quads in the
        // retained stream (renderer_step_typing_dots).
    }
    if (!content_hidden && renderer.dm_profile_panel)
        s.push(renderer.dm_profile_panel->active, renderer.dm_profile_panel->revision,
               avatar_color_token(renderer.dm_profile_panel))
            .widgets(renderer.dm_profile_panel->widgets);
    if (!content_hidden && renderer.member_list_scene) {
        const MemberListScene& scene = *renderer.member_list_scene;
        s.push(scene.active, scene.source, scene.focused_user_id)
            .span(scene.scroll_dips, scene.source_count)
            .span(scene.first, scene.layout_passes)
            .widgets(scene.widgets);
    }
    if (renderer.popup_scene) {
        const PopupScene& scene = *renderer.popup_scene;
        s.push(scene.active, scene.kind).span(scene.owner_id, scene.revision).widgets(scene.widgets);
    }
    s.push(renderer.text_form);
    if (renderer.text_form && renderer.text_form->scene) {
        const auto& form = *renderer.text_form->scene;
        s.push(form.active, form.revision).widgets(form.widgets);
    }
    s.push(renderer.navigation_view, renderer.chat_state, renderer.user_store);
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
        // A conversation with its context menu open shows its nameplate.
        s.push(ui_private_row_menu_channel(*rows));
    }
    if (renderer.chat_state)
        s.span(renderer.chat_state->guild_mutations, renderer.chat_state->emoji_mutations)
            .push(renderer.chat_state->selection_revision);
    if (renderer.user_store)
        s.span(renderer.user_store->revision, renderer.user_store->mutations);
    if (!content_hidden) {
        s.push(renderer.composer_layout.active, renderer.composer_layout.selection_count);
        const TimelineInputs timeline(&stamp);
        s.span(renderer.message_text_selection_message_id, renderer.message_text_selection_caret);
    }
    return stamp;
}

bool renderer_note_text_effect_damage(D3D11Renderer* renderer, u64 now_ms) noexcept {
    if (!renderer->text_effect_animated || now_ms < renderer->text_effect_next_ms || !renderer->dpi)
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
    u64 hash = hash64_words(kHash64Seed, first, static_cast<u32>(backdrop - first));
    hash = hash64_words(hash, resume, static_cast<u32>(end - resume));
    const u32 inline_range[] = {renderer.inline_media_first_batch, renderer.inline_media_last_batch,
                                renderer.inline_media.count};
    hash = hash64_bytes(hash, inline_range, sizeof(inline_range));
    hash = hash64_bytes(hash, renderer.inline_media_layer_first, sizeof(renderer.inline_media_layer_first));
    return hash64_bytes(hash, renderer.inline_media_layer_end, sizeof(renderer.inline_media_layer_end));
}

template <typename Quad>
bool add_quad_damage(PresentationDamageTracker* tracker, const Quad& quad, PresentationDamageRect clip) noexcept {
    return presentation_damage_add(tracker,
                                   {quad.left - kPaintDiffFringe, quad.top - kPaintDiffFringe,
                                    quad.right + kPaintDiffFringe, quad.bottom + kPaintDiffFringe},
                                   clip);
}

// Adds old/new bounds of the quads between the common prefix and suffix of
// each batch. False means the structures differ and nothing was added.
bool add_paint_damage(D3D11Renderer* renderer, const RendererFrameCopy& diff) noexcept {
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
        for (u32 index = prefix; index < old_count - suffix; ++index)
            if (!add_quad_damage(&renderer->presentation_damage, old_quads[index], bounds))
                return false;
        for (u32 index = prefix; index < new_count - suffix; ++index)
            if (!add_quad_damage(&renderer->presentation_damage, new_quads[index], bounds))
                return false;
    }
    return true;
}

// Media batches, handles and domains must match exactly; changed sources and
// pending atlas writes for visible handles become damage, as on a retained frame.
bool add_media_damage(D3D11Renderer* renderer, const RendererFrameCopy& diff) noexcept {
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
            if (!add_quad_damage(&renderer->presentation_damage, old, clip) ||
                !add_quad_damage(&renderer->presentation_damage, quad, clip))
                return false;
        }
    }
    return true;
}

bool record_bytes(StableStorage* storage, const void* source, u32 count, usize size) noexcept {
    if (count == 0)
        return true;
    if (!source || !stable_storage_reserve(storage, count))
        return false;
    CopyMemory(stable_storage_data<u8>(*storage), source, static_cast<usize>(count) * size);
    return true;
}

template <typename T>
bool record_array(StableStorage* storage, const T* source, u32 count) noexcept {
    return record_bytes(storage, source, count, sizeof(T));
}

} // namespace

void renderer_frame_copy_release(RendererFrameCopy* copy) noexcept {
    stable_storage_release(&copy->paint_quads);
    stable_storage_release(&copy->media_quads);
    stable_storage_release(&copy->media_handles);
    stable_storage_release(&copy->media_domains);
    stable_storage_release(&copy->media_batches);
}

bool renderer_frame_copy_record(RendererFrameCopy* copy, const D3D11Renderer& renderer) noexcept {
    const PaintStream& paint = renderer.paint_stream;
    const RendererMediaDrawStorage& media = renderer.media_draw;
    const u32 count = renderer.media_quad_count, batches = renderer.media_batch_count;
    if (!copy->paint_quads.arena.base &&
        !(stable_storage_initialize(&copy->paint_quads, kMaxPaintQuads, 0, sizeof(PaintQuad), alignof(PaintQuad)) &&
          stable_storage_initialize(&copy->media_quads, kRendererMediaQuadCapacity, 0, sizeof(MediaQuad),
                                    alignof(MediaQuad)) &&
          stable_storage_initialize(&copy->media_handles, kRendererMediaQuadCapacity, 0, sizeof(MediaAtlasHandle),
                                    alignof(MediaAtlasHandle)) &&
          stable_storage_initialize(&copy->media_domains, kRendererMediaQuadCapacity, 0, 1, 1) &&
          stable_storage_initialize(&copy->media_batches, kRendererMediaBatchCapacity, 0, sizeof(MediaDrawBatch),
                                    alignof(MediaDrawBatch)))) {
        renderer_frame_copy_release(copy);
        return false;
    }
    if (paint.batch_count > kMaxPaintBatches || paint.clip_count > kMaxPaintClips ||
        !record_array(&copy->paint_quads, paint.quads, paint.count) ||
        !record_array(&copy->media_quads, media.quad_data, count) ||
        !record_array(&copy->media_handles, media.handle_data, count) ||
        !record_array(&copy->media_domains, media.domain_data, count) ||
        !record_array(&copy->media_batches, media.batch_data, batches))
        return false;
    CopyMemory(copy->clips, paint.clips, static_cast<usize>(paint.clip_count) * sizeof(PaintClip));
    CopyMemory(copy->batches, paint.batches, static_cast<usize>(paint.batch_count) * sizeof(PaintBatch));
    copy->paint_count = paint.count;
    copy->clip_count = paint.clip_count;
    copy->batch_count = paint.batch_count;
    copy->media_count = count;
    copy->media_batch_count = batches;
    return true;
}

bool renderer_paint_diff_damage(D3D11Renderer* renderer, const Theme& theme) noexcept {
    RendererPaintDiff& diff = renderer->paint_diff;
    const PaintStream& paint = renderer->paint_stream;
    const RendererPresentationStamp structure = renderer_capture_presentation_stamp(*renderer, theme, true);
    const u64 text = text_signature(*renderer);
    const u64 layout = layout_signature(*renderer);
    // Damage is staged on a copy so a rejected comparison leaves the tracker
    // exactly as it was (the caller then invalidates the whole frame).
    const PresentationDamageTracker staged = renderer->presentation_damage;
    const bool partial =
        diff.valid && !renderer->login_visible && renderer->content_text && paint.rejected == 0 &&
        paint.batch_count <= kMaxPaintBatches && paint.clip_count <= kMaxPaintClips &&
        renderer_presentation_stamp_equal(structure, diff.structure) && text == diff.text_signature &&
        layout == diff.layout_signature && paint.clip_count == diff.frame.clip_count &&
        paint.batch_count == diff.frame.batch_count &&
        bytes_equal(paint.clips, diff.frame.clips, static_cast<usize>(paint.clip_count) * sizeof(PaintClip)) &&
        add_paint_damage(renderer, diff.frame) && add_media_damage(renderer, diff.frame);
    if (!partial)
        copy_value(&renderer->presentation_damage, staged);
    diff.valid = false;
    diff.pending = renderer_frame_copy_record(&diff.frame, *renderer);
    if (diff.pending) {
        diff.structure = structure;
        diff.text_signature = text;
        diff.layout_signature = layout;
    }
    if (partial)
        ++diff.partial_rebuilds;
    return partial;
}

void renderer_record_typing_dots(D3D11Renderer* renderer, const UiTypingDotsPaint& paint) noexcept {
    auto& dots = renderer->typing_dots;
    dots.paint = paint;
    dots.gpu_first = paint.first_quad;
    dots.upload_pending = false;
}

bool renderer_typing_dots_pending(const D3D11Renderer& renderer) noexcept {
    return renderer.typing_dots.paint.quads && renderer.message_composer_scene &&
           renderer.message_composer_scene->typing_phase != renderer.typing_dots.paint.phase;
}

bool renderer_step_typing_dots(D3D11Renderer* renderer) noexcept {
    auto& dots = renderer->typing_dots;
    const UiTypingDotsPaint& p = dots.paint;
    PaintStream& stream = renderer->paint_stream;
    if (!renderer->message_composer_scene || p.quads != kUiTypingDotCount || p.first_quad > stream.count ||
        stream.count - p.first_quad < kUiTypingDotCount)
        return false;
    // Full-size dots inside the clip: no phase culls one, so the count holds.
    PaintQuad before[kUiTypingDotCount], after[kUiTypingDotCount];
    ui_typing_dots_quads(p.row, p.color, kUiTypingDotsRest, after);
    for (const auto& quad : after)
        if (quad.left < p.clip.left || quad.top < p.clip.top || quad.right > p.clip.right ||
            quad.bottom > p.clip.bottom)
            return false;
    PaintQuad* quads = stream.quads + p.first_quad;
    ui_typing_dots_quads(p.row, p.color, p.phase, before);
    if (!bytes_equal(quads, before, sizeof(before)))
        return false;
    const u16 phase = renderer->message_composer_scene->typing_phase;
    ui_typing_dots_quads(p.row, p.color, phase, after);
    const PresentationDamageRect clip{p.clip.left, p.clip.top, p.clip.right, p.clip.bottom};
    for (u32 i = 0; i < kUiTypingDotCount; ++i)
        if (!add_quad_damage(&renderer->presentation_damage, before[i], clip) ||
            !add_quad_damage(&renderer->presentation_damage, after[i], clip))
            presentation_damage_invalidate_full(&renderer->presentation_damage);
    CopyMemory(quads, after, sizeof(after));
    // The diff record mirrors the built stream: keep it equal to the screen.
    RendererPaintDiff& diff = renderer->paint_diff;
    if ((diff.valid || diff.pending) && diff.frame.paint_count == stream.count)
        CopyMemory(stable_storage_data<PaintQuad>(diff.frame.paint_quads) + p.first_quad, after, sizeof(after));
    dots.paint.phase = phase;
    dots.upload_pending = true;
    ++dots.steps;
    return true;
}

void renderer_paint_diff_commit(D3D11Renderer* renderer) noexcept {
    if (renderer->paint_diff.pending) {
        renderer->paint_diff.valid = true;
        renderer->paint_diff.pending = false;
    }
}

void renderer_paint_diff_release(D3D11Renderer* renderer) noexcept {
    renderer_frame_copy_release(&renderer->paint_diff.frame);
    reset_value<RendererPaintDiff>(&renderer->paint_diff);
}

} // namespace maicord

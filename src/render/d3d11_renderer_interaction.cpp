#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/ui/thread_header.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "d3d11_renderer_interaction_state.hpp"

namespace maicord {
void renderer_bind_focus_owner(D3D11Renderer* renderer, const UiWidgetManager* owner) noexcept {
    if (renderer)
        renderer->focus_owner = owner;
}

bool renderer_bind_audio_scene(D3D11Renderer* renderer, const UiAudioScene* scene) noexcept {
    if (!renderer ||
        (scene && scene->active &&
         (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle))))
        return false;
    if (renderer->audio_scene == scene && (!scene || !renderer->video_scene))
        return true;
    renderer->audio_scene = scene;
    if (scene)
        renderer->video_scene = nullptr;
    renderer->interaction_text_audio_revision = ~0ull;
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}

bool renderer_bind_poll_scene(D3D11Renderer* renderer, const UiPollScene* scene) noexcept {
    if (!renderer ||
        (scene && scene->active &&
         (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle))))
        return false;
    if (renderer->poll_scene == scene)
        return true;
    renderer->poll_scene = scene;
    renderer->interaction_text_poll_revision = ~u64{0};
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}

bool renderer_bind_poll_voters_scene(D3D11Renderer* renderer, const UiPollVotersScene* scene) noexcept {
    if (!renderer ||
        (scene && scene->active &&
         (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle))))
        return false;
    if (renderer->poll_voters_scene == scene)
        return true;
    renderer->poll_voters_scene = scene;
    renderer->interaction_text_poll_voters_revision = ~u64{0};
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}

bool renderer_bind_ui_scene(D3D11Renderer* renderer, const UiScene* scene) noexcept {
    if (!renderer || !scene || scene->count == 0 ||
        !ui_scene_resolve(*scene, ui_scene_find(*scene, UiSemantic::root))) {
        return false;
    }
    renderer->ui_scene = scene;
    return true;
}

bool renderer_bind_thread_header(D3D11Renderer* renderer, const UiThreadHeader* header) noexcept {
    if (!renderer || (header && header->active && !header->manager))
        return false;
    if (renderer->thread_header == header)
        return true;
    renderer->thread_header = header;
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}

bool renderer_bind_guild_rail_scene(D3D11Renderer* renderer, const GuildRailScene* scene) noexcept {
    if (!renderer || !scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->guild_rail_scene = scene;
    return true;
}

bool renderer_bind_shell_navigation_scene(D3D11Renderer* renderer,
                                          const ShellNavigationScene* scene) noexcept {
    if (!renderer || !scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->shell_navigation_scene = scene;
    return true;
}

bool renderer_bind_message_scene(D3D11Renderer* renderer, const MessageScene* scene) noexcept {
    if (!renderer || !scene || !scene->timeline || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->message_scene = scene;
    return true;
}

bool renderer_bind_message_action_bar(D3D11Renderer* renderer, const MessageActionBar* bar) noexcept {
    if (!renderer || !bar) {
        return false;
    }
    renderer->message_action_bar = bar;
    return true;
}

bool renderer_bind_message_composer_scene(D3D11Renderer* renderer,
                                          const MessageComposerScene* scene) noexcept {
    if (!renderer || !scene || !scene->widgets.storage.data) {
        return false;
    }
    if (renderer->message_composer_scene != scene)
        text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->message_composer_scene = scene;
    return true;
}

bool renderer_bind_member_list_scene(D3D11Renderer* renderer, const MemberListScene* scene) noexcept {
    if (!renderer || !scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->member_list_scene = scene;
    return true;
}

bool renderer_bind_popup_scene(D3D11Renderer* renderer, const PopupScene* scene) noexcept {
    // Only the widget manager is required; popup item/row storage is created on first open.
    if (!renderer || !scene || !scene->widgets.storage.data) {
        return false;
    }
    renderer->popup_scene = scene;
    return true;
}

bool renderer_bind_reaction_viewer(D3D11Renderer* renderer, const ReactionViewer* viewer) noexcept {
    if (!renderer || !viewer) {
        return false;
    }
    renderer->reaction_viewer = viewer;
    return true;
}

void renderer_bind_forum_form(D3D11Renderer* renderer, ForumPostSurface* surface) noexcept {
    if (!renderer || renderer->forum_form == surface)
        return;
    renderer->forum_form = surface;
    renderer->interaction_text_forum_form_revision =
        ~(surface && surface->scene ? surface->scene->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}

void renderer_bind_text_form(D3D11Renderer* renderer, TextFormSurface* surface) noexcept {
    if (!renderer)
        return;
    renderer->text_form = surface;
    renderer->interaction_text_form_revision = ~(surface && surface->scene ? surface->scene->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}

void renderer_bind_settings_scene(D3D11Renderer* renderer, const UiSettingsScene* scene) noexcept {
    if (!renderer)
        return;
    if (renderer->settings_scene != scene) {
        // A newly opened dialog draws the live shell beneath it on its first frame.
        renderer_release_settings_backdrop(renderer);
        renderer->settings_backdrop_failed = false;
    }
    renderer->settings_scene = scene;
    if (scene && scene->active) {
        reset_value(&renderer->friends_search_geometry);
        text_edit_layout_invalidate(&renderer->composer_layout);
    }
    renderer->interaction_text_settings_revision = ~(scene ? scene->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}

bool renderer_bind_message_text_presentation(D3D11Renderer* renderer,
                                             const MessageTextPresentation* presentation) noexcept {
    if (!renderer || !presentation || !presentation->commands.data || !presentation->measurements.data ||
        renderer->message_text) {
        return false;
    }
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    content_text_retention_invalidate(&renderer->content_text_retention);
    renderer->message_text = presentation;
    renderer->message_measure_cache = nullptr;
    return true;
}

void renderer_bind_friends_scene(D3D11Renderer* renderer, const UiFriendsScene* scene) noexcept {
    if (!renderer || renderer->friends_scene == scene)
        return;
    renderer->friends_scene = scene;
    reset_value(&renderer->friends_search_geometry);
    text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->interaction_text_friends_revision = ~(scene ? scene->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}
void renderer_bind_friends_requests_scene(D3D11Renderer* renderer, const UiFriendsScene* scene) noexcept {
    if (!renderer || renderer->friends_requests_scene == scene)
        return;
    renderer->friends_requests_scene = scene;
    reset_value(&renderer->friends_search_geometry);
    text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->interaction_text_friends_requests_revision = ~(scene ? scene->revision : 0u);
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}

void renderer_unbind_message_text_presentation(D3D11Renderer* renderer) noexcept {
    if (renderer) {
        renderer->message_text = nullptr;
        renderer->message_measure_cache = nullptr;
        message_hit_test_cache_invalidate(&renderer->message_hit_test);
        content_text_retention_invalidate(&renderer->content_text_retention);
        renderer_clear_message_text_selection(renderer);
    }
}

bool renderer_bind_search_message_text_presentation(D3D11Renderer* renderer,
                                                    const MessageTextPresentation* presentation,
                                                    const UserStore* users,
                                                    UiTextMeasureCache* cache) noexcept {
    if (!renderer || !presentation || !presentation->commands.data || !presentation->measurements.data ||
        !users || !cache)
        return false;
    if (renderer->search_message_text == presentation && renderer->search_message_users == users &&
        renderer->search_message_measure_cache == cache)
        return true;
    renderer->search_message_text = presentation;
    renderer->search_message_users = users;
    renderer->search_message_measure_cache = cache;
    renderer->retained_presentation.valid = false;
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    content_text_retention_invalidate(&renderer->content_text_retention);
    return true;
}
void renderer_unbind_search_message_text_presentation(D3D11Renderer* renderer) noexcept {
    if (!renderer)
        return;
    if (!renderer->search_message_text && !renderer->search_message_users &&
        !renderer->search_message_measure_cache)
        return;
    renderer->search_message_text = nullptr;
    renderer->search_message_users = nullptr;
    renderer->search_message_measure_cache = nullptr;
    renderer->retained_presentation.valid = false;
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    content_text_retention_invalidate(&renderer->content_text_retention);
}

bool renderer_login_edit_hit_test(const D3D11Renderer& renderer, const UiRect& text_rectangle,
                                  Utf8View visible_text, i32 client_x_pixels, i32 client_y_pixels,
                                  u32* codepoint) noexcept {
    if (!codepoint || !renderer.login_edit.active || renderer.dpi == 0 ||
        !ui_rect_has_area(text_rectangle) || (!visible_text.data && visible_text.bytes != 0)) {
        return false;
    }
    const Utf8Validation validation = utf8_validate(visible_text);
    u32 utf16_units = 0;
    if (!validation.valid ||
        !utf8_utf16_position_for_codepoint(visible_text, validation.codepoints, &utf16_units) ||
        utf16_units != renderer.login_edit.utf16_units ||
        visible_text.bytes != renderer.login_edit.text_bytes ||
        text_edit_signature(visible_text) != renderer.login_edit.text_signature) {
        return false;
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    if (renderer.login_scene && renderer.login_scene->phone_mode) {
        bool inside = false;
        return login_phone_point_position(text_rectangle, visible_text.bytes, x, y, true, codepoint,
                                          &inside);
    }
    if (visible_text.bytes == 0) {
        *codepoint = 0;
        return true;
    }
    u32 position = 0;
    if (!text_edit_layout_hit_test(renderer.login_edit, x, y, &position)) {
        return false;
    }
    return utf8_codepoint_for_utf16_position(visible_text, position, codepoint);
}

bool renderer_message_composer_edit_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                             i32 client_y_pixels, u32* model_byte_offset) noexcept {
    const MessageComposerScene* scene = renderer.message_composer_scene;
    if (!model_byte_offset || !scene || !scene->active || !scene->model ||
        d3d11_renderer_active(renderer.friends_scene) ||
        d3d11_renderer_active(renderer.settings_scene) || !renderer.composer_layout.active ||
        renderer.dpi == 0) {
        return false;
    }
    const UiLayoutNode* edit = message_composer_scene_layout_node(*scene, MessageComposerControl::edit);
    const Utf8View text = message_composer_scene_display_text(*scene);
    if (!edit || text.bytes != renderer.composer_layout.text_bytes ||
        scene->display_signature != renderer.composer_layout.text_signature) {
        return false;
    }
    if (text.bytes == 0) {
        *model_byte_offset = scene->display_first;
        return true;
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    u32 position = 0;
    u32 display_byte_offset = 0;
    if (!text_edit_layout_hit_test(renderer.composer_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(text, position, &display_byte_offset) ||
        display_byte_offset > scene->display_bytes) {
        return false;
    }
    *model_byte_offset = message_composer_scene_model_offset_from_display(*scene, display_byte_offset);
    return true;
}

bool renderer_message_composer_edit_move_vertical(const D3D11Renderer& renderer, bool down,
                                                  u32* model_byte_offset) noexcept {
    const MessageComposerScene* scene = renderer.message_composer_scene;
    if (!model_byte_offset || !scene || !scene->active || !scene->model ||
        d3d11_renderer_active(renderer.friends_scene) ||
        d3d11_renderer_active(renderer.settings_scene) || !renderer.composer_layout.active) {
        return false;
    }
    const UiLayoutNode* edit = message_composer_scene_layout_node(*scene, MessageComposerControl::edit);
    const Utf8View text = message_composer_scene_display_text(*scene);
    if (!edit || text.bytes != renderer.composer_layout.text_bytes ||
        scene->display_signature != renderer.composer_layout.text_signature) {
        return false;
    }
    if (text.bytes == 0) {
        *model_byte_offset = scene->display_first;
        return true;
    }
    u32 position = 0;
    u32 display_byte_offset = 0;
    if (!text_edit_layout_move_vertical(renderer.composer_layout, down, &position) ||
        !utf8_byte_offset_for_utf16_position(text, position, &display_byte_offset) ||
        display_byte_offset > scene->display_bytes) {
        return false;
    }
    *model_byte_offset = message_composer_scene_model_offset_from_display(*scene, display_byte_offset);
    return true;
}

void renderer_set_login_caret_visible(D3D11Renderer* renderer, bool visible) noexcept {
    if (renderer) {
        renderer->login_caret_visible = visible && renderer->login_edit.active;
    }
}

bool renderer_message_view_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                    i32 client_y_pixels) noexcept {
    if (renderer.dpi == 0 || client_x_pixels < 0 || client_y_pixels < 0 || !renderer.message_scene ||
        !renderer.message_scene->active) {
        return false;
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const UiRect& viewport = renderer.message_scene->viewport;
    return x >= viewport.left && x < viewport.right && y >= viewport.top && y < viewport.bottom;
}

MessageTextHit renderer_content_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                         i32 client_y_pixels) noexcept {
    if (!renderer.content_text || renderer.dpi == 0 || client_x_pixels < 0 || client_y_pixels < 0) {
        return MessageTextHit{};
    }
    const RendererTextStorage& storage = *renderer.content_text;
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    for (u32 cursor = storage.content_hit_region_count; cursor != 0; --cursor) {
        const TextHitRegion& region = storage.content_hit_regions[cursor - 1];
        if (x < region.left || x >= region.right || y < region.top || y >= region.bottom ||
            region.semantic_index >= storage.content_semantic_count) {
            continue;
        }
        const MessageTextSemantic& semantic = storage.content_semantics[region.semantic_index];
        if (semantic.reserved & kRendererSearchSemanticFlag)
            continue;
        // Message rows clip to themselves: only the visible timeline takes hits.
        if (semantic.message_id != 0 &&
            !renderer_message_view_hit_test(renderer, client_x_pixels, client_y_pixels))
            continue;
        return MessageTextHit{semantic.message_id,
                              semantic.target,
                              semantic.message_revision,
                              semantic.source_offset,
                              semantic.source_bytes,
                              semantic.kind,
                              true};
    }
    return MessageTextHit{};
}

RendererMessageTextPositionHit renderer_message_text_position_hit_test(D3D11Renderer& renderer,
                                                                       i32 client_x_pixels,
                                                                       i32 client_y_pixels) noexcept {
    const MessageTextPresentation* presentation = renderer.message_text;
    if (!presentation || !renderer.message_scene || !renderer.message_scene->active ||
        !ui_rect_has_area(renderer.message_scene->viewport) ||
        !renderer_message_view_hit_test(renderer, client_x_pixels, client_y_pixels)) {
        return RendererMessageTextPositionHit{};
    }
    return message_position_hit_test(&renderer.message_hit_test, *presentation,
                                            renderer.glyph_text.system, renderer.dpi, client_x_pixels,
                                            client_y_pixels);
}

void renderer_clear_message_text_selection(D3D11Renderer* renderer) noexcept {
    if (!renderer) {
        return;
    }
    renderer->message_text_selection_message_id = 0;
    renderer->message_text_selection_command_identity = 0;
    renderer->message_text_selection_revision = 0;
    renderer->message_text_selection_command_index = 0;
    renderer->message_text_selection_anchor = 0;
    renderer->message_text_selection_caret = 0;
}

void renderer_set_message_text_selection(D3D11Renderer* renderer,
                                         const RendererMessageTextPositionHit& command, u32 anchor,
                                         u32 caret) noexcept {
    if (!renderer || !command.valid || command.message_id == 0 || anchor > command.text_bytes ||
        caret > command.text_bytes) {
        renderer_clear_message_text_selection(renderer);
        return;
    }
    renderer->message_text_selection_message_id = command.message_id;
    renderer->message_text_selection_command_identity = command.command_identity;
    renderer->message_text_selection_revision = command.message_revision;
    renderer->message_text_selection_command_index = command.command_index;
    renderer->message_text_selection_anchor = anchor;
    renderer->message_text_selection_caret = caret;
}

} // namespace maicord

#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/ui/chat/threads/header.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/chat/search/scene.hpp"
#include "renderer_interaction_state.hpp"

namespace maicord {
namespace {
// A bound scene changed what the frame shows: the retained presentation and
// the retained content text are stale.
void invalidate_retained_frame(D3D11Renderer* renderer) noexcept {
    renderer->retained_presentation.valid = false;
    content_text_retention_invalidate(&renderer->content_text_retention);
}
} // namespace

void renderer_bind_focus_owner(D3D11Renderer* renderer, const UiWidgetManager* owner) noexcept {
    renderer->focus_owner = owner;
}

bool renderer_bind_audio_scene(D3D11Renderer* renderer, const UiAudioScene* scene) noexcept {
    if (scene && scene->active &&
        (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle)))
        return false;
    if (renderer->audio_scene == scene && (!scene || !renderer->video_scene))
        return true;
    renderer->audio_scene = scene;
    if (scene)
        renderer->video_scene = nullptr;
    renderer->interaction_text_audio_revision = ~0ull;
    invalidate_retained_frame(renderer);
    return true;
}

bool renderer_bind_poll_scene(D3D11Renderer* renderer, const UiPollScene* scene) noexcept {
    if (scene && scene->active &&
        (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle)))
        return false;
    if (renderer->poll_scene == scene)
        return true;
    renderer->poll_scene = scene;
    renderer->interaction_text_poll_revision = ~u64{0};
    invalidate_retained_frame(renderer);
    return true;
}

bool renderer_bind_poll_voters_scene(D3D11Renderer* renderer, const UiPollVotersScene* scene) noexcept {
    if (scene && scene->active &&
        (!scene->owner.manager || !ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle)))
        return false;
    if (renderer->poll_voters_scene == scene)
        return true;
    renderer->poll_voters_scene = scene;
    renderer->interaction_text_poll_voters_revision = ~u64{0};
    invalidate_retained_frame(renderer);
    return true;
}

bool renderer_bind_ui_scene(D3D11Renderer* renderer, const UiScene* scene) noexcept {
    if (!scene || scene->count == 0 || !ui_scene_node(*scene, UiSemantic::root)) {
        return false;
    }
    renderer->ui_scene = scene;
    return true;
}

bool renderer_bind_thread_header(D3D11Renderer* renderer, const UiThreadHeader* header) noexcept {
    if (header && header->active && !header->manager)
        return false;
    if (renderer->thread_header == header)
        return true;
    renderer->thread_header = header;
    invalidate_retained_frame(renderer);
    return true;
}

bool renderer_bind_guild_rail_scene(D3D11Renderer* renderer, const GuildRailScene* scene) noexcept {
    if (!scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->guild_rail_scene = scene;
    return true;
}

bool renderer_bind_shell_navigation_scene(D3D11Renderer* renderer,
                                          const ShellNavigationScene* scene) noexcept {
    if (!scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->shell_navigation_scene = scene;
    return true;
}

bool renderer_bind_message_scene(D3D11Renderer* renderer, const MessageScene* scene) noexcept {
    if (!scene || !scene->timeline || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->message_scene = scene;
    return true;
}

bool renderer_bind_message_action_bar(D3D11Renderer* renderer, const MessageActionBar* bar) noexcept {
    if (!bar) {
        return false;
    }
    renderer->message_action_bar = bar;
    return true;
}

bool renderer_bind_message_composer_scene(D3D11Renderer* renderer,
                                          const MessageComposerScene* scene) noexcept {
    if (!scene || !scene->widgets.storage.data) {
        return false;
    }
    if (renderer->message_composer_scene != scene)
        text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->message_composer_scene = scene;
    return true;
}

bool renderer_bind_member_list_scene(D3D11Renderer* renderer, const MemberListScene* scene) noexcept {
    if (!scene || !scene->rows.data || !scene->widgets.storage.data) {
        return false;
    }
    renderer->member_list_scene = scene;
    return true;
}

bool renderer_bind_popup_scene(D3D11Renderer* renderer, const PopupScene* scene) noexcept {
    // Only the widget manager is required; popup item/row storage is created on first open.
    if (!scene || !scene->widgets.storage.data) {
        return false;
    }
    renderer->popup_scene = scene;
    return true;
}

bool renderer_bind_reaction_viewer(D3D11Renderer* renderer, const ReactionViewer* viewer) noexcept {
    if (!viewer) {
        return false;
    }
    renderer->reaction_viewer = viewer;
    return true;
}

void renderer_bind_forum_form(D3D11Renderer* renderer, ForumPostSurface* surface) noexcept {
    if (renderer->forum_form == surface)
        return;
    renderer->forum_form = surface;
    renderer->interaction_text_forum_form_revision =
        ~(surface && surface->scene ? surface->scene->revision : 0u);
    invalidate_retained_frame(renderer);
}

void renderer_bind_text_form(D3D11Renderer* renderer, TextFormSurface* surface) noexcept {
    renderer->text_form = surface;
    renderer->interaction_text_form_revision = ~(surface && surface->scene ? surface->scene->revision : 0u);
    invalidate_retained_frame(renderer);
}

void renderer_bind_settings_scene(D3D11Renderer* renderer, const UiSettingsScene* scene) noexcept {
    if (renderer->settings_scene != scene && !(scene && renderer->settings_backdrop_frozen)) {
        // A newly opened dialog draws the live shell beneath it on its first
        // frame; one opened over the sign-in screen keeps that frozen image.
        renderer_release_settings_backdrop(renderer);
        renderer->settings_backdrop_failed = false;
    }
    renderer->settings_scene = scene;
    if (scene && scene->active) {
        reset_value(&renderer->friends_search_geometry);
        text_edit_layout_invalidate(&renderer->composer_layout);
    }
    renderer->interaction_text_settings_revision = ~(scene ? scene->revision : 0u);
    invalidate_retained_frame(renderer);
}

bool renderer_bind_message_text_presentation(D3D11Renderer* renderer,
                                             const MessageTextPresentation* presentation) noexcept {
    if (!presentation || !presentation->commands.data || !presentation->measurements.data ||
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
    if (renderer->friends_scene == scene)
        return;
    renderer->friends_scene = scene;
    reset_value(&renderer->friends_search_geometry);
    text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->interaction_text_friends_revision = ~(scene ? scene->revision : 0u);
    invalidate_retained_frame(renderer);
}
void renderer_bind_friends_requests_scene(D3D11Renderer* renderer, const UiFriendsScene* scene) noexcept {
    if (renderer->friends_requests_scene == scene)
        return;
    renderer->friends_requests_scene = scene;
    reset_value(&renderer->friends_search_geometry);
    text_edit_layout_invalidate(&renderer->composer_layout);
    renderer->interaction_text_friends_requests_revision = ~(scene ? scene->revision : 0u);
    invalidate_retained_frame(renderer);
}

void renderer_unbind_message_text_presentation(D3D11Renderer* renderer) noexcept {
    renderer->message_text = nullptr;
    renderer->message_measure_cache = nullptr;
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    content_text_retention_invalidate(&renderer->content_text_retention);
    renderer_clear_message_text_selection(renderer);
}

bool renderer_bind_search_message_text_presentation(D3D11Renderer* renderer,
                                                    const MessageTextPresentation* presentation,
                                                    const UserStore* users,
                                                    UiTextMeasureCache* cache) noexcept {
    if (!presentation || !presentation->commands.data || !presentation->measurements.data || !users || !cache)
        return false;
    if (renderer->search_message_text == presentation && renderer->search_message_users == users &&
        renderer->search_message_measure_cache == cache)
        return true;
    renderer->search_message_text = presentation;
    renderer->search_message_users = users;
    renderer->search_message_measure_cache = cache;
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    invalidate_retained_frame(renderer);
    return true;
}
void renderer_unbind_search_message_text_presentation(D3D11Renderer* renderer) noexcept {
    if (!renderer->search_message_text && !renderer->search_message_users &&
        !renderer->search_message_measure_cache)
        return;
    renderer->search_message_text = nullptr;
    renderer->search_message_users = nullptr;
    renderer->search_message_measure_cache = nullptr;
    message_hit_test_cache_invalidate(&renderer->message_hit_test);
    invalidate_retained_frame(renderer);
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

namespace {
// The composer whose measured layout still describes its display text, and that text; null otherwise.
const MessageComposerScene* composer_edit_scene(const D3D11Renderer& renderer, Utf8View* text) noexcept {
    const MessageComposerScene* scene = renderer.message_composer_scene;
    if (!scene || !scene->active || !scene->model || d3d11_renderer_active(renderer.friends_scene) ||
        d3d11_renderer_active(renderer.settings_scene) || !renderer.composer_layout.active ||
        !message_composer_scene_layout_node(*scene, MessageComposerControl::edit))
        return nullptr;
    *text = message_composer_scene_display_text(*scene);
    return text->bytes == renderer.composer_layout.text_bytes &&
                   scene->display_signature == renderer.composer_layout.text_signature
               ? scene
               : nullptr;
}
// The model byte offset of UTF-16 `position` in the composer's display text.
bool composer_model_offset(const MessageComposerScene& scene, Utf8View text, u32 position,
                           u32* model_byte_offset) noexcept {
    u32 display_byte_offset = 0;
    if (!utf8_byte_offset_for_utf16_position(text, position, &display_byte_offset) ||
        display_byte_offset > scene.display_bytes)
        return false;
    *model_byte_offset = message_composer_scene_model_offset_from_display(scene, display_byte_offset);
    return true;
}
} // namespace

bool renderer_message_composer_edit_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                             i32 client_y_pixels, u32* model_byte_offset) noexcept {
    Utf8View text{};
    const MessageComposerScene* scene =
        model_byte_offset && renderer.dpi ? composer_edit_scene(renderer, &text) : nullptr;
    if (!scene)
        return false;
    if (text.bytes == 0) {
        *model_byte_offset = scene->display_first;
        return true;
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    u32 position = 0;
    return text_edit_layout_hit_test(renderer.composer_layout, x, y, &position) &&
           composer_model_offset(*scene, text, position, model_byte_offset);
}

bool renderer_message_composer_edit_move_vertical(const D3D11Renderer& renderer, bool down,
                                                  u32* model_byte_offset) noexcept {
    Utf8View text{};
    const MessageComposerScene* scene = model_byte_offset ? composer_edit_scene(renderer, &text) : nullptr;
    if (!scene)
        return false;
    if (text.bytes == 0) {
        *model_byte_offset = scene->display_first;
        return true;
    }
    u32 position = 0;
    return text_edit_layout_move_vertical(renderer.composer_layout, down, &position) &&
           composer_model_offset(*scene, text, position, model_byte_offset);
}

void renderer_set_login_caret_visible(D3D11Renderer* renderer, bool visible) noexcept {
    renderer->login_caret_visible = visible && renderer->login_edit.active;
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
        // Icon cells belong to the chip around them, which hits instead; the "(edited)" label is only a tooltip's
        // target (renderer_edited_label_hit_test), no link, cursor or click of its own.
        if ((semantic.reserved & kRendererSearchSemanticFlag) ||
            semantic.kind == MessageMarkdownSemanticKind::chip_icon ||
            semantic.kind == MessageMarkdownSemanticKind::guild_icon ||
            semantic.kind == MessageMarkdownSemanticKind::game_icon ||
            semantic.kind == MessageMarkdownSemanticKind::author_avatar ||
            semantic.kind == MessageMarkdownSemanticKind::role_dot ||
            semantic.kind == MessageMarkdownSemanticKind::alt_text ||
            semantic.kind == MessageMarkdownSemanticKind::edited)
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
                              true,
                              semantic.reserved};
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
    return message_position_hit_test(&renderer.message_hit_test, *presentation, renderer.glyph_text.system,
                                     renderer_text_system(&renderer, ui_text_face_chat(UiTextFace::normal)),
                                     renderer.dpi, client_x_pixels, client_y_pixels);
}

MessageEmojiHit renderer_message_emoji_hit_test(D3D11Renderer& renderer, float x, float y) noexcept {
    const MessageTextPresentation* presentation = renderer.message_text;
    if (!presentation || !renderer.message_scene || !renderer.message_scene->active ||
        !ui_rect_contains(renderer.message_scene->viewport, x, y))
        return {};
    return message_emoji_hit_test(&renderer.message_hit_test, *presentation, renderer.glyph_text.system,
                                  renderer_text_system(&renderer, ui_text_face_chat(UiTextFace::normal)), x, y);
}

RendererEditedLabelHit renderer_edited_label_hit_test(const D3D11Renderer& renderer, float x, float y) noexcept {
    if (!renderer.content_text || !ui_coordinate_valid(x) || !ui_coordinate_valid(y))
        return {};
    const RendererTextStorage& storage = *renderer.content_text;
    const bool timeline = renderer.message_scene && renderer.message_scene->active &&
                          ui_rect_contains(renderer.message_scene->viewport, x, y);
    for (u32 cursor = storage.content_hit_region_count; cursor != 0; --cursor) {
        const TextHitRegion& region = storage.content_hit_regions[cursor - 1];
        if (x < region.left || x >= region.right || y < region.top || y >= region.bottom ||
            region.semantic_index >= storage.content_semantic_count) {
            continue;
        }
        const MessageTextSemantic& semantic = storage.content_semantics[region.semantic_index];
        if (semantic.kind != MessageMarkdownSemanticKind::edited || semantic.message_id == 0)
            continue;
        const bool search = (semantic.reserved & kRendererSearchSemanticFlag) != 0;
        const UiRect label{region.left, region.top, region.right, region.bottom};
        const UiRect visible = search ? (renderer.search_scene ? ui_search_scene_content_clip(*renderer.search_scene, label)
                                                               : UiRect{})
                                      : (timeline ? ui_rect_intersection(label, renderer.message_scene->viewport) : UiRect{});
        if (!ui_rect_contains(visible, x, y))
            continue;
        return RendererEditedLabelHit{semantic.message_id, semantic.target, visible, semantic.message_revision,
                                      search, true};
    }
    return {};
}

void renderer_clear_message_text_selection(D3D11Renderer* renderer) noexcept {
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
    if (!command.valid || command.message_id == 0 || anchor > command.text_bytes || caret > command.text_bytes) {
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

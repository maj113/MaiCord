#pragma once

#include "maicord/ui/chat/messages/interaction.hpp"
#include "maicord/render/d3d11/renderer.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "maicord/ui/chat/polls/presentation.hpp"

#include "renderer_interaction_state.hpp"

#include <stddef.h>

namespace maicord {
namespace renderer_text_detail {

// Everything append_content_text shapes and places for one text run.
struct ContentTextRequest {
    Utf8View input;
    float maximum_width, maximum_height, origin_x, origin_y;
    const UiRect* clip;
    const UiRect* batch_clip;
    const Color* color;
    ConstSlice<TextStyleSpan> styles;
    const Theme* style_theme;
    const Color* inline_role_color;
    ConstSlice<MessageMarkdownSemantic> semantics;
    UiWidgetReference layout_owner;
    u64 message_id;
    u32 message_revision, selection_anchor, selection_caret;
    UiTextAlignment horizontal_alignment, vertical_alignment;
    TextWrapping wrapping;
    TextOverflow overflow;
    UiTextFace face;
    bool force_new_batch, selection_active, cluster_effect, plain_flags, publish_semantics;
};

// Boundary ordinals within the overlay text stream. The search message
// boundaries come last: the raster pass does not compare them.
struct TextOverlayBoundaries {
    u32 friends_end, media_picker_start, media_picker_end, menu_start, form_start, forum_end;
    u32 popup_start, popup_end, settings_choice_start, settings_choice_end, theme_editor_start,
        theme_editor_end, profile_start, profile_end;
    u32 audio_start{}, audio_end{};
    u32 poll_start{}, poll_end{};
    // Profile editor overlays on a settings page (~0u: none this frame).
    u32 settings_preview_start{~0u}, settings_preview_end{~0u}, settings_dialog_start{~0u}, settings_dialog_end{~0u};
    // The toast's text closes the stream (~0u: none this frame).
    u32 toast_start{~0u};
    u32 search_rich_start{}, search_rich_end{};
};
constexpr u32 kComparedOverlayBoundaryBytes = offsetof(TextOverlayBoundaries, search_rich_start);
static_assert(kComparedOverlayBoundaryBytes == 23 * sizeof(u32) &&
              sizeof(TextOverlayBoundaries) == kComparedOverlayBoundaryBytes + 2 * sizeof(u32));

// Where a boundary publishes: the renderer's text batch at that ordinal. The
// form start and forum end are gated on a form or settings page and handled
// beside this table; the menu start, last, has no end-of-stream endpoint.
struct OverlayBatchBoundary {
    u32 boundary, batch;
};
inline constexpr OverlayBatchBoundary kOverlayBatchBoundaries[]{
    {offsetof(TextOverlayBoundaries, friends_end), offsetof(D3D11Renderer, friends_requests_text_end_batch)},
    {offsetof(TextOverlayBoundaries, media_picker_start), offsetof(D3D11Renderer, media_picker_text_first_batch)},
    {offsetof(TextOverlayBoundaries, media_picker_end), offsetof(D3D11Renderer, media_picker_text_end_batch)},
    {offsetof(TextOverlayBoundaries, profile_start), offsetof(D3D11Renderer, profile_text_first_batch)},
    {offsetof(TextOverlayBoundaries, profile_end), offsetof(D3D11Renderer, profile_text_end_batch)},
    {offsetof(TextOverlayBoundaries, theme_editor_start), offsetof(D3D11Renderer, theme_editor_text_first_batch)},
    {offsetof(TextOverlayBoundaries, theme_editor_end), offsetof(D3D11Renderer, theme_editor_text_end_batch)},
    {offsetof(TextOverlayBoundaries, popup_start), offsetof(D3D11Renderer, popup_text_first_batch)},
    {offsetof(TextOverlayBoundaries, popup_end), offsetof(D3D11Renderer, popup_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_choice_start),
     offsetof(D3D11Renderer, settings_choice_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_choice_end), offsetof(D3D11Renderer, settings_choice_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_preview_start),
     offsetof(D3D11Renderer, settings_preview_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_preview_end), offsetof(D3D11Renderer, settings_preview_text_end_batch)},
    {offsetof(TextOverlayBoundaries, settings_dialog_start),
     offsetof(D3D11Renderer, settings_dialog_text_first_batch)},
    {offsetof(TextOverlayBoundaries, settings_dialog_end), offsetof(D3D11Renderer, settings_dialog_text_end_batch)},
    {offsetof(TextOverlayBoundaries, toast_start), offsetof(D3D11Renderer, toast_text_first_batch)},
    {offsetof(TextOverlayBoundaries, menu_start), offsetof(D3D11Renderer, menu_text_first_batch)},
};
constexpr u32 kOverlayEndBoundaryCount = sizeof(kOverlayBatchBoundaries) / sizeof(kOverlayBatchBoundaries[0]) - 1;

struct OverlayReservation {
    u32 capacity, quads;
    u32 batches{};
    const UiWidgetManager* picker_owner{};
    UiRect picker_clip{};
};

struct TextRasterConsumer {
    D3D11Renderer* renderer;
    const Theme* theme;
    bool* atlas_invalidated;
    const TextOverlayBoundaries* boundaries;
    bool overlay;
    bool pending_boundary{}, settings_page_started{}, previous_settings_page{};
    u32 search_command_cursor{};
};

bool resolve_role_mention_color_callback(void* context, u64 message_id, u64 role_id,
                                         Color* color) noexcept;
float aligned_text_origin(UiTextAlignment alignment, float start, float available, float used) noexcept;
bool append_text_batch(RendererTextStorage* storage, const UiRect* clip, u32 first_quad,
                       bool force_new_batch, const UiRect* batch_clip) noexcept;
u64 typography_source_identity(const TextSystem& system) noexcept;
TextSystem command_text_system(D3D11Renderer* renderer, UiTextFace face) noexcept;
bool flag_projection_wanted(const D3D11Renderer& renderer, const ContentTextRequest& request,
                            RendererInlineMediaLayer* layer) noexcept;
bool append_content_text(D3D11Renderer* renderer, const ContentTextRequest& request, float* shaped_height,
                         bool* atlas_invalidated) noexcept;
bool append_content_text_command(D3D11Renderer* renderer, const UiTextCommand& command, const Theme& theme,
                                 bool* atlas_invalidated, bool force_new_batch = false,
                                 const UiRect* batch_clip = nullptr) noexcept;
bool append_message_text_command(D3D11Renderer* renderer, const MessageTextPresentation* presentation,
                                 const MessageTextCommand& command, u32 command_index, const Theme& theme,
                                 bool* atlas_invalidated, bool search_result = false,
                                 bool force_new_batch = false) noexcept;
ContentTextRetentionKey content_text_retention_key(const D3D11Renderer& renderer, const Theme& theme,
                                                   u64 shell_signature, u64 popup_signature,
                                                   bool include_messages) noexcept;
bool measure_content_search(D3D11Renderer* renderer) noexcept;
bool measure_message_search(D3D11Renderer* renderer) noexcept;
bool measure_media_picker(D3D11Renderer* renderer) noexcept;
void reset_motion_text_ranges(D3D11Renderer* renderer, u32 batch) noexcept;
bool reserve_overlay_text(void* context, const UiTextCommand& command, u32) noexcept;
bool project_shell_static_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept;
bool project_shell_member_text(D3D11Renderer* renderer, const Theme& theme,
                               UiTextEmitter* emitter) noexcept;
bool project_shell_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter) noexcept;
bool project_overlay_text(D3D11Renderer* renderer, const Theme& theme, UiTextEmitter* emitter,
                          TextOverlayBoundaries* boundaries) noexcept;
bool raster_text_command(void* context, const UiTextCommand& command, u32 ordinal) noexcept;
u32 overlay_boundary(const TextOverlayBoundaries& boundaries, const OverlayBatchBoundary& entry) noexcept;
u32& overlay_batch(D3D11Renderer* renderer, const OverlayBatchBoundary& entry) noexcept;

} // namespace renderer_text_detail
} // namespace maicord

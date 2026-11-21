#include "profile.hpp"
#include "account_presentation.hpp"
#include "../shell/content_input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/state.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/text/renderer_text_storage.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"

namespace maicord::app_detail {
namespace {
// The spoiler or link of the bio of `s` under the point, as the renderer published it (message id zero).
bool bio_hit(const ProfileScene& s, UiPoint point, MessageTextSemantic* hit) noexcept {
    const auto* text = g_app.renderer.content_text;
    const auto* bio = ui_widget_manager_layout_node(s.widgets, s.bio_widget);
    if (!text || !bio || !ui_rect_contains(bio->rectangle, point.x, point.y))
        return false;
    for (u32 i = text->content_hit_region_count; i > 0; --i) {
        const auto& r = text->content_hit_regions[i - 1];
        if (point.x < r.left || point.x >= r.right || point.y < r.top || point.y >= r.bottom ||
            r.semantic_index >= text->content_semantic_count)
            continue;
        const auto& semantic = text->content_semantics[r.semantic_index];
        if (!semantic.message_id && (semantic.kind == MessageMarkdownSemanticKind::spoiler ||
                                     semantic.kind == MessageMarkdownSemanticKind::link)) {
            *hit = semantic;
            return true;
        }
    }
    return false;
}
} // namespace

bool application_profile_bio_pointer(ProfileScene* s, UiButtonPointerEvent event, UiPoint point) noexcept {
    if (event == UiButtonPointerEvent::cancel) {
        s->bio_pressed = 0;
        return false;
    }
    if (event != UiButtonPointerEvent::press && event != UiButtonPointerEvent::release)
        return false;
    MessageTextSemantic hit{};
    const bool over = bio_hit(*s, point, &hit);
    if (event == UiButtonPointerEvent::press) {
        if (!over)
            return false;
        s->bio_pressed = hit.source_offset + 1;
        window_capture_pointer(g_app.window);
        return true;
    }
    if (!s->bio_pressed)
        return false;
    const u32 pressed = s->bio_pressed;
    s->bio_pressed = 0;
    application_release_capture();
    if (!over || hit.source_offset + 1 != pressed)
        return true;
    if (hit.kind == MessageMarkdownSemanticKind::spoiler && s->bio_revealed_count < 64) {
        s->bio_revealed[s->bio_revealed_count++] = hit.source_offset;
        profile_scene_layout(s, application_viewport_dips(), true);
        if (refresh_account_text(true) && g_app.window)
            application_request_frame();
    } else if (hit.kind == MessageMarkdownSemanticKind::link) {
        const auto source = s->profile->guild_bio.bytes ? s->profile->guild_bio.view() : s->profile->bio.view();
        if (hit.source_offset + hit.source_bytes <= source.bytes)
            application_open_content_url({source.data + hit.source_offset, hit.source_bytes});
    }
    return true;
}
} // namespace maicord::app_detail

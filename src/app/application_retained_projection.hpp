#pragma once

#include "application_account_presentation.hpp"
#include "application_state.hpp"
#include "maicord/render/d3d11_renderer.hpp"

namespace maicord::app_detail {
// Retained surfaces publish their own input/scroll geometry. Only a changed
// message viewport requires account text convergence; real account mutations
// still reconcile on every caller's normal freshness check.
inline bool refresh_retained_projection(bool message_viewport_changed = false) noexcept {
    return refresh_account_text(message_viewport_changed) &&
           (message_viewport_changed || !g_app.renderer_ready ||
            renderer_rebuild_content_text(&g_app.renderer, application_theme())) &&
           application_request_frame();
}
} // namespace maicord::app_detail

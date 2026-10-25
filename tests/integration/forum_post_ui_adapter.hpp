#pragma once
#include "application_forum_post.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/protocol/forum_post.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "reply_draft_ui_adapter.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/forum_post_scene.hpp"
namespace maicord {
inline constexpr u64 kForumPostProbeParent = 97400;
inline constexpr u64 kForumPostProbeThread = 97450;
inline constexpr u64 kForumPostProbeMessage = 97451;
struct ForumPostProbeCosts {
    u32 form_bytes, form_committed_bytes, scene_committed_bytes, edit_committed_bytes[2],
        history_owner_committed_bytes[2];
    SecureEditHistoryCosts history[2];
    u32 form_revision, scene_revision, layout_revision, measure_calls, scene_commit_count;
    u64 form_serial;
};
bool forum_post_ui_adapter_begin(HWND, D3D11Renderer&, UiTextMeasureCache&, bool (*)(void*) noexcept,
                                 void*) noexcept;
void forum_post_ui_adapter_end() noexcept;
bool forum_post_ui_adapter_active() noexcept;
bool forum_post_ui_adapter_navigate(u64 channel_id, u64 message_id) noexcept;
D3D11Renderer& forum_post_ui_adapter_renderer(D3D11Renderer&) noexcept;
void forum_post_ui_adapter_viewport(D3D11Renderer&, const UiScene&) noexcept;
bool forum_post_ui_adapter_browser() noexcept;
bool forum_post_ui_adapter_browser_create() noexcept;
bool forum_post_ui_adapter_open() noexcept;
bool forum_post_ui_adapter_input(UiInputKind, u32, bool repeated = false, bool control = false,
                                 bool shift = false) noexcept;
bool forum_post_ui_adapter_pointer(ForumPostTarget) noexcept;
bool forum_post_ui_adapter_pointer_down(ForumPostTarget) noexcept;
bool forum_post_ui_adapter_pointer_up() noexcept;
bool forum_post_ui_adapter_synchronize() noexcept;
bool forum_post_ui_adapter_submit(u64) noexcept;
bool forum_post_ui_adapter_reject_before_wake(u64) noexcept;
bool forum_post_ui_adapter_response(u16, Utf8View = {}, RestClientError = RestClientError::none,
                                    u64 exact_id = 0) noexcept;
bool forum_post_ui_adapter_flight(ForumPostRequest*, u64*) noexcept;
u32 forum_post_ui_adapter_submissions() noexcept;
u32 forum_post_ui_adapter_cancellations() noexcept;
ForumPostScene* forum_post_ui_adapter_scene() noexcept;
ForumPostSurface* forum_post_ui_adapter_surface() noexcept;
SecureEdit* forum_post_ui_adapter_edit(u32) noexcept;
bool forum_post_ui_adapter_costs(ForumPostProbeCosts*) noexcept;
bool forum_post_ui_adapter_metadata(bool require_tag, Utf8View*, bool unnamed_tag = false) noexcept;
bool forum_post_ui_adapter_drop_files() noexcept;
bool forum_post_ui_adapter_success(Utf8View*) noexcept;
bool forum_post_ui_adapter_tsf_paste_composition() noexcept;
bool forum_post_ui_adapter_retire(u8 reason) noexcept;
bool forum_post_ui_adapter_manage_threads(bool) noexcept;
bool forum_post_ui_adapter_escape_tails() noexcept;
} // namespace maicord

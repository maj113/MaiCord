#include "video_focus_runtime_internal.hpp"
#include "shell/state.hpp"
#include "media/video.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
bool renderer_bind_audio_scene(D3D11Renderer* renderer, const UiAudioScene* scene) noexcept {
    if (!renderer)
        return false;
    if (renderer->audio_scene != scene) {
        renderer->audio_scene = scene;
        renderer->retained_presentation.valid = false;
    }
    if (scene) {
        renderer->video_scene = nullptr;
        renderer->video_scene_search_result = false;
    }
    return true;
}
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    ++video_focus_runtime_test::text_publications;
    if (video_focus_runtime_test::reject_text_projection)
        return false;
    renderer->interaction_text_video_revision =
        renderer->video_scene ? ui_video_scene_text_paint_revision(*renderer->video_scene) : 0;
    renderer->interaction_text_audio_revision =
        renderer->audio_scene ? ui_audio_scene_text_revision(*renderer->audio_scene) : 0;
    return true;
}
MediaAtlasGpuResult renderer_present_video_frame(D3D11Renderer*, MediaAssetKey,
                                                 const GpuVideoFrame&, u32) noexcept {
    ++video_focus_runtime_test::account.gpu_publications;
    if (video_focus_runtime_test::provider_upload) return {};
    return {{}, E_FAIL, MediaAtlasGpuError::invalid_input};
}
namespace app_detail {
void application_record_presentation_failure(const char*) noexcept {
    ++video_focus_runtime_test::presentation_rejections;
}
UiSearchScene* application_search_scene() noexcept { return video_focus_runtime_test::mutable_search_scene; }
void application_search_media_dimensions_changed() noexcept {}
bool refresh_account_text(bool) noexcept {
    return message_scene_update(&g_app.message_scene, g_app.message_scene.viewport).ok();
}
bool refresh_account_frame(bool) noexcept {
    ++video_focus_runtime_test::media_refreshes;
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {
    ++video_focus_runtime_test::confirmations;
}
bool application_image_viewer_open_video(u64, u32, bool, u8, bool, const ApplicationSearchSourceTicket*,
                                         const MediaAssetKey&, const UiRect&) noexcept {
    if (!video_focus_runtime_test::viewer_allowed)
        return false;
    ++video_focus_runtime_test::viewer_opens;
    g_app.image_viewer.scene.active = true;
    return true;
}
bool application_image_viewer_video_geometry(const MediaAssetKey&, UiRect* preview, UiRect* clip) noexcept {
    if (!g_app.image_viewer.scene.active || !preview || !clip)
        return false;
    *preview = video_focus_runtime_test::viewer_preview;
    *clip = {0, 0, 640, 480};
    return true;
}
void application_image_viewer_close() noexcept {
    g_app.image_viewer.scene.active = false;
    application_video_viewer_detach();
}
void application_image_viewer_dismiss() noexcept {
    application_image_viewer_close();
}
bool application_downloads_begin(u64 message, u64 attachment, bool snapshot) noexcept {
    ++video_focus_runtime_test::downloads;
    video_focus_runtime_test::download_message = message;
    video_focus_runtime_test::download_attachment = attachment;
    video_focus_runtime_test::download_snapshot = snapshot;
    return true;
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& source, u64 attachment,
                                        bool snapshot) noexcept {
    return application_downloads_begin(source.message_id, attachment, snapshot);
}
bool application_search_source_ticket(u32 index, ApplicationSearchSourceTicket* ticket) noexcept {
    if (!video_focus_runtime_test::search_current || !ticket ||
        index != video_focus_runtime_test::search_view.ticket.index)
        return false;
    *ticket = video_focus_runtime_test::search_view.ticket;
    return true;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    const auto& view = video_focus_runtime_test::search_view;
    if (!video_focus_runtime_test::search_current || !source ||
        ticket.message_id != view.ticket.message_id ||
        ticket.publication_revision != view.ticket.publication_revision ||
        ticket.index != view.ticket.index)
        return false;
    *source = view;
    source->snapshot = snapshot;
    source->rich = snapshot ? message_rich_store_snapshot(*view.rich_store, ticket.message_id)
                            : message_rich_store_find(*view.rich_store, ticket.message_id);
    return source->rich != nullptr;
}
bool application_search_source_resolve(u64 id, u32 publication, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    auto ticket = video_focus_runtime_test::search_view.ticket;
    ticket.message_id = id;
    ticket.publication_revision = publication;
    return application_search_source_resolve(ticket, snapshot, source);
}
bool application_settings_active() noexcept {
    return video_focus_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
void application_composer_text_input_suspend() noexcept {
}
bool application_call_pump(bool*) noexcept {
    return true;
}
u64 application_call_next_wake() noexcept {
    return 0;
}
// Provider-embed pills open their page through the link trust rules.
bool application_link_open(Utf8View, bool) noexcept {
    return true;
}

} // namespace app_detail
} // namespace maicord

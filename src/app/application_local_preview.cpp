#include "application_composer_preview.hpp"
#include "application_forum_preview.hpp"
#include "application_profile_editor.hpp"

// Local draft previews belong to the forum form or the composer strip; keys
// never collide because composer variants carry the thumbnail bit.
namespace maicord::app_detail {
bool application_local_preview_key_current(MediaAssetKey key) noexcept {
    return application_forum_preview_key_current(key) || application_composer_preview_key_current(key) ||
           application_profile_editor_frame_current(key);
}
bool application_local_preview_frame_current(const AccountMediaFrame& frame) noexcept {
    return application_forum_preview_frame_current(frame) || application_composer_preview_frame_current(frame) ||
           application_profile_editor_frame_current(frame.key);
}
void application_local_preview_frame_uploaded(const AccountMediaFrame& frame) noexcept {
    application_forum_preview_frame_uploaded(frame);
    application_composer_preview_frame_uploaded(frame);
    application_profile_editor_frame_uploaded(frame.key);
}
void application_local_preview_deferred(MediaAssetKey key, u64 retry_at_ms) noexcept {
    application_composer_preview_deferred(key, retry_at_ms);
}
void application_local_preview_rejected(MediaAssetKey key) noexcept {
    application_forum_preview_rejected(key);
    application_composer_preview_rejected(key);
    application_profile_editor_frame_rejected(key);
}
} // namespace maicord::app_detail

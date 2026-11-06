// The offline renderer fixture owns a UiMediaPickerScene directly. Account,
// transport and application input ownership are covered by the separate
// shipping-controller runtime test; they are deliberately absent here.
#include "../../src/app/media/picker.hpp"
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/core/geometry.hpp"

namespace maicord::app_detail {
MessageComposer* application_media_picker_edit_model() noexcept { return nullptr; }
bool application_media_picker_edit_focused() noexcept { return false; }
TextInputTarget application_media_picker_edit_target() noexcept { return {}; }
UiRect application_media_picker_edit_bounds() noexcept { return {}; }
bool application_media_picker_edit_changed(bool) noexcept { return false; }
void application_media_picker_edit_cancel_native_group(const TextInputTarget&) noexcept {}
bool application_media_picker_active() noexcept { return false; }
bool application_media_picker_preview_owner_current(MediaAssetKey, u64, u32) noexcept { return false; }
bool application_media_picker_preview_visible(MediaAssetKey) noexcept { return false; }
bool application_media_picker_preview_rejected(MediaAssetKey, u64, u32) noexcept { return false; }
bool application_media_picker_preview_current(const MediaAssetKey&, u64, u32) noexcept { return false; }
bool application_media_picker_preview_request(u32, AccountMediaCatalogPreviewRequest*) noexcept { return false; }
} // namespace maicord::app_detail

// The offline renderer fixture owns a UiMediaPickerScene directly. Account,
// transport and application input ownership are covered by the separate
// shipping-controller runtime test; they are deliberately absent here.
#include "../../src/app/application_media_picker.hpp"
#include "media_picker_ui_adapter.hpp"
#include "maicord/ui/media_picker.hpp"

namespace maicord::app_detail {
namespace {MediaAssetKey probe_key{};u32 probe_rejections=0;}
void media_picker_probe_set_key(MediaAssetKey key) noexcept {probe_key=key;probe_rejections=0;}
u32 media_picker_probe_rejections() noexcept {return probe_rejections;}
MessageComposer* application_media_picker_edit_model() noexcept { return nullptr; }
bool application_media_picker_edit_focused() noexcept { return false; }
TextInputTarget application_media_picker_edit_target() noexcept { return {}; }
UiRect application_media_picker_edit_bounds() noexcept { return {}; }
bool application_media_picker_edit_changed(bool) noexcept { return false; }
void application_media_picker_edit_cancel_native_group(const TextInputTarget&) noexcept {}
bool application_media_picker_active() noexcept { return false; }
bool application_media_picker_preview_owner_current(MediaAssetKey key,u64 scope,u32 generation) noexcept {
    return probe_key.id && media_asset_key_equal(key,probe_key) && scope==99 && generation==1;
}
bool application_media_picker_preview_visible(MediaAssetKey key) noexcept {
    return probe_key.id && media_asset_key_equal(key,probe_key);
}
bool application_media_picker_preview_rejected(MediaAssetKey key,u64 scope,u32 generation) noexcept {
    if(!scope || !generation || !application_media_picker_preview_visible(key))return false;
    ++probe_rejections;return true;
}
bool application_media_picker_preview_current(MediaAssetKey key,u64 scope,u32 generation) noexcept {
    return application_media_picker_preview_owner_current(key,scope,generation);
}
bool application_media_picker_preview_request(u32, AccountMediaCatalogPreviewRequest*) noexcept { return false; }
} // namespace maicord::app_detail

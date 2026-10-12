#pragma once
#include "maicord/media/asset.hpp"
namespace maicord::app_detail {
void media_picker_probe_set_key(MediaAssetKey key) noexcept;
u32 media_picker_probe_rejections() noexcept;
}

#pragma once
// A complete draw pipeline in one call, for the probes. The renderer stages its pipeline on a worker
// (create), sets the frame transform and publishes separately; this does the three in order.
#include "maicord/render/d3d11/draw_pipeline.hpp"
#include "maicord/core/win32_error.hpp"

namespace maicord {
// create + frame transform + publish; a failure leaves the owner untouched.
inline D3D11DrawPipelineResult d3d11_draw_pipeline_initialize(D3D11DrawPipelineOwner* owner, ID3D11Device* device,
                                                              ID3D11DeviceContext* context, u32 width, u32 height,
                                                              u32 dpi, const D3D11DrawPipelineConfig& config) noexcept {
    D3D11DrawPipelineOwner staged{};
    const D3D11DrawPipelineResult created = d3d11_draw_pipeline_create(&staged, device, config);
    if (!created.ok())
        return created;
    if (!d3d11_draw_pipeline_update_transform(staged, context, width, height, dpi)) {
        d3d11_draw_pipeline_release(&staged);
        return D3D11DrawPipelineResult{E_INVALIDARG, D3D11DrawPipelineError::invalid_input};
    }
    d3d11_draw_pipeline_publish(owner, &staged);
    return D3D11DrawPipelineResult{S_OK, D3D11DrawPipelineError::none};
}
} // namespace maicord

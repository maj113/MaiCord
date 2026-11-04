#include "maicord/gpu/gpu.hpp"
#include "../support/test_output.hpp"

// The GPU vocabulary compiles on every platform: on Windows each handle is the COM interface it stands for (so
// structs holding handles keep their layout), elsewhere an opaque type.
namespace maicord {
namespace {
template <typename A, typename B> inline constexpr bool same_type_v = false;
template <typename A> inline constexpr bool same_type_v<A, A> = true;

#if defined(_WIN32)
static_assert(same_type_v<GpuStatus, HRESULT>);
static_assert(same_type_v<GpuDevice, ID3D11Device>);
static_assert(same_type_v<GpuContext, ID3D11DeviceContext>);
static_assert(same_type_v<GpuContext1, ID3D11DeviceContext1>);
static_assert(same_type_v<GpuContext4, ID3D11DeviceContext4>);
static_assert(same_type_v<GpuBuffer, ID3D11Buffer>);
static_assert(same_type_v<GpuTexture, ID3D11Texture2D>);
static_assert(same_type_v<GpuSrv, ID3D11ShaderResourceView>);
static_assert(same_type_v<GpuUav, ID3D11UnorderedAccessView>);
static_assert(same_type_v<GpuRtv, ID3D11RenderTargetView>);
static_assert(same_type_v<GpuVertexShader, ID3D11VertexShader>);
static_assert(same_type_v<GpuPixelShader, ID3D11PixelShader>);
static_assert(same_type_v<GpuComputeShader, ID3D11ComputeShader>);
static_assert(same_type_v<GpuInputLayout, ID3D11InputLayout>);
static_assert(same_type_v<GpuSampler, ID3D11SamplerState>);
static_assert(same_type_v<GpuBlend, ID3D11BlendState>);
static_assert(same_type_v<GpuRaster, ID3D11RasterizerState>);
static_assert(same_type_v<GpuQuery, ID3D11Query>);
static_assert(same_type_v<GpuFence, ID3D11Fence>);
#else
static_assert(same_type_v<GpuStatus, i32>);
static_assert(same_type_v<GpuContext1, GpuContext> && same_type_v<GpuContext4, GpuContext>);
static_assert(!same_type_v<GpuBuffer, GpuTexture>);
#endif
static_assert(sizeof(GpuStatus) == 4);
static_assert(gpu_ok(0) && gpu_ok(1) && !gpu_ok(-1));
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_types_entry) {
    maicord::test_exit(0, "gpu handle vocabulary");
}

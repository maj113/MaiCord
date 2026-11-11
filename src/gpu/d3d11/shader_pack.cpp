#include "maicord/gpu/api.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/media/compression/deflate.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
// raw_bytes carries kShaderPatchFlag for the shader stored as a patch of the previous stream.
struct PackedShader { u16 offset, packed_bytes, raw_bytes; };
#include "maicord_shader_pack.inc"

#define MAICORD_SHADER_ORDER(name) \
    static_assert(kShaderIndex_##name == static_cast<u32>(ShaderId::name), "shader pack order")
MAICORD_SHADER_ORDER(ui_vertex); MAICORD_SHADER_ORDER(ui_pixel);
MAICORD_SHADER_ORDER(glyph_vertex); MAICORD_SHADER_ORDER(glyph_pixel);
MAICORD_SHADER_ORDER(media_vertex); MAICORD_SHADER_ORDER(media_pixel);
MAICORD_SHADER_ORDER(glyph_compute); MAICORD_SHADER_ORDER(glyph_culled_compute);
MAICORD_SHADER_ORDER(colr_compute); MAICORD_SHADER_ORDER(colr_wide_compute);
MAICORD_SHADER_ORDER(colr_layers_compute); MAICORD_SHADER_ORDER(png_compute);
MAICORD_SHADER_ORDER(vp8_reconstruct); MAICORD_SHADER_ORDER(vp8_idct);
MAICORD_SHADER_ORDER(vp8_filter); MAICORD_SHADER_ORDER(vp8_atlas);
MAICORD_SHADER_ORDER(jpeg_idct); MAICORD_SHADER_ORDER(jpeg_idct_rows); MAICORD_SHADER_ORDER(jpeg_atlas);
MAICORD_SHADER_ORDER(video_frame);
MAICORD_SHADER_ORDER(fx_vertex); MAICORD_SHADER_ORDER(fx_builtin); MAICORD_SHADER_ORDER(fx_target_vertex);
MAICORD_SHADER_ORDER(fx_composite_vertex); MAICORD_SHADER_ORDER(fx_composite_pixel);
#undef MAICORD_SHADER_ORDER
static_assert(sizeof(kShaders) / sizeof(kShaders[0]) == static_cast<u32>(ShaderId::count));
static_assert(kShaderPatchCount <= sizeof(kShaderPatchOffsets) / sizeof(kShaderPatchOffsets[0]) &&
              kShaderPatchCount <= sizeof(kShaderPatchValues));
constexpr u32 raw_size(const PackedShader& entry) noexcept { return entry.raw_bytes & ~kShaderPatchFlag; }
static_assert([]() constexpr {
    for (const auto& entry : kShaders) {
        if (entry.offset > sizeof(kShaderPacked) || entry.packed_bytes > sizeof(kShaderPacked) - entry.offset ||
            !raw_size(entry) || raw_size(entry) > kShaderMaximumBytes)
            return false;
        if (entry.raw_bytes & kShaderPatchFlag)
            for (u32 i = 0; i < kShaderPatchCount; ++i)
                if (kShaderPatchOffsets[i] >= raw_size(entry) ||
                    (i && kShaderPatchOffsets[i - 1] >= kShaderPatchOffsets[i]))
                    return false;
    }
    return true;
}());

bool inflate(const PackedShader& entry, DeflateWorkspace* workspace, u8* output) noexcept {
    const u32 raw = raw_size(entry);
    const DeflateInputSpan input{kShaderPacked + entry.offset, entry.packed_bytes};
    if (!zlib_inflate({&input, 1}, {output, raw}, raw, workspace).ok())
        return false;
    if (entry.raw_bytes & kShaderPatchFlag)
        for (u32 i = 0; i < kShaderPatchCount; ++i)
            output[kShaderPatchOffsets[i]] = kShaderPatchValues[i];
    return true;
}

// Inflates the shader into heap scratch (the deflate workspace, then the
// exact-size bytecode) and runs `call(create, bytes, count)` on it; the D3D11
// Create* call copies the bytecode. Not stack: workspace and shader reach 38 KB,
// and every stack page a thread touches stays committed for its lifetime.
HRESULT inflate_and_create(ShaderId id, HRESULT (*call)(void*, const u8*, u32) noexcept, void* create) noexcept {
    if (id >= ShaderId::count) return E_FAIL;
    const PackedShader& entry = kShaders[static_cast<u32>(id)];
    const u32 raw = raw_size(entry);
    auto* workspace = static_cast<DeflateWorkspace*>(HeapAlloc(process_heap(), 0, sizeof(DeflateWorkspace) + raw));
    if (!workspace) return E_OUTOFMEMORY;
    u8* bytes = reinterpret_cast<u8*>(workspace + 1);
    const HRESULT result = inflate(entry, workspace, bytes) ? call(create, bytes, raw) : E_FAIL;
    HeapFree(process_heap(), 0, workspace);
    return result;
}

// One inflate body for every shader kind: `create(bytes, count)` runs through a pointer.
template <typename Create>
HRESULT create_from_bytes(ShaderId id, Create create) noexcept {
    return inflate_and_create(
        id,
        [](void* function, const u8* bytes, u32 count) noexcept {
            return (*static_cast<Create*>(function))(bytes, count);
        },
        &create);
}

// The most attributes a built-in vertex shader reads.
constexpr u32 kMaximumInputElements = 8;
} // namespace

GpuStatus gpu_compute_shader_create(GpuDevice* device, ShaderId id, GpuComputeShader** shader) {
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        return device->CreateComputeShader(bytes, count, nullptr, shader);
    });
}

GpuStatus gpu_pixel_shader_create(GpuDevice* device, ShaderId id, GpuPixelShader** shader) {
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        return device->CreatePixelShader(bytes, count, nullptr, shader);
    });
}

GpuStatus gpu_vertex_shader_create(GpuDevice* device, ShaderId id, ConstSlice<GpuInputElement> elements,
                                   GpuVertexShader** shader, GpuInputLayout** layout, bool* layout_failed) {
    bool failed = false;
    if (!layout_failed)
        layout_failed = &failed;
    *layout_failed = false;
    if (elements.count > kMaximumInputElements)
        return E_INVALIDARG;
    // Every attribute is four floats per instance from vertex buffer slot 0.
    D3D11_INPUT_ELEMENT_DESC descriptions[kMaximumInputElements]{};
    for (u32 i = 0; i < elements.count; ++i)
        descriptions[i] = {elements[i].semantic,       elements[i].index, DXGI_FORMAT_R32G32B32A32_FLOAT,
                           0,                          elements[i].offset, D3D11_INPUT_PER_INSTANCE_DATA,
                           1};
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        const HRESULT result = device->CreateVertexShader(bytes, count, nullptr, shader);
        if (FAILED(result) || !layout)
            return result;
        *layout_failed = true;
        const HRESULT created = device->CreateInputLayout(descriptions, elements.count, bytes, count, layout);
        *layout_failed = FAILED(created);
        return created;
    });
}

// The effect packages ship DXBC.
GpuStatus gpu_vertex_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuVertexShader** shader) {
    return device->CreateVertexShader(payload.data, payload.count, nullptr, shader);
}

GpuStatus gpu_pixel_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuPixelShader** shader) {
    return device->CreatePixelShader(payload.data, payload.count, nullptr, shader);
}

} // namespace maicord

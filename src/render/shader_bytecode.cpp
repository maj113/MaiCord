#include "maicord/render/shader_bytecode.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/media/deflate.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
struct PackedShader { u32 offset, packed_bytes, raw_bytes; };
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
MAICORD_SHADER_ORDER(jpeg_idct); MAICORD_SHADER_ORDER(jpeg_atlas);
MAICORD_SHADER_ORDER(video_frame);
MAICORD_SHADER_ORDER(fx_vertex); MAICORD_SHADER_ORDER(fx_builtin);
#undef MAICORD_SHADER_ORDER
static_assert(sizeof(kShaders) / sizeof(kShaders[0]) == static_cast<u32>(ShaderId::count));
static_assert(kShaderPatchCount <= sizeof(kShaderPatchOffsets) / sizeof(kShaderPatchOffsets[0]) &&
              kShaderPatchCount <= sizeof(kShaderPatchValues));
static_assert([]() constexpr {
    for (const auto& entry : kShaders) {
        const u32 offset = entry.offset & ~kShaderPatchFlag;
        if (offset > sizeof(kShaderPacked) || entry.packed_bytes > sizeof(kShaderPacked) - offset)
            return false;
        if (entry.offset & kShaderPatchFlag)
            for (u32 i = 0; i < kShaderPatchCount; ++i)
                if (kShaderPatchOffsets[i] >= entry.raw_bytes ||
                    (i && kShaderPatchOffsets[i - 1] >= kShaderPatchOffsets[i]))
                    return false;
    }
    return true;
}());

const PackedShader* packed_shader(ShaderId id) noexcept {
    if (id >= ShaderId::count) return nullptr;
    const PackedShader& entry = kShaders[static_cast<u32>(id)];
    return entry.raw_bytes && entry.raw_bytes <= kShaderMaximumBytes ? &entry : nullptr;
}

bool inflate(const PackedShader& entry, DeflateWorkspace* workspace, Slice<u8> output) noexcept {
    if (entry.raw_bytes > output.count) return false;
    const DeflateInputSpan input{kShaderPacked + (entry.offset & ~kShaderPatchFlag), entry.packed_bytes};
    if (!zlib_inflate({&input, 1}, {output.data, entry.raw_bytes}, entry.raw_bytes, workspace).ok())
        return false;
    if (entry.offset & kShaderPatchFlag)
        for (u32 i = 0; i < kShaderPatchCount; ++i)
            output.data[kShaderPatchOffsets[i]] = kShaderPatchValues[i];
    return true;
}

// Inflates the shader into heap scratch (the deflate workspace, then the
// exact-size bytecode) and runs `create` on it; the D3D11 Create* call copies
// the bytecode. Not stack: workspace and shader reach 38 KB, and every stack
// page a thread touches stays committed for its lifetime.
template <typename Create>
HRESULT create_from_bytes(ShaderId id, Create create) noexcept {
    const PackedShader* entry = packed_shader(id);
    if (!entry) return E_FAIL;
    auto* workspace =
        static_cast<DeflateWorkspace*>(HeapAlloc(process_heap(), 0, sizeof(DeflateWorkspace) + entry->raw_bytes));
    if (!workspace) return E_OUTOFMEMORY;
    u8* bytes = reinterpret_cast<u8*>(workspace + 1);
    const HRESULT result =
        inflate(*entry, workspace, {bytes, entry->raw_bytes}) ? create(bytes, entry->raw_bytes) : E_FAIL;
    HeapFree(process_heap(), 0, workspace);
    return result;
}
} // namespace

HRESULT shader_create_compute(ID3D11Device* device, ShaderId id, ID3D11ComputeShader** shader) noexcept {
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        return device->CreateComputeShader(bytes, count, nullptr, shader);
    });
}
HRESULT shader_create_pixel(ID3D11Device* device, ShaderId id, ID3D11PixelShader** shader) noexcept {
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        return device->CreatePixelShader(bytes, count, nullptr, shader);
    });
}
HRESULT shader_create_vertex(ID3D11Device* device, ShaderId id, ConstSlice<D3D11_INPUT_ELEMENT_DESC> elements,
                             ID3D11VertexShader** shader, ID3D11InputLayout** layout,
                             bool* layout_failed) noexcept {
    *layout_failed = false;
    return create_from_bytes(id, [&](const u8* bytes, u32 count) noexcept {
        const HRESULT result = device->CreateVertexShader(bytes, count, nullptr, shader);
        if (FAILED(result) || !layout)
            return result;
        *layout_failed = true;
        const HRESULT created = device->CreateInputLayout(elements.data, elements.count, bytes, count, layout);
        *layout_failed = FAILED(created);
        return created;
    });
}
} // namespace maicord

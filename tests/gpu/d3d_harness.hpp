#pragma once

// The Direct3D 11 plumbing the Windows golden tests share (glyphs_windows.cpp, media_windows.cpp): a device, buffers
// and views of the shapes the shaders read, a texture readback, and the capture files tools/update_gpu_goldens.py
// consumes. These tests draw with Direct3D directly, not through the GPU API, so they stay an independent reference
// for what the OpenGL ES backend must reproduce.

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../support/test_output.hpp"

namespace maicord::d3d_harness {

inline bool fail(const char* what) noexcept {
    test_write_text("FAIL: ");
    test_write_line(what);
    return false;
}

inline bool create_device(ID3D11Device** device, ID3D11DeviceContext** context) noexcept {
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       device, nullptr, context)) ||
           SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device,
                                       nullptr, context));
}

inline bool write_file(const WCHAR* path, const void* data, DWORD bytes) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}

// The capture prefix the environment variable names (empty: no capture requested). `path` holds room for a suffix.
inline u32 capture_prefix(const WCHAR* variable, WCHAR* path, u32 capacity) noexcept {
    const DWORD length = GetEnvironmentVariableW(variable, path, capacity - 16);
    return length != 0 && length < capacity - 16 ? length : 0;
}

// Writes `data` to the prefix plus `suffix` (a short string such as L".atlas").
inline bool write_capture(WCHAR* path, u32 prefix_length, const WCHAR* suffix, const void* data, u32 bytes) noexcept {
    u32 at = prefix_length;
    for (; *suffix; ++suffix)
        path[at++] = *suffix;
    path[at] = 0;
    return write_file(path, data, bytes);
}

inline bool constant_buffer(ID3D11Device* device, const void* data, u32 bytes, ID3D11Buffer** buffer) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    return SUCCEEDED(device->CreateBuffer(&description, data ? &initial : nullptr, buffer));
}

// A byte-address buffer with a raw shader resource view and, when `uav` is given, a raw unordered access view.
inline bool raw_buffer(ID3D11Device* device, const void* data, u32 bytes, ID3D11Buffer** buffer,
                       ID3D11ShaderResourceView** view, ID3D11UnorderedAccessView** uav = nullptr) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    if (FAILED(device->CreateBuffer(&description, data ? &initial : nullptr, buffer)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
    descriptor.Format = DXGI_FORMAT_R32_TYPELESS;
    descriptor.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    descriptor.BufferEx.NumElements = bytes / 4;
    descriptor.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(device->CreateShaderResourceView(*buffer, &descriptor, view)))
        return false;
    if (!uav)
        return true;
    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = DXGI_FORMAT_R32_TYPELESS;
    access.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    access.Buffer.NumElements = bytes / 4;
    access.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(device->CreateUnorderedAccessView(*buffer, &access, uav));
}

// A typed buffer of `count` R32_UINT elements (the PNG palette).
inline bool typed_buffer(ID3D11Device* device, const u32* data, u32 count, ID3D11Buffer** buffer,
                         ID3D11ShaderResourceView** view) noexcept {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = count * 4;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    if (FAILED(device->CreateBuffer(&description, &initial, buffer)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
    descriptor.Format = DXGI_FORMAT_R32_UINT;
    descriptor.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    descriptor.Buffer.NumElements = count;
    return SUCCEEDED(device->CreateShaderResourceView(*buffer, &descriptor, view));
}

// The rows of a texture as `texel_bytes` per texel, tightly packed.
inline bool read_back(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, u32 width, u32 height,
                      u32 texel_bytes, u8* out) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool ok = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (ok) {
        for (u32 y = 0; y < height; ++y)
            copy_bytes(out + y * width * texel_bytes, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch,
                       width * texel_bytes);
        context->Unmap(staging, 0);
    }
    com_release(staging);
    return ok;
}

} // namespace maicord::d3d_harness

#pragma once
#include "maicord/render/d3d11_renderer.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/gdi.hpp"

namespace maicord {
// A mapped CPU copy of the renderer's active viewport (and caption rows).
struct ProbeTargetReadback {
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    u32 width = 0, height = 0;
    ProbeTargetReadback() = default;
    ProbeTargetReadback(const ProbeTargetReadback&) = delete;
    ProbeTargetReadback& operator=(const ProbeTargetReadback&) = delete;
    ~ProbeTargetReadback() noexcept {
        if (mapped.pData)
            context->Unmap(staging, 0);
        com_release(staging);
    }
};

// Reads the drawn back buffer before flip-model Present advances it. Returns 0,
// or 1 (no texture target), 2 (viewport larger than the target or a BMP),
// 3 (staging texture) or 4 (map).
inline u32 probe_target_readback(D3D11Renderer& r, ProbeTargetReadback* out) noexcept {
    ID3D11Resource* resource{};
    ID3D11Texture2D* source{};
    r.graphics.target->GetResource(&resource);
    const bool texture =
        SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source)));
    com_release(resource);
    if (!texture)
        return 1;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    // Resize may retain a larger backing texture; only the active viewport and
    // the caption rows above it are read, each row following the mapped pitch.
    const u32 rows = r.height + r.caption_height;
    if (!r.width || !r.height || r.width > description.Width || rows > description.Height ||
        static_cast<u64>(r.width) * rows * 4 >
            MAXDWORD - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER)) {
        com_release(source);
        return 2;
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &out->staging))) {
        com_release(source);
        return 3;
    }
    out->context = r.graphics.context;
    out->context->CopyResource(out->staging, source);
    com_release(source);
    if (FAILED(out->context->Map(out->staging, 0, D3D11_MAP_READ, 0, &out->mapped))) {
        out->mapped = {};
        return 4;
    }
    out->width = r.width;
    out->height = rows;
    return 0;
}

// Writes the readback as a top-down 32-bit BMP when MAICORD_UI_PROBE_CAPTURE_DIR
// names a directory; otherwise writes nothing and succeeds.
inline bool probe_write_capture(const ProbeTargetReadback& readback, const wchar_t* filename) noexcept {
    wchar_t path[1024]{};
    u32 end = GetEnvironmentVariableW(L"MAICORD_UI_PROBE_CAPTURE_DIR", path, 900);
    if (end >= 900)
        return false;
    if (!end)
        return true;
    path[end++] = L'/';
    for (u32 i = 0; filename[i]; ++i) {
        if (end + 1 >= 1024)
            return false;
        path[end++] = filename[i];
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const auto write = [&](const void* data, u32 bytes) noexcept {
        DWORD written{};
        return WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    };
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + readback.width * readback.height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(readback.width);
    info.biHeight = -static_cast<LONG>(readback.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    bool valid = write(&header, sizeof(header)) && write(&info, sizeof(info));
    for (u32 y = 0; valid && y < readback.height; ++y)
        valid = write(static_cast<const u8*>(readback.mapped.pData) + y * readback.mapped.RowPitch, readback.width * 4);
    return CloseHandle(file) && valid;
}
} // namespace maicord

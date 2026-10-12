// GPU JPEG IDCT probe. MAICORD_JPEG_CORPUS=<directory> compares an external corpus against the
// reference shader and is manual; the fixture comparisons are maicord_media_gpu_reference.
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/core/log.hpp"
#include "jpeg_idct_reference.hpp"
#include "jpeg_idct_candidate.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {
struct CorpusState {
    JpegWorkspace workspace;
    u8 encoded[kJpegEncodedMaximumBytes];
    u8 rgba[2][512 * 512 * 4];
    u32 reference_samples[kJpegCoefficientBlockMaximum * 64];
    MediaAtlas atlas;
    MediaAtlasEntry entries[8];
    GlyphAtlasEntry allocations[8];
    GlyphAtlasRow rows[8];
    u32 buckets[16];
    MediaAtlasGpu gpu;
};
CorpusState corpus;
struct Difference {
    u64 changed_pixels, changed_channels, absolute_sum, squared_sum;
    u32 maximum;
};
struct Csv {
    HANDLE file;
    char data[2048];
    u32 count;
    void text(const char* value) noexcept {
        while (*value && count < sizeof(data))
            data[count++] = *value++;
    }
    void number(u64 value) noexcept {
        char digits[24];
        u32 size = 0;
        do {
            digits[size++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (size && count < sizeof(data))
            data[count++] = digits[--size];
    }
    void field(u64 value) noexcept {
        text(",");
        number(value);
    }
    bool flush() noexcept {
        text("\r\n");
        DWORD written = 0;
        const bool ok = WriteFile(file, data, count, &written, nullptr) && written == count;
        count = 0;
        return ok;
    }
};
bool path_join(wchar_t (&path)[1400], const wchar_t* directory, const wchar_t* name) noexcept {
    u32 i = 0;
    while (*directory && i < 1000)
        path[i++] = *directory++;
    if (*directory)
        return false;
    path[i++] = L'/';
    while (*name && i < 1399)
        path[i++] = *name++;
    path[i] = 0;
    return !*name;
}
bool write_bytes(const wchar_t* directory, const wchar_t* name, const void* bytes, u32 size) noexcept {
    wchar_t path[1400]{};
    if (!path_join(path, directory, name))
        return false;
    HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}
bool query(ID3D11DeviceContext* context, ID3D11Query* value, void* data, u32 bytes) noexcept {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    HRESULT result;
    while ((result = context->GetData(value, data, bytes, 0)) == S_FALSE && GetTickCount64() < deadline)
        Sleep(1);
    return result == S_OK;
}
u64 median(u64 a, u64 b, u64 c) noexcept {
    return maximum(minimum(a, b), minimum(maximum(a, b), c));
}
bool read_rgba(ID3D11DeviceContext* context, ID3D11Texture2D* staging, const MediaAtlasResult& allocation,
               u8* output) noexcept {
    context->CopyResource(staging, corpus.gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    for (u32 y = 0; y < allocation.height; ++y)
        copy_bytes(output + y * allocation.width * 4u,
                   static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch +
                       allocation.x * 4u,
                   allocation.width * 4u);
    context->Unmap(staging, 0);
    return true;
}
void bind_idct(ID3D11DeviceContext1* context, ID3D11ComputeShader* shader) noexcept {
    auto& gpu = corpus.gpu;
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &gpu.jpeg_idct_constants);
    ID3D11ShaderResourceView* views[]{gpu.vp8_coefficient_view, gpu.vp8_mode_view};
    context->CSSetShaderResources(0, 2, views);
    context->CSSetUnorderedAccessViews(0, 1, &gpu.vp8_yuv_uav, nullptr);
}
void unbind(ID3D11DeviceContext1* context) noexcept {
    ID3D11ShaderResourceView* views[2]{};
    ID3D11UnorderedAccessView* output = nullptr;
    context->CSSetShaderResources(0, 2, views);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
}
} // namespace

// Opt-in, offline corpus test. GPU timestamps exclude readback, file IO and CPU
// entropy decoding. Compare both raw IDCT planes and final oriented/color-managed
// atlas RGBA. Reference/candidate shaders never enter the shipping executable.
bool jpeg_gpu_probe(D3D11Renderer& renderer) noexcept {
    wchar_t directory[1000]{};
    if (!GetEnvironmentVariableW(L"MAICORD_JPEG_CORPUS", directory, 1000))
        return true;
    auto* device = renderer.graphics.device;
    auto* context = renderer.graphics.context1;
    auto& gpu = corpus.gpu;
    bool ok = media_atlas_initialize(&corpus.atlas, 1024, 1024, 1, {corpus.entries, 8},
                                     {corpus.buckets, 16}, {corpus.allocations, 8}, {corpus.rows, 8}) &&
              media_atlas_gpu_initialize(&gpu, device, context, 1024, 1024).ok();
    ID3D11ComputeShader* shaders[2]{};
    ok = ok &&
         SUCCEEDED(device->CreateComputeShader(g_jpeg_idct_reference, sizeof(g_jpeg_idct_reference),
                                               nullptr, &shaders[0])) &&
         SUCCEEDED(device->CreateComputeShader(g_jpeg_idct_candidate, sizeof(g_jpeg_idct_candidate),
                                               nullptr, &shaders[1]));
    ID3D11ComputeShader* shipping = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 1024;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* start = nullptr;
    ID3D11Query* end = nullptr;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ok = ok && SUCCEEDED(device->CreateQuery(&qd, &disjoint));
    qd.Query = D3D11_QUERY_TIMESTAMP;
    ok = ok && SUCCEEDED(device->CreateQuery(&qd, &start)) && SUCCEEDED(device->CreateQuery(&qd, &end));
    wchar_t path[1400]{};
    path_join(path, directory, L"gpu-results.csv");
    Csv csv{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        nullptr),
            {},
            0};
    ok = ok && csv.file != INVALID_HANDLE_VALUE;
    csv.text("file,width,height,blocks,progressive,components,max_delta,changed_pixels,changed_channels,"
             "abs_sum,squared_sum,idct_max_delta,idct_changed,idct_ref_ns,idct_candidate_ns,upload_ref_ns,"
             "upload_candidate_ns,error");
    ok = ok && csv.flush();
    WIN32_FIND_DATAW found{};
    path_join(path, directory, L"*.jpg");
    HANDLE search = ok ? FindFirstFileW(path, &found) : INVALID_HANDLE_VALUE;
    ok = ok && search != INVALID_HANDLE_VALUE;
    u32 cases = 0, rejected = 0;
    u64 worst_score = 0;
    while (ok) {
        path_join(path, directory, found.cFileName);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD bytes = 0;
        ok = file != INVALID_HANDLE_VALUE &&
             ReadFile(file, corpus.encoded, sizeof(corpus.encoded), &bytes, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        JpegImage image{};
        const auto decoded = jpeg_decode_coefficients({corpus.encoded, bytes}, &image, &corpus.workspace);
        for (u32 i = 0; found.cFileName[i] && csv.count < sizeof(csv.data); ++i)
            csv.data[csv.count++] = static_cast<char>(found.cFileName[i]);
        if (!decoded.ok()) {
            for (u32 i = 0; i < 16; ++i)
                csv.field(0);
            csv.field(static_cast<u32>(decoded.error));
            ok = csv.flush();
            ++rejected;
        } else if (ok) {
            media_atlas_reset(&corpus.atlas);
            const auto key = media_asset_attachment(987654321);
            // Initialize all shared resources and transfer the shipping shader's
            // ownership out while temporary A/B shader pointers are installed.
            const auto prepared = media_atlas_gpu_upload_jpeg(&gpu, &corpus.atlas, device, context, key,
                                                              image, corpus.workspace.coefficients);
            ok = prepared.ok();
            if (!shipping)
                shipping = gpu.jpeg_idct_shader;
            D3D11_BUFFER_DESC bd{};
            if (ok)
                gpu.vp8_yuv_buffer->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = bd.MiscFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Buffer* sample_staging = nullptr;
            ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &sample_staging));
            u32 samples = 0;
            for (u32 c = 0; c < image.component_count; ++c)
                samples += image.components[c].block_columns * image.components[c].block_rows * 64u;
            u64 plane_changed = 0;
            u32 plane_maximum = 0;
            for (u32 variant = 0; ok && variant < 2; ++variant) {
                gpu.jpeg_idct_shader = shaders[variant];
                const auto upload = media_atlas_gpu_upload_jpeg(&gpu, &corpus.atlas, device, context, key,
                                                                image, corpus.workspace.coefficients);
                ok = upload.ok() && read_rgba(context, staging, upload.allocation, corpus.rgba[variant]);
                context->CopyResource(sample_staging, gpu.vp8_yuv_buffer);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                ok = ok && SUCCEEDED(context->Map(sample_staging, 0, D3D11_MAP_READ, 0, &mapped));
                if (ok) {
                    const auto* pixels = static_cast<const u32*>(mapped.pData);
                    if (variant == 0)
                        copy_bytes(corpus.reference_samples, pixels, samples * 4u);
                    else
                        for (u32 i = 0; i < samples; ++i) {
                            const u32 a = pixels[i], b = corpus.reference_samples[i],
                                      delta = maximum(a, b) - minimum(a, b);
                            plane_changed += delta != 0;
                            plane_maximum = maximum(plane_maximum, delta);
                        }
                    context->Unmap(sample_staging, 0);
                }
            }
            com_release(sample_staging);
            const u32 pixels = static_cast<u32>(image.width) * image.height;
            Difference diff{};
            for (u32 i = 0; i < pixels; ++i) {
                bool changed = false;
                for (u32 c = 0; c < 4; ++c) {
                    const u32 a = corpus.rgba[0][i * 4 + c], b = corpus.rgba[1][i * 4 + c];
                    const u32 delta = maximum(a, b) - minimum(a, b);
                    changed |= delta != 0;
                    diff.changed_channels += delta != 0;
                    diff.maximum = maximum(diff.maximum, delta);
                    diff.absolute_sum += delta;
                    diff.squared_sum += delta * delta;
                }
                diff.changed_pixels += changed;
            }
            u64 timings[2][2][3]{};
            for (u32 trial = 0; ok && trial < 3; ++trial)
                for (u32 order = 0; ok && order < 2; ++order) {
                    const u32 variant = (order + trial + cases) % 2;
                    gpu.jpeg_idct_shader = shaders[variant];
                    for (u32 stage = 0; ok && stage < 2; ++stage) {
                        const u32 repetitions = stage == 0 ? 16 : 4;
                        if (stage == 0)
                            bind_idct(context, shaders[variant]);
                        context->Begin(disjoint);
                        context->End(start);
                        for (u32 n = 0; ok && n < repetitions; ++n) {
                            if (stage == 0)
                                context->Dispatch(image.coefficient_blocks, 1, 1);
                            else
                                ok = media_atlas_gpu_upload_jpeg(&gpu, &corpus.atlas, device, context, key,
                                                                 image, corpus.workspace.coefficients)
                                         .ok();
                        }
                        context->End(end);
                        context->End(disjoint);
                        unbind(context);
                        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
                        u64 a = 0, b = 0;
                        ok = ok && query(context, disjoint, &frequency, sizeof(frequency)) &&
                             !frequency.Disjoint && query(context, start, &a, sizeof(a)) &&
                             query(context, end, &b, sizeof(b));
                        if (ok)
                            timings[stage][variant][trial] =
                                (b - a) * 1000000000ull / frequency.Frequency / repetitions;
                    }
                }
            csv.field(jpeg_display_width(image));
            csv.field(jpeg_display_height(image));
            csv.field(image.coefficient_blocks);
            csv.field(image.progressive);
            csv.field(image.component_count);
            csv.field(diff.maximum);
            csv.field(diff.changed_pixels);
            csv.field(diff.changed_channels);
            csv.field(diff.absolute_sum);
            csv.field(diff.squared_sum);
            csv.field(plane_maximum);
            csv.field(plane_changed);
            for (auto& stage : timings)
                for (auto& variant : stage)
                    csv.field(median(variant[0], variant[1], variant[2]));
            csv.field(ok ? 0 : 999);
            ok = csv.flush() && ok;
            const u64 score = diff.maximum * 1000000000ull + diff.changed_pixels * 1000000000ull / pixels;
            if (ok && score > worst_score) {
                worst_score = score;
                ok = write_bytes(directory, L"worst.reference.rgba", corpus.rgba[0], pixels * 4) &&
                     write_bytes(directory, L"worst.candidate.rgba", corpus.rgba[1], pixels * 4);
                Csv detail{INVALID_HANDLE_VALUE, {}, 0};
                for (u32 i = 0; found.cFileName[i] && detail.count < sizeof(detail.data); ++i)
                    detail.data[detail.count++] = static_cast<char>(found.cFileName[i]);
                detail.field(jpeg_display_width(image));
                detail.field(jpeg_display_height(image));
                ok = ok && write_bytes(directory, L"worst.txt", detail.data, detail.count);
            }
            ++cases;
            if (cases % 50 == 0)
                log_u64(LogLevel::info, "jpeg_probe", "completed", cases);
        }
        if (!FindNextFileW(search, &found))
            break;
    }
    if (search != INVALID_HANDLE_VALUE)
        FindClose(search);
    if (csv.file != INVALID_HANDLE_VALUE)
        CloseHandle(csv.file);
    gpu.jpeg_idct_shader = shipping;
    media_atlas_gpu_release(&gpu);
    for (auto*& shader : shaders)
        com_release(shader);
    com_release(staging);
    com_release(disjoint);
    com_release(start);
    com_release(end);
    log_u64(LogLevel::info, "jpeg_probe", "cases", cases);
    log_u64(LogLevel::info, "jpeg_probe", "rejected", rejected);
    return ok && cases != 0 && rejected == 0;
}
} // namespace maicord

#include "maicord/core/time.hpp"
#include "maicord/render/glyph_atlas.hpp"
#include "maicord/render/glyph_atlas_gpu.hpp"
#include "maicord/render/gpu_timing.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/render/native_glyph_outlines.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/text/font_session.hpp"

#include <d3d11_1.h>

namespace maicord {
namespace {

inline constexpr u32 kDispatchIterations = 256;

wchar_t g_utf16[16];
TextGlyph g_glyphs[16];
TextRun g_runs[4];
u16 g_clusters[16];
GlyphPathCommand g_path_commands[512];
GlyphAtlasEntry g_atlas_entries[32];
GlyphAtlasRow g_atlas_rows[16];
// Mirrors the renderer glyph atlas: 1024x1024, 2048 entries, 128 rows.
inline constexpr u32 kFullEntries = 2048;
inline constexpr u32 kLookupIterations = 200'000;
inline constexpr u32 kChurnIterations = 100'000;
GlyphAtlasEntry g_full_entries[kFullEntries];
GlyphAtlasRow g_full_rows[128];
u16 g_full_index[kFullEntries * 2];

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value), &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

bool wait_query(ID3D11DeviceContext* context, ID3D11Query* query, void* output, u32 output_bytes) noexcept {
    for (u32 attempt = 0; attempt < 1'000'000; ++attempt) {
        const HRESULT result = context->GetData(query, output, output_bytes, 0);
        if (result == S_OK) {
            return true;
        }
        if (FAILED(result)) {
            return false;
        }
        Sleep(0);
    }
    return false;
}

constexpr u64 full_key(u32 index) noexcept {
    return (1ull << 48) | (96ull << 32) | (1024ull << 16) | (index & 0xFFFFu) | (static_cast<u64>(index >> 16) << 40);
}

u64 nanoseconds_per(const Clock& clock, i64 begin, i64 end, u32 operations) noexcept {
    return clock_microseconds(clock, begin, end) * 1000ull / operations;
}

void write_prefixed(const char* prefix, const char* name, u64 value) noexcept {
    write_text(prefix);
    write_metric(name, value);
}

// CPU-only full-occupancy lookup/churn cost, with the renderer's key index or
// the linear fallback. Returns false on any incorrect result.
bool run_full_occupancy(const Clock& clock, bool indexed) noexcept {
    GlyphAtlasAllocator atlas{};
    GlyphAtlasIndex storage{};
    GlyphAtlasIndex* index = indexed ? &storage : nullptr;
    if (!glyph_atlas_initialize(&atlas, 1024, 1024, 1, Slice<GlyphAtlasEntry>{g_full_entries, kFullEntries},
                                Slice<GlyphAtlasRow>{g_full_rows, 128}) ||
        (indexed && !glyph_atlas_index_initialize(index, Slice<u16>{g_full_index, kFullEntries * 2})))
        return false;
    for (u32 entry = 0; entry < kFullEntries; ++entry)
        if (!glyph_atlas_allocate_unique_indexed(&atlas, index, full_key(entry), 9, 11).ok())
            return false;
    if (atlas.entry_count != kFullEntries)
        return false;
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kLookupIterations; ++iteration) {
        const GlyphAtlasAllocation found = glyph_atlas_find_indexed(&atlas, index, full_key((iteration * 7919u) % kFullEntries));
        if (!found.ok())
            return false;
        checksum += found.x + found.handle.slot;
    }
    const u64 resident_ns = nanoseconds_per(clock, begin, clock_now(), kLookupIterations);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLookupIterations; ++iteration) {
        if (glyph_atlas_find_indexed(&atlas, index, full_key(kFullEntries + iteration)).ok())
            return false;
        ++checksum;
    }
    const u64 missing_ns = nanoseconds_per(clock, begin, clock_now(), kLookupIterations);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kChurnIterations; ++iteration) {
        const GlyphAtlasAllocation inserted =
            glyph_atlas_allocate_unique_indexed(&atlas, index, full_key(0x10000u + iteration), 9, 11);
        if (!inserted.ok())
            return false;
        checksum += inserted.handle.slot;
    }
    const u64 churn_ns = nanoseconds_per(clock, begin, clock_now(), kChurnIterations);
    // Every key from the final churn window must still resolve after evictions.
    for (u32 iteration = kChurnIterations - 64; iteration < kChurnIterations; ++iteration)
        if (!glyph_atlas_find_indexed(&atlas, index, full_key(0x10000u + iteration)).ok())
            return false;
    const char* prefix = indexed ? "atlas_full_" : "atlas_linear_";
    write_prefixed(prefix, "entries", atlas.entry_count);
    write_prefixed(prefix, "resident_lookup_ns", resident_ns);
    write_prefixed(prefix, "missing_lookup_ns", missing_ns);
    write_prefixed(prefix, "churn_allocate_ns", churn_ns);
    write_prefixed(prefix, "evicted_entries", atlas.evicted_entries);
    write_prefixed(prefix, "index_rebuilds", storage.rebuilds);
    write_prefixed(prefix, "checksum", checksum);
    return true;
}

int run() noexcept {
    if (!run_full_occupancy(clock_initialize(), false) || !run_full_occupancy(clock_initialize(), true)) {
        return 11;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                          &device, &feature_level, &context);
    if (FAILED(result) || feature_level < D3D_FEATURE_LEVEL_11_0) {
        com_release(context);
        com_release(device);
        return 1;
    }

    TextSystem text_system{};
    ShapedText shaped{};
    GlyphOutline outline{};
    GlyphAtlasAllocator allocator{};
    GlyphAtlasGpu gpu{};
    if (!text_system_initialize(&text_system, L"Segoe UI", 16.0f, L"en-us").ok() ||
        !shaped_text_initialize(&shaped, Slice<TextGlyph>{g_glyphs, 16}, Slice<TextRun>{g_runs, 4},
                                Slice<u16>{g_clusters, 16}) ||
        !glyph_outline_initialize(&outline, Slice<GlyphPathCommand>{g_path_commands, 512}) ||
        !glyph_atlas_initialize(&allocator, 256, 256, 1, Slice<GlyphAtlasEntry>{g_atlas_entries, 32},
                                Slice<GlyphAtlasRow>{g_atlas_rows, 16}) ||
        !glyph_atlas_gpu_initialize(&gpu, device, 256, 256, 512).ok()) {
        glyph_atlas_gpu_release(&gpu);
        shaped_text_reset(&shaped);
        text_system_release(&text_system);
        com_release(context);
        com_release(device);
        return 2;
    }

    const TextShapeResult shaped_result = text_shape_utf8(&text_system, utf8_literal("A"), 64.0f, 64.0f,
                                                          Slice<wchar_t>{g_utf16, 16}, &shaped);
    // The renderer's outline owner binds the run's font instance and scratch.
    NativeGlyphOutlines* outlines = nullptr;
    if (!shaped_result.ok() || shaped.run_count != 1 || !shaped.native_fonts() || shaped.glyph_count != 1 ||
        !native_glyph_outline(&outlines, 1, shaped.glyphs[0].index, &outline, shaped.native_fonts(),
                              shaped.runs[0].native_face)) {
        native_glyph_outlines_release(&outlines);
        glyph_atlas_gpu_release(&gpu);
        shaped_text_reset(&shaped);
        text_system_release(&text_system);
        com_release(context);
        com_release(device);
        return 3;
    }
    native_glyph_outlines_release(&outlines);

    GlyphRasterBounds bounds{};
    if (!glyph_raster_bounds(outline, 32.0f, 1, &bounds)) {
        return 4;
    }
    const GlyphAtlasAllocation allocation =
        glyph_atlas_allocate(&allocator, 1, bounds.width, bounds.height);
    if (!allocation.ok()) {
        return 5;
    }

    D3D11_QUERY_DESC disjoint_description{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC timestamp_description{D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query* disjoint_query = nullptr;
    ID3D11Query* begin_query = nullptr;
    ID3D11Query* end_query = nullptr;
    result = device->CreateQuery(&disjoint_description, &disjoint_query);
    if (SUCCEEDED(result)) {
        result = device->CreateQuery(&timestamp_description, &begin_query);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateQuery(&timestamp_description, &end_query);
    }
    if (FAILED(result)) {
        return 6;
    }

    const Clock clock = clock_initialize();
    const i64 cpu_begin = clock_now();
    context->Begin(disjoint_query);
    context->End(begin_query);
    bool dispatch_ok = true;
    for (u32 iteration = 0; iteration < kDispatchIterations; ++iteration) {
        GlyphRasterBatch batch(&gpu, context);
        dispatch_ok = dispatch_ok && batch.add(outline, bounds, allocation, 32.0f, allocator.padding).ok() &&
                      batch.finish();
    }
    context->End(end_query);
    context->End(disjoint_query);
    const u64 cpu_us = clock_microseconds(clock, cpu_begin, clock_now());
    context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    u64 gpu_begin = 0;
    u64 gpu_end = 0;
    const bool timed = dispatch_ok && wait_query(context, disjoint_query, &disjoint, sizeof(disjoint)) &&
                       wait_query(context, begin_query, &gpu_begin, sizeof(gpu_begin)) &&
                       wait_query(context, end_query, &gpu_end, sizeof(gpu_end)) && !disjoint.Disjoint &&
                       disjoint.Frequency != 0 && gpu_end >= gpu_begin;
    if (!timed) {
        return 7;
    }
    const u64 gpu_us = gpu_timestamp_microseconds(gpu_end - gpu_begin, disjoint.Frequency);

    D3D11_TEXTURE2D_DESC staging_description{};
    staging_description.Width = gpu.width;
    staging_description.Height = gpu.height;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Format = DXGI_FORMAT_R8_UNORM;
    staging_description.SampleDesc.Count = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    result = device->CreateTexture2D(&staging_description, nullptr, &staging);
    if (FAILED(result)) {
        return 8;
    }
    context->CopyResource(staging, gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        return 9;
    }
    u64 coverage_sum = 0;
    u32 covered_pixels = 0;
    u32 partial_pixels = 0;
    for (u32 y = 0; y < allocation.height; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) +
                        (static_cast<u32>(allocation.y) + y) * mapped.RowPitch + allocation.x;
        for (u32 x = 0; x < allocation.width; ++x) {
            coverage_sum += row[x];
            covered_pixels += row[x] != 0;
            partial_pixels += row[x] != 0 && row[x] != 255;
        }
    }
    context->Unmap(staging, 0);

    write_metric("glyph_commands", outline.count);
    write_metric("glyph_width", bounds.width);
    write_metric("glyph_height", bounds.height);
    write_metric("glyph_covered_pixels", covered_pixels);
    write_metric("glyph_partial_pixels", partial_pixels);
    write_metric("glyph_coverage_sum", coverage_sum);
    write_metric("glyph_dispatch_count", gpu.dispatch_count);
    write_metric("glyph_dispatch_cpu_batch_us", cpu_us);
    write_metric("glyph_dispatch_gpu_batch_us", gpu_us);

    const bool valid_output = coverage_sum != 0 && covered_pixels != 0 && partial_pixels != 0 &&
                              gpu.dispatch_count == kDispatchIterations;
    com_release(staging);
    com_release(end_query);
    com_release(begin_query);
    com_release(disjoint_query);
    glyph_atlas_gpu_release(&gpu);
    shaped_text_reset(&shaped);
    text_system_release(&text_system);
    com_release(context);
    com_release(device);
    return valid_output ? 0 : 10;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_glyph_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}

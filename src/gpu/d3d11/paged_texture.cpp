#include "maicord/gpu/api.hpp"
#include "maicord/core/heap.hpp"

// Paged textures on Direct3D 11: one tiled texture whose tile bands map bands of a tile pool (api.hpp).
namespace maicord {
namespace {

constexpr u32 kTileBytes = 64u * 1024u;

// The pool keeps each texture band's tiles together: band slot s is pool tiles [s * tiles_across, ...). Unbacked
// bands map nothing (kUnmapped).
constexpr u32 kUnmapped = ~0u;

u64 pool_bytes(const GpuPagedTexture& paged, u32 bands) noexcept {
    return u64{bands} * paged.tiles_across * kTileBytes;
}

u32 lowest_free_slot(const GpuPagedTexture& paged) noexcept {
    u64 used[2]{};
    for (const u8 slot : paged.band_slot) {
        if (slot) {
            used[(slot - 1u) >> 6] |= 1ull << ((slot - 1u) & 63u);
        }
    }
    for (u32 slot = 0;; ++slot) {
        if (!(used[slot >> 6] & (1ull << (slot & 63u)))) {
            return slot;
        }
    }
}

u32 slots_in_use(const GpuPagedTexture& paged) noexcept {
    u32 top = 0;
    for (const u8 slot : paged.band_slot) {
        top = maximum<u32>(top, slot);
    }
    return top;
}

// Maps `count` bands from `band` to nothing, or one band to `slot`.
HRESULT map_bands(ID3D11DeviceContext2* context, GpuPagedTexture* paged, u32 band, u32 count, u32 slot) noexcept {
    const D3D11_TILED_RESOURCE_COORDINATE coordinate{0, band, 0, 0};
    const UINT tiles = paged->tiles_across * count;
    const D3D11_TILE_REGION_SIZE size{tiles, TRUE, paged->tiles_across, static_cast<UINT16>(count), 1};
    const UINT flags = slot == kUnmapped ? static_cast<UINT>(D3D11_TILE_RANGE_NULL) : 0u;
    const UINT start = slot == kUnmapped ? 0u : slot * paged->tiles_across;
    return context->UpdateTileMappings(paged->texture, 1, &coordinate, &size, paged->pool, 1, &flags, &start, &tiles,
                                       0);
}

} // namespace

// Creation, commits and release run per atlas page: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
GpuStatus gpu_paged_texture_create(GpuDevice* device, const GpuPagedTextureDesc& desc, GpuPagedTexture** texture) {
    *texture = nullptr;
    const u32 texel = gpu_texel_bytes(desc.format);
    const u32 band_rows = gpu_paged_commit_rows(texel);
    const u32 height = desc.page_rows * desc.pages;
    // Never a render target: a renderable tiled texture must be cleared through every tile whose mapping changed
    // before it is used, and whole tiles at that ("Tile access limitations with duplicate mappings"), which the
    // partial clears of atlas entries cannot do. Atlases are written by compute and cleared through their UAV.
    if (!device || desc.width == 0 || desc.pages == 0 || desc.pages > kGpuPagedTextureMaximumPages ||
        desc.page_rows == 0 || (desc.pages > 1 && desc.page_rows % band_rows != 0) ||
        (height + band_rows - 1u) / band_rows > 128 || height > 16384 || (texel != 1 && texel != 4) ||
        (desc.bind & gpu_bind_rtv)) {
        return E_INVALIDARG;
    }
    auto* paged = static_cast<GpuPagedTexture*>(heap_allocate(sizeof(GpuPagedTexture), true));
    if (!paged) {
        return E_OUTOFMEMORY;
    }
    paged->page_rows = desc.page_rows;
    paged->pages = desc.pages;
    paged->band_rows = band_rows;
    // A tile is 64 KiB: 128 x 128 texels of 4 bytes, 256 x 256 of 1.
    const u32 tile_width = texel == 1 ? 256u : 128u;
    paged->tiles_across = (desc.width + tile_width - 1u) / tile_width;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = desc.width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = static_cast<DXGI_FORMAT>(desc.format);
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = desc.bind;
    description.MiscFlags = D3D11_RESOURCE_MISC_TILED;
    HRESULT result = device->CreateTexture2D(&description, nullptr, &paged->texture);
    if (SUCCEEDED(result) && (desc.bind & gpu_bind_srv)) {
        result = device->CreateShaderResourceView(paged->texture, nullptr, &paged->view);
    }
    if (SUCCEEDED(result) && (desc.bind & gpu_bind_uav)) {
        result = device->CreateUnorderedAccessView(paged->texture, nullptr, &paged->uav);
    }
    if (SUCCEEDED(result)) {
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        result = device->CreateQuery(&query, &paged->unmapped);
    }
    if (FAILED(result)) {
        gpu_paged_texture_release(&paged);
        return result;
    }
    *texture = paged;
    return S_OK;
}

void gpu_paged_texture_release(GpuPagedTexture** texture) {
    GpuPagedTexture* paged = *texture;
    if (!paged) {
        return;
    }
    com_release(paged->unmapped);
    com_release(paged->uav);
    com_release(paged->view);
    com_release(paged->texture);
    com_release(paged->pool);
    heap_free(paged);
    *texture = nullptr;
}

GpuStatus gpu_paged_texture_commit(GpuContext* context, GpuPagedTexture* paged, u32 page, u32 rows) {
    if (!context || !paged || page >= paged->pages || rows > paged->page_rows) {
        return E_INVALIDARG;
    }
    // Rows round up to whole bands both ways: a page of a part band holds that band.
    const u32 want = (rows + paged->band_rows - 1u) / paged->band_rows;
    const u32 have = (paged->committed[page] + paged->band_rows - 1u) / paged->band_rows;
    // The pool gives bands back only once the GPU has passed the unmaps that freed them: ResizeTilePool frees the
    // memory at once, and work queued before the unmaps may still write it, so shrinking earlier is a use after free
    // on any GPU. The event `unmapped` is ended after an unmap; an unmap while it is pending ends it again once it
    // has passed.
    bool shrink = false;
    if (paged->shrinking && context->GetData(paged->unmapped, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        if (paged->unmapped_again) {
            context->End(paged->unmapped);
            paged->unmapped_again = false;
        } else {
            shrink = true;
        }
    }
    if (want == have && !shrink) {
        paged->committed[page] = static_cast<u16>(rows);
        return S_OK;
    }
    // Tiled Resources Tier 1, the device floor, comes with Direct3D 11.2's context.
    ID3D11DeviceContext2* tiles = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void**>(&tiles));
    HRESULT result = S_OK;
    const u32 first_band = page * (paged->page_rows / paged->band_rows);
    for (u32 band = have; SUCCEEDED(result) && band < want; ++band) {
        const u32 slot = lowest_free_slot(*paged);
        if (slot >= paged->pool_bands) {
            if (!paged->pool) {
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = static_cast<UINT>(pool_bytes(*paged, slot + 1u));
                description.Usage = D3D11_USAGE_DEFAULT;
                description.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                result = device->CreateBuffer(&description, nullptr, &paged->pool);
                device->Release();
            } else {
                result = tiles->ResizeTilePool(paged->pool, pool_bytes(*paged, slot + 1u));
            }
            if (FAILED(result)) {
                break;
            }
            paged->pool_bands = slot + 1u;
        }
        result = map_bands(tiles, paged, first_band + band, 1, slot);
        if (SUCCEEDED(result)) {
            paged->band_slot[first_band + band] = static_cast<u8>(slot + 1u);
            paged->committed[page] = static_cast<u16>((band + 1u) * paged->band_rows);
        }
    }
    // The bands past the last one in use go back.
    if (shrink && SUCCEEDED(result)) {
        const u32 in_use = slots_in_use(*paged);
        if (in_use == paged->pool_bands || SUCCEEDED(tiles->ResizeTilePool(paged->pool, pool_bytes(*paged, in_use)))) {
            paged->pool_bands = in_use;
            paged->shrinking = false;
        }
    }
    if (want < have && SUCCEEDED(result)) {
        result = map_bands(tiles, paged, first_band + want, have - want, kUnmapped);
        if (SUCCEEDED(result)) {
            for (u32 band = want; band < have; ++band) {
                paged->band_slot[first_band + band] = 0;
            }
            if (slots_in_use(*paged) == 0) {
                // Nothing is backed: the pool goes whole, and a released pool lives until the GPU is done with it.
                com_release(paged->pool);
                paged->pool_bands = 0;
            } else if (paged->shrinking) {
                paged->unmapped_again = true;
            } else {
                context->End(paged->unmapped);
                paged->shrinking = true;
            }
        }
    }
    if (SUCCEEDED(result)) {
        paged->committed[page] = static_cast<u16>(rows);
    }
    tiles->Release();
    return result;
}

u64 gpu_paged_texture_bytes(const GpuPagedTexture* texture) {
    return texture ? pool_bytes(*texture, texture->pool_bands) : 0;
}
#pragma clang attribute pop

} // namespace maicord

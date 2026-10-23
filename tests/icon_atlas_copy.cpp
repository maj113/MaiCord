#include "maicord/render/icon_atlas.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
IconAtlas atlas;
PaintQuad source[8], destination[10];
u32 failures;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
bool resolve(IconAtlas* owner, ID3D11DeviceContext* context, u32 count, u32 dpi=96) noexcept {
    return icon_atlas_resolve(owner, context, source, destination + 1, count, dpi, 1);
}
void reset_output() noexcept {
    memset(destination, 0x5a, sizeof(destination));
}
void guards(u32 count) noexcept {
    u8 expected[sizeof(PaintQuad)];
    memset(expected, 0x5a, sizeof(expected));
    check(bytes_equal(destination, expected, sizeof(expected)) && bytes_equal(destination + count + 1, expected, sizeof(expected)), "icon copy: output guards");
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_icon_atlas_copy_entry() noexcept {
    using namespace maicord;
    // No GPU work is admitted: the unavailable-atlas and missing-resource paths
    // must finish without calling through this non-null context token.
    u64 context_token = 0;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(&context_token);
    for (u32 i = 0; i < 8; ++i) {
        source[i].right = source[i].bottom = 16;
        source[i].fill = {.2f, .4f, .6f, 1};
        source[i].reserved_x = source[i].reserved_y = 99;
        if (i % 4 == 1)
            source[i].border_width = kPaintIconMode;
    }
    reset_output();
    check(resolve(&atlas, context, 8), "icon copy: unavailable atlas fallback");
    for (u32 i = 0; i < 8; ++i) {
        auto expected = source[i];
        if (ui_icon_quad(expected))
            expected.reserved_x = expected.reserved_y = -1;
        check(bytes_equal(destination + i + 1, &expected, sizeof(expected)), "icon copy: plain and unresolved quads");
    }
    guards(8);
    reset_output();
    check(!resolve(nullptr, context, 8), "icon copy: null atlas rejection");
    check(bytes_equal(destination + 1, source, 8 * sizeof(PaintQuad)), "icon copy: fallback copies every quad");
    guards(8);
    reset_output();
    check(!resolve(&atlas, context, 8, 0), "icon copy: zero DPI rejection");
    check(bytes_equal(destination + 1, source, 8 * sizeof(PaintQuad)), "icon copy: fallback copies every quad");
    guards(8);
    check(glyph_atlas_initialize(&atlas.allocator, kIconAtlasExtent, kIconAtlasExtent, 1,
        {atlas.entries, kIconAtlasEntries}, {atlas.rows, kIconAtlasRows}), "icon copy: initialize allocator");
    atlas.ready = true;
    reset_output();
    check(!resolve(&atlas, context, 8), "icon copy: missing GPU resources reject rasterization");
    check(bytes_equal(destination + 1, source, 8 * sizeof(PaintQuad)) && atlas.gpu.rejected == 1, "icon copy: raster failure preserves current quad and tail");
    guards(8);
    atlas.ready = false;
    reset_output();
    check(!resolve(&atlas, nullptr, 8), "icon copy: null context rejection");
    check(bytes_equal(destination + 1, source, 8 * sizeof(PaintQuad)), "icon copy: null context plain copy");
    guards(8);
    reset_output();
    check(resolve(&atlas, context, 0), "icon copy: empty stream");
    guards(0);
    ExitProcess(failures ? 1 : 0);
}

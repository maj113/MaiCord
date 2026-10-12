#include "glyph_effect_ordinals.hpp"
#include "maicord/render/d3d11_glyph_text.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/ansi_text.hpp"
#include "maicord/render/colr_paint.hpp"
#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/render/native_glyph_outlines.hpp"
#include "maicord/text/font_run.hpp"

#include <d3d11_1.h>

namespace maicord {
struct D3D11ColorGlyphOwner {
    ColrPaintProgram program;
    ColrPaintGpu gpu;
    // Direct-mapped dependency hints. Identity includes face and glyph; a
    // collision only causes a rebuild, never reuse with the wrong foreground.
    u32 foreground_hints[256];
};
ID3D11ShaderResourceView* d3d11_glyph_text_color_view(const D3D11GlyphTextOwner& owner) noexcept {
    return owner.color ? owner.color->gpu.view : nullptr;
}
namespace {
// Worker-compiled programs borrow account-module storage only for this call.
bool rasterize_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                       const ColrPaintProgram& program, const GlyphAtlasAllocation& allocation,
                       u16 extent, float pixels_per_unit, u16 target_width, u16 target_height,
                       u16 padding, ID3D11UnorderedAccessView* target) noexcept {
    if (!owner || !owner->gpu.width || !context || !target || allocation.width != extent ||
        allocation.height != extent)
        return false;
    if (owner->color_batch) {
        if (!owner->color_batch->finish())
            return false;
        owner->color_batch->reset();
    }
    if (!owner->color)
        owner->color = static_cast<D3D11ColorGlyphOwner*>(
            VirtualAlloc(nullptr, sizeof(D3D11ColorGlyphOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner->color)
        return false;
    const GlyphRasterBounds bounds{0, 0, extent, extent, extent, extent};
    return colr_paint_rasterize_target(&owner->color->gpu, context, program, bounds, allocation,
                                       pixels_per_unit, owner->gpu.width, target_width, target_height,
                                       padding, target);
}
} // namespace
bool d3d11_glyph_text_rasterize_flag_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                                             const ColrPaintProgram& program,
                                             const GlyphAtlasAllocation& allocation, u16 target_width,
                                             u16 target_height, u16 padding,
                                             ID3D11UnorderedAccessView* target) noexcept {
    return rasterize_program(owner, context, program, allocation, 64, kRegionalFlagPixelsPerUnit,
                             target_width, target_height, padding, target);
}
bool d3d11_glyph_text_rasterize_lottie_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
    const ColrPaintProgram& program,const GlyphAtlasAllocation& allocation,u16 target_width,
    u16 target_height,u16 padding,ID3D11UnorderedAccessView* target) noexcept {
    return rasterize_program(owner, context, program, allocation, 128, 128.f, target_width, target_height,
                             padding, target);
}
bool d3d11_glyph_text_gpu_footprint(const D3D11GlyphTextOwner& owner,
                                    D3D11GlyphGpuFootprint* output) noexcept {
    if (!output)
        return false;
    *output = {};
    const auto texture = [&](ID3D11Texture2D* resource, DXGI_FORMAT format, u32 bytes) noexcept {
        if (!resource)
            return true;
        D3D11_TEXTURE2D_DESC d{};
        resource->GetDesc(&d);
        if (d.Format != format || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1)
            return false;
        output->texture_bytes += static_cast<u64>(d.Width) * d.Height * bytes;
        return true;
    };
    const auto buffer = [&](ID3D11Buffer* resource) noexcept {
        if (resource) {
            D3D11_BUFFER_DESC d{};
            resource->GetDesc(&d);
            output->buffer_bytes += d.ByteWidth;
        }
    };
    if (!texture(owner.gpu.texture, DXGI_FORMAT_R8_UNORM, 1))
        return false;
    buffer(owner.gpu.command_buffer);
    output->mono_pixels = owner.gpu.dispatched_pixels;
    output->mono_commands = owner.gpu.dispatched_commands;
    output->mono_dispatches = owner.gpu.raster_count;
    if (owner.color) {
        const auto& g = owner.color->gpu;
        if (!texture(g.texture, DXGI_FORMAT_R8G8B8A8_UNORM, 4))
            return false;
        buffer(g.program);
        buffer(g.constants);
        output->color_dispatches = g.dispatches;
    }
    return true;
}
namespace {
constexpr u16 color_face_flag = 2;

struct GlyphTextStorage {
    wchar_t utf16[kRendererGlyphCapacity];
    TextGlyph glyphs[kRendererGlyphCapacity];
    TextRun runs[kRendererTextRunCapacity];
    u16 clusters[kRendererGlyphCapacity];
    GlyphPathCommand path_commands[kRendererPathCapacity];
    GlyphAtlasEntry atlas_entries[kRendererAtlasEntryCapacity];
    GlyphAtlasRow atlas_rows[kRendererAtlasRowCapacity];
    GlyphRasterOrigin atlas_origins[kRendererAtlasEntryCapacity];
    GlyphAtlasIndex atlas_index;
    u16 atlas_index_buckets[kRendererAtlasEntryCapacity * 2];
    GlyphQuad title_quads[kRendererGlyphCapacity];
};

// The owner header has a fixed size budget, so storage-resident state is
// reached through the storage block that owns atlas_origins.
GlyphTextStorage* text_storage(D3D11GlyphTextOwner* owner) noexcept {
    return reinterpret_cast<GlyphTextStorage*>(reinterpret_cast<u8*>(owner->atlas_origins) -
                                               offsetof(GlyphTextStorage, atlas_origins));
}

GlyphAtlasIndex* atlas_index(D3D11GlyphTextOwner* owner) noexcept {
    return owner->atlas_origins ? &text_storage(owner)->atlas_index : nullptr;
}

bool initialize_storage(D3D11GlyphTextOwner* owner) noexcept {
    if (!owner || owner->storage.base ||
        !arena_initialize(&owner->storage,
                          sizeof(GlyphTextStorage) +
                              sizeof(D3D11GlyphTextFontFace) * kRendererFontFaceCapacity,
                          0, 4 * 1024)) {
        return false;
    }
    // VirtualAlloc supplies zero bytes in this fresh arena. Do not touch unused pages.
    auto* storage = arena_allocate<GlyphTextStorage>(&owner->storage);
    if (!storage) {
        return false;
    }
    owner->utf16 = storage->utf16;
    owner->glyphs = storage->glyphs;
    owner->runs = storage->runs;
    owner->clusters = storage->clusters;
    owner->path_commands = storage->path_commands;
    owner->atlas_entries = storage->atlas_entries;
    owner->atlas_rows = storage->atlas_rows;
    owner->atlas_origins = storage->atlas_origins;
    owner->title_quads = storage->title_quads;
    owner->font_faces =
        arena_allocate<D3D11GlyphTextFontFace>(&owner->storage, glyph_font_initial_capacity);
    if (!owner->font_faces)
        return false;
    owner->font_face_capacity = static_cast<u16>(glyph_font_initial_capacity);
    return true;
}

bool reserve_font_record(D3D11GlyphTextOwner* owner) noexcept {
    if (owner->font_face_count < owner->font_face_capacity)
        return true;
    if (owner->font_face_capacity >= kRendererFontFaceCapacity)
        return false;
    const u32 added =
        minimum(glyph_font_initial_capacity, kRendererFontFaceCapacity - owner->font_face_capacity);
    // Font records are the arena's final allocation. Append in place so live
    // font IDs, outline bindings, atlas keys and retained quads stay valid.
    auto* next = arena_allocate_zero<D3D11GlyphTextFontFace>(&owner->storage, added);
    if (!next || next != owner->font_faces + owner->font_face_capacity)
        return false;
    owner->font_face_capacity = static_cast<u16>(owner->font_face_capacity + added);
    return true;
}

enum class ColorGlyphAppendResult : u8 {
    not_color,
    appended,
    failed,
};

D3D11GlyphTextInitializeResult initialize_result(D3D11GlyphTextInitializeError error,
                                                 HRESULT native_error = S_OK,
                                                 TextShapeError text_error = TextShapeError::none,
                                                 GlyphAtlasGpuError gpu_error = GlyphAtlasGpuError::none,
                                                 bool color_glyphs = false) noexcept {
    return D3D11GlyphTextInitializeResult{native_error, text_error, gpu_error, error, color_glyphs};
}

// Renderer font IDs are stable and retain one native run reference each.
u16 retain_run_font(D3D11GlyphTextOwner* owner, const ShapedText& text, const TextRun& run) noexcept {
    if (!run.native_font || !text.native_fonts() || text.native_fonts() != owner->system.native_fonts)
        return 0;
    const NativeFontRunHandle handle = run.native_face;
    auto* store = native_font_session_runs(owner->system.native_fonts);
    const auto* source = store ? native_font_run_get(*store, handle) : nullptr;
    if (!source)
        return 0;
    for (u32 i = 0; i < owner->font_face_count; ++i) {
        const auto& face = owner->font_faces[i];
        if (face.native_face.slot == handle.slot && face.native_face.generation == handle.generation)
            return face.id;
    }
    if (!reserve_font_record(owner)) {
        log_u64(LogLevel::error, "render", "glyph_font_face_capacity", owner->font_face_count);
        return 0;
    }
    if (!native_font_run_retain(store, handle))
        return 0;
    auto& face = owner->font_faces[owner->font_face_count++];
    face = {};
    face.native_face = handle;
    face.id = static_cast<u16>(owner->font_face_count);
    const auto* font =
        native_font_get(*native_font_session_fonts(owner->system.native_fonts), source->source.face);
    ConstSlice<u8> colr{};
    if (font && sfnt_table(font->geometry.face, sfnt_tag('C', 'O', 'L', 'R'), &colr) == SfntStatus::ok &&
        colr.count)
        face.flags |= color_face_flag;
    return face.id;
}

bool retained_font_face_is_color(const D3D11GlyphTextOwner* owner, u16 face_id) noexcept {
    return owner && face_id != 0 && face_id <= owner->font_face_count &&
           (owner->font_faces[face_id - 1u].flags & color_face_flag) != 0;
}

Color text_style_color(const Theme* theme, const Color& base, u32 flags,
                       const Color* inline_role_color) noexcept {
    if (!theme) {
        return base;
    }
    // Role colours are server data: no theme remaps them.
    if ((flags & text_style_inline_role_color) != 0 && inline_role_color &&
        inline_role_color->alpha > 0.0f) {
        return *inline_role_color;
    }
    if ((flags & text_style_syntax_comment) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.comment);
    }
    if ((flags & text_style_syntax_string) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.string);
    }
    if ((flags & text_style_syntax_number) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.number);
    }
    if ((flags & text_style_syntax_keyword) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.keyword);
    }
    if ((flags & text_style_syntax_type) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.type);
    }
    if ((flags & text_style_syntax_builtin) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.builtin);
    }
    if ((flags & text_style_syntax_preprocessor) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.preprocessor);
    }
    if ((flags & text_style_syntax_operator) != 0) {
        return ui_foreground_color(*theme, theme->tokens.syntax.operator_);
    }
    if ((flags & text_style_spoiler) != 0) {
        return theme->interactive;
    }
    if ((flags & text_style_mention) != 0) {
        return ui_foreground_color(*theme, theme->tokens.mention.text);
    }
    if ((flags & text_style_link) != 0) {
        return ui_foreground_color(*theme, theme->tokens.text.link);
    }
    if ((flags & text_style_subtext) != 0) {
        return ui_background_muted_text_color(*theme);
    }
    return base;
}

Color text_decoration_color(const Theme* theme, const Color& base,
                            const TextDecoration& decoration) noexcept {
    if (!theme) {
        return base;
    }
    if (decoration.kind == TextDecorationKind::quote_bar) {
        return ui_background_muted_text_color(*theme);
    }
    // ```ansi lines take their decorating tag's colour, faded like the glyphs.
    if (const u16 line = ansi_decoration_line_color(decoration)) {
        const Color color = ansi_palette_color(*theme, static_cast<u8>(line - 1u));
        return Color{color.red, color.green, color.blue, base.alpha};
    }
    if (decoration.kind != TextDecorationKind::background) {
        return text_style_color(theme, base, decoration.style_flags, nullptr);
    }
    if (const u16 background = ansi_decoration_background(decoration)) {
        return ansi_palette_color(*theme, static_cast<u8>(background - 1u));
    }
    if ((decoration.style_flags & text_style_spoiler) != 0) {
        return theme->interactive;
    }
    if ((decoration.style_flags & text_style_mention) != 0) {
        return theme->tokens.mention.inline_background;
    }
    return theme->surface_high;
}

bool resolve_role_color(const D3D11GlyphTextAppendRequest& request, u64 role_id, Color* color) noexcept {
    return request.resolve_role_color && role_id != 0 &&
           request.resolve_role_color(request.role_context, request.message_id, role_id, color);
}

bool role_color_for_run(const D3D11GlyphTextAppendRequest& request, const TextRun& run,
                        Color* color) noexcept {
    if (!color || (run.style_flags & text_style_mention) == 0) {
        return false;
    }
    const u32 run_end = run.text_position + run.text_length;
    for (u32 index = 0; index < request.semantics.count; ++index) {
        const MessageMarkdownSemantic& semantic = request.semantics[index];
        if (semantic.kind != MessageMarkdownSemanticKind::role_mention || semantic.byte_count == 0) {
            continue;
        }
        u32 first = 0;
        u32 last = 0;
        if (!utf8_utf16_position_for_byte_offset(request.source, semantic.byte_offset, &first) ||
            !utf8_utf16_position_for_byte_offset(request.source, semantic.byte_offset + semantic.byte_count,
                                                 &last) ||
            first >= run_end || last <= run.text_position) {
            continue;
        }
        return resolve_role_color(request, semantic.target, color);
    }
    return false;
}

bool role_color_for_decoration(const D3D11GlyphTextAppendRequest& request, const TextDecoration& decoration,
                               Color* color) noexcept {
    if (!color || !request.text || (decoration.style_flags & text_style_mention) == 0) {
        return false;
    }
    for (u32 index = 0; index < request.text->hit_region_count; ++index) {
        const TextHitRegion& region = request.text->hit_regions[index];
        if (region.semantic_index < request.semantic_base ||
            region.semantic_index - request.semantic_base >= request.semantics.count ||
            region.right <= decoration.left || region.left >= decoration.right ||
            region.bottom <= decoration.top || region.top >= decoration.bottom) {
            continue;
        }
        const MessageMarkdownSemantic& semantic =
            request.semantics[region.semantic_index - request.semantic_base];
        if (semantic.kind == MessageMarkdownSemanticKind::role_mention &&
            resolve_role_color(request, semantic.target, color)) {
            return true;
        }
    }
    return false;
}

ColorGlyphAppendResult append_color_glyph_run(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                                              const D3D11GlyphTextAppendRequest& request,
                                              const TextRun& run, const Color& foreground) noexcept {
    if (!owner->color) {
        owner->color = static_cast<D3D11ColorGlyphOwner*>(
            VirtualAlloc(nullptr, sizeof(D3D11ColorGlyphOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner->color)
            return ColorGlyphAppendResult::failed;
    }
    auto& color = *owner->color;
    // UI opacity is applied once by the draw shader, including palette colors.
    const ColrColor text_color{foreground.red, foreground.green, foreground.blue, 1};
    const u16 face_id = retain_run_font(owner, *request.text, run);
    if (!face_id)
        return ColorGlyphAppendResult::failed;
    const float pixel_scale = request.dpi / 96.f, pixels_per_em = run.em_size * pixel_scale;
    const u32 em_fixed = static_cast<u32>(run.em_size * 64.f + .5f);
    float advance = 0;
    for (u32 i = 0; i < run.glyph_count; ++i) {
        const auto& glyph = request.text->glyphs[run.first_glyph + i];
        u64 key = (1ull << 63) | (static_cast<u64>(face_id) << 48) | (static_cast<u64>(request.dpi) << 32) |
                  (static_cast<u64>(em_fixed) << 16) | glyph.index;
        auto allocation = glyph_atlas_find_indexed(&owner->allocator, atlas_index(owner), key);
        if (!allocation.ok()) {
            const u32 identity = (static_cast<u32>(face_id) << 16) | glyph.index;
            auto& hint = color.foreground_hints[(glyph.index ^ (face_id * 31u)) & 255u];
            const u64 base_key = key;
            if ((hint & 0x7fffffffu) == identity && (hint & 0x80000000u)) {
                key = hash64_bytes(base_key, &text_color, sizeof(text_color)) | (1ull << 63);
                allocation = glyph_atlas_find_indexed(&owner->allocator, atlas_index(owner), key);
            }
            if (!allocation.ok()) {
                bool uses_foreground = false;
                if (!native_color_glyph_build(
                        &owner->native_outlines, face_id, glyph.index, text_color, pixels_per_em,
                        &color.program, &uses_foreground, owner->system.native_fonts, run.native_face))
                    return ColorGlyphAppendResult::failed;
                hint = identity | (uses_foreground ? 0x80000000u : 0);
                if (uses_foreground) {
                    key = hash64_bytes(base_key, &text_color, sizeof(text_color)) | (1ull << 63);
                    allocation = glyph_atlas_find_indexed(&owner->allocator, atlas_index(owner), key);
                }
            }
        }
        GlyphRasterBounds bounds{};
        if (allocation.ok()) {
            const auto origin = owner->atlas_origins[allocation.handle.slot];
            bounds = {origin.x,
                      origin.y,
                      origin.x + allocation.width,
                      origin.y + allocation.height,
                      allocation.width,
                      allocation.height};
        } else {
            if (!color.program.bounds.has_bounds) {
                advance += glyph.advance;
                continue;
            }
            const auto& extent = color.program.bounds;
            if (!glyph_raster_bounds(extent.minimum_x, extent.minimum_y, extent.maximum_x, extent.maximum_y,
                                     pixels_per_em, 1, &bounds))
                return ColorGlyphAppendResult::failed;
            allocation =
                glyph_atlas_allocate_indexed(&owner->allocator, atlas_index(owner), key, bounds.width, bounds.height);
            if (!allocation.ok())
                return ColorGlyphAppendResult::failed;
            if (allocation.evicted_entries || allocation.atlas_reset) {
                *request.atlas_invalidated = true;
                return ColorGlyphAppendResult::failed;
            }
            if (!colr_paint_rasterize(&color.gpu, context, color.program, bounds, allocation, pixels_per_em,
                                      owner->gpu.width, owner->allocator.padding, owner->color_batch)) {
                log_u64(LogLevel::error, "render", "colrv1_raster_failed", glyph.index);
                glyph_atlas_reset(&owner->allocator);
                return ColorGlyphAppendResult::failed;
            }
            owner->atlas_origins[allocation.handle.slot] = {bounds.left, bounds.top};
            ++owner->cache_builds;
        }
        if (*request.count >= request.capacity)
            return ColorGlyphAppendResult::failed;
        // Odd bidi levels carry a right-edge origin. Cached outlines/paint
        // programs use the ordinary left glyph origin, so consume this glyph's
        // advance before applying its directional offset.
        const bool rtl = (run.bidi_level & 1) != 0;
        const float x =
            request.origin_x + run.baseline_x +
            (rtl ? -(advance + glyph.advance + glyph.advance_offset) : advance + glyph.advance_offset);
        const float y = request.origin_y + run.baseline_y - glyph.ascender_offset;
        auto& q = request.output[(*request.count)++];
        q = {x + bounds.left / pixel_scale,
             y + bounds.top / pixel_scale,
             x + bounds.right / pixel_scale,
             y + bounds.bottom / pixel_scale,
             2.f + static_cast<float>(allocation.x) / owner->gpu.width,
             static_cast<float>(allocation.y) / owner->gpu.height,
             2.f + static_cast<float>(allocation.x + allocation.width) / owner->gpu.width,
             static_cast<float>(allocation.y + allocation.height) / owner->gpu.height,
             {1, 1, 1, foreground.alpha}};
        advance += glyph.advance;
    }
    return ColorGlyphAppendResult::appended;
}

// Empty hit-test rectangles are valid after trimming. Keep their validation off
// the visible-quad path; integer bits preserve NaN checks under /fp:fast.
__declspec(noinline) bool finite_decoration_bounds(const TextDecoration& decoration) noexcept {
    const float bounds[]{decoration.left, decoration.top, decoration.right, decoration.bottom};
    for (float bound : bounds) {
        u32 bits = 0;
        copy_bytes(&bits, &bound, sizeof(bits));
        if ((bits & 0x7f800000u) == 0x7f800000u)
            return false;
    }
    return true;
}

} // namespace

// The area test must preserve unordered comparisons before classifying empty
// geometry. /fp:fast otherwise permits NaN bounds to bypass the cold check.
#pragma float_control(precise, on, push)
bool d3d11_glyph_text_append_solid(const TextDecoration& decoration, float origin_x, float origin_y,
                                   const Color& color, GlyphQuad* output, u32 capacity,
                                   u32* count) noexcept {
    if (!output || !count || *count > capacity)
        return false;
    if (!(decoration.right > decoration.left) || !(decoration.bottom > decoration.top)) {
        return finite_decoration_bounds(decoration);
    }
    if (*count == capacity)
        return false;
    const bool circle = decoration.kind == TextDecorationKind::list_bullet;
    output[(*count)++] =
        GlyphQuad{origin_x + decoration.left,   origin_y + decoration.top, origin_x + decoration.right,
                  origin_y + decoration.bottom, circle ? -3.0f : -1.0f,    circle ? 0.0f : -1.0f,
                  circle ? -2.0f : -1.0f,       circle ? 1.0f : -1.0f,     color};
    return true;
}

#pragma float_control(pop)

// Caller-owned glyph buffers lend their reserved storage to effect preparation.
// No geometry or cluster-map field changes, and no submitted quad keeps a tag.
bool d3d11_glyph_text_append(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                             const D3D11GlyphTextAppendRequest& request) noexcept {
    if (!owner || !context || !request.text || !request.output || !request.count ||
        !request.atlas_invalidated || *request.count > request.capacity) {
        return false;
    }
    const u32 first_quad = *request.count;
    bool effect_glyphs = false;
    const float pixel_scale = static_cast<float>(request.dpi) / 96.0f;
    if (!(pixel_scale > 0.0f)) {
        return false;
    }
    GlyphOutline outline{};
    if (!glyph_outline_initialize(&outline,
                                  Slice<GlyphPathCommand>{owner->path_commands, kRendererPathCapacity})) {
        return false;
    }
    GlyphRasterBatch raster(&owner->gpu, context);
    struct PendingRaster {
        GlyphRasterBatch& batch;
        D3D11GlyphTextOwner& owner;
        bool& invalidated;
        ~PendingRaster() noexcept {
            if (batch.pending()) {
                batch.cancel();
                glyph_atlas_reset(&owner.allocator);
                invalidated = true;
            }
        }
    } pending{raster, *owner, *request.atlas_invalidated};
    for (u32 index = 0; index < request.text->decoration_count; ++index) {
        const TextDecoration& decoration = request.text->decorations[index];
        if (decoration.kind != TextDecorationKind::background &&
            decoration.kind != TextDecorationKind::quote_bar &&
            decoration.kind != TextDecorationKind::list_bullet) {
            continue;
        }
        Color decoration_color = text_decoration_color(request.theme, request.color, decoration);
        Color role_color{};
        if (role_color_for_decoration(request, decoration, &role_color)) {
            role_color.alpha = 0.18f;
            decoration_color = role_color;
        }
        if (!d3d11_glyph_text_append_solid(decoration, request.origin_x, request.origin_y, decoration_color,
                                           request.output, request.capacity, request.count)) {
            return false;
        }
    }
    if (request.selection_active) {
        Color selected = request.theme ? request.theme->accent : Color{.3451f, .3961f, .9490f, 1};
        selected.alpha = .38f;
        for (u32 i = 0; i < request.text->hit_region_count; ++i) {
            const auto& region = request.text->hit_regions[i];
            if (region.semantic_index != request.selection_semantic_index || region.right <= region.left ||
                region.bottom <= region.top)
                continue;
            const TextDecoration highlight{region.left,   region.top, region.right,
                                           region.bottom, 0,          TextDecorationKind::background};
            if (!d3d11_glyph_text_append_solid(highlight, request.origin_x, request.origin_y, selected,
                                               request.output, request.capacity, request.count))
                return false;
        }
    }
    // The official client draws ansi colours as authored in every theme: no readability
    // adjustment, which would erase a light theme's white-on-black runs.
    const bool ansi_glyphs = request.theme && !request.cluster_effect &&
                             ansi_text_tag_glyphs(*request.text, request.source, request.styles);
    u16 ansi_code = kAnsiColorNone;
    Color ansi_color{};
    for (u32 run_index = 0; run_index < request.text->run_count; ++run_index) {
        const TextRun& run = request.text->runs[run_index];
        if ((run.style_flags & text_style_custom_emoji) != 0) {
            continue;
        }
        const u16 face_id = retain_run_font(owner, *request.text, run);
        if (face_id == 0 || !(run.em_size > 0.0f) || run.em_size > 256.0f) {
            return false;
        }
        const u32 em_fixed = static_cast<u32>(run.em_size * 64.0f + 0.5f);
        if (em_fixed == 0 || em_fixed > 0xFFFFu || request.dpi > 0xFFFFu) {
            return false;
        }
        Color foreground =
            text_style_color(request.theme, request.color, run.style_flags, request.inline_role_color);
        Color role_color{};
        if (role_color_for_run(request, run, &role_color)) {
            foreground = role_color;
        }
        if (retained_font_face_is_color(owner, face_id)) {
            if (!raster.finish())
                return false;
            const ColorGlyphAppendResult colored =
                append_color_glyph_run(owner, context, request, run, foreground);
            if (colored == ColorGlyphAppendResult::failed) {
                return false;
            }
            if (colored == ColorGlyphAppendResult::appended) {
                continue;
            }
        }
        const float pixels_per_em = run.em_size * pixel_scale;
        float advance = 0.0f;
        for (u32 offset = 0; offset < run.glyph_count; ++offset) {
            const u32 glyph_index = run.first_glyph + offset;
            if (glyph_index >= request.text->glyph_count) {
                return false;
            }
            const TextGlyph& glyph = request.text->glyphs[glyph_index];
            auto& face_cache = owner->font_faces[face_id - 1u];
            if (face_cache.empty_glyph_plus_one == static_cast<u32>(glyph.index) + 1u) {
                advance += glyph.advance;
                continue;
            }
            const u64 key = (static_cast<u64>(face_id) << 48) | (static_cast<u64>(request.dpi) << 32) |
                            (static_cast<u64>(em_fixed) << 16) | glyph.index;
            auto allocation = glyph_atlas_find_indexed(&owner->allocator, atlas_index(owner), key);
            GlyphRasterBounds bounds{};
            if (allocation.ok()) {
                const auto origin = owner->atlas_origins[allocation.handle.slot];
                bounds = {origin.x,
                          origin.y,
                          origin.x + allocation.width,
                          origin.y + allocation.height,
                          allocation.width,
                          allocation.height};
            } else {
                // Origin and dimensions are needed on a hit; the outline is only
                // needed to populate a missing atlas tile. Origins share the same
                // slots for monochrome and color glyphs and are rewritten on reuse.
                glyph_outline_reset(&outline);
                if (!native_glyph_outline(&owner->native_outlines, face_id, glyph.index, &outline,
                                          owner->system.native_fonts, run.native_face))
                    return false;
                if (!outline.has_bounds || outline.count == 0) {
                    face_cache.empty_glyph_plus_one = static_cast<u32>(glyph.index) + 1u;
                    advance += glyph.advance;
                    continue;
                }
                if (!glyph_raster_bounds(outline, pixels_per_em, 1, &bounds)) {
                    return false;
                }
                allocation =
                    glyph_atlas_allocate_unique_indexed(&owner->allocator, atlas_index(owner), key, bounds.width,
                                                        bounds.height);
                if (!allocation.ok()) {
                    log_u64(LogLevel::error, "render", "glyph_allocation_error",
                            static_cast<u64>(allocation.error));
                    return false;
                }
                if (allocation.evicted_entries != 0 || allocation.atlas_reset) {
                    *request.atlas_invalidated = true;
                    return false;
                }
                if (!allocation.cached) {
                    if (owner->color_batch) {
                        if (!owner->color_batch->finish())
                            return false;
                        owner->color_batch->reset();
                    }
                    const GlyphAtlasGpuResult rasterized =
                        raster.add(outline, bounds, allocation, pixels_per_em, owner->allocator.padding);
                    if (!rasterized.ok()) {
                        raster.cancel();
                        glyph_atlas_reset(&owner->allocator);
                        *request.atlas_invalidated = true;
                        return false;
                    }
                    ++owner->cache_builds;
                }
                owner->atlas_origins[allocation.handle.slot] = {bounds.left, bounds.top};
            }
            if (*request.count >= request.capacity) {
                return false;
            }
            const float inverse_scale = 1.0f / pixel_scale;
            const float baseline_x_unaligned =
                (run.bidi_level & 1)
                    ? request.origin_x + run.baseline_x - (advance + glyph.advance + glyph.advance_offset)
                    : request.origin_x + run.baseline_x + advance + glyph.advance_offset;
            const float baseline_x =
                static_cast<float>(static_cast<i32>(baseline_x_unaligned * pixel_scale + 0.5f)) *
                inverse_scale;
            const float baseline_y = static_cast<float>(static_cast<i32>(
                                         (request.origin_y + run.baseline_y) * pixel_scale + 0.5f)) *
                                         inverse_scale -
                                     glyph.ascender_offset;
            GlyphQuad& quad = request.output[(*request.count)++];
            quad.left = baseline_x + static_cast<float>(bounds.left) * inverse_scale;
            quad.top = baseline_y + static_cast<float>(bounds.top) * inverse_scale;
            quad.right = baseline_x + static_cast<float>(bounds.right) * inverse_scale;
            quad.bottom = baseline_y + static_cast<float>(bounds.bottom) * inverse_scale;
            quad.u0 = static_cast<float>(allocation.x) / owner->gpu.width;
            quad.v0 = static_cast<float>(allocation.y) / owner->gpu.height;
            quad.u1 = static_cast<float>(allocation.x + allocation.width) / owner->gpu.width;
            quad.v1 = static_cast<float>(allocation.y + allocation.height) / owner->gpu.height;
            quad.color = foreground;
            if (ansi_glyphs && glyph.reserved != kAnsiColorNone) {
                if (glyph.reserved != ansi_code) {
                    ansi_code = glyph.reserved;
                    ansi_color = ansi_palette_color(*request.theme, static_cast<u8>(ansi_code - 1u));
                }
                quad.color = Color{ansi_color.red, ansi_color.green, ansi_color.blue, foreground.alpha};
            }
            // Filled with the final ordinal after all glyphs/fonts are ready.
            if (request.cluster_effect) {
                copy_bytes(&quad.color.green, &glyph_index, sizeof(glyph_index));
                effect_glyphs = true;
            }
            advance += glyph.advance;
        }
    }
    if (!raster.finish())
        return false;
    if (effect_glyphs) {
        struct EffectContext {
            D3D11GlyphTextOwner* owner;
            const ShapedText* text;
        } effect{owner, request.text};
        const auto colored = +[](void* opaque, u32 glyph) noexcept {
            auto& state = *static_cast<EffectContext*>(opaque);
            for (u32 i = 0; i < state.text->run_count; ++i) {
                const auto& run = state.text->runs[i];
                if (glyph >= run.first_glyph && glyph - run.first_glyph < run.glyph_count)
                    return retained_font_face_is_color(state.owner,
                                                       retain_run_font(state.owner, *state.text, run));
            }
            return false;
        };
        const bool prepared = glyph_effect_prepare(*request.text, request.source, colored, &effect);
        for (u32 i = first_quad; i < *request.count; ++i) {
            auto& quad = request.output[i];
            // Decorations have negative u; color atlas tiles start at u=2.
            if (quad.u0 < 0 || quad.u0 >= 2)
                continue;
            u32 glyph = 0;
            copy_bytes(&glyph, &quad.color.green, sizeof(glyph));
            quad.color.green = static_cast<float>(
                prepared ? request.text->glyphs[glyph].reserved
                         : glyph_effect_ordinal(*request.text, request.source, glyph, colored, &effect));
        }
    }
    for (u32 index = 0; index < request.text->decoration_count; ++index) {
        const TextDecoration& decoration = request.text->decorations[index];
        if (decoration.kind != TextDecorationKind::underline &&
            decoration.kind != TextDecorationKind::strikethrough) {
            continue;
        }
        Color decoration_color = text_decoration_color(request.theme, request.color, decoration);
        Color role_color{};
        if (role_color_for_decoration(request, decoration, &role_color)) {
            decoration_color = role_color;
        }
        if (!d3d11_glyph_text_append_solid(decoration, request.origin_x, request.origin_y, decoration_color,
                                           request.output, request.capacity, request.count)) {
            return false;
        }
    }
    return true;
}

bool d3d11_glyph_text_rebuild_title(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context, u32 dpi,
                                    bool reset_atlas) noexcept {
    if (!owner || !context || !owner->gpu.texture || !owner->title.glyphs) {
        return false;
    }
    // Login/content normally replaces the bootstrap title before the first draw.
    // Keep its shape and atlas entries absent until a caller actually needs them.
    if (owner->title.run_count == 0 &&
        !text_shape_utf8(&owner->system, utf8_literal("MaiCord - GPU text"), 600.0f, 64.0f,
                         Slice<wchar_t>{owner->utf16, kRendererGlyphCapacity}, &owner->title)
             .ok()) {
        return false;
    }
    if (reset_atlas) {
        glyph_atlas_reset(&owner->allocator);
    }
    owner->title_quad_count = 0;
    bool invalidated = false;
    const D3D11GlyphTextAppendRequest request{
        &owner->title,
        owner->title_quads,
        &owner->title_quad_count,
        &invalidated,
        {},
        {},
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        kDefaultTheme.text_primary,
        0.0f,
        0.0f,
        0,
        0,
        kRendererGlyphCapacity,
        dpi,
    };
    return d3d11_glyph_text_append(owner, context, request) && !invalidated && owner->title_quad_count != 0;
}

D3D11GlyphTextInitializeResult d3d11_glyph_text_initialize(D3D11GlyphTextOwner* owner, ID3D11Device* device,
                                                           ID3D11DeviceContext* context, u32 dpi) noexcept {
    if (!owner || !device || !context || dpi == 0 || owner->storage.base || owner->system.native_text ||
        owner->gpu.texture || owner->color_glyphs) {
        return initialize_result(D3D11GlyphTextInitializeError::invalid_input, E_INVALIDARG);
    }
    if (!initialize_storage(owner)) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::shaped_storage, E_OUTOFMEMORY);
    }
    const TextShapeResult system = text_system_initialize(&owner->system, L"Segoe UI", 16.0f, L"en-us");
    if (!system.ok()) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::text_system, system.native_error,
                                 system.error);
    }
    if (!shaped_text_initialize(&owner->title, Slice<TextGlyph>{owner->glyphs, kRendererGlyphCapacity},
                                Slice<TextRun>{owner->runs, kRendererTextRunCapacity},
                                Slice<u16>{owner->clusters, kRendererGlyphCapacity})) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::shaped_storage, E_INVALIDARG);
    }
    if (!glyph_atlas_initialize(&owner->allocator, kRendererGlyphAtlasExtent, kRendererGlyphAtlasExtent, 1,
                                Slice<GlyphAtlasEntry>{owner->atlas_entries, kRendererAtlasEntryCapacity},
                                Slice<GlyphAtlasRow>{owner->atlas_rows, kRendererAtlasRowCapacity}) ||
        !glyph_atlas_index_initialize(atlas_index(owner), Slice<u16>{text_storage(owner)->atlas_index_buckets,
                                                                     kRendererAtlasEntryCapacity * 2})) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::atlas_storage, E_INVALIDARG);
    }
    const GlyphAtlasGpuResult gpu = glyph_atlas_gpu_initialize(
        &owner->gpu, device, kRendererGlyphAtlasExtent, kRendererGlyphAtlasExtent, kRendererPathCapacity);
    if (!gpu.ok()) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::atlas_gpu, gpu.native_error,
                                 TextShapeError::none, gpu.error);
    }
    owner->color_glyphs = true;
    owner->ready = true;
    return initialize_result(D3D11GlyphTextInitializeError::none, S_OK, TextShapeError::none,
                             GlyphAtlasGpuError::none, owner->color_glyphs);
}

void d3d11_glyph_text_reset_atlas(D3D11GlyphTextOwner* owner) noexcept {
    if (owner) {
        if (owner->color_batch)
            owner->color_batch->cancel();
        glyph_atlas_reset(&owner->allocator);
    }
}

void d3d11_glyph_text_release(D3D11GlyphTextOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    if (owner->color_batch)
        owner->color_batch->cancel();
    owner->color_batch = nullptr;
    native_glyph_outlines_release(&owner->native_outlines);
    if (owner->color) {
        colr_paint_gpu_release(&owner->color->gpu);
        arena_release(&owner->color->program.storage);
        VirtualFree(owner->color, 0, MEM_RELEASE);
        owner->color = nullptr;
    }
    glyph_atlas_gpu_release(&owner->gpu);
    owner->gpu.dispatched_pixels = 0;
    owner->gpu.dispatched_commands = 0;
    owner->gpu.dispatch_count = 0;
    owner->gpu.raster_count = 0;
    owner->gpu.rejected = 0;
    shaped_text_reset(&owner->title);
    for (u32 index = 0; owner->font_faces && index < owner->font_face_count; ++index) {
        native_font_run_release(native_font_session_runs(owner->system.native_fonts),
                                owner->font_faces[index].native_face);
        owner->font_faces[index] = D3D11GlyphTextFontFace{};
    }
    text_system_release(&owner->system);
    arena_release(&owner->storage);
    *owner = D3D11GlyphTextOwner{};
}

} // namespace maicord

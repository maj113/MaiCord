#include "../text/glyph_effect_ordinals.hpp"
#include "maicord/core/memory.hpp"
#include "../text/d3d11_color_glyph_owner.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/ui/text/ansi_text.hpp"
#include "maicord/render/effects/colr_paint.hpp"
#include "maicord/media/image/regional_flag_svg.hpp"
#include "maicord/render/text/native_glyph_outlines.hpp"
#include "maicord/text/fonts/run.hpp"

#include "maicord/sys/d3d11.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"

namespace maicord {
ID3D11ShaderResourceView* d3d11_glyph_text_color_view(const D3D11GlyphTextOwner& owner) noexcept {
    return owner.color ? owner.color->gpu.view : nullptr;
}
namespace {
D3D11ColorGlyphOwner* color_owner(D3D11GlyphTextOwner* owner) noexcept {
    if (!owner->color)
        owner->color = static_cast<D3D11ColorGlyphOwner*>(
            VirtualAlloc(nullptr, sizeof(D3D11ColorGlyphOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return owner->color;
}

// Worker-compiled programs borrow account-module storage only for this call.
bool rasterize_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                       const ColrPaintProgram& program, const GlyphAtlasAllocation& allocation,
                       u16 extent, float pixels_per_unit, u16 target_width, u16 target_height,
                       u16 padding, ID3D11UnorderedAccessView* target) noexcept {
    if (!owner || !owner->gpu.width || !context || !target || allocation.width != extent ||
        allocation.height != extent)
        return false;
    if (owner->color_batch)
        owner->color_batch->reset();
    if (!color_owner(owner))
        return false;
    const GlyphRasterBounds bounds{0, 0, extent, extent, extent, extent};
    return colr_paint_rasterize_target(&owner->color->gpu, context, program, bounds, allocation,
                                       pixels_per_unit, target_width, target_height,
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
// The frame was compiled at its tile's extent (lottie_raster_pixels).
bool d3d11_glyph_text_rasterize_lottie_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
    const ColrPaintProgram& program,const GlyphAtlasAllocation& allocation,u16 target_width,
    u16 target_height,u16 padding,ID3D11UnorderedAccessView* target) noexcept {
    return rasterize_program(owner, context, program, allocation, static_cast<u16>(allocation.width),
                             static_cast<float>(allocation.width), target_width, target_height, padding, target);
}
namespace {
constexpr u16 color_face_flag = 2;

// The nearest whole device pixel, also for the negative origins of text
// scrolled above its clip.
float whole_pixels(float pixels) noexcept {
    const float shifted = pixels + .5f;
    const i32 truncated = static_cast<i32>(shifted);
    return static_cast<float>(static_cast<float>(truncated) > shifted ? truncated - 1 : truncated);
}

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
    GlyphAtlasGrowth growth;
    bool geometry_stale;
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

inline GlyphAtlasGrowth& atlas_growth(D3D11GlyphTextOwner* owner) noexcept {
    return text_storage(owner)->growth;
}

// u follows the fixed atlas width, v the height that grows by steps: a quad
// built before a resize is stale after it (growth bumps full_resets).
struct AtlasTexel {
    float u, v;
};
inline AtlasTexel atlas_texel(const D3D11GlyphTextOwner& owner) noexcept {
    return {1.0f / owner.allocator.width, 1.0f / owner.allocator.height};
}

enum class AtlasGrowth : u8 { none, grown, failed };

// Recreates the mono and COLR atlas textures `height` rows tall, applying
// trim's row moves from the old ones: both change or neither.
bool resize_textures(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context, u16 height,
                     ConstSlice<GlyphAtlasRowMove> moves) noexcept {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    auto& mono = owner->gpu;
    const u16 before = mono.height;
    bool ok = device && SUCCEEDED(glyph_atlas_texture_resize(device, context, &mono.texture, &mono.texture_view,
                                                             &mono.texture_uav, height, moves));
    if (ok) {
        mono.height = height;
        auto* color = owner->color ? &owner->color->gpu : nullptr;
        if (color && color->texture) {
            ok = SUCCEEDED(glyph_atlas_texture_resize(device, context, &color->texture, &color->view, &color->uav,
                                                      height, moves));
            if (ok) {
                color->height = height;
            } else {
                glyph_atlas_texture_resize(device, context, &mono.texture, &mono.texture_view, &mono.texture_uav,
                                           before, {});
                mono.height = before;
            }
        }
    }
    com_release(device);
    return ok;
}

// Makes room for one glyph before it is allocated: when it would only fit by
// evicting glyphs used within kRendererGlyphProtectMs, the atlas grows by a
// step (glyph_atlas_growth_prepare) and both textures are recreated taller,
// keeping their rows. Queued COLR jobs and `raster` are dispatched into the
// old textures first. A grown atlas invalidates every retained quad.
AtlasGrowth grow_atlas(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context, u16 width, u16 height,
                       GlyphRasterBatch* raster) noexcept {
    auto& growth = atlas_growth(owner);
    const u16 before = owner->allocator.height;
    if (!glyph_atlas_growth_prepare(&growth, &owner->allocator, width, height))
        return AtlasGrowth::none;
    const auto revert = [&]() noexcept {
        owner->allocator.height = before;
        --growth.growths;
        return AtlasGrowth::failed;
    };
    if (raster && !raster->finish())
        return revert();
    if (owner->color_batch)
        owner->color_batch->reset();
    if (!resize_textures(owner, context, owner->allocator.height, {}))
        return revert();
    ++owner->allocator.full_resets;
    return AtlasGrowth::grown;
}

// Allocation of a glyph the atlas does not hold yet that, below the maximum height,
// leaves recently used rows alone (grow_atlas already grew when nothing stale
// could hold the request); the
// plain LRU result is the fallback when bookkeeping limits stop the protected one.
GlyphAtlasAllocation allocate_protected(D3D11GlyphTextOwner* owner, u64 key, u16 width, u16 height) noexcept {
    auto& growth = atlas_growth(owner);
    auto& allocator = owner->allocator;
    const bool protect = growth.maximum_height != 0 && allocator.height < growth.maximum_height;
    allocator.protect_after = protect ? glyph_atlas_growth_protect_after(growth) : 0;
    GlyphAtlasAllocation allocation;
    for (bool retry = protect;; retry = false) {
        allocation = glyph_atlas_allocate_unique_indexed(&allocator, atlas_index(owner), key, width, height);
        allocator.protect_after = 0;
        if (!retry || allocation.ok() || allocation.error != GlyphAtlasError::no_capacity)
            break;
    }
    glyph_atlas_growth_note_activity(&growth);
    return allocation;
}

bool initialize_storage(D3D11GlyphTextOwner* owner) noexcept {
    if (!arena_initialize(&owner->storage,
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

D3D11GlyphTextInitializeResult initialize_result(D3D11GlyphTextInitializeError error,
                                                 HRESULT native_error = S_OK) noexcept {
    return D3D11GlyphTextInitializeResult{native_error, error};
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
    reset_value(&face);
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
    return face_id != 0 && face_id <= owner->font_face_count &&
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
    // A hovered mention or link chip is an accent pill with on-accent text; role
    // mentions keep their colour.
    constexpr u32 hovered_mention = text_style_mention | text_style_interactive_hover;
    if ((flags & hovered_mention) == hovered_mention) {
        return theme->tokens.emphasis.text;
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
        return (decoration.style_flags & text_style_interactive_hover) ? theme->tokens.emphasis.fill
                                                                       : theme->tokens.mention.inline_background;
    }
    return theme->surface_high;
}

bool resolve_role_color(const D3D11GlyphTextAppendRequest& request, const MessageMarkdownSemantic& semantic,
                        Color* color) noexcept {
    return request.resolve_role_color && semantic.target != 0 &&
           request.resolve_role_color(request.role_context, request.message_id, semantic.kind, semantic.target,
                                      color);
}

bool role_color_for_run(const D3D11GlyphTextAppendRequest& request, const TextRun& run,
                        Color* color) noexcept {
    if ((run.style_flags & text_style_mention) == 0) {
        return false;
    }
    const u32 run_end = run.text_position + run.text_length;
    for (u32 index = 0; index < request.semantics.count; ++index) {
        const MessageMarkdownSemantic& semantic = request.semantics[index];
        if ((semantic.kind != MessageMarkdownSemanticKind::role_mention &&
             semantic.kind != MessageMarkdownSemanticKind::user_mention) ||
            semantic.byte_count == 0) {
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
        return resolve_role_color(request, semantic, color);
    }
    return false;
}

bool role_color_for_decoration(const D3D11GlyphTextAppendRequest& request, const TextDecoration& decoration,
                               Color* color) noexcept {
    if ((decoration.style_flags & text_style_mention) == 0) {
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
        if ((semantic.kind == MessageMarkdownSemanticKind::role_mention ||
             semantic.kind == MessageMarkdownSemanticKind::user_mention) &&
            resolve_role_color(request, semantic, color)) {
            return true;
        }
    }
    return false;
}

// `origin_x`/`origin_y` are the block's origin on whole device pixels.
bool append_color_glyph_run(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                            const D3D11GlyphTextAppendRequest& request, const TextRun& run,
                            const Color& foreground, float origin_x, float origin_y) noexcept {
    if (!color_owner(owner))
        return false;
    auto& color = *owner->color;
    // UI opacity is applied once by the draw shader, including palette colors.
    const ColrColor text_color{foreground.red, foreground.green, foreground.blue, 1};
    const u16 face_id = retain_run_font(owner, *request.text, run);
    if (!face_id)
        return false;
    const float pixel_scale = request.dpi / 96.f, pixels_per_em = run.em_size * pixel_scale;
    const AtlasTexel texel = atlas_texel(*owner);
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
                    return false;
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
                return false;
            switch (grow_atlas(owner, context, bounds.width, bounds.height, nullptr)) {
            case AtlasGrowth::grown:
                *request.atlas_invalidated = true;
                return false;
            case AtlasGrowth::failed:
                return false;
            case AtlasGrowth::none:
                break;
            }
            allocation = allocate_protected(owner, key, bounds.width, bounds.height);
            if (!allocation.ok())
                return false;
            if (!colr_paint_rasterize(&color.gpu, context, color.program, bounds, allocation, pixels_per_em,
                                      owner->allocator.width, owner->allocator.height, owner->allocator.padding,
                                      owner->color_batch)) {
                log_u64(LogLevel::error, "render", "colrv1_raster_failed", glyph.index);
                glyph_atlas_reset(&owner->allocator);
                return false;
            }
            owner->atlas_origins[allocation.handle.slot] = {bounds.left, bounds.top};
            ++owner->cache_builds;
            if (allocation.evicted_entries || allocation.atlas_reset) {
                *request.atlas_invalidated = true;
                return false;
            }
        }
        if (*request.count >= request.capacity)
            return false;
        // Odd bidi levels carry a right-edge origin. Cached outlines/paint
        // programs use the ordinary left glyph origin, so consume this glyph's
        // advance before applying its directional offset.
        const bool rtl = (run.bidi_level & 1) != 0;
        const float x =
            origin_x + run.baseline_x +
            (rtl ? -(advance + glyph.advance + glyph.advance_offset) : advance + glyph.advance_offset);
        const float y = origin_y + run.baseline_y - glyph.ascender_offset;
        auto& q = request.output[(*request.count)++];
        q = {x + bounds.left / pixel_scale,
             y + bounds.top / pixel_scale,
             x + bounds.right / pixel_scale,
             y + bounds.bottom / pixel_scale,
             2.f + static_cast<float>(allocation.x) * texel.u,
             static_cast<float>(allocation.y) * texel.v,
             2.f + static_cast<float>(allocation.x + allocation.width) * texel.u,
             static_cast<float>(allocation.y + allocation.height) * texel.v,
             {1, 1, 1, foreground.alpha}};
        advance += glyph.advance;
    }
    return true;
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
    const AtlasTexel texel = atlas_texel(*owner);
    if (!(pixel_scale > 0.0f)) {
        return false;
    }
    // The block moves as one on whole device pixels and each glyph rounds only
    // its offset inside it. Rounding every glyph's absolute position instead
    // made the spacing inside a word depend on the block's fractional origin:
    // centred text at odd window sizes moved some letters a pixel and not
    // their neighbours.
    const float origin_x_pixels = whole_pixels(request.origin_x * pixel_scale);
    const float origin_y_pixels = whole_pixels(request.origin_y * pixel_scale);
    const float origin_x = origin_x_pixels / pixel_scale, origin_y = origin_y_pixels / pixel_scale;
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
            role_color.alpha = (decoration.style_flags & text_style_interactive_hover) ? .3f : .18f;
            decoration_color = role_color;
        }
        if (!d3d11_glyph_text_append_solid(decoration, origin_x, origin_y, decoration_color,
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
            if (!d3d11_glyph_text_append_solid(highlight, origin_x, origin_y, selected,
                                               request.output, request.capacity, request.count))
                return false;
        }
    }
    // ansi colours are drawn as authored in every theme; a readability adjustment
    // would erase a light theme's white-on-black runs.
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
            if (!append_color_glyph_run(owner, context, request, run, foreground, origin_x, origin_y))
                return false;
            continue;
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
                const AtlasGrowth growth_result = grow_atlas(owner, context, bounds.width, bounds.height, &raster);
                if (growth_result != AtlasGrowth::none) {
                    *request.atlas_invalidated = growth_result == AtlasGrowth::grown;
                    return false;
                }
                allocation = allocate_protected(owner, key, bounds.width, bounds.height);
                if (!allocation.ok()) {
                    log_u64(LogLevel::error, "render", "glyph_allocation_error",
                            static_cast<u64>(allocation.error));
                    return false;
                }
                if (!allocation.cached) {
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
                if (allocation.evicted_entries || allocation.atlas_reset) {
                    // A retry may reuse these entries, so submit every pending tile first.
                    if (!raster.finish())
                        glyph_atlas_reset(&owner->allocator);
                    *request.atlas_invalidated = true;
                    return false;
                }
            }
            if (*request.count >= request.capacity) {
                return false;
            }
            const float inverse_scale = 1.0f / pixel_scale;
            const float offset_x = (run.bidi_level & 1)
                                       ? run.baseline_x - (advance + glyph.advance + glyph.advance_offset)
                                       : run.baseline_x + advance + glyph.advance_offset;
            const float baseline_x = (origin_x_pixels + whole_pixels(offset_x * pixel_scale)) * inverse_scale;
            const float baseline_y =
                (origin_y_pixels + whole_pixels(run.baseline_y * pixel_scale)) * inverse_scale -
                glyph.ascender_offset;
            GlyphQuad& quad = request.output[(*request.count)++];
            quad.left = baseline_x + static_cast<float>(bounds.left) * inverse_scale;
            quad.top = baseline_y + static_cast<float>(bounds.top) * inverse_scale;
            quad.right = baseline_x + static_cast<float>(bounds.right) * inverse_scale;
            quad.bottom = baseline_y + static_cast<float>(bounds.bottom) * inverse_scale;
            quad.u0 = static_cast<float>(allocation.x) * texel.u;
            quad.v0 = static_cast<float>(allocation.y) * texel.v;
            quad.u1 = static_cast<float>(allocation.x + allocation.width) * texel.u;
            quad.v1 = static_cast<float>(allocation.y + allocation.height) * texel.v;
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
        // Shaper output always satisfies the preparation limits: its glyph and
        // cluster counts are bounded by the renderer's storage capacities,
        // which stay below the 16-bit ordinal range.
        if (!glyph_effect_prepare(*request.text, request.source, colored, &effect))
            return false;
        for (u32 i = first_quad; i < *request.count; ++i) {
            auto& quad = request.output[i];
            // Decorations have negative u; color atlas tiles start at u=2.
            if (quad.u0 < 0 || quad.u0 >= 2)
                continue;
            u32 glyph = 0;
            copy_bytes(&glyph, &quad.color.green, sizeof(glyph));
            quad.color.green = static_cast<float>(request.text->glyphs[glyph].reserved);
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
        if (!d3d11_glyph_text_append_solid(decoration, origin_x, origin_y, decoration_color,
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
        !text_shape_utf8(&owner->system, utf8_literal("MaiCord - GPU text"), {}, 600.0f, 64.0f,
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
                                                           ID3D11DeviceContext* context, u32 dpi,
                                                           GlyphAtlasGpu* prestarted_atlas) noexcept {
    if (!owner || !device || !context || dpi == 0 || owner->storage.base || owner->system.native_text ||
        owner->gpu.texture || owner->color_glyphs) {
        return initialize_result(D3D11GlyphTextInitializeError::invalid_input, E_INVALIDARG);
    }
    if (!initialize_storage(owner)) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::shaped_storage, E_OUTOFMEMORY);
    }
    const TextShapeResult system = text_system_initialize(&owner->system, L"Segoe UI", 16.0f);
    if (!system.ok()) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::text_system, system.native_error);
    }
    if (!shaped_text_initialize(&owner->title, Slice<TextGlyph>{owner->glyphs, kRendererGlyphCapacity},
                                Slice<TextRun>{owner->runs, kRendererTextRunCapacity},
                                Slice<u16>{owner->clusters, kRendererGlyphCapacity})) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::shaped_storage, E_INVALIDARG);
    }
    if (!glyph_atlas_initialize(&owner->allocator, kRendererGlyphAtlasExtent, kRendererGlyphAtlasMaximumHeight, 1,
                                Slice<GlyphAtlasEntry>{owner->atlas_entries, kRendererAtlasEntryCapacity},
                                Slice<GlyphAtlasRow>{owner->atlas_rows, kRendererAtlasRowCapacity}) ||
        !glyph_atlas_growth_configure(&atlas_growth(owner), &owner->allocator, kRendererGlyphAtlasExtent,
                                      kRendererGlyphAtlasExtent, kRendererGlyphProtectMs) ||
        !glyph_atlas_index_initialize(atlas_index(owner), Slice<u16>{text_storage(owner)->atlas_index_buckets,
                                                                     kRendererAtlasEntryCapacity * 2})) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::atlas_storage, E_INVALIDARG);
    }
    const bool adopt = prestarted_atlas && prestarted_atlas->texture && prestarted_atlas->texture_view &&
                       prestarted_atlas->texture_uav && prestarted_atlas->command_view &&
                       prestarted_atlas->raster_shader && prestarted_atlas->width == kRendererGlyphAtlasExtent &&
                       prestarted_atlas->height == kRendererGlyphAtlasExtent &&
                       prestarted_atlas->command_capacity == kRendererPathCapacity;
    GlyphAtlasGpuResult gpu{S_OK, GlyphAtlasGpuError::none};
    if (adopt) {
        owner->gpu = *prestarted_atlas;
        reset_value<GlyphAtlasGpu>(prestarted_atlas);
    } else {
        gpu = glyph_atlas_gpu_initialize(&owner->gpu, device, kRendererGlyphAtlasExtent, kRendererGlyphAtlasExtent,
                                         kRendererPathCapacity);
    }
    if (!gpu.ok()) {
        d3d11_glyph_text_release(owner);
        return initialize_result(D3D11GlyphTextInitializeError::atlas_gpu, gpu.native_error);
    }
    owner->color_glyphs = true;
    owner->ready = true;
    return initialize_result(D3D11GlyphTextInitializeError::none);
}

void d3d11_glyph_text_reset_atlas(D3D11GlyphTextOwner* owner) noexcept {
    if (owner) {
        if (owner->color_batch)
            owner->color_batch->reset();
        glyph_atlas_reset(&owner->allocator);
    }
}

// A lost device took the atlas textures with it: new ones at the height the allocator holds, an empty allocator
// (every glyph is rasterized again on demand) and the stale-geometry flag so retained quads are rebuilt.
bool d3d11_glyph_text_recreate_gpu(D3D11GlyphTextOwner* owner, ID3D11Device* device) noexcept {
    if (!owner || !device || !owner->atlas_origins)
        return false;
    if (owner->color_batch)
        owner->color_batch->reset();
    owner->color_batch = nullptr;
    if (owner->color)
        colr_paint_gpu_release(&owner->color->gpu);
    glyph_atlas_gpu_release(&owner->gpu);
    const u16 height = owner->allocator.height;
    owner->ready = glyph_atlas_gpu_initialize(&owner->gpu, device, kRendererGlyphAtlasExtent, height,
                                              kRendererPathCapacity)
                       .ok();
    glyph_atlas_reset(&owner->allocator);
    ++owner->allocator.full_resets;
    text_storage(owner)->geometry_stale = true;
    owner->title_quad_count = 0;
    return owner->ready;
}

u64 d3d11_glyph_text_trim_deadline(const D3D11GlyphTextOwner& owner) noexcept {
    return owner.atlas_origins
               ? glyph_atlas_growth_trim_deadline(
                     text_storage(const_cast<D3D11GlyphTextOwner*>(&owner))->growth, kRendererGlyphIdleTrimMs)
               : 0;
}

bool d3d11_glyph_text_trim(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context, u64 now_ms) noexcept {
    if (!owner || !context || !owner->atlas_origins)
        return false;
    auto& growth = atlas_growth(owner);
    glyph_atlas_growth_observe(&growth, owner->allocator, now_ms);
    const u64 deadline = glyph_atlas_growth_trim_deadline(growth, kRendererGlyphIdleTrimMs);
    if (!deadline || now_ms < deadline)
        return false;
    if (owner->color_batch)
        owner->color_batch->reset();
    GlyphAtlasRowMove moves[256];
    bool resized = false;
    const u32 evicted = owner->allocator.evicted_entries;
    const u32 moved = glyph_atlas_growth_trim(&growth, &owner->allocator, {moves, 256}, &resized);
    const u16 height = owner->allocator.height;
    if ((resized || moved) && !resize_textures(owner, context, height, {moves, moved})) {
        // The textures and the allocator disagree: start the atlas over at
        // the height the textures have.
        glyph_atlas_reset(&owner->allocator);
        owner->allocator.height = owner->gpu.height;
    }
    const bool changed = resized || moved || owner->allocator.evicted_entries != evicted;
    if (changed) {
        ++owner->allocator.full_resets; // Rows moved or the v scale changed: retained quads are stale.
        text_storage(owner)->geometry_stale = true;
    }
    return changed;
}

bool d3d11_glyph_text_take_stale(D3D11GlyphTextOwner* owner) noexcept {
    if (!owner || !owner->atlas_origins || !text_storage(owner)->geometry_stale)
        return false;
    text_storage(owner)->geometry_stale = false;
    return true;
}

void d3d11_glyph_text_release(D3D11GlyphTextOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    if (owner->color_batch)
        owner->color_batch->reset();
    owner->color_batch = nullptr;
    native_glyph_outlines_release(&owner->native_outlines);
    if (owner->color) {
        colr_paint_gpu_release(&owner->color->gpu);
        arena_release(&owner->color->program.storage);
        VirtualFree(owner->color, 0, MEM_RELEASE);
        owner->color = nullptr;
    }
    glyph_atlas_gpu_release(&owner->gpu);
    shaped_text_reset(&owner->title);
    for (u32 index = 0; owner->font_faces && index < owner->font_face_count; ++index) {
        native_font_run_release(native_font_session_runs(owner->system.native_fonts),
                                owner->font_faces[index].native_face);
        reset_value<D3D11GlyphTextFontFace>(&owner->font_faces[index]);
    }
    text_system_release(&owner->system);
    arena_release(&owner->storage);
    reset_value<D3D11GlyphTextOwner>(owner);
}

} // namespace maicord

#include "shaped_text_cache.hpp"

#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

// Word-at-a-time mixing; keys are compared exactly, so this only spreads buckets.
u64 mix_bytes(u64 hash, const void* source, u32 bytes) noexcept {
    const auto* input = static_cast<const u8*>(source);
    constexpr u64 multiplier = 0x9E3779B97F4A7C15ull;
    for (; bytes >= 8; bytes -= 8, input += 8) {
        u64 word;
        copy_bytes(&word, input, 8);
        hash = (hash ^ word) * multiplier;
        hash ^= hash >> 29;
    }
    u64 tail = bytes;
    for (u32 index = 0; index < bytes; ++index)
        tail = tail << 8 | input[index];
    hash = (hash ^ tail) * multiplier;
    return hash ^ (hash >> 32);
}

bool same_system(const TextSystem& left, const TextSystem& right) noexcept {
    return left.native_fonts == right.native_fonts &&
        left.native_text == right.native_text && left.native_format == right.native_format &&
        left.em_size == right.em_size &&
        left.tracking == right.tracking && left.fixed_weight == right.fixed_weight &&
        left.independent_letters == right.independent_letters &&
        left.message_hit_policy == right.message_hit_policy && left.typography == right.typography;
}

u64 request_hash(const ShapedTextCacheRequest& request) noexcept {
    const TextSystem& system = *request.system;
    const u64 scalars[] = {
        reinterpret_cast<usize>(system.native_text) ^ system.native_format,
        reinterpret_cast<usize>(system.typography),
        static_cast<u64>(request.text.bytes) << 32 | request.styles.count << 16 | request.hits.count,
        static_cast<u64>(request.dpi) << 32 | request.typography_revision,
        static_cast<u64>(request.wrapping) << 8 | request.overflow |
            static_cast<u64>(system.fixed_weight) << 16 | static_cast<u64>(system.independent_letters) << 17 |
            static_cast<u64>(system.message_hit_policy) << 18,
    };
    float floats[4] = {system.em_size, system.tracking, request.maximum_width, request.maximum_height};
    u64 hash = mix_bytes(0x243F6A8885A308D3ull, scalars, sizeof(scalars));
    hash = mix_bytes(hash, floats, sizeof(floats));
    hash = mix_bytes(hash, request.text.data, request.text.bytes);
    hash = mix_bytes(hash, request.styles.data, request.styles.count * sizeof(TextStyleSpan));
    return mix_bytes(hash, request.hits.data, request.hits.count * sizeof(TextHitSpan));
}

bool entry_matches(const ShapedTextGeneration& generation, const ShapedTextCacheEntry& entry,
                   const ShapedTextCacheRequest& request, u64 hash) noexcept {
    if (entry.moved || entry.hash != hash || entry.text_bytes != request.text.bytes ||
        entry.style_count != request.styles.count || entry.hit_span_count != request.hits.count ||
        entry.maximum_width != request.maximum_width || entry.maximum_height != request.maximum_height ||
        entry.dpi != request.dpi || entry.typography_revision != request.typography_revision ||
        entry.wrapping != request.wrapping || entry.overflow != request.overflow ||
        !same_system(entry.system, *request.system))
        return false;
    const u8* key = generation.key_bytes + entry.key_offset;
    const u32 style_bytes = request.styles.count * static_cast<u32>(sizeof(TextStyleSpan));
    return bytes_equal(key, request.text.data, request.text.bytes) &&
           bytes_equal(key + request.text.bytes, request.styles.data, style_bytes) &&
           bytes_equal(key + request.text.bytes + style_bytes, request.hits.data,
                       request.hits.count * static_cast<u32>(sizeof(TextHitSpan)));
}

ShapedTextCacheEntry* find_in(ShapedTextGeneration& generation, const ShapedTextCacheRequest& request,
                              u64 hash) noexcept {
    for (u32 probe = 0; probe < kShapedTextCacheBuckets; ++probe) {
        const u16 slot = generation.buckets[(hash + probe) & (kShapedTextCacheBuckets - 1)];
        if (!slot)
            return nullptr;
        ShapedTextCacheEntry& entry = generation.entries[slot - 1u];
        if (entry_matches(generation, entry, request, hash))
            return &entry;
    }
    return nullptr;
}

ShapedText view_of(ShapedTextGeneration& generation, const ShapedTextCacheEntry& entry) noexcept {
    ShapedText view{};
    view.glyphs = generation.glyphs + entry.first_glyph;
    view.runs = generation.runs + entry.first_run;
    view.decorations = generation.decorations + entry.first_decoration;
    view.hit_regions = generation.hits + entry.first_hit;
    view.glyph_capacity = view.glyph_count = entry.glyph_count;
    view.run_capacity = view.run_count = entry.run_count;
    view.decoration_capacity = view.decoration_count = entry.decoration_count;
    view.hit_region_capacity = view.hit_region_count = entry.hit_count;
    view.width = entry.width;
    view.width_including_trailing_whitespace = entry.width_including_trailing_whitespace;
    view.height = entry.height;
    view.line_count = entry.line_count;
    view.owner_flags = entry.owner_flags;
    return view;
}

void clear_generation(ShapedTextGeneration& generation) noexcept {
    ShapedText view; // Reset reads only the ownership fields assigned below.
    for (u32 index = 0; index < generation.entry_count; ++index) {
        ShapedTextCacheEntry& entry = generation.entries[index];
        if (entry.moved)
            continue;
        // Reset releases only the runs and session; no rendering view is needed.
        view.runs = generation.runs + entry.first_run;
        view.run_count = entry.run_count;
        view.owner_flags = entry.owner_flags;
        shaped_text_reset(&view);
    }
    clear_bytes(generation.buckets, sizeof(generation.buckets));
    generation.entry_count = generation.glyph_count = generation.run_count = 0;
    generation.decoration_count = generation.hit_count = generation.key_byte_count = 0;
}

bool fits(const ShapedTextGeneration& generation, u32 key_bytes, u32 glyphs, u32 runs, u32 decorations,
          u32 hits) noexcept {
    // Keep the probe table at most half full.
    return generation.entry_count < kShapedTextCacheBuckets / 2 &&
           generation.entry_count < kShapedTextCacheEntries &&
           key_bytes <= kShapedTextCacheKeyBytes - generation.key_byte_count &&
           glyphs <= kShapedTextCacheGlyphs - generation.glyph_count &&
           runs <= kShapedTextCacheRuns - generation.run_count &&
           decorations <= kShapedTextCacheDecorations - generation.decoration_count &&
           hits <= kShapedTextCacheHits - generation.hit_count;
}

// Caller checked fits() and populated the key bytes in place.
// Copies arrays and transfers ownership of owner_flags.
ShapedTextCacheEntry* append(ShapedTextGeneration& generation, const ShapedTextCacheEntry& key,
                             u32 key_count, const ShapedText& source) noexcept {
    ShapedTextCacheEntry& entry = generation.entries[generation.entry_count];
    entry = key;
    entry.moved = false;
    entry.key_offset = generation.key_byte_count;
    generation.key_byte_count += key_count;
    entry.first_glyph = static_cast<u16>(generation.glyph_count);
    entry.glyph_count = static_cast<u16>(source.glyph_count);
    copy_bytes(generation.glyphs + entry.first_glyph, source.glyphs, source.glyph_count * sizeof(TextGlyph));
    generation.glyph_count += source.glyph_count;
    entry.first_run = static_cast<u16>(generation.run_count);
    entry.run_count = static_cast<u16>(source.run_count);
    copy_bytes(generation.runs + entry.first_run, source.runs, source.run_count * sizeof(TextRun));
    generation.run_count += source.run_count;
    entry.first_decoration = static_cast<u16>(generation.decoration_count);
    entry.decoration_count = static_cast<u16>(source.decoration_count);
    copy_bytes(generation.decorations + entry.first_decoration, source.decorations,
               source.decoration_count * sizeof(TextDecoration));
    generation.decoration_count += source.decoration_count;
    entry.first_hit = static_cast<u16>(generation.hit_count);
    entry.hit_count = static_cast<u16>(source.hit_region_count);
    copy_bytes(generation.hits + entry.first_hit, source.hit_regions,
               source.hit_region_count * sizeof(TextHitRegion));
    generation.hit_count += source.hit_region_count;
    entry.width = source.width;
    entry.width_including_trailing_whitespace = source.width_including_trailing_whitespace;
    entry.height = source.height;
    entry.line_count = source.line_count;
    entry.owner_flags = source.owner_flags;
    ++generation.entry_count;
    for (u32 probe = 0;; ++probe) {
        u16& slot = generation.buckets[(entry.hash + probe) & (kShapedTextCacheBuckets - 1)];
        if (!slot) {
            slot = static_cast<u16>(generation.entry_count);
            break;
        }
    }
    return &entry;
}

} // namespace

ShapedTextCache* shaped_text_cache_create() noexcept {
    // Zero pages are committed on first touch; most capacity stays untouched.
    return static_cast<ShapedTextCache*>(
        VirtualAlloc(nullptr, sizeof(ShapedTextCache), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

void shaped_text_cache_clear(ShapedTextCache* cache) noexcept {
    if (!cache)
        return;
    clear_generation(cache->generations[0]);
    clear_generation(cache->generations[1]);
    saturating_increment(cache->stats.clears);
}

void shaped_text_cache_destroy(ShapedTextCache** cache) noexcept {
    if (!cache || !*cache)
        return;
    shaped_text_cache_clear(*cache);
    VirtualFree(*cache, 0, MEM_RELEASE);
    *cache = nullptr;
}

bool shaped_text_cache_find(ShapedTextCache* cache, const ShapedTextCacheRequest& request, u64* hash,
                            ShapedText* view) noexcept {
    if (!cache || !request.system || !hash || !view || (!request.text.data && request.text.bytes))
        return false;
    *hash = request_hash(request);
    ShapedTextGeneration& current = cache->generations[cache->current];
    if (ShapedTextCacheEntry* entry = find_in(current, request, *hash)) {
        *view = view_of(current, *entry);
        saturating_increment(cache->stats.hits);
        return true;
    }
    ShapedTextGeneration& older = cache->generations[cache->current ^ 1u];
    ShapedTextCacheEntry* entry = find_in(older, request, *hash);
    if (!entry) {
        saturating_increment(cache->stats.misses);
        return false;
    }
    saturating_increment(cache->stats.hits);
    const u32 key_count = entry->text_bytes + entry->style_count * static_cast<u32>(sizeof(TextStyleSpan)) +
                          entry->hit_span_count * static_cast<u32>(sizeof(TextHitSpan));
    // Promotion never retires a generation, so the older copy stays valid when full.
    if (!fits(current, key_count, entry->glyph_count, entry->run_count, entry->decoration_count,
              entry->hit_count)) {
        *view = view_of(older, *entry);
        return true;
    }
    const ShapedText source = view_of(older, *entry);
    copy_bytes(current.key_bytes + current.key_byte_count, older.key_bytes + entry->key_offset, key_count);
    const ShapedTextCacheEntry* moved = append(current, *entry, key_count, source);
    entry->moved = true;
    saturating_increment(cache->stats.promotions);
    *view = view_of(current, *moved);
    return true;
}

bool shaped_text_cache_insert(ShapedTextCache* cache, const ShapedTextCacheRequest& request, u64 hash,
                              ShapedText* shaped) noexcept {
    if (!cache || !request.system || !shaped || request.styles.count > 0xFFFFu || request.hits.count > 0xFFFFu)
        return false;
    const u32 style_bytes = request.styles.count * static_cast<u32>(sizeof(TextStyleSpan));
    const u32 hit_bytes = request.hits.count * static_cast<u32>(sizeof(TextHitSpan));
    const u32 key_count = request.text.bytes + style_bytes + hit_bytes;
    if (!fits(cache->generations[cache->current], key_count, shaped->glyph_count, shaped->run_count,
              shaped->decoration_count, shaped->hit_region_count)) {
        cache->current ^= 1u;
        clear_generation(cache->generations[cache->current]);
        saturating_increment(cache->stats.flips);
        if (!fits(cache->generations[cache->current], key_count, shaped->glyph_count, shaped->run_count,
                  shaped->decoration_count, shaped->hit_region_count)) {
            saturating_increment(cache->stats.rejected);
            return false;
        }
    }
    ShapedTextGeneration& generation = cache->generations[cache->current];
    // Assemble the exact key in place, then publish the entry over it.
    u8* key_bytes = generation.key_bytes + generation.key_byte_count;
    copy_bytes(key_bytes, request.text.data, request.text.bytes);
    copy_bytes(key_bytes + request.text.bytes, request.styles.data, style_bytes);
    copy_bytes(key_bytes + request.text.bytes + style_bytes, request.hits.data, hit_bytes);
    ShapedTextCacheEntry key{};
    key.hash = hash;
    key.system = *request.system;
    key.maximum_width = request.maximum_width;
    key.maximum_height = request.maximum_height;
    key.dpi = request.dpi;
    key.typography_revision = request.typography_revision;
    key.text_bytes = request.text.bytes;
    key.style_count = static_cast<u16>(request.styles.count);
    key.hit_span_count = static_cast<u16>(request.hits.count);
    key.wrapping = request.wrapping;
    key.overflow = request.overflow;
    append(generation, key, key_count, *shaped);
    // The cache now owns the run and session references.
    shaped->run_count = 0;
    shaped->set_native_fonts(nullptr);
    saturating_increment(cache->stats.inserts);
    return true;
}

} // namespace maicord

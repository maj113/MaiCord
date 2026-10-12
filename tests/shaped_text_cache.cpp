#include "../src/render/shaped_text_cache.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

struct Fixture {
    TextGlyph glyphs[64];
    TextRun runs[4];
    TextDecoration decorations[4];
    TextHitRegion hits[4];
    ShapedText shaped;
};

void fill(Fixture* fixture, u32 glyphs, float marker) noexcept {
    *fixture = {};
    auto& shaped = fixture->shaped;
    shaped.glyphs = fixture->glyphs;
    shaped.runs = fixture->runs;
    shaped.decorations = fixture->decorations;
    shaped.hit_regions = fixture->hits;
    shaped.glyph_capacity = 64;
    shaped.run_capacity = shaped.decoration_capacity = shaped.hit_region_capacity = 4;
    for (u32 i = 0; i < glyphs; ++i)
        fixture->glyphs[i] = {marker + static_cast<float>(i), 0, 0, static_cast<u16>(i), 0};
    shaped.glyph_count = glyphs;
    fixture->runs[0] = {};
    fixture->runs[0].glyph_count = glyphs;
    fixture->runs[0].baseline_x = marker;
    shaped.run_count = 1;
    fixture->decorations[0] = {1, 2, 3, 4, 0, TextDecorationKind::background, 0, 0, 0};
    shaped.decoration_count = 1;
    fixture->hits[0] = {0, 0, 10, 10, 0xFFFFu, 0};
    fixture->hits[1] = {10, 0, 20, 10, 0, 0};
    shaped.hit_region_count = 2;
    shaped.width = marker * 2;
    shaped.height = 18;
    shaped.line_count = 1;
}

} // namespace

void test_shaped_text_cache(void (*check)(bool, const char*) noexcept) noexcept {
    ShapedTextCache* cache = shaped_text_cache_create();
    check(cache != nullptr, "shaped text cache allocates lazily-committed storage");
    if (!cache)
        return;
    TextSystem system{};
    system.em_size = 14;
    const TextStyleSpan styles[] = {{0, 5, text_style_bold}};
    const TextHitSpan spans[] = {{1, 3, 0, 0}, {0, 2, 0xFFFFu, 0}};
    const ShapedTextCacheRequest base{&system, utf8_literal("hello"), {styles, 1}, {spans, 2}, 300, 40, 96, 1,
                                      0, 0};
    u64 hash = 0;
    ShapedText view{};
    Fixture fixture{};
    fill(&fixture, 5, 100);
    check(!shaped_text_cache_find(cache, base, &hash, &view) && cache->stats.misses == 1,
          "shaped text cache misses before insertion");
    check(shaped_text_cache_insert(cache, base, hash, &fixture.shaped) && fixture.shaped.run_count == 0,
          "shaped text cache insertion moves run references out of the scratch result");
    const bool hit = shaped_text_cache_find(cache, base, &hash, &view);
    check(hit && view.glyph_count == 5 && view.glyphs[4].advance == 104 && view.run_count == 1 &&
              view.runs[0].baseline_x == 100 && view.decoration_count == 1 && view.decorations[0].right == 3 &&
              view.hit_region_count == 2 && view.hit_regions[0].semantic_index == 0xFFFFu &&
              view.width == 200 && view.height == 18,
          "shaped text cache returns the exact shaped glyphs, runs, decorations and hit regions");

    // Every shaping input is part of the key; origin, colour and atlas state are not inputs.
    const auto misses = [&](ShapedTextCacheRequest request) noexcept {
        u64 other = 0;
        ShapedText ignored{};
        return !shaped_text_cache_find(cache, request, &other, &ignored);
    };
    bool keyed = true;
    ShapedTextCacheRequest changed = base;
    changed.text = utf8_literal("hellp");
    keyed = keyed && misses(changed);
    changed = base;
    changed.maximum_width = 299.5f;
    keyed = keyed && misses(changed);
    changed = base;
    changed.maximum_height = 41;
    keyed = keyed && misses(changed);
    changed = base;
    changed.dpi = 144;
    keyed = keyed && misses(changed);
    changed = base;
    changed.typography_revision = 2;
    keyed = keyed && misses(changed);
    changed = base;
    changed.wrapping = 1;
    keyed = keyed && misses(changed);
    changed = base;
    changed.overflow = 1;
    keyed = keyed && misses(changed);
    const TextStyleSpan spoiler[] = {{0, 5, text_style_bold | text_style_spoiler}};
    changed = base;
    changed.styles = {spoiler, 1};
    keyed = keyed && misses(changed);
    const TextHitSpan moved_selection[] = {{1, 3, 0, 0}, {0, 3, 0xFFFFu, 0}};
    changed = base;
    changed.hits = {moved_selection, 2};
    keyed = keyed && misses(changed);
    changed = base;
    changed.hits = {spans, 1};
    keyed = keyed && misses(changed);
    TextSystem larger = system;
    larger.em_size = 16;
    changed = base;
    changed.system = &larger;
    keyed = keyed && misses(changed);
    TextSystem letters = system;
    letters.independent_letters = true;
    changed.system = &letters;
    keyed = keyed && misses(changed);
    check(keyed, "shaped text cache keys text, styles, spoilers, selection, size, wrap, DPI and typography");

    // Fill the current generation so a flip retires the older one. An entry
    // used in between is copied forward and survives; an idle one does not.
    char text[16]{};
    const auto request_for = [&](u32 index) noexcept {
        for (u32 digit = 0; digit < 8; ++digit)
            text[digit] = static_cast<char>('a' + ((index >> (digit * 4)) & 15u));
        ShapedTextCacheRequest request = base;
        request.text = Utf8View{text, 8};
        request.hits = {};
        return request;
    };
    ShapedTextCacheRequest idle = base;
    idle.text = utf8_literal("idle entry");
    idle.hits = {};
    fill(&fixture, 3, 7);
    shaped_text_cache_find(cache, idle, &hash, &view);
    shaped_text_cache_insert(cache, idle, hash, &fixture.shaped);
    u32 inserted = 0;
    const u32 flips = cache->stats.flips;
    while (cache->stats.flips == flips && inserted < 4096) {
        const auto request = request_for(inserted++);
        fill(&fixture, 64, 1);
        shaped_text_cache_find(cache, request, &hash, &view);
        shaped_text_cache_insert(cache, request, hash, &fixture.shaped);
    }
    const bool reused = shaped_text_cache_find(cache, base, &hash, &view) && view.glyphs[0].advance == 100;
    const u32 promotions = cache->stats.promotions;
    const u32 second = cache->stats.flips;
    while (cache->stats.flips == second && inserted < 8192) {
        const auto request = request_for(inserted++);
        fill(&fixture, 64, 1);
        shaped_text_cache_find(cache, request, &hash, &view);
        shaped_text_cache_insert(cache, request, hash, &fixture.shaped);
    }
    check(reused && promotions >= 1 && shaped_text_cache_find(cache, base, &hash, &view) &&
              view.glyph_count == 5 && view.glyphs[4].advance == 104 && misses(idle),
          "shaped text cache keeps recently used results across generations and evicts idle ones");
    fill(&fixture, 64, 1);
    fixture.shaped.glyph_count = 64;
    ShapedTextCacheRequest oversized = request_for(9'999'999);
    oversized.text = Utf8View{reinterpret_cast<const char*>(cache), kShapedTextCacheKeyBytes + 1};
    check(!shaped_text_cache_insert(cache, oversized, 1, &fixture.shaped) && fixture.shaped.run_count == 1,
          "shaped text cache rejects a result larger than a generation without taking ownership");
    shaped_text_cache_clear(cache);
    check(misses(base) && cache->generations[0].entry_count == 0 && cache->generations[1].entry_count == 0,
          "shaped text cache clear drops every entry");
    shaped_text_cache_destroy(&cache);
    check(cache == nullptr, "shaped text cache destroy releases storage");
}

} // namespace maicord

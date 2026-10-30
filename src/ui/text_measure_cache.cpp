#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool key_valid(const UiTextMeasureKey& key) noexcept {
    return key.source_id != 0 || key.content_signature != 0;
}

bool key_equal(const UiTextMeasureKey& left, const UiTextMeasureKey& right) noexcept {
    return left.source_id == right.source_id && left.content_signature == right.content_signature &&
           left.source_revision == right.source_revision && left.item == right.item &&
           left.backend_revision == right.backend_revision;
}

bool value_valid(const UiTextMeasureValue& value) noexcept {
    return ui_size_valid_or_empty(value.size) && ui_extent_valid(value.baseline) &&
           value.baseline <= value.size.height;
}

u64 mix(u64 hash, u64 value) noexcept {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

u64 key_hash(const UiTextMeasureKey& key) noexcept {
    u64 hash = mix(0x243f6a8885a308d3ull, key.source_id);
    hash = mix(hash, key.content_signature);
    hash = mix(hash, key.source_revision);
    hash = mix(hash, key.item);
    return mix(hash, key.backend_revision);
}

u64 next_stamp(UiTextMeasureCache* cache) noexcept {
    if (cache->stamp == ~0ull) {
        ui_text_measure_cache_invalidate(cache);
    }
    return ++cache->stamp;
}

void advance_generation(UiTextMeasureCache* cache) noexcept {
    ++cache->generation;
    if (cache->generation == 0) {
        for (u32 index = 0; index < cache->storage.count; ++index) {
            cache->storage[index].generation = 0;
        }
        cache->generation = 1;
    }
}

} // namespace

bool ui_text_measure_cache_initialize(UiTextMeasureCache* cache, Slice<UiTextMeasureRecord> storage,
                                      UiTextMeasureBackend backend, void* backend_context) noexcept {
    if (!cache || !storage.data || storage.count < kUiTextMeasureCacheWays ||
        storage.count % kUiTextMeasureCacheWays != 0 || !backend) {
        return false;
    }
    reset_value<UiTextMeasureCache>(cache);
    cache->storage = storage;
    cache->backend = backend;
    cache->backend_context = backend_context;
    cache->generation = 1;
    for (u32 index = 0; index < storage.count; ++index) {
        storage[index].generation = 0;
    }
    return true;
}

void ui_text_measure_cache_invalidate(UiTextMeasureCache* cache) noexcept {
    if (!cache || !cache->storage.data) {
        return;
    }
    advance_generation(cache);
}

UiTextMeasureResult ui_text_measure_cache_measure(UiTextMeasureCache* cache,
                                                  const UiTextMeasureRequest& request,
                                                  UiTextMeasureValue* value) noexcept {
    if (!cache || !cache->storage.data || !cache->backend || !value || !key_valid(request.key) ||
        (!request.text.data && request.text.bytes != 0) ||
        (!request.styles.data && request.styles.count != 0) || !ui_size_valid(request.constraint)) {
        return UiTextMeasureResult{UiTextMeasureError::invalid_input, false};
    }
    const u32 set_count = cache->storage.count / kUiTextMeasureCacheWays;
    const u32 first = static_cast<u32>(key_hash(request.key) % set_count) * kUiTextMeasureCacheWays;
    UiTextMeasureRecord* replacement = nullptr;
    bool replacement_stale = false;
    for (u32 way = 0; way < kUiTextMeasureCacheWays; ++way) {
        UiTextMeasureRecord& record = cache->storage[first + way];
        if (record.generation != cache->generation) {
            if (!replacement_stale) {
                replacement = &record;
                replacement_stale = true;
            }
            continue;
        }
        if (record.face == request.face && key_equal(record.key, request.key) &&
            record.constraint.width == request.constraint.width &&
            record.constraint.height == request.constraint.height) {
            record.access_stamp = next_stamp(cache);
            *value = record.value;
            return UiTextMeasureResult{UiTextMeasureError::none, true};
        }
        if (!replacement_stale && (!replacement || record.access_stamp < replacement->access_stamp)) {
            replacement = &record;
        }
    }
    saturating_increment(cache->backend_calls);
    UiTextMeasureValue measured{};
    if (!cache->backend(cache->backend_context, request, &measured) || !value_valid(measured)) {
        return UiTextMeasureResult{UiTextMeasureError::backend, false};
    }
    replacement->face = request.face;
    replacement->key = request.key;
    replacement->constraint = request.constraint;
    replacement->value = measured;
    replacement->access_stamp = next_stamp(cache);
    replacement->generation = cache->generation;
    *value = measured;
    return UiTextMeasureResult{UiTextMeasureError::none, false};
}

} // namespace maicord

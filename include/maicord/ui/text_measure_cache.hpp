#pragma once

#include "maicord/ui/layout_engine.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {

inline constexpr u32 kUiTextMeasureCacheWays = 4;

struct UiTextMeasureKey {
    u64 source_id;
    u64 content_signature;
    u32 source_revision;
    u16 item;
    u16 backend_revision;
};

struct UiTextMeasureRequest {
    UiTextMeasureKey key;
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    UiSize constraint;
    bool no_wrap;
    UiTextFace face = UiTextFace::normal;
};

struct UiTextMeasureValue {
    UiSize size;
    float baseline;
    u32 line_count;
};

using UiTextMeasureBackend = bool (*)(void* context, const UiTextMeasureRequest& request,
                                      UiTextMeasureValue* value) noexcept;

struct UiTextMeasureRecord {
    UiTextMeasureKey key;
    UiSize constraint;
    UiTextMeasureValue value;
    u64 access_stamp;
    u32 generation;
    UiTextFace face;
};

enum class UiTextMeasureError : u8 {
    none,
    invalid_input,
    backend,
};

struct UiTextMeasureResult {
    UiTextMeasureError error;
    bool cached;

    constexpr bool ok() const noexcept {
        return error == UiTextMeasureError::none;
    }
};

struct UiTextMeasureCache {
    Slice<UiTextMeasureRecord> storage;
    UiTextMeasureBackend backend;
    void* backend_context;
    u64 stamp;
    u32 generation;
    u32 resident;
    u32 hits;
    u32 misses;
    u32 backend_calls;
    u32 inserts;
    u32 evictions;
    u32 invalidations;
    u32 rejected;
};

bool ui_text_measure_cache_initialize(UiTextMeasureCache* cache, Slice<UiTextMeasureRecord> storage,
                                      UiTextMeasureBackend backend, void* backend_context) noexcept;
void ui_text_measure_cache_invalidate(UiTextMeasureCache* cache) noexcept;
UiTextMeasureResult ui_text_measure_cache_measure(UiTextMeasureCache* cache,
                                                  const UiTextMeasureRequest& request,
                                                  UiTextMeasureValue* value) noexcept;

static_assert(sizeof(UiTextMeasureKey) == 24);
static_assert(sizeof(UiTextMeasureValue) == 16);
static_assert(sizeof(UiTextMeasureRecord) == 64);
static_assert(sizeof(UiTextMeasureCache) == 80);

} // namespace maicord
